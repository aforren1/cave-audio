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

#ifdef BW_HAVE_ASIO
    if (!force_null) {
        /* Production path: try the real device first. */
        char asio_err[256] = {0};
        BwSink* a = bw_asio_sink_open(sample_rate, block_size, channels, render, user,
                                      asio_err, sizeof asio_err);
        if (a) return a;
        /* keep the ASIO reason around in case the null sink also fails */
        if (err && errcap) { strncpy(err, asio_err, errcap - 1); err[errcap - 1] = 0; }
    }
#else
    (void)force_null;
#endif
    return bw_null_sink_open(sample_rate, block_size, channels, render, user, err, errcap);
}

int         bw_sink_start(BwSink* s)   { return s ? s->vt->start(s) : 1; }
void        bw_sink_stop(BwSink* s)    { if (s) s->vt->stop(s); }
void        bw_sink_close(BwSink* s)   { if (s) s->vt->close(s); }
const char* bw_sink_backend(BwSink* s) { return s ? s->vt->backend(s) : "none"; }
