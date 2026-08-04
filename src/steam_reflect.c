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
#include "ambisonics.h"   /* BWA_AMBI_CH (max ambisonic channel count for scratch sizing) */
#include "profile.h"

#include <phonon.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REFL_HZ          12      /* reflection sim rate (reverb changes slowly; cheaper than occlusion) */
#define REFL_DIFFUSE     1024    /* diffuse-reflection sample directions */
#define REFL_TRANSITION_S 0.25f  /* hybrid transition time: IR length rendered by directional convolution
                                  * (early reflections); the longer parametric tail is the FDN's job */
/* HYBRID = early-reflection convolution + parametric (FDN) late tail, rendered as a full ambisonic
 * field (order `order`, (order+1)^2 channels) so the early reflections are DIRECTIONAL — they decode
 * to the 26 speakers from the directions they actually arrive. (Requires the patched phonon: Steam
 * Audio 4.8.1's complex ArrayMath::multiplyAccumulate reads the accumulator with an aligned SSE load
 * on its unaligned code path, which access-violates on the odd ambisonic channels — numSpectrumSamples
 * is odd, so the per-channel FFT stride is 8 mod 16. Fixed load->loadu in the vendored build; see
 * third_party/README.md. A symmetric scene still yields a near-omni field, which is physically right.) */
#define REFL_TYPE        IPL_REFLECTIONEFFECTTYPE_HYBRID
#define BWA_BAKE_SPACING  1.5f    /* spacing (m) of the manual reverb-probe grid (bwa_reflections_desc.bake) */
#define BWA_BAKE_MARGIN   1.5f    /* expand the speaker XZ bounds by this so the grid covers the whole room */

struct SteamReflect {
    IPLContext   ctx;            /* BORROWED from scene */
    IPLScene     scene_ipl;      /* BORROWED from scene */
    SteamScene*  scene;          /* BORROWED: for the shared scene lock around RunReflections */
    RtCore*      rt;             /* for the listener pose */
    IPLSimulator sim;            /* OWNED: reflections-only simulator */
    IPLSource    bed;            /* OWNED + IMMORTAL: listener-centric bed source */
    IPLReflectionEffect       refl;   /* OWNED */
    IPLAmbisonicsDecodeEffect dec;    /* OWNED: custom 26-dir layout, panning */
    IPLVector3   spk[BWA_CHANNELS];
    IPLProbeBatch probes;             /* OWNED (baked mode): probes carrying the baked reverb, NULL = real-time */
    IPLSceneType  scene_type;         /* DEFAULT/EMBREE, for the bake + sim */
    int           baked;              /* 1 = look up baked reverb instead of ray-tracing each sim tick */

    uint32_t channels;          /* the layout's speaker count (<= BWA_CHANNELS capacity) */
    uint32_t order, ambi_ch, n;
    IPLint32 ir_size;
    float    duration; uint32_t rays, bounces;
    _Atomic float wet_gain;     /* control thread sets, audio-thread tap reads (linear; 1 = unity) */
    float*   ambi;              /* ambi_ch * n */
    float*   out26;             /* BWA_CHANNELS * n */

    /* seqlock-published reflection params (sim thread writes, audio thread reads) */
    _Atomic uint32_t seq;
    uint32_t         seq_w;     /* writer's private copy of seq */
    IPLReflectionEffectParams pub;

    HANDLE        thread;
    volatile LONG stop;
};

