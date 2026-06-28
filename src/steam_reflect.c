/*
 * steam_reflect.c — reflection bed, phonon side. See steam_reflect.h. Build-only-with-SDK.
 *
 * Owns: a REFLECTIONS-only IPLSimulator (borrowing SteamScene's context + scene), an IMMORTAL bed
 * IPLSource at the listener, a dedicated sim thread (RunReflections at REFL_HZ, publishing the
 * IPLReflectionEffectParams POD through a seqlock), and the audio-thread effects (a HYBRID
 * IPLReflectionEffect + a custom 26-direction IPLAmbisonicsDecodeEffect, panning, no HRTF) + scratch.
 *
 * Thread model: the sim thread is the SOLE writer of the published params; steam_reflect_tap (audio
 * thread) is the SOLE reader and the SOLE consumer of the IR triple buffer (a hard invariant — apply
 * mutates the triple buffer's read slot). The IR (params.ir) aliases interior memory of the bed
 * source, so the bed source is created once and never released until destroy, after the audio thread
 * has joined.
 */
#include "steam_reflect.h"
#include "ambisonics.h"   /* BW_AMBI_CH (max ambisonic channel count for scratch sizing) */

#include <phonon.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define REFL_HZ          12      /* reflection sim rate (reverb changes slowly; cheaper than occlusion) */
#define REFL_DIFFUSE     1024    /* diffuse-reflection sample directions */
/* HYBRID = early-reflection convolution + parametric (FDN) late tail. The simulator's reflection IR
 * for a listener-centric reverb source is order-0 (only the W/omni ambisonic channel is populated;
 * reading channels 1+ access-violates in phonon's overlap-save convolution — verified down to a
 * 1-channel repro). So the convolution is run as ONE channel (omni). That is the right model for a
 * diffuse bed anyway: omni early reflections add room character over the parametric tail, and the
 * result is spread (world-locked) across the 26 speakers via the W ambisonic decode. */
#define REFL_TYPE        IPL_REFLECTIONEFFECTTYPE_HYBRID
#define REFL_CONV_CH     1       /* convolution/effect channel count: omni (W) only — see above */

struct SteamReflect {
    IPLContext   ctx;            /* BORROWED from scene */
    IPLScene     scene_ipl;      /* BORROWED from scene */
    RtCore*      rt;             /* for the listener pose */
    IPLSimulator sim;            /* OWNED: reflections-only simulator */
    IPLSource    bed;            /* OWNED + IMMORTAL: listener-centric bed source */
    IPLReflectionEffect       refl;   /* OWNED */
    IPLAmbisonicsDecodeEffect dec;    /* OWNED: custom 26-dir layout, panning */
    IPLVector3   spk[BW_CHANNELS];

    uint32_t order, ambi_ch, n;
    IPLint32 ir_size;
    float    duration; uint32_t rays, bounces;
    float*   ambi;              /* ambi_ch * n */
    float*   out26;             /* BW_CHANNELS * n */

    /* seqlock-published reflection params (sim thread writes, audio thread reads) */
    _Atomic uint32_t seq;
    uint32_t         seq_w;     /* writer's private copy of seq */
    IPLReflectionEffectParams pub;

    HANDLE        thread;
    volatile LONG stop;
};

static void cs_at(IPLCoordinateSpace3* cs, const float origin[3]) {
    cs->right = (IPLVector3){ 1, 0, 0 }; cs->up = (IPLVector3){ 0, 1, 0 };
    cs->ahead = (IPLVector3){ 0, 0, -1 }; cs->origin = (IPLVector3){ origin[0], origin[1], origin[2] };
}

/* seqlock publish (sim thread, sole writer) */
static void refl_publish(SteamReflect* r, const IPLReflectionEffectParams* p) {
    uint32_t s = r->seq_w;
    atomic_store_explicit(&r->seq, s + 1, memory_order_relaxed);   /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
    r->pub = *p;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&r->seq, s + 2, memory_order_relaxed);   /* even: committed */
    r->seq_w = s + 2;
}

/* seqlock read (audio thread, sole reader). 1 + *out filled on a consistent read; 0 if never
 * published or (rarely) contended this block. */
static int refl_read(SteamReflect* r, IPLReflectionEffectParams* out) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint32_t s1 = atomic_load_explicit(&r->seq, memory_order_acquire);
        if (s1 == 0) return 0;            /* no publish yet -> silent */
        if (s1 & 1u) continue;            /* writer mid-write */
        *out = r->pub;
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&r->seq, memory_order_relaxed);
        if (s1 == s2) return 1;
    }
    return 0;
}

