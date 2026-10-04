/*
 * vec.h — the small vectorized kernels the audio thread's channel-major stages share.
 *
 * Every kernel here works on ONE contiguous row (a bus channel, a scratch signal), because that is
 * the access the planar bus makes cheap: the stages that used to walk the bus frame-major (one
 * sample across every channel, a row stride per load) are the ones that were slow.
 *
 * Header-only, alloc-free, audio-thread safe. SSE on x86, NEON on ARM, plain C elsewhere (wasm:
 * clang vectorizes the fallbacks). Each vector path evaluates the same expression as its scalar
 * tail, so a row's result does not depend on where the vector loop stopped.
 */
#ifndef BWA_DSP_VEC_H
#define BWA_DSP_VEC_H

#include <math.h>
#include <stdint.h>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__SSE__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
  #include <xmmintrin.h>
  #define BWA_VEC_SSE 1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #include <arm_neon.h>
  #define BWA_VEC_NEON 1      /* AArch64 only: the abs-max kernels need vmaxnmq (NaN-ignoring max) */
#endif

/* out[j] += (g0 + st * (k0 + j)) * x[j],   j in [0, n)
 *
 * The gain is the CLOSED FORM of a per-sample linear ramp, not a running `g += st`. A running sum
 * is a loop-carried dependency (one add latency per sample, no vectorization), and it is the less
 * accurate of the two: its error grows with the sample index while the closed form rounds once.
 * k0 is the ramp's own sample offset, so a block mixed in chunks lands on the same gains it would
 * mixed whole. Exact for k0 + n < 2^24. */
static inline void ramp_mac(float* out, const float* x, uint32_t n, uint32_t k0, float g0, float st) {
    if (g0 == 0.f && st == 0.f) return;    /* a silent, settled row (most of a VBAP vector): nothing to add */
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 vg0 = _mm_set1_ps(g0), vst = _mm_set1_ps(st), four = _mm_set1_ps(4.f);
    __m128 vk = _mm_setr_ps((float)k0, (float)(k0 + 1), (float)(k0 + 2), (float)(k0 + 3));
    for (; j + 4 <= n; j += 4) {
        const __m128 g = _mm_add_ps(vg0, _mm_mul_ps(vst, vk));
        _mm_storeu_ps(out + j, _mm_add_ps(_mm_loadu_ps(out + j), _mm_mul_ps(g, _mm_loadu_ps(x + j))));
        vk = _mm_add_ps(vk, four);
    }
#elif defined(BWA_VEC_NEON)
    const float32x4_t vg0 = vdupq_n_f32(g0), vst = vdupq_n_f32(st), four = vdupq_n_f32(4.f);
    const float k4[4] = { (float)k0, (float)(k0 + 1), (float)(k0 + 2), (float)(k0 + 3) };
    float32x4_t vk = vld1q_f32(k4);
    for (; j + 4 <= n; j += 4) {
        const float32x4_t g = vaddq_f32(vg0, vmulq_f32(vst, vk));
        vst1q_f32(out + j, vaddq_f32(vld1q_f32(out + j), vmulq_f32(g, vld1q_f32(x + j))));
        vk = vaddq_f32(vk, four);
    }
#endif
    for (; j < n; ++j) out[j] += (g0 + st * (float)(k0 + j)) * x[j];
}

/* out[j] += sum over r < nr of (g0[r] + st[r] * (k0 + j)) * rows[r * stride + j],   j in [0, n)
 *
 * Several ramp_mac rows into ONE output, the sum held in a register so `out` is loaded and stored
 * once rather than once per row. Same closed-form ramps as ramp_mac. */
