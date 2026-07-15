/*
 * steam_scene.c — materials occlusion, off-thread half. See steam_scene.h.
 *
 * Build-only-with-SDK (BWA_WITH_STEAMAUDIO). phonon's room/coord convention equals ours
 * (x=right, y=up, -z=ahead), so positions pass through unchanged. A dedicated sim thread owns all
 * phonon objects; the control thread only writes a locked shadow (mesh + per-source enable/pos),
 * and the audio thread only reads the published occlusion float (rt_set_occlusion).
 */
#include "steam_scene.h"
#include "frame.h"         /* BWA_ROOM_AHEAD (default source forward) */
#include "profile.h"

#include <phonon.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_HZ 30
#define BWA_MAX_DYN_MESH 32       /* movable-occluder capacity (IPLInstancedMesh slots) */

/* One movable occluder/reflector: a sub-scene (its own geometry + BVH, built once) placed in the main
 * scene by an instanced mesh with a rigid transform. Moving it is a cheap main-scene BVH refit, not a
 * geometry rebuild. All phonon objects here are sim-thread-owned; the control thread only writes the
 * shadow (dyn_shadow) below. */
typedef struct {
    IPLScene         sub;        /* OWNED sub-scene */
    IPLStaticMesh    sub_mesh;   /* OWNED mesh in the sub-scene */
    IPLInstancedMesh inst;       /* OWNED instance in the main scene */
    IPLVector3*      verts;      /* kept alive for sub_mesh's lifetime (phonon may reference, not copy) */
    IPLTriangle*     tris;
    IPLint32*        mi;
    IPLMaterial*     mat;
    int              live;       /* instance built + added to the main scene */
} DynMesh;

struct SteamScene {
    IPLContext    context;
    IPLScene      scene;
    IPLSimulator  simulator;
    IPLStaticMesh mesh;            /* current committed geometry (sim-thread-owned; NULL = none) */
    IPLEmbreeDevice embree;        /* Embree ray tracer (NULL unless opted in + available); released last */
    IPLSceneType  scene_type;      /* DEFAULT or EMBREE; the reflection sim must match it (shares this scene) */
    RtCore*       rt;
    uint32_t      voice_cap;

    SRWLOCK       scene_srw;       /* guards the COMMITTED scene: exclusive around iplSceneCommit (this
                                    * thread), shared around a borrowing sim's ray trace (steam_scene_ray_lock) */
    CRITICAL_SECTION lock;
    /* shadow: written by the control thread under lock, snapshotted by the sim thread. `features` is
     * a per-slot bitmask (occlusion and/or directivity) so a source can be directional WITHOUT being
     * occluded; an IPLSource exists iff features != 0. fwd is the source's forward axis (unit). */
    struct { uint32_t handle; float pos[3]; uint8_t features;
             float dir_weight, dir_power, fwd[3]; } *shadow;          /* [voice_cap] by voice slot */
    int           mesh_dirty;
    IPLVector3*   pend_verts; int pend_nverts;     /* pending mesh (control thread) */
    IPLTriangle*  pend_tris;  int pend_ntris;
    IPLMaterial*  pend_mats;   int pend_nmat;       /* pending materials + per-triangle index */
    IPLint32*     pend_tri_mat;

    IPLSource*    srcs;            /* [voice_cap], sim-thread-only IPLSource per slot (NULL = none) */
    IPLVector3*   mesh_verts;      /* kept alive for the committed mesh's lifetime */
    IPLTriangle*  mesh_tris;
    IPLint32*     mesh_mi;
    IPLMaterial*  mesh_mats;

