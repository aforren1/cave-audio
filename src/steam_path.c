/* steam_path.c — see steam_path.h. PATHING simulator + path bake over a probe grid. Build-only-with-SDK. */
#include "steam_path.h"
#include "rt.h"
#include "ambisonics.h"   /* BWA_AMBI_CH */
#include "profile.h"

#include <phonon.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_HZ        10        /* pathing changes slowly (geometry is static; only the listener moves) */
#define PATH_SPACING   1.0f      /* probe grid spacing (m) — denser than reverb so paths route cleanly */
#define PATH_MARGIN    1.5f      /* expand the layout bounds by this for the probe volume */
#define PATH_VIS_RADIUS 0.5f
#define PATH_VIS_THRESH 0.1f
#define PATH_VIS_RANGE  2.0f     /* only probes within this are visibility-tested (graph edges) */
#define PATH_RANGE      20.0f    /* max path length */
#define PATH_MAX_SRC    64

struct SteamPath {
    IPLContext   ctx;            /* BORROWED */
    IPLScene     scene_ipl;      /* BORROWED */
    SteamScene*  scene;          /* BORROWED: for the shared scene lock around RunPathing */
    IPLSceneType scene_type;
    RtCore*      rt;
    IPLSimulator sim;            /* OWNED: pathing-only simulator */
    IPLProbeBatch probes;        /* OWNED: probes carrying the baked visibility graph */
    IPLAmbisonicsDecodeEffect dec;  /* OWNED: ambisonic path field -> 26 speakers (custom layout, panning) */
    IPLVector3   spk[BWA_CHANNELS];
    float*       out26;          /* channels * n decode scratch */
    uint32_t     channels;       /* the layout's speaker count (<= BWA_CHANNELS capacity) */
    uint32_t     order, ambi_ch, n, sample_rate;

    /* Per-source state, steam_scene-style split: handle/pos/want are the CONTROL thread's shadow
     * (written under `lock`); `src` is the phonon object, created/committed ONLY by the sim thread
     * (reconcile_sources) — iplSimulatorCommit must never race iplSimulatorRunPathing, and both now
     * live on one thread. (The debug seam reconciles inline; it documents "no sim thread running".)
     * Slots are claim-once: a muted source keeps its slot + IPLSource until destroy. */
    CRITICAL_SECTION lock;
    struct { uint32_t handle; float pos[3]; uint8_t want; IPLSource src; } srcs[PATH_MAX_SRC];
    int          nsrc;

    HANDLE        thread;
    volatile LONG stop;
};

/* phonon's identity basis (ahead = -z), not the room's (+z ahead) — harmless here: pathing outputs
 * (eqCoeffs + world-space shCoeffs) depend on positions and the probe graph, not on which way this
 * fictitious listener faces, and the decode side (steam_path_tap) uses the SAME basis, so the
 * frames cancel. See steam_scene.c's identity_cs note. */
static void cs_at(IPLCoordinateSpace3* cs, const float o[3]) {
    cs->right = (IPLVector3){1,0,0}; cs->up = (IPLVector3){0,1,0};
    cs->ahead = (IPLVector3){0,0,-1}; cs->origin = (IPLVector3){o[0],o[1],o[2]};
}

/* probe grid + path (visibility) bake; returns probe count, 0 on failure. Same OBB convention as the
 * reflection bake: translation column = the probe CENTER, basis lengths = the box (radius = min/2). */
