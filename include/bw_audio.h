/*
 * bw_audio.h — public C ABI for the spatial audio core.
 *
 * Self-hosted native audio engine driving a 26-speaker CAVE array via ASIO/Dante,
 * with a binaural (HRTF) debug monitor. Engines (Unity/Unreal) are thin control
 * clients; no audio buffers cross this boundary — only control (sound triggers,
 * source positions, the listener pose).
 *
 * Usage docs: docs/api.md — quickstart, profiles, the threading contract,
 * coordinates, error handling, environment variables, and the per-call
 * reference. examples/minimal.c runs the whole client lifecycle; examples/ambisonic.c
 * walks the bed API (AmbiX/FuMa, rotation, renderer/max-rE A/Bs); examples/streaming.c
 * walks disk streaming + push sources.
 *
 * Naming follows the sokol / miniaudio conventions: the symbol prefix is `bwa_`
 * ("bw" is the family namespace, "a" is audio — other bw_* libraries can sit
 * beside this one), types are lowercase snake_case, config structs are `_desc`,
 * constants are `BWA_`. Internal (non-exported) code shares the same prefix.
 *
 * THREADING CONTRACT (see docs/concurrency.md — this is load-bearing):
 *   - All bwa_* calls must come from ONE control thread (the engine main thread).
 *   - Every per-frame call is non-blocking: it encodes a command onto an SPSC
 *     ring that the audio thread drains. It does NOT touch DSP state.
 *   - Only create/start/load/source_create/tracker_connect may allocate or do
 *     I/O. Do these at load time, never mid-frame in a hot loop.
 *   - Position/pose are latest-wins; push them every frame, then bwa_commit once.
 *
 * LICENSING NOTE: links the Steinberg ASIO SDK (GPLv3 option, see docs/build.md)
 * and Steam Audio. Distribution implications are copyleft — read docs/build.md.
 */
#ifndef BW_AUDIO_H
#define BW_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #ifdef BWA_BUILD_DLL
    #define BWA_API __declspec(dllexport)
  #else
    #define BWA_API __declspec(dllimport)
  #endif
#else
  #define BWA_API
#endif

typedef struct bwa_engine bwa_engine;   /* opaque */

/* Handles are (index | generation<<16). A stale handle (slot destroyed and
 * reused) fails validation on the audio side and is silently dropped. 0 = invalid. */
typedef uint32_t bwa_sound;
typedef uint32_t bwa_source;
typedef uint32_t bwa_bed;       /* an ambisonic-bed voice (world-locked soundfield); see bwa_bed_* */

typedef enum {
    BWA_PROFILE_CAVE     = 0,  /* speaker bus -> ASIO/DVS. Listener POSITION only. */
    BWA_PROFILE_BINAURAL = 1,  /* speaker bus -> binaural monitor -> stereo device. Full POSE. */
    BWA_PROFILE_BOTH     = 2,  /* array to DVS + binaural tap to a stereo device. Full POSE. */
} bwa_profile;

/* Diffuse-bed ambisonic decoder (fixed for the engine's lifetime — the decode matrix is built
 * before start). ALLRAD (All-Round Ambisonic Decoding, the default) decodes to a uniform virtual
 * layout then VBAPs onto the real array — robust on an IRREGULAR/lopsided array, localizes a
 * touch sharper. EPAD (Energy-Preserving Ambisonic Decoding, Zotter/Pomberger/Noisternig 2012)
 * makes a panned plane wave's decoded ENERGY constant over direction by construction — the
 * flattest loudness-vs-direction of the two. Which sounds better on a real array is a by-ear
 * call. The plain sampling (projection) decode is NOT selectable: on an irregular array it
 * over-energises dense speaker regions (dominated by both options above) — it survives only as
 * the engine's automatic fallback when a degenerate layout defeats the chosen build. Affects the
 * ambisonic + reflection BEDS only, not the point-source panner. See docs/spatialization.md. */
typedef enum { BWA_DECODE_ALLRAD = 0, BWA_DECODE_EPAD = 1 } bwa_bed_decoder;

/* Output-device policy. AUTO (the default) tries ASIO and falls back to the silent offline sink
 * — the engine keeps rendering with no device (the tools' visual-only mode). ASIO is an explicit
 * demand: an open failure fails bwa_start loudly instead of hiding behind silence (production, or
 * a speaker audition that must reach real speakers). NULL forces the offline sink (CI, profiling,
 * tracking-only tools). bwa_get_audio_backend reports what actually opened; in binaural/both it
 * also names the monitor in use — "(steam HRTF monitor)" or "(simple-pan monitor)" — because the
 * HRTF decode falls back to the simple pan silently and a by-ear report needs to know which ran. */
typedef enum { BWA_SINK_AUTO = 0, BWA_SINK_ASIO = 1, BWA_SINK_NULL = 2,
               BWA_SINK_MANUAL = 3 /* no device/thread — pump blocks yourself with bwa_render_block */ } bwa_sink_type;

/* Engine configuration. Zero-init and set what you need — every field's zero is its default. */
typedef struct {
    bwa_profile    profile;
    const char*  layout_path;   /* surveyed speaker geometry (JSON). cave/both. */
    const char*  hrtf_path;     /* HRTF (SOFA) or NULL for built-in. binaural/both. */
    uint32_t     sample_rate;   /* 48000 */
    uint32_t     block_size;    /* ASIO buffer hint, e.g. 256/512 */
    bwa_sink_type sink;         /* output-device policy; 0 = AUTO (ASIO, else the silent null sink) */
    const char*  asio_driver;   /* ASIO driver name to open; NULL = auto-pick the first registered
                                 * driver with enough output channels for the profile. */
    bool         embree;        /* ray-trace the acoustics sims on Intel Embree — needs an
                                 * Embree-enabled phonon build; falls back to the default ray
                                 * tracer (one stderr notice) otherwise. No-op without the SDK. */
    bool         enable_pathing;/* run the sound-pathing sim from bwa_start (needs scene geometry +
                                 * the Steam Audio build; sources opt in via bwa_source_set_pathing). */
    bwa_bed_decoder bed_decoder;   /* diffuse-bed SH->speaker decoder; 0 = AllRAD (the default). */
    uint32_t     reserved[4];   /* zero; reserved so the struct can grow without an ABI break */
} bwa_desc;

/* Result codes for the calls that can fail with a reason (everything else reports through
 * bwa_last_error, or is fire-and-forget). 0 = success, nonzero = the failure class. */
typedef enum {
    BWA_OK           = 0,
    BWA_ERR_CONFIG   = 1,   /* invalid bwa_desc / bad call arguments */
    BWA_ERR_DEVICE   = 2,   /* audio device could not be opened / lacked channels / failed to start */
    BWA_ERR_LAYOUT   = 3,   /* reserved: layout failures are currently non-fatal (bwa_last_error) */
    BWA_ERR_HRTF     = 4,   /* reserved: HRTF failures are currently non-fatal (bwa_last_error) */
    BWA_ERR_STATE    = 5,   /* reserved: wrong-state calls currently report via bwa_last_error */
    BWA_ERR_INTERNAL = 6,   /* reserved */
    BWA_ERR_TRACKER  = 7,   /* tracker (NatNet) connection failed — bwa_last_error has the reason */
} bwa_result;

