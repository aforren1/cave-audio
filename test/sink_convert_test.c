/*
 * sink_convert_test.c — the shared bus-to-device conversion (src/sink/sink_convert.h).
 *
 * This code runs on the audio thread of every backend, and CI has no device to catch it on, so
 * the whole thing is pinned here: each format in both layouts, the clamps at and past full
 * scale, the NaN rule, and a 26-channel interleave round trip at the FIFO's stride (the shape
 * sink_quant hands a backend, where the channel stride is NOT nframes).
 */
#include "sink/sink_convert.h"
#include "sink/sink.h"          /* BWA_CHANNELS: the round trip runs at the real bus width */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

/* Read one packed 24-bit little-endian sample back as a signed value. */
static int32_t get_i24(const uint8_t* d) {
    const int32_t raw = (int32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16));
    return (raw & 0x800000) ? (raw - 0x1000000) : raw;   /* sign-extend from 24 bits */
}

/* The values that decide the rules: past full scale both ways, exactly full scale, the middle,
 * and the three non-finite cases. NaN is the load-bearing one — it slips BOTH clamp compares,
 * and float-to-int out of range is undefined behavior, which on a real driver is a full-scale
 * pop rather than the silence the limiter-off path needs. */
static const float SRC[] = { -2.0f, -1.0f, -0.5f, 0.0f, 0.25f, 1.0f, 2.0f, 0.0f, 0.0f, 0.0f };
enum { NSRC = (int)(sizeof SRC / sizeof *SRC) };
enum { IDX_NAN = 7, IDX_PINF = 8, IDX_NINF = 9 };

static void fill_src(float* v) {
    memcpy(v, SRC, sizeof SRC);
    v[IDX_NAN]  = (float)NAN;
    v[IDX_PINF] = (float)INFINITY;
    v[IDX_NINF] = -(float)INFINITY;
}