static int do_path_bake(SteamPath* sp, const Layout* L) {
    float xmin=1e30f,xmax=-1e30f,zmin=1e30f,zmax=-1e30f,ysum=0.f;
    for (uint32_t s=0;s<L->count;++s){ const float* p=L->speakers[s].pos;
        if(p[0]<xmin)xmin=p[0]; if(p[0]>xmax)xmax=p[0]; if(p[2]<zmin)zmin=p[2]; if(p[2]>zmax)zmax=p[2]; ysum+=p[1]; }
    const float head = ysum/(float)L->count;
    xmin-=PATH_MARGIN; xmax+=PATH_MARGIN; zmin-=PATH_MARGIN; zmax+=PATH_MARGIN;

    if (iplProbeBatchCreate(sp->ctx, &sp->probes) != IPL_STATUS_SUCCESS) return 0;
    int np=0; const float bs = 2.0f*PATH_SPACING;       /* radius = bs/2 = spacing => overlapping coverage */
    for (float x=xmin; x<=xmax+1e-3f; x+=PATH_SPACING)
    for (float z=zmin; z<=zmax+1e-3f; z+=PATH_SPACING) {
        IPLProbeArray pa=NULL; if (iplProbeArrayCreate(sp->ctx,&pa)!=IPL_STATUS_SUCCESS) continue;
        IPLProbeGenerationParams gp; memset(&gp,0,sizeof gp); gp.type=IPL_PROBEGENERATIONTYPE_CENTROID;
        gp.transform.elements[0][0]=bs; gp.transform.elements[0][3]=x;     /* center = grid point */
        gp.transform.elements[1][1]=bs; gp.transform.elements[1][3]=head;
        gp.transform.elements[2][2]=bs; gp.transform.elements[2][3]=z;
        gp.transform.elements[3][3]=1.f;
        steam_scene_ray_lock(sp->scene);                 /* shared: reads the scene — no concurrent commit */
        iplProbeArrayGenerateProbes(pa, sp->scene_ipl, &gp);
        steam_scene_ray_unlock(sp->scene);
        if (iplProbeArrayGetNumProbes(pa)>0){ iplProbeBatchAddProbeArray(sp->probes,pa); ++np; }
        iplProbeArrayRelease(&pa);
    }
    if (np==0){ iplProbeBatchRelease(&sp->probes); sp->probes=NULL; return 0; }
    iplProbeBatchCommit(sp->probes);

    IPLPathBakeParams bp; memset(&bp,0,sizeof bp);
    bp.scene=sp->scene_ipl; bp.probeBatch=sp->probes;
    bp.identifier.type=IPL_BAKEDDATATYPE_PATHING; bp.identifier.variation=IPL_BAKEDDATAVARIATION_DYNAMIC;
    bp.numSamples=1; bp.radius=PATH_VIS_RADIUS; bp.threshold=PATH_VIS_THRESH;
    bp.visRange=PATH_VIS_RANGE; bp.pathRange=PATH_RANGE; bp.numThreads=2;
    steam_scene_ray_lock(sp->scene);                     /* shared, for the whole bake: probe-pair visibility
                                                          * ray-traces the scene and must not race a commit
                                                          * from the occlusion sim thread */
    iplPathBakerBake(sp->ctx, &bp, NULL, NULL);          /* BLOCKS: probe-pair visibility graph */
    steam_scene_ray_unlock(sp->scene);

    iplSimulatorAddProbeBatch(sp->sim, sp->probes);
    iplSimulatorCommit(sp->sim);
    fprintf(stderr, "bw_audio: baked pathing visibility at %d probes\n", np);
    return np;
}

/* (sim thread — or the debug seam with no sim thread running) create + add IPLSources for wanted
 * slots that lack one, then ONE iplSimulatorCommit. Runs on the same thread as RunPathing, which is
 * the point: phonon forbids committing while a simulation runs, so creation must not stay on the
 * control thread. Phonon calls happen outside `lock` (only the shadow snapshot is under it). */
static void reconcile_sources(SteamPath* sp) {
    int changed = 0;
    for (int i = 0; i < PATH_MAX_SRC; ++i) {
        EnterCriticalSection(&sp->lock);
        int need = (i < sp->nsrc) && sp->srcs[i].want && !sp->srcs[i].src;
        LeaveCriticalSection(&sp->lock);
        if (!need) continue;
        IPLSourceSettings ss; memset(&ss,0,sizeof ss); ss.flags = IPL_SIMULATIONFLAGS_PATHING;
        IPLSource src = NULL;
        if (iplSourceCreate(sp->sim, &ss, &src) == IPL_STATUS_SUCCESS) {
            iplSourceAdd(src, sp->sim);
            sp->srcs[i].src = src;      /* sim-thread-owned; the control thread never reads it */
            changed = 1;
        }
    }
    if (changed) iplSimulatorCommit(sp->sim);
}

/* set per-source pathing inputs + run + read outputs into eq/sh. `src` + `pos` are the caller's
 * snapshot (the sim is single-consumer: sim thread, or the test seam). 1 = a path carries energy. */
