/*
 * asio_convert.h — ASIO sample-format converters shared by the examples/ capture shells
 * (zylia_capture, calib_capture). One copy of the Int32/Int24/Int16/Float32 switches, so a format
 * fix lands everywhere at once. LSB (little-endian) types only — what Windows drivers actually
 * use; unknown types convert to silence rather than noise.
 *
 * Include AFTER the ASIO SDK headers (asiosys.h / asio.h): the ASIOST* constants come from there.
 */
#ifndef BWA_ASIO_CONVERT_H
#define BWA_ASIO_CONVERT_H

#include <stdint.h>
#include <string.h>

static inline int32_t asio_f_to_i32(float v) {
    return v >= 1.f ? 2147483647 : v <= -1.f ? -2147483647 - 1 : (int32_t)(v * 2147483647.f);
}

/* driver buffer -> float, one channel's block */
static inline void asio_in_to_float(float* dst, const void* src, long n, long type) {
    switch (type) {
    case ASIOSTFloat32LSB: memcpy(dst, src, (size_t)n * sizeof(float)); break;
    case ASIOSTInt32LSB: { const int32_t* s = (const int32_t*)src; for (long i = 0; i < n; ++i) dst[i] = s[i] / 2147483648.f; break; }
    case ASIOSTInt24LSB: { const uint8_t* s = (const uint8_t*)src; for (long i = 0; i < n; ++i) {
                           int32_t v = (s[i*3]) | (s[i*3+1] << 8) | (s[i*3+2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; dst[i] = v / 8388608.f; } break; }
    case ASIOSTInt16LSB: { const int16_t* s = (const int16_t*)src; for (long i = 0; i < n; ++i) dst[i] = s[i] / 32768.f; break; }
    default: memset(dst, 0, (size_t)n * sizeof(float)); break;
    }
}

/* float -> driver buffer, one channel's block */
static inline void asio_float_to_out(void* dst, const float* src, long n, long type) {
    switch (type) {
    case ASIOSTFloat32LSB: memcpy(dst, src, (size_t)n * sizeof(float)); break;
    case ASIOSTInt32LSB: { int32_t* d = (int32_t*)dst; for (long i = 0; i < n; ++i) d[i] = asio_f_to_i32(src[i]); break; }
    case ASIOSTInt24LSB: { uint8_t* d = (uint8_t*)dst; for (long i = 0; i < n; ++i) { int32_t v = asio_f_to_i32(src[i]) >> 8;
                           d[i*3] = v & 0xFF; d[i*3+1] = (v >> 8) & 0xFF; d[i*3+2] = (v >> 16) & 0xFF; } break; }
    case ASIOSTInt16LSB: { int16_t* d = (int16_t*)dst; for (long i = 0; i < n; ++i) { float v = src[i];
                           v = v > 1.f ? 1.f : (v < -1.f ? -1.f : v); d[i] = (int16_t)(v * 32767.f); } break; }
    default: memset(dst, 0, (size_t)n * 4); break;
    }
}

#endif /* BWA_ASIO_CONVERT_H */
