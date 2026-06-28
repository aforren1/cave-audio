/*
 * steam_decode.c — production binaural monitor stage 2: 26-ch bus → 3rd-order ambisonics →
 * Steam Audio HRTF → stereo. See steam_decode.h.
 *
 * Build-only-with-SDK: compiled ONLY when the prebuilt phonon SDK is vendored
 * (third_party/steamaudio/, BWAUDIO_WITH_STEAMAUDIO in CMake). It links phonon and has NOT been
 * compiled in this environment — like asio_sink.cpp at M1, treat it as pending on-SDK
 * verification. The three CONVENTION assumptions below (SH normalization, room↔ambisonic axes,
 * head-orientation frame) are the load-bearing unknowns; they are isolated in small helpers so a
 * by-ear check in the playground (does a front source image in front? does turning the head
 * rotate the field the right way?) localizes any fix.
 */
#include "steam_decode.h"
#include "ambisonics.h"
#include "sink.h"           /* BW_CHANNELS */

#include <phonon.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct SteamMonitor {
    IPLContext                 context;
    IPLHRTF                    hrtf;
    IPLAmbisonicsDecodeEffect  decode;
    uint32_t                   channels;      /* 26 */
    uint32_t                   max_block;
    float                      encode[BW_CHANNELS][BW_AMBI_CH];  /* fixed 26→16 SH matrix */
    float*                     ambi;          /* 16 * max_block, planar scratch */
};

/* CONVENTION 1 — axes. Map a room-space direction (x=right, y=up, z=back) to the ambisonic
 * convention ambi_encode_sn3d expects (AmbiX: x=front, y=left, z=up). front=-z_room, left=-x_room,
 * up=+y_room. VERIFY against the linked phonon build; if the image is mirrored/rotated, this and
 * head_basis() (CONVENTION 3) are where to correct it — they must be corrected together. */
static void room_to_ambi_dir(const float r[3], float a[3]) {
    a[0] = -r[2];   /* front */
    a[1] = -r[0];   /* left  */
    a[2] =  r[1];   /* up    */
}

/* CONVENTION 2 — normalization. ambi_encode_sn3d is SN3D (AmbiX). If the linked phonon build's
 * ambisonics are N3D, scale ACN n of degree l by sqrt(2l+1) here. Left as SN3D pending verify. */
static void sh_encode(const float room_dir[3], float y[BW_AMBI_CH]) {
    float a[3];
    room_to_ambi_dir(room_dir, a);
    ambi_encode_sn3d(a, y);
}

