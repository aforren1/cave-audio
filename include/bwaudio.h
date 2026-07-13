/*
 * bwaudio.h — public C ABI for the spatial audio core.
 *
 * Self-hosted native audio engine driving a 26-speaker CAVE array via ASIO/Dante,
 * with a binaural (HRTF) debug monitor. Engines (Unity/Unreal) are thin control
 * clients; no audio buffers cross this boundary — only control (sound triggers,
 * source positions, the listener pose).
 *
 * Usage docs: docs/api.md — quickstart, profiles, the threading contract,
 * coordinates, error handling, environment variables, and the per-call
 * reference. examples/minimal.c runs the whole client lifecycle.
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
 *   - Position/pose are latest-wins; push them every frame, then bw_commit once.
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
    BW_PROFILE_CAVE     = 0,  /* speaker bus -> ASIO/DVS. Listener POSITION only. */
    BW_PROFILE_BINAURAL = 1,  /* speaker bus -> binaural monitor -> stereo device. Full POSE. */
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
/* Like bw_load_sound but STREAMED from disk for long files (music/ambience) — the file is not decoded
 * into RAM; a background thread feeds the voice as it plays. Mono (downmixed) at the engine sample rate
 * (a rate mismatch fails — pre-convert, or use bw_load_sound which resamples). Plays on one voice at a
 * time. 0 = failure. WAV/FLAC/MP3. */
BW_API BwSound bw_load_sound_streaming(BwEngine* e, const char* path);
BW_API void    bw_unload_sound(BwEngine* e, BwSound snd);    /* safe: retire-acked internally */
/* Load a pre-encoded AmbiX (ACN/SN3D) soundfield, KEEPING its channels (4/9/16 -> order 1/2/3). Plays
 * via bw_bed_* as a world-locked diffuse bed decoded to the speakers; resampled to the engine rate
 * at load if needed. 0 = failure (bad channel count / decode error). */
BW_API BwSound bw_load_ambix(BwEngine* e, const char* path);

/* ---- sources (control thread; non-blocking, enqueue only) ---- */
BW_API BwSource bw_source_create(BwEngine* e);              /* handle returned synchronously */
BW_API void     bw_source_destroy(BwEngine* e, BwSource s);
/* Voice-steal priority, 0 = expendable .. 255 = protected (default 128). When the voice pool is full,
 * bw_source_create stops the lowest-priority active source to make room for the new one (so an overload
 * drops the least-important sound instead of failing the new one). Set high on music/critical SFX. */
BW_API void     bw_source_set_priority(BwEngine* e, BwSource s, int priority);
BW_API void     bw_source_set_pos(BwEngine* e, BwSource s, float x, float y, float z); /* ROOM space, right-handed */
BW_API void     bw_source_set_gain(BwEngine* e, BwSource s, float linear);
/* Timed gain fade: glide the source's gain to `gain` over `seconds` (engine-side, so no per-frame
 * scripting; seconds <= 0 sets it immediately). A later bw_source_set_gain or fade replaces it.
 * bw_source_fade_out fades to silence and then STOPS the voice (the click-free stop path) — the
 * one-call answer to "fade this out and clean it up". Per-frame-safe. */
BW_API void     bw_source_fade_to (BwEngine* e, BwSource s, float gain, float seconds);
BW_API void     bw_source_fade_out(BwEngine* e, BwSource s, float seconds);
/* Mix groups (0..7; sources start in group 0): group gain multiplies into every member's gain solve
 * (ramped), and a paused group ramps out + freezes its members' playheads exactly like per-voice
 * pause — "duck the SFX, keep the dialog", scene-wide category control without touching each source.
 * All per-frame-safe. */
