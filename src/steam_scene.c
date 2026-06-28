/*
 * steam_scene.c — materials occlusion, off-thread half. See steam_scene.h.
 *
 * Build-only-with-SDK (BWAUDIO_WITH_STEAMAUDIO). phonon's room/coord convention equals ours
 * (x=right, y=up, -z=ahead), so positions pass through unchanged. A dedicated sim thread owns all
 * phonon objects; the control thread only writes a locked shadow (mesh + per-source enable/pos),
 * and the audio thread only reads the published occlusion float (rt_set_occlusion).
 */
#include "steam_scene.h"

#include <phonon.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIM_HZ 30

struct SteamScene {
    IPLContext    context;
    IPLScene      scene;
    IPLSimulator  simulator;
    IPLStaticMesh mesh;            /* current committed geometry (sim-thread-owned; NULL = none) */
    RtCore*       rt;
    uint32_t      voice_cap;

    CRITICAL_SECTION lock;
    /* shadow: written by the control thread under lock, snapshotted by the sim thread */
    struct { uint32_t handle; float pos[3]; int enabled; } *shadow;   /* [voice_cap] by voice slot */
    int           mesh_dirty;
    IPLVector3*   pend_verts; int pend_nverts;     /* pending mesh (control thread) */
    IPLTriangle*  pend_tris;  int pend_ntris;
    IPLMaterial   pend_mat;

    IPLSource*    srcs;            /* [voice_cap], sim-thread-only IPLSource per slot (NULL = none) */
    IPLVector3*   mesh_verts;      /* kept alive for the committed mesh's lifetime */
    IPLTriangle*  mesh_tris;
    IPLint32*     mesh_mi;

    HANDLE        thread;
    volatile LONG stop;
};

static IPLVector3 vec3(const float p[3]) { IPLVector3 v = { p[0], p[1], p[2] }; return v; }
static void identity_cs(IPLCoordinateSpace3* cs, const float origin[3]) {
    cs->right = (IPLVector3){ 1, 0, 0 }; cs->up = (IPLVector3){ 0, 1, 0 };
    cs->ahead = (IPLVector3){ 0, 0, -1 }; cs->origin = vec3(origin);
}

/* (sim thread) rebuild the committed static mesh from the pending arrays */
static void apply_mesh(SteamScene* s, IPLVector3* verts, int nverts, IPLTriangle* tris, int ntris,
                       IPLMaterial mat) {
    if (s->mesh) { iplStaticMeshRemove(s->mesh, s->scene); iplStaticMeshRelease(&s->mesh); s->mesh = NULL; }
    free(s->mesh_verts); free(s->mesh_tris); free(s->mesh_mi);
    s->mesh_verts = verts; s->mesh_tris = tris;
    s->mesh_mi = (IPLint32*)calloc((size_t)ntris, sizeof(IPLint32));   /* all triangles -> material 0 */
    if (!s->mesh_mi) { free(verts); free(tris); s->mesh_verts = NULL; s->mesh_tris = NULL; return; }

    IPLStaticMeshSettings ms; memset(&ms, 0, sizeof ms);
    ms.numVertices = nverts; ms.numTriangles = ntris; ms.numMaterials = 1;
    /* iplStaticMeshCreate deep-copies the materials array synchronously, so the by-value `mat`
     * parameter (a stable local for the call's duration) suffices — no shared static needed. */
    ms.vertices = verts; ms.triangles = tris; ms.materialIndices = s->mesh_mi; ms.materials = &mat;
    if (iplStaticMeshCreate(s->scene, &ms, &s->mesh) == IPL_STATUS_SUCCESS) {
        iplStaticMeshAdd(s->mesh, s->scene);
        iplSceneCommit(s->scene);
    }
}

