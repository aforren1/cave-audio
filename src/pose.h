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
#ifndef BW_POSE_H
#define BW_POSE_H

#include <intrin.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    volatile long seq;     /* even = stable, odd = write in progress */
    float p[3];            /* position (room space) */
    float q[4];            /* orientation quaternion xyzw */
} PoseSlot;

/* Writer (NatNet receiver thread): publish a new pose. */
static inline void pose_write(PoseSlot* s, const float p[3], const float q[4]) {
    long s0 = s->seq;
    _InterlockedExchange(&s->seq, s0 + 1);          /* enter (odd) — full barrier */
    memcpy(s->p, p, sizeof s->p);
    memcpy(s->q, q, sizeof s->q);
    _InterlockedExchange(&s->seq, s0 + 2);          /* leave (even) — full barrier */
}

/* Reader (audio thread): sample the latest pose. Returns false if it lost the race after a
 * few tries (the caller keeps its previous pose); never blocks. */
static inline bool pose_read(const PoseSlot* s, float p[3], float q[4]) {
    for (int t = 0; t < 8; ++t) {
        long s0 = _InterlockedCompareExchange((volatile long*)&s->seq, 0, 0);  /* barriered read */
        if (s0 == 0) return false;                  /* never published — caller keeps its own pose */
        if (s0 & 1) continue;                       /* writer mid-update */
        memcpy(p, s->p, sizeof s->p);
        memcpy(q, s->q, sizeof s->q);
        long s1 = _InterlockedCompareExchange((volatile long*)&s->seq, 0, 0);
        if (s0 == s1) return true;                  /* no write straddled the read */
    }
    return false;
}

#endif /* BW_POSE_H */
