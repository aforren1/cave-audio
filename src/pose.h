/*
 * pose.h — a lock-free single-slot pose handoff (seqlock) from the NatNet receiver thread
 * to the audio thread.
 *
 * The tracked head pose is 7 floats (position + quaternion) — too wide for one atomic store.
 * A seqlock lets a single writer publish it and a single reader sample it without a lock:
 * the writer brackets its write with an odd->even sequence counter; the reader retries if it
 * observes a write in progress or a torn read. The reader's retry is BOUNDED (the writer stores
 * eight scalars), so the audio thread never blocks — it just keeps its previous pose if it
 * loses the race, which for a per-block sample is inaudible.
 *
 * C11 atomics, following Boehm 2012, "Can seqlocks get along with programming language memory
 * models?". Two rules from that paper are why this is not the obvious code:
 *
 *   1. THE PAYLOAD FIELDS ARE ATOMIC, loaded and stored RELAXED. A seqlock reader deliberately
 *      reads data a writer may be writing and throws the result away; on plain (non-atomic)
 *      fields that is a data race, which is undefined behavior, not merely a stale value. Making
 *      them relaxed atomics costs nothing on any real target (a plain load or store) and makes
 *      the racy read defined. It also means no memcpy: memcpy over atomics is not atomic access.
 *   2. THE FENCES CARRY THE ORDERING, not the counter accesses alone. The writer's seq_cst fence
 *      after the odd store keeps the payload stores from sinking above it, and the reader's
 *      acquire fence after the payload loads keeps them from floating below the validating
 *      reload. Without the reader's fence a compiler may reorder a payload load past the second
 *      counter read and the tear check stops meaning anything.
 *
 * C++ translation units (examples/validate.cpp reaches this through natnet.h) get the type as
 * OPAQUE: `_Atomic` is not C++, and a C++ caller only ever holds a `const PoseSlot*`.
 */
#ifndef BWA_POSE_H
#define BWA_POSE_H

/* The forward declaration, shared verbatim by rt.h and natnet.h so neither drags <stdatomic.h>
 * (and with it MSVC's /experimental:c11atomics) into every translation unit that only passes the
 * slot around by pointer. Guarded rather than repeated, because a redundant typedef is a C11-only
 * allowance and not every compiler in play is in C11 mode. */
#ifndef BWA_POSESLOT_FWD
#define BWA_POSESLOT_FWD
typedef struct PoseSlot PoseSlot;
#endif

#ifndef __cplusplus   /* the seqlock itself is C11-atomics code; C++ gets the type opaque */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct PoseSlot {
    _Atomic uint32_t seq;   /* even = stable, odd = write in progress, 0 = never published */
    _Atomic float    p[3];  /* position (room space) */
    _Atomic float    q[4];  /* orientation quaternion xyzw */
    _Atomic uint64_t t_ns;  /* writer's monotonic clock at publish (0 = untimestamped). Consumers may
                             * only DIFFERENCE t_ns values from the same slot (one writer, one clock);
                             * never compare against another clock. */
};

/* Writer (NatNet receiver thread): publish a new pose, stamped with the writer's clock.
 * Unsigned counter math, and the exit value skips 0 on wrap so the reader's "never published"
 * sentinel stays unambiguous — otherwise one sample per 2^32 writes would be dropped. */
static inline void pose_write_t(PoseSlot* s, const float p[3], const float q[4], uint64_t t_ns) {
    const uint32_t s0 = atomic_load_explicit(&s->seq, memory_order_relaxed);  /* sole writer */
    atomic_exchange_explicit(&s->seq, s0 + 1u, memory_order_relaxed);         /* enter (odd) */
    atomic_thread_fence(memory_order_seq_cst);   /* the odd counter lands before any payload store */
    for (int i = 0; i < 3; ++i) atomic_store_explicit(&s->p[i], p[i], memory_order_relaxed);
    for (int i = 0; i < 4; ++i) atomic_store_explicit(&s->q[i], q[i], memory_order_relaxed);
    atomic_store_explicit(&s->t_ns, t_ns, memory_order_relaxed);
    uint32_t s2 = s0 + 2u;
    if (s2 == 0u) s2 = 2u;                                                    /* skip the sentinel */
    atomic_store_explicit(&s->seq, s2, memory_order_release);                 /* leave (even) */
}
static inline void pose_write(PoseSlot* s, const float p[3], const float q[4]) {
    pose_write_t(s, p, q, 0);                       /* untimestamped (the readback slot) */
}

/* Reader (audio thread): sample the latest pose (+ its stamp). Returns false if it lost the race
 * after a few tries (the caller keeps its previous pose); never blocks. The payload lands in
 * locals first, so a torn attempt never half-writes the caller's buffers. */
static inline bool pose_read_t(const PoseSlot* s, float p[3], float q[4], uint64_t* t_ns) {
    for (int t = 0; t < 8; ++t) {
        const uint32_t s0 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (s0 == 0u) return false;                 /* never published — caller keeps its own pose */
        if (s0 & 1u) continue;                      /* writer mid-update */
        float pp[3], qq[4];
        for (int i = 0; i < 3; ++i) pp[i] = atomic_load_explicit(&s->p[i], memory_order_relaxed);
        for (int i = 0; i < 4; ++i) qq[i] = atomic_load_explicit(&s->q[i], memory_order_relaxed);
        const uint64_t tt = atomic_load_explicit(&s->t_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);  /* payload loads stay ABOVE the reload */
        const uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (s0 == s1) {                             /* no write straddled the read */
            for (int i = 0; i < 3; ++i) p[i] = pp[i];
            for (int i = 0; i < 4; ++i) q[i] = qq[i];
            if (t_ns) *t_ns = tt;
            return true;
        }
    }
    return false;
}
static inline bool pose_read(const PoseSlot* s, float p[3], float q[4]) {
    return pose_read_t(s, p, q, 0);
}

#endif /* __cplusplus */

#endif /* BWA_POSE_H */