static void test_planar_rules(void) {
    float v[NSRC];
    fill_src(v);

    /* float32 is a straight memcpy — no clamp, no NaN scrub. That is deliberate: the format has
     * the range, and a device taking float32 is not at risk of the UB the integer paths are. */
    float f32[NSRC];
    sink_convert_planar(f32, v, NSRC, SINK_FMT_F32);
    for (int i = 0; i < NSRC; ++i) {
        if (i == IDX_NAN) { CHECK(f32[i] != f32[i], "f32 passes NaN through unchanged"); continue; }
        CHECK(memcmp(&f32[i], &v[i], sizeof(float)) == 0, "f32 planar bit-exact at %d", i);
    }

    int32_t i32[NSRC];
    sink_convert_planar(i32, v, NSRC, SINK_FMT_I32);
    CHECK(i32[0] == -2147483647 - 1, "i32 below -1 clamps to min, got %d", i32[0]);
    CHECK(i32[1] == -2147483647 - 1, "i32 at -1 clamps to min, got %d", i32[1]);
    CHECK(i32[3] == 0,               "i32 zero, got %d", i32[3]);
    CHECK(i32[5] ==  2147483647,     "i32 at +1 clamps to max, got %d", i32[5]);
    CHECK(i32[6] ==  2147483647,     "i32 above +1 clamps to max, got %d", i32[6]);
    CHECK(i32[IDX_NAN]  == 0,             "i32 NaN converts to 0, got %d", i32[IDX_NAN]);
    CHECK(i32[IDX_PINF] ==  2147483647,   "i32 +Inf clamps to max, got %d", i32[IDX_PINF]);
    CHECK(i32[IDX_NINF] == -2147483647 - 1, "i32 -Inf clamps to min, got %d", i32[IDX_NINF]);
    CHECK(i32[2] < 0 && i32[4] > 0, "i32 keeps polarity in range");

    uint8_t i24[NSRC * 3];
    sink_convert_planar(i24, v, NSRC, SINK_FMT_I24);
    CHECK(get_i24(i24 + 0 * 3) == -8388608, "i24 below -1 clamps to min, got %d", get_i24(i24));
    CHECK(get_i24(i24 + 5 * 3) ==  8388607, "i24 at +1 clamps to max, got %d", get_i24(i24 + 15));
    CHECK(get_i24(i24 + IDX_NAN * 3) == 0,  "i24 NaN converts to 0");
    CHECK(get_i24(i24 + 3 * 3) == 0,        "i24 zero");
    /* Every 24-bit sample must be the top 24 bits of the 32-bit one: one conversion, one rule. */
    for (int i = 0; i < NSRC; ++i)
        CHECK(get_i24(i24 + (size_t)i * 3) == (i32[i] >> 8), "i24 tracks i32 >> 8 at %d", i);

    int16_t i16[NSRC];
    sink_convert_planar(i16, v, NSRC, SINK_FMT_I16);
    /* -32767, not -32768: the ASIO sink clamped the FLOAT and then scaled, and that behavior is
     * carried over verbatim rather than silently changed under a live driver. */
    CHECK(i16[0] == -32767, "i16 below -1 clamps to -32767, got %d", i16[0]);
    CHECK(i16[1] == -32767, "i16 at -1 clamps to -32767, got %d", i16[1]);
    CHECK(i16[5] ==  32767, "i16 at +1 clamps to +32767, got %d", i16[5]);
    CHECK(i16[6] ==  32767, "i16 above +1 clamps to +32767, got %d", i16[6]);
    CHECK(i16[IDX_NAN]  == 0,      "i16 NaN converts to 0, got %d", i16[IDX_NAN]);
    CHECK(i16[IDX_PINF] ==  32767, "i16 +Inf clamps to max");
    CHECK(i16[IDX_NINF] == -32767, "i16 -Inf clamps to min");
    CHECK(i16[4] == (int16_t)(0.25f * 32767.0f), "i16 scales in range, got %d", i16[4]);

    CHECK(sink_fmt_bytes(SINK_FMT_F32) == 4 && sink_fmt_bytes(SINK_FMT_I32) == 4 &&
          sink_fmt_bytes(SINK_FMT_I24) == 3 && sink_fmt_bytes(SINK_FMT_I16) == 2,
          "sample widths");
}

/* The interleaved variants must agree with the planar ones sample for sample. If they ever
 * diverge, one backend family gets a different clamp from the other and only hardware finds it. */
static void test_interleaved_matches_planar(void) {
    enum { CH = 3, N = NSRC };
    float bus[CH * N];
    float v[NSRC];
    fill_src(v);
    for (int c = 0; c < CH; ++c)
        for (int i = 0; i < N; ++i)
            bus[c * N + i] = (c == 1) ? -v[i] : v[i];   /* channel 1 inverted, so a channel mixup shows */

    /* i32 */
    int32_t ilv[CH * N], pl[CH][N];
    sink_convert_interleaved(ilv, bus, CH, N, N, SINK_FMT_I32);
    for (int c = 0; c < CH; ++c) sink_convert_planar(pl[c], bus + c * N, N, SINK_FMT_I32);
    for (int f = 0; f < N; ++f)
        for (int c = 0; c < CH; ++c)
            CHECK(ilv[f * CH + c] == pl[c][f], "i32 interleave ch%d frame%d: %d vs %d",
                  c, f, ilv[f * CH + c], pl[c][f]);

    /* i16 */
    int16_t ilv16[CH * N], pl16[CH][N];
    sink_convert_interleaved(ilv16, bus, CH, N, N, SINK_FMT_I16);
    for (int c = 0; c < CH; ++c) sink_convert_planar(pl16[c], bus + c * N, N, SINK_FMT_I16);
    for (int f = 0; f < N; ++f)
        for (int c = 0; c < CH; ++c)
            CHECK(ilv16[f * CH + c] == pl16[c][f], "i16 interleave ch%d frame%d", c, f);

    /* i24, packed: the byte layout is the part that goes wrong silently */
    uint8_t ilv24[CH * N * 3], pl24[CH][N * 3];
    sink_convert_interleaved(ilv24, bus, CH, N, N, SINK_FMT_I24);
    for (int c = 0; c < CH; ++c) sink_convert_planar(pl24[c], bus + c * N, N, SINK_FMT_I24);
    for (int f = 0; f < N; ++f)
        for (int c = 0; c < CH; ++c)
            CHECK(memcmp(ilv24 + ((size_t)f * CH + c) * 3, pl24[c] + (size_t)f * 3, 3) == 0,
                  "i24 interleave ch%d frame%d", c, f);

    /* f32 */
    float ilvf[CH * N];
    sink_convert_interleaved(ilvf, bus, CH, N, N, SINK_FMT_F32);
    for (int f = 0; f < N; ++f)
        for (int c = 0; c < CH; ++c) {
            const float a = ilvf[f * CH + c], b = bus[c * N + f];
            if (b != b) { CHECK(a != a, "f32 interleave keeps NaN ch%d frame%d", c, f); continue; }
            CHECK(a == b, "f32 interleave ch%d frame%d", c, f);
        }
}

