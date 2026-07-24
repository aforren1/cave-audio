/*
 * steam_decode.c — production binaural monitor stage 2: 26-ch bus → 3rd-order ambisonics →
 * Steam Audio HRTF → stereo. See steam_decode.h.
 *
 * Build-only-with-SDK: compiled ONLY when the prebuilt phonon SDK is vendored
 * (third_party/steamaudio/, BWA_WITH_STEAMAUDIO in CMake). It links phonon and has NOT been
 * compiled in this environment — like asio_sink.cpp at M1, treat it as pending on-SDK verification.
 *
 * Convention status — all three RESOLVED against the phonon source (third_party/steam-audio @480dd64):
 *  - CONVENTION 1 (axes): room_to_ambi_dir matches phonon's net AmbiX assignment (sh.cpp
 *    convertedDirection: front=-z, left=-x, up=+y). ACN ordering matches.
 *  - CONVENTION 2 (normalization): phonon decodes orthonormal real SH (N3D/sqrt(4pi)); the SN3D
 *    encode is scaled by ambi_phonon_scale = sqrt(2l+1)/sqrt(4pi), VERIFIED in test_ambi against
 *    phonon's hardcoded SH constants. (Deriving the matrix from iplAmbisonicsEncodeEffect is an
 *    equivalent alternative.)
 *  - CONVENTION 3 (orientation): phonon is right-handed x=right/y=up/-z=ahead; its world axes are
 *    the room frame. The room's HEAD convention is identity-faces-+z (Motive default), handled in
 *    head_basis (ahead=q*+z, right=q*-x).
 *  - CONVENTION 2b (m<0 sign): NO fix-up — phonon's real-SH m<0 convention MATCHES this encode
 *    (the xval golden pins ambi_encode_sn3d against phonon's own SH table, sin harmonics included).
 *    A negation of ACN 1,4,5,9,10,11 lived here briefly, added to satisfy a laterality test that
 *    drove the decode with DC: the default HRTF's per-ear DC gains are laterally OPPOSITE its
 *    audible ILD, so the DC assertion passed exactly when the field was mirrored — it inverted
 *    left/right for all real audio and was caught by ear at the rig. THE LESSON: a laterality
 *    check must drive a tone in the hearing band, never DC (test_steam_decode uses 660 Hz).
 *  Still pending: HRTF *quality* (timbre / externalization / front-back / elevation) is unverified by
 *  ear — the smoke test covers gross laterality only.
 */
#include "steam_decode.h"
#include "ambisonics.h"
#include "frame.h"          /* BWA_ROOM_* identity basis + frame_qrot */
#include "sink.h"           /* BWA_CHANNELS */

#include <phonon.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct SteamMonitor {
    IPLContext                 context;
    IPLHRTF                    hrtf;
    IPLAmbisonicsDecodeEffect  decode;
    uint32_t                   channels;      /* 26 */
    uint32_t                   frame_size;    /* phonon frameSize == the per-block n (fixed at create) */
    float                      encode[BWA_CHANNELS][BWA_AMBI_CH];  /* fixed 26→16 SH matrix */
    float*                     ambi;          /* 16 * frame_size, planar scratch */
    /* per-voice fleet (BWA_PROFILE_BINAURAL mode 2): one binaural effect per rt voice slot. The
     * effect carries overlap state, so a slot RECYCLE (generation change / inactive gap) resets it
     * before reuse — otherwise a stolen slot's tail bleeds into the next voice's first block. */
    IPLBinauralEffect*         pv_fx;         /* pv_cap effects; NULL = no fleet */
    uint32_t                   pv_cap;
    uint32_t*                  pv_gen;        /* last-seen voice generation per slot */
    uint8_t*                   pv_live;       /* slot was active last block */
    float*                     pv_scratch;    /* 2 * frame_size, one effect's stereo out */
};

