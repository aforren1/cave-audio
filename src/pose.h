/*
 * pose.h — a lock-free single-slot pose handoff (seqlock) from the NatNet receiver thread
 * to the audio thread.
 *
 * The tracked head pose is 7 floats (position + quaternion) — too wide for one atomic store.
 * A seqlock lets a single writer publish it and a single reader sample it without a lock:
 * the writer brackets its write with an odd→even sequence counter; the reader retries if it
 * observes a write in progress or a torn read. The reader's retry is BOUNDED (the writer is
 * a few memcpys), so the audio thread never blocks — it just keeps its previous pose if it
 * loses the race, which for a per-block sample is inaudible.
 *
 * Windows/x86 only (the whole engine is): the InterlockedExchange/CompareExchange on the
 * sequence are full barriers, which order the data stores/loads between them on this target.
 */
#ifndef BWA_POSE_H
#define BWA_POSE_H

#include <intrin.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    volatile long seq;     /* even = stable, odd = write in progress */
    float p[3];            /* position (room space) */
    float q[4];            /* orientation quaternion xyzw */
    unsigned __int64 t_ns; /* writer's monotonic clock at publish (0 = untimestamped). Consumers may
                            * only DIFFERENCE t_ns values from the same slot (one writer, one clock);
                            * never compare against another clock (ASIO systemTime, QPC elsewhere). */
} PoseSlot;

/* Writer (NatNet receiver thread): publish a new pose, stamped with the writer's clock.
 * Unsigned counter math: `s0 + 1` on a long is signed overflow (UB) once the counter tops out,
 * and the wrap runs THROUGH 0 — the reader's "never published" sentinel — which would drop one
 * sample per 2^31 writes. The exit value skips 0 so the sentinel stays unambiguous. */
static inline void pose_write_t(PoseSlot* s, const float p[3], const float q[4], unsigned __int64 t_ns) {
    unsigned long s0 = (unsigned long)s->seq;
    _InterlockedExchange(&s->seq, (long)(s0 + 1u)); /* enter (odd) — full barrier */
    memcpy(s->p, p, sizeof s->p);
    memcpy(s->q, q, sizeof s->q);
    s->t_ns = t_ns;
    unsigned long s2 = s0 + 2u;
    if (s2 == 0u) s2 = 2u;                          /* skip the never-published sentinel on wrap */
    _InterlockedExchange(&s->seq, (long)s2);        /* leave (even) — full barrier */
}
static inline void pose_write(PoseSlot* s, const float p[3], const float q[4]) {
    pose_write_t(s, p, q, 0);                       /* untimestamped (e.g. the readback slot) */
}

/* Reader (audio thread): sample the latest pose (+ its stamp). Returns false if it lost the race
 * after a few tries (the caller keeps its previous pose); never blocks. */
static inline bool pose_read_t(const PoseSlot* s, float p[3], float q[4], unsigned __int64* t_ns) {
    for (int t = 0; t < 8; ++t) {
        long s0 = _InterlockedCompareExchange((volatile long*)&s->seq, 0, 0);  /* barriered read */
        if (s0 == 0) return false;                  /* never published — caller keeps its own pose */
        if (s0 & 1) continue;                       /* writer mid-update */
        memcpy(p, s->p, sizeof s->p);
        memcpy(q, s->q, sizeof s->q);
        unsigned __int64 tt = s->t_ns;
        long s1 = _InterlockedCompareExchange((volatile long*)&s->seq, 0, 0);
        if (s0 == s1) { if (t_ns) *t_ns = tt; return true; }   /* no write straddled the read */
    }
    return false;
}
static inline bool pose_read(const PoseSlot* s, float p[3], float q[4]) {
    return pose_read_t(s, p, q, 0);
}

#endif /* BWA_POSE_H */
