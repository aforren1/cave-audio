/*
 * os_test.c — the OS portability shim (src/os.h) and the seqlock built on top of it (src/pose.h).
 *
 * Why this exists: phase 2 of docs/backends.md moved every Win32 call the engine made outside the
 * sinks behind one seam, and rewrote pose.h's seqlock from Interlocked intrinsics onto C11 atomics.
 * The rest of the suite exercises the shim only INCIDENTALLY (every threaded test sleeps), so a
 * broken sleep or a torn pose would show up as a confusing failure somewhere else. Here it shows up
 * as itself.
 *
 * On the torn-read section, read the comment above pose_torn_section before trusting it green.
 */
#include "os.h"
#include "pose.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   %s\n", (msg)); \
    else { printf("  FAIL %s\n", (msg)); ++g_fail; } } while (0)

/* ---- threads ------------------------------------------------------------------------------ */

typedef struct { _Atomic int ran; int arg_seen; } ThreadProbe;

static void probe_body(void* user) {
    ThreadProbe* t = (ThreadProbe*)user;
    t->arg_seen = 1;                                  /* joined before the reader looks: no race */
    atomic_store_explicit(&t->ran, 1, memory_order_release);
}

static void thread_section(void) {
    printf("threads\n");
    ThreadProbe t;
    memset(&t, 0, sizeof t);
    os_thread th;
    memset(&th, 0, sizeof th);
    CHECK(!os_thread_valid(&th), "a zeroed os_thread is not valid");
    CHECK(os_thread_create(&th, probe_body, &t) == 0, "os_thread_create");
    CHECK(os_thread_valid(&th), "the handle is valid once created");
    os_thread_join(&th);
    CHECK(atomic_load_explicit(&t.ran, memory_order_acquire) == 1, "the body ran before join returned");
    CHECK(t.arg_seen == 1, "the argument reached the body");
    CHECK(!os_thread_valid(&th), "join clears the handle");
    os_thread_join(&th);                              /* must be a no-op, not a crash */
    CHECK(1, "a second join is a no-op");
    CHECK(os_thread_create(NULL, probe_body, &t) != 0, "os_thread_create(NULL) fails cleanly");
}

/* ---- sleep and clock ---------------------------------------------------------------------- */

static void time_section(void) {
    printf("time and sleep\n");

    /* The monotonic clock advances, and at the right RATE. The C clock() is a different clock
     * (process CPU time on Windows, and this thread is asleep for most of the window), so the
     * cross-check is against a busy wait: spin on clock() for 100 ms of CPU and require the
     * monotonic clock to have moved by roughly the same wall time. */
    const uint64_t t0 = os_monotonic_ns();
    const clock_t c0 = clock();
    while ((double)(clock() - c0) / CLOCKS_PER_SEC < 0.100) { }   /* 100 ms of busy work */
    const double c_ms = 1000.0 * (double)(clock() - c0) / CLOCKS_PER_SEC;
    const double m_ms = (double)(os_monotonic_ns() - t0) / 1.0e6;
    printf("       C clock %.1f ms, os_monotonic_ns %.1f ms\n", c_ms, m_ms);
    /* A busy loop is wall time, so the two should agree closely; the window is wide enough for a
     * scheduler hiccup and narrow enough to catch a clock off by a factor (a missing QPC frequency
     * divide, or mach ticks reported as nanoseconds). */
    CHECK(m_ms > 0.5 * c_ms && m_ms < 2.5 * c_ms, "os_monotonic_ns tracks the C clock over 100 ms");

    /* Monotonic means never backward. */
    int backward = 0;
    uint64_t prev = os_monotonic_ns();
    for (int i = 0; i < 20000; ++i) {
        const uint64_t now = os_monotonic_ns();
        if (now < prev) backward = 1;
        prev = now;
    }
    CHECK(!backward, "os_monotonic_ns never steps backward");

    /* os_sleep_ms sleeps AT LEAST the asked-for time. The upper bound is loose on purpose: without
     * os_timer_resolution_begin, a Windows Sleep(1) can take 15 ms, and that is not a defect. */
    os_timer_resolution_begin();
    const uint64_t s0 = os_monotonic_ns();
    os_sleep_ms(50);
    const double slept_ms = (double)(os_monotonic_ns() - s0) / 1.0e6;
    os_timer_resolution_end();
    printf("       os_sleep_ms(50) took %.1f ms\n", slept_ms);
    CHECK(slept_ms >= 45.0, "os_sleep_ms(50) waited about 50 ms (lower bound)");
    CHECK(slept_ms < 500.0, "os_sleep_ms(50) did not hang");

    os_sleep_ms(0);
    CHECK(1, "os_sleep_ms(0) returns");
}