static inline void ramp_mat_mac(float* out, const float* rows, uint32_t stride, uint32_t nr,
                                const float* g0, const float* st, uint32_t k0, uint32_t n) {
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 four = _mm_set1_ps(4.f);
    __m128 vk = _mm_setr_ps((float)k0, (float)(k0 + 1), (float)(k0 + 2), (float)(k0 + 3));
    for (; j + 4 <= n; j += 4) {
        __m128 acc = _mm_setzero_ps();
        for (uint32_t r = 0; r < nr; ++r) {
            const __m128 g = _mm_add_ps(_mm_set1_ps(g0[r]), _mm_mul_ps(_mm_set1_ps(st[r]), vk));
            acc = _mm_add_ps(acc, _mm_mul_ps(g, _mm_loadu_ps(rows + (size_t)r * stride + j)));
        }
        _mm_storeu_ps(out + j, _mm_add_ps(_mm_loadu_ps(out + j), acc));
        vk = _mm_add_ps(vk, four);
    }
#elif defined(BWA_VEC_NEON)
    const float32x4_t four = vdupq_n_f32(4.f);
    const float k4[4] = { (float)k0, (float)(k0 + 1), (float)(k0 + 2), (float)(k0 + 3) };
    float32x4_t vk = vld1q_f32(k4);
    for (; j + 4 <= n; j += 4) {
        float32x4_t acc = vdupq_n_f32(0.f);
        for (uint32_t r = 0; r < nr; ++r) {
            const float32x4_t g = vaddq_f32(vdupq_n_f32(g0[r]), vmulq_f32(vdupq_n_f32(st[r]), vk));
            acc = vaddq_f32(acc, vmulq_f32(g, vld1q_f32(rows + (size_t)r * stride + j)));
        }
        vst1q_f32(out + j, vaddq_f32(vld1q_f32(out + j), acc));
        vk = vaddq_f32(vk, four);
    }
#endif
    for (; j < n; ++j) {
        float acc = 0.f;
        for (uint32_t r = 0; r < nr; ++r) acc += (g0[r] + st[r] * (float)(k0 + j)) * rows[(size_t)r * stride + j];
        out[j] += acc;
    }
}

/* out[j] += sum over r < nr of coef[r] * rows[r * stride + j],   j in [0, n)
 *
 * One row of a small matrix product (a decode matrix row against nr signal rows). The sum stays in
 * a register across r, so `out` is loaded and stored once per vector rather than once per row. */
static inline void mat_mac(float* out, const float* rows, uint32_t stride, uint32_t nr, const float* coef, uint32_t n) {
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    for (; j + 4 <= n; j += 4) {
        __m128 acc = _mm_setzero_ps();
        for (uint32_t r = 0; r < nr; ++r)
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(coef[r]), _mm_loadu_ps(rows + (size_t)r * stride + j)));
        _mm_storeu_ps(out + j, _mm_add_ps(_mm_loadu_ps(out + j), acc));
    }
#elif defined(BWA_VEC_NEON)
    for (; j + 4 <= n; j += 4) {
        float32x4_t acc = vdupq_n_f32(0.f);
        for (uint32_t r = 0; r < nr; ++r)
            acc = vaddq_f32(acc, vmulq_f32(vdupq_n_f32(coef[r]), vld1q_f32(rows + (size_t)r * stride + j)));
        vst1q_f32(out + j, vaddq_f32(vld1q_f32(out + j), acc));
    }
#endif
    for (; j < n; ++j) {
        float acc = 0.f;
        for (uint32_t r = 0; r < nr; ++r) acc += coef[r] * rows[(size_t)r * stride + j];
        out[j] += acc;
    }
}

/* out[j] = sum over v < T of kr[v] * h[j + v],   j in [0, n)
 *
 * A direct-form FIR over a LINEAR history: h holds the T-1 inputs before out[0]'s, then the n new
 * ones, and kr is the kernel stored reversed (kr[v] = k[T-1-v]). Eight accumulators (32 outputs)
 * per pass, because each accumulator is a chain of T dependent adds: with fewer, a long kernel is
 * add-LATENCY bound rather than throughput bound. Writes out (does not accumulate). */
