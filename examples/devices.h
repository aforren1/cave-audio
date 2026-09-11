/*
 * devices.h — the examples' device-query helper: the backend list, its labels, and the
 * `--list-devices` printout, shared so bwa_playground and bwa_minimal name a device the same way.
 *
 * Client code of the public ABI only (bwa_get_device_count/_name/_id from include/bw_audio.h), like
 * portable.h: the examples never reach into src/. The engine picks a backend on its own under
 * BWA_SINK_AUTO; this is only what a tool needs to SHOW the choice and to pass one back through
 * bwa_desc.device. See docs/backends.md, "AUTO order" and "Selection semantics".
 */
#ifndef BWA_EXAMPLES_DEVICES_H
#define BWA_EXAMPLES_DEVICES_H

#include <stdio.h>
#include <string.h>

#include "bw_audio.h"

/* The concrete backends this platform can carry, in the order AUTO tries them for a 2-channel
 * request. A backend this build left out reports 0 devices rather than failing, so an absent one
 * costs one call and drops out of every list below. */
static const bwa_sink_type bwa_ex_backends[] = {
#if defined(_WIN32)
    BWA_SINK_WASAPI, BWA_SINK_ASIO
#elif defined(__APPLE__)
    BWA_SINK_COREAUDIO
#else
    BWA_SINK_JACK, BWA_SINK_ALSA
#endif
};
enum { BWA_EX_NBACKEND = (int)(sizeof bwa_ex_backends / sizeof bwa_ex_backends[0]) };

/* The name the backend answers to on the command line, and the prefix bwa_get_audio_backend
 * prints. ASCII (it reaches a console). */
static inline const char* bwa_ex_backend_label(bwa_sink_type s) {
    switch (s) {
    case BWA_SINK_WASAPI:    return "wasapi";
    case BWA_SINK_ASIO:      return "asio";
    case BWA_SINK_COREAUDIO: return "coreaudio";
    case BWA_SINK_ALSA:      return "alsa";
    case BWA_SINK_JACK:      return "jack";
    case BWA_SINK_NULL:      return "null";
    case BWA_SINK_MANUAL:    return "manual";
    default:                 return "auto";
    }
}

/* --sink <name>. AUTO and NULL are always accepted; a concrete backend only if this platform's
 * list carries it, so `--sink jack` on Windows fails here instead of inside bwa_start. */
static inline int bwa_ex_parse_sink(const char* s, bwa_sink_type* out) {
    if (!s || !out) return 0;
    if (!strcmp(s, "auto")) { *out = BWA_SINK_AUTO; return 1; }
    if (!strcmp(s, "null")) { *out = BWA_SINK_NULL; return 1; }
    for (int b = 0; b < BWA_EX_NBACKEND; ++b)
        if (!strcmp(s, bwa_ex_backend_label(bwa_ex_backends[b]))) { *out = bwa_ex_backends[b]; return 1; }
    return 0;
}

/* --list-devices: every device of every backend, labeled with the backend that owns it and with
 * the stable id, because bwa_desc.device takes either string and two endpoints often share a
 * friendly name. `only` names one backend (the deprecated --list-drivers); BWA_SINK_AUTO = all. */
static inline void bwa_ex_list_devices(bwa_sink_type only) {
    char nm[192], id[256];
    for (int b = 0; b < BWA_EX_NBACKEND; ++b) {
        bwa_sink_type s = bwa_ex_backends[b];
        if (only != BWA_SINK_AUTO && s != only) continue;
        uint32_t n = bwa_get_device_count(s);
        printf("%s devices (%u):\n", bwa_ex_backend_label(s), n);
        for (uint32_t i = 0; i < n; ++i) {
            if (!bwa_get_device_name(s, i, nm, sizeof nm)) continue;
            if (!bwa_get_device_id(s, i, id, sizeof id)) id[0] = '\0';
            printf("  %2u. %s\n", i, nm);
            if (id[0] && strcmp(id, nm) != 0) printf("      id: %s\n", id);
        }
    }
    printf("--device takes either the name or the id, matched exactly.\n");
}

#endif /* BWA_EXAMPLES_DEVICES_H */