/* ---- the absolute-deadline sleep ---------------------------------------------------------- */

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

/* THE regression this section exists for: os_sleep_until_ns must get its precision from the
 * high-resolution waitable timer, not from raising the global timer resolution. Lose the
 * high-resolution path and Windows wakes land on the 15.6 ms default granularity, which misses a
 * 2 ms deadline by an order of magnitude and blows the p50 bound below.
 *
 * DELIBERATE-RED CHECK (per the CLAUDE.md trap): confirmed red. Stubbing the
 * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION flag out of os_win.c (so the create fails and the coarse
 * path runs) AND removing the fallback's timeBeginPeriod made p50 jump from 0.3 ms to 15.6 ms and
 * the p50 assertion fail. Restored after. */
static void deadline_section(void) {
    printf("os_sleep_until_ns\n");
    enum { N = 200 };
    const uint64_t step_ns = 2000000ull;          /* 2 ms apart */
    static uint64_t late[N];

    const uint64_t base = os_monotonic_ns();
    for (int i = 0; i < N; ++i) {
        const uint64_t deadline = base + (uint64_t)(i + 1) * step_ns;
        os_sleep_until_ns(deadline);
        const uint64_t woke = os_monotonic_ns();
        /* Clamped at 0: a wake BEFORE the deadline is only possible on the coarse fallback, and
         * "how early" is not what this measures. It is reported separately below. */
        late[i] = (woke > deadline) ? (woke - deadline) : 0;
    }

    int early = 0;
    for (int i = 0; i < N; ++i) if (late[i] == 0) ++early;
    qsort(late, N, sizeof late[0], cmp_u64);
    const double p50 = (double)late[N / 2] / 1.0e6;
    const double p99 = (double)late[(N * 99) / 100] / 1.0e6;
    printf("       lateness over %d wakes 2 ms apart: p50 %.3f ms, p99 %.3f ms, max %.3f ms"
           " (%d landed at or before the deadline)\n",
           N, p50, p99, (double)late[N - 1] / 1.0e6, early);

    /* The p50 bound is the load-bearing one; p99 is loose enough to survive one scheduler hiccup
     * on a busy CI machine but still far under the 15.6 ms default granularity. */
    CHECK(p50 < 1.0, "os_sleep_until_ns median lateness under 1 ms");
    CHECK(p99 < 4.0, "os_sleep_until_ns p99 lateness under 4 ms");

    /* A deadline already in the past returns immediately, rather than waiting a whole period. */
    const uint64_t t0 = os_monotonic_ns();
    os_sleep_until_ns(t0 - 1000000ull);
    CHECK(os_monotonic_ns() - t0 < 1000000ull, "a past deadline returns at once");
}

/* ---- mutex -------------------------------------------------------------------------------- */

/* Two threads hammer one counter through the mutex with a deliberately non-atomic
 * read-modify-write. A broken mutex loses increments; a working one cannot. */
typedef struct { os_mutex m; volatile long counter; int iters; } MutexProbe;

static void mutex_body(void* user) {
    MutexProbe* mp = (MutexProbe*)user;
    for (int i = 0; i < mp->iters; ++i) {
        os_mutex_lock(&mp->m);
        const long v = mp->counter;      /* the split RMW a lost lock would interleave */
        mp->counter = v + 1;
        os_mutex_unlock(&mp->m);
    }
}