/* rotate vector v by quaternion q (xyzw) */
static void qrot(const float q[4], const float v[3], float o[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2.f * (y * v[2] - z * v[1]);
    float ty = 2.f * (z * v[0] - x * v[2]);
    float tz = 2.f * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* CONVENTION 3 — head orientation frame. Build Steam Audio's listener basis from the room head
 * quaternion. Assumes the engine's room frame matches phonon's (x=right, y=up, -z=ahead). VERIFY
 * with CONVENTION 1; if head-turn rotates the wrong way, flip the mapping here. */
static void head_basis(const float q[4], IPLCoordinateSpace3* cs) {
    const float fwd[3] = { 0, 0, -1 }, up[3] = { 0, 1, 0 }, rgt[3] = { 1, 0, 0 };
    float a[3], u[3], r[3];
    qrot(q, fwd, a); qrot(q, up, u); qrot(q, rgt, r);
    cs->ahead = (IPLVector3){ a[0], a[1], a[2] };
    cs->up    = (IPLVector3){ u[0], u[1], u[2] };
    cs->right = (IPLVector3){ r[0], r[1], r[2] };
    cs->origin = (IPLVector3){ 0, 0, 0 };
}

SteamMonitor* steam_monitor_create(const Layout* L, uint32_t sample_rate, uint32_t max_block,
                                   const char* hrtf_path) {
    if (!L) return NULL;
    SteamMonitor* m = (SteamMonitor*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->channels = BW_CHANNELS;
    m->max_block = max_block;

    IPLContextSettings cs = { 0 };
    cs.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&cs, &m->context) != IPL_STATUS_SUCCESS) { free(m); return NULL; }

    IPLAudioSettings as = { 0 };
    as.samplingRate = (IPLint32)sample_rate;
    as.frameSize    = (IPLint32)max_block;

    IPLHRTFSettings hs = { 0 };
    hs.type   = hrtf_path ? IPL_HRTFTYPE_SOFA : IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    if (hrtf_path) hs.sofaFileName = hrtf_path;
    if (iplHRTFCreate(m->context, &as, &hs, &m->hrtf) != IPL_STATUS_SUCCESS) {
        iplContextRelease(&m->context); free(m); return NULL;
    }

    IPLAmbisonicsDecodeEffectSettings ds = { 0 };
    ds.maxOrder = BW_AMBI_ORDER;
    ds.hrtf = m->hrtf;
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;   /* binaural output is 2-ch */
    if (iplAmbisonicsDecodeEffectCreate(m->context, &as, &ds, &m->decode) != IPL_STATUS_SUCCESS) {
        iplHRTFRelease(&m->hrtf); iplContextRelease(&m->context); free(m); return NULL;
    }

    /* fixed 26→16 encode matrix: each speaker is a virtual source at its room direction */
    for (uint32_t k = 0; k < m->channels; ++k) {
        const float* pos = L->speakers[k].pos;
        float len = sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
        float dir[3] = { 0, 0, -1 };
        if (len > 1e-6f) { dir[0] = pos[0] / len; dir[1] = pos[1] / len; dir[2] = pos[2] / len; }
        sh_encode(dir, m->encode[k]);
    }

    m->ambi = (float*)calloc((size_t)BW_AMBI_CH * max_block, sizeof(float));
    if (!m->ambi) { steam_monitor_destroy(m); return NULL; }
    return m;
}

void steam_monitor_process(SteamMonitor* m, const float* bus26, const float p[3], const float q[4],
                           float* out, uint32_t n) {
    (void)p;
    if (!m || n == 0) return;
    if (n > m->max_block) n = m->max_block;

    /* encode: ambi[k][i] = sum over speakers of encode[spk][k] * bus[spk][i] (planar, stride n) */
    memset(m->ambi, 0, sizeof(float) * (size_t)BW_AMBI_CH * n);
    for (uint32_t s = 0; s < m->channels; ++s) {
        const float* src = bus26 + (size_t)s * n;
        const float* g = m->encode[s];
        for (uint32_t k = 0; k < BW_AMBI_CH; ++k) {
            if (g[k] == 0.0f) continue;
            float* a = m->ambi + (size_t)k * n;
            float gk = g[k];
            for (uint32_t i = 0; i < n; ++i) a[i] += gk * src[i];
        }
    }

    IPLfloat32* ambiPtrs[BW_AMBI_CH];
    for (uint32_t k = 0; k < BW_AMBI_CH; ++k) ambiPtrs[k] = m->ambi + (size_t)k * n;
    IPLfloat32* outPtrs[2] = { out, out + n };

    IPLAudioBuffer inBuf  = { (IPLint32)BW_AMBI_CH, (IPLint32)n, ambiPtrs };
    IPLAudioBuffer outBuf = { 2, (IPLint32)n, outPtrs };

    IPLAmbisonicsDecodeEffectParams params = { 0 };
    params.order = BW_AMBI_ORDER;
    params.hrtf = m->hrtf;
    params.binaural = IPL_TRUE;
    head_basis(q, &params.orientation);

    iplAmbisonicsDecodeEffectApply(m->decode, &params, &inBuf, &outBuf);
}

void steam_monitor_destroy(SteamMonitor* m) {
    if (!m) return;
    if (m->decode)  iplAmbisonicsDecodeEffectRelease(&m->decode);
    if (m->hrtf)    iplHRTFRelease(&m->hrtf);
    if (m->context) iplContextRelease(&m->context);
    free(m->ambi);
    free(m);
}
