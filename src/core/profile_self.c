/*
 * profile_self.c — see profile_self.h. A fixed table of named zones; each BWA_ZONE_END adds its
 * elapsed nanoseconds + a call count with relaxed atomics, so it's safe across the audio + sim
 * threads. Always compiled into the dll (reset/report are exported unconditionally); the per-zone
 * accumulation only runs when the dll was built with -DBWA_PROFILE_SELF (else begin/end are never
 * called).
 */
#include "core/profile_self.h"
#include "os/os.h"

#include <stdatomic.h>
#include <stdio.h>

#define BWA_PROF_MAXZ 64

static os_mutex          g_lock;
static _Atomic int       g_ready;                 /* 1 once g_lock is initialized */
static struct { const char* name; _Atomic uint64_t total, count; } g_z[BWA_PROF_MAXZ];
static _Atomic int       g_nz;                    /* published slot count (write name before bumping) */
static _Atomic uint64_t  g_frames;

bwa_prof_zone bwa_prof__begin(const char* name) {
    bwa_prof_zone z; z.name = name;
    z.t0 = os_monotonic_ns();
    return z;
}

void bwa_prof__end(bwa_prof_zone* z) {
    if (!atomic_load_explicit(&g_ready, memory_order_acquire)) return;   /* before the first reset() */
    const uint64_t dt = os_monotonic_ns() - z->t0;

    /* Lock-free scan by pooled-literal pointer. ACQUIRE the published count before reading any
     * name it covers — the same publish-then-flag rule the register path below writes to. */
    int slot = -1, n = atomic_load_explicit(&g_nz, memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_z[i].name == z->name) { slot = i; break; }
    if (slot < 0) {                                /* first sight of this zone: register under the lock */
        os_mutex_lock(&g_lock);
        n = atomic_load_explicit(&g_nz, memory_order_relaxed);
        for (int i = 0; i < n; ++i) if (g_z[i].name == z->name) { slot = i; break; }
        if (slot < 0 && n < BWA_PROF_MAXZ) {
            g_z[n].name = z->name;
            atomic_store_explicit(&g_z[n].total, 0u, memory_order_relaxed);
            atomic_store_explicit(&g_z[n].count, 0u, memory_order_relaxed);
            slot = n;
            atomic_store_explicit(&g_nz, n + 1, memory_order_release);   /* publish AFTER the name */
        }
        os_mutex_unlock(&g_lock);
    }
    if (slot >= 0) {
        atomic_fetch_add_explicit(&g_z[slot].total, dt, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_z[slot].count, 1u, memory_order_relaxed);
    }
}

void bwa_prof__frame(void) {
    if (atomic_load_explicit(&g_ready, memory_order_acquire))
        atomic_fetch_add_explicit(&g_frames, 1u, memory_order_relaxed);
}

int bwa_prof_reset(void) {
    static _Atomic int once = 0;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&once, &expected, 1,
                                                memory_order_acq_rel, memory_order_acquire)) {
        os_mutex_init(&g_lock);
        atomic_store_explicit(&g_ready, 1, memory_order_release);   /* only now may a zone lock */
    }
    while (!atomic_load_explicit(&g_ready, memory_order_acquire)) os_sleep_ms(1);  /* lost the init race */
    os_mutex_lock(&g_lock);
    atomic_store_explicit(&g_nz, 0, memory_order_relaxed);           /* names re-register on next sight */
    atomic_store_explicit(&g_frames, 0u, memory_order_relaxed);
    os_mutex_unlock(&g_lock);
#ifdef BWA_PROFILE_SELF
    return 1;
#else
    return 0;                                       /* accumulation compiled out — nothing will land */
#endif
}

int bwa_prof_report(void) {
    if (!atomic_load_explicit(&g_ready, memory_order_acquire)) return 0;
    os_mutex_lock(&g_lock);
    const int n = atomic_load_explicit(&g_nz, memory_order_relaxed);
    const uint64_t frames = atomic_load_explicit(&g_frames, memory_order_relaxed);
    printf("  %-18s %10s %10s %9s %9s\n", "zone", "calls", "total ms", "mean us", "us/block");
    for (int i = 0; i < n; ++i) {
        const double total_us = (double)atomic_load_explicit(&g_z[i].total, memory_order_relaxed) / 1000.0;
        const uint64_t cnt = atomic_load_explicit(&g_z[i].count, memory_order_relaxed);
        printf("  %-18s %10llu %10.2f %9.2f %9.2f\n",
               g_z[i].name ? g_z[i].name : "?", (unsigned long long)cnt, total_us / 1000.0,
               cnt ? total_us / (double)cnt : 0.0,
               frames ? total_us / (double)frames : 0.0);
    }
    os_mutex_unlock(&g_lock);
    return n;
}