BW_API void     bw_source_set_group(BwEngine* e, BwSource s, uint32_t group);
BW_API void     bw_group_set_gain  (BwEngine* e, uint32_t group, float linear);
BW_API void     bw_group_set_paused(BwEngine* e, uint32_t group, bool paused);
/* Playback rate (1 = native; clamped to [0.25, 4]): a fractional-cursor linear-interp resample of
 * IN-MEMORY sounds — variation on repeated one-shots, slow-mo, engines. Rate changes GLIDE across a
 * block (a change bends the pitch, never steps it) and compose with Doppler. Streamed sounds ignore
 * it (the stream ring is sequential); beds are unaffected. Per-frame-safe. */
BW_API void     bw_source_set_pitch(BwEngine* e, BwSource s, float rate);
BW_API void     bw_source_play(BwEngine* e, BwSource s, BwSound snd, bool loop);
/* Sample-accurate scheduled play: begin output exactly when the engine's dsp clock reaches
 * `start_sample` (the voice is silent until then, then starts at the precise in-block sample).
 * Get "now" from bw_dsp_time and add a delay, e.g. play 0.5 s out: bw_dsp_time(e) + sample_rate/2.
 * A start_sample already in the past plays immediately (best-effort). 0 = play now (== bw_source_play). */
BW_API void     bw_source_play_at(BwEngine* e, BwSource s, BwSound snd, bool loop, uint64_t start_sample);
/* The engine's current dsp-sample clock (most recently rendered block's first sample): the device
 * sample position when running, an internal block counter otherwise. Monotonic. 0 before the first block. */
BW_API uint64_t bw_dsp_time(BwEngine* e);
BW_API void     bw_source_stop(BwEngine* e, BwSource s);
/* Pause/resume the source's voice in place. The gate ramps over one block (~5 ms — no click) and the
 * playhead freezes once silent, so resume continues exactly where pause landed. Works for in-memory,
 * streamed, and bed sounds. A paused voice still reads as playing (it has not ended); bw_source_play
 * always starts un-paused. */
BW_API void     bw_source_set_paused(BwEngine* e, BwSource s, bool paused);
/* Global pause (app focus loss, menu pause): EVERY voice ramps out and freezes — memory, streamed,
 * and bed alike — and resume continues exactly. Same semantics as per-voice pause (paused voices
 * still read as playing). Live, per-frame-safe. */
BW_API void     bw_set_paused(BwEngine* e, bool paused);
/* Jump the voice's content position to `frame` (engine-rate frames into the sound). Click-free: the
 * voice ramps out, jumps, ramps back in (~10 ms end to end); on a paused voice the jump is immediate
 * (and stays paused). Past-the-end: loops wrap, one-shots end. In-memory and bed sounds only —
 * streamed sounds ignore it (the stream ring cannot jump). */
BW_API void     bw_source_seek(BwEngine* e, BwSource s, uint64_t frame);
/* Is the source's voice still producing audio? Control-thread poll (latest-wins readback): true while
 * a sound plays, false once a non-loop sound finishes, after stop, or for a stale/destroyed handle.
 * Best-effort — a sound shorter than the caller's poll interval may never be observed as playing. */
BW_API bool     bw_source_is_playing(BwEngine* e, BwSource s);
BW_API void     bw_play_oneshot(BwEngine* e, BwSound snd, float x, float y, float z, float gain);

/* ---- ambisonic beds (control thread; a world-locked soundfield decoded straight to the speakers,
 * not DBAP-panned — for diffuse/ambient content). Play a bw_load_ambix asset; no position. Occlusion
 * and directivity do NOT apply to a bed (it is world-locked diffuse). bw_bed_play requires a
 * multichannel asset and bw_source_play a mono one — a mismatch is rejected (see bw_last_error). ---- */
BW_API BwBed bw_bed_create(BwEngine* e);
BW_API void  bw_bed_play(BwEngine* e, BwBed b, BwSound snd, bool loop);
BW_API void  bw_bed_set_gain(BwEngine* e, BwBed b, float linear);   /* master gain, ramped */
/* Yaw the bed's soundfield about the room's vertical axis (radians; positive turns the field from
 * room +z/front toward room +x): line a capture up with the scene, or rotate it slowly for effect.
 * Closed-form yaw SH rotation (each degree's +-m pair rotates by m*yaw — exact, all orders), glides
 * to the target at ~one turn/s (click-free, live), applied before EITHER bed renderer (matrix and
 * parametric see the same turned field). Per-frame-safe. */
