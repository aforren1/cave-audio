/*
 * sink.c — backend-agnostic sink dispatch. Picks ASIO when available, else the null
 * sink, and forwards start/stop/close/backend through the vtable.
 */
#include "sink.h"

#include <stdlib.h>
#include <string.h>

bwa_sink* bwa_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                     int sink_type, const char* asio_driver,
                     bwa_render_fn render, void* user, char* err, size_t errcap) {
    /* bwa_desc.sink: BWA_SINK_NULL forces the offline sink (CI, desk debugging without
     * touching audio hardware); BWA_SINK_ASIO asks for ASIO explicitly. Default is
     * auto: try the real device, fall back to offline. */
    const int force_null = (sink_type == 2);   /* BWA_SINK_NULL */
    const int force_asio = (sink_type == 1);   /* BWA_SINK_ASIO */

    /* BWA_SINK_MANUAL: no device, no thread — the caller pumps blocks (bwa_render_block). Bypasses all
     * device probing; this is the offline/deterministic render path. */
    if (sink_type == 3)
        return bwa_manual_sink_open(sample_rate, block_size, channels, render, user, err, errcap);

    char asio_err[256] = {0};
#ifdef BWA_HAVE_ASIO
    if (!force_null) {
        /* Production path: try the real device first. */
        bwa_sink* a = bwa_asio_sink_open(sample_rate, block_size, channels, asio_driver,
                                      render, user, asio_err, sizeof asio_err);
        if (a) return a;
        /* BWA_SINK_ASIO is an explicit request: surface the failure instead of hiding
         * it behind the silent null sink (otherwise the caller "starts" but hears nothing). */
        if (force_asio) {
            if (err && errcap) {
                strncpy(err, asio_err[0] ? asio_err : "asio: open failed", errcap - 1);
                err[errcap - 1] = 0;
            }
            return NULL;
        }
    }
#else
    if (force_asio) {
        if (err && errcap) { strncpy(err, "asio: engine built without the ASIO SDK", errcap - 1); err[errcap - 1] = 0; }
        return NULL;
    }
    (void)force_null;
#endif

    char null_err[256] = {0};
    bwa_sink* n = bwa_null_sink_open(sample_rate, block_size, channels, render, user,
                                  null_err, sizeof null_err);
    if (n) return n;

    /* Both failed: report the ASIO (production) reason if we have one, so the null
     * sink's message can't clobber the real device-failure diagnostic. */
    if (err && errcap) {
        const char* msg = asio_err[0] ? asio_err : null_err;
        strncpy(err, msg, errcap - 1);
        err[errcap - 1] = 0;
    }
    return NULL;
}

int         bwa_sink_start(bwa_sink* s)   { return s ? s->vt->start(s) : 1; }
void        bwa_sink_stop(bwa_sink* s)    { if (s) s->vt->stop(s); }
void        bwa_sink_close(bwa_sink* s)   { if (s) s->vt->close(s); }
const char* bwa_sink_backend(bwa_sink* s) { return s ? s->vt->backend(s) : "none"; }
uint32_t    bwa_sink_block_size(bwa_sink* s) { return s ? s->vt->block_size(s) : 0; }
uint32_t    bwa_sink_output_latency(bwa_sink* s) { return (s && s->vt->output_latency) ? s->vt->output_latency(s) : 0; }
const float* bwa_sink_render_block(bwa_sink* s, uint32_t* channels, uint32_t* nframes) {
    return (s && s->vt->render_block) ? s->vt->render_block(s, channels, nframes) : NULL;
}

void bwa_sink_get_health(bwa_sink* s, bwa_sink_health* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);            /* measured = false: no sink, or a backend that measures nothing */
    if (s && s->vt->health) s->vt->health(s, out);
}

/* The gap rule, in one place because both threaded sinks and the test share it.
 *
 * A dropout is the device advancing PAST where our next callback was predicted to land: it clocked
 * out audio we never rendered. Everything else that can move a reported position is deliberately
 * NOT a dropout:
 *   - actual < expected  the position went backward — a driver reset or a stale/garbage stamp.
 *   - a jump beyond the sane window — same story, and counting it would report millions of lost
 *     frames from one bad stamp, which is worse than missing a real dropout.
 * The window is generous (a full second of blocks) because a genuine dropout under load can span
 * many blocks, while a reset typically lands nowhere near the running position. */
uint64_t sink_position_gap(uint64_t expected, uint64_t actual, uint32_t block) {
    if (actual <= expected) return 0;
    const uint64_t gap = actual - expected;
    const uint64_t sane = (uint64_t)(block ? block : 1u) * 4096ull;
    return gap <= sane ? gap : 0;
}
