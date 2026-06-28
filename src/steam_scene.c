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
    /* shadow: written by the control thread under lock, snapshotted by the sim thread. `features` is
     * a per-slot bitmask (occlusion and/or directivity) so a source can be directional WITHOUT being
     * occluded; an IPLSource exists iff features != 0. fwd is the source's forward axis (unit). */
    struct { uint32_t handle; float pos[3]; uint8_t features;
             float dir_weight, dir_power, fwd[3]; } *shadow;          /* [voice_cap] by voice slot */
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

enum { FEAT_OCC = 1, FEAT_DIR = 2 };

static IPLVector3 vec3(const float p[3]) { IPLVector3 v = { p[0], p[1], p[2] }; return v; }
static void identity_cs(IPLCoordinateSpace3* cs, const float origin[3]) {
    cs->right = (IPLVector3){ 1, 0, 0 }; cs->up = (IPLVector3){ 0, 1, 0 };
    cs->ahead = (IPLVector3){ 0, 0, -1 }; cs->origin = vec3(origin);
}

/* Build an orthonormal source frame whose `ahead` is `fwd` (the dipole axis); right/up are
 * synthesized (the weighted-dipole pattern is rotationally symmetric about ahead, so they only need
 * to be orthonormal). origin is the source position. */
static void oriented_cs(IPLCoordinateSpace3* cs, const float origin[3], const float fwd[3]) {
    float ax = fwd[0], ay = fwd[1], az = fwd[2];
    float m = sqrtf(ax*ax + ay*ay + az*az);
    if (m < 1e-6f) { identity_cs(cs, origin); return; }
    ax /= m; ay /= m; az /= m;
    float ux = 0.f, uy = 1.f, uz = 0.f;                  /* reference up; swap if ahead is ~vertical */
    if (fabsf(ay) > 0.99f) { ux = 0.f; uy = 0.f; uz = 1.f; }
    float rx = uy*az - uz*ay, ry = uz*ax - ux*az, rz = ux*ay - uy*ax;   /* right = up x ahead */
    float rm = sqrtf(rx*rx + ry*ry + rz*rz); rx /= rm; ry /= rm; rz /= rm;
    float vx = ay*rz - az*ry, vy = az*rx - ax*rz, vz = ax*ry - ay*rx;   /* up = ahead x right */
    cs->right = (IPLVector3){ rx, ry, rz }; cs->up = (IPLVector3){ vx, vy, vz };
    cs->ahead = (IPLVector3){ ax, ay, az }; cs->origin = vec3(origin);
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
    uint32_t* snap_h    = (uint32_t*)malloc(cap * sizeof(uint32_t));
    float*    snap_p    = (float*)malloc(cap * 3 * sizeof(float));
    uint8_t*  snap_feat = (uint8_t*)malloc(cap * sizeof(uint8_t));
    float*    snap_dw   = (float*)malloc(cap * sizeof(float));        /* directivity weight */
    float*    snap_dp   = (float*)malloc(cap * sizeof(float));        /* directivity power */
    float*    snap_fwd  = (float*)malloc(cap * 3 * sizeof(float));    /* source forward axis */
    if (!snap_h || !snap_p || !snap_feat || !snap_dw || !snap_dp || !snap_fwd) {
        free(snap_h); free(snap_p); free(snap_feat); free(snap_dw); free(snap_dp); free(snap_fwd); return 0;
    }

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
            snap_h[i]    = s->shadow[i].handle;
            snap_feat[i] = s->shadow[i].features;
            snap_dw[i]   = s->shadow[i].dir_weight;
            snap_dp[i]   = s->shadow[i].dir_power;
            snap_p[i*3+0]   = s->shadow[i].pos[0]; snap_p[i*3+1]   = s->shadow[i].pos[1]; snap_p[i*3+2]   = s->shadow[i].pos[2];
            snap_fwd[i*3+0] = s->shadow[i].fwd[0]; snap_fwd[i*3+1] = s->shadow[i].fwd[1]; snap_fwd[i*3+2] = s->shadow[i].fwd[2];
        }
        LeaveCriticalSection(&s->lock);

        /* 2. apply geometry change */
        if (verts && tris) apply_mesh(s, verts, nverts, tris, ntris, mat);

        /* 3. reconcile IPLSources against the snapshot: a source exists iff it has any feature. */
        int changed = 0;
        for (uint32_t i = 0; i < cap; ++i) {
            if (snap_feat[i] && !s->srcs[i]) {
                IPLSourceSettings ss; memset(&ss, 0, sizeof ss); ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
                IPLSource src = NULL;
                if (iplSourceCreate(s->simulator, &ss, &src) == IPL_STATUS_SUCCESS) {
                    iplSourceAdd(src, s->simulator); s->srcs[i] = src; changed = 1;
                }
            } else if (!snap_feat[i] && s->srcs[i]) {
                iplSourceRemove(s->srcs[i], s->simulator); iplSourceRelease(&s->srcs[i]); s->srcs[i] = NULL;
                changed = 1;
                rt_set_occlusion(s->rt, snap_h[i], 1.0f);   /* clear on disable, so it doesn't stay attenuated */
            }
        }
        if (changed) iplSimulatorCommit(s->simulator);

        /* 4. shared (listener) + per-source inputs. directFlags follow each source's feature mask:
         * occlusion+transmission (needs geometry) and/or directivity (orientation only). */
        float lp[3], lq[4]; rt_read_pose(s->rt, lp, lq); (void)lq;
        IPLSimulationSharedInputs shared; memset(&shared, 0, sizeof shared);
        identity_cs(&shared.listener, lp);
        iplSimulatorSetSharedInputs(s->simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);

        for (uint32_t i = 0; i < cap; ++i) {
            if (!s->srcs[i]) continue;
            IPLSimulationInputs in; memset(&in, 0, sizeof in);
            in.flags = IPL_SIMULATIONFLAGS_DIRECT;
            int df = 0;
            if (snap_feat[i] & FEAT_OCC) df |= IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION;
            if (snap_feat[i] & FEAT_DIR) df |= IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY;
            in.directFlags = (IPLDirectSimulationFlags)df;
            oriented_cs(&in.source, &snap_p[i*3], &snap_fwd[i*3]);   /* dipole axis = the source forward */
            in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;   /* smooth partial occlusion */
            in.occlusionRadius = 0.3f;
            in.numOcclusionSamples = 16;
            in.numTransmissionRays = 1;                        /* single occluding surface (our wall) */
            if (snap_feat[i] & FEAT_DIR) { in.directivity.dipoleWeight = snap_dw[i]; in.directivity.dipolePower = snap_dp[i]; }
            iplSourceSetInputs(s->srcs[i], IPL_SIMULATIONFLAGS_DIRECT, &in);
        }

        /* 5. run + publish each source's (level, 3-band tilt, directivity). Direct sim computes
         * occlusion (clear if no geometry) and directivity (orientation only) — run it whenever a
         * source is active. Occlusion split (matching Steam Audio): raw = occlusion +
         * (1-occlusion)*transmission[b]; the broadband LEVEL is max(raw), the normalized tilt raw/max
         * (floored) is the EQ. Each output is read only for the features that asked for it. */
        int any_src = 0;
        for (uint32_t i = 0; i < cap; ++i) if (s->srcs[i]) { any_src = 1; break; }
        if (any_src) iplSimulatorRunDirect(s->simulator);
        for (uint32_t i = 0; i < cap; ++i) {
            if (!s->srcs[i]) continue;
            IPLSimulationOutputs out; memset(&out, 0, sizeof out);
            iplSourceGetOutputs(s->srcs[i], IPL_SIMULATIONFLAGS_DIRECT, &out);
            const IPLDirectEffectParams* d = &out.direct;
            float level = 1.f, bands[3] = { 1.f, 1.f, 1.f }, dir = 1.f;
            if (snap_feat[i] & FEAT_OCC) {
                float raw[3], maxg = 0.f;
                for (int b = 0; b < 3; ++b) {
                    raw[b] = d->occlusion + (1.0f - d->occlusion) * d->transmission[b];
                    if (raw[b] > maxg) maxg = raw[b];
                }
                if (maxg <= 1e-6f) level = 0.f;          /* fully blocked: silence via level, flat bands */
                else { level = maxg; for (int b = 0; b < 3; ++b) { bands[b] = raw[b] / maxg; if (bands[b] < 0.0625f) bands[b] = 0.0625f; } }
            }
            if (snap_feat[i] & FEAT_DIR) dir = d->directivity;
            rt_set_direct(s->rt, snap_h[i], level, bands, dir);
        }

        Sleep(1000 / SIM_HZ);
    }
    free(snap_h); free(snap_p); free(snap_feat); free(snap_dw); free(snap_dp); free(snap_fwd);
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