/* ---- lifecycle (control thread; may block/allocate; do at load time) ---- */
BWA_API bwa_engine* bwa_create(const bwa_desc* cfg);
BWA_API bwa_result  bwa_start(bwa_engine* e);   /* opens device(s), starts audio thread */
BWA_API bwa_result  bwa_stop(bwa_engine* e);
BWA_API void      bwa_destroy(bwa_engine* e);
BWA_API const char* bwa_last_error(bwa_engine* e); /* human-readable; NULL if none */
/* Backend actually in use after bwa_start: "asio:<driver>", "null" (offline/SILENT), or
 * "none" (not started). Lets a client confirm it got a real device, not a silent fallback. */
BWA_API const char* bwa_get_audio_backend(bwa_engine* e);

/* ---- ASIO device query (control thread; needs NO engine — call before bwa_create to populate a
 * driver picker for bwa_desc.asio_driver). Reads the OS's registered-driver list fresh each call
 * (a newly installed driver appears immediately); nothing is loaded, initialized, or opened, so
 * it is safe alongside a running engine too. ---- */
BWA_API uint32_t bwa_get_asio_driver_count(void);
/* Copy driver `index`'s registered name into buf (always NUL-terminated, truncated to cap-1) —
 * the exact string bwa_desc.asio_driver expects. False = index out of range or no buffer. */
BWA_API bool     bwa_get_asio_driver_name(uint32_t index, char* buf, uint32_t cap);

/* ---- assets (control thread; file I/O; do at load time) ---- */
BWA_API bwa_sound bwa_load_sound(bwa_engine* e, const char* path); /* mono point-source asset; 0 = failure */
/* Like bwa_load_sound but STREAMED from disk for long files (music/ambience) — the file is not decoded
 * into RAM; a background thread feeds the voice as it plays. Mono (downmixed) at the engine sample rate
 * (a rate mismatch fails — pre-convert, or use bwa_load_sound which resamples). Plays on one voice at a
 * time. 0 = failure. WAV/FLAC/MP3. */
BWA_API bwa_sound bwa_load_sound_streaming(bwa_engine* e, const char* path);
BWA_API void    bwa_unload_sound(bwa_engine* e, bwa_sound snd);    /* safe: retire-acked internally */
/* Load a pre-encoded AmbiX (ACN/SN3D) soundfield, KEEPING its channels (4/9/16 -> order 1/2/3). Plays
 * via bwa_bed_* as a world-locked diffuse bed decoded to the speakers; resampled to the engine rate
 * at load if needed. 0 = failure (bad channel count / decode error). */
BWA_API bwa_sound bwa_load_ambix(bwa_engine* e, const char* path);
/* Like bwa_load_ambix for legacy FuMa B-format recordings (.amb and friends: WXYZ|RSTUV|KLMNOPQ
 * channel order, MaxN + the W -3 dB): converted to AmbiX at load, so past this call the asset is
 * indistinguishable from an AmbiX load of the same field. Full 3D sets only (4/9/16 channels). */
BWA_API bwa_sound bwa_load_fuma(bwa_engine* e, const char* path);
/* Asset metadata (control thread; any time after a successful load). Frames are at the ENGINE
 * rate (in-memory assets were resampled at load; streams must already match), so
 * seconds = frames / bwa_desc.sample_rate. get_frames returns 0 for an invalid handle or a
 * stream whose length is unknown (push sources); get_channels returns 1 for a mono point-source
 * asset, 4/9/16 for an ambisonic bed (order 1/2/3), 0 for an invalid handle. */
BWA_API uint64_t bwa_sound_get_frames(bwa_engine* e, bwa_sound snd);
BWA_API uint32_t bwa_sound_get_channels(bwa_engine* e, bwa_sound snd);

/* ---- sources (control thread; non-blocking, enqueue only) ---- */
BWA_API bwa_source bwa_source_create(bwa_engine* e);              /* handle returned synchronously */
BWA_API void     bwa_source_destroy(bwa_engine* e, bwa_source s);
/* Voice-steal priority, 0 = expendable .. 255 = protected (default 128). When the voice pool is full,
 * bwa_source_create stops the lowest-priority active source to make room for the new one (so an overload
 * drops the least-important sound instead of failing the new one). Set high on music/critical SFX. */
BWA_API void     bwa_source_set_priority(bwa_engine* e, bwa_source s, int priority);
BWA_API void     bwa_source_set_pos(bwa_engine* e, bwa_source s, float x, float y, float z); /* ROOM space, right-handed */
BWA_API void     bwa_source_set_gain(bwa_engine* e, bwa_source s, float linear);
/* Timed gain fade: glide the source's gain to `gain` over `seconds` (engine-side, so no per-frame
 * scripting; seconds <= 0 sets it immediately). A later bwa_source_set_gain or fade replaces it.
 * bwa_source_fade_out fades to silence and then STOPS the voice (the click-free stop path) — the
 * one-call answer to "fade this out and clean it up". Per-frame-safe. */
BWA_API void     bwa_source_fade_to (bwa_engine* e, bwa_source s, float gain, float seconds);
BWA_API void     bwa_source_fade_out(bwa_engine* e, bwa_source s, float seconds);
/* Mix groups (0..7; sources start in group 0): group gain multiplies into every member's gain solve
 * (ramped), and a paused group ramps out + freezes its members' playheads exactly like per-voice
 * pause — "duck the SFX, keep the dialog", scene-wide category control without touching each source.
 * All per-frame-safe. */
BWA_API void     bwa_source_set_group(bwa_engine* e, bwa_source s, uint32_t group);
BWA_API void     bwa_group_set_gain  (bwa_engine* e, uint32_t group, float linear);
BWA_API void     bwa_group_set_paused(bwa_engine* e, uint32_t group, bool paused);
/* Playback rate (1 = native; clamped to [0.25, 4]): a fractional-cursor linear-interp resample of
 * IN-MEMORY sounds — variation on repeated one-shots, slow-mo, engines. Rate changes GLIDE across a
 * block (a change bends the pitch, never steps it) and compose with Doppler. Streamed sounds ignore
 * it (the stream ring is sequential); beds are unaffected. Per-frame-safe. */
BWA_API void     bwa_source_set_pitch(bwa_engine* e, bwa_source s, float rate);
BWA_API void     bwa_source_play(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop);
/* Sample-accurate scheduled play: begin output exactly when the engine's dsp clock reaches
 * `start_sample` (the voice is silent until then, then starts at the precise in-block sample).
 * Get "now" from bwa_get_dsp_time and add a delay, e.g. play 0.5 s out: bwa_get_dsp_time(e) + sample_rate/2.
 * A start_sample already in the past plays immediately (best-effort). 0 = play now (== bwa_source_play). */
BWA_API void     bwa_source_play_at(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop, uint64_t start_sample);
/* The engine's current dsp-sample clock (most recently rendered block's first sample): the device
 * sample position when running, an internal block counter otherwise. Monotonic. 0 before the first block. */
BWA_API uint64_t bwa_get_dsp_time(bwa_engine* e);
BWA_API void     bwa_source_stop(bwa_engine* e, bwa_source s);
/* Pause/resume the source's voice in place. The gate ramps over one block (~5 ms — no click) and the
 * playhead freezes once silent, so resume continues exactly where pause landed. Works for in-memory,
 * streamed, and bed sounds. A paused voice still reads as playing (it has not ended); bwa_source_play
 * always starts un-paused. */