static inline void vec_fir(float* out, const float* h, const float* kr, uint32_t T, uint32_t n) {
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    for (; j + 32 <= n; j += 32) {
        __m128 a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
        const float* p = h + j;
        for (uint32_t v = 0; v < T; ++v, ++p) {
            const __m128 b = _mm_set1_ps(kr[v]);
            a0 = _mm_add_ps(a0, _mm_mul_ps(b, _mm_loadu_ps(p)));
            a1 = _mm_add_ps(a1, _mm_mul_ps(b, _mm_loadu_ps(p + 4)));
            a2 = _mm_add_ps(a2, _mm_mul_ps(b, _mm_loadu_ps(p + 8)));
            a3 = _mm_add_ps(a3, _mm_mul_ps(b, _mm_loadu_ps(p + 12)));
            a4 = _mm_add_ps(a4, _mm_mul_ps(b, _mm_loadu_ps(p + 16)));
            a5 = _mm_add_ps(a5, _mm_mul_ps(b, _mm_loadu_ps(p + 20)));
            a6 = _mm_add_ps(a6, _mm_mul_ps(b, _mm_loadu_ps(p + 24)));
            a7 = _mm_add_ps(a7, _mm_mul_ps(b, _mm_loadu_ps(p + 28)));
        }
        _mm_storeu_ps(out + j,      a0); _mm_storeu_ps(out + j + 4,  a1);
        _mm_storeu_ps(out + j + 8,  a2); _mm_storeu_ps(out + j + 12, a3);
        _mm_storeu_ps(out + j + 16, a4); _mm_storeu_ps(out + j + 20, a5);
        _mm_storeu_ps(out + j + 24, a6); _mm_storeu_ps(out + j + 28, a7);
    }
    for (; j + 4 <= n; j += 4) {
        __m128 a = _mm_setzero_ps();
        for (uint32_t v = 0; v < T; ++v) a = _mm_add_ps(a, _mm_mul_ps(_mm_set1_ps(kr[v]), _mm_loadu_ps(h + j + v)));
        _mm_storeu_ps(out + j, a);
    }
#elif defined(BWA_VEC_NEON)
    for (; j + 32 <= n; j += 32) {
        float32x4_t a[8];
        for (int q = 0; q < 8; ++q) a[q] = vdupq_n_f32(0.f);
        const float* p = h + j;
        for (uint32_t v = 0; v < T; ++v, ++p) {
            const float32x4_t b = vdupq_n_f32(kr[v]);
            for (int q = 0; q < 8; ++q) a[q] = vaddq_f32(a[q], vmulq_f32(b, vld1q_f32(p + 4 * q)));
        }
        for (int q = 0; q < 8; ++q) vst1q_f32(out + j + 4 * q, a[q]);
    }
    for (; j + 4 <= n; j += 4) {
        float32x4_t a = vdupq_n_f32(0.f);
        for (uint32_t v = 0; v < T; ++v) a = vaddq_f32(a, vmulq_f32(vdupq_n_f32(kr[v]), vld1q_f32(h + j + v)));
        vst1q_f32(out + j, a);
    }
#endif
    for (; j < n; ++j) {
        float acc = 0.f;
        for (uint32_t v = 0; v < T; ++v) acc += kr[v] * h[j + v];
        out[j] = acc;
    }
}

/* sum over j < n of a[j] * b[j], in four partial sums (SSE/NEON) folded at the end. */
static inline float vec_dot(const float* a, const float* b, uint32_t n) {
    uint32_t j = 0;
    float s = 0.f;
#if defined(BWA_VEC_SSE)
    if (n >= 4) {
        __m128 acc = _mm_mul_ps(_mm_loadu_ps(a), _mm_loadu_ps(b));
        for (j = 4; j + 4 <= n; j += 4) acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(a + j), _mm_loadu_ps(b + j)));
        acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
        acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
        s = _mm_cvtss_f32(acc);
    }
#elif defined(BWA_VEC_NEON)
    if (n >= 4) {
        float32x4_t acc = vmulq_f32(vld1q_f32(a), vld1q_f32(b));
        for (j = 4; j + 4 <= n; j += 4) acc = vaddq_f32(acc, vmulq_f32(vld1q_f32(a + j), vld1q_f32(b + j)));
        s = vaddvq_f32(acc);
    }
#endif
    for (; j < n; ++j) s += a[j] * b[j];
    return s;
}