static DWORD WINAPI sim_thread(LPVOID arg) {
    SteamReflect* r = (SteamReflect*)arg;
    while (!r->stop) {
        float lp[3], lq[4]; rt_read_pose(r->rt, lp, lq); (void)lq;

        IPLSimulationInputs in; memset(&in, 0, sizeof in);
        in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        cs_at(&in.source, lp);                          /* bed source co-located with the listener */
        iplSourceSetInputs(r->bed, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);

        IPLSimulationSharedInputs sh; memset(&sh, 0, sizeof sh);
        cs_at(&sh.listener, lp);
        sh.numRays = (IPLint32)r->rays; sh.numBounces = (IPLint32)r->bounces;
        sh.duration = r->duration; sh.order = (IPLint32)r->order;
        sh.irradianceMinDistance = 1.0f;
        iplSimulatorSetSharedInputs(r->sim, IPL_SIMULATIONFLAGS_REFLECTIONS, &sh);

        iplSimulatorRunReflections(r->sim);

        IPLSimulationOutputs out; memset(&out, 0, sizeof out);
        iplSourceGetOutputs(r->bed, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
        out.reflections.type    = REFL_TYPE;            /* getOutputs does NOT set type */
        out.reflections.tanDevice = NULL; out.reflections.tanSlot = -1;
        refl_publish(r, &out.reflections);

        Sleep(1000 / REFL_HZ);
    }
    return 0;
}

void steam_reflect_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux) {
    SteamReflect* r = (SteamReflect*)ud;
    (void)lp; (void)lq;
    if (n != r->n) return;                              /* off-spec device block: run dry */
    IPLReflectionEffectParams rp;
    if (!refl_read(r, &rp)) return;                     /* silent until the first sim publish */
    rp.type = REFL_TYPE;                                /* authoritative: must match the effect's create type */
    if (rp.numChannels > REFL_CONV_CH) rp.numChannels = REFL_CONV_CH;   /* read only the valid (omni) IR channel */
    if (rp.irSize > r->ir_size)        rp.irSize       = r->ir_size;

    /* convolve the mono aux send -> the omni (W) ambisonic channel. r->ambi channels 1..ambi_ch-1
     * stay zero (calloc'd once; the 1-channel apply only writes channel 0), so the decode below sees
     * a [W,0,0,..] field and spreads it omnidirectionally across the 26 speakers. */
    float* inP[1]  = { (float*)aux };
    IPLAudioBuffer in   = { 1, (IPLint32)n, inP };
    float* ambP[BW_AMBI_CH];
    for (uint32_t k = 0; k < r->ambi_ch; ++k) ambP[k] = r->ambi + (size_t)k * n;
    IPLAudioBuffer conv = { REFL_CONV_CH, (IPLint32)n, ambP };          /* effect out: omni channel only */
    iplReflectionEffectApply(r->refl, &rp, &in, &conv, NULL);   /* NULL mixer: single bed */
    IPLAudioBuffer amb  = { (IPLint32)r->ambi_ch, (IPLint32)n, ambP };  /* full field [W,0,0,..] for the decode */

    /* decode ambisonic -> 26 speakers, world-locked (identity orientation matches the sim's world listener) */
    float* outP[BW_CHANNELS];
    for (uint32_t s = 0; s < BW_CHANNELS; ++s) outP[s] = r->out26 + (size_t)s * n;
    IPLAudioBuffer o26 = { (IPLint32)BW_CHANNELS, (IPLint32)n, outP };
    IPLAmbisonicsDecodeEffectParams dp; memset(&dp, 0, sizeof dp);
    dp.order = (IPLint32)r->order; dp.hrtf = NULL; dp.binaural = IPL_FALSE;
    cs_at(&dp.orientation, (float[3]){ 0.f, 0.f, 0.f });
    iplAmbisonicsDecodeEffectApply(r->dec, &dp, &amb, &o26);

    for (uint32_t s = 0; s < BW_CHANNELS; ++s)          /* sum onto the bus (composes with the dry voices) */
        for (uint32_t i = 0; i < n; ++i) bus[(size_t)s * n + i] += r->out26[(size_t)s * n + i];
}