BWA_API void     bwa_source_set_paused(bwa_engine* e, bwa_source s, bool paused);
/* Global pause (app focus loss, menu pause): EVERY voice ramps out and freezes — memory, streamed,
 * and bed alike — and resume continues exactly. Same semantics as per-voice pause (paused voices
 * still read as playing). Live, per-frame-safe. */
BWA_API void     bwa_set_paused(bwa_engine* e, bool paused);
/* Jump the voice's content position to `frame` (engine-rate frames into the sound). Click-free: the
 * voice ramps out, jumps, ramps back in (~10 ms end to end); on a paused voice the jump is immediate
 * (and stays paused). Past-the-end: loops wrap, one-shots end. In-memory and bed sounds only —
 * streamed sounds ignore it (the stream ring cannot jump). */
BWA_API void     bwa_source_seek(bwa_engine* e, bwa_source s, uint64_t frame);
/* Is the source's voice still producing audio? Control-thread poll (latest-wins readback): true while
 * a sound plays, false once a non-loop sound finishes, after stop, or for a stale/destroyed handle.
 * Best-effort — a sound shorter than the caller's poll interval may never be observed as playing. */
BWA_API bool     bwa_source_is_playing(bwa_engine* e, bwa_source s);
BWA_API void     bwa_play_oneshot(bwa_engine* e, bwa_sound snd, float x, float y, float z, float gain);

/* ---- procedural (push) sources: engine-generated audio, no file ----
 * bwa_source_create_stream returns a source whose voice plays PCM you PUSH — mono float frames at
 * the engine sample rate, through a per-source ring (~1.3 s deep at 48 kHz). The full spatial path
 * applies: position, gain, spread, occlusion, Doppler, groups, fades — every bwa_source_* control
 * except play/seek/pitch (a push source plays what you push; those are rejected/ignored). It starts
 * consuming at create: silence until the first push, and if you fall behind (underrun) it renders
 * silence WITHOUT losing your place — output resumes at the next pushed sample (the stream clock is
 * data-driven: it slips, it never drops). Push from the ONE control thread, like every bwa_* call,
 * at least a frame's worth ahead. bwa_source_push returns the count accepted (< n when the ring is
 * full — pace with bwa_source_push_space). Non-finite samples are written as 0. bwa_source_push_end
 * marks end-of-data: the voice ends (is_playing -> false) once the ring drains and further pushes
 * are refused — a push source is not restartable, create a new one. bwa_source_stop / fade_out end
 * it the same one-way way (the unconsumed remainder is dropped, pushes refused); set_paused is the
 * temporary silence. A full pool can steal a push
 * source like any voice (its pushed audio is dropped); protect an important one with priority 255.
 * bwa_source_destroy releases the ring (safe while playing). */
BWA_API bwa_source bwa_source_create_stream(bwa_engine* e);            /* 0 = failure (see bwa_last_error) */
BWA_API uint32_t   bwa_source_push(bwa_engine* e, bwa_source s, const float* frames, uint32_t n);
BWA_API uint32_t   bwa_source_push_space(bwa_engine* e, bwa_source s);
BWA_API void       bwa_source_push_end(bwa_engine* e, bwa_source s);

/* ---- ambisonic beds (control thread; a world-locked soundfield decoded straight to the speakers,
 * not DBAP-panned — for diffuse/ambient content). Play a bwa_load_ambix asset; no position. Occlusion
 * and directivity do NOT apply to a bed (it is world-locked diffuse). bwa_bed_play requires a
 * multichannel asset and bwa_source_play a mono one — a mismatch is rejected (see bwa_last_error). ---- */
BWA_API bwa_bed bwa_bed_create(bwa_engine* e);
BWA_API void  bwa_bed_play(bwa_engine* e, bwa_bed b, bwa_sound snd, bool loop);
BWA_API void  bwa_bed_set_gain(bwa_engine* e, bwa_bed b, float linear);   /* master gain, ramped */
/* Yaw the bed's soundfield about the room's vertical axis (radians; positive turns the field from
 * room +z/front toward room +x): line a capture up with the scene, or rotate it slowly for effect.
 * Closed-form yaw SH rotation (each degree's +-m pair rotates by m*yaw — exact, all orders), glides
 * to the target at ~one turn/s (click-free, live), applied before EITHER bed renderer (matrix and
 * parametric see the same turned field). Per-frame-safe. */
BWA_API void  bwa_bed_set_rotation(bwa_engine* e, bwa_bed b, float yaw_rad);
/* Full 3-axis orientation (radians; supersedes bwa_bed_set_rotation, which is the yaw-only shorthand
 * and resets pitch/roll to 0): LEVEL or reorient a recorded soundfield whose capture wasn't upright.
 * Yaw as above; positive pitch tilts the field's front (+z) upward; positive roll tilts its top
 * toward the room's right (-x). Applied roll -> pitch -> yaw. Yaw-only stays on the closed-form
 * phasor path; any pitch/roll runs a full SH rotation matrix (Ivanic-Ruedenberg), rebuilt per block
 * from angles that glide at ~one turn/s — click-free and live, same as yaw. Per-frame-safe. */
BWA_API void  bwa_bed_set_orientation(bwa_engine* e, bwa_bed b, float yaw_rad, float pitch_rad, float roll_rad);
BWA_API void  bwa_bed_stop(bwa_engine* e, bwa_bed b);
BWA_API void  bwa_bed_destroy(bwa_engine* e, bwa_bed b);
/* The rest of a bed's control surface matches the bwa_source_* call of the same name (a bed IS a
 * voice — same pool, same semantics; these exist so bed code never mixes prefixes): timed fades
 * (fade_out = the click-free stop), pause/seek (resume/jump lands exactly), steal priority (a bed
 * competes for voices like any source — protect a music bed with 255), mix groups, and the
 * is-playing readback. Position/spatial calls (pos, spread, occlusion, ...) do not apply — a bed
 * is world-locked; and pitch is a no-op on beds (see bwa_source_set_pitch). */
BWA_API void  bwa_bed_fade_to (bwa_engine* e, bwa_bed b, float gain, float seconds);
BWA_API void  bwa_bed_fade_out(bwa_engine* e, bwa_bed b, float seconds);
BWA_API void  bwa_bed_set_paused(bwa_engine* e, bwa_bed b, bool paused);
BWA_API void  bwa_bed_seek(bwa_engine* e, bwa_bed b, uint64_t frame);
BWA_API void  bwa_bed_set_priority(bwa_engine* e, bwa_bed b, int priority);
BWA_API void  bwa_bed_set_group(bwa_engine* e, bwa_bed b, uint32_t group);
BWA_API bool  bwa_bed_is_playing(bwa_engine* e, bwa_bed b);

/* ---- materials / occlusion (control thread; needs the Steam Audio build) ---- */

/* An acoustic material — an opaque, engine-scoped token. 0 is always a built-in generic default.
 * Mint materials at LOAD time (the table is fixed-capacity); a token indexes both occlusion
 * (per-band transmission) and the reflection bed (absorption/scattering). */