static void mutex_section(void) {
    printf("mutex\n");
    MutexProbe mp;
    memset(&mp, 0, sizeof mp);
    mp.iters = 200000;
    CHECK(os_mutex_init(&mp.m) == 0, "os_mutex_init");

    /* Recursive, matching the CRITICAL_SECTION the shim replaced: the Steam scene lock nests. */
    os_mutex_lock(&mp.m);
    os_mutex_lock(&mp.m);
    os_mutex_unlock(&mp.m);
    os_mutex_unlock(&mp.m);
    CHECK(1, "the mutex is recursive (a nested lock does not deadlock)");

    os_thread a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    CHECK(os_thread_create(&a, mutex_body, &mp) == 0, "writer A started");
    CHECK(os_thread_create(&b, mutex_body, &mp) == 0, "writer B started");
    os_thread_join(&a);
    os_thread_join(&b);
    printf("       counter %ld, expected %ld\n", mp.counter, (long)mp.iters * 2);
    CHECK(mp.counter == (long)mp.iters * 2, "no increment was lost under two threads");
    os_mutex_destroy(&mp.m);
}

/* ---- the pose seqlock --------------------------------------------------------------------- */

/* Single-threaded first: the contract pose.h states. */
static void pose_basic_section(void) {
    printf("pose seqlock (single-threaded)\n");
    PoseSlot slot;
    memset(&slot, 0, sizeof slot);
    float p[3] = { 9, 9, 9 }, q[4] = { 9, 9, 9, 9 };
    CHECK(!pose_read(&slot, p, q), "a zeroed slot reads false (the never-published sentinel)");

    const float wp[3] = { 1.5f, -2.25f, 3.75f }, wq[4] = { 0.f, 0.7071068f, 0.f, 0.7071068f };
    pose_write(&slot, wp, wq);
    CHECK(pose_read(&slot, p, q), "read after write");
    CHECK(p[0] == wp[0] && p[1] == wp[1] && p[2] == wp[2], "position round-trips");
    CHECK(q[0] == wq[0] && q[1] == wq[1] && q[2] == wq[2] && q[3] == wq[3], "quaternion round-trips");

    uint64_t t = 12345;
    CHECK(pose_read_t(&slot, p, q, &t) && t == 0, "pose_write leaves the stamp at 0 (untimestamped)");
    pose_write_t(&slot, wp, wq, 987654321ull);
    CHECK(pose_read_t(&slot, p, q, &t) && t == 987654321ull, "pose_write_t round-trips the stamp");

    /* The wrap: the counter must never LAND on 0, which is the sentinel. Drive it there directly. */
    atomic_store_explicit(&slot.seq, 0xFFFFFFFEu, memory_order_relaxed);
    pose_write(&slot, wp, wq);
    CHECK(atomic_load_explicit(&slot.seq, memory_order_relaxed) != 0u,
          "the counter skips 0 on wrap (the sentinel stays unambiguous)");
    CHECK(pose_read(&slot, p, q), "a wrapped slot still reads");
}

/* Torn-read detection. The writer publishes a GENERATION NUMBER into all seven payload floats at
 * once; the reader asserts that every field it got back carries the SAME number. A seqlock that
 * lets a read straddle a write returns a mix of two generations, which is exactly the corruption
 * that would put a head pose halfway between two frames.
 *
 * DELIBERATE-RED CHECK (per the CLAUDE.md trap): this section WAS confirmed to go red. Dropping
 * the writer's release store to relaxed and deleting the reader's acquire fence is NOT enough on
 * x86 -- the hardware does not reorder those, so the test stays green and would imply coverage it
 * does not have. What DOES make it red is shortening the validation: with the reader's second
 * counter load removed (accept on the first even read), the run reports torn reads within a
 * fraction of a second. So this section pins the ALGORITHM (the bracket and its retry), and the
 * MEMORY ORDERING is pinned by review and by the fences being present, not by this test on x86.
 * Say so rather than let the green imply more.
 */
