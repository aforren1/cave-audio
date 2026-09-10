/*
 * sink.c — backend-agnostic sink dispatch: pick a backend, forward the vtable, and own the two
 * rules that are policy rather than device code (the AUTO order, and how a `device` string is
 * matched). docs/backends.md rules 9 and 10 are implemented here and nowhere else.
 */
#include "sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sink_set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Human name for a message. Fixed strings stay ASCII (the repo rule: every one of these reaches
 * bwa_last_error, which every binding and example prints to a console). */
static const char* backend_label(bwa_sink_type t) {
    switch (t) {
    case BWA_SINK_ASIO:      return "asio";
    case BWA_SINK_WASAPI:    return "wasapi";
    case BWA_SINK_COREAUDIO: return "coreaudio";
    case BWA_SINK_ALSA:      return "alsa";
    case BWA_SINK_AAUDIO:    return "aaudio";
    case BWA_SINK_JACK:      return "jack";
    case BWA_SINK_NULL:      return "null";
    case BWA_SINK_MANUAL:    return "manual";
    default:                 return "auto";
    }
}

/* ---- device enumeration (bwa_get_device_count/_name/_id forward here) ---- */

uint32_t sink_device_count(bwa_sink_type backend) {
    switch (backend) {
#ifdef BWA_HAVE_ASIO
    case BWA_SINK_ASIO:   return sink_asio_driver_count();
#endif
#ifdef BWA_HAVE_WASAPI
    case BWA_SINK_WASAPI: return sink_wasapi_device_count();
#endif
    default: return 0;   /* AUTO/NULL/MANUAL have no devices; so does a backend not built in */
    }
}

bool sink_device_name(bwa_sink_type backend, uint32_t index, char* buf, uint32_t cap) {
    if (buf && cap) buf[0] = 0;
    switch (backend) {
#ifdef BWA_HAVE_ASIO
    case BWA_SINK_ASIO:   return sink_asio_driver_name(index, buf, cap);
#endif
#ifdef BWA_HAVE_WASAPI
    case BWA_SINK_WASAPI: return sink_wasapi_device_name(index, buf, cap);
#endif
    default: (void)index; return false;
    }
}

bool sink_device_id(bwa_sink_type backend, uint32_t index, char* buf, uint32_t cap) {
    if (buf && cap) buf[0] = 0;
    switch (backend) {
#ifdef BWA_HAVE_ASIO
    /* ASIO has no id separate from the registered name: that name IS the stable key the SDK
     * matches on, so both queries answer the same string rather than one of them answering
     * nothing (which a picker would persist as an empty choice). */
    case BWA_SINK_ASIO:   return sink_asio_driver_name(index, buf, cap);
#endif
#ifdef BWA_HAVE_WASAPI
    case BWA_SINK_WASAPI: return sink_wasapi_device_id(index, buf, cap);
#endif
    default: (void)index; return false;
    }
}

/* Rule 10: a `device` string matches EXACTLY, first against the friendly name and then against
 * the stable id. No substring matching — two endpoints called "Speakers" are common, and a
 * picker hands back an exact string. Under AUTO this decides whether a backend is even tried:
 * a backend that has no device by this name is SKIPPED, so the name reaches the one that owns it
 * rather than opening that backend's default device and playing into the wrong hardware. */
static bool backend_has_device(bwa_sink_type backend, const char* device) {
    if (!device || !*device) return true;              /* no name = the backend's own default */
    const uint32_t n = sink_device_count(backend);
    char buf[256];
    for (uint32_t i = 0; i < n; ++i) {
        if (sink_device_name(backend, i, buf, sizeof buf) && strcmp(buf, device) == 0) return true;
        if (sink_device_id  (backend, i, buf, sizeof buf) && strcmp(buf, device) == 0) return true;
    }
    return false;
}

/* ---- open ---- */

/* One backend attempt. Returns the sink, or NULL with the reason in `msg`. */
static bwa_sink* try_backend(bwa_sink_type backend, uint32_t sample_rate, uint32_t block_size,
                             uint32_t channels, const char* device, uint32_t flags, bool exact_rate,
                             bwa_render_fn render, void* user, char* msg, size_t msgcap) {
    switch (backend) {
#ifdef BWA_HAVE_ASIO
    case BWA_SINK_ASIO:
        return bwa_asio_sink_open(sample_rate, block_size, channels, device,
                                  render, user, msg, msgcap);
#endif
#ifdef BWA_HAVE_WASAPI
    case BWA_SINK_WASAPI:
        return bwa_wasapi_sink_open(sample_rate, block_size, channels, device, flags, exact_rate,
                                    render, user, msg, msgcap);
#endif
    default:
        break;
    }
    (void)sample_rate; (void)block_size; (void)channels; (void)device;
    (void)flags; (void)exact_rate; (void)render; (void)user;
    /* Reached only for a backend this build does not carry. Say WHICH and say why, because the
     * alternative is a caller staring at a silent null sink wondering what it asked for. */
    {
        char m[192];
        const char* label = backend_label(backend);
        snprintf(m, sizeof m, "%s: this build has no %s backend (see docs/backends.md for the "
                              "platforms and the build options)", label, label);
        sink_set_err(msg, msgcap, m);
    }
    return NULL;
}