/* CONVENTIONS 1 + 2 — axes and normalization, now hosted as ambi_encode_phonon (ambisonics.c) so
 * the SDK-independent direct-binaural encode (rt.c) shares the exact function. Axes: room
 * (x=right, y=up, z=back) -> phonon net-AmbiX (front=-z_room, left=-x_room, up=+y_room; sh.cpp
 * convertedDirection). Normalization: iplAmbisonicsDecodeEffect takes no ambisonics-type param and
 * decodes orthonormal real SH (= N3D/sqrt(4pi); audio_buffer.h "N3D is used internally for
 * everything"), so the SN3D encode is scaled per ACN channel by ambi_phonon_scale =
 * sqrt(2l+1)/sqrt(4pi); test_ambi verifies the product against phonon's hardcoded SH constants.
 * If the image is mirrored/rotated, ambi_encode_phonon and head_basis() (CONVENTION 3) are where
 * to correct it — they must be corrected together.
 * No m<0 sign fix-up: phonon's real-SH convention matches ambi_encode_sn3d for ALL channels (the
 * xval golden pins the encode against phonon's own SH table, sin harmonics included). See the
 * CONVENTION 2b note above for the history — a DC-driven test once argued otherwise, wrongly. */
static void sh_encode(const float room_dir[3], float y[BWA_AMBI_CH]) {
    ambi_encode_phonon(room_dir, y);
}

/* CONVENTION 3 — head orientation frame. phonon is right-handed x=right/y=up/-z=ahead (C API
 * docs); its WORLD axes coincide with the room frame, so the head's world-space ahead/up/right
 * vectors pass straight into IPLCoordinateSpace3. The head's LOCAL frame is the room convention
 * (bw_audio.h BWA_ROOM_*): identity faces +z (Motive default), right ear at -x. */
static void head_basis(const float q[4], IPLCoordinateSpace3* cs) {
    float a[3], u[3], r[3];
    frame_qrot(q, BWA_ROOM_AHEAD, a); frame_qrot(q, BWA_ROOM_UP, u); frame_qrot(q, BWA_ROOM_RIGHT, r);
    cs->ahead = (IPLVector3){ a[0], a[1], a[2] };
    cs->up    = (IPLVector3){ u[0], u[1], u[2] };
    cs->right = (IPLVector3){ r[0], r[1], r[2] };
    cs->origin = (IPLVector3){ 0, 0, 0 };
}