static int run_get(SteamPath* sp, const float listener[3], IPLSource src, const float pos[3],
                   float eq[3], float* sh) {
    IPLSimulationInputs in; memset(&in,0,sizeof in);
    in.flags = IPL_SIMULATIONFLAGS_PATHING;
    cs_at(&in.source, pos);
    in.pathingProbes = sp->probes;
    in.visRadius = PATH_VIS_RADIUS; in.visThreshold = PATH_VIS_THRESH; in.visRange = PATH_VIS_RANGE;
    in.pathingOrder = (IPLint32)sp->order;
    iplSourceSetInputs(src, IPL_SIMULATIONFLAGS_PATHING, &in);

    IPLSimulationSharedInputs sh_in; memset(&sh_in,0,sizeof sh_in);
    cs_at(&sh_in.listener, listener);
    iplSimulatorSetSharedInputs(sp->sim, IPL_SIMULATIONFLAGS_PATHING, &sh_in);

    steam_scene_ray_lock(sp->scene);             /* shared: RunPathing reads the scene BVH (direct-path
                                                  * visibility) — can't race a dynamic-mesh iplSceneCommit */
    iplSimulatorRunPathing(sp->sim);
    steam_scene_ray_unlock(sp->scene);

    IPLSimulationOutputs out; memset(&out,0,sizeof out);
    iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_PATHING, &out);
    for (int b=0;b<3;++b) eq[b] = out.pathing.eqCoeffs[b];
    if (out.pathing.shCoeffs) for (uint32_t k=0;k<sp->ambi_ch;++k) sh[k] = out.pathing.shCoeffs[k];
    else                      for (uint32_t k=0;k<sp->ambi_ch;++k) sh[k] = 0.f;
    return sh[0] > 1e-6f;
}

static int find_slot(SteamPath* sp, uint32_t handle) {
    for (int i=0;i<sp->nsrc;++i) if (sp->srcs[i].handle==handle) return i;
    return -1;
}

/* Normalize the raw bending-loss eqCoeffs to a pure spectral tilt (loudest band = 1), mirroring
 * phonon's EQEffect::normalizeGains / IPLPathEffectParams.normalizeEQ. The path LEVEL rides shCoeffs
 * (calcAmbisonicsCoeffsForPaths weights each path by its distance attenuation), so eqCoeffs must carry
 * only COLOR here — else the deviation gain would double-count against the level in shCoeffs. Floor at
 * phonon's kMaxEQGain so a single band can't cut more than ~-24 dB (guards over-aggressive filtering). */
#define PATH_EQ_MIN_GAIN 0.0625f
static void normalize_eq(float eq[3]) {
    float mx = 0.f;
    for (int b=0;b<3;++b) if (eq[b] > mx) mx = eq[b];
    if (mx < 1e-8f) { eq[0]=eq[1]=eq[2]=1.f; return; }         /* no path / silence -> flat */
    for (int b=0;b<3;++b) { float g = eq[b]/mx; eq[b] = g < PATH_EQ_MIN_GAIN ? PATH_EQ_MIN_GAIN : g; }
}

/* CONTROL thread: shadow-only (claim a slot, set pos/want under the lock) — no phonon calls, so it
 * is genuinely non-blocking and cannot race the sim thread's RunPathing. The IPLSource itself is
 * created by reconcile_sources on the sim thread (first tick after the claim; the sim publishes a
 * zero path until then, which is also what a just-enabled blocked source would render). */
void steam_path_set_source(SteamPath* sp, uint32_t handle, const float pos[3], int on) {
    if (!sp) return;
    EnterCriticalSection(&sp->lock);
    int slot = find_slot(sp, handle);
    if (slot < 0 && on && sp->nsrc < PATH_MAX_SRC) {
        slot = sp->nsrc++;
        sp->srcs[slot].handle = handle;            /* src stays NULL: the sim thread creates it */
    }
    if (slot >= 0) { sp->srcs[slot].pos[0]=pos[0]; sp->srcs[slot].pos[1]=pos[1]; sp->srcs[slot].pos[2]=pos[2]; sp->srcs[slot].want=(uint8_t)(on!=0); }
    LeaveCriticalSection(&sp->lock);
}

void steam_path_set_pos(SteamPath* sp, uint32_t handle, float x, float y, float z) {
    if (!sp) return;
    EnterCriticalSection(&sp->lock);
    int slot = find_slot(sp, handle);                  /* update only; enabling is steam_path_set_source's job */
    if (slot >= 0) { sp->srcs[slot].pos[0]=x; sp->srcs[slot].pos[1]=y; sp->srcs[slot].pos[2]=z; }
    LeaveCriticalSection(&sp->lock);
}