static DWORD WINAPI sim_thread(LPVOID arg) {
    SteamScene* s = (SteamScene*)arg;
    const uint32_t cap = s->voice_cap;
    uint32_t* snap_h  = (uint32_t*)malloc(cap * sizeof(uint32_t));
    float*    snap_p  = (float*)malloc(cap * 3 * sizeof(float));
    int*      snap_en = (int*)malloc(cap * sizeof(int));
    if (!snap_h || !snap_p || !snap_en) { free(snap_h); free(snap_p); free(snap_en); return 0; }

    while (!s->stop) {
        /* 1. snapshot the shadow + take the pending mesh */
        IPLVector3* verts = NULL; IPLTriangle* tris = NULL; int nverts = 0, ntris = 0; IPLMaterial mat;
        memset(&mat, 0, sizeof mat);
        EnterCriticalSection(&s->lock);
        if (s->mesh_dirty) {
            s->mesh_dirty = 0;
            verts = s->pend_verts; tris = s->pend_tris; nverts = s->pend_nverts; ntris = s->pend_ntris; mat = s->pend_mat;
            s->pend_verts = NULL; s->pend_tris = NULL;     /* ownership moves to the sim thread */
        }
        for (uint32_t i = 0; i < cap; ++i) {
            snap_h[i]  = s->shadow[i].handle;
            snap_en[i] = s->shadow[i].enabled;
            snap_p[i*3+0] = s->shadow[i].pos[0]; snap_p[i*3+1] = s->shadow[i].pos[1]; snap_p[i*3+2] = s->shadow[i].pos[2];
        }
        LeaveCriticalSection(&s->lock);

        /* 2. apply geometry change */
        if (verts && tris) apply_mesh(s, verts, nverts, tris, ntris, mat);

        /* 3. reconcile IPLSources against the snapshot */
        int changed = 0;
        for (uint32_t i = 0; i < cap; ++i) {
            if (snap_en[i] && !s->srcs[i]) {
                IPLSourceSettings ss; memset(&ss, 0, sizeof ss); ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
                IPLSource src = NULL;
                if (iplSourceCreate(s->simulator, &ss, &src) == IPL_STATUS_SUCCESS) {
                    iplSourceAdd(src, s->simulator); s->srcs[i] = src; changed = 1;
                }
            } else if (!snap_en[i] && s->srcs[i]) {
                iplSourceRemove(s->srcs[i], s->simulator); iplSourceRelease(&s->srcs[i]); s->srcs[i] = NULL;
                changed = 1;
                rt_set_occlusion(s->rt, snap_h[i], 1.0f);   /* clear on disable, so it doesn't stay attenuated */
            }
        }
        if (changed) iplSimulatorCommit(s->simulator);

        /* 4. shared (listener) + per-source inputs */
        float lp[3], lq[4]; rt_read_pose(s->rt, lp, lq); (void)lq;
        IPLSimulationSharedInputs shared; memset(&shared, 0, sizeof shared);
        identity_cs(&shared.listener, lp);
        iplSimulatorSetSharedInputs(s->simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);

        for (uint32_t i = 0; i < cap; ++i) {
            if (!s->srcs[i]) continue;
            IPLSimulationInputs in; memset(&in, 0, sizeof in);
            in.flags = IPL_SIMULATIONFLAGS_DIRECT;
            /* occlusion (geometry) + transmission (material), so a glass wall and a concrete wall
             * differ — without TRANSMISSION the material coefficients would be dead inputs. */
            in.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION;
            identity_cs(&in.source, &snap_p[i*3]);
            in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;   /* smooth partial occlusion */
            in.occlusionRadius = 0.3f;
            in.numOcclusionSamples = 16;
            in.numTransmissionRays = 1;                        /* single occluding surface (our wall) */
            iplSourceSetInputs(s->srcs[i], IPL_SIMULATIONFLAGS_DIRECT, &in);
        }

        /* 5. run + publish a broadband level + a 3-band transmission tilt (the spectral shape of the
         * occluded sound). No geometry => clear, so a removed/failed mesh never leaves a voice stuck
         * attenuated. Per band: raw = occlusion + (1-occlusion)*transmission[b]; the broadband LEVEL
         * is max(raw) and the normalized tilt raw/max (floored) is the EQ — matching Steam Audio's
         * own split (tilt in the EQ, level in the scalar). */
        int have_mesh = (s->mesh != NULL);
        if (have_mesh) iplSimulatorRunDirect(s->simulator);
        for (uint32_t i = 0; i < cap; ++i) {
            if (!s->srcs[i]) continue;
            if (!have_mesh) { rt_set_occlusion(s->rt, snap_h[i], 1.0f); continue; }   /* clear: flat EQ */
            IPLSimulationOutputs out; memset(&out, 0, sizeof out);
            iplSourceGetOutputs(s->srcs[i], IPL_SIMULATIONFLAGS_DIRECT, &out);
            const IPLDirectEffectParams* d = &out.direct;
            float raw[3], maxg = 0.f;
            for (int b = 0; b < 3; ++b) {
                raw[b] = d->occlusion + (1.0f - d->occlusion) * d->transmission[b];
                if (raw[b] > maxg) maxg = raw[b];
            }
            if (maxg <= 1e-6f) {
                const float flat[3] = { 1.f, 1.f, 1.f };
                rt_set_occlusion_eq(s->rt, snap_h[i], 0.0f, flat);     /* fully blocked: silence via level */
            } else {
                float g[3];
                for (int b = 0; b < 3; ++b) { g[b] = raw[b] / maxg; if (g[b] < 0.0625f) g[b] = 0.0625f; }
                rt_set_occlusion_eq(s->rt, snap_h[i], maxg, g);
            }
        }

        Sleep(1000 / SIM_HZ);
    }
    free(snap_h); free(snap_p); free(snap_en);
    return 0;
}