BW_API void  bw_bed_set_rotation(BwEngine* e, BwBed b, float yaw_rad);
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
/* Convenience: a shoebox enclosure of size w x h x d metres, FLOOR-based — centred on the origin
 * in x/z, y from 0 (the floor) up to h — with one material per face. faces[6] = (-x,+x,-y,+y,-z,+z);
 * each a BwMaterial token (0 = default). Triangle normals face inward (the listener is inside).
 * Load-time. No-op without the Steam Audio backend. */
BW_API void     bw_scene_set_box(BwEngine* e, float w, float h, float d, const BwMaterial faces[6]);
/* Enable per-source occlusion: geometry between the source and listener attenuates it (ramped).
 * Per-frame-safe. No-op without the Steam Audio backend. */
BW_API void     bw_source_set_occlusion(BwEngine* e, BwSource s, bool on);
/* MANUAL occlusion (no SDK needed): drive the same handle-gated, audio-thread-ramped path the sim
 * publishes through, from your own game logic — "behind a door the gameplay knows about",
 * underwater, muffled-by-menu. `level` is broadband transmittance (1 = clear .. 0 = blocked);
 * `bands` (optional, NULL = broadband only) is a low/mid/high tilt in [0,1] rendered as the same
 * 3-biquad transmission EQ the sim uses (so a wall MUFFLES, not just attenuates). Everything ramps.
 * Do not drive a source from both this and the sim (bw_source_set_occlusion) — the sim republishes
 * every tick and wins. Per-frame-safe. */
BW_API void     bw_source_set_occlusion_manual(BwEngine* e, BwSource s, float level, const float bands[3]);

/* ---- reflection bed (materials; needs the Steam Audio build) ----
 * A single shared listener-centric reverb bed (Steam Audio hybrid reverb), decoded straight to the
 * speakers and summed onto the bus. Configure at LOAD time (the IR length + order are baked at
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
/* Set the reverb wet level (linear; 1 = unity) live. Per-frame-safe. No-op if no bed is running.
 * Applies to whichever reverb bed owns the tap — the Steam bed or the FDN below. */
BW_API void     bw_reflections_set_gain(BwEngine* e, float linear);

/* ---- directional FDN reverb bed (load-time; no SDK needed) ----
 * A phonon-free late-reverb alternative to the Steam reflection bed: a 16-line feedback delay
 * network whose lines are rendered as plane waves through the layout's SH->speaker bed decode, fed by
 * the SAME mono aux send (bw_source_set_reflections + the per-source send levels apply unchanged).
 * The decay is a DESIGN parameter: set what the content wants. Don't copy the room's measured RT60 —
 * the real room adds its own decay on top (docs/calibration.md). Decay can be ANISOTROPIC: scale the decay time toward a
 * direction (factor < 1 = the field dies faster that way — an open or treated side), the diagonal
 * special case of the Directional FDN (Alary/Politis/Schlecht, JAES 2019). Enabling it takes the
 * reverb tap INSTEAD of the Steam bed (one reverb bed at a time); works in no-SDK builds — the
 * playground's reverb scene runs without phonon. All three calls are load-time (between bw_create
 * and bw_start); defaults: 1.2 s low / 0.7 s high @ 2 kHz crossover, uniform, unity return. */
BW_API void     bw_reverb_fdn(BwEngine* e, bool on);
BW_API void     bw_fdn_set_decay(BwEngine* e, float rt60_low_s, float rt60_high_s, float xover_hz);
BW_API void     bw_fdn_set_decay_direction(BwEngine* e, const float dir[3], float factor);
/* Opt a source into the shared bed's wet send (per-frame-safe, enqueue-only). With the bed disabled
 * or no SDK, this just gates a send that goes nowhere. */