/* Test seam: ONLY valid with the sim thread not running (steam_path_start not called) — it
 * reconciles + runs the simulator inline, which would race a live sim thread. */
int steam_path_debug_run_get(SteamPath* sp, const float listener[3], uint32_t handle, float eq[3], float* sh) {
    if (!sp) return 0;
    reconcile_sources(sp);                       /* single-threaded here: create the claimed sources now */
    EnterCriticalSection(&sp->lock);
    int slot = find_slot(sp, handle);
    IPLSource src = (slot >= 0) ? sp->srcs[slot].src : NULL;
    float pos[3] = { 0, 0, 0 };
    if (slot >= 0) { pos[0]=sp->srcs[slot].pos[0]; pos[1]=sp->srcs[slot].pos[1]; pos[2]=sp->srcs[slot].pos[2]; }
    LeaveCriticalSection(&sp->lock);
    if (!src) return 0;
    return run_get(sp, listener, src, pos, eq, sh);
}

/* AUDIO thread (rt path tap): decode the summed indirect ambisonic field to the 26 speakers + sum. */
void steam_path_tap(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* ambi, uint32_t ambi_ch) {
    SteamPath* sp = (SteamPath*)ud; (void)lp; (void)lq;
    if (n != sp->n) return;
    if (ambi_ch > sp->ambi_ch) ambi_ch = sp->ambi_ch;
    float* ambP[BWA_AMBI_CH]; for (uint32_t k=0;k<ambi_ch;++k) ambP[k] = (float*)(ambi + (size_t)k * n);
    IPLAudioBuffer ab = { .numChannels = (IPLint32)ambi_ch, .numSamples = (IPLint32)n, .data = ambP };
    float* outP[BWA_CHANNELS]; for (uint32_t s=0;s<sp->channels;++s) outP[s] = sp->out26 + (size_t)s * n;
    IPLAudioBuffer o26 = { .numChannels = (IPLint32)sp->channels, .numSamples = (IPLint32)n, .data = outP };
    IPLAmbisonicsDecodeEffectParams dp; memset(&dp,0,sizeof dp);
    dp.order = (IPLint32)sp->order; dp.hrtf = NULL; dp.binaural = IPL_FALSE;
    cs_at(&dp.orientation, (float[3]){0,0,0});
    iplAmbisonicsDecodeEffectApply(sp->dec, &dp, &ab, &o26);
    for (uint32_t s=0;s<sp->channels;++s) for (uint32_t i=0;i<n;++i) bus[(size_t)s*n+i] += sp->out26[(size_t)s*n+i];
}

