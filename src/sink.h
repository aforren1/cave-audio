/*
 * sink.h — internal device-sink abstraction (NOT part of the public ABI).
 *
 * The architecture keeps ASIO "just the Windows sink": the engine renders into an
 * in-memory 26-channel master bus, and one or more *sinks* consume it (see
 * docs/architecture.md). This header is the seam. Do not let ASIO types leak past
 * the asio_sink.c boundary — everything here is device-agnostic.
 *
 * Backends:
 *   - asio_sink.c  (BW_HAVE_ASIO): drives a real ASIO driver (DVS in production).
 *   - null_sink.c  (always built): a threaded *offline* sink that paces blocks from
 *                  a high-resolution clock and discards the audio. Lets the engine
 *                  run with no hardware (desk dev, CI, the `binaural` array path).
 *
 * Bus layout: PLANAR, channel-major. For an N-channel bus of `nframes` samples,
 * channel `c` sample `i` lives at bus[c * nframes + i]. This matches the per-channel
 * work in align_speakers and the ASIO planar driver buffers (one buffer per channel).
 */
#ifndef BW_SINK_H
#define BW_SINK_H

#include <stddef.h>
#include <stdint.h>

/* Hardware-anchored timestamp captured at the top of each block. Mirrors what ASIO
 * delivers via ASIOTime (sample position + nanosecond systemTime); the null sink
 * synthesizes it from QueryPerformanceCounter. */
typedef struct {
    uint64_t sample_pos;       /* running output sample-frame position             */
    uint64_t system_time_ns;   /* monotonic host time at block start, nanoseconds   */
} BwTimestamp;

/* The engine's per-block render. Called on the audio thread, once per buffer.
 * MUST fill `bus` (planar, channels * nframes floats) and obey the audio-thread
 * invariants (no alloc/lock/syscall/I/O — see CLAUDE.md). `user` is the engine. */
typedef void (*BwRenderFn)(void* user, float* bus, uint32_t nframes, const BwTimestamp* ts);

typedef struct BwSink BwSink;

/* Backend dispatch. Each concrete sink embeds `struct BwSink` as its FIRST member and
 * fills `vt`; the generic bw_sink_* calls (in sink.c) dispatch through it. */
typedef struct {
    int         (*start)(BwSink*);
    void        (*stop)(BwSink*);
    void        (*close)(BwSink*);
    const char* (*backend)(BwSink*);
} BwSinkVtbl;

struct BwSink { const BwSinkVtbl* vt; };

/* Open the best available sink for this format: ASIO if compiled in and a driver
 * opens, otherwise the null sink. On failure returns NULL and writes a message to
 * `err` (if err/errcap given). Does NOT start the audio thread yet. */
BwSink* bw_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                     BwRenderFn render, void* user, char* err, size_t errcap);

int          bw_sink_start(BwSink* s);   /* begin the callback loop; 0 = ok        */
void         bw_sink_stop(BwSink* s);    /* stop the loop; safe if already stopped */
void         bw_sink_close(BwSink* s);   /* stop (if needed) + release             */
const char*  bw_sink_backend(BwSink* s); /* e.g. "asio:DVS" or "null"              */

/* Backend constructors used by bw_sink_open. null is always present; asio only when
 * BW_HAVE_ASIO is defined (third_party/asiosdk vendored — see third_party/README.md). */
BwSink* bw_null_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          BwRenderFn render, void* user, char* err, size_t errcap);
#ifdef BW_HAVE_ASIO
BwSink* bw_asio_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                          BwRenderFn render, void* user, char* err, size_t errcap);
#endif

#endif /* BW_SINK_H */
