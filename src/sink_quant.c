/*
 * sink_quant.c — the fixed-quantum adapter. See sink_quant.h for what it is for.
 *
 * The whole file is one loop and its accounting. It is pure (no device, no OS beyond the
 * monotonic clock), which is the point: the backends it serves cannot be tested on CI, so the
 * arithmetic they all share is tested on its own (test/sink_quant_test.c).
 */
#include "sink_quant.h"
#include "profile.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

uint64_t sink_quant_now_ns(void) {
#if defined(_WIN32)
    static uint64_t freq = 0;
    if (!freq) { LARGE_INTEGER f; freq = QueryPerformanceFrequency(&f) ? (uint64_t)f.QuadPart : 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const uint64_t t = (uint64_t)c.QuadPart;
    /* Split the scale: t * 1e9 wraps uint64 after ~30 min at a 10 MHz QPC (same fix as null_sink.c). */
    return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

int sink_quant_init(SinkQuant* q, uint32_t sample_rate, uint32_t block, uint32_t channels,
                    uint32_t max_request, bwa_render_fn render, void* user) {
    if (!q || !render || !sample_rate || !block || !channels) return 1;
    memset(q, 0, sizeof *q);
    if (max_request < block) max_request = block;

    /* A pull always starts holding fewer than B frames (it drops exactly what it served), and it
     * renders whole blocks until it has `n`. Worst case is one partly-drained slot plus
     * ceil(n / B) more. Allocated once, here, so the pull loop never does. */
    const uint64_t slots = (uint64_t)((max_request + block - 1u) / block) + 1u;
    const uint64_t floats = slots * (uint64_t)channels * (uint64_t)block;
    if (slots > 0xFFFFu || floats > (uint64_t)0x10000000) return 1;   /* absurd geometry */

    q->fifo = (float*)calloc((size_t)floats, sizeof(float));
    if (!q->fifo) return 1;
    q->rate     = sample_rate;
    q->block    = block;
    q->channels = channels;
    q->slots    = (uint32_t)slots;
    q->render   = render;
    q->user     = user;
    return 0;
}

void sink_quant_free(SinkQuant* q) {
    if (!q) return;
    free(q->fifo);
    q->fifo = NULL;
}

uint32_t sink_quant_queued(const SinkQuant* q) {
    if (!q) return 0;
    return q->filled * q->block - q->rd_off;
}

static inline float* quant_slot(SinkQuant* q, uint32_t slot) {
    return q->fifo + ((size_t)slot * q->channels) * q->block;
}

/* Render exactly one engine block into the write slot. `host_now_ns` is this callback's host
 * time; the block's own stamp is that plus the nominal duration of the frames queued ahead of
 * it, so the second block rendered inside one callback does not reuse the first one's time. */
static void quant_render_one(SinkQuant* q, uint64_t host_now_ns) {
    const uint64_t ahead    = q->rendered - q->written;   /* frames that play before this block */
    const uint64_t block_ns = (uint64_t)q->block * 1000000000ull / (uint64_t)q->rate;
    uint64_t stamp_ns = host_now_ns + ahead * 1000000000ull / (uint64_t)q->rate;

    /* THE STAMP MUST NEVER STEP BACKWARD, and the extrapolation above can make it. A callback that
     * renders several blocks stamps the later ones AHEAD of its own host time; if the device is
     * catching up after a fault its next callback fires immediately, with a host time EARLIER than
     * where the last batch was extrapolated to. The device-versus-host drift fit
     * (bwa_get_clock_model) would then see a negative interval against a forward sample position,
     * and bwa_get_clock would hand a caller a clock that ran backward.
     *
     * Advance by one nominal block instead of clamping to the previous value: equal stamps for two
     * sample positions are the zero-slope pair rule 3 warns about, which is the other way to
     * poison the same fit. Once the device catches up, host_now_ns overtakes this again and the
     * stamp re-anchors to the real clock on its own. Found by test_audio_sink's monotonicity
     * assertion on a deliberately starved WASAPI stream, not by reading the code. */
    if (q->have_stamp && stamp_ns <= q->last_stamp_ns) stamp_ns = q->last_stamp_ns + block_ns;
    q->last_stamp_ns = stamp_ns;
    q->have_stamp    = true;

    bwa_timestamp ts = { .sample_pos = q->rendered, .system_time_ns = stamp_ns };

    /* The slot's channel stride IS the block size, which is exactly the planar bus render()
     * promises to fill — so the engine writes straight into the FIFO, no staging buffer. */
    float* bus = quant_slot(q, q->wr_slot);

    BWA_ZONE_BEGIN(zb, "quant block");
    const uint64_t t0 = sink_quant_now_ns();
    q->render(q->user, bus, q->block, &ts);
    const uint64_t ns = sink_quant_now_ns() - t0;
    BWA_ZONE_END(zb);
    BWA_FRAME_MARK();

    /* Overrunning the block period is what eventually becomes a device dropout, and it is OUR
     * half of the measurement (the device's half needs a device). Two counter reads, no syscall. */
    if (ns > block_ns) q->h_late++;                       /* the budget IS one block period */
    if (ns > q->h_render_ns_peak) q->h_render_ns_peak = ns;
    q->h_blocks++;

    q->rendered += q->block;
    q->wr_slot   = (q->wr_slot + 1u) % q->slots;
    q->filled   += 1u;
}

void sink_quant_pull(SinkQuant* q, uint32_t n, uint64_t device_pos_now, bool device_pos_valid,
                     uint64_t host_now_ns, sink_quant_out_fn out, void* out_user) {
    if (!q || !q->fifo || n == 0) return;

    /* THE DROPOUT. `written` is what we handed over; `device_pos_now` is what the device says it
     * has consumed. The device ahead of us means it clocked out frames nobody rendered. Only a
     * device-reported position can show this — our own counters are continuous by construction,
     * which is exactly why `measured` gates the whole health readback. sink_position_gap supplies
     * the sane-jump window, so a reset or a stale stamp is not reported as millions of frames. */
    if (device_pos_valid) {
        if (q->have_pos) {
            const uint64_t lost = sink_position_gap(q->written, device_pos_now, q->block);
            if (lost) {
                q->h_dropouts++;
                q->h_dropped_frames += lost;
                /* Re-anchor to the device. Both counters move, so `rendered - written` (the frames
                 * held in the FIFO) stays exact and the next block's timestamp lands on the
                 * device's stream rather than on our own, now-behind, one. */
                q->written  += lost;
                q->rendered += lost;
            }
        }
        q->have_pos = true;
        q->measured = true;
    }

    const uint32_t capacity = q->slots * q->block;
    if (n > capacity) n = capacity;    /* a request past what open() sized for: serve what we can */

    const bool passthrough = (n == q->block && q->filled == 0);
    while (q->filled * q->block - q->rd_off < n) quant_render_one(q, host_now_ns);
    if (passthrough) q->h_passthrough++;   /* accounting only: one slot in, one run out, no residue */

    /* Deliver, one contiguous run per slot. A pass-through pull makes exactly one call. */
    uint32_t served = 0;
    while (served < n) {
        const uint32_t run_max = q->block - q->rd_off;
        const uint32_t run     = (n - served < run_max) ? (n - served) : run_max;
        if (out) out(out_user, quant_slot(q, q->rd_slot) + q->rd_off, q->block, served, run);
        served    += run;
        q->rd_off += run;
        if (q->rd_off == q->block) {     /* slot drained: retire it */
            q->rd_off  = 0;
            q->rd_slot = (q->rd_slot + 1u) % q->slots;
            q->filled -= 1u;
        }
    }
    q->written += n;
}

void sink_quant_note_dropout(SinkQuant* q, uint64_t frames) {
    if (!q) return;
    /* Same window the position rule uses: a dropout can genuinely span many blocks, but a garbage
     * time delta must not be believed. Past the window the EVENT still counts (the backend saw a
     * real fault) and only the unbelievable frame estimate is dropped. */
    const uint64_t sane = (uint64_t)(q->block ? q->block : 1u) * 4096ull;
    q->h_dropouts++;
    q->h_dropped_frames += (frames <= sane) ? frames : sane;
    q->measured = true;
}

void sink_quant_health(const SinkQuant* q, bwa_sink_health* out) {
    if (!out) return;
    if (!q) { memset(out, 0, sizeof *out); return; }
    out->blocks         = q->h_blocks;
    out->dropouts       = q->h_dropouts;
    out->dropped_frames = q->h_dropped_frames;
    out->driver_resyncs = 0;                 /* the backend's to fill: only the device API knows */
    out->late_blocks    = q->h_late;
    out->render_ns_peak = q->h_render_ns_peak;
    out->period_ns      = q->rate ? (uint64_t)q->block * 1000000000ull / (uint64_t)q->rate : 0;
    out->measured       = q->measured;
}