    /* dynamic (instanced) movers. dyn is sim-thread-only (the phonon objects); dyn_shadow is the
     * control thread's DESIRED state, reconciled by the sim thread each tick (like the IPLSources). */
    DynMesh*      dyn;             /* [BWA_MAX_DYN_MESH] */
    struct { int want;            /* control thread: 1 = this mover should exist */
             int live_ack;        /* sim thread: phonon objects currently exist for this slot */
             IPLVector3*  verts; int nverts;    /* pending-add geometry (heap; ownership -> sim on build) */
             IPLTriangle* tris;  int ntris;
             IPLMaterial  mat;
             IPLMatrix4x4 xform; int xform_dirty; } *dyn_shadow;   /* [BWA_MAX_DYN_MESH], under `lock` */

    HANDLE        thread;
    volatile LONG stop;
};

static IPLMatrix4x4 mat_identity(void) {
    IPLMatrix4x4 m; memset(&m, 0, sizeof m);
    m.elements[0][0] = m.elements[1][1] = m.elements[2][2] = m.elements[3][3] = 1.f;
    return m;
}

enum { FEAT_OCC = 1, FEAT_DIR = 2 };
#define EQ_BAND_FLOOR 0.0625f   /* -24 dB per-band floor (matches Steam's normalizeGains): the broadband
                                 * attenuation rides the level scalar, so the EQ tilt never fully silences a band */

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

/* Commit the MAIN scene under the exclusive scene lock, so the commit's BVH rebuild can't race a
 * borrowing sim's ray trace (steam_scene_ray_lock holds it shared). The whole mutation BATCH — the
 * iplStaticMesh/InstancedMesh add/remove/update calls AND the iplSceneCommit that applies them — is
 * held exclusive as one region, so a reader can never observe a partially-mutated scene (phonon queues
 * the add/remove/update and applies them at commit, but we don't rely on that being lock-free vs a
 * concurrent trace). Held only while there is real work, never on an idle tick. */
static void scene_w_lock(SteamScene* s)   { AcquireSRWLockExclusive(&s->scene_srw); }
static void scene_w_unlock(SteamScene* s) { ReleaseSRWLockExclusive(&s->scene_srw); }

/* (sim thread) rebuild the committed static mesh from the pending arrays */
/* Adopt the pending mesh buffers (verts/tris/tri_mat/mats, all heap, ownership transferred) as the
 * committed geometry. iplStaticMeshCreate reads them all during the call; we keep them alive for the
 * mesh's lifetime (freed on the next apply_mesh / destroy) to stay robust to phonon referencing
 * rather than copying. The whole remove/create/add/commit runs under the exclusive scene lock (a rare
 * event — a full static-mesh swap — so holding the lock across the BVH build is fine). */
static void apply_mesh(SteamScene* s, IPLVector3* verts, int nverts, IPLTriangle* tris, int ntris,
                       IPLMaterial* mats, int nmat, IPLint32* tri_mat) {
    scene_w_lock(s);
    if (s->mesh) { iplStaticMeshRemove(s->mesh, s->scene); iplStaticMeshRelease(&s->mesh); s->mesh = NULL; }
    free(s->mesh_verts); free(s->mesh_tris); free(s->mesh_mi); free(s->mesh_mats);
    s->mesh_verts = verts; s->mesh_tris = tris; s->mesh_mi = tri_mat; s->mesh_mats = mats;

    IPLStaticMeshSettings ms; memset(&ms, 0, sizeof ms);
    ms.numVertices = nverts; ms.numTriangles = ntris; ms.numMaterials = nmat;
    ms.vertices = verts; ms.triangles = tris; ms.materialIndices = tri_mat; ms.materials = mats;
    if (iplStaticMeshCreate(s->scene, &ms, &s->mesh) == IPL_STATUS_SUCCESS) {
        iplStaticMeshAdd(s->mesh, s->scene);
        iplSceneCommit(s->scene);
    }
    scene_w_unlock(s);
}

/* (sim thread) Build slot d's sub-scene + mesh + instance and ADD the instance to the main scene
 * (queued — the caller commits the main scene once for the whole batch). Takes ownership of verts/tris
 * on success; leaves them to the caller on failure. Returns 1 on success. */
