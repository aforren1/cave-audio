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

/* ---- sink flags (bwa_desc.sink_flags) on the command line --------------------------------- */

/* The tools take the three bits one at a time (--exclusive, --exact-rate, --tight-buffer) and as
 * one LATENCY CLASS, which is PsychToolbox's PsychPortAudio reqlatencyclass 0..4 in this engine's
 * terms. A psychophysics rig arrives already knowing which class it needs, and the mapping is
 * otherwise something you have to reconstruct from two sets of source. docs/api.md's
 * "Latency classes" carries the same table. */
enum { BWA_EX_MAX_LATENCY_CLASS = 4 };

/* Flags for a class. Classes 0 and 1 are shared mode with the OS free to resample, which is what
 * PsychPortAudio's paWinWasapiAutoConvert does; 2 and 3 take the device; 4 adds "fail rather than
 * convert" (paMacCoreFailIfConversionRequired, eStreamOptionMatchFormat). */
static inline uint32_t bwa_ex_latency_class_flags(int cls) {
    switch (cls) {
    case 2:  return BWA_SINK_FLAG_EXCLUSIVE;
    case 3:  return BWA_SINK_FLAG_EXCLUSIVE | BWA_SINK_FLAG_TIGHT_BUFFER;
    case 4:  return BWA_SINK_FLAG_EXCLUSIVE | BWA_SINK_FLAG_TIGHT_BUFFER | BWA_SINK_FLAG_EXACT_RATE;
    default: return 0u;
    }
}

/* The block size that goes with a class. A HINT only: the flags decide what the device is asked
 * for, and the engine's render quantum is whatever bwa_desc.block_size ends up saying. An explicit
 * --block on the command line wins over this. */
static inline uint32_t bwa_ex_latency_class_block(int cls) {
    switch (cls) {
    case 0:  return 512;
    case 1:  return 256;
    case 2:  return 256;
    case 3:  return 128;
    case 4:  return 128;
    default: return 0;
    }
}

/* Consume one flag argument. Returns 1 when `arg` was one of ours (and ORs the bit into *flags),
 * 0 otherwise, so a caller's argument loop can chain it in front of its own cases. */
static inline int bwa_ex_parse_sink_flag(const char* arg, uint32_t* flags) {
    if (!arg || !flags) return 0;
    if (!strcmp(arg, "--exclusive"))    { *flags |= BWA_SINK_FLAG_EXCLUSIVE;    return 1; }
    if (!strcmp(arg, "--exact-rate"))   { *flags |= BWA_SINK_FLAG_EXACT_RATE;   return 1; }
    if (!strcmp(arg, "--tight-buffer")) { *flags |= BWA_SINK_FLAG_TIGHT_BUFFER; return 1; }
    return 0;
}

/* --latency-class N. Writes the class's flags and its block hint; returns 0 for a class outside
 * 0..4 so the caller can refuse the run rather than pick something the user did not ask for. */
static inline int bwa_ex_parse_latency_class(const char* v, uint32_t* flags, uint32_t* block_hint) {
    if (!v || !flags) return 0;
    if (v[0] < '0' || v[0] > '0' + BWA_EX_MAX_LATENCY_CLASS || v[1]) return 0;
    const int cls = v[0] - '0';
    *flags |= bwa_ex_latency_class_flags(cls);
    if (block_hint) *block_hint = bwa_ex_latency_class_block(cls);
    return 1;
}

/* "shared" when nothing is set, else the bits joined by '+'. ASCII, for a console or a HUD. */
static inline const char* bwa_ex_sink_flags_string(uint32_t flags, char* buf, size_t cap) {
    if (!buf || cap == 0) return "";
    buf[0] = 0;
    if (!flags) { snprintf(buf, cap, "shared"); return buf; }
    snprintf(buf, cap, "%s%s%s",
             (flags & BWA_SINK_FLAG_EXCLUSIVE)    ? "exclusive" : "",
             (flags & BWA_SINK_FLAG_EXACT_RATE)   ? ((flags & BWA_SINK_FLAG_EXCLUSIVE) ? "+exact-rate" : "exact-rate") : "",
             (flags & BWA_SINK_FLAG_TIGHT_BUFFER) ? ((flags & (BWA_SINK_FLAG_EXCLUSIVE | BWA_SINK_FLAG_EXACT_RATE)) ? "+tight-buffer" : "tight-buffer") : "");
    return buf;
}

/* The --help block for all four switches, so both tools document them the same way. */
static inline void bwa_ex_print_sink_flag_help(void) {
    printf("  --exclusive        take the device from every other application (WASAPI exclusive\n"
           "                     mode, AAudio MMAP); lowest latency, but a VR runtime, the\n"
           "                     browser and the OS lose the endpoint while this runs\n"
           "  --exact-rate       fail the open if the device cannot run at the engine rate,\n"
           "                     instead of letting the OS resample\n"
           "  --tight-buffer     smallest device buffer the backend can take (ALSA: 2 periods;\n"
           "                     AAudio: 1 burst); trades dropout margin for latency\n"
           "  --latency-class N  PsychPortAudio's reqlatencyclass 0..4 as a shorthand:\n"
           "                       0  shared, 512-frame blocks, OS may resample\n"
           "                       1  shared, 256-frame blocks, OS may resample\n"
           "                       2  --exclusive, 256-frame blocks\n"
           "                       3  --exclusive --tight-buffer, 128-frame blocks\n"
           "                       4  --exclusive --tight-buffer --exact-rate, 128-frame blocks\n"
           "                     the block size is a hint; an explicit one wins\n");
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