typedef uint32_t bwa_material;
/* The built-in material presets. Values are ABI: they index the engine's coefficient table. */
typedef enum {
    BWA_MAT_GENERIC = 0, BWA_MAT_BRICK, BWA_MAT_CONCRETE, BWA_MAT_CERAMIC, BWA_MAT_GRAVEL,
    BWA_MAT_CARPET, BWA_MAT_GLASS, BWA_MAT_PLASTER, BWA_MAT_WOOD, BWA_MAT_METAL, BWA_MAT_ROCK,
} bwa_material_type;
/* Mint a material from a built-in preset. GENERIC returns the canonical token 0 without minting
 * (it IS the default — tag as many surfaces as you like without spending table slots). Returns 0
 * also on a full table or an out-of-range value — bwa_last_error distinguishes. The returned token
 * is the same kind of handle bwa_material_define returns; the enum only names the preset. */
BWA_API bwa_material bwa_material_preset(bwa_engine* e, bwa_material_type preset);
/* Mint a custom material: 3-band absorption + 3-band transmission (low/mid/high, each 0..1) and a
 * scattering coefficient (0..1). Returns the new token, reusing a released slot before growing the
 * table; 0 if the (64-slot) table is full. */
BWA_API bwa_material bwa_material_define(bwa_engine* e, const float absorption[3], float scattering, const float transmission[3]);
/* Release a token so bwa_material_define can reuse its slot — the churn escape hatch for the fixed
 * table (long-running apps that mint many distinct materials over their lifetime). CALLER-MANAGED,
 * like free(): only release a token no live mesh or source still references. Meshes copy the material
 * at set time, so already-set geometry is unaffected; a FUTURE bwa_scene_set_mesh_mat with a released
 * token gets whatever the slot is reused for. Token 0 (the default) and out-of-range tokens are
 * refused (bwa_last_error says which). Most apps mint once and never release — this is opt-in. */
BWA_API void         bwa_material_release(bwa_engine* e, bwa_material token);
/* Set the STATIC occluding geometry (room space, RH metres; tris are CCW vertex-index triples) with
 * PER-TRIANGLE materials: tri_material is ntris bwa_material tokens (one per triangle; out-of-range
 * clamps to the default). Replaces any prior static mesh. Safe to call at runtime, but it rebuilds the
 * whole scene BVH — for things that MOVE, use the dynamic-mesh API below (a cheap instance transform).
 * No-op without the Steam Audio backend. */
BWA_API void     bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                                      const bwa_material* tri_material);
/* Convenience: a shoebox enclosure of size w x h x d metres, FLOOR-based — centred on the origin
 * in x/z, y from 0 (the floor) up to h — with one material per face. faces[6] = (-x,+x,-y,+y,-z,+z);
 * each a bwa_material token (0 = default). Triangle normals face inward (the listener is inside).
 * Load-time. No-op without the Steam Audio backend. */
BWA_API void     bwa_scene_set_box(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]);

/* Dynamic (movable) occluders/reflectors — the acoustic analogue of a physics collider with a
 * transform. The static mesh above is committed once (BVH built once); a dynamic mesh is a rigid
 * INSTANCE of its own sub-scene, so moving it is a cheap top-level BVH refit, not a geometry rebuild.
 * Occlusion and REAL-TIME reflections/pathing pick up the motion on their next sim tick (~10-30 Hz);
 * BAKED reflections/pathing do NOT (the bake froze the geometry — see docs/materials.md).
 *
 * add: geometry is in the mover's LOCAL space (metres, CCW), placed by set_dynamic_transform; one
 * `material` token covers every triangle. Returns a handle >= 0, or -1 (no SDK / bad geometry / the
 * movable table is full — bwa_last_error distinguishes). Runtime- and load-time-safe. */
BWA_API int      bwa_scene_add_dynamic_mesh(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                                          bwa_material material);
/* Move/rotate a dynamic mesh (rigid: room-space position + orientation quaternion). Per-frame-safe;
 * unknown/removed handles are ignored. No-op without the Steam Audio backend. */
BWA_API void     bwa_scene_set_dynamic_transform(bwa_engine* e, int handle, float x, float y, float z,
                                               float qx, float qy, float qz, float qw);
/* Remove a dynamic mesh (frees its sub-scene). No-op without the Steam Audio backend. */
BWA_API void     bwa_scene_remove_dynamic_mesh(bwa_engine* e, int handle);

/* Enable per-source occlusion: geometry between the source and listener attenuates it (ramped).
 * Per-frame-safe. No-op without the Steam Audio backend. */
BWA_API void     bwa_source_set_occlusion(bwa_engine* e, bwa_source s, bool on);
/* MANUAL occlusion (no SDK needed): drive the same handle-gated, audio-thread-ramped path the sim
 * publishes through, from your own game logic — "behind a door the gameplay knows about",
 * underwater, muffled-by-menu. `level` is broadband transmittance (1 = clear .. 0 = blocked);
 * `bands` (optional, NULL = broadband only) is a low/mid/high tilt in [0,1] rendered as the same
 * 3-biquad transmission EQ the sim uses (so a wall MUFFLES, not just attenuates). Everything ramps.
 * Do not drive a source from both this and the sim (bwa_source_set_occlusion) — the sim republishes
 * every tick and wins. Per-frame-safe. */
BWA_API void     bwa_source_set_occlusion_manual(bwa_engine* e, bwa_source s, float level, const float bands[3]);

/* ---- reflection bed (materials; needs the Steam Audio build) ----
 * A single shared listener-centric reverb bed (Steam Audio hybrid reverb), decoded straight to the
 * speakers and summed onto the bus. Configure at LOAD time (the IR length + order are baked at
 * bwa_start), then opt sources into its wet send. v1 assumes a static scene (set geometry before
 * bwa_start). No-op without the Steam Audio backend. */
typedef struct {
    float    ir_seconds;     /* reverb tail / IR length; 0 -> default 1.0 (range ~0.5..2.0) */
    uint32_t order;          /* reflection ambisonic order, 1 or 2; 0 -> default 1 */
    uint32_t num_rays;       /* off-thread ray budget; 0 -> default 4096 */
    uint32_t num_bounces;    /* off-thread bounce depth; 0 -> default 16 */
    int      enabled;        /* 0 = no bed created (engine behaves exactly as today) */
    int      bake;           /* non-0: precompute (bake) the reverb over a probe grid at bwa_start,
                              * so the sim thread looks it up instead of ray-tracing live */
    uint32_t reserved[3];    /* zero; reserved so the struct can grow without an ABI break */
} bwa_reflections_desc;
/* Set the reflection config. Load-time only (between bwa_create and bwa_start); copies the struct,
 * zero fields take defaults. No-op without the Steam Audio backend. */
BWA_API void     bwa_reflections_config(bwa_engine* e, const bwa_reflections_desc* cfg);
/* The reverb wet level (linear; 1 = unity, the default) — the ONE control, live, for whichever
 * reverb bed owns the tap (the Steam bed or the FDN below). Valid from bwa_create on: a value set
 * before bwa_start seeds the bed it creates. Per-frame-safe. */
BWA_API void     bwa_reflections_set_gain(bwa_engine* e, float linear);