/* x[j] *= g0 + st * j,   j in [0, n): a gain ramp applied in place, closed form (see ramp_mac). */
static inline void vec_ramp_scale(float* x, uint32_t n, float g0, float st) {
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 vg0 = _mm_set1_ps(g0), vst = _mm_set1_ps(st), four = _mm_set1_ps(4.f);
    __m128 vk = _mm_setr_ps(0.f, 1.f, 2.f, 3.f);
    for (; j + 4 <= n; j += 4) {
        _mm_storeu_ps(x + j, _mm_mul_ps(_mm_loadu_ps(x + j), _mm_add_ps(vg0, _mm_mul_ps(vst, vk))));
        vk = _mm_add_ps(vk, four);
    }
#elif defined(BWA_VEC_NEON)
    const float32x4_t vg0 = vdupq_n_f32(g0), vst = vdupq_n_f32(st), four = vdupq_n_f32(4.f);
    const float k4[4] = { 0.f, 1.f, 2.f, 3.f };
    float32x4_t vk = vld1q_f32(k4);
    for (; j + 4 <= n; j += 4) {
        vst1q_f32(x + j, vmulq_f32(vld1q_f32(x + j), vaddq_f32(vg0, vmulq_f32(vst, vk))));
        vk = vaddq_f32(vk, four);
    }
#endif
    for (; j < n; ++j) x[j] *= g0 + st * (float)j;
}

/* One biquad section (DF-I, a0-normalized: c = b0 b1 b2 a1 a2) over EIGHT interleaved channels in
 * place: v[8*i + l] is channel l's sample i, st = x1 x2 y1 y2 per lane. A biquad on one channel is
 * latency-bound (each output waits on the last), so two vectors of four lanes run eight independent
 * chains, and the sum is associated so only `- a1*y1` sits on the recurrence: everything else
 * reads inputs and the output from two samples back. An identity lane (1 0 0 0 0, zero state)
 * passes its samples through exactly, which is how callers pad a short group. */
static inline void vec_biquad8(float* v, uint32_t n, const float c[5][8], float st[4][8]) {
#if defined(BWA_VEC_SSE)
    const __m128 b0a = _mm_loadu_ps(c[0]), b1a = _mm_loadu_ps(c[1]), b2a = _mm_loadu_ps(c[2]);
    const __m128 a1a = _mm_loadu_ps(c[3]), a2a = _mm_loadu_ps(c[4]);
    const __m128 b0b = _mm_loadu_ps(c[0] + 4), b1b = _mm_loadu_ps(c[1] + 4), b2b = _mm_loadu_ps(c[2] + 4);
    const __m128 a1b = _mm_loadu_ps(c[3] + 4), a2b = _mm_loadu_ps(c[4] + 4);
    __m128 x1a = _mm_loadu_ps(st[0]),     x2a = _mm_loadu_ps(st[1]),     y1a = _mm_loadu_ps(st[2]),     y2a = _mm_loadu_ps(st[3]);
    __m128 x1b = _mm_loadu_ps(st[0] + 4), x2b = _mm_loadu_ps(st[1] + 4), y1b = _mm_loadu_ps(st[2] + 4), y2b = _mm_loadu_ps(st[3] + 4);
    for (uint32_t i = 0; i < n; ++i) {
        const __m128 ina = _mm_loadu_ps(v + 8 * i), inb = _mm_loadu_ps(v + 8 * i + 4);
        const __m128 pa = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(b0a, ina), _mm_mul_ps(b1a, x1a)), _mm_mul_ps(b2a, x2a)),
                                     _mm_mul_ps(a2a, y2a));
        const __m128 pb = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(b0b, inb), _mm_mul_ps(b1b, x1b)), _mm_mul_ps(b2b, x2b)),
                                     _mm_mul_ps(a2b, y2b));
        const __m128 ya = _mm_sub_ps(pa, _mm_mul_ps(a1a, y1a));
        const __m128 yb = _mm_sub_ps(pb, _mm_mul_ps(a1b, y1b));
        x2a = x1a; x1a = ina; y2a = y1a; y1a = ya;
        x2b = x1b; x1b = inb; y2b = y1b; y1b = yb;
        _mm_storeu_ps(v + 8 * i, ya);
        _mm_storeu_ps(v + 8 * i + 4, yb);
    }
    _mm_storeu_ps(st[0], x1a); _mm_storeu_ps(st[1], x2a); _mm_storeu_ps(st[2], y1a); _mm_storeu_ps(st[3], y2a);
    _mm_storeu_ps(st[0] + 4, x1b); _mm_storeu_ps(st[1] + 4, x2b); _mm_storeu_ps(st[2] + 4, y1b); _mm_storeu_ps(st[3] + 4, y2b);