SteamMonitor* steam_monitor_create(const Layout* L, uint32_t sample_rate, uint32_t block_size,
                                   const char* hrtf_path, uint32_t max_voices) {
    if (!L || block_size == 0) return NULL;
    SteamMonitor* m = (SteamMonitor*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->channels = L->count;                   /* the layout's speaker count (<= BWA_CHANNELS capacity) */
    m->frame_size = block_size;

    IPLContextSettings cs = { 0 };
    cs.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&cs, &m->context) != IPL_STATUS_SUCCESS) { free(m); return NULL; }

    IPLAudioSettings as = { 0 };
    as.samplingRate = (IPLint32)sample_rate;
    as.frameSize    = (IPLint32)block_size;   /* phonon effects process exactly frameSize samples/apply */

    IPLHRTFSettings hs = { 0 };
    hs.type   = hrtf_path ? IPL_HRTFTYPE_SOFA : IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    if (hrtf_path) hs.sofaFileName = hrtf_path;
    if (iplHRTFCreate(m->context, &as, &hs, &m->hrtf) != IPL_STATUS_SUCCESS) {
        iplContextRelease(&m->context); free(m); return NULL;
    }

    IPLAmbisonicsDecodeEffectSettings ds = { 0 };
    ds.maxOrder = BWA_AMBI_ORDER;
    ds.hrtf = m->hrtf;
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;   /* binaural output is 2-ch */
    if (iplAmbisonicsDecodeEffectCreate(m->context, &as, &ds, &m->decode) != IPL_STATUS_SUCCESS) {
        iplHRTFRelease(&m->hrtf); iplContextRelease(&m->context); free(m); return NULL;
    }

    /* fixed 26→16 encode matrix: each speaker is a virtual source at its direction from the
     * layout's nominal listening point (the centroid — the origin canonically sits on the floor) */
    for (uint32_t k = 0; k < m->channels; ++k) {
        float pos[3] = { L->speakers[k].pos[0] - L->ref[0],
                         L->speakers[k].pos[1] - L->ref[1],
                         L->speakers[k].pos[2] - L->ref[2] };
        float len = sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
        float dir[3] = { 0, 0, -1 };
        if (len > 1e-6f) { dir[0] = pos[0] / len; dir[1] = pos[1] / len; dir[2] = pos[2] / len; }
        sh_encode(dir, m->encode[k]);
    }

    m->ambi = (float*)calloc((size_t)BWA_AMBI_CH * block_size, sizeof(float));
    if (!m->ambi) { steam_monitor_destroy(m); return NULL; }

    /* per-voice fleet (mode 2): NON-FATAL — a partial/failed fleet is torn down and the monitor
     * still works in field mode (the engine reads steam_monitor_pervoice and keeps rt in mode 1).
     * Effects are created here on the control thread; each holds only its overlap state at this
     * frameSize, so even the full 256-slot fleet is modest. */
    if (max_voices) {
        m->pv_fx      = (IPLBinauralEffect*)calloc(max_voices, sizeof *m->pv_fx);
        m->pv_gen     = (uint32_t*)calloc(max_voices, sizeof *m->pv_gen);
        m->pv_live    = (uint8_t*)calloc(max_voices, sizeof *m->pv_live);
        m->pv_scratch = (float*)calloc((size_t)2 * block_size, sizeof(float));
        int ok = m->pv_fx && m->pv_gen && m->pv_live && m->pv_scratch;
        if (ok) {
            IPLBinauralEffectSettings bs = { 0 };
            bs.hrtf = m->hrtf;
            for (uint32_t i = 0; i < max_voices; ++i)
                if (iplBinauralEffectCreate(m->context, &as, &bs, &m->pv_fx[i]) != IPL_STATUS_SUCCESS) {
                    for (uint32_t j = 0; j < i; ++j) iplBinauralEffectRelease(&m->pv_fx[j]);
                    ok = 0;
                    break;
                }
        }
        if (ok) m->pv_cap = max_voices;
        else {
            free(m->pv_fx); free(m->pv_gen); free(m->pv_live); free(m->pv_scratch);
            m->pv_fx = NULL; m->pv_gen = NULL; m->pv_live = NULL; m->pv_scratch = NULL;
        }
    }
    return m;
}

int steam_monitor_pervoice(const SteamMonitor* m) { return m && m->pv_cap > 0; }

