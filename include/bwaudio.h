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
/* Is the source's voice still producing audio? Control-thread poll (latest-wins readback): true while
 * a sound plays, false once a non-loop sound finishes, after stop, or for a stale/destroyed handle.
 * Best-effort — a sound shorter than the caller's poll interval may never be observed as playing. */
BW_API bool     bw_source_is_playing(BwEngine* e, BwSource s);
BW_API void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);

/* ---- ambisonic beds (control thread; a world-locked soundfield decoded straight to the 26 speakers,
 * not DBAP-panned — for diffuse/ambient content). Play a bw_load_ambix asset; no position. Occlusion
 * and directivity do NOT apply to a bed (it is world-locked diffuse). bw_bed_play requires a
 * multichannel asset and bw_source_play a mono one — a mismatch is rejected (see bw_last_error). ---- */
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

/* An acoustic material — an opaque, engine-scoped token. 0 is always a built-in generic default.
 * Mint materials at LOAD time (the table is fixed-capacity); a token indexes both occlusion
 * (per-band transmission) and the reflection bed (absorption/scattering). */
typedef uint32_t BwMaterial;
/* Mint a material from a named preset (case-insensitive): "generic", "brick", "concrete", "ceramic",
 * "gravel", "carpet", "glass", "plaster", "wood", "metal", "rock". Returns 0 (the default material,
 * NOT an error sentinel) on an unknown name or a full table — check bw_last_error to distinguish. */
BW_API BwMaterial bw_material_preset(BwEngine* e, const char* name);
/* Mint a custom material: 3-band absorption + 3-band transmission (low/mid/high, each 0..1) and a
 * scattering coefficient (0..1). Returns the new token, or 0 if the material table is full. */
BW_API BwMaterial bw_material_define(BwEngine* e, const float absorption[3], float scattering, const float transmission[3]);
/* Set occluding geometry with PER-TRIANGLE materials: tri_material is ntris BwMaterial tokens (one
 * per triangle; out-of-range clamps to the default). Same room frame + winding as bw_scene_set_mesh.
 * Load-time. No-op without the Steam Audio backend. */
BW_API void     bw_scene_set_mesh_mat(BwEngine* e, const float* verts, int nverts, const int* tris, int ntris,
                                      const BwMaterial* tri_material);
/* Convenience: a shoebox enclosure of size w x h x d metres, centred at the origin, with one
 * material per face. faces[6] = (-x,+x,-y,+y,-z,+z); each a BwMaterial token (0 = default). Triangle
 * normals face inward (the listener is inside). Load-time. No-op without the Steam Audio backend. */
BW_API void     bw_scene_set_box(BwEngine* e, float w, float h, float d, const BwMaterial faces[6]);
/* Enable per-source occlusion: geometry between the source and listener attenuates it (ramped).
 * Per-frame-safe. No-op without the Steam Audio backend. */
BW_API void     bw_source_set_occlusion(BwEngine* e, BwSource s, bool on);

/* ---- reflection bed (materials; needs the Steam Audio build) ----
 * A single shared listener-centric reverb bed (Steam Audio hybrid reverb), decoded straight to the
 * 26 speakers and summed onto the bus. Configure at LOAD time (the IR length + order are baked at
 * bw_start), then opt sources into its wet send. v1 assumes a static scene (set geometry before
 * bw_start). No-op without the Steam Audio backend. */
typedef struct {
    float    ir_seconds;     /* reverb tail / IR length; 0 -> default 1.0 (range ~0.5..2.0) */
    uint32_t order;          /* reflection ambisonic order, 1 or 2; 0 -> default 1 */
    uint32_t num_rays;       /* off-thread ray budget; 0 -> default 4096 */
    uint32_t num_bounces;    /* off-thread bounce depth; 0 -> default 16 */
    int      enabled;        /* 0 = no bed created (engine behaves exactly as today) */
    float    wet_gain;       /* linear level of the reverb summed onto the bus; 0 -> default 1.0 */
    uint32_t reserved[3];    /* zero; reserved so the struct can grow without an ABI break */
} BwReflectionConfig;
/* Set the reflection config. Load-time only (between bw_create and bw_start); copies the struct,
 * zero fields take defaults. No-op without the Steam Audio backend. */
