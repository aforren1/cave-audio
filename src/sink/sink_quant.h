/*
 * sink_quant.h — the fixed-quantum adapter (docs/backends.md, contract rule 1).
 *
 * WHY IT EXISTS. The engine renders a FIXED block: with the SDK the headphone decode is created
 * for one frame size and silences any other (steam_decode.c), and cave_both's handoff exchanges
 * blocks of exactly the array's size. ASIO gives that for free — the driver's buffer size is
 * fixed once buffers exist. No other device API promises it: WASAPI shared mode hands out
 * `bufferFrameCount - GetCurrentPadding()` per event, CoreAudio can change size on a route
 * change, AAudio treats the frame count as a hint. So the backends that cannot pin the size
 * render whole engine blocks into a small FIFO and hand the device whatever count it asked for.
 *
 * WHY THE FIFO IS A RING OF BLOCKS, not of frames. render() fills a planar bus whose channel
 * stride IS nframes, so a block only lands somewhere without a copy if that somewhere has a
 * block-sized stride. Slots do; a frame-indexed ring does not. The device's frames then come
 * straight out of the slots, which is why sink_quant_out_fn can be called more than once per
 * pull: one call per contiguous run inside a slot. Nothing is copied on either side.
 *
 * WHAT IT COSTS. Nothing when the device size already equals the block size and the FIFO is
 * empty: that pull renders one block into one slot and the backend converts straight out of it,
 * in a single out call. Otherwise the adapter holds up to one block of latency, which
 * output_latency() must add (sink_quant_queued).
 *
 * AUDIO THREAD. Everything but sink_quant_init/free runs in the render path. The slots are
 * allocated once at open, sized for the worst case (see sink_quant_init), and the pull loop
 * allocates, locks, and logs nothing.
 */
#ifndef BWA_SINK_QUANT_H
#define BWA_SINK_QUANT_H

#include "sink/sink.h"

#include <stdbool.h>
#include <stdint.h>

/* Hand one contiguous run of the FIFO to the device. `planar` points at channel 0's first frame
 * of the run; channel c's data is at `planar + (size_t)c * stride`. `frame_offset` is how many
 * frames of THIS device request were already delivered by earlier calls, so the backend knows
 * where in its buffer the run goes. sink_convert_interleaved takes the same stride, so a backend
 * converts straight out of the FIFO with no intermediate copy. */
typedef void (*sink_quant_out_fn)(void* user, const float* planar, uint32_t stride,
                                  uint32_t frame_offset, uint32_t nframes);

typedef struct {
    /* --- geometry, fixed at init --- */
    uint32_t      rate;
    uint32_t      block;        /* B: the engine's render quantum, the ONLY nframes render() sees */
    uint32_t      channels;
    uint32_t      slots;        /* FIFO depth in whole blocks                                     */
    bwa_render_fn render;
    void*         user;
    float*        fifo;         /* slots * channels * block; slot s, channel c at
                                 * fifo + ((size_t)s * channels + c) * block                      */

    /* --- FIFO + stream position, audio thread only --- */
    uint32_t      rd_slot;      /* slot being served                                */
    uint32_t      rd_off;       /* frames already served out of that slot           */
    uint32_t      wr_slot;      /* slot the next render fills                       */
    uint32_t      filled;       /* slots holding rendered audio (rd_slot forward)   */
    uint64_t      rendered;     /* frames rendered: the stream position the NEXT block starts at */
    uint64_t      written;      /* frames handed to the device                                  */
    uint64_t      last_stamp_ns;/* the previous block's system_time_ns, to keep them increasing */
    bool          have_stamp;   /* last_stamp_ns holds a real value                             */
    bool          have_pos;     /* a device position has been seen, so a gap is comparable      */

    /* --- health. Written on the audio thread, read from the control thread. Every one is a
     * monotonic count nobody synchronizes ON, and a reader that sees one field a block stale is
     * reading a monotonic count either way — the same call the ASIO sink's relaxed atomics make.
     * Plain uint64_t here keeps the file off the /experimental:c11atomics list. --- */
    uint64_t      h_blocks, h_dropouts, h_dropped_frames, h_late;
    SinkPeakWindow h_peak;         /* worst render time over the last few seconds (sink.h)       */
    uint64_t      h_passthrough;   /* pulls that took the no-residue fast path (accounting only) */
    bool          measured;        /* the device reported a position at least once               */

    /* The clock a block's RENDER TIME is measured with. sink_quant_now_ns unless a test swaps it
     * after init, which is how the test injects a slow block or a clock that steps backward. */
    uint64_t    (*clock_ns)(void);
} SinkQuant;

