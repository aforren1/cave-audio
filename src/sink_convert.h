/*
 * sink_convert.h — planar float bus -> the device's sample format, planar or interleaved.
 *
 * Moved out of asio_sink.cpp so every backend shares one set of clamp/NaN rules. ASIO takes
 * planar driver buffers (one per channel) and keeps calling the planar variants; every other
 * API on the roadmap (WASAPI, CoreAudio, ALSA, AAudio) wants interleaved frames, so each
 * format has both layouts. JACK needs neither: its ports are planar float already.
 *
 * THE NaN RULE. NaN converts to 0, never to an out-of-range integer. The limiter is optional
 * (bwa_set_limiter), so a NaN CAN reach here, and it slips BOTH clamp compares (`v >= 1` and
 * `v <= -1` are false for NaN) — after which float->int is undefined behavior and the driver
 * gets whatever the instruction happened to leave behind, which is a full-scale pop on real
 * speakers. Silence is the only safe answer.
 *
 * AUDIO THREAD. Everything here runs in the render path: header-only, no allocation, no
 * branches per sample beyond the clamps, and `src` is read once.
 */
#ifndef BWA_SINK_CONVERT_H
#define BWA_SINK_CONVERT_H

#include <stdint.h>
#include <string.h>

/* The formats a backend may negotiate. Anything else is rejected at open(), so the audio
 * thread never reaches a default case. Ordered by preference: float32 first (no conversion
 * at all), then descending integer width. */
typedef enum {
    SINK_FMT_F32 = 0,   /* 32-bit float, [-1, 1]                    */
    SINK_FMT_I32 = 1,   /* 32-bit signed, little-endian             */
    SINK_FMT_I24 = 2,   /* 24-bit signed, PACKED (3 bytes/sample)   */
    SINK_FMT_I16 = 3    /* 16-bit signed, little-endian             */
} sink_fmt;

/* Bytes one sample of `fmt` occupies in the device buffer. */
static inline uint32_t sink_fmt_bytes(sink_fmt fmt) {
    switch (fmt) {
    case SINK_FMT_F32: return 4;
    case SINK_FMT_I32: return 4;
    case SINK_FMT_I24: return 3;
    case SINK_FMT_I16: return 2;
    }
    return 4;
}

static inline int32_t sink_to_i32(float v) {
    if (v >=  1.0f) return  2147483647;
    if (v <= -1.0f) return -2147483647 - 1;
    if (v != v)     return 0;       /* NaN: both clamps read false and float->int is UB */
    return (int32_t)(v * 2147483647.0f);
}

/* Symmetric on purpose, and deliberately NOT sink_to_i32's -2147483648: the ASIO sink's i16 path
 * clamped the FLOAT to [-1, 1] and then scaled by 32767, so full negative scale has always been
 * -32767 here. Carried over verbatim rather than "fixed" — the one LSB is inaudible and a silent
 * change to what reaches a driver is not worth it. */
static inline int16_t sink_to_i16(float v) {
    if (v >=  1.0f) return  32767;
    if (v <= -1.0f) return -32767;
    if (v != v)     return 0;       /* same NaN rule, one width down */
    return (int16_t)(v * 32767.0f);
}

/* Write one 24-bit sample as three little-endian bytes. */
static inline void sink_put_i24(uint8_t* d, float v) {
    const int32_t x = sink_to_i32(v) >> 8;      /* top 24 bits of the 32-bit conversion */
    d[0] = (uint8_t)( x        & 0xFF);
    d[1] = (uint8_t)((x >>  8) & 0xFF);
    d[2] = (uint8_t)((x >> 16) & 0xFF);
}

/* ---- planar: one channel's `n` samples, contiguous in both src and dst (the ASIO shape) ---- */
static inline void sink_convert_planar(void* dst, const float* src, uint32_t n, sink_fmt fmt) {
    switch (fmt) {
    case SINK_FMT_F32:
        memcpy(dst, src, (size_t)n * sizeof(float));
        break;
    case SINK_FMT_I32: {
        int32_t* d = (int32_t*)dst;
        for (uint32_t i = 0; i < n; ++i) d[i] = sink_to_i32(src[i]);
        break;
    }
    case SINK_FMT_I24: {
        uint8_t* d = (uint8_t*)dst;
        for (uint32_t i = 0; i < n; ++i) sink_put_i24(d + (size_t)i * 3, src[i]);
        break;
    }
    case SINK_FMT_I16: {
        int16_t* d = (int16_t*)dst;
        for (uint32_t i = 0; i < n; ++i) d[i] = sink_to_i16(src[i]);
        break;
    }
    }
}

/* ---- interleaved: the whole block, planar bus in, frame-major device buffer out ----
 *
 * `src` is channel-major with `src_stride` FRAMES between channels (the plain bus passes
 * nframes; the fixed-quantum adapter passes its FIFO capacity, so a backend interleaves
 * straight out of the FIFO with no intermediate copy). `dst` holds nframes * channels
 * samples of `fmt`, frame-major: dst[f * channels + c].
 *
 * The channel loop is the inner one so `dst` is written strictly forward. */
static inline void sink_convert_interleaved(void* dst, const float* src, uint32_t channels,
                                            uint32_t nframes, uint32_t src_stride, sink_fmt fmt) {
    switch (fmt) {
    case SINK_FMT_F32: {
        float* d = (float*)dst;
        for (uint32_t f = 0; f < nframes; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                *d++ = src[(size_t)c * src_stride + f];
        break;
    }
    case SINK_FMT_I32: {
        int32_t* d = (int32_t*)dst;
        for (uint32_t f = 0; f < nframes; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                *d++ = sink_to_i32(src[(size_t)c * src_stride + f]);
        break;
    }
    case SINK_FMT_I24: {
        uint8_t* d = (uint8_t*)dst;
        for (uint32_t f = 0; f < nframes; ++f)
            for (uint32_t c = 0; c < channels; ++c, d += 3)
                sink_put_i24(d, src[(size_t)c * src_stride + f]);
        break;
    }
    case SINK_FMT_I16: {
        int16_t* d = (int16_t*)dst;
        for (uint32_t f = 0; f < nframes; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                *d++ = sink_to_i16(src[(size_t)c * src_stride + f]);
        break;
    }
    }
}

#endif /* BWA_SINK_CONVERT_H */