BW_API void     bw_reflections_config(BwEngine* e, const BwReflectionConfig* cfg);
/* Set the reverb wet level (linear; 1 = unity) live. Per-frame-safe. No-op if the bed isn't running. */
BW_API void     bw_reflections_set_gain(BwEngine* e, float linear);
/* Opt a source into the shared bed's wet send (per-frame-safe, enqueue-only). With the bed disabled
 * or no SDK, this just gates a send that goes nowhere. */
BW_API void     bw_source_set_reflections(BwEngine* e, BwSource s, bool on);
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

/* ---- channel test / diagnostics (control thread; no SDK needed) ----
 * Drive a single OUTPUT channel with a built-in test signal, injected AFTER the per-speaker align
 * stage (a raw value straight on the channel) — a speaker-check / wiring-verification / calibration
 * tool, NOT a spatial path (it bypasses the panner; don't use it to "place" audio). `channel` is in
 * [0, 26). Per-frame-safe, takes effect next block, no bw_commit needed; multiple channels at once.
 * gain 0 or BW_TEST_OFF silences a channel. Composes with the profiles: cave/both -> a raw tone on
 * that DVS channel/speaker; binaural -> that bus channel HRTF'd as its virtual speaker. */
typedef enum { BW_TEST_OFF = 0, BW_TEST_SINE = 1, BW_TEST_NOISE = 2 } BwTestKind;
BW_API void     bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain);

/* Read back the effective speaker layout (the default grid, or the file from BwConfig.layout_path):
 * fills `xyz` with up to `cap` speakers' positions (3 floats each: x,y,z room space, in channel/index
 * order) and returns the total count (26). Pass xyz=NULL to just query the count. For visualizing or
 * auditioning the geometry the engine is actually panning with. Control thread; safe any time. */
BW_API uint32_t bw_get_speakers(BwEngine* e, float* xyz, uint32_t cap);

/* ---- panner selection (load-time, or live: the switch is atomic) ----
 * The per-source panner that writes the 26-ch bus. DBAP (default) is listener-relative, recomputed
 * per frame from the tracked position — for a MOVING observer roaming the array. SPCAP is a smooth,
 * all-speaker, placement-correcting sweet-spot panner for a FIXED observer (a static listener: don't
 * track, set the sweet spot once); it conserves loudspeaker power across an uneven array. VBAP is the
 * sharpest (the 2-3 nearest speakers of the containing triangle carry a source) — also fixed-observer,
 * best when the array triangulates cleanly; it falls back to DBAP for a non-triangulable array. SPCAP
 * and VBAP assume a fixed listener (with internal tracking their cache rebuilds every block — use DBAP
 * for a moving observer). Does not affect the diffuse bed / ambisonic paths. See docs/spatialization.md. */
typedef enum { BW_PAN_DBAP = 0, BW_PAN_SPCAP = 1, BW_PAN_VBAP = 2 } BwPanner;
BW_API void     bw_set_panner(BwEngine* e, BwPanner panner);

/* Select the diffuse-bed ambisonic decoder (load-time: between bw_create and bw_start). SAMPLING is the
 * default projection decode; ALLRAD (All-Round Ambisonic Decoding) decodes to a uniform virtual layout
 * then VBAPs onto the real array — robust on an IRREGULAR/lopsided array (keeps a diffuse field even
 * with no loud directions, and localizes better), at the cost of a heavier load-time build. Affects the
 * ambisonic + reflection BEDS only, not the point-source panner. See docs/spatialization.md. */
typedef enum { BW_DECODE_SAMPLING = 0, BW_DECODE_ALLRAD = 1 } BwBedDecoder;
BW_API void     bw_set_bed_decoder(BwEngine* e, BwBedDecoder decoder);

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