BW_API void     bw_source_set_reflections(BwEngine* e, BwSource s, bool on);
/* Per-source wet-send LEVEL (default 1.0; 0 = none). Scales how much of this source feeds the shared
 * reverb bed — drive it yourself for a manual dry/wet, or use the distance mode below. Per-frame-safe. */
BW_API void     bw_source_set_reflection_send(BwEngine* e, BwSource s, float gain);
/* Distance->wet: when on, the engine scales this source's send by its distance to the listener (near =
 * drier, far = wetter), on top of the level above. Off by default (constant send). Per-frame-safe. */
BW_API void     bw_source_set_reflection_distance(BwEngine* e, BwSource s, bool on);
/* Opt a source into sound PATHING: when the direct line is blocked, its sound is routed to the
 * listener along indirect paths (around occluders / through openings) and decoded to the array from
 * those arrival directions. Requires the scene geometry + BWAUDIO_PATHING at start; no-op otherwise.
 * Per-frame-safe (enqueue-only). The indirect field updates at the sim rate (~10 Hz), ramped. */
BW_API void     bw_source_set_pathing(BwEngine* e, BwSource s, bool on);
/* Read the source's current occlusion factor (1 = clear .. 0 = fully blocked) — for HUD/diagnostics. */
BW_API float    bw_source_get_occlusion(BwEngine* e, BwSource s);

/* ---- directivity (control thread; needs the Steam Audio build) ----
 * Source radiation pattern. Sources are omni by default; set a weighted-dipole pattern + the
 * source's forward orientation, and a listener off the source's forward axis hears it attenuated.
 * Independent of occlusion (a source can be directional without being occluded). */
typedef enum { BW_DIR_OMNI = 0, BW_DIR_CARDIOID = 1, BW_DIR_FIGURE8 = 2 } BwDirectivity;
/* Source orientation as a quaternion (same room frame + handedness as bw_set_listener_pose); the
 * dipole axis is the source's forward (+z rotated by q). Per-frame-safe. No-op without the SDK. */
BW_API void     bw_source_set_orientation(BwEngine* e, BwSource s, float qx, float qy, float qz, float qw);
/* Radiation pattern: weight 0=omni (off) .. 0.5=cardioid .. 1=figure-8; power>=1 sharpens the lobe.
 * Per-frame-safe. No-op without the Steam Audio backend. */
BW_API void     bw_source_set_directivity(BwEngine* e, BwSource s, float weight, float power);
/* Named-preset sugar over bw_source_set_directivity (OMNI disables it). */
BW_API void     bw_source_set_directivity_preset(BwEngine* e, BwSource s, BwDirectivity pattern);
/* Read the source's current directivity gain (1 = on-axis/omni .. 0 = full null) — HUD/diagnostics. */
BW_API float    bw_source_get_directivity(BwEngine* e, BwSource s);

/* ---- propagation effects (control thread; no SDK needed — pure per-voice DSP) ----
 * Physically-motivated, opt-in per source; both derive from the live source<->listener distance.
 * Per-frame-safe (enqueue-only), default OFF, independent of the panner/profile and of each other. */
/* Doppler: render the source through its acoustic propagation delay (distance/c). As the source (or
 * the tracked listener) moves, the delay glides and the audio resamples — pitch up when approaching,
 * down when receding. The delay (hence the effect) saturates past ~8 m; enabling adds the real
 * propagation latency. Best for fast movers; subtle for slow ones in a small room. */
BW_API void     bw_source_set_doppler(BwEngine* e, BwSource s, bool on);
/* Air absorption: a distance-driven high-frequency low-pass (far sources sound duller). Subtle at a
 * few metres, pronounced for sources placed at large virtual distances. */
