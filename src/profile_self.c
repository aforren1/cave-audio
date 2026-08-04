/*
 * profile_self.c — see profile_self.h. A fixed table of named zones; each BWA_ZONE_END adds its
 * elapsed QPC ticks + a call count via interlocked ops, so it's safe across the audio + sim threads.
 * Always compiled into the dll (reset/report are exported unconditionally); the per-zone accumulation
 * only runs when the dll was built with -DBWA_PROFILE_SELF (else begin/end are never called).
 */
#include "profile_self.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define BWA_PROF_MAXZ 64

static CRITICAL_SECTION g_lock;
static volatile LONG     g_ready;                 /* 1 once g_lock + g_ns_per_tick are initialized */
static double            g_ns_per_tick;
static struct { const char* name; volatile LONG64 total, count; } g_z[BWA_PROF_MAXZ];
static volatile LONG     g_nz;                    /* published slot count (write name before bumping) */
static volatile LONG64   g_frames;

bwa_prof_zone bwa_prof__begin(const char* name) {
    bwa_prof_zone z; z.name = name;
    LARGE_INTEGER c; QueryPerformanceCounter(&c); z.t0 = (uint64_t)c.QuadPart;
    return z;
}

void bwa_prof__end(bwa_prof_zone* z) {
    if (!g_ready) return;                          /* before the first reset(): ignore */
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    LONG64 dt = (LONG64)((uint64_t)c.QuadPart - z->t0);

    int slot = -1, n = (int)g_nz;                  /* lock-free scan by pooled-literal pointer */
    for (int i = 0; i < n; ++i) if (g_z[i].name == z->name) { slot = i; break; }
    if (slot < 0) {                                /* first sight of this zone: register under the lock */
        EnterCriticalSection(&g_lock);
        n = (int)g_nz;
        for (int i = 0; i < n; ++i) if (g_z[i].name == z->name) { slot = i; break; }
        if (slot < 0 && n < BWA_PROF_MAXZ) {
            g_z[n].name = z->name; g_z[n].total = 0; g_z[n].count = 0;
            slot = n; g_nz = n + 1;                /* publish AFTER the name is written */
        }
        LeaveCriticalSection(&g_lock);
    }
    if (slot >= 0) {
        InterlockedAdd64(&g_z[slot].total, dt);
        InterlockedIncrement64(&g_z[slot].count);
    }
}

void bwa_prof__frame(void) { if (g_ready) InterlockedIncrement64(&g_frames); }

int bwa_prof_reset(void) {
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ns_per_tick = 1.0e9 / (double)f.QuadPart;
        g_ready = 1;                                /* only now may a zone touch g_lock */
    }
    EnterCriticalSection(&g_lock);
    g_nz = 0; g_frames = 0;                         /* names re-register on next sight */
    LeaveCriticalSection(&g_lock);
#ifdef BWA_PROFILE_SELF
    return 1;
#else
    return 0;                                       /* accumulation compiled out — nothing will land */
#endif
}

int bwa_prof_report(void) {
    if (!g_ready) return 0;
    EnterCriticalSection(&g_lock);
    int n = (int)g_nz; LONG64 frames = g_frames;
    printf("  %-18s %10s %10s %9s %9s\n", "zone", "calls", "total ms", "mean us", "us/block");
    for (int i = 0; i < n; ++i) {
        double total_us = (double)g_z[i].total * g_ns_per_tick / 1000.0;
        LONG64 cnt = g_z[i].count;
        printf("  %-18s %10lld %10.2f %9.2f %9.2f\n",
               g_z[i].name ? g_z[i].name : "?", (long long)cnt, total_us / 1000.0,
               cnt ? total_us / (double)cnt : 0.0,
               frames ? total_us / (double)frames : 0.0);
    }
    LeaveCriticalSection(&g_lock);
    return n;
}