static int build_dyn(SteamScene* s, DynMesh* d, IPLVector3* verts, int nverts, IPLTriangle* tris, int ntris,
                     const IPLMaterial* mat, const IPLMatrix4x4* xform) {
    memset(d, 0, sizeof *d);
    IPLMaterial* m  = (IPLMaterial*)malloc(sizeof(IPLMaterial));
    IPLint32*    mi = (IPLint32*)calloc((size_t)ntris, sizeof(IPLint32));   /* all triangles -> material 0 */
    if (!m || !mi) { free(m); free(mi); return 0; }
    *m = *mat;

    IPLSceneSettings sc; memset(&sc, 0, sizeof sc); sc.type = s->scene_type; sc.embreeDevice = s->embree;
    if (iplSceneCreate(s->context, &sc, &d->sub) != IPL_STATUS_SUCCESS) { free(m); free(mi); return 0; }

    IPLStaticMeshSettings ms; memset(&ms, 0, sizeof ms);
    ms.numVertices = nverts; ms.numTriangles = ntris; ms.numMaterials = 1;
    ms.vertices = verts; ms.triangles = tris; ms.materialIndices = mi; ms.materials = m;
    if (iplStaticMeshCreate(d->sub, &ms, &d->sub_mesh) != IPL_STATUS_SUCCESS) {
        iplSceneRelease(&d->sub); free(m); free(mi); return 0;
    }
    iplStaticMeshAdd(d->sub_mesh, d->sub);
    iplSceneCommit(d->sub);                 /* sub-scene isn't reachable by readers yet: no lock */

    IPLInstancedMeshSettings is; memset(&is, 0, sizeof is);
    is.subScene = d->sub; is.transform = *xform;
    if (iplInstancedMeshCreate(s->scene, &is, &d->inst) != IPL_STATUS_SUCCESS) {
        iplStaticMeshRelease(&d->sub_mesh); iplSceneRelease(&d->sub); free(m); free(mi); return 0;
    }
    iplInstancedMeshAdd(d->inst, s->scene);  /* queued; caller commits the main scene */
    d->verts = verts; d->tris = tris; d->mi = mi; d->mat = m; d->live = 1;
    return 1;
}

/* (sim thread) release a torn-down slot's phonon objects + geometry. MUST run AFTER the main-scene
 * commit that removed the instance (the instance references the sub-scene until then). */
static void release_dyn(DynMesh* d) {
    if (d->inst)     iplInstancedMeshRelease(&d->inst);
    if (d->sub_mesh) iplStaticMeshRelease(&d->sub_mesh);
    if (d->sub)      iplSceneRelease(&d->sub);
    free(d->verts); free(d->tris); free(d->mi); free(d->mat);
    memset(d, 0, sizeof *d);
}

/* (sim thread) reconcile the instanced movers to the control thread's shadow: build pending adds,
 * apply transform updates, tear down removes — all queued, then ONE main-scene commit, then release
 * torn-down slots' objects (after the commit that unlinked them). */