BW_API void     bw_source_set_air_absorption(BwEngine* e, BwSource s, bool on);
/* Equal-loudness distance compensation: as distance attenuation takes level away, the ear also loses
 * LF sensitivity (ISO 226 contours), so an attenuated source reads THIN as well as far. This restores
 * part of the body with an LF shelf that tracks the attenuation (+0.4 dB per dB taken, capped +8 dB) —
 * "far, not tinny". A perceptual stylization, not physics; leave it off for strict realism. Direct
 * path only (like air/Doppler); ramped; per-frame-safe. */
BW_API void     bw_source_set_loudness_comp(BwEngine* e, BwSource s, bool on);
/* Source spread / size: angular width of the source, 0 = a point (default) .. 1 = wide. Spreads the
 * source's energy across the speakers around its direction (a waterfall/crowd/ambience that shouldn't
 * collapse to one point), centred on its direction and constant-power. Works with any panner. */
BW_API void     bw_source_set_spread(BwEngine* e, BwSource s, float amount);
/* Source size in METERS (radius; 0 = point, the default) — the physical alternative to the angular
 * spread above. The rendered width is the angle the radius subtends from the tracked listener, so a
 * 2 m waterfall STAYS 2 m wide as the listener walks (an angular spread changes physical size with
 * distance), and a source that engulfs the listener (dist < radius) goes fully wide. Floors spread
 * (the larger of the two wins), rides the selected spread mode + decorrelation, and subsumes
 * bw_set_near_spread for sized sources. Per-frame-safe. */
BW_API void     bw_source_set_size(BwEngine* e, BwSource s, float radius_m);

/* ---- channel test / diagnostics (control thread; no SDK needed) ----
 * Drive a single OUTPUT channel with a built-in test signal, injected AFTER the per-speaker align
 * stage (a raw value straight on the channel) — a speaker-check / wiring-verification / calibration
 * tool, NOT a spatial path (it bypasses the panner; don't use it to "place" audio). `channel` is in
 * [0, bw_channel_count()). Per-frame-safe, next block, no bw_commit needed; multiple channels at once.
 * gain 0 or BW_TEST_OFF silences a channel. Composes with the profiles: cave/both -> a raw tone on
 * that DVS channel/speaker; binaural -> that bus channel HRTF'd as its virtual speaker. */
typedef enum { BW_TEST_OFF = 0, BW_TEST_SINE = 1, BW_TEST_NOISE = 2 } BwTestKind;
BW_API void     bw_test_signal(BwEngine* e, uint32_t channel, BwTestKind kind, float gain);

/* Read back the effective speaker layout (the default grid, or the file from BwConfig.layout_path):
 * fills `xyz` with up to `cap` speakers' positions (3 floats each: x,y,z room space, in channel/index
 * order) and returns the total count (== bw_channel_count()). Pass xyz=NULL to just query the count. For visualizing or
 * auditioning the geometry the engine is actually panning with. Control thread; safe any time. */
BW_API uint32_t bw_get_speakers(BwEngine* e, float* xyz, uint32_t cap);

/* Per-channel output meter: fills `peaks` with up to `cap` channels' LAST-BLOCK peak |sample|
 * (linear), measured at the very end of the render — after align, the test signal, and the limiter,
 * i.e. exactly what each device channel received. Returns the count filled. Control thread,
 * per-frame-safe (relaxed atomic reads; no locks/alloc) — drive channel meters or a speaker-activity
 * display (the playground lights each speaker gizmo with it). Reads 0 until audio is running. */
BW_API uint32_t bw_get_bus_levels(BwEngine* e, float* peaks, uint32_t cap);
/* Last block's ACTIVE voice count (playing, sound bound) — a voice-pool gauge for HUDs/health
 * monitoring next to the meters. Control thread, per-frame-safe; 0 until audio runs. */
BW_API uint32_t bw_get_active_voices(BwEngine* e);