/* (caller holds s->lock) adopt `handle` as the slot's occupant; if it changed (slot reused, or first
 * touch before any feature was set) reset its features/params so a new occupant never inherits the
 * prior one's occlusion/directivity. This is what makes set_pos-before-set_occlusion land correctly
 * and a recycled slot start clean. */
static void shadow_adopt(SteamScene* s, uint16_t idx, uint32_t handle) {
    if (s->shadow[idx].handle != handle) {
        s->shadow[idx].handle     = handle;
        s->shadow[idx].features   = 0;
        s->shadow[idx].dir_weight = 0.f; s->shadow[idx].dir_power = 1.f;
        s->shadow[idx].fwd[0] = 0.f; s->shadow[idx].fwd[1] = 0.f; s->shadow[idx].fwd[2] = -1.f;  /* default ahead = -z */
        s->shadow[idx].pos[0] = 0.f; s->shadow[idx].pos[1] = 0.f; s->shadow[idx].pos[2] = 0.f;   /* origin until set_pos lands */
    }
}

void steam_scene_set_occlusion(SteamScene* s, uint32_t handle, int on) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    if (on) s->shadow[idx].features |= FEAT_OCC; else s->shadow[idx].features &= (uint8_t)~FEAT_OCC;
    LeaveCriticalSection(&s->lock);
}

