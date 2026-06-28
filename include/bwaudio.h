/*
 * bwaudio.h — public C ABI for the spatial audio core.
 *
 * Self-hosted native audio engine driving a 26-speaker CAVE array via ASIO/Dante,
 * with a binaural (HRTF) debug monitor. Engines (Unity/Unreal) are thin control
 * clients; no audio buffers cross this boundary.
 *
 * Name "bw" is a placeholder prefix — rename freely, but keep it consistent
 * across the header, the lib symbols, and the engine bindings.
 *
 * THREADING CONTRACT (see docs/concurrency.md — this is load-bearing):
 *   - All bw_* calls must come from ONE control thread (the engine main thread).
 *   - Every per-frame call is non-blocking: it encodes a command onto an SPSC
 *     ring that the audio thread drains. It does NOT touch DSP state.
 *   - Only create/start/load/source_create may allocate or do I/O. Do these at
 *     load time, never mid-frame in a hot loop.
 *   - Position/pose are latest-wins; push them every frame.
 *
 * LICENSING NOTE: links the Steinberg ASIO SDK (GPLv3 option, see docs/build.md)
 * and Steam Audio. Distribution implications are copyleft — read docs/build.md.
 */
#ifndef BWAUDIO_H
#define BWAUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #ifdef BW_BUILD_DLL
    #define BW_API __declspec(dllexport)
  #else
    #define BW_API __declspec(dllimport)
  #endif
#else
  #define BW_API
#endif

typedef struct BwEngine BwEngine;   /* opaque */

/* Handles are (index | generation<<16). A stale handle (slot destroyed and
 * reused) fails validation on the audio side and is silently dropped. 0 = invalid. */
typedef uint32_t BwSound;
typedef uint32_t BwSource;
typedef uint32_t BwBed;       /* an ambisonic-bed voice (world-locked soundfield); see bw_bed_* */

typedef enum {
    BW_PROFILE_CAVE     = 0,  /* 26-ch bus -> ASIO/DVS. Listener POSITION only. */
    BW_PROFILE_BINAURAL = 1,  /* 26-ch bus -> binaural monitor -> stereo device. Full POSE. */
    BW_PROFILE_BOTH     = 2,  /* array to DVS + binaural tap to a stereo device. Full POSE. */
} BwProfile;

typedef struct {
    BwProfile   profile;
    const char* layout_path;   /* surveyed speaker geometry (JSON). cave/both. */
    const char* hrtf_path;     /* HRTF (SOFA) or NULL for built-in. binaural/both. */
    uint32_t    sample_rate;   /* 48000 */
    uint32_t    block_size;    /* ASIO buffer hint, e.g. 256/512 */
    bool        track_internal;/* true: core reads OptiTrack/NatNet itself.
                                * false: engine pushes pose via bw_set_listener_pose. */
} BwConfig;

/* ---- lifecycle (control thread; may block/allocate; do at load time) ---- */
BW_API BwEngine* bw_create(const BwConfig* cfg);
BW_API int       bw_start(BwEngine* e);   /* opens device(s), starts audio thread. 0 = ok */
BW_API int       bw_stop(BwEngine* e);
BW_API void      bw_destroy(BwEngine* e);
BW_API const char* bw_last_error(BwEngine* e); /* human-readable; NULL if none */
/* Backend actually in use after bw_start: "asio:<driver>", "null" (offline/SILENT), or
 * "none" (not started). Lets a client confirm it got a real device, not a silent fallback. */
BW_API const char* bw_audio_backend(BwEngine* e);

/* ---- assets (control thread; file I/O; do at load time) ---- */
BW_API BwSound bw_load_sound(BwEngine* e, const char* path); /* mono point-source asset; 0 = failure */
BW_API void    bw_unload_sound(BwEngine* e, BwSound snd);    /* safe: retire-acked internally */
/* Load a pre-encoded AmbiX (ACN/SN3D) soundfield, KEEPING its channels (4/9/16 -> order 1/2/3). Plays
 * via bw_bed_* as a world-locked diffuse bed decoded to the 26 speakers; resampled to the engine rate
 * at load if needed. 0 = failure (bad channel count / decode error). */
BW_API BwSound bw_load_ambix(BwEngine* e, const char* path);