static DWORD WINAPI sim_thread(LPVOID arg) {
    SteamPath* sp = (SteamPath*)arg;
    BWA_THREAD_NAME("bw-sim (pathing)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   /* never preempt the audio callback */
    float eq[3], sh[BWA_AMBI_CH];
    while (!sp->stop) {
        float lp[3], lq[4]; rt_read_pose(sp->rt, lp, lq); (void)lq;
        reconcile_sources(sp);                       /* create + commit here, NEVER on the control thread */
        EnterCriticalSection(&sp->lock); int n = sp->nsrc; LeaveCriticalSection(&sp->lock);
        for (int i = 0; i < n; ++i) {
            EnterCriticalSection(&sp->lock);         /* snapshot the slot's shadow (no torn pos reads) */
            uint32_t handle = sp->srcs[i].handle; uint8_t want = sp->srcs[i].want;
            float pos[3] = { sp->srcs[i].pos[0], sp->srcs[i].pos[1], sp->srcs[i].pos[2] };
            LeaveCriticalSection(&sp->lock);
            IPLSource src = sp->srcs[i].src;         /* sim-thread-owned */
            if (!want || !src) { memset(sh, 0, sizeof(float)*sp->ambi_ch); rt_set_pathing(sp->rt, handle, sh, NULL, sp->ambi_ch); continue; }
            run_get(sp, lp, src, pos, eq, sh);       /* sh carries direction+level; eq is the bending-loss tilt */
            normalize_eq(eq);                        /* -> pure color (level already in sh); rt applies it pre-encode */
            rt_set_pathing(sp->rt, handle, sh, eq, sp->ambi_ch);
        }
        Sleep(1000 / PATH_HZ);
    }
    return 0;
}

void steam_path_start(SteamPath* sp) { if (sp && !sp->thread) sp->thread = CreateThread(NULL, 0, sim_thread, sp, 0, NULL); }

SteamPath* steam_path_create(SteamScene* scene, RtCore* rt, const Layout* L,
                             uint32_t sample_rate, uint32_t block, uint32_t order) {
    if (!scene || !rt || !L || order < 1 || order > 3) return NULL;
    SteamPath* sp = (SteamPath*)calloc(1, sizeof *sp);
    if (!sp) return NULL;
    sp->ctx = (IPLContext)steam_scene_ipl_context(scene);
    sp->scene_ipl = (IPLScene)steam_scene_ipl_scene(scene);
    sp->scene = scene;                           /* for the shared scene lock on the sim thread */
    sp->scene_type = (IPLSceneType)steam_scene_ipl_scenetype(scene);
    if (!sp->ctx || !sp->scene_ipl) { free(sp); return NULL; }
    sp->rt = rt; sp->n = block; sp->order = order; sp->ambi_ch = (order+1)*(order+1); sp->sample_rate = sample_rate;
    InitializeCriticalSection(&sp->lock);

    IPLSimulationSettings ss; memset(&ss,0,sizeof ss);
    ss.flags = IPL_SIMULATIONFLAGS_PATHING;
    ss.sceneType = sp->scene_type;
    ss.maxNumSources = PATH_MAX_SRC;
    ss.maxOrder = (IPLint32)order;             /* else the path field is capped to order 0 (omni, no direction) */
    ss.samplingRate = (IPLint32)sample_rate; ss.frameSize = (IPLint32)block;
    if (iplSimulatorCreate(sp->ctx, &ss, &sp->sim) != IPL_STATUS_SUCCESS) goto fail;
    iplSimulatorSetScene(sp->sim, sp->scene_ipl);
    iplSimulatorCommit(sp->sim);

    steam_scene_flush(scene);      /* the bake ray-traces NOW: commit any just-staged room first
                                    * (set_box -> start can beat the scene sim thread's next tick) */
    if (do_path_bake(sp, L) == 0) goto fail;

    sp->channels = L->count;                           /* the layout's speaker count (<= BWA_CHANNELS cap) */
    for (uint32_t s = 0; s < sp->channels; ++s) {      /* speaker dirs in phonon's cartesian frame (== room),
                                                        * from the layout's nominal listening point */
        float p[3] = { L->speakers[s].pos[0] - L->ref[0], L->speakers[s].pos[1] - L->ref[1],
                       L->speakers[s].pos[2] - L->ref[2] };
        float m = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        sp->spk[s] = (m < 1e-6f) ? (IPLVector3){0,0,-1} : (IPLVector3){ p[0]/m, p[1]/m, p[2]/m };
    }
    IPLAudioSettings as; memset(&as,0,sizeof as); as.samplingRate=(IPLint32)sample_rate; as.frameSize=(IPLint32)block;
    IPLAmbisonicsDecodeEffectSettings ds; memset(&ds,0,sizeof ds);
    ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_CUSTOM;
    ds.speakerLayout.numSpeakers = (IPLint32)sp->channels;
    ds.speakerLayout.speakers = sp->spk;
    ds.hrtf = NULL; ds.maxOrder = (IPLint32)order;
    if (iplAmbisonicsDecodeEffectCreate(sp->ctx, &as, &ds, &sp->dec) != IPL_STATUS_SUCCESS) goto fail;
    sp->out26 = (float*)calloc((size_t)sp->channels * block, sizeof(float));
    if (!sp->out26) goto fail;
    return sp;
fail:
    steam_path_destroy(sp);
    return NULL;
}

void steam_path_destroy(SteamPath* sp) {
    if (!sp) return;
    if (sp->thread) { InterlockedExchange(&sp->stop,1); WaitForSingleObject(sp->thread, INFINITE); CloseHandle(sp->thread); }
    if (sp->dec) iplAmbisonicsDecodeEffectRelease(&sp->dec);
    free(sp->out26);
    for (int i=0;i<sp->nsrc;++i) if (sp->srcs[i].src) { iplSourceRemove(sp->srcs[i].src, sp->sim); iplSourceRelease(&sp->srcs[i].src); }
    if (sp->probes) { iplSimulatorRemoveProbeBatch(sp->sim, sp->probes); iplProbeBatchRelease(&sp->probes); }
    if (sp->sim) iplSimulatorRelease(&sp->sim);
    DeleteCriticalSection(&sp->lock);
    free(sp);
}