/* The real shape: a full-width bus, interleaved out and de-interleaved back, at a channel stride
 * LARGER than the frame count. That stride is not decorative — sink_quant hands a backend a FIFO
 * slot whose stride is the block size while the device asks for some other count, and reading it
 * as if the stride were nframes would silently shear the channels. */
static void test_round_trip_26ch_strided(void) {
    enum { CH = BWA_CHANNELS, STRIDE = 256, N = 100 };
    static float bus[CH * STRIDE];
    static float out[CH * STRIDE];
    static float ilv[CH * N];

    for (int c = 0; c < CH; ++c)
        for (int i = 0; i < STRIDE; ++i)
            bus[c * STRIDE + i] = (float)(c * 1000 + i) / 32768.0f;   /* unique, in range */

    sink_convert_interleaved(ilv, bus, CH, N, STRIDE, SINK_FMT_F32);
    memset(out, 0, sizeof out);
    for (int f = 0; f < N; ++f)
        for (int c = 0; c < CH; ++c)
            out[c * STRIDE + f] = ilv[f * CH + c];

    for (int c = 0; c < CH; ++c)
        for (int i = 0; i < N; ++i)
            CHECK(out[c * STRIDE + i] == bus[c * STRIDE + i],
                  "26-ch round trip ch%d frame%d: %f vs %f", c, i,
                  (double)out[c * STRIDE + i], (double)bus[c * STRIDE + i]);

    /* Frames past the request must be untouched — a backend converts only what the device asked
     * for, and writing past it would run off the end of the device's buffer. */
    for (int c = 0; c < CH; ++c)
        CHECK(out[c * STRIDE + N] == 0.0f, "nothing written past the request on ch%d", c);
}

/* Zero frames is a legal request (a device can hand back an empty period). It must write nothing
 * rather than fall off the end of a buffer sized for it. */
static void test_zero_frames(void) {
    float bus[8] = {0}, dst[4];
    for (int i = 0; i < 4; ++i) dst[i] = 12345.0f;
    sink_convert_interleaved(dst, bus, 2, 0, 4, SINK_FMT_F32);
    sink_convert_planar(dst, bus, 0, SINK_FMT_I32);
    for (int i = 0; i < 4; ++i) CHECK(dst[i] == 12345.0f, "zero frames writes nothing at %d", i);
}

int main(void) {
    test_planar_rules();
    test_interleaved_matches_planar();
    test_round_trip_26ch_strided();
    test_zero_frames();

    if (fails) { fprintf(stderr, "sink_convert: %d failure(s)\n", fails); return 1; }
    printf("sink_convert OK\n");
    return 0;
}