/* ---- directional FDN reverb bed (load-time; no SDK needed) ----
 * A phonon-free late-reverb alternative to the Steam reflection bed: a 16-line feedback delay
 * network whose lines are rendered as plane waves through the layout's SH->speaker bed decode, fed by
 * the SAME mono aux send (bwa_source_set_reflections + the per-source send levels apply unchanged).
 * The decay is a DESIGN parameter: set what the content wants. Don't copy the room's measured RT60 —
 * the real room adds its own decay on top (docs/calibration.md). Decay can be ANISOTROPIC: scale the decay time toward a
 * direction (factor < 1 = the field dies faster that way — an open or treated side), the diagonal
 * special case of the Directional FDN (Alary/Politis/Schlecht, JAES 2019). Enabling it takes the
 * reverb tap INSTEAD of the Steam bed (one reverb bed at a time); works in no-SDK builds — the
 * playground's reverb scene runs without phonon. Load-time (between bwa_create and bwa_start);
 * zero fields take the defaults; the wet level is bwa_reflections_set_gain (live, default 1). */
typedef struct {
    int      enabled;        /* 0 = no FDN created (the default) */
    float    rt60_low_s;     /* low-band decay time; 0 -> default 1.2 */
    float    rt60_high_s;    /* high-band decay time; 0 -> default 0.7 */
    float    xover_hz;       /* decay-band crossover; 0 -> default 2000 */
    float    decay_dir[3];   /* anisotropy direction (room space); all-zero = uniform decay */
    float    decay_factor;   /* decay-time scale toward decay_dir: < 1 = the field dies faster
                              * that way, > 1 = slower; 0 or 1 = uniform */
    uint32_t reserved[3];    /* zero; reserved so the struct can grow without an ABI break */
} bwa_fdn_desc;
BWA_API void     bwa_fdn_config(bwa_engine* e, const bwa_fdn_desc* cfg);

/* ---- image-source EARLY reflections (per source; no SDK needed) ----
 * The other half of a phonon-free acoustics path: the FDN above renders the late diffuse tail, this
 * renders the six FIRST-ORDER specular reflections — the wall bounces that actually carry room size
 * and source distance. Needs the room: call bwa_scene_set_box (which now captures the shoebox even in
 * a no-SDK build). Each reflection is rendered as a real POINT SOURCE at its mirrored position,
 * panned through the engine's own LISTENER-RELATIVE panner — so reflections have correct direction
 * AND parallax as the listener walks, which no shared reverb bed (Steam's or the FDN's) can give.
 * Path delay, distance attenuation, and per-band wall absorption (walls eat treble, so a reflection
 * is duller than the direct sound) all fall out of the geometry. Delays glide and gains ramp, so a
 * moving source bends its reflections instead of stepping them. Order 1 only by design: higher
 * orders blend into the diffuse field within tens of ms — that is the FDN's job, and it renders
 * them for free. A source outside the room renders dry. Per-frame-safe; the gain is live. */
BWA_API void     bwa_source_set_early_reflections(bwa_engine* e, bwa_source s, bool on);
BWA_API void     bwa_early_reflections_set_gain(bwa_engine* e, float linear);   /* default 1 */
/* Opt a source into the shared bed's wet send (per-frame-safe, enqueue-only). With the bed disabled
 * or no SDK, this just gates a send that goes nowhere. */
BWA_API void     bwa_source_set_reflections(bwa_engine* e, bwa_source s, bool on);
/* Per-source wet-send LEVEL (default 1.0; 0 = none). Scales how much of this source feeds the shared
 * reverb bed — drive it yourself for a manual dry/wet, or use the distance mode below. Per-frame-safe. */
BWA_API void     bwa_source_set_reflection_send(bwa_engine* e, bwa_source s, float gain);
/* Distance->wet: when on, the engine scales this source's send by its distance to the listener (near =
 * drier, far = wetter), on top of the level above. Off by default (constant send). Per-frame-safe. */
BWA_API void     bwa_source_set_reflection_distance(bwa_engine* e, bwa_source s, bool on);
/* Opt a source into sound PATHING: when the direct line is blocked, its sound is routed to the
 * listener along indirect paths (around occluders / through openings) and decoded to the array from
 * those arrival directions. Requires the scene geometry + bwa_desc.enable_pathing; no-op otherwise.
 * Per-frame-safe (enqueue-only). The indirect field updates at the sim rate (~10 Hz), ramped. */
BWA_API void     bwa_source_set_pathing(bwa_engine* e, bwa_source s, bool on);
/* Read the source's current occlusion factor (1 = clear .. 0 = fully blocked) — for HUD/diagnostics. */
BWA_API float    bwa_source_get_occlusion(bwa_engine* e, bwa_source s);

/* ---- directivity (control thread; needs the Steam Audio build) ----
 * Source radiation pattern. Sources are omni by default; set a weighted-dipole pattern + the
 * source's forward orientation, and a listener off the source's forward axis hears it attenuated.
 * Independent of occlusion (a source can be directional without being occluded). */
typedef enum { BWA_DIR_OMNI = 0, BWA_DIR_CARDIOID = 1, BWA_DIR_FIGURE8 = 2 } bwa_directivity;
/* Source orientation as a quaternion (same room frame + handedness as bwa_set_listener_pose); the
 * dipole axis is the source's forward (+z rotated by q). Per-frame-safe. No-op without the SDK. */
BWA_API void     bwa_source_set_orientation(bwa_engine* e, bwa_source s, float qx, float qy, float qz, float qw);
/* Radiation pattern: weight 0=omni (off) .. 0.5=cardioid .. 1=figure-8; power>=1 sharpens the lobe.
 * Per-frame-safe. No-op without the Steam Audio backend. */
BWA_API void     bwa_source_set_directivity(bwa_engine* e, bwa_source s, float weight, float power);
/* Named-preset sugar over bwa_source_set_directivity (OMNI disables it). */
BWA_API void     bwa_source_set_directivity_preset(bwa_engine* e, bwa_source s, bwa_directivity pattern);
/* Read the source's current directivity gain (1 = on-axis/omni .. 0 = full null) — HUD/diagnostics. */
BWA_API float    bwa_source_get_directivity(bwa_engine* e, bwa_source s);

/* ---- propagation effects (control thread; no SDK needed — pure per-voice DSP) ----
 * Physically-motivated, opt-in per source; both derive from the live source<->listener distance.
 * Per-frame-safe (enqueue-only), default OFF, independent of the panner/profile and of each other. */
/* Doppler: render the source through its acoustic propagation delay (distance/c). As the source (or
 * the tracked listener) moves, the delay glides and the audio resamples — pitch up when approaching,
 * down when receding. The delay (hence the effect) saturates past ~8 m; enabling adds the real
 * propagation latency. Best for fast movers; subtle for slow ones in a small room. */
BWA_API void     bwa_source_set_doppler(bwa_engine* e, bwa_source s, bool on);
/* Air absorption: a distance-driven high-frequency low-pass (far sources sound duller). Subtle at a
 * few metres, pronounced for sources placed at large virtual distances. */