/* phonon's identity basis (ahead = -z), not the room's (+z ahead) — harmless AND load-bearing to
 * keep consistent: the sim's ambisonic IR is oriented by this listener frame, and the decode side
 * (steam_reflect_tap) passes the SAME basis as the decode orientation, so the two cancel and the
 * field stays world-locked with the custom speaker directions given in room coordinates. Change
 * one only with the other. See steam_scene.c's identity_cs note. */
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
    BWA_THREAD_NAME("bw-sim (reflections)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  /* never preempt the audio callback */
    while (!r->stop) {
        float lp[3], lq[4]; rt_read_pose(r->rt, lp, lq); (void)lq;

        IPLSimulationInputs in; memset(&in, 0, sizeof in);
        in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        cs_at(&in.source, lp);                          /* bed source co-located with the listener */
        /* Hybrid-reverb inputs. These are NOT optional: a zeroed reverbScale nulls the parametric
         * tail, and a zero transition time leaves the convolution part empty — together they collapse
         * the bed to silence/omni. reverbScale 1 = use the simulated decay; the transition time is the
         * length of IR rendered by (directional) convolution, the rest by the parametric FDN. */
        in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
        in.hybridReverbTransitionTime = (r->duration < REFL_TRANSITION_S) ? r->duration : REFL_TRANSITION_S;
        in.hybridReverbOverlapPercent = 0.25f;
        if (r->baked) {                                 /* look up the precomputed reverb at the listener instead of ray-tracing */
            in.baked = IPL_TRUE;
            in.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
            in.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
        }
        iplSourceSetInputs(r->bed, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);

        IPLSimulationSharedInputs sh; memset(&sh, 0, sizeof sh);
        cs_at(&sh.listener, lp);
        sh.numRays = (IPLint32)r->rays; sh.numBounces = (IPLint32)r->bounces;
        sh.duration = r->duration; sh.order = (IPLint32)r->order;
        sh.irradianceMinDistance = 1.0f;
        iplSimulatorSetSharedInputs(r->sim, IPL_SIMULATIONFLAGS_REFLECTIONS, &sh);

        BWA_ZONE_BEGIN(zr, "reflection ray-trace");
        steam_scene_ray_lock(r->scene);          /* shared: can't race an iplSceneCommit (a mover moved) */
        iplSimulatorRunReflections(r->sim);
        steam_scene_ray_unlock(r->scene);
        BWA_ZONE_END(zr);

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
    if (rp.numChannels > (IPLint32)r->ambi_ch) rp.numChannels = (IPLint32)r->ambi_ch;   /* clamp to the baked order */
    if (rp.irSize > r->ir_size)                rp.irSize       = r->ir_size;

    /* convolve the mono aux send -> the full ambisonic reflection field (directional early reflections) */
    float* inP[1]  = { (float*)aux };
    IPLAudioBuffer in  = { .numChannels = 1, .numSamples = (IPLint32)n, .data = inP };
    float* ambP[BWA_AMBI_CH];
    for (uint32_t k = 0; k < r->ambi_ch; ++k) ambP[k] = r->ambi + (size_t)k * n;
    IPLAudioBuffer amb = { .numChannels = (IPLint32)r->ambi_ch, .numSamples = (IPLint32)n, .data = ambP };
    iplReflectionEffectApply(r->refl, &rp, &in, &amb, NULL);   /* NULL mixer: single bed */

    /* decode ambisonic -> the array, world-locked (identity orientation matches the sim's world listener) */
    float* outP[BWA_CHANNELS];
    for (uint32_t s = 0; s < r->channels; ++s) outP[s] = r->out26 + (size_t)s * n;
    IPLAudioBuffer o26 = { .numChannels = (IPLint32)r->channels, .numSamples = (IPLint32)n, .data = outP };
    IPLAmbisonicsDecodeEffectParams dp; memset(&dp, 0, sizeof dp);
    dp.order = (IPLint32)r->order; dp.hrtf = NULL; dp.binaural = IPL_FALSE;
    cs_at(&dp.orientation, (float[3]){ 0.f, 0.f, 0.f });
    iplAmbisonicsDecodeEffectApply(r->dec, &dp, &amb, &o26);

    const float g = atomic_load_explicit(&r->wet_gain, memory_order_relaxed);   /* live wet level */
    for (uint32_t s = 0; s < r->channels; ++s)          /* sum the wet reverb onto the bus (composes with the dry voices) */
        for (uint32_t i = 0; i < n; ++i) bus[(size_t)s * n + i] += g * r->out26[(size_t)s * n + i];
}

void steam_reflect_set_gain(SteamReflect* r, float linear) {
    if (r) atomic_store_explicit(&r->wet_gain, linear, memory_order_relaxed);
}

/* Precompute the listener-centric reverb at a grid of probes covering the listening zone, then attach
 * the batch to the simulator so the sim thread looks the reverb up instead of ray-tracing each tick.
 * Probes are placed MANUALLY (one CENTROID call per grid point) because UNIFORMFLOOR's floor-finding is
 * mesh-winding-sensitive. NB: phonon's CENTROID generator reads the probe CENTER from the transform's
 * translation column and the influence radius from the basis lengths (min/2) — so the translation is
 * the grid POINT (not a corner), and a box of 2*spacing gives radius == spacing => overlapping coverage,
 * which is what getInfluencingProbes needs to match the listener to a probe. */
static int do_bake(SteamReflect* r, const Layout* L) {
    float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f, ysum = 0.f;
    for (uint32_t s = 0; s < L->count; ++s) {
        const float* p = L->speakers[s].pos;
        if (p[0] < xmin) xmin = p[0]; if (p[0] > xmax) xmax = p[0];
        if (p[2] < zmin) zmin = p[2]; if (p[2] > zmax) zmax = p[2];
        ysum += p[1];
    }
    const float head = ysum / (float)L->count;        /* listening-plane height = mean speaker height */
    xmin -= BWA_BAKE_MARGIN; xmax += BWA_BAKE_MARGIN; zmin -= BWA_BAKE_MARGIN; zmax += BWA_BAKE_MARGIN;

    if (iplProbeBatchCreate(r->ctx, &r->probes) != IPL_STATUS_SUCCESS) return 0;
    int np = 0;
    const float bs = 2.0f * BWA_BAKE_SPACING;          /* box edge; radius = bs/2 = spacing => probes overlap */
    for (float x = xmin; x <= xmax + 1e-3f; x += BWA_BAKE_SPACING)
    for (float z = zmin; z <= zmax + 1e-3f; z += BWA_BAKE_SPACING) {
        IPLProbeArray pa = NULL;
        if (iplProbeArrayCreate(r->ctx, &pa) != IPL_STATUS_SUCCESS) continue;
        IPLProbeGenerationParams gp; memset(&gp, 0, sizeof gp);
        gp.type = IPL_PROBEGENERATIONTYPE_CENTROID;
        gp.transform.elements[0][0] = bs; gp.transform.elements[0][3] = x;     /* translation = box CENTER = the grid point */
        gp.transform.elements[1][1] = bs; gp.transform.elements[1][3] = head;
        gp.transform.elements[2][2] = bs; gp.transform.elements[2][3] = z;
        gp.transform.elements[3][3] = 1.f;
        steam_scene_ray_lock(r->scene);                 /* shared: reads the scene — no concurrent commit */
        iplProbeArrayGenerateProbes(pa, r->scene_ipl, &gp);
        steam_scene_ray_unlock(r->scene);
        if (iplProbeArrayGetNumProbes(pa) > 0) { iplProbeBatchAddProbeArray(r->probes, pa); ++np; }
        iplProbeArrayRelease(&pa);
    }
    if (np == 0) { iplProbeBatchRelease(&r->probes); r->probes = NULL; return 0; }
    iplProbeBatchCommit(r->probes);
    fprintf(stderr, "bw_audio: baking reverb at %d probes\n", np);

    IPLReflectionsBakeParams bp; memset(&bp, 0, sizeof bp);
    bp.scene = r->scene_ipl; bp.probeBatch = r->probes; bp.sceneType = r->scene_type;
    bp.identifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
    bp.identifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
    bp.bakeFlags = (IPLReflectionsBakeFlags)(IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION | IPL_REFLECTIONSBAKEFLAGS_BAKEPARAMETRIC);
    bp.numRays = (IPLint32)r->rays; bp.numDiffuseSamples = REFL_DIFFUSE;
    bp.numBounces = (IPLint32)(r->bounces > 32u ? r->bounces : 32u);
    bp.simulatedDuration = 2.0f;                        /* long IR so the parametric RT60 estimates well... */
    bp.savedDuration = r->duration;                     /* ...but only save the early (directional) part */
    bp.order = (IPLint32)r->order; bp.numThreads = 2; bp.irradianceMinDistance = 1.f;
    steam_scene_ray_lock(r->scene);                     /* shared, for the whole bake: it ray-traces the scene
                                                         * and must not race a commit from the occlusion sim
                                                         * thread (geometry can't change mid-bake anyway) */
    iplReflectionsBakerBake(r->ctx, &bp, NULL, NULL);   /* BLOCKS: ray-traces every probe once */
    steam_scene_ray_unlock(r->scene);

    iplSimulatorAddProbeBatch(r->sim, r->probes);
    iplSimulatorCommit(r->sim);
    return np;
}

SteamReflect* steam_reflect_create(SteamScene* scene, RtCore* rt, const Layout* L,
                                   uint32_t sample_rate, uint32_t block, uint32_t order,
                                   float ir_seconds, uint32_t num_rays, uint32_t num_bounces,
                                   float wet_gain, int bake) {
    if (!scene || !rt || !L || block == 0 || order < 1 || order > 3) return NULL;
    SteamReflect* r = (SteamReflect*)calloc(1, sizeof *r);
    if (!r) return NULL;
    atomic_store_explicit(&r->wet_gain, wet_gain, memory_order_relaxed);
    r->ctx       = (IPLContext)steam_scene_ipl_context(scene);
    r->scene_ipl = (IPLScene)steam_scene_ipl_scene(scene);
    r->scene     = scene;                        /* for the shared scene lock on the sim thread */
    if (!r->ctx || !r->scene_ipl) { free(r); return NULL; }
    r->scene_type = (IPLSceneType)steam_scene_ipl_scenetype(scene);
    r->rt = rt; r->n = block; r->order = order; r->ambi_ch = (order + 1) * (order + 1);
    r->channels = L->count;                         /* the layout's speaker count (<= BWA_CHANNELS cap) */
    r->ir_size = (IPLint32)ceilf(ir_seconds * (float)sample_rate);
    r->duration = ir_seconds; r->rays = num_rays; r->bounces = num_bounces;

    IPLAudioSettings as; memset(&as, 0, sizeof as);
    as.samplingRate = (IPLint32)sample_rate; as.frameSize = (IPLint32)block;

    IPLSimulationSettings ss; memset(&ss, 0, sizeof ss);
    ss.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    ss.sceneType = r->scene_type;                                    /* match the borrowed scene (DEFAULT/EMBREE) */
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

    steam_scene_flush(scene);      /* a just-staged room (set_box -> start inside one scene-sim tick)
                                    * must be COMMITTED before the bake below ray-traces — and before
                                    * the real-time sim's first tick, for a deterministic start */

    IPLSourceSettings src; memset(&src, 0, sizeof src);
    src.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    if (iplSourceCreate(r->sim, &src, &r->bed) != IPL_STATUS_SUCCESS) goto fail;
    iplSourceAdd(r->bed, r->sim);
    iplSimulatorCommit(r->sim);

    if (bake) {                                         /* precompute the reverb now; sim thread then looks it up */
        r->baked = (do_bake(r, L) > 0);
        if (!r->baked) fprintf(stderr, "bw_audio: reflection bake requested but produced no probes; using real-time reflections\n");
    }

    IPLReflectionEffectSettings rs; memset(&rs, 0, sizeof rs);
    rs.type = REFL_TYPE; rs.irSize = r->ir_size; rs.numChannels = (IPLint32)r->ambi_ch;   /* full directional ambisonic */
    if (iplReflectionEffectCreate(r->ctx, &as, &rs, &r->refl) != IPL_STATUS_SUCCESS) goto fail;

    for (uint32_t s = 0; s < r->channels; ++s) {        /* speaker dirs in phonon's cartesian frame (== room),
                                                         * from the layout's nominal listening point */
        float p[3] = { L->speakers[s].pos[0] - L->ref[0], L->speakers[s].pos[1] - L->ref[1],
                       L->speakers[s].pos[2] - L->ref[2] };
        float m = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        r->spk[s] = (m < 1e-6f) ? (IPLVector3){ 0, 0, -1 } : (IPLVector3){ p[0]/m, p[1]/m, p[2]/m };
    }
    IPLAmbisonicsDecodeEffectSettings ds; memset(&ds, 0, sizeof ds);
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_CUSTOM;
    ds.speakerLayout.numSpeakers = (IPLint32)r->channels;
    ds.speakerLayout.speakers = r->spk;
    ds.hrtf = NULL; ds.maxOrder = (IPLint32)order;
    if (iplAmbisonicsDecodeEffectCreate(r->ctx, &as, &ds, &r->dec) != IPL_STATUS_SUCCESS) goto fail;

    r->ambi  = (float*)calloc((size_t)r->ambi_ch * block, sizeof(float));
    r->out26 = (float*)calloc((size_t)r->channels * block, sizeof(float));
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
    if (r->probes) { iplSimulatorRemoveProbeBatch(r->sim, r->probes); iplProbeBatchRelease(&r->probes); }
    if (r->sim) iplSimulatorRelease(&r->sim);        /* ctx + scene are BORROWED — never released here */
    free(r->ambi); free(r->out26);
    free(r);
}
