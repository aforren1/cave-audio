/*
 * sink.h — internal device-sink abstraction (NOT part of the public ABI).
 *
 * The architecture keeps ASIO "just the Windows sink": the engine renders into an
 * in-memory 26-channel master bus, and one or more *sinks* consume it (see
 * docs/architecture.md). This header is the seam. Do not let ASIO types leak past
 * the asio_sink.c boundary — everything here is device-agnostic.
 *
 * Backends:
 *   - asio_sink.c  (BWA_HAVE_ASIO): drives a real ASIO driver (DVS in production).
 *   - null_sink.c  (always built): a threaded *offline* sink that paces blocks from
 *                  a high-resolution clock and discards the audio. Lets the engine
 *                  run with no hardware (desk dev, CI, the `binaural` array path).
 *
 * Bus layout: PLANAR, channel-major. For an N-channel bus of `nframes` samples,
 * channel `c` sample `i` lives at bus[c * nframes + i]. This matches the per-channel
 * work in align_speakers and the ASIO planar driver buffers (one buffer per channel).
 */
#ifndef BWA_SINK_H
#define BWA_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The array-width CAPACITY — sizes every fixed array (gain vectors, decode matrices, meters). The
 * ACTIVE channel count is the loaded layout's speaker count (4..BWA_CHANNELS, Layout.count; the
 * default grid is exactly BWA_CHANNELS), fixed per engine instance — a collaborator's 24-speaker
 * array loads into the same binary. Raising the cap is a recompile. */
#define BWA_CHANNELS 26

/* TEST HOOK (defined in null_sink.c, exported from the dll; deliberately not in bw_audio.h):
 * when set, observers of the device-bound audio call it — the null sink hands it every block it
 * would discard, and render_binaural hands it the monitor's stereo on ANY sink. It is the seam
 * that lets a test (or a rig-side probe) see what actually goes to the device. Read your
 * accumulators only once rendering has stopped; the hook runs on the sink's render thread. */
#ifdef BWA_BUILD_DLL
__declspec(dllexport)
#endif
extern void (*bwa_null_sink_tap)(const float* bus, uint32_t channels, uint32_t block_size);

/* Hardware-anchored timestamp captured at the top of each block. Mirrors what ASIO
 * delivers via ASIOTime (sample position + nanosecond systemTime); the null sink
 * synthesizes it from QueryPerformanceCounter. */
typedef struct {
    uint64_t sample_pos;       /* running output sample-frame position             */
    uint64_t system_time_ns;   /* monotonic host time at block start, nanoseconds   */
} bwa_timestamp;

/* The engine's per-block render. Called on the audio thread, once per buffer.
 * MUST fill `bus` (planar, channels * nframes floats) and obey the audio-thread
 * invariants (no alloc/lock/syscall/I/O — see CLAUDE.md). `user` is the engine. */
typedef void (*bwa_render_fn)(void* user, float* bus, uint32_t nframes, const bwa_timestamp* ts);

typedef struct bwa_sink bwa_sink;

/* Backend dispatch. Each concrete sink embeds `struct bwa_sink` as its FIRST member and
 * fills `vt`; the generic bwa_sink_* calls (in sink.c) dispatch through it. */
typedef struct {
    int         (*start)(bwa_sink*);
    void        (*stop)(bwa_sink*);
    void        (*close)(bwa_sink*);
    const char* (*backend)(bwa_sink*);
    uint32_t    (*block_size)(bwa_sink*);   /* actual frames per callback (driver dictates for ASIO) */
    /* Device-reported render->DAC output latency in FRAMES (ASIOGetLatencies for the ASIO sink —
     * DVS includes its network buffering there). NULL entry or 0 = unknown / no physical output
     * (the null and manual sinks have no DAC). */
    uint32_t    (*output_latency)(bwa_sink*);
    /* MANUAL sink only (NULL on threaded backends): render ONE block synchronously on the CALLER's
     * thread into the sink's own bus, returning a pointer to it (planar, *channels * *nframes floats)
     * valid until the next call. This is the offline/deterministic render path (bwa_render_block). */
    const float* (*render_block)(bwa_sink*, uint32_t* channels, uint32_t* nframes);
} bwa_sink_vtbl;

struct bwa_sink { const bwa_sink_vtbl* vt; };

/* Open a sink for this format per `sink_type` (bwa_sink_type: 0 = auto — ASIO if compiled in and
 * a driver opens, else the null sink; 1 = demand ASIO, failure returns NULL; 2 = force null).
 * `asio_driver` names the ASIO driver to open (NULL = auto-pick by channel count). On failure
 * returns NULL and writes a message to `err` (if err/errcap given). Does NOT start the audio
 * thread yet. */
bwa_sink* bwa_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                     int sink_type, const char* asio_driver,
                     bwa_render_fn render, void* user, char* err, size_t errcap);

int          bwa_sink_start(bwa_sink* s);   /* begin the callback loop; 0 = ok        */
void         bwa_sink_stop(bwa_sink* s);    /* stop the loop; safe if already stopped */
void         bwa_sink_close(bwa_sink* s);   /* stop (if needed) + release             */
const char*  bwa_sink_backend(bwa_sink* s); /* e.g. "asio:DVS" or "null"              */
uint32_t     bwa_sink_block_size(bwa_sink* s); /* actual frames per block; 0 if none  */
uint32_t     bwa_sink_output_latency(bwa_sink* s); /* device render->DAC latency, frames; 0 = unknown */
/* Manual/offline: render one block synchronously (bwa_render_block). NULL unless this is a manual
 * sink — threaded backends don't support caller-driven pumping. */
const float* bwa_sink_render_block(bwa_sink* s, uint32_t* channels, uint32_t* nframes);

/* Backend constructors used by bwa_sink_open. null is always present; asio only when
 * BWA_HAVE_ASIO is defined (third_party/asiosdk vendored — see third_party/README.md). */
bwa_sink* bwa_null_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          bwa_render_fn render, void* user, char* err, size_t errcap);
/* Manual/offline sink: no thread — bwa_sink_render_block pumps blocks on the caller's thread with a
 * DETERMINISTIC clock (sample_pos + nominal block time, no wall clock), so a render is reproducible. */
bwa_sink* bwa_manual_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          bwa_render_fn render, void* user, char* err, size_t errcap);
#ifdef BWA_HAVE_ASIO
bwa_sink* bwa_asio_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          const char* driver /* NULL = auto-pick */,
                          bwa_render_fn render, void* user, char* err, size_t errcap);
/* Registered-driver enumeration (bwa_get_asio_driver_count/_name): control thread, needs no
 * engine or device — reads the OS's driver registry, loads nothing. asio_sink.cpp implements
 * it (the one place ASIO lives). */
uint32_t sink_asio_driver_count(void);
bool     sink_asio_driver_name(uint32_t index, char* buf, uint32_t cap);
#endif

#endif /* BWA_SINK_H */
