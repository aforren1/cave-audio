/*
 * audio_sink_test.c — M1 verification of the audio loop without hardware. Drives the
 * null sink (the offline backend) and asserts the heart of M1's "done when": a stable
 * callback fires, and the timestamp (sample position + system time) advances
 * monotonically. The ASIO backend shares the same render contract; only the device
 * differs, so this exercises the engine-facing seam directly.
 */
#include "sink.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    unsigned long blocks;
    uint64_t      last_sample_pos;
    uint64_t      last_ns;
    uint64_t      prev_ns;
    int           first;
    int           time_monotonic;
    int           pos_monotonic;
    uint64_t      prev_pos;
} Probe;

static void on_render(void* user, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    Probe* p = (Probe*)user;
    /* prove the bus is writable for the full block (the engine would mix here) */
    memset(bus, 0, sizeof(float) * (size_t)nframes * BW_CHANNELS);

    if (!p->first) {
        if (ts->system_time_ns < p->prev_ns)  p->time_monotonic = 0;
        if (ts->sample_pos    <  p->prev_pos) p->pos_monotonic  = 0;
    }
    p->prev_ns  = ts->system_time_ns;
    p->prev_pos = ts->sample_pos;
    p->first    = 0;

    p->blocks++;
    p->last_sample_pos = ts->sample_pos;
    p->last_ns         = ts->system_time_ns;
}

int main(void) {
    Probe p;
    memset(&p, 0, sizeof p);
    p.first = 1; p.time_monotonic = 1; p.pos_monotonic = 1;

    const uint32_t SR = 48000, BS = 256, CH = BW_CHANNELS;
    char err[256] = {0};
    BwSink* s = bw_null_sink_open(SR, BS, CH, on_render, &p, err, sizeof err);
    if (!s) { fprintf(stderr, "FAIL: open: %s\n", err[0] ? err : "(no message)"); return 1; }

    const char* backend = bw_sink_backend(s);
    if (bw_sink_start(s) != 0) { fprintf(stderr, "FAIL: start\n"); bw_sink_close(s); return 1; }

    Sleep(200);                                  /* ~37 blocks at 256/48000 = 5.33 ms */
    bw_sink_stop(s);                             /* joins the audio thread */

    /* thread is joined; reading the probe is race-free now */
    const unsigned long blocks = p.blocks;
    int ok = 1;
    if (blocks < 5)             { fprintf(stderr, "FAIL: too few blocks (%lu)\n", blocks); ok = 0; }
    if (!p.time_monotonic)      { fprintf(stderr, "FAIL: system_time_ns not monotonic\n"); ok = 0; }
    if (!p.pos_monotonic)       { fprintf(stderr, "FAIL: sample_pos not monotonic\n");     ok = 0; }
    if (p.last_sample_pos != (uint64_t)BS * (blocks - 1)) {
        fprintf(stderr, "FAIL: sample_pos drift (last=%llu, expected=%llu)\n",
                (unsigned long long)p.last_sample_pos, (unsigned long long)((uint64_t)BS * (blocks - 1)));
        ok = 0;
    }

    bw_sink_close(s);
    if (!ok) return 1;

    printf("audio sink OK: backend=%s blocks=%lu last_sample_pos=%llu last_ns=%llu\n",
           backend, blocks, (unsigned long long)p.last_sample_pos, (unsigned long long)p.last_ns);
    return 0;
}