SteamScene* steam_scene_create(RtCore* rt, uint32_t sample_rate, uint32_t frame_size, uint32_t voice_cap) {
    if (!rt || voice_cap == 0) return NULL;
    SteamScene* s = (SteamScene*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rt = rt; s->voice_cap = voice_cap;
    s->shadow = calloc(voice_cap, sizeof *s->shadow);
    s->srcs   = calloc(voice_cap, sizeof *s->srcs);
    if (!s->shadow || !s->srcs) { free(s->shadow); free(s->srcs); free(s); return NULL; }
    InitializeCriticalSection(&s->lock);

    IPLContextSettings cs; memset(&cs, 0, sizeof cs); cs.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&cs, &s->context) != IPL_STATUS_SUCCESS) goto fail;

    IPLSceneSettings sc; memset(&sc, 0, sizeof sc); sc.type = IPL_SCENETYPE_DEFAULT;
    if (iplSceneCreate(s->context, &sc, &s->scene) != IPL_STATUS_SUCCESS) goto fail;

    IPLSimulationSettings ss; memset(&ss, 0, sizeof ss);
    ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
    ss.sceneType = IPL_SCENETYPE_DEFAULT;
    ss.maxNumOcclusionSamples = 32;
    ss.maxNumSources = (IPLint32)voice_cap;
    ss.samplingRate = (IPLint32)sample_rate;
    ss.frameSize = (IPLint32)frame_size;
    if (iplSimulatorCreate(s->context, &ss, &s->simulator) != IPL_STATUS_SUCCESS) goto fail;
    iplSimulatorSetScene(s->simulator, s->scene);
    iplSimulatorCommit(s->simulator);

    s->thread = CreateThread(NULL, 0, sim_thread, s, 0, NULL);
    if (!s->thread) goto fail;
    return s;

fail:
    steam_scene_destroy(s);
    return NULL;
}

void steam_scene_set_mesh(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                          const float absorption[3], float scattering, const float transmission[3]) {
    if (!s || nverts <= 0 || ntris <= 0) return;
    IPLVector3*  v = (IPLVector3*)malloc((size_t)nverts * sizeof(IPLVector3));
    IPLTriangle* t = (IPLTriangle*)malloc((size_t)ntris * sizeof(IPLTriangle));
    if (!v || !t) { free(v); free(t); return; }
    for (int i = 0; i < nverts; ++i) v[i] = (IPLVector3){ verts[i*3+0], verts[i*3+1], verts[i*3+2] };
    for (int i = 0; i < ntris;  ++i) { t[i].indices[0] = tris[i*3+0]; t[i].indices[1] = tris[i*3+1]; t[i].indices[2] = tris[i*3+2]; }
    IPLMaterial m; memset(&m, 0, sizeof m);
    for (int b = 0; b < 3; ++b) { m.absorption[b] = absorption[b]; m.transmission[b] = transmission[b]; }
    m.scattering = scattering;

    EnterCriticalSection(&s->lock);
    free(s->pend_verts); free(s->pend_tris);   /* drop any un-consumed prior pending mesh */
    s->pend_verts = v; s->pend_nverts = nverts;
    s->pend_tris = t;  s->pend_ntris = ntris;
    s->pend_mat = m; s->mesh_dirty = 1;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_occlusion(SteamScene* s, uint32_t handle, int on) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    s->shadow[idx].handle  = handle;
    s->shadow[idx].enabled = on ? 1 : 0;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_pos(SteamScene* s, uint32_t handle, float x, float y, float z) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    /* Reconcile the slot to the calling occupant: if the handle changed (slot reused, or this is the
     * first call before set_occlusion), adopt it and clear `enabled` — the new occupant hasn't asked
     * for occlusion. Then always record the position, so a set_pos-before-set_occlusion ordering
     * still lands the position (previously it was silently dropped, simulating at the origin). */
    if (s->shadow[idx].handle != handle) {
        s->shadow[idx].handle  = handle;
        s->shadow[idx].enabled = 0;
    }
    s->shadow[idx].pos[0] = x; s->shadow[idx].pos[1] = y; s->shadow[idx].pos[2] = z;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_destroy(SteamScene* s) {
    if (!s) return;
    if (s->thread) { InterlockedExchange(&s->stop, 1); WaitForSingleObject(s->thread, INFINITE); CloseHandle(s->thread); }
    for (uint32_t i = 0; i < s->voice_cap; ++i)
        if (s->srcs && s->srcs[i]) { iplSourceRemove(s->srcs[i], s->simulator); iplSourceRelease(&s->srcs[i]); }
    if (s->mesh)      iplStaticMeshRelease(&s->mesh);
    if (s->simulator) iplSimulatorRelease(&s->simulator);
    if (s->scene)     iplSceneRelease(&s->scene);
    if (s->context)   iplContextRelease(&s->context);
    DeleteCriticalSection(&s->lock);
    free(s->mesh_verts); free(s->mesh_tris); free(s->mesh_mi);
    free(s->pend_verts); free(s->pend_tris);
    free(s->shadow); free(s->srcs);
    free(s);
}