static void reconcile_dynamic(SteamScene* s) {
    /* idle fast-path: one lock + a cheap scan. The vast majority of installs never add a dynamic mesh,
     * so skip the whole per-slot loop (and never touch the exclusive scene lock) when nothing is live. */
    EnterCriticalSection(&s->lock);
    int any = 0;
    for (int i = 0; i < BWA_MAX_DYN_MESH; ++i)
        if (s->dyn_shadow[i].want || s->dyn_shadow[i].live_ack || s->dyn_shadow[i].verts) { any = 1; break; }
    LeaveCriticalSection(&s->lock);
    if (!any) return;

    char torn[BWA_MAX_DYN_MESH]; memset(torn, 0, sizeof torn);
    int main_dirty = 0, excl = 0;   /* excl: acquired lazily before the first main-scene mutation, held to commit */
    for (int i = 0; i < BWA_MAX_DYN_MESH; ++i) {
        /* snapshot this slot's desire under the lock; take ownership of pending-add geometry */
        EnterCriticalSection(&s->lock);
        int want = s->dyn_shadow[i].want, ack = s->dyn_shadow[i].live_ack;
        int have_geo = (s->dyn_shadow[i].verts != NULL);
        IPLMatrix4x4 xform = s->dyn_shadow[i].xform; int xdirty = s->dyn_shadow[i].xform_dirty;
        IPLVector3* bv = NULL; IPLTriangle* bt = NULL; int bnv = 0, bnt = 0; IPLMaterial bm; memset(&bm, 0, sizeof bm);
        int op = 0;   /* 1=build, 2=update, 3=teardown, 4=free-unbuilt */
        if (want && !ack && have_geo) {
            op = 1; bv = s->dyn_shadow[i].verts; bnv = s->dyn_shadow[i].nverts;
            bt = s->dyn_shadow[i].tris; bnt = s->dyn_shadow[i].ntris; bm = s->dyn_shadow[i].mat;
            s->dyn_shadow[i].verts = NULL; s->dyn_shadow[i].tris = NULL;   /* ownership -> sim */
            s->dyn_shadow[i].xform_dirty = 0;
        } else if (!want && ack) {
            op = 3;
        } else if (!want && !ack && have_geo) {                            /* added then removed before any build */
            op = 4; bv = s->dyn_shadow[i].verts; bt = s->dyn_shadow[i].tris;
            s->dyn_shadow[i].verts = NULL; s->dyn_shadow[i].tris = NULL;
        } else if (want && ack && xdirty) {
            op = 2; s->dyn_shadow[i].xform_dirty = 0;
        }
        LeaveCriticalSection(&s->lock);

        /* Every main-scene mutation below runs under the exclusive scene lock, acquired on the FIRST
         * one and held through the commit — so the whole add/remove/update+commit batch is atomic vs a
         * borrowing sim's ray trace (which holds the lock shared). Acquiring it also guarantees no
         * reader is inside RunReflections/RunPathing while build_dyn's phonon calls run. */
        if (op == 1) {
            if (!excl) { scene_w_lock(s); excl = 1; }
            if (build_dyn(s, &s->dyn[i], bv, bnv, bt, bnt, &bm, &xform)) {
                main_dirty = 1;
                EnterCriticalSection(&s->lock); s->dyn_shadow[i].live_ack = 1; LeaveCriticalSection(&s->lock);
            } else {                                                       /* build failed: drop the request */
                free(bv); free(bt);
                EnterCriticalSection(&s->lock); s->dyn_shadow[i].want = 0; LeaveCriticalSection(&s->lock);
            }
        } else if (op == 2 && s->dyn[i].live) {
            if (!excl) { scene_w_lock(s); excl = 1; }
            iplInstancedMeshUpdateTransform(s->dyn[i].inst, s->scene, xform);   /* queued; committed below */
            main_dirty = 1;
        } else if (op == 3) {
            if (!excl) { scene_w_lock(s); excl = 1; }
            if (s->dyn[i].inst) iplInstancedMeshRemove(s->dyn[i].inst, s->scene);
            torn[i] = 1; main_dirty = 1;
        } else if (op == 4) {
            free(bv); free(bt);
        }
    }
    if (main_dirty) iplSceneCommit(s->scene);   /* under excl */
    if (excl) scene_w_unlock(s);
    for (int i = 0; i < BWA_MAX_DYN_MESH; ++i) if (torn[i]) {              /* after the unlinking commit */
        release_dyn(&s->dyn[i]);                                          /* instance already unlinked: no lock needed */
        EnterCriticalSection(&s->lock); s->dyn_shadow[i].live_ack = 0; LeaveCriticalSection(&s->lock);
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
    BWA_THREAD_NAME("bw-sim (occlusion)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  /* never preempt the audio callback */

    while (!s->stop) {
        /* 1. snapshot the shadow + take the pending mesh */
        IPLVector3* verts = NULL; IPLTriangle* tris = NULL; int nverts = 0, ntris = 0;
        IPLMaterial* mats = NULL; int nmat = 0; IPLint32* tri_mat = NULL;
        EnterCriticalSection(&s->lock);
        if (s->mesh_dirty) {
            s->mesh_dirty = 0;
            verts = s->pend_verts; tris = s->pend_tris; nverts = s->pend_nverts; ntris = s->pend_ntris;
            mats = s->pend_mats; nmat = s->pend_nmat; tri_mat = s->pend_tri_mat;
            s->pend_verts = NULL; s->pend_tris = NULL;     /* ownership moves to the sim thread */
            s->pend_mats = NULL; s->pend_tri_mat = NULL;
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

        /* 2. apply geometry change: the static mesh, then reconcile the instanced movers. Both mutate +
         * commit the main scene under the exclusive scene lock (scene_w_lock/unlock). */
        if (verts && tris && mats && tri_mat) apply_mesh(s, verts, nverts, tris, ntris, mats, nmat, tri_mat);
        else { free(verts); free(tris); free(mats); free(tri_mat); }   /* partial set: drop it intact */
        reconcile_dynamic(s);

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
        if (any_src) { BWA_ZONE_BEGIN(zs, "occlusion ray-trace"); iplSimulatorRunDirect(s->simulator); BWA_ZONE_END(zs); }
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
                else { level = maxg; for (int b = 0; b < 3; ++b) { bands[b] = raw[b] / maxg; if (bands[b] < EQ_BAND_FLOOR) bands[b] = EQ_BAND_FLOOR; } }
            }
            if (snap_feat[i] & FEAT_DIR) dir = d->directivity;
            rt_set_direct(s->rt, snap_h[i], level, bands, dir);
        }

        Sleep(1000 / SIM_HZ);
    }
    free(snap_h); free(snap_p); free(snap_feat); free(snap_dw); free(snap_dp); free(snap_fwd);
    return 0;
}

SteamScene* steam_scene_create(RtCore* rt, uint32_t sample_rate, uint32_t frame_size, uint32_t voice_cap,
                               int use_embree) {
    if (!rt || voice_cap == 0) return NULL;
    SteamScene* s = (SteamScene*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->rt = rt; s->voice_cap = voice_cap;
    s->shadow = calloc(voice_cap, sizeof *s->shadow);
    s->srcs   = calloc(voice_cap, sizeof *s->srcs);
    s->dyn        = calloc(BWA_MAX_DYN_MESH, sizeof *s->dyn);
    s->dyn_shadow = calloc(BWA_MAX_DYN_MESH, sizeof *s->dyn_shadow);
    if (!s->shadow || !s->srcs || !s->dyn || !s->dyn_shadow) {
        free(s->shadow); free(s->srcs); free(s->dyn); free(s->dyn_shadow); free(s); return NULL;
    }
    InitializeCriticalSection(&s->lock);
    InitializeSRWLock(&s->scene_srw);

    IPLContextSettings cs; memset(&cs, 0, sizeof cs); cs.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&cs, &s->context) != IPL_STATUS_SUCCESS) goto fail;

    /* Ray tracer: default to Steam Audio's built-in, opt into Embree (faster CPU ray tracing) via
     * bwa_desc.embree. If the device can't be created (phonon built without Embree, or no runtime),
     * fall back to default so the flag is always safe. The reflection sim borrows this scene, so it
     * must use the same scene type — exposed via steam_scene_ipl_scenetype. */
    s->scene_type = IPL_SCENETYPE_DEFAULT;
    if (use_embree) {
        if (iplEmbreeDeviceCreate(s->context, NULL, &s->embree) == IPL_STATUS_SUCCESS) {
            s->scene_type = IPL_SCENETYPE_EMBREE;
            fprintf(stderr, "bw_audio: Steam Audio ray tracing on Embree (bwa_desc.embree)\n");
        } else {
            fprintf(stderr, "bw_audio: embree requested but unavailable; using the default ray tracer\n");
        }
    }

    IPLSceneSettings sc; memset(&sc, 0, sizeof sc);
    sc.type = s->scene_type;
    sc.embreeDevice = s->embree;                /* ignored unless type == EMBREE */
    if (iplSceneCreate(s->context, &sc, &s->scene) != IPL_STATUS_SUCCESS) goto fail;

    IPLSimulationSettings ss; memset(&ss, 0, sizeof ss);
    ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
    ss.sceneType = s->scene_type;
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

/* Copy the caller's geometry + materials into freshly-allocated pending buffers and publish them to
 * the sim thread under the lock. nmat materials come as flat float arrays; tri_material may be NULL
 * (all triangles -> material 0) and out-of-range indices clamp to 0. All-or-nothing: a partial
 * allocation frees what it got and leaves the prior mesh in place. */
static void set_mesh_internal(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                              int nmat, const float* absorption, const float* scattering,
                              const float* transmission, const int* tri_material) {
    if (!s || nverts <= 0 || ntris <= 0 || nmat <= 0) return;
    if (!verts || !tris || !absorption || !scattering || !transmission) return;   /* tri_material may be NULL */
    IPLVector3*  v  = (IPLVector3*)malloc((size_t)nverts * sizeof(IPLVector3));
    IPLTriangle* t  = (IPLTriangle*)malloc((size_t)ntris  * sizeof(IPLTriangle));
    IPLMaterial* m  = (IPLMaterial*)malloc((size_t)nmat   * sizeof(IPLMaterial));
    IPLint32*    mi = (IPLint32*)   malloc((size_t)ntris  * sizeof(IPLint32));
    if (!v || !t || !m || !mi) { free(v); free(t); free(m); free(mi); return; }
    for (int i = 0; i < nverts; ++i) v[i] = (IPLVector3){ verts[i*3+0], verts[i*3+1], verts[i*3+2] };
    for (int i = 0; i < ntris;  ++i) { t[i].indices[0] = tris[i*3+0]; t[i].indices[1] = tris[i*3+1]; t[i].indices[2] = tris[i*3+2]; }
    for (int k = 0; k < nmat;   ++k) {
        memset(&m[k], 0, sizeof m[k]);
        for (int b = 0; b < 3; ++b) { m[k].absorption[b] = absorption[k*3+b]; m[k].transmission[b] = transmission[k*3+b]; }
        m[k].scattering = scattering[k];
    }
    for (int i = 0; i < ntris;  ++i) {
        int idx = tri_material ? tri_material[i] : 0;
        mi[i] = (idx >= 0 && idx < nmat) ? (IPLint32)idx : 0;
    }

    EnterCriticalSection(&s->lock);
    free(s->pend_verts); free(s->pend_tris); free(s->pend_mats); free(s->pend_tri_mat);   /* drop un-consumed prior */
    s->pend_verts = v; s->pend_nverts = nverts;
    s->pend_tris = t;  s->pend_ntris = ntris;
    s->pend_mats = m;  s->pend_nmat = nmat;
    s->pend_tri_mat = mi; s->mesh_dirty = 1;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_mesh_mat(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                              int nmat, const float* absorption, const float* scattering,
                              const float* transmission, const int* tri_material) {
    set_mesh_internal(s, verts, nverts, tris, ntris, nmat, absorption, scattering, transmission, tri_material);
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
        s->shadow[idx].fwd[0] = BWA_ROOM_AHEAD[0]; s->shadow[idx].fwd[1] = BWA_ROOM_AHEAD[1];      /* default ahead = the room */
        s->shadow[idx].fwd[2] = BWA_ROOM_AHEAD[2];                                                /* frame's identity forward */
        s->shadow[idx].pos[0] = 0.f; s->shadow[idx].pos[1] = 0.f; s->shadow[idx].pos[2] = 0.f;   /* origin until set_pos lands */
    }
}

void steam_scene_set_occlusion(SteamScene* s, uint32_t handle, int on) {
    if (!s) return;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    if (on) s->shadow[idx].features |= FEAT_OCC; else s->shadow[idx].features &= (uint8_t)~FEAT_OCC;
    LeaveCriticalSection(&s->lock);
}

/* weight 0 = omni (no directivity); 0.5 = cardioid; 1 = figure-8. power >= 1 sharpens the lobe. */
void steam_scene_set_directivity(SteamScene* s, uint32_t handle, float weight, float power) {
    if (!s) return;
    uint16_t idx = BWA_H_IDX(handle);
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
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    s->shadow[idx].fwd[0] = fx; s->shadow[idx].fwd[1] = fy; s->shadow[idx].fwd[2] = fz;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_set_pos(SteamScene* s, uint32_t handle, float x, float y, float z) {
    if (!s) return;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    shadow_adopt(s, idx, handle);
    s->shadow[idx].pos[0] = x; s->shadow[idx].pos[1] = y; s->shadow[idx].pos[2] = z;
    LeaveCriticalSection(&s->lock);
}

/* On bwa_source_destroy: clear ALL features so the sim tears the IPLSource down and stops simulating
 * the slot (else a recycled slot inherits stale state). */
void steam_scene_source_gone(SteamScene* s, uint32_t handle) {
    if (!s) return;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= s->voice_cap) return;
    EnterCriticalSection(&s->lock);
    if (s->shadow[idx].handle == handle) s->shadow[idx].features = 0;
    LeaveCriticalSection(&s->lock);
}

void steam_scene_ray_lock(SteamScene* s)   { if (s) AcquireSRWLockShared(&s->scene_srw); }
void steam_scene_ray_unlock(SteamScene* s) { if (s) ReleaseSRWLockShared(&s->scene_srw); }

/* Allocate a dynamic-mesh slot and stash its geometry + a single material for the sim thread to build.
 * The verts/tris are copied into heap arrays (ownership passes to the sim thread on build). A slot is
 * free only once fully torn down (want==0 && live_ack==0 && no pending geometry). Returns the slot
 * index, or -1 on bad input / a full table. */
int steam_scene_add_dynamic_mesh(SteamScene* s, const float* verts, int nverts, const int* tris, int ntris,
                                 const float absorption[3], float scattering, const float transmission[3]) {
    if (!s || nverts <= 0 || ntris <= 0 || !verts || !tris) return -1;
    IPLVector3*  v = (IPLVector3*)malloc((size_t)nverts * sizeof(IPLVector3));
    IPLTriangle* t = (IPLTriangle*)malloc((size_t)ntris  * sizeof(IPLTriangle));
    if (!v || !t) { free(v); free(t); return -1; }
    for (int i = 0; i < nverts; ++i) v[i] = (IPLVector3){ verts[i*3+0], verts[i*3+1], verts[i*3+2] };
    for (int i = 0; i < ntris;  ++i) { t[i].indices[0] = tris[i*3+0]; t[i].indices[1] = tris[i*3+1]; t[i].indices[2] = tris[i*3+2]; }
    IPLMaterial m; memset(&m, 0, sizeof m);
    for (int b = 0; b < 3; ++b) { m.absorption[b] = absorption ? absorption[b] : 0.1f; m.transmission[b] = transmission ? transmission[b] : 0.05f; }
    m.scattering = scattering;

    EnterCriticalSection(&s->lock);
    int slot = -1;
    for (int i = 0; i < BWA_MAX_DYN_MESH; ++i)
        if (!s->dyn_shadow[i].want && !s->dyn_shadow[i].live_ack && !s->dyn_shadow[i].verts) { slot = i; break; }
    if (slot >= 0) {
        s->dyn_shadow[slot].want  = 1;
        s->dyn_shadow[slot].verts = v; s->dyn_shadow[slot].nverts = nverts;
        s->dyn_shadow[slot].tris  = t; s->dyn_shadow[slot].ntris  = ntris;
        s->dyn_shadow[slot].mat   = m;
        s->dyn_shadow[slot].xform = mat_identity();   /* until set_dynamic_transform */
        s->dyn_shadow[slot].xform_dirty = 0;
    }
    LeaveCriticalSection(&s->lock);
    if (slot < 0) { free(v); free(t); return -1; }
    return slot;
}

void steam_scene_set_dynamic_transform(SteamScene* s, int handle, const float m16[16]) {
    if (!s || handle < 0 || handle >= BWA_MAX_DYN_MESH || !m16) return;
    EnterCriticalSection(&s->lock);
    if (s->dyn_shadow[handle].want) {                 /* ignore updates to a removed/unallocated slot */
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) s->dyn_shadow[handle].xform.elements[r][c] = m16[r*4+c];
        s->dyn_shadow[handle].xform_dirty = 1;
    }
    LeaveCriticalSection(&s->lock);
}