BWA_API void     bwa_source_set_air_absorption(bwa_engine* e, bwa_source s, bool on);
/* Equal-loudness distance compensation: as distance attenuation takes level away, the ear also loses
 * LF sensitivity (ISO 226 contours), so an attenuated source reads THIN as well as far. This restores
 * part of the body with an LF shelf that tracks the attenuation (+0.4 dB per dB taken, capped +8 dB) —
 * "far, not tinny". A perceptual stylization, not physics; leave it off for strict realism. Direct
 * path only (like air/Doppler); ramped; per-frame-safe. */
BWA_API void     bwa_source_set_loudness_comp(bwa_engine* e, bwa_source s, bool on);
/* Override the LAYOUT's distance-attenuation curve for one source (mono point sources; a bed has
 * no distance). Same formula as the layout knob: atten = clamp((ref/max(d,ref))^rolloff, min, 1),
 * with d the source→primary-listener distance. rolloff 0 = constant level at any distance (a
 * direction-only cue that never fades); ref_dist <= 0 CLEARS the override (back to the layout
 * curve). Applied by ratio in the gain solve, so it is panner-agnostic and composes with spread,
 * dual-band, decorrelation, and loudness compensation (which tracks the override's own curve);
 * the reflection distance→wet send keeps its own mapping. Per-frame-safe; ramps like any gain. */
BWA_API void     bwa_source_set_attenuation_override(bwa_engine* e, bwa_source s,
                                                     float ref_dist, float rolloff, float min_gain);
/* Source spread / size: angular width of the source, 0 = a point (default) .. 1 = wide. Spreads the
 * source's energy across the speakers around its direction (a waterfall/crowd/ambience that shouldn't
 * collapse to one point), centred on its direction and constant-power. Works with any panner. */
BWA_API void     bwa_source_set_spread(bwa_engine* e, bwa_source s, float amount);
/* Anisotropic extent (BS.2127-style width/height): separate HORIZONTAL and VERTICAL angular extents,
 * each 0..1 — a shoreline is wide but not tall, rain is tall but not wide. Equal values behave as the
 * isotropic spread; bwa_source_set_spread resets to isotropic (last call wins). Width/height are
 * room-referenced (anchored to the room's up axis), so straight overhead the split is inherently
 * ill-defined — BS.2127's polar extent shares the singularity. Rides the selected spread mode, the
 * size/near-widening floors (they floor BOTH axes), and decorrelation; constant-power like spread. */
BWA_API void     bwa_source_set_extent(bwa_engine* e, bwa_source s, float width, float height);
/* Source size in METERS (radius; 0 = point, the default) — the physical alternative to the angular
 * spread above. The rendered width is the angle the radius subtends from the tracked listener, so a
 * 2 m waterfall STAYS 2 m wide as the listener walks (an angular spread changes physical size with
 * distance), and a source that engulfs the listener (dist < radius) goes fully wide. Floors spread
 * (the larger of the two wins), rides the selected spread mode + decorrelation, and subsumes
 * bwa_set_near_spread for sized sources. Per-frame-safe. */
BWA_API void     bwa_source_set_size(bwa_engine* e, bwa_source s, float radius_m);

/* ---- channel test / diagnostics (control thread; no SDK needed) ----
 * Drive a single OUTPUT channel with a built-in test signal, injected AFTER the per-speaker align
 * stage (a raw value straight on the channel) — a speaker-check / wiring-verification / calibration
 * tool, NOT a spatial path (it bypasses the panner; don't use it to "place" audio). `channel` is in
 * [0, bwa_get_channel_count()). Per-frame-safe, next block, no bwa_commit needed; multiple channels at once.
 * gain 0 or BWA_TEST_OFF silences a channel. Composes with the profiles: cave/both -> a raw tone on
 * that DVS channel/speaker; binaural -> that bus channel HRTF'd as its virtual speaker. */
typedef enum { BWA_TEST_OFF = 0, BWA_TEST_SINE = 1, BWA_TEST_NOISE = 2 } bwa_test_kind;
BWA_API void     bwa_set_test_signal(bwa_engine* e, uint32_t channel, bwa_test_kind kind, float gain);

/* Read back the effective speaker layout (the default grid, or the file from bwa_desc.layout_path):
 * fills `xyz` with up to `cap` speakers' positions (3 floats each: x,y,z room space, in channel/index
 * order) and returns the total count (== bwa_get_channel_count()). Pass xyz=NULL to just query the count. For visualizing or
 * auditioning the geometry the engine is actually panning with. Control thread; safe any time. */
BWA_API uint32_t bwa_get_speakers(bwa_engine* e, float* xyz, uint32_t cap);

/* Per-channel output meter: fills `peaks` with up to `cap` channels' LAST-BLOCK peak |sample|
 * (linear), measured at the very end of the render — after align, the test signal, and the limiter,
 * i.e. exactly what each device channel received. Returns the count filled. Control thread,
 * per-frame-safe (relaxed atomic reads; no locks/alloc) — drive channel meters or a speaker-activity
 * display (the playground lights each speaker gizmo with it). Reads 0 until audio is running. */
BWA_API uint32_t bwa_get_bus_levels(bwa_engine* e, float* peaks, uint32_t cap);
/* Last block's ACTIVE voice count (playing, sound bound) — a voice-pool gauge for HUDs/health
 * monitoring next to the meters. Control thread, per-frame-safe; 0 until audio runs. */
BWA_API uint32_t bwa_get_active_voices(bwa_engine* e);

/* ---- output capture (recording / offline sanity + golden checks) ----
 * Tap the FINAL device-bound output — post-limiter, exactly what reaches the device. The callback runs
 * on the AUDIO thread, once per block, with PLANAR channel-major data: `planar[c*nframes + i]` is
 * channel c, sample i. `channels` is the PRIMARY device's channel count — the array count
 * (bwa_get_channel_count) for the 'cave' and 'both' profiles, 2 for 'binaural'. Obey the audio-thread
 * rules: copy out only, no alloc/lock/syscall/file I/O (write to a ring your OWN thread drains to a
 * file/buffer). Pass cb = NULL to stop; keep `user` alive until after the NULL set plus one block.
 * Run the engine on the offline null sink (bwa_desc.sink = BWA_SINK_NULL) for a hardware-free,
 * deterministically-paced render — the basis for golden-audio tests. Note: the async sim layers
 * (Steam occlusion/reflection/pathing) are wall-clock-timed and NOT bit-reproducible, so golden
 * comparisons want the synchronous DSP (or the manual occlusion path) — see docs/api.md. */
typedef void (*bwa_output_fn)(void* user, const float* planar, uint32_t channels, uint32_t nframes);
BWA_API void bwa_set_output_capture(bwa_engine* e, bwa_output_fn cb, void* user);

/* ---- offline / deterministic render (bwa_desc.sink = BWA_SINK_MANUAL) ----
 * With a MANUAL sink, no device or audio thread is created; YOU pump one block at a time on your own
 * thread. Each call renders exactly one block (bwa_desc.block_size frames) of the profile's primary
 * output — binaural: 2 ch; cave/both: bwa_get_channel_count() ch — into engine-owned memory and returns
 * a pointer to it (PLANAR, `channels * nframes` floats, `p[c*nframes + i]`), valid until the next call
 * or bwa_stop. Fills *channels / *nframes (either may be NULL). Returns NULL if the engine isn't started
 * or the sink isn't MANUAL. The timestamp is a pure sample counter (no wall clock), so a fixed input +
 * fixed call sequence renders bit-identically every run — the basis for golden-audio tests. Same caveat
 * as bwa_set_output_capture: the async Steam sims aren't reproducible; keep golden renders on the
 * synchronous DSP (or the manual occlusion path). Control thread. */