SteamReflect* steam_reflect_create(SteamScene* scene, RtCore* rt, const Layout* L,
                                   uint32_t sample_rate, uint32_t block, uint32_t order,
                                   float ir_seconds, uint32_t num_rays, uint32_t num_bounces) {
    if (!scene || !rt || !L || block == 0 || order < 1 || order > 3) return NULL;
    SteamReflect* r = (SteamReflect*)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->ctx       = (IPLContext)steam_scene_ipl_context(scene);
    r->scene_ipl = (IPLScene)steam_scene_ipl_scene(scene);
    if (!r->ctx || !r->scene_ipl) { free(r); return NULL; }
    r->rt = rt; r->n = block; r->order = order; r->ambi_ch = (order + 1) * (order + 1);
    r->ir_size = (IPLint32)ceilf(ir_seconds * (float)sample_rate);
    r->duration = ir_seconds; r->rays = num_rays; r->bounces = num_bounces;

    IPLAudioSettings as; memset(&as, 0, sizeof as);
    as.samplingRate = (IPLint32)sample_rate; as.frameSize = (IPLint32)block;

    IPLSimulationSettings ss; memset(&ss, 0, sizeof ss);
    ss.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    ss.sceneType = IPL_SCENETYPE_DEFAULT;
    ss.reflectionType = REFL_TYPE;
    ss.maxNumRays = (IPLint32)num_rays;
    ss.numDiffuseSamples = REFL_DIFFUSE;
    ss.maxDuration = ir_seconds;
    ss.maxOrder = (IPLint32)order;
    ss.maxNumSources = 1;
    ss.numThreads = 1;
    ss.samplingRate = (IPLint32)sample_rate;
    ss.frameSize = (IPLint32)block;
    if (iplSimulatorCreate(r->ctx, &ss, &r->sim) != IPL_STATUS_SUCCESS) goto fail;
    iplSimulatorSetScene(r->sim, r->scene_ipl);
    iplSimulatorCommit(r->sim);

    IPLSourceSettings src; memset(&src, 0, sizeof src);
    src.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    if (iplSourceCreate(r->sim, &src, &r->bed) != IPL_STATUS_SUCCESS) goto fail;
    iplSourceAdd(r->bed, r->sim);
    iplSimulatorCommit(r->sim);

    IPLReflectionEffectSettings rs; memset(&rs, 0, sizeof rs);
    rs.type = REFL_TYPE; rs.irSize = r->ir_size; rs.numChannels = REFL_CONV_CH;   /* omni convolution — see REFL_TYPE note */
    if (iplReflectionEffectCreate(r->ctx, &as, &rs, &r->refl) != IPL_STATUS_SUCCESS) goto fail;

    for (uint32_t s = 0; s < BW_CHANNELS; ++s) {        /* speaker dirs in phonon's cartesian frame (== room) */
        const float* p = L->speakers[s].pos;
        float m = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        r->spk[s] = (m < 1e-6f) ? (IPLVector3){ 0, 0, -1 } : (IPLVector3){ p[0]/m, p[1]/m, p[2]/m };
    }
    IPLAmbisonicsDecodeEffectSettings ds; memset(&ds, 0, sizeof ds);
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_CUSTOM;
    ds.speakerLayout.numSpeakers = (IPLint32)BW_CHANNELS;
    ds.speakerLayout.speakers = r->spk;
    ds.hrtf = NULL; ds.maxOrder = (IPLint32)order;
    if (iplAmbisonicsDecodeEffectCreate(r->ctx, &as, &ds, &r->dec) != IPL_STATUS_SUCCESS) goto fail;

    r->ambi  = (float*)calloc((size_t)r->ambi_ch * block, sizeof(float));
    r->out26 = (float*)calloc((size_t)BW_CHANNELS * block, sizeof(float));
    if (!r->ambi || !r->out26) goto fail;

    r->thread = CreateThread(NULL, 0, sim_thread, r, 0, NULL);
    if (!r->thread) goto fail;
    return r;

fail:
    steam_reflect_destroy(r);
    return NULL;
}

void steam_reflect_destroy(SteamReflect* r) {
    if (!r) return;
    if (r->thread) { InterlockedExchange(&r->stop, 1); WaitForSingleObject(r->thread, INFINITE); CloseHandle(r->thread); }
    if (r->dec) iplAmbisonicsDecodeEffectRelease(&r->dec);
    if (r->refl) iplReflectionEffectRelease(&r->refl);
    if (r->bed) { iplSourceRemove(r->bed, r->sim); iplSourceRelease(&r->bed); }
    if (r->sim) iplSimulatorRelease(&r->sim);        /* ctx + scene are BORROWED — never released here */
    free(r->ambi); free(r->out26);
    free(r);
}