/* Allocate the slots and latch the geometry. `max_request` is the largest nframes the device can
 * ask for in one callback (its buffer size). Depth is ceil(max_request / B) + 1 blocks: a pull
 * always begins holding fewer than B frames, and it renders whole blocks until it has enough.
 * Returns 0 on success, nonzero on a bad argument or an allocation failure. Control thread. */
int  sink_quant_init(SinkQuant* q, uint32_t sample_rate, uint32_t block, uint32_t channels,
                     uint32_t max_request, bwa_render_fn render, void* user);
void sink_quant_free(SinkQuant* q);       /* control thread, after the audio thread has joined */

/* Serve one device callback. Renders whole blocks until `n` frames are held, hands them to `out`
 * (once per contiguous run), and drops them.
 *
 * `device_pos_now` is the device's own stream position in frames and `device_pos_valid` says
 * whether it can be trusted this callback. When it can, the queued depth `written - device_pos`
 * must sit at or above zero; a device position PAST what was written is audio clocked out that
 * nobody rendered, which is the definition of a dropout. The position then becomes the truth and
 * the adapter re-anchors to it, the way the ASIO sink takes the driver's position.
 *
 * `host_now_ns` is the monotonic host time for this callback. Each rendered block gets that time
 * plus the nominal duration of the frames already queued ahead of it, so two blocks rendered
 * inside one callback do not share a host time (docs/backends.md rule 3: a repeated time feeds
 * the drift fit a zero-slope pair). Audio thread. */
void sink_quant_pull(SinkQuant* q, uint32_t n, uint64_t device_pos_now, bool device_pos_valid,
                     uint64_t host_now_ns, sink_quant_out_fn out, void* out_user);

/* Frames the adapter is holding right now: what output_latency() adds to the device's own figure. */
uint32_t sink_quant_queued(const SinkQuant* q);

/* Book a dropout the BACKEND detected some other way than the queued-depth rule above. The counters
 * stay in one place - a backend that kept its own would have to be merged back in at readback, and
 * the two would drift. `frames` is that backend's estimate of the silence and is clamped to the
 * same sane window sink_position_gap uses, so one absurd time delta cannot report millions of lost
 * frames. Audio thread; allocates and locks nothing.
 *
 * WASAPI shared mode is why this exists: its request size is exactly what the device consumed
 * since the last callback, so `written` telescopes to follow the device position and the depth can
 * never go negative. The device reports the same fault directly instead (see wasapi_sink.cpp). */
void sink_quant_note_dropout(SinkQuant* q, uint64_t frames);

/* Fill the fields the adapter can measure (blocks, dropouts, dropped_frames, late_blocks,
 * render_ns_peak, period_ns, measured). `driver_resyncs` is the backend's to set — only the
 * device API knows about one — and a backend may override `measured` when it can observe
 * dropouts by some other route. Control thread. */
void sink_quant_health(const SinkQuant* q, bwa_sink_health* out);

/* Monotonic host time in nanoseconds, the clock every backend stamps its callbacks with. Exposed
 * because the backend reads it before calling in (rule 3 wants ONE capture per callback), and
 * because a backend that lost its device paces from it. Cheap: a userspace counter read. */
uint64_t sink_quant_now_ns(void);

#endif /* BWA_SINK_QUANT_H */