/* weight 0 = omni (no directivity); 0.5 = cardioid; 1 = figure-8. power >= 1 sharpens the lobe. */
void steam_scene_set_directivity(SteamScene* s, uint32_t handle, float weight, float power) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    if (weight < 0.f) weight = 0.f; if (weight > 1.f) weight = 1.f;
    if (power  < 1.f) power  = 1.f;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    if (weight > 0.f) { s->shadow[idx].features |= FEAT_DIR; s->shadow[idx].dir_weight = weight; s->shadow[idx].dir_power = power; }
    else              { s->shadow[idx].features &= (uint8_t)~FEAT_DIR; }
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_orientation(SteamScene* s, uint32_t handle, float fx, float fy, float fz) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    s->shadow[idx].fwd[0] = fx; s->shadow[idx].fwd[1] = fy; s->shadow[idx].fwd[2] = fz;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_pos(SteamScene* s, uint32_t handle, float x, float y, float z) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    s->shadow[idx].pos[0] = x; s->shadow[idx].pos[1] = y; s->shadow[idx].pos[2] = z;
    LeaveCriticalSection(&s->lock);
}

/* On bw_source_destroy: clear ALL features so the sim tears the IPLSource down and stops simulating
 * the slot (else a recycled slot inherits stale state). */
void steam_scene_source_gone(SteamScene* s, uint32_t handle) {
    if (!s) return;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    if (s->shadow[idx].handle == handle) s->shadow[idx].features = 0;
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