/* ---- panner selection (load-time, or live: the switch is atomic) ----
 * The per-source panner that writes the speaker bus. DBAP (default) is listener-relative, recomputed
 * per frame from the tracked position — for a MOVING observer roaming the array. SPCAP is a smooth,
 * all-speaker, placement-correcting sweet-spot panner for a FIXED observer (a static listener: don't
 * track, set the sweet spot once); it conserves loudspeaker power across an uneven array. VBAP is the
 * sharpest (the 2-3 nearest speakers of the containing triangle carry a source) — also fixed-observer,
 * best when the array triangulates cleanly; it falls back to DBAP for a non-triangulable array. SPCAP
 * and VBAP assume a fixed listener (with internal tracking their cache rebuilds every block — use DBAP
 * for a moving observer). Does not affect the diffuse bed / ambisonic paths. See docs/spatialization.md. */
typedef enum { BW_PAN_DBAP = 0, BW_PAN_SPCAP = 1, BW_PAN_VBAP = 2 } BwPanner;
BW_API void     bw_set_panner(BwEngine* e, BwPanner panner);
/* The engine's ACTIVE channel count = the layout's speaker count (4..26; the 26-grid default with no
 * layout_path). Fixed for the engine's lifetime — size meter/speaker arrays with it. BW_CHANNELS(26)
 * is only the compile-time CAPACITY; a collaborator's 24-speaker layout loads into the same binary.
 * NOTE: a failed layout load falls back to the 26 default (non-fatal, see bw_last_error) — which now
 * also means a different channel count, so smaller installs must check bw_last_error at create. */
BW_API uint32_t bw_channel_count(BwEngine* e);
/* Dual-band panning (off by default): split each source at ~700 Hz and pan the low band with
 * amplitude (pressure / velocity-vector) normalisation, the high band with the panner's usual power
 * (energy-vector) normalisation — better low-frequency localisation for a near-centred listener. Wraps
 * the selected panner; live-toggleable for A/B. Sweet-spot dependent like VBAP (see docs). */
BW_API void     bw_set_dual_band(BwEngine* e, bool on);
/* How bw_source_set_spread renders a source's width (live A/B; sources with spread 0 are unaffected).
 * LOBE (default) reshapes the point gains toward a width-controlled lobe — one solve, smooth,
 * approximate. MDAP (Pulkki's multiple-direction amplitude panning) pans a ring of virtual sources
 * around the source direction with the selected panner and sums — the extent is made of real panner
 * solves (panner-true: VBAP stays sparse, SPCAP stays placement-corrected), at ~13x the gain-solve
 * cost (block-rate + dirty-gated, still cheap). Both are constant-power. See docs/spatialization.md. */
typedef enum { BW_SPREAD_LOBE = 0, BW_SPREAD_MDAP = 1 } BwSpreadMode;
BW_API void     bw_set_spread_mode(BwEngine* e, BwSpreadMode mode);
/* Decorrelate the WIDE part of spread sources (off by default; live A/B). Amplitude panning feeds
 * every speaker the SAME signal, so a wide source still collapses to phantom images and comb-filters
 * as the tracked listener walks. With this on, a spread source's energy splits: the coherent share
 * stays on the normal path, the rest passes through per-speaker sparse velvet-noise filters
 * (~30 taps, time-domain, no latency) so the copies are mutually incoherent — real extent, stable
 * timbre while moving. Constant-power (incoherent energy adds); the split follows spread (0 = all
 * coherent). Point sources (spread 0) are untouched. */
BW_API void     bw_set_decorrelation(BwEngine* e, bool on);
/* Near-listener widening (0 = off, the default): floor every source's spread at 1 - dist/radius, so
 * a source flying at the head WIDENS (it subtends a growing angle) instead of collapsing into the
 * nearest speaker and snapping across it. radius_m ~ 1.0 is a good start; the widened part follows
 * the selected spread mode and (when enabled) decorrelates. Engine-wide policy; live-safe. */
BW_API void     bw_set_near_spread(BwEngine* e, float radius_m);