#elif defined(BWA_VEC_NEON)
    const float32x4_t b0a = vld1q_f32(c[0]), b1a = vld1q_f32(c[1]), b2a = vld1q_f32(c[2]);
    const float32x4_t a1a = vld1q_f32(c[3]), a2a = vld1q_f32(c[4]);
    const float32x4_t b0b = vld1q_f32(c[0] + 4), b1b = vld1q_f32(c[1] + 4), b2b = vld1q_f32(c[2] + 4);
    const float32x4_t a1b = vld1q_f32(c[3] + 4), a2b = vld1q_f32(c[4] + 4);
    float32x4_t x1a = vld1q_f32(st[0]),     x2a = vld1q_f32(st[1]),     y1a = vld1q_f32(st[2]),     y2a = vld1q_f32(st[3]);
    float32x4_t x1b = vld1q_f32(st[0] + 4), x2b = vld1q_f32(st[1] + 4), y1b = vld1q_f32(st[2] + 4), y2b = vld1q_f32(st[3] + 4);
    for (uint32_t i = 0; i < n; ++i) {
        const float32x4_t ina = vld1q_f32(v + 8 * i), inb = vld1q_f32(v + 8 * i + 4);
        const float32x4_t pa = vsubq_f32(vaddq_f32(vaddq_f32(vmulq_f32(b0a, ina), vmulq_f32(b1a, x1a)), vmulq_f32(b2a, x2a)),
                                         vmulq_f32(a2a, y2a));
        const float32x4_t pb = vsubq_f32(vaddq_f32(vaddq_f32(vmulq_f32(b0b, inb), vmulq_f32(b1b, x1b)), vmulq_f32(b2b, x2b)),
                                         vmulq_f32(a2b, y2b));
        const float32x4_t ya = vsubq_f32(pa, vmulq_f32(a1a, y1a));
        const float32x4_t yb = vsubq_f32(pb, vmulq_f32(a1b, y1b));
        x2a = x1a; x1a = ina; y2a = y1a; y1a = ya;
        x2b = x1b; x1b = inb; y2b = y1b; y1b = yb;
        vst1q_f32(v + 8 * i, ya);
        vst1q_f32(v + 8 * i + 4, yb);
    }
    vst1q_f32(st[0], x1a); vst1q_f32(st[1], x2a); vst1q_f32(st[2], y1a); vst1q_f32(st[3], y2a);
    vst1q_f32(st[0] + 4, x1b); vst1q_f32(st[1] + 4, x2b); vst1q_f32(st[2] + 4, y1b); vst1q_f32(st[3] + 4, y2b);
#else
    for (uint32_t i = 0; i < n; ++i)
        for (int l = 0; l < 8; ++l) {
            const float in = v[8 * i + l];
            const float y  = (c[0][l]*in + c[1][l]*st[0][l] + c[2][l]*st[1][l] - c[4][l]*st[3][l]) - c[3][l]*st[2][l];
            st[1][l] = st[0][l]; st[0][l] = in; st[3][l] = st[2][l]; st[2][l] = y;
            v[8 * i + l] = y;
        }
#endif
}

/* peak[j] = max(peak[j], |x[j]|). A NaN sample is ignored (the scalar `a > p` is false for it):
 * SSE's maxps returns its second operand when either is NaN, NEON's vmaxnm the number. */