void steam_monitor_process(SteamMonitor* m, const float* bus26, const float* direct16,
                           const RtDirectVoice* voices, uint32_t nvoices,
                           const float p[3], const float q[4], float* out, uint32_t n) {
    (void)p;
    if (!m || n == 0) return;
    /* phonon's effect was created for exactly frame_size samples — an off-spec device block
     * (a renegotiating ASIO device != cfg.block_size) renders silence rather than crash. */
    if (n != m->frame_size) { memset(out, 0, sizeof(float) * (size_t)n * 2); return; }

    /* encode: ambi[k][i] = sum over speakers of encode[spk][k] * bus[spk][i] (planar, stride n) */
    memset(m->ambi, 0, sizeof(float) * (size_t)BWA_AMBI_CH * n);
    for (uint32_t s = 0; s < m->channels; ++s) {
        const float* src = bus26 + (size_t)s * n;
        const float* g = m->encode[s];
        for (uint32_t k = 0; k < BWA_AMBI_CH; ++k) {
            if (g[k] == 0.0f) continue;
            float* a = m->ambi + (size_t)k * n;
            float gk = g[k];
            for (uint32_t i = 0; i < n; ++i) a[i] += gk * src[i];
        }
    }
    /* direct-binaural field (BWA_PROFILE_BINAURAL): already in this basis (rt.c encodes with the
     * same ambi_encode_phonon), so it sums straight into the virtual-speaker field — one decode. */
    if (direct16)
        for (uint32_t k = 0; k < BWA_AMBI_CH; ++k) {
            float* a = m->ambi + (size_t)k * n;
            const float* d = direct16 + (size_t)k * n;
            for (uint32_t i = 0; i < n; ++i) a[i] += d[i];
        }

    IPLfloat32* ambiPtrs[BWA_AMBI_CH];
    for (uint32_t k = 0; k < BWA_AMBI_CH; ++k) ambiPtrs[k] = m->ambi + (size_t)k * n;
    IPLfloat32* outPtrs[2] = { out, out + n };

    IPLAudioBuffer inBuf  = { .numChannels = (IPLint32)BWA_AMBI_CH, .numSamples = (IPLint32)n, .data = ambiPtrs };
    IPLAudioBuffer outBuf = { .numChannels = 2, .numSamples = (IPLint32)n, .data = outPtrs };

    IPLAmbisonicsDecodeEffectParams params = { 0 };
    params.order = BWA_AMBI_ORDER;
    params.hrtf = m->hrtf;
    params.binaural = IPL_TRUE;
    head_basis(q, &params.orientation);

    iplAmbisonicsDecodeEffectApply(m->decode, &params, &inBuf, &outBuf);

    /* per-voice point taps (mode 2): one true HRTF convolution per active voice, summed onto the
     * field decode. Directions arrive room-frame from rt; iplCalculateRelativeDirection turns them
     * into the listener-local frame the binaural effect expects (CONVENTION 3's head basis — one
     * source of truth for the orientation math). A slot whose generation changed (voice recycled)
     * or that went inactive gets its effect reset, so no stale overlap tail bleeds into a new
     * voice. Voices fade THROUGH the rt gain ramps before deactivating, so a dropped tail is a
     * tail of silence. */
    if (m->pv_fx && voices) {
        IPLCoordinateSpace3 head;
        head_basis(q, &head);
        const uint32_t cap = nvoices < m->pv_cap ? nvoices : m->pv_cap;
        for (uint32_t i = 0; i < cap; ++i) {
            const RtDirectVoice* dv = &voices[i];
            if (!dv->active || !dv->mono) { m->pv_live[i] = 0; continue; }
            if (!m->pv_live[i] || m->pv_gen[i] != dv->gen) {
                iplBinauralEffectReset(m->pv_fx[i]);
                m->pv_gen[i] = dv->gen;
            }
            m->pv_live[i] = 1;

            IPLBinauralEffectParams bp = { 0 };
            bp.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;   /* block-rate dirs: interpolate, no snap */
            bp.spatialBlend  = 1.0f;
            bp.hrtf          = m->hrtf;
            const IPLVector3 src = { dv->dir[0], dv->dir[1], dv->dir[2] };
            const IPLVector3 org = { 0, 0, 0 };
            bp.direction = iplCalculateRelativeDirection(m->context, src, org, head.ahead, head.up);

            IPLfloat32* inPtr[1]  = { (IPLfloat32*)dv->mono };
            IPLfloat32* sc[2]     = { m->pv_scratch, m->pv_scratch + n };
            IPLAudioBuffer vin    = { .numChannels = 1, .numSamples = (IPLint32)n, .data = inPtr };
            IPLAudioBuffer vout   = { .numChannels = 2, .numSamples = (IPLint32)n, .data = sc };
            iplBinauralEffectApply(m->pv_fx[i], &bp, &vin, &vout);
            for (uint32_t s = 0; s < n; ++s) { out[s] += sc[0][s]; out[n + s] += sc[1][s]; }
        }
        for (uint32_t i = cap; i < m->pv_cap; ++i) m->pv_live[i] = 0;
    }
}

void steam_monitor_destroy(SteamMonitor* m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->pv_cap; ++i) iplBinauralEffectRelease(&m->pv_fx[i]);
    free(m->pv_fx); free(m->pv_gen); free(m->pv_live); free(m->pv_scratch);
    if (m->decode)  iplAmbisonicsDecodeEffectRelease(&m->decode);
    if (m->hrtf)    iplHRTFRelease(&m->hrtf);
    if (m->context) iplContextRelease(&m->context);
    free(m->ambi);
    free(m);
}