/* ---- output protection limiter (ON by default at -1 dBFS) ----
 * The final stage on the speaker output — everything (voices, beds, reflections, pathing, per-speaker
 * trims, the test signal) passes through it before the device. LINKED across channels: one gain from
 * the cross-channel peak, so engaging never shifts the spatial image; ~1 ms attack / ~120 ms release,
 * then a hard clamp at the ceiling. This is driver/speaker protection against digital overs, not a
 * mastering limiter — if it engages in normal use, turn the content down. Control thread; live. */
BW_API void     bw_set_limiter(BwEngine* e, bool on);
BW_API void     bw_set_limiter_ceiling(BwEngine* e, float ceiling_db);   /* e.g. -1.0f; clamped to [-60, 0] */

/* Master gain: one ramped scalar over the whole mix — voices, beds, reverb/pathing — applied BEFORE
 * the per-speaker align stage (trims and the raw channel-test signal stay calibrated) and before the
 * limiter (which still guards the sum). The volume knob / scene-fade control the API previously
 * lacked. Live, per-frame-safe; ramps across a block, so slider drags never zipper. */
BW_API void     bw_set_master_gain(BwEngine* e, float linear);

/* Select the diffuse-bed ambisonic decoder (load-time: between bw_create and bw_start). SAMPLING is the
 * default projection decode; ALLRAD (All-Round Ambisonic Decoding) decodes to a uniform virtual layout
 * then VBAPs onto the real array — robust on an IRREGULAR/lopsided array (keeps a diffuse field even
 * with no loud directions, and localizes better), at the cost of a heavier load-time build. Affects the
 * ambisonic + reflection BEDS only, not the point-source panner. See docs/spatialization.md. */
typedef enum { BW_DECODE_SAMPLING = 0, BW_DECODE_ALLRAD = 1 } BwBedDecoder;
BW_API void     bw_set_bed_decoder(BwEngine* e, BwBedDecoder decoder);

/* Select how ambisonic BEDS render (live: each bed crossfades to the selection — a click-free A/B).
 * MATRIX (default) is the static SH->speaker decode (sampling or AllRAD per bw_set_bed_decoder) — cheap,
 * world-locked, sweet-spot-ish. PARAMETRIC analyzes the bed's first-order channels per frequency
 * band into a direction + diffuseness (DirAC-style intensity analysis): the non-diffuse stream is
 * RE-PANNED through the engine's listener-relative panner at the array shell — a recorded soundfield
 * becomes WALKABLE (off-centre listeners get correct directions + parallax, which no matrix decode
 * can give) — and the diffuse stream decodes through the matrix into per-speaker decorrelators
 * (incoherent envelopment). Loudness-matched to the matrix decode; beds with < 4 channels stay on
 * the matrix. See docs/spatialization.md. */
typedef enum { BW_BED_MATRIX = 0, BW_BED_PARAMETRIC = 1 } BwBedRenderer;
BW_API void     bw_set_bed_renderer(BwEngine* e, BwBedRenderer renderer);

/* Tracked room EQ (layouts carrying a room_eq_grid, written by bw_calibrate --room-eq-grid): the LF
 * modal cuts are re-interpolated at the LIVE listener position each block and the per-speaker biquads
 * glide toward them — room correction that survives a moving/tracked listener, unlike the static
 * per-speaker room_eq (which bw_start rejects for moving sessions). ON by default when a grid is
 * present; this is the live kill switch (off glides every cut to flat — a click-free A/B). A no-op
 * for layouts without a grid. Control thread, per-frame-safe. See docs/calibration.md. */
BW_API void     bw_set_tracked_room_eq(BwEngine* e, bool on);

/* ---- offline panner evaluation (no engine handle; for layout scoring/optimization in tools) ----
 * The per-speaker gains the given `panner` produces for `nsrc` source positions heard from one
 * listener `lis`, over a layout of `n` speaker positions (`positions` = n*3 floats, room space). Writes
 * out[i*n + s] (nsrc*n floats); returns nsrc. Default DBAP/distance tuning. Shares the SPCAP/VBAP
 * per-listener cache across the batch (efficient over a grid). Pure — uses the same panner solves the
 * audio path does, so a tool scores a layout against the ACTUAL panner, not a copy. */