typedef struct {
    PoseSlot    slot;
    _Atomic int stop;
    _Atomic uint64_t writes;
    _Atomic uint64_t reads, torn, missed;
} SeqProbe;

static void seq_writer(void* user) {
    SeqProbe* sp = (SeqProbe*)user;
    float gen = 1.0f;
    while (!atomic_load_explicit(&sp->stop, memory_order_relaxed)) {
        const float p[3] = { gen, gen, gen };
        const float q[4] = { gen, gen, gen, gen };
        pose_write_t(&sp->slot, p, q, (uint64_t)gen);
        atomic_fetch_add_explicit(&sp->writes, 1u, memory_order_relaxed);
        gen += 1.0f;
        if (gen > 1.0e6f) gen = 1.0f;      /* stay exactly representable in a float */
    }
}

static void seq_reader(void* user) {
    SeqProbe* sp = (SeqProbe*)user;
    while (!atomic_load_explicit(&sp->stop, memory_order_relaxed)) {
        float p[3], q[4];
        uint64_t t = 0;
        if (!pose_read_t(&sp->slot, p, q, &t)) {
            atomic_fetch_add_explicit(&sp->missed, 1u, memory_order_relaxed);
            continue;                       /* lost the race: allowed, the caller keeps its pose */
        }
        atomic_fetch_add_explicit(&sp->reads, 1u, memory_order_relaxed);
        const float g = p[0];
        int torn = (p[1] != g || p[2] != g ||
                    q[0] != g || q[1] != g || q[2] != g || q[3] != g ||
                    t != (uint64_t)g);
        if (torn) atomic_fetch_add_explicit(&sp->torn, 1u, memory_order_relaxed);
    }
}

static void pose_torn_section(void) {
    printf("pose seqlock (one writer, one reader)\n");
    static SeqProbe sp;                     /* static: the two threads outlive nothing else here */
    memset(&sp, 0, sizeof sp);

    os_thread w, r;
    memset(&w, 0, sizeof w); memset(&r, 0, sizeof r);
    CHECK(os_thread_create(&w, seq_writer, &sp) == 0, "writer thread started");
    CHECK(os_thread_create(&r, seq_reader, &sp) == 0, "reader thread started");
    os_sleep_ms(400);
    atomic_store_explicit(&sp.stop, 1, memory_order_relaxed);
    os_thread_join(&w);
    os_thread_join(&r);

    const uint64_t writes = atomic_load_explicit(&sp.writes, memory_order_relaxed);
    const uint64_t reads  = atomic_load_explicit(&sp.reads,  memory_order_relaxed);
    const uint64_t torn   = atomic_load_explicit(&sp.torn,   memory_order_relaxed);
    const uint64_t missed = atomic_load_explicit(&sp.missed, memory_order_relaxed);
    printf("       %llu writes, %llu good reads, %llu torn, %llu missed\n",
           (unsigned long long)writes, (unsigned long long)reads,
           (unsigned long long)torn, (unsigned long long)missed);

    /* A margin, not a coin flip (the CLAUDE.md trap): the run has to have actually CONTENDED, or
     * "zero torn reads" is what an idle test reports too. The read bound is far below the millions
     * a plain build reaches, because the same binary is worth running under ThreadSanitizer and
     * that costs about two orders of magnitude; a few thousand interleaved reads still prove
     * contention, which is all this is asserting. */
    CHECK(writes > 10000, "the writer published enough to contend");
    CHECK(reads  > 2000,  "the reader completed enough reads to contend");
    CHECK(torn == 0, "no torn read in a contended run");
}

int main(void) {
    printf("== os shim ==\n");
    thread_section();
    time_section();
    deadline_section();
    mutex_section();
    pose_basic_section();
    pose_torn_section();
    if (g_fail) printf("\nFAILED (%d)\n", g_fail);
    else        printf("\nOK\n");
    return g_fail ? 1 : 0;
}