BWA_API const float* bwa_render_block(bwa_engine* e, uint32_t* channels, uint32_t* nframes);

/* ---- panner selection (load-time, or live: the switch is atomic) ----
 * The per-source panner that writes the speaker bus. DBAP (default) is listener-relative, recomputed
 * per frame from the tracked position — for a MOVING observer roaming the array. SPCAP is a smooth,
 * all-speaker, placement-correcting sweet-spot panner for a FIXED observer (a static listener: don't
 * track, set the sweet spot once); it conserves loudspeaker power across an uneven array. VBAP is the
 * sharpest (the 2-3 nearest speakers of the containing triangle carry a source) — also fixed-observer,
 * best when the array triangulates cleanly; it falls back to DBAP for a non-triangulable array. SPCAP
 * and VBAP assume a fixed listener (with internal tracking their cache rebuilds every block — use DBAP
 * for a moving observer). Does not affect the diffuse bed / ambisonic paths. See docs/spatialization.md. */
typedef enum { BWA_PAN_DBAP = 0, BWA_PAN_SPCAP = 1, BWA_PAN_VBAP = 2 } bwa_panner;
BWA_API void     bwa_set_panner(bwa_engine* e, bwa_panner panner);
/* The engine's ACTIVE channel count = the layout's speaker count (4..26; the 26-grid default with no
 * layout_path). Fixed for the engine's lifetime — size meter/speaker arrays with it. BWA_CHANNELS(26)
 * is only the compile-time CAPACITY; a collaborator's 24-speaker layout loads into the same binary.
 * NOTE: a failed layout load falls back to the 26 default (non-fatal, see bwa_last_error) — which now
 * also means a different channel count, so smaller installs must check bwa_last_error at create. */
BWA_API uint32_t bwa_get_channel_count(bwa_engine* e);
/* Dual-band panning (off by default): split each source at ~700 Hz and pan the low band with
 * amplitude (pressure / velocity-vector) normalisation, the high band with the panner's usual power
 * (energy-vector) normalisation — better low-frequency localisation for a near-centred listener. Wraps
 * the selected panner; live-toggleable for A/B. Sweet-spot dependent like VBAP (see docs). */
BWA_API void     bwa_set_dual_band(bwa_engine* e, bool on);
/* max-rE weighting for the SH->speaker BED decode (off by default; live A/B, crossfaded): tapers the
 * higher ambisonic orders (Zotter & Frank's psychoacoustic decoder weights, diffuse-energy-matched so
 * A and B stay level-fair), which suppresses decode sidelobes and lengthens the energy vector —
 * better localization AWAY from the sweet spot, exactly the walking-listener case, at a slightly
 * wider main lobe. Reaches every consumer of the engine's own decode: ambisonic beds (matrix
 * renderer), the FDN reverb's line render. Point-source panning (DBAP/SPCAP/VBAP) and phonon's own
 * decodes are untouched. Bake the winner after the hardware bake-off. */
BWA_API void     bwa_set_max_re(bwa_engine* e, bool on);
/* Band-split max-rE (off by default; live A/B, crossfaded; only meaningful with bwa_set_max_re on):
 * apply the taper only ABOVE the ~700 Hz crossover and keep the unweighted (rV-optimal) decode below —
 * the ear localizes LF by summed pressure, where the plain decode maximizes |rV|, and HF by energy,
 * where max-rE wins. The literature-standard Gerzon basic-LF/max-rE-HF split (the broadband taper is
 * the incumbent A/B side). Bed matrix decodes only; the FDN's render stays broadband (a diffuse tail
 * has no LF image to sharpen). */
BWA_API void     bwa_set_max_re_split(bwa_engine* e, bool on);
/* How bwa_source_set_spread renders a source's width (live A/B; sources with spread 0 are unaffected).
 * LOBE (default) reshapes the point gains toward a width-controlled lobe — one solve, smooth,
 * approximate. MDAP (Pulkki's multiple-direction amplitude panning) pans a ring of virtual sources
 * around the source direction with the selected panner and sums — the extent is made of real panner
 * solves (panner-true: VBAP stays sparse, SPCAP stays placement-corrected), at ~13x the gain-solve
 * cost (block-rate + dirty-gated, still cheap). SPECTRAL is frequency-dependent panning (Zotter &
 * Frank's phantom-source widening): the source splits into 6 bands and each band is panned to its
 * own direction inside the spread cone (LF stays on the source direction) — the ear integrates the
 * scattered spectrum into width, with no decorrelation noise and no phantom collapse as the tracked
 * listener walks; costs ~6 band filters + gain sets per wide voice. All are constant-power (SPECTRAL
 * to within its crossover overlap, < ~1 dB). See docs/spatialization.md. */
typedef enum { BWA_SPREAD_LOBE = 0, BWA_SPREAD_MDAP = 1, BWA_SPREAD_SPECTRAL = 2 } bwa_spread_mode;
BWA_API void     bwa_set_spread_mode(bwa_engine* e, bwa_spread_mode mode);
/* Decorrelate the WIDE part of spread sources (off by default; live A/B). Amplitude panning feeds
 * every speaker the SAME signal, so a wide source still collapses to phantom images and comb-filters
 * as the tracked listener walks. With this on, a spread source's energy splits: the coherent share
 * stays on the normal path, the rest passes through per-speaker sparse velvet-noise filters
 * (~30 taps, time-domain, no latency) so the copies are mutually incoherent — real extent, stable
 * timbre while moving. Constant-power (incoherent energy adds); the split follows spread (0 = all
 * coherent). Point sources (spread 0) are untouched. */
BWA_API void     bwa_set_decorrelation(bwa_engine* e, bool on);
/* Near-listener widening (0 = off, the default): floor every source's spread at 1 - dist/radius, so
 * a source flying at the head WIDENS (it subtends a growing angle) instead of collapsing into the
 * nearest speaker and snapping across it. radius_m ~ 1.0 is a good start; the widened part follows
 * the selected spread mode and (when enabled) decorrelates. Engine-wide policy; live-safe. */
BWA_API void     bwa_set_near_spread(bwa_engine* e, float radius_m);

/* ---- output protection limiter (ON by default at -1 dBFS) ----
 * The final stage on the speaker output — everything (voices, beds, reflections, pathing, per-speaker
 * trims, the test signal) passes through it before the device. LINKED across channels: one gain from
 * the cross-channel peak, so engaging never shifts the spatial image; ~1 ms attack / ~120 ms release,
 * then a hard clamp at the ceiling. This is driver/speaker protection against digital overs, not a
 * mastering limiter — if it engages in normal use, turn the content down. Control thread; live. */
BWA_API void     bwa_set_limiter(bwa_engine* e, bool on);
BWA_API void     bwa_set_limiter_ceiling(bwa_engine* e, float ceiling_db);   /* e.g. -1.0f; clamped to [-60, 0] */

/* Master gain: one ramped scalar over the whole mix — voices, beds, reverb/pathing — applied BEFORE
 * the per-speaker align stage (trims and the raw channel-test signal stay calibrated) and before the
 * limiter (which still guards the sum). The volume knob / scene-fade control the API previously
 * lacked. Live, per-frame-safe; ramps across a block, so slider drags never zipper. */