BW_API uint32_t bw_panner_gains_batch(BwPanner panner, const float* positions, uint32_t n,
                                      const float lis[3], const float* srcs, uint32_t nsrc, float* out);

/* ---- listener (control thread; skip if track_internal) ---- */
/* Position in room space. Quaternion is head orientation; used by the binaural
 * monitor only — the array render ignores orientation (real speakers, real ears).
 * ROOM FRAME: right-handed, +y up, metres, identity orientation faces +z (so the
 * right ear is at -x). The origin sits ON THE FLOOR at the working-area centre
 * (x/z) — Motive's ground-plane calibration — so OptiTrack rigid-body poses pass
 * through unchanged and y is height above the floor. The engine's world-locked
 * decodes reference the ARRAY CENTROID (the nominal listening point, also the
 * default listener position), not the origin, so nothing breaks if a survey
 * places the origin elsewhere. */
/* The identity-listener basis, as data — derive "forward"/"right" from these
 * (the engine's own orientation seams do) rather than re-hardcoding the
 * convention. An identity quaternion's ahead is BW_ROOM_AHEAD, etc. */
static const float BW_ROOM_AHEAD[3] = {  0.0f, 0.0f, 1.0f };
static const float BW_ROOM_UP[3]    = {  0.0f, 1.0f, 0.0f };
static const float BW_ROOM_RIGHT[3] = { -1.0f, 0.0f, 0.0f };
BW_API void bw_set_listener_pose(BwEngine* e, float px, float py, float pz,
                                              float qx, float qy, float qz, float qw);

/* Read back the listener pose the engine is currently rendering with — the committed pose, or,
 * under track_internal, the freshest tracked pose. For visuals/logging/debugging tracking; safe
 * to poll from the control thread. Fills p[3] (position) and q[4] (orientation xyzw). */
BW_API void bw_get_listener_pose(BwEngine* e, float p[3], float q[4]);

/* Pose prediction (track_internal only; 0 = off, the default): extrapolate the tracked POSITION by
 * `lead_ms` along a velocity estimated from the tracker's own frame timestamps (smoothed ~100 ms,
 * speed-capped, reset across drop-outs). The tracking chain — Motive solve, network, the audio
 * block, the DAC — puts the rendered pose 20-40 ms behind the head; at walking speed that is a few
 * cm of panning lag this hides. Set lead_ms to your measured motion-to-ears latency; too much lead
 * OVERSHOOTS on direction changes (clamped at 200 ms). Live-safe. */
BW_API void bw_set_pose_prediction(BwEngine* e, float lead_ms);

/* Extra listeners (multi-occupant compromise panning; 0 = off, the default). A CAVE usually holds
 * more than one person, and single-listener panning is exact for the tracked head and wrong for
 * everyone else. Give the OTHER occupants' positions here (up to 3, `xyz` = count*3 floats, room
 * space; the primary listener stays bw_set_listener_pose / tracking): every source's gains become
 * the per-speaker ENERGY MEAN of the per-listener solves — each occupant hears the image biased
 * toward their own solve instead of one exact + N wrong. Constant-power; works with every panner
 * (each extra gets its own SPCAP/VBAP cache). Spread/Doppler/air and the monitor stay primary-
 * relative. Per-frame-safe, commit-gated like the pose; count 0 restores single-listener panning. */
BW_API void bw_set_extra_listeners(BwEngine* e, const float* xyz, uint32_t count);

/* ---- frame boundary ----
 * Promotes this frame's position/pose updates as one coherent snapshot and drains
 * the event ring (voice-ended, sound-retired). Call once per frame after pushing
 * all source + listener updates. */
BW_API void bw_commit(BwEngine* e);

#ifdef __cplusplus
}
#endif
#endif /* BWAUDIO_H */