bwa_sink* bwa_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                     bwa_sink_type sink_type, const char* device, uint32_t flags, bool exact_rate,
                     bwa_render_fn render, void* user, char* err, size_t errcap) {
    /* BWA_SINK_MANUAL: no device, no thread — the caller pumps blocks (bwa_render_block). Bypasses
     * all device probing; this is the offline/deterministic render path. */
    if (sink_type == BWA_SINK_MANUAL)
        return bwa_manual_sink_open(sample_rate, block_size, channels, render, user, err, errcap);
    if (sink_type == BWA_SINK_NULL)
        return bwa_null_sink_open(sample_rate, block_size, channels, render, user, err, errcap);

    char msg[256] = {0};

    /* An explicitly named backend is a DEMAND: its failure surfaces instead of hiding behind the
     * silent null sink (otherwise the caller "starts" and hears nothing). No fallback, no skip. */
    if (sink_type != BWA_SINK_AUTO) {
        bwa_sink* s = try_backend(sink_type, sample_rate, block_size, channels, device, flags,
                                  exact_rate, render, user, msg, sizeof msg);
        if (s) { sink_set_err(err, errcap, msg); return s; }   /* msg on SUCCESS = a degradation */
        if (!msg[0]) snprintf(msg, sizeof msg, "%s: open failed", backend_label(sink_type));
        sink_set_err(err, errcap, msg);
        return NULL;
    }

    /* THE AUTO ORDER (docs/backends.md). On Windows a 2-channel request is a headphone profile or
     * the cave_both monitor, and the device the headphones are actually on is a WASAPI endpoint,
     * not whichever ASIO driver happens to be registered first — the old auto-pick could land on
     * the Digiface and play the monitor into Dante channels 1 and 2. Anything wider is the array,
     * and the array's transport is a locked decision: ASIO, or nothing. Multichannel WASAPI is
     * never chosen here, because reaching it means an exclusive-mode grab of a WDM device. */
    static const bwa_sink_type ORDER_STEREO[] = { BWA_SINK_WASAPI, BWA_SINK_ASIO };
    static const bwa_sink_type ORDER_WIDE[]   = { BWA_SINK_ASIO };
    const bwa_sink_type* order = (channels <= 2) ? ORDER_STEREO : ORDER_WIDE;
    const uint32_t norder = (channels <= 2) ? 2u : 1u;

    char first_err[256] = {0};
    bool tried = false;               /* did any backend get as far as an open attempt? */
    for (uint32_t i = 0; i < norder; ++i) {
        const bwa_sink_type b = order[i];
#if !defined(BWA_HAVE_ASIO)
        if (b == BWA_SINK_ASIO) continue;
#endif
#if !defined(BWA_HAVE_WASAPI)
        if (b == BWA_SINK_WASAPI) continue;
#endif
        if (!backend_has_device(b, device)) continue;   /* rule 10: the name belongs to another backend */

        msg[0] = 0;
        tried = true;
        bwa_sink* s = try_backend(b, sample_rate, block_size, channels, device, flags, exact_rate,
                                  render, user, msg, sizeof msg);
        /* A message alongside a SUCCESSFUL open is the "succeeded but degraded" channel, not a
         * failure: the backend opened but had to accept less than it was asked for (rule 6's OS
         * resampler is the case that exists today). It has to reach the caller, because a clean
         * bwa_last_error after bwa_start is what says the device runs at the engine's rate. */
        if (s) { sink_set_err(err, errcap, msg); return s; }
        if (msg[0] && !first_err[0]) { strncpy(first_err, msg, sizeof first_err - 1); }
    }

    /* Every candidate was SKIPPED for the device name: the caller named a device no compiled
     * backend has. Silently opening the null sink would look identical to "your headphones are
     * connected but muted", so say it, through the same degraded channel. */
    if (!tried && device && *device)
        snprintf(first_err, sizeof first_err,
                 "no compiled backend has a device named '%s' (checked the exact name and the "
                 "stable id); rendering to the silent offline sink", device);

    /* Nothing real opened: fall back to the offline sink so the engine still runs (the tools'
     * visual-only mode, CI, a desk with no audio at all). */
    char null_err[256] = {0};
    bwa_sink* n = bwa_null_sink_open(sample_rate, block_size, channels, render, user,
                                     null_err, sizeof null_err);
    if (n) { sink_set_err(err, errcap, first_err); return n; }

    /* Both failed: report the real device's reason if we have one, so the null sink's message
     * cannot clobber the diagnostic that matters. */
    sink_set_err(err, errcap, first_err[0] ? first_err : null_err);
    return NULL;
}

int         bwa_sink_start(bwa_sink* s)   { return s ? s->vt->start(s) : 1; }
void        bwa_sink_stop(bwa_sink* s)    { if (s) s->vt->stop(s); }
void        bwa_sink_close(bwa_sink* s)   { if (s) s->vt->close(s); }
const char* bwa_sink_backend(bwa_sink* s) { return s ? s->vt->backend(s) : "none"; }
bwa_sink_type bwa_sink_type_of(bwa_sink* s) { return s ? s->vt->type : BWA_SINK_NULL; }
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