static inline void vec_absmax_into(float* peak, const float* x, uint32_t n) {
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 sign = _mm_set1_ps(-0.f);
    for (; j + 4 <= n; j += 4)
        _mm_storeu_ps(peak + j, _mm_max_ps(_mm_andnot_ps(sign, _mm_loadu_ps(x + j)), _mm_loadu_ps(peak + j)));
#elif defined(BWA_VEC_NEON)
    for (; j + 4 <= n; j += 4)
        vst1q_f32(peak + j, vmaxnmq_f32(vabsq_f32(vld1q_f32(x + j)), vld1q_f32(peak + j)));
#endif
    for (; j < n; ++j) { const float a = x[j] < 0.f ? -x[j] : x[j]; if (a > peak[j]) peak[j] = a; }
}

/* max |x[j]| over the row, NaN ignored; 0 for an empty or all-NaN row. */
static inline float vec_absmax(const float* x, uint32_t n) {
    float m = 0.f;
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 sign = _mm_set1_ps(-0.f);
    __m128 vm = _mm_setzero_ps();
    for (; j + 4 <= n; j += 4) vm = _mm_max_ps(_mm_andnot_ps(sign, _mm_loadu_ps(x + j)), vm);
    float t[4]; _mm_storeu_ps(t, vm);
    for (int l = 0; l < 4; ++l) if (t[l] > m) m = t[l];
#elif defined(BWA_VEC_NEON)
    float32x4_t vm = vdupq_n_f32(0.f);
    for (; j + 4 <= n; j += 4) vm = vmaxnmq_f32(vabsq_f32(vld1q_f32(x + j)), vm);
    m = vmaxvq_f32(vm);
#endif
    for (; j < n; ++j) { const float a = x[j] < 0.f ? -x[j] : x[j]; if (a > m) m = a; }
    return m;
}

/* x[j] = clamp(x[j] * g[j], -c, c), with a non-finite product scrubbed to 0 (never to +-c: that
 * would be a full-scale DC step). Returns max |x[j]| of the result, so a caller can meter the row
 * in the same pass. */
static inline float vec_scale_clamp(float* x, const float* g, uint32_t n, float c) {
    float m = 0.f;
    uint32_t j = 0;
#if defined(BWA_VEC_SSE)
    const __m128 hi = _mm_set1_ps(c), lo = _mm_set1_ps(-c), zero = _mm_setzero_ps(), sign = _mm_set1_ps(-0.f);
    __m128 vm = zero;
    for (; j + 4 <= n; j += 4) {
        const __m128 s = _mm_mul_ps(_mm_loadu_ps(x + j), _mm_loadu_ps(g + j));
        const __m128 fin = _mm_cmpeq_ps(_mm_sub_ps(s, s), zero);      /* s - s is NaN for Inf and NaN */
        const __m128 y = _mm_and_ps(fin, _mm_min_ps(_mm_max_ps(s, lo), hi));
        _mm_storeu_ps(x + j, y);
        vm = _mm_max_ps(_mm_andnot_ps(sign, y), vm);
    }
    float t[4]; _mm_storeu_ps(t, vm);
    for (int l = 0; l < 4; ++l) if (t[l] > m) m = t[l];
#elif defined(BWA_VEC_NEON)
    const float32x4_t hi = vdupq_n_f32(c), lo = vdupq_n_f32(-c), zero = vdupq_n_f32(0.f);
    float32x4_t vm = zero;
    for (; j + 4 <= n; j += 4) {
        const float32x4_t s = vmulq_f32(vld1q_f32(x + j), vld1q_f32(g + j));
        const uint32x4_t fin = vceqq_f32(vsubq_f32(s, s), zero);
        const float32x4_t y = vreinterpretq_f32_u32(vandq_u32(fin, vreinterpretq_u32_f32(vminq_f32(vmaxq_f32(s, lo), hi))));
        vst1q_f32(x + j, y);
        vm = vmaxq_f32(vabsq_f32(y), vm);
    }
    m = vmaxvq_f32(vm);
#endif
    for (; j < n; ++j) {
        float s = x[j] * g[j];
        if (!isfinite(s)) s = 0.f;                    /* NaN or Inf: the clamp can't see either */
        else if (s >  c) s =  c;
        else if (s < -c) s = -c;
        x[j] = s;
        const float a = s < 0.f ? -s : s;
        if (a > m) m = a;
    }
    return m;
}

#endif /* BWA_DSP_VEC_H */