/* ---- sources (control thread; non-blocking, enqueue only) ---- */
BW_API BwSource bw_source_create(BwEngine* e);              /* handle returned synchronously */
BW_API void     bw_source_destroy(BwEngine* e, BwSource s);
BW_API void     bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z); /* ROOM space, right-handed */
BW_API void     bw_source_set_gain(BwEngine* e, BwSource s, float linear);
BW_API void     bw_source_play(BwEngine* e, BwSource s, BwSound snd, bool loop);
BW_API void     bw_source_stop(BwEngine* e, BwSource s);
BW_API void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);

/* ---- ambisonic beds (control thread; a world-locked soundfield decoded straight to the 26 speakers,
 * not DBAP-panned — for diffuse/ambient content). Play a bw_load_ambix asset; no position. ---- */
BW_API BwBed bw_bed_create(BwEngine* e);
BW_API void  bw_bed_play(BwEngine* e, BwBed b, BwSound snd, bool loop);
BW_API void  bw_bed_set_gain(BwEngine* e, BwBed b, float linear);   /* master gain, ramped */
BW_API void  bw_bed_stop(BwEngine* e, BwBed b);
BW_API void  bw_bed_destroy(BwEngine* e, BwBed b);

/* ---- materials / occlusion (control thread; needs the Steam Audio build) ----
 * Set the occluding geometry (room space, RH metres; tris are CCW vertex-index triples) with one
 * material (3-band absorption/transmission + scattering, each 0..1). Load-time. No-op without the
 * Steam Audio backend. */
BW_API void     bw_scene_set_mesh(BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                                  const float absorption[3], float scattering, const float transmission[3]);
/* Enable per-source occlusion: geometry between the source and listener attenuates it (ramped).
 * Per-frame-safe. No-op without the Steam Audio backend. */
BW_API void     bw_source_set_occlusion(BwEngine* e, BwSource s, bool on);
/* Read the source's current occlusion factor (1 = clear .. 0 = fully blocked) — for HUD/diagnostics. */
BW_API float    bw_source_get_occlusion(BwEngine* e, BwSource s);

/* ---- directivity (control thread; needs the Steam Audio build) ----
 * Source radiation pattern. Sources are omni by default; set a weighted-dipole pattern + the
 * source's forward orientation, and a listener off the source's forward axis hears it attenuated.
 * Independent of occlusion (a source can be directional without being occluded). */
typedef enum { BW_DIR_OMNI = 0, BW_DIR_CARDIOID = 1, BW_DIR_FIGURE8 = 2 } BwDirectivity;
/* Source orientation as a quaternion (same room frame + handedness as bw_set_listener_pose); the
 * dipole axis is the source's forward (-z rotated by q). Per-frame-safe. No-op without the SDK. */
BW_API void     bw_source_set_orientation(BwEngine* e, BwSource s, float qx, float qy, float qz, float qw);
/* Radiation pattern: weight 0=omni (off) .. 0.5=cardioid .. 1=figure-8; power>=1 sharpens the lobe.
 * Per-frame-safe. No-op without the Steam Audio backend. */
BW_API void     bw_source_set_directivity(BwEngine* e, BwSource s, float weight, float power);
/* Named-preset sugar over bw_source_set_directivity (OMNI disables it). */
BW_API void     bw_source_set_directivity_preset(BwEngine* e, BwSource s, BwDirectivity pattern);
/* Read the source's current directivity gain (1 = on-axis/omni .. 0 = full null) — HUD/diagnostics. */
BW_API float    bw_source_get_directivity(BwEngine* e, BwSource s);

/* ---- listener (control thread; skip if track_internal) ---- */
/* Position in room space. Quaternion is head orientation; used by the binaural
 * monitor only — the array render ignores orientation (real speakers, real ears). */
BW_API void bw_set_listener_pose(BwEngine* e, float px, float py, float pz,
                                              float qx, float qy, float qz, float qw);

/* Read back the listener pose the engine is currently rendering with — the committed pose, or,
 * under track_internal, the freshest tracked pose. For visuals/logging/debugging tracking; safe
 * to poll from the control thread. Fills p[3] (position) and q[4] (orientation xyzw). */
BW_API void bw_get_listener_pose(BwEngine* e, float p[3], float q[4]);

/* ---- frame boundary ----
 * Promotes this frame's position/pose updates as one coherent snapshot and drains
 * the event ring (voice-ended, sound-retired). Call once per frame after pushing
 * all source + listener updates. */
BW_API void bw_commit(BwEngine* e);

#ifdef __cplusplus
}
#endif
#endif /* BWAUDIO_H */
