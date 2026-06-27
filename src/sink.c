/*
 * sink.c — backend-agnostic sink dispatch. Picks ASIO when available, else the null
 * sink, and forwards start/stop/close/backend through the vtable.
 */
#include "sink.h"

#include <stdlib.h>
#include <string.h>

BwSink* bw_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                     BwRenderFn render, void* user, char* err, size_t errcap) {
    /* Override: BWAUDIO_SINK=null forces the offline sink (CI, desk debugging without
     * touching audio hardware); BWAUDIO_SINK=asio asks for ASIO explicitly. Default is
     * auto: try the real device, fall back to offline. */
    const char* want = getenv("BWAUDIO_SINK");
    const int force_null = (want && strcmp(want, "null") == 0);

    char asio_err[256] = {0};
#ifdef BW_HAVE_ASIO
    if (!force_null) {
        /* Production path: try the real device first. */
        BwSink* a = bw_asio_sink_open(sample_rate, block_size, channels, render, user,
                                      asio_err, sizeof asio_err);
        if (a) return a;
    }
#else
    (void)force_null;
#endif

    char null_err[256] = {0};
    BwSink* n = bw_null_sink_open(sample_rate, block_size, channels, render, user,
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

int         bw_sink_start(BwSink* s)   { return s ? s->vt->start(s) : 1; }
void        bw_sink_stop(BwSink* s)    { if (s) s->vt->stop(s); }
void        bw_sink_close(BwSink* s)   { if (s) s->vt->close(s); }
const char* bw_sink_backend(BwSink* s) { return s ? s->vt->backend(s) : "none"; }