void steam_scene_remove_dynamic_mesh(SteamScene* s, int handle) {
    if (!s || handle < 0 || handle >= BWA_MAX_DYN_MESH) return;
    EnterCriticalSection(&s->lock);
    s->dyn_shadow[handle].want = 0;                   /* the sim tears down + clears live_ack */
    LeaveCriticalSection(&s->lock);
}

void* steam_scene_ipl_context(SteamScene* s) { return s ? (void*)s->context : NULL; }
void* steam_scene_ipl_scene  (SteamScene* s) { return s ? (void*)s->scene   : NULL; }
int   steam_scene_ipl_scenetype(SteamScene* s) { return s ? (int)s->scene_type : 0; }  /* IPLSceneType; reflect sim matches it */

void steam_scene_destroy(SteamScene* s) {
    if (!s) return;
    if (s->thread) { InterlockedExchange(&s->stop, 1); WaitForSingleObject(s->thread, INFINITE); CloseHandle(s->thread); }
    for (uint32_t i = 0; i < s->voice_cap; ++i)
        if (s->srcs && s->srcs[i]) { iplSourceRemove(s->srcs[i], s->simulator); iplSourceRelease(&s->srcs[i]); }
    /* dynamic movers: unlink every live instance, commit once so the scene drops its references, then
     * release each instance + sub-scene. The sim thread is joined, so no lock is needed. */
    if (s->dyn) {
        int any = 0;
        for (int i = 0; i < BWA_MAX_DYN_MESH; ++i) if (s->dyn[i].inst) { iplInstancedMeshRemove(s->dyn[i].inst, s->scene); any = 1; }
        if (any && s->scene) iplSceneCommit(s->scene);
        for (int i = 0; i < BWA_MAX_DYN_MESH; ++i) release_dyn(&s->dyn[i]);
    }
    if (s->dyn_shadow)
        for (int i = 0; i < BWA_MAX_DYN_MESH; ++i) { free(s->dyn_shadow[i].verts); free(s->dyn_shadow[i].tris); }
    if (s->mesh)      iplStaticMeshRelease(&s->mesh);
    if (s->simulator) iplSimulatorRelease(&s->simulator);
    if (s->scene)     iplSceneRelease(&s->scene);
    if (s->embree)    iplEmbreeDeviceRelease(&s->embree);   /* after the scene that referenced it */
    if (s->context)   iplContextRelease(&s->context);
    DeleteCriticalSection(&s->lock);
    free(s->mesh_verts); free(s->mesh_tris); free(s->mesh_mi); free(s->mesh_mats);
    free(s->pend_verts); free(s->pend_tris); free(s->pend_mats); free(s->pend_tri_mat);
    free(s->dyn); free(s->dyn_shadow);
    free(s->shadow); free(s->srcs);
    free(s);
}