BWA_API void     bwa_set_master_gain(bwa_engine* e, float linear);

/* Select how ambisonic BEDS render (live: each bed crossfades to the selection — a click-free A/B).
 * MATRIX (default) is the static SH->speaker decode (AllRAD or EPAD per bwa_desc.bed_decoder) — cheap,
 * world-locked, sweet-spot-ish. PARAMETRIC analyzes the bed's first-order channels per frequency
 * band into a direction + diffuseness (DirAC-style intensity analysis): the non-diffuse stream is
 * RE-PANNED through the engine's listener-relative panner at the array shell — a recorded soundfield
 * becomes WALKABLE (off-centre listeners get correct directions + parallax, which no matrix decode
 * can give) — and the diffuse stream decodes through the matrix into per-speaker decorrelators
 * (incoherent envelopment). Loudness-matched to the matrix decode; beds with < 4 channels stay on
 * the matrix. See docs/spatialization.md. */
typedef enum { BWA_BED_MATRIX = 0, BWA_BED_PARAMETRIC = 1 } bwa_bed_renderer;
BWA_API void     bwa_set_bed_renderer(bwa_engine* e, bwa_bed_renderer renderer);

/* Tracked room EQ (layouts carrying a room_eq_grid, written by bwa_calibrate --room-eq-grid): the LF
 * modal cuts are re-interpolated at the LIVE listener position each block and the per-speaker biquads
 * glide toward them — room correction that survives a moving/tracked listener, unlike the static
 * per-speaker room_eq (which bwa_start rejects for moving sessions). ON by default when a grid is
 * present; this is the live kill switch (off glides every cut to flat — a click-free A/B). A no-op
 * for layouts without a grid. Control thread, per-frame-safe. See docs/calibration.md. */
BWA_API void     bwa_set_tracked_room_eq(bwa_engine* e, bool on);

/* ---- offline panner evaluation (no engine handle; for layout scoring/optimization in tools) ----
 * The per-speaker gains the given `panner` produces for `nsrc` source positions heard from one
 * listener `lis`, over a layout of `n` speaker positions (`positions` = n*3 floats, room space). Writes
 * out[i*n + s] (nsrc*n floats); returns nsrc. Default DBAP/distance tuning. Shares the SPCAP/VBAP
 * per-listener cache across the batch (efficient over a grid). Pure — uses the same panner solves the
 * audio path does, so a tool scores a layout against the ACTUAL panner, not a copy. */
BWA_API uint32_t bwa_panner_gains_batch(bwa_panner panner, const float* positions, uint32_t n,
                                      const float lis[3], const float* srcs, uint32_t nsrc, float* out);

/* ---- listener (control thread; skip when a tracker is connected) ---- */
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
 * convention. An identity quaternion's ahead is BWA_ROOM_AHEAD, etc. */
static const float BWA_ROOM_AHEAD[3] = {  0.0f, 0.0f, 1.0f };
static const float BWA_ROOM_UP[3]    = {  0.0f, 1.0f, 0.0f };
static const float BWA_ROOM_RIGHT[3] = { -1.0f, 0.0f, 0.0f };
BWA_API void bwa_set_listener_pose(bwa_engine* e, float px, float py, float pz,
                                              float qx, float qy, float qz, float qw);

/* Read back the listener pose the engine is currently rendering with — the committed pose, or,
 * with a tracker connected, the freshest tracked pose. For visuals/logging/debugging tracking;
 * safe to poll from the control thread. Fills p[3] (position) and q[4] (orientation xyzw). */
BWA_API void bwa_get_listener_pose(bwa_engine* e, float p[3], float q[4]);

/* ---- internal tracking (OptiTrack/NatNet; control thread, may block — like the lifecycle calls) ----
 * Connect the engine to a NatNet (Motive) stream and it samples the freshest head pose itself at
 * block time, overriding the committed listener — no per-frame bwa_set_listener_pose needed.
 * Callable ANY time (before or after bwa_start; that is how NatNet clients work everywhere else);
 * a repeat connect replaces the previous connection, disconnect returns to the pushed/committed
 * pose. Zero-init the desc and set what you need — every field's zero is its default. */
typedef struct {
    const char* multicast;       /* NatNet multicast group; NULL -> "239.255.42.99" */
    const char* server;          /* Motive host, for the command handshake + rigid-body names;
                                  * NULL -> multicast-only (no handshake) */
    const char* local_iface;     /* local interface IP to bind; NULL -> any */
    uint16_t    data_port;       /* 0 -> 1511 */
    uint16_t    command_port;    /* 0 -> 1510 */
    int32_t     rigid_body_id;   /* streaming ID to follow; 0 -> first rigid body in the frame */
    const char* rigid_body_name; /* follow by name instead of ID (needs `server`); NULL -> use the ID */
    int32_t     version_major;   /* NatNet bitstream version; 0 -> handshake (or default 3.1) */
    int32_t     version_minor;
    uint32_t    reserved[4];     /* zero; reserved so the struct can grow without an ABI break */
} bwa_tracker_desc;
BWA_API bwa_result bwa_tracker_connect(bwa_engine* e, const bwa_tracker_desc* desc);
BWA_API void       bwa_tracker_disconnect(bwa_engine* e);

/* Pose prediction (internal tracking only — needs a connected tracker; 0 = off, the default):
 * extrapolate the tracked POSITION by
 * `lead_ms` along a velocity estimated from the tracker's own frame timestamps (smoothed ~100 ms,
 * speed-capped, reset across drop-outs). The tracking chain — Motive solve, network, the audio
 * block, the DAC — puts the rendered pose 20-40 ms behind the head; at walking speed that is a few
 * cm of panning lag this hides. Set lead_ms to your measured motion-to-ears latency; too much lead
 * OVERSHOOTS on direction changes (clamped at 200 ms). Live-safe. */
BWA_API void bwa_set_pose_prediction(bwa_engine* e, float lead_ms);

/* Extra listeners (multi-occupant compromise panning; 0 = off, the default). A CAVE usually holds
 * more than one person, and single-listener panning is exact for the tracked head and wrong for
 * everyone else. Give the OTHER occupants' positions here (up to 3, `xyz` = count*3 floats, room
 * space; the primary listener stays bwa_set_listener_pose / tracking): every source's gains become
 * the per-speaker ENERGY MEAN of the per-listener solves — each occupant hears the image biased
 * toward their own solve instead of one exact + N wrong. Constant-power; works with every panner
 * (each extra gets its own SPCAP/VBAP cache). Spread/Doppler/air and the monitor stay primary-
 * relative. Per-frame-safe, commit-gated like the pose; count 0 restores single-listener panning. */
BWA_API void bwa_set_extra_listeners(bwa_engine* e, const float* xyz, uint32_t count);

/* ---- frame boundary ----
 * Promotes this frame's position/pose updates as one coherent snapshot and drains
 * the event ring (voice-ended, sound-retired). Call once per frame after pushing
 * all source + listener updates. */
BWA_API void bwa_commit(bwa_engine* e);

#ifdef __cplusplus
}
#endif
#endif /* BW_AUDIO_H */
