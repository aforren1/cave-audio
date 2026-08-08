/*
 * bw_audio.h — public C ABI for the spatial audio core.
 *
 * Self-hosted native audio engine driving a 26-speaker CAVE array via ASIO/Dante,
 * with binaural (HRTF) headphone rendering: a first-class direct render
 * (BWA_PROFILE_BINAURAL) and an array-audition monitor (BWA_PROFILE_CAVE_SIM).
 * Engines (Unity/Unreal) are thin control clients; no audio buffers cross this
 * boundary — only control (sound triggers, source positions, the listener pose).
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
 * MEMORY CONTRACT: every pointer argument is consumed before the call returns —
 * no call retains caller memory (bwa_create copies its path strings; descs,
 * geometry, and position arrays are copied at call time). Returned pointers are
 * engine-owned; each call's comment states their lifetime.
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

/* Header/DLL ABI version. bwa_get_version() returns the DLL's BWA_VERSION so a client can verify
 * the binary matches the header it compiled against (the desc structs grow via reserved fields,
 * but enum VALUES and struct layouts are only guaranteed within a major.minor). This tracks ABI
 * COMPATIBILITY and is bumped by hand only when the ABI changes - it is deliberately independent of
 * the distribution/release version (the git tag), which moves on its own cadence. */
#define BWA_VERSION_MAJOR 0
#define BWA_VERSION_MINOR 11
#define BWA_VERSION_PATCH 0
#define BWA_VERSION ((BWA_VERSION_MAJOR << 16) | (BWA_VERSION_MINOR << 8) | BWA_VERSION_PATCH)

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

/* Render profile — WHAT the engine renders, fixed at create:
 *   CAVE      drives the physical array. BINAURAL is the first-class headphone render: point
 *             sources (and their ISM reflections) SH-encode at their TRUE listener-relative
 *             directions and HRTF-decode to stereo — no speaker-array simulation in the direct
 *             path, so none of its phantom-source spread. The diffuse layer (ambisonic beds,
 *             the FDN/reflection tails, pathing) still rides the virtual-speaker encode.
 *   CAVE_SIM  auditions the ARRAY RENDER on headphones: every bus channel becomes a virtual
 *             speaker at its surveyed position, DBAP artifacts included — what the room would
 *             do, not the best a headphone can do. CAVE_BOTH is the rig plus that sim tap.
 * BINAURAL/CAVE_SIM open a 2-ch device; CAVE/CAVE_BOTH open the array device. */
typedef enum {
    BWA_PROFILE_CAVE      = 0,  /* speaker bus -> ASIO/Digiface. Listener POSITION only. */
    BWA_PROFILE_BINAURAL  = 1,  /* direct per-source binaural -> stereo device. Full POSE. */
    BWA_PROFILE_CAVE_SIM  = 2,  /* speaker bus -> virtual-speaker monitor -> stereo device. Full POSE. */
    BWA_PROFILE_CAVE_BOTH = 3,  /* array to the Digiface + the CAVE_SIM tap to a stereo device. Full POSE. */
} bwa_profile;

/* Diffuse-bed ambisonic decoder (fixed for the engine's lifetime — the decode matrix is built
 * before start). ALLRAD (All-Round Ambisonic Decoding, the default) decodes to a uniform virtual
 * layout then VBAPs onto the real array — robust on an IRREGULAR/lopsided array, localizes a
 * touch sharper. EPAD (Energy-Preserving Ambisonic Decoding, Zotter/Pomberger/Noisternig 2012)
 * makes a panned plane wave's decoded ENERGY constant over direction by construction — the
 * flattest loudness-vs-direction of the two. Which sounds better on a real array is a by-ear
 * call. The plain sampling (projection) decode is NOT selectable: on an irregular array it
 * over-energizes dense speaker regions (dominated by both options above) — it survives only as
 * the engine's automatic fallback when a degenerate layout defeats the chosen build. Affects the
 * ambisonic + reflection BEDS only, not the point-source panner. See docs/spatialization.md. */
typedef enum { BWA_DECODE_ALLRAD = 0, BWA_DECODE_EPAD = 1 } bwa_bed_decoder;

/* Output-device policy. AUTO (the default) tries ASIO and falls back to the silent offline sink
 * — the engine keeps rendering with no device (the tools' visual-only mode). ASIO is an explicit
 * demand: an open failure fails bwa_start loudly instead of hiding behind silence (production, or
 * a speaker audition that must reach real speakers). NULL forces the offline sink (CI, profiling,
 * tracking-only tools). bwa_get_audio_backend reports what actually opened; the headphone
 * profiles also name the decode in use — "(steam HRTF ...)" or "(simple-pan ...)" — because the
 * HRTF decode falls back to the simple pan silently and a by-ear report needs to know which ran. */
typedef enum { BWA_SINK_AUTO = 0, BWA_SINK_ASIO = 1, BWA_SINK_NULL = 2,
               BWA_SINK_MANUAL = 3 /* no device/thread — pump blocks yourself with bwa_render_block */ } bwa_sink_type;

/* Engine configuration. Zero-init and set what you need — every field's zero is its default. */
typedef struct {
    bwa_profile    profile;
    const char*  layout_path;   /* surveyed speaker geometry (JSON). cave/cave_both. */
    const char*  hrtf_path;     /* HRTF (SOFA) or NULL for built-in. binaural/cave_sim/cave_both. */
    uint32_t     sample_rate;   /* Hz; 0 = 48000. 48 kHz is the validated rate. The DSP is
                                 * rate-derived (96 kHz renders correctly in software), but rates
                                 * above 48 k are unverified against the real Digiface/Dante chain —
                                 * treat 48 kHz as supported until the rig confirms more. */
    uint32_t     block_size;    /* render quantum, frames; 0 = 256. Also the ASIO buffer-size
                                 * HINT — a driver may run its own size (the sinks adapt);
                                 * bwa_get_block_size reads back the engine's resolved quantum. */
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
    BWA_ERR_LAYOUT   = 3,   /* bwa_start: an explicitly-passed layout_path failed to load at
                             * create (the reason is in bwa_last_error). Fix the file — or pass
                             * layout_path = NULL to opt into the 26-grid default deliberately. */
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
/* Human-readable reason for the most recent failure/degradation on this engine; NULL when clean.
 * Lifetime: cleared at ENTRY to the lifecycle/load-class calls (create, start, the loads, the
 * source creates, tracker_connect) — a successful one leaves it NULL — and never touched by
 * per-frame calls. So read it right after the call you are checking: a later successful
 * lifecycle call wipes it. The string stays valid until the next bwa_* call on this engine. */
BWA_API const char* bwa_last_error(bwa_engine* e);
BWA_API uint32_t    bwa_get_version(void);   /* the DLL's BWA_VERSION — check against the header's */
/* Backend actually in use after bwa_start: "asio:<driver>", "null" (offline/SILENT), or
 * "none" (not started); the headphone profiles also name the decode in use — "(steam HRTF
 * direct)" / "(simple-pan direct)" for BINAURAL, "(steam HRTF sim)" / "(simple-pan sim)" for
 * CAVE_SIM/CAVE_BOTH. Human-readable (logs/HUDs); program logic wants bwa_get_sink_type. */
BWA_API const char* bwa_get_audio_backend(bwa_engine* e);
/* The RESOLVED engine config (zero-defaulted desc fields resolved), valid from bwa_create on.
 * Derive time from these — seconds = frames / bwa_get_sample_rate(e) — not from the desc you
 * passed, which may have said 0. The block size is the engine's render quantum (the device may
 * buffer differently; bwa_get_output_latency_frames carries the real render->DAC delay). */
BWA_API uint32_t bwa_get_sample_rate(bwa_engine* e);   /* Hz */
BWA_API uint32_t bwa_get_block_size (bwa_engine* e);   /* frames per render block */
/* The sink actually running — the machine-readable side of bwa_get_audio_backend: after
 * bwa_start, AUTO has resolved to BWA_SINK_ASIO or BWA_SINK_NULL; before start (or after stop)
 * it reports the configured policy. */
BWA_API bwa_sink_type bwa_get_sink_type(bwa_engine* e);

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
 * seconds = frames / bwa_get_sample_rate(e). get_frames returns 0 for an invalid handle or a
 * stream whose length is unknown (push sources); get_channels returns 1 for a mono point-source
 * asset, 4/9/16 for an ambisonic bed (order 1/2/3), 0 for an invalid handle. */
BWA_API uint64_t bwa_sound_get_frames(bwa_engine* e, bwa_sound snd);
BWA_API uint32_t bwa_sound_get_channels(bwa_engine* e, bwa_sound snd);

/* ---- sources (control thread; non-blocking, enqueue only) ----
 * The model: a source drives at most ONE voice. Play on an already-playing source restarts it
 * (and a new sound replaces the old); the same bwa_sound can play on any number of sources at
 * once. Sound-position readback is bwa_source_get_playhead_frames — bwa_source_set_pos below is the
 * SPATIAL position, the playhead is the CONTENT position; they are unrelated. */
BWA_API bwa_source bwa_source_create(bwa_engine* e);              /* handle returned synchronously */
BWA_API void     bwa_source_destroy(bwa_engine* e, bwa_source s);
/* Voice-steal priority, 0 = expendable .. 255 = protected (default 128; out-of-range clamps).
 * When the voice pool is full, bwa_source_create stops the lowest-priority active source to make
 * room for the new one (so an overload drops the least-important sound instead of failing the
 * new one). Set high on music/critical SFX. */
BWA_API void     bwa_source_set_priority(bwa_engine* e, bwa_source s, int priority);
/* ROOM space, right-handed, meters. Commit-gated: takes effect at the next bwa_commit. */
BWA_API void     bwa_source_set_pos(bwa_engine* e, bwa_source s, float x, float y, float z);
BWA_API void     bwa_source_set_gain(bwa_engine* e, bwa_source s, float linear);
/* Timed gain fade: glide the source's gain to `gain` over `seconds` (engine-side, so no per-frame
 * scripting; seconds <= 0 sets it immediately). A later bwa_source_set_gain or fade replaces it.
 * bwa_source_fade_out fades to silence and then STOPS the voice (the click-free stop path) — the
 * one-call answer to "fade this out and clean it up". Per-frame-safe. */
BWA_API void     bwa_source_fade_to (bwa_engine* e, bwa_source s, float gain, float seconds);
BWA_API void     bwa_source_fade_out(bwa_engine* e, bwa_source s, float seconds);
/* Mix groups (ids 0..BWA_GROUPS-1; sources start in group 0): group gain multiplies into every
 * member's gain solve (ramped), and a paused group ramps out + freezes its members' playheads
 * exactly like per-voice pause — "duck the SFX, keep the dialog", scene-wide category control
 * without touching each source. All per-frame-safe. Out of range: set_group falls back to group
 * 0; group gain/pause calls are ignored. */
#define BWA_GROUPS 8
BWA_API void     bwa_source_set_group(bwa_engine* e, bwa_source s, uint32_t group);
BWA_API void     bwa_group_set_gain  (bwa_engine* e, uint32_t group, float linear);
BWA_API void     bwa_group_set_paused(bwa_engine* e, uint32_t group, bool paused);
/* Playback rate (1 = native; clamped to [0.25, 4]): a fractional-cursor linear-interp resample of
 * IN-MEMORY sounds — variation on repeated one-shots, slow-mo, engines. Rate changes GLIDE across a
 * block (a change bends the pitch, never steps it) and compose with Doppler. Streamed sounds ignore
 * it (the stream ring is sequential); beds are unaffected. Per-frame-safe. */
BWA_API void     bwa_source_set_pitch(bwa_engine* e, bwa_source s, float rate);
/* Play `snd` on this source (looping or one-shot). Play on an already-playing source RESTARTS it
 * (new sound, un-paused, from frame 0). The start is click-free: the per-channel gains ramp up from
 * silence over the first block (~5 ms), so re-triggering a source whose asset doesn't begin near
 * zero never pops — the same one-block ramp bwa_source_stop uses to fade out. */
BWA_API void     bwa_source_play(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop);
/* Sample-accurate scheduled play: begin output exactly when the engine's dsp clock reaches
 * `start_sample` (the voice is silent until then, then starts at the precise in-block sample).
 * Get "now" from bwa_get_dsp_time (the clock/scheduling section below) and add a delay, e.g.
 * play 0.5 s out: bwa_get_dsp_time(e) + rate/2. A start_sample already in the past plays
 * immediately (best-effort). 0 = play now (== bwa_source_play). */
BWA_API void     bwa_source_play_at(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop, uint64_t start_sample);
/* Loop a sub-region (the intro->loop pattern): play from the start of the sound but, on reaching
 * loop_end, wrap back to loop_beg instead of the clip end — so a non-repeating intro [0, loop_beg)
 * plays once, then the body [loop_beg, loop_end) loops forever. Frames are engine-rate (seconds =
 * frame / rate). loop_end 0 means the clip end, so loop_beg 0 / loop_end 0 == bwa_source_play(loop
 * = true). Out-of-range bounds or loop_beg >= loop_end fall back to whole-clip looping. In-memory
 * sounds only; a streamed source loops its whole file (the ring is sequential — the region is
 * ignored). Always loops. Same threading + guards as bwa_source_play. The loop seam is a hard wrap
 * (no crossfade), so the loop points want matched endpoints — as with any looped asset. */
BWA_API void     bwa_source_play_loop(bwa_engine* e, bwa_source s, bwa_sound snd, uint64_t loop_beg, uint64_t loop_end);
BWA_API void     bwa_source_stop(bwa_engine* e, bwa_source s);
/* Schedule a click-free stop: when the engine's dsp clock reaches stop_sample, the voice fades to
 * silence over one block (the same click-free path as bwa_source_stop) and ends — never a hard cut,
 * so a scheduled stop cannot pop. Block-granular: the fade begins in the block that contains
 * stop_sample, so silence lands within ~one block (~5 ms at 256/48k) of it. A stop_sample already
 * in the past stops immediately (best-effort). A later bwa_source_play / _play_at / _play_loop on
 * the same source clears a pending stop. Get "now" from bwa_get_dsp_time, like bwa_source_play_at.
 * For push sources use bwa_source_stop / _push_end (their ring is control-thread owned). */
BWA_API void     bwa_source_stop_at(bwa_engine* e, bwa_source s, uint64_t stop_sample);
/* Gapless chaining: queue `snd` to play seamlessly the instant the current sound ends — no silence
 * at the seam (the mixer swaps sounds mid-block if the boundary falls there). Queue several to build
 * a sequence (A ends -> B -> C ...); a queued sound with `loop` = true is the terminal, looping item
 * (an intro clip then a looping body across two files: play(intro, loop=false) then queue(body,
 * loop=true)). Up to 7 pending (further queues drop). QUEUE AFTER the play: bwa_source_play / _at /
 * _loop RESTART the source and clear the queue. Nothing chains after a LOOPING current sound (it
 * never ends) or a stopped one. In-memory mono sounds only, on both ends: a multichannel/streamed/
 * push `snd` is rejected (see bwa_last_error), and a queue behind a streamed current sound is
 * ignored. The playhead (bwa_source_get_playhead_frames) restarts at each chained item; the voice reads
 * as playing throughout. Per-frame-safe. bwa_source_clear_queue drops the pending chain. */
BWA_API void     bwa_source_queue(bwa_engine* e, bwa_source s, bwa_sound snd, bool loop);
BWA_API void     bwa_source_clear_queue(bwa_engine* e, bwa_source s);
/* Pause/resume the source's voice in place. The gate ramps over one block (~5 ms — no click) and the
 * playhead freezes once silent, so resume continues exactly where pause landed. Works for in-memory,
 * streamed, and bed sounds. A paused voice still reads as playing (it has not ended); bwa_source_play
 * always starts un-paused. */
BWA_API void     bwa_source_set_paused(bwa_engine* e, bwa_source s, bool paused);
/* Jump the voice's content position to `frame` (engine-rate frames into the sound). Click-free: the
 * voice ramps out, jumps, ramps back in (~10 ms end to end); on a paused voice the jump is immediate
 * (and stays paused). Past-the-end: loops wrap, one-shots end. In-memory and bed sounds only —
 * streamed sounds ignore it (the stream ring cannot jump). */
BWA_API void     bwa_source_seek(bwa_engine* e, bwa_source s, uint64_t frame);
/* Is the source's voice still producing audio? Control-thread poll (latest-wins readback): true while
 * a sound plays, false once a non-loop sound finishes, after stop, or for a stale/destroyed handle.
 * Best-effort — a sound shorter than the caller's poll interval may never be observed as playing. */
BWA_API bool     bwa_source_is_playing(bwa_engine* e, bwa_source s);
/* The voice's CONTENT playhead in engine-rate frames (seconds = playhead / rate) as of the last
 * rendered block — the engine-owned truth for sync/UI (NOT the spatial position; that is
 * bwa_source_set_pos's domain). It freezes under pause, lands where a seek lands, tracks pitch,
 * wraps with a loop, and for stream/push sources counts frames actually CONSUMED (an underrun
 * slips it, like the audible clock). A finished non-loop voice keeps its final value; an idle
 * voice, a play_at still held silent, or a stale handle reads 0. Block-granular latest-wins
 * readback like is_playing — for tighter-than-a-block timing use bwa_get_dsp_time arithmetic.
 * The full contract: docs/api.md, "The playhead is a poll too". */
BWA_API uint64_t bwa_source_get_playhead_frames(bwa_engine* e, bwa_source s);
/* Fire-and-forget: an internal transient voice at (x,y,z), recycled when the sound ends — no
 * handle to manage. Default priority, and it never STEALS: with the voice pool (or the command
 * ring) momentarily full the oneshot is DROPPED, so spam can't evict named sources.
 * Returns whether it was accepted; false means nothing will be heard, and bwa_last_error says
 * which drop it was (stale handle, multichannel asset, or the full-pool/full-ring case). There
 * is no handle to poll — if you need to track the voice, use bwa_source_create + bwa_source_play. */
BWA_API bool     bwa_play_oneshot(bwa_engine* e, bwa_sound snd, float x, float y, float z, float gain);

/* ---- clock / scheduling (control thread; the time base for bwa_source_play_at) ---- */
/* The engine's dsp-sample clock: the most recently rendered block's first sample — the device
 * sample position when running, an internal block counter otherwise. Monotonic; 0 before the
 * first block. */
BWA_API uint64_t bwa_get_dsp_time(bwa_engine* e);
/* The (output sample position, host time) pair stamped INSIDE the last block callback — the
 * jitter-free wall<->dsp bridge for AV sync (pairing bwa_get_dsp_time with your own clock read
 * carries up to a block of slack; this does not). Map with
 *   dsp_at(T) = dsp_sample + (T_ns - host_time_ns) * rate / 1e9,   then bwa_source_play_at.
 * host_time_ns rides a BACKEND-DEFINED epoch (null sink: from stream start) — anchor it against
 * your own monotonic clock and refresh the offset per frame (clocks drift ~ppm). Returns false
 * (outputs untouched) until a host-stamped block renders; the manual sink stamps a nominal,
 * wall-free time (exact for arithmetic, not real wall time). Lock-free.
 * Full recipe (epochs, graphics-side events, Unity helpers): docs/api.md, "Syncing with graphics". */
BWA_API bool     bwa_get_clock(bwa_engine* e, uint64_t* dsp_sample, uint64_t* host_time_ns);
/* How fast the device clock runs against the host clock. bwa_get_clock's pair is an exact INSTANT,
 * but extrapolating it at the nominal rate drifts — the two are different oscillators, and 10 ppm is
 * 36 ms per hour, which is the whole long-show AV-sync problem. This is the slope: an exponentially
 * weighted least-squares fit (~2 min window) over the per-block stamps, so
 *   dsp_at(T) = dsp_sample + (T_ns - host_time_ns) * rate_hz / 1e9
 * stays true across a show instead of only near the anchor. Re-anchoring per frame off bwa_get_clock
 * needs none of this; a MINUTES-long extrapolation, a drift readout for the rig log, or reconciling
 * with an external master (video, timecode, another node) does. */
typedef struct bwa_clock_model {
    double   ppm;        /* device clock vs host clock, parts per million (+ = device fast) */
    double   ppm_sigma;  /* 1-sigma standard error of `ppm` — the fit's own confidence. Assumes
                          * independent stamp noise, so real correlated jitter makes it optimistic:
                          * read it as a lower bound. Shrinks as span_s grows. */
    double   rate_hz;    /* fitted device rate in samples per host second (nominal x (1 + ppm/1e6)) */
    double   span_s;     /* host seconds of stamps behind the fit; the number is worth trusting to
                          * sub-ppm after a minute or two (watch ppm_sigma, not the clock) */
    double   jitter_ns;  /* rms residual of the stamps about the fit — driver stamp quality. A driver
                          * stamping from its own hardware reads ~microseconds; the QPC-synthesized
                          * fallback (no kSystemTimeValid) carries callback-dispatch jitter on top. */
    uint32_t stamps;     /* effective (exponentially weighted) stamp count in the fit */
} bwa_clock_model;
/* False with `out` untouched until the fit has ~1 s of stamps: before bwa_start, under a backend
 * with no host stamp, and again for ~1 s after a restart re-bases the device sample position (the
 * fit reseeds rather than draw a line through the jump). The manual sink's clock is SYNTHESIZED from
 * the sample position, so it fits ppm = 0 exactly — true of that fiction, not of any hardware.
 * Lock-free; same seqlock as bwa_get_clock, so the pair and the model agree on a block. */
BWA_API bool     bwa_get_clock_model(bwa_engine* e, bwa_clock_model* out);
/* The primary device's self-reported render->DAC latency in FRAMES at the engine rate
 * (ASIOGetLatencies — the Digiface includes its Dante buffering): audio scheduled for dsp time T
 * is HEARD at T + latency. The audio half of AV alignment (the video half — display delay — you
 * measure once; docs/api.md). 0 = unknown or no physical output (null sink, or before
 * bwa_start). Constant while the device is open. */
BWA_API uint32_t bwa_get_output_latency_frames(bwa_engine* e);

/* ---- procedural (push) sources: engine-generated audio, no file (full story: docs/api.md,
 *      "Procedural (push) sources") ----
 * bwa_source_create_push returns a source whose voice plays mono float PCM you PUSH at the engine
 * rate through a per-source ring (65536 frames, ~1.37 s at 48 kHz). A normal source otherwise —
 * position/gain/spread/occlusion/Doppler/groups/fades all apply; play/seek/pitch don't (rejected).
 * It consumes from create: underrun renders silence without losing your place (the clock slips,
 * never drops). Push from the ONE control thread, a frame ahead; bwa_source_push returns the count
 * accepted (< n = ring full, pace with bwa_source_push_space); non-finite samples become 0.
 * bwa_source_push_end ends the voice once the ring drains (one-way — not restartable; stop/fade_out
 * end it the same way, set_paused just silences). destroy releases the ring, safe while playing. */
BWA_API bwa_source bwa_source_create_push(bwa_engine* e);            /* 0 = failure (see bwa_last_error) */
BWA_API uint32_t   bwa_source_push(bwa_engine* e, bwa_source s, const float* frames, uint32_t n);
BWA_API uint32_t   bwa_source_push_space(bwa_engine* e, bwa_source s);
BWA_API void       bwa_source_push_end(bwa_engine* e, bwa_source s);

/* ---- global mix control (control thread; live, per-frame-safe) ---- */
/* Master gain: one ramped scalar over the whole mix — voices, beds, reverb/pathing — applied
 * BEFORE the per-speaker align stage (trims and the raw channel-test signal stay calibrated) and
 * before the limiter (which still guards the sum). The volume knob / scene fade; ramps across a
 * block, so slider drags never zipper. */
BWA_API void     bwa_set_master_gain(bwa_engine* e, float linear);
/* Global pause (app focus loss, menu pause): EVERY voice ramps out and freezes — memory,
 * streamed, and bed alike — and resume continues exactly. Same semantics as per-voice pause
 * (paused voices still read as playing). */
BWA_API void     bwa_set_paused(bwa_engine* e, bool paused);

/* ---- ambisonic beds (control thread; a world-locked soundfield decoded straight to the speakers,
 * not DBAP-panned — for diffuse/ambient content). Play a bwa_load_ambix asset; no position. Occlusion
 * and directivity do NOT apply to a bed (it is world-locked diffuse). bwa_bed_play requires a
 * multichannel asset and bwa_source_play a mono one — a mismatch is rejected (see bwa_last_error). ---- */
BWA_API bwa_bed bwa_bed_create(bwa_engine* e);
BWA_API void  bwa_bed_play(bwa_engine* e, bwa_bed b, bwa_sound snd, bool loop);
BWA_API void  bwa_bed_set_gain(bwa_engine* e, bwa_bed b, float linear);   /* master gain, ramped */
/* Full 3-axis orientation of the bed's soundfield (radians): LEVEL or reorient a recorded soundfield
 * whose capture wasn't upright, line a capture up with the scene, or spin it slowly for effect.
 * Positive yaw turns the field about the room's vertical axis from room +z/front toward room +x;
 * positive pitch tilts the field's front (+z) upward; positive roll tilts its top toward the room's
 * right (-x). Applied roll -> pitch -> yaw. Yaw-only (pitch=roll=0) stays on the exact closed-form
 * phasor path (each degree's +-m pair rotates by m*yaw — exact, all orders); any pitch/roll runs a
 * full SH rotation matrix (Ivanic-Ruedenberg), rebuilt per block from angles that glide at ~one
 * turn/s — click-free and live, applied before EITHER bed renderer (matrix and parametric see the
 * same turned field). Per-frame-safe. */
BWA_API void  bwa_bed_set_orientation(bwa_engine* e, bwa_bed b, float yaw_rad, float pitch_rad, float roll_rad);
BWA_API void  bwa_bed_stop(bwa_engine* e, bwa_bed b);
BWA_API void  bwa_bed_destroy(bwa_engine* e, bwa_bed b);
/* The rest of a bed's control surface matches the bwa_source_* call of the same name (a bed IS a
 * voice — same pool, same semantics; these are DELIBERATE aliases so bed code never mixes prefixes): timed fades
 * (fade_out = the click-free stop), pause/seek (resume/jump lands exactly), steal priority (a bed
 * competes for voices like any source — protect a music bed with 255), mix groups, and the
 * is-playing/playhead readbacks. Position/spatial calls (pos, spread, occlusion, ...) do not apply —
 * a bed is world-locked; and pitch is a no-op on beds (see bwa_source_set_pitch). */
BWA_API void  bwa_bed_fade_to (bwa_engine* e, bwa_bed b, float gain, float seconds);
BWA_API void  bwa_bed_fade_out(bwa_engine* e, bwa_bed b, float seconds);
BWA_API void  bwa_bed_set_paused(bwa_engine* e, bwa_bed b, bool paused);
BWA_API void  bwa_bed_seek(bwa_engine* e, bwa_bed b, uint64_t frame);
BWA_API void  bwa_bed_set_priority(bwa_engine* e, bwa_bed b, int priority);
BWA_API void  bwa_bed_set_group(bwa_engine* e, bwa_bed b, uint32_t group);
BWA_API bool  bwa_bed_is_playing(bwa_engine* e, bwa_bed b);
BWA_API uint64_t bwa_bed_get_playhead_frames(bwa_engine* e, bwa_bed b);

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
/* Set the STATIC occluding geometry (room space, RH meters; tris are CCW vertex-index triples) with
 * PER-TRIANGLE materials: tri_material is ntris bwa_material tokens (one per triangle; out-of-range
 * clamps to the default). Replaces any prior static mesh. Safe to call at runtime, but it rebuilds the
 * whole scene BVH — for things that MOVE, use the dynamic-mesh API below (a cheap instance transform).
 * No-op without the Steam Audio backend. */
BWA_API void     bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                                      const bwa_material* tri_material);
/* Convenience: a FLOOR-based shoebox of w x h x d meters — centered on the origin in x/z, y from 0
 * (floor) to h — one material per face. faces[6] = (-x,+x,-y,+y,-z,+z), each a bwa_material token
 * (0 = default); triangle normals face inward. Works WITH and WITHOUT the Steam build: it feeds the
 * ray-traced scene (SDK) and ALWAYS captures the box for the image-source early reflections.
 * LIVE-safe: the ISM capture publishes to the audio thread lock-free and each opted-in source
 * re-solves its images next block (gains ramp, delays glide — a room change bends, not clicks);
 * with the SDK a live call also pays set_mesh_mat's BVH rebuild.
 * It IS a bwa_scene_set_mesh_mat call, so it REPLACES the static mesh — the box and your own
 * geometry are alternatives, not layers; to keep both, call set_box first then ONE set_mesh_mat
 * carrying the box's 12 inward triangles plus yours. Why (the half-way drop): docs/api.md,
 * "Materials and scene geometry". */
BWA_API void     bwa_scene_set_box(bwa_engine* e, float w, float h, float d, const bwa_material faces[6]);
/* The OUTDOOR degenerate of the box: one horizontal mirror plane at height y (room meters) — the
 * ground bounce, the dominant early reflection when there is no room around you. Same dual capture
 * as the box: the image-source reflections get the plane (every build), the ray-traced scene (SDK)
 * gets a large ground quad. REPLACES any prior box (one room at a time; last call wins), and like
 * the box it is live-safe (a mid-scene call re-solves the reflections next block — the submerge
 * transition). pressure_release flips the reflection's polarity — a boundary into
 * a much SOFTER medium reflects inverted (the underside of a water surface: y = the surface height,
 * a submerged listener, and the ground bounce becomes the Lloyd's-mirror comb that makes near-
 * surface sources sound thin). For ordinary ground, pass false. */
BWA_API void     bwa_scene_set_ground(bwa_engine* e, float y, bwa_material mat, bool pressure_release);
/* Flag box faces as pressure-release boundaries (bit f = face f in the set_box order
 * -x,+x,-y,+y,-z,+z): that face's image-source reflection NEGATES — the physics of reflecting off
 * a much softer medium (air, seen from water: reflection coefficient ~ -1). The flagship case is a
 * virtual underwater room whose ceiling is the surface: mask 1u<<3 (+y). Only the image-source
 * renderer is affected (polarity is a specular concept); the ray-traced scene keeps the face's
 * material for occlusion/reverb. Call AFTER set_box/set_ground (it flags the captured room);
 * live-safe, like the room itself. A later set_box/set_ground resets every face to normal. */
BWA_API void     bwa_scene_set_pressure_release(bwa_engine* e, uint32_t face_mask);

/* Dynamic (movable) occluders/reflectors — the acoustic analogue of a physics collider with a
 * transform. The static mesh above is committed once (BVH built once); a dynamic mesh is a rigid
 * INSTANCE of its own sub-scene, so moving it is a cheap top-level BVH refit, not a geometry rebuild.
 * Occlusion and REAL-TIME reflections/pathing pick up the motion on their next sim tick (~10-30 Hz);
 * BAKED reflections/pathing do NOT (the bake froze the geometry — see docs/materials.md).
 *
 * add: geometry is in the mover's LOCAL space (meters, CCW), placed by set_dynamic_transform; one
 * `material` token covers every triangle. Returns a handle >= 0, or -1 (no SDK / bad geometry / the
 * movable table is full — bwa_last_error distinguishes). Runtime- and load-time-safe. NOTE: this
 * handle is deliberately a plain small int index (movers are few and app-managed), not a
 * generation-checked bwa_* handle — the one deviation from the 0-is-invalid handle scheme. */
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

/* ---- reverb & reflections: the map ----
 * Shared REVERB TAP (bwa_source_set_reverb + _send / _distance): rendered by EITHER the Steam
 * reflection bed (bwa_reflections_config) OR the FDN (bwa_fdn_config), one at a time, with
 * bwa_set_reverb_gain the single live wet level. Image-source EARLY reflections
 * (bwa_source_set_early_reflections) are a SEPARATE per-source system: pair them with the FDN,
 * never with the Steam bed (double early reflections; the engine warns once).
 * The decision guide: docs/materials.md, "Choosing an acoustics path". */

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
BWA_API void     bwa_set_reverb_gain(bwa_engine* e, float linear);

/* ---- directional FDN reverb bed (load-time; no SDK needed) ----
 * A phonon-free late-reverb alternative to the Steam reflection bed: a 16-line feedback delay
 * network whose lines are rendered as plane waves through the layout's SH->speaker bed decode, fed by
 * the SAME mono aux send (bwa_source_set_reverb + the per-source send levels apply unchanged).
 * The decay is a DESIGN parameter: set what the content wants. Don't copy the room's measured RT60 —
 * the real room adds its own decay on top (docs/calibration.md). Decay can be ANISOTROPIC: scale the decay time toward a
 * direction (factor < 1 = the field dies faster that way — an open or treated side), the diagonal
 * special case of the Directional FDN (Alary/Politis/Schlecht, JAES 2019). Enabling it takes the
 * reverb tap INSTEAD of the Steam bed (one reverb bed at a time); works in no-SDK builds — the
 * playground's reverb scene runs without phonon. Load-time (between bwa_create and bwa_start);
 * zero fields take the defaults; the wet level is bwa_set_reverb_gain (live, default 1) and the
 * DECAY is live-retunable after start via bwa_fdn_set_decay below. */
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
/* LIVE decay retune — the acoustic character knob that works while the engine runs: the FDN ramps
 * its per-line loss gains to the new decay across one block (~5 ms), so the tail keeps ringing and
 * only its slope changes (no click, no restart). This is what a room TRANSITION sounds like:
 * stepping into a cathedral, submerging (long low band, short high band), leaving for open air
 * (both short) — pair it with bwa_set_reverb_gain for the wet level. <= 0 keeps a parameter's
 * current value. Pre-start it updates the staged config (call it unconditionally); after start it
 * needs the FDN enabled (bwa_last_error otherwise). Decay clamps to [0.05, 30] s, the crossover to
 * [100, 0.4*rate] Hz. Control thread. */
BWA_API void     bwa_fdn_set_decay(bwa_engine* e, float rt60_low_s, float rt60_high_s, float xover_hz);

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
BWA_API void     bwa_set_early_reflections_gain(bwa_engine* e, float linear);   /* default 1 */
/* Opt a source into the shared bed's wet send (per-frame-safe, enqueue-only). With the bed disabled
 * or no SDK, this just gates a send that goes nowhere. */
BWA_API void     bwa_source_set_reverb(bwa_engine* e, bwa_source s, bool on);
/* Per-source wet-send LEVEL (default 1.0; 0 = none). Scales how much of this source feeds the shared
 * reverb bed — drive it yourself for a manual dry/wet, or use the distance mode below. Per-frame-safe. */
BWA_API void     bwa_source_set_reverb_send(bwa_engine* e, bwa_source s, float gain);
/* Distance->wet: when on, the engine scales this source's send by its distance to the listener (near =
 * drier, far = wetter), on top of the level above. Off by default (constant send). Per-frame-safe. */
BWA_API void     bwa_source_set_reverb_distance(bwa_engine* e, bwa_source s, bool on);
/* Opt a source into sound PATHING: when the direct line is blocked, its sound is routed to the
 * listener along indirect paths (around occluders / through openings) and decoded to the array from
 * those arrival directions. Requires the scene geometry + bwa_desc.enable_pathing; no-op otherwise.
 * Per-frame-safe (enqueue-only). The indirect field updates at the sim rate (~10 Hz), ramped. */
BWA_API void     bwa_source_set_pathing(bwa_engine* e, bwa_source s, bool on);
/* Read the source's current occlusion factor (1 = clear .. 0 = fully blocked) — for HUD/diagnostics. */
BWA_API float    bwa_source_get_occlusion(bwa_engine* e, bwa_source s);

/* ---- directivity (control thread; works in EVERY build) ----
 * Source radiation pattern. Sources are omni by default; set a weighted-dipole pattern + the
 * source's forward orientation, and a listener off the source's forward axis hears it attenuated.
 * Independent of occlusion (a source can be directional without being occluded). Two renderers,
 * same |(1-w) + w cos(theta)|^p model: WITH the Steam scene the occlusion sim evaluates it
 * (~10-30 Hz, published + ramped); without a scene (or without the SDK at all) the audio thread
 * evaluates it per block from the same forward axis — walk-correct in both. */
typedef enum { BWA_DIR_OMNI = 0, BWA_DIR_CARDIOID = 1, BWA_DIR_FIGURE8 = 2 } bwa_directivity;
/* Source orientation as a quaternion (same room frame + handedness as bwa_set_listener_pose); the
 * dipole axis is the source's forward (+z rotated by q). Per-frame-safe. */
BWA_API void     bwa_source_set_orientation(bwa_engine* e, bwa_source s, float qx, float qy, float qz, float qw);
/* Radiation pattern: weight 0=omni (off) .. 0.5=cardioid .. 1=figure-8; power>=1 sharpens the lobe.
 * Per-frame-safe. */
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
 * few meters, pronounced for sources placed at large virtual distances. */
BWA_API void     bwa_source_set_air_absorption(bwa_engine* e, bwa_source s, bool on);
/* Equal-loudness distance compensation: as distance attenuation takes level away, the ear also loses
 * LF sensitivity (ISO 226 contours), so an attenuated source reads THIN as well as far. This restores
 * part of the body with an LF shelf that tracks the attenuation (+0.4 dB per dB taken, capped +8 dB) —
 * "far, not tinny". A perceptual stylization, not physics; leave it off for strict realism. Direct
 * path only (like air/Doppler); ramped; per-frame-safe. */
BWA_API void     bwa_source_set_loudness_comp(bwa_engine* e, bwa_source s, bool on);
/* Near-field proximity boost: an LF shelf that RISES as the source closes inside ~1 m — the
 * spherical-wavefront proximity effect, and the near mirror of loudness comp above (that one
 * restores body far away). In a walkable volume this is the missing half of distance: "at arm's
 * length" should read as bass, not just level (0 dB at 1 m, up to +6 dB at the head; ~300 Hz
 * corner). Direct path only; ramped; per-frame-safe. */
BWA_API void     bwa_source_set_proximity(bwa_engine* e, bwa_source s, bool on);
/* Engine-wide speed of sound (m/s; default 343 — air). Everything that renders a propagation DELAY
 * derives from it: Doppler (delay AND pitch-shift magnitude) and the image-source reflection
 * delays. Live + per-frame-safe: a change glides every delay to its new target (bends, never
 * steps). Underwater is 1480; small values exaggerate Doppler for slow-motion effects, saturating
 * against each delay ring's capacity (~40 ms at 48 k) — clamped to [30, 20000]. The Steam sim's
 * ray clock is phonon's own and does not follow this. */
BWA_API void     bwa_set_speed_of_sound(bwa_engine* e, float meters_per_s);
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
 * collapse to one point), centered on its direction and constant-power. Works with any panner. */
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
 * gain 0 or BWA_TEST_OFF silences a channel. Composes with the profiles: cave/cave_both -> a raw
 * tone on that Digiface channel/speaker; the headphone profiles -> that bus channel HRTF'd as its
 * virtual speaker (in BINAURAL the test tone rides the diffuse/virtual-speaker path — only point
 * SOURCES render direct). */
typedef enum { BWA_TEST_OFF = 0, BWA_TEST_SINE = 1, BWA_TEST_NOISE = 2 } bwa_test_kind;
BWA_API void     bwa_set_test_signal(bwa_engine* e, uint32_t channel, bwa_test_kind kind, float gain);

/* Read back the effective speaker layout (the default grid, or the file from bwa_desc.layout_path):
 * fills `xyz` with up to `cap` speakers' positions (3 floats each: x,y,z room space, in channel/index
 * order) and returns the count FILLED — min(cap, count), the same convention as bwa_get_bus_levels.
 * Pass xyz = NULL to query the total count (== bwa_get_channel_count()). For visualizing or
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

/* ---- device health: was the callback starved, and by whom ----
 * Every other readback here describes the RENDER — voices, meters, the clock. None of them can tell
 * you the device asked for a block and did not get one, which is the failure that makes a clean
 * render sound broken. These counters do, and they are monotonic since bwa_start.
 *
 * The two halves answer different questions and have different fixes. `xruns` is the DEVICE running
 * on without us: its sample position jumped past where the last callback said the next one would
 * land, so it clocked out audio nobody rendered (bigger buffer, fewer other loads on the machine,
 * check the driver). `late_blocks` is US overrunning the block period, which is what eventually
 * produces those (cheaper scene, fewer voices, look at peak_load). `stream_starves` is neither: the
 * device kept its deadline and a streamed voice had nothing to give it, because the disk thread
 * didn't refill in time — the block's tail rendered silence.
 *
 * Reading it: the counts mean nothing without `blocks` under them. One xrun in two million blocks is
 * a machine hiccup; one in a thousand is a configuration to fix. */
typedef struct bwa_health {
    uint64_t blocks;          /* blocks rendered since start — the denominator for everything else  */
    uint64_t xruns;           /* device dropouts: it ran on without us                              */
    uint64_t dropped_frames;  /* frames those dropouts swallowed (xruns tells you how many events)  */
    uint64_t driver_resyncs;  /* the driver reporting a discontinuity itself (ASIO kAsioResyncRequest) */
    uint64_t late_blocks;     /* our render overran the block period — we are the cause             */
    uint64_t stream_starves;  /* a streamed voice's ring ran dry without the asset having ended     */
    float    peak_load;       /* worst single-block render time / block period. 1.0 = exactly at
                               * budget, so anything approaching it is living dangerously           */
} bwa_health;
/* Fills `out` and returns whether the numbers MEAN anything. False = this configuration cannot
 * observe a dropout at all: no engine, not started, the manual sink (no clock, no deadline — an
 * offline render cannot miss one by construction), or an ASIO driver that never flags a valid
 * sample position, leaving nothing to compare against. `out` is zeroed either way.
 *
 * That boolean is the point. A zero xrun count means "none happened" ONLY when this returned true;
 * on its own it is indistinguishable from "never measured", and silently reading it as a clean bill
 * of health is exactly how a starved device goes unnoticed. Control thread, per-frame-safe. */
BWA_API bool     bwa_get_health(bwa_engine* e, bwa_health* out);
/* The one-line form: device dropouts since bwa_start, for a HUD or a log line. 0 when nothing was
 * dropped AND when nothing could be measured — call bwa_get_health once at startup if you need to
 * tell those apart (and you do, on an unfamiliar driver). */
BWA_API uint64_t bwa_get_xruns(bwa_engine* e);

/* ---- output capture (recording / offline sanity + golden checks) ----
 * Tap the FINAL device-bound output — post-limiter, exactly what reaches the device. The callback runs
 * on the AUDIO thread, once per block, with PLANAR channel-major data: `planar[c*nframes + i]` is
 * channel c, sample i. `channels` is the PRIMARY device's channel count — the array count
 * (bwa_get_channel_count) for cave/cave_both, 2 for binaural/cave_sim. Obey the audio-thread
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
 * output — binaural/cave_sim: 2 ch; cave/cave_both: bwa_get_channel_count() ch — into engine-owned memory and returns
 * a pointer to it (PLANAR, `channels * nframes` floats, `p[c*nframes + i]`), valid until the next call
 * or bwa_stop. Fills *channels / *nframes (either may be NULL). Returns NULL if the engine isn't started
 * or the sink isn't MANUAL. The timestamp is a pure sample counter (no wall clock), so a fixed input +
 * fixed call sequence renders bit-identically every run — the basis for golden-audio tests. Same caveat
 * as bwa_set_output_capture: the async Steam sims aren't reproducible; keep golden renders on the
 * synchronous DSP (or the manual occlusion path). Control thread. */
BWA_API const float* bwa_render_block(bwa_engine* e, uint32_t* channels, uint32_t* nframes);

/* ---- panner selection & layout query (load-time, or live: the switch is atomic) ----
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
/* SPCAP's two tuning knobs (dimensionless exponents; inert under DBAP/VBAP). `focus` is the lobe
 * sharpness in ((1+cos)/2)^focus: higher concentrates a source on fewer speakers (tighter image),
 * lower spreads it (smoother, blurrier). `density` is the exponent of the placement-correction
 * kernel that de-biases a clustered array; 2.0 is the default and it is rarely worth moving.
 * Pass <= 0 for EITHER argument to revert that one to the default: focus falls back to a value
 * DERIVED from the array geometry (the exponent that puts the lobe 6 dB down at the mean
 * nearest-neighbor speaker angle — about 12.7 on the 26-speaker cube grid, wider on a sparse array),
 * density to 2.0. Live and per-frame-safe: every source re-solves on the next block, static ones
 * included, and the gains ramp. A runtime knob like the other live A/B toggles, so it lives nowhere
 * on disk — persisting a dialed value is your application's business, not the layout file's. */
BWA_API void     bwa_set_spcap_focus(bwa_engine* e, float focus, float density);
/* The focus value bwa_set_spcap_focus reverts to, for an array given as `n` speaker positions
 * (3 floats each, the bwa_panner_gains_batch convention). Pure, not engine state: a tool can show
 * what an in-progress layout implies before you override it. Returns 0 on bad arguments. */
BWA_API float    bwa_spcap_focus_default(const float* positions, uint32_t n);
/* The engine's ACTIVE channel count = the layout's speaker count (4..26; the 26-grid default with no
 * layout_path). Fixed for the engine's lifetime — size meter/speaker arrays with it. BWA_CHANNELS(26)
 * is only the compile-time CAPACITY; a collaborator's 24-speaker layout loads into the same binary.
 * A failed EXPLICIT layout load leaves the engine on the 26-grid (channel count included) but then
 * FAILS bwa_start with BWA_ERR_LAYOUT — a wrong-channel-count session can't start silently. */
BWA_API uint32_t bwa_get_channel_count(bwa_engine* e);
/* Dual-band panning (off by default): split each source at ~700 Hz and pan the low band with
 * amplitude (pressure / velocity-vector) normalization, the high band with the panner's usual power
 * (energy-vector) normalization — better low-frequency localization for a near-centered listener. Wraps
 * the selected panner; live-toggleable for A/B. Sweet-spot dependent like VBAP (see docs). */
BWA_API void     bwa_set_dual_band(bwa_engine* e, bool on);
/* Compensated amplitude panning on that low band (off by default; live A/B). REQUIRES dual-band: the
 * low band is the only thing it touches, so with bwa_set_dual_band off this call changes nothing.
 *
 * Dual-band's low band aims the velocity vector at the source and accepts whatever |rV| < 1 the
 * speaker geometry gives, which leaves the rendered ITD short of a real source's by a direction-
 * dependent amount — so the image shifts when you turn your head. CAP instead constrains the one
 * quantity the ear reads below ~700 Hz, the INTERAURAL component of the summed field, to equal a
 * real source's at that bearing, using the tracked head ORIENTATION. Matching one scalar is
 * satisfiable where matching a 3-vector is not, so the ITD comes out exact and stays exact as the
 * head turns. Facing the source it is a no-op and reduces to the selected panner; it fades out with
 * bwa_source_set_spread (an engulfing source has no single bearing to fix).
 *
 * This is the one engine feature that reads head orientation into the SPEAKER path — everywhere
 * else orientation enters only at the binaural decode. So it wants a real pose: with an untracked
 * (identity) head it still corrects ITD for a listener facing room-ahead, but the head-rotation
 * benefit is exactly what you are not getting. Aimed at the seated/fixed-position case, where it
 * costs one 3-vector rotate plus one dot product per speaker per block and rebuilds no panner cache.
 * Composes with any panner and with the multi-listener compromise (primary head). Spread fades it
 * out under LOBE and MDAP, but BWA_SPREAD_SPECTRAL bypasses it entirely: that mode replaces the
 * single-path output stage, so a spread source there gets plain dual-band on its low bands and no
 * ITD correction at all. Known exclusion. See docs/spatialization.md. */
BWA_API void     bwa_set_dual_band_cap(bwa_engine* e, bool on);
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
/* How bwa_source_set_spread renders a source's width (engine-wide, live A/B; spread-0 sources
 * unaffected). LOBE (default): reshape the point gains toward a width lobe — one solve, cheap,
 * approximate. MDAP (Pulkki): pan a ring of virtual sources around the direction with the selected
 * panner and sum — panner-true extent (VBAP sparse, SPCAP placement-corrected), ~13x solve cost.
 * SPECTRAL (Zotter/Frank): split into 6 bands, pan each to its own direction in the cone (LF stays
 * put) — width with no decorrelation noise, nothing to comb as the listener walks. All constant-
 * power. Full story: docs/api.md, "Source spread / size" (design: docs/spatialization.md). */
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
/* Hole-aware spread floor (0 = off, the default): floor a source's spread by how far its bearing
 * sits from the NEAREST speaker, so a source aimed where the array has no speaker renders as an
 * honest WIDE source instead of a fake point.
 *
 * Only arrays with holes are affected. The CAVE array is a barrel: speakers mount between the screen
 * cube and the truss, so nothing covers the poles. Aim a source into one and the panner closes the
 * hole with a big triangle of distant speakers — the direction comes out about right, but the energy
 * is carried by speakers up to 113 degrees apart, which is a split image, not a phantom. A source
 * with no speaker anywhere near it is genuinely not a point, so this stops pretending it is.
 *
 * The floor is 0 until the nearest speaker is more than one mean inter-speaker spacing away (the
 * same array geometry bwa_spcap_focus_default measures, so it self-scales), then rises linearly,
 * reaching fully wide when no speaker is within 90 degrees. A surrounding array is therefore inert
 * by construction, not by tuning: no direction on it is ever a full speaker spacing from a speaker.
 *
 * `strength` scales the derived floor: 1.0 is the honest width, below that a partial widening, above
 * it an exaggeration (clamped to 0..2, so a negative value reads as off). The widened part follows
 * the selected spread mode and (when
 * enabled) decorrelates, exactly like bwa_source_set_spread, and it composes with the other spread
 * floors as a max — the widest claim wins. Engine-wide policy; live-safe, and every source re-solves
 * on the next block, static ones included. No effect in BWA_PROFILE_BINAURAL (no speakers, no
 * holes). Full story: docs/spatialization.md, "Array holes". */
BWA_API void     bwa_set_hole_spread(bwa_engine* e, float strength);

/* ---- output protection limiter (ON by default at -1 dBFS) ----
 * The final stage on the speaker output — everything (voices, beds, reflections, pathing, per-speaker
 * trims, the test signal) passes through it before the device. LINKED across channels: one gain from
 * the cross-channel peak, so engaging never shifts the spatial image; ~1 ms attack / ~120 ms release,
 * then a hard clamp at the ceiling. This is driver/speaker protection against digital overs, not a
 * mastering limiter — if it engages in normal use, turn the content down. Control thread; live. */
BWA_API void     bwa_set_limiter(bwa_engine* e, bool on);
BWA_API void     bwa_set_limiter_ceiling(bwa_engine* e, float linear);   /* peak amplitude ceiling, linear in (0..1]; default 0.891251f (-1 dBFS). A value >1 clamps to 1; <=0 is ignored. */

/* ---- headphone correction EQ (the headphone-side align stage) ----
 * Corrects the TRANSDUCER, not the render: an AutoEq-style parametric EQ file
 * (ParametricEQ.txt — "Preamp: -6.4 dB" + "Filter N: ON PK Fc 105 Hz Gain -4.6 dB Q 0.70";
 * PK/LSC/HSC map onto the engine's RBJ biquads) is parsed into a cascade applied to the final
 * device-bound STEREO of every headphone profile (binaural, cave_sim, cave_both's monitor tap)
 * — after the HRTF decode, before the output clamp. The array render never sees it (speakers
 * get the per-speaker align stage instead); in the cave profile it is inert. AutoEq
 * (github.com/jaakkopasanen/AutoEq) publishes corrections for thousands of headphone models;
 * the Preamp line is honored — corrections boost dips, and the preamp is the headroom that
 * keeps them out of the clamp.
 * bwa_load_headphone_eq parses + swaps in click-free (a running correction ramps out, the new
 * one ramps in); NULL or "" clears. Load-class: file I/O, may block — not per-frame. A parse
 * failure returns BWA_ERR_CONFIG with the reason in bwa_last_error and KEEPS the previous EQ.
 * bwa_set_headphone_eq is the ramped live A/B (default ON: loading engages). Per-frame-safe. */
BWA_API bwa_result bwa_load_headphone_eq(bwa_engine* e, const char* path);
BWA_API void       bwa_set_headphone_eq(bwa_engine* e, bool on);

/* Select how ambisonic BEDS render (live: each bed crossfades to the selection — a click-free A/B).
 * MATRIX (default) is the static SH->speaker decode (AllRAD or EPAD per bwa_desc.bed_decoder) — cheap,
 * world-locked, sweet-spot-ish. PARAMETRIC analyzes the bed's first-order channels per frequency
 * band into a direction + diffuseness (DirAC-style intensity analysis): the non-diffuse stream is
 * RE-PANNED through the engine's listener-relative panner at the array shell — a recorded soundfield
 * becomes WALKABLE (off-center listeners get correct directions + parallax, which no matrix decode
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

/* Tracked listener alignment (OFF by default). The layout's per-speaker delay and gain trims align
 * the array's arrival times at ONE point, the array centroid, so the array is time-coherent there and
 * progressively less so as the listener walks away. Turn this on and the output stage re-references
 * that alignment onto the TRACKED listener instead: per speaker it adds the propagation delay and the
 * 1/r level for |speaker - listener| against |speaker - centroid|, so coherence follows the head.
 * Purely geometric, downstream of every panner, and works with any of them.
 *
 * It is opt-in because a moving delay line is a resampling event. A walking listener means every
 * speaker's delay gliding at once, which is a Doppler shift on everything the array plays — a global,
 * always-audible failure mode rather than a local one. Two guards keep it usable, and both are yours
 * to tune (pass <= 0 for either to take the default):
 *
 *   dead_zone_m         How far the head must move before anything is recomputed. Default 0.05 (5 cm).
 *                       Tracker position output is jittery; without a dead zone the array glides
 *                       permanently. 5 cm of slack is 0.15 ms of residual arrival error.
 *   slew_frames_per_s   Ceiling on how fast a speaker's delay may change, in output FRAMES per second.
 *                       This ratio against the sample rate IS the resampling ratio, so it is what
 *                       bounds the pitch shift. The default follows a listener closing on a speaker at
 *                       0.45 m/s, which is about 63 frames/s at 48 kHz and 0.13% (2.3 cents) of shift.
 *                       Move faster than that and the alignment LAGS instead of warbling, which is the
 *                       trade the default takes deliberately. Raise it for tighter tracking of a fast
 *                       listener, and expect to hear it.
 *
 * Corrections saturate about 4 m from the centroid (the reserved delay headroom) and the per-speaker
 * level trim is clamped to +/-6 dB. Off is exact: the trims revert to the layout's own values, and
 * toggling either way glides rather than steps. Control thread, per-frame-safe, live A/B.
 *
 * The enable and its guards are SEPARATE calls, the same split as bwa_set_limiter /
 * bwa_set_limiter_ceiling: the enable is what you A/B, and toggling it must not silently reset a
 * dialed guard. Order does not matter, and the guards may be changed while it is running (the
 * renderer re-reads them every block).
 * See docs/spatialization.md, "Re-aligning to the tracked listener". */
BWA_API void     bwa_set_tracked_align(bwa_engine* e, bool on);
BWA_API void     bwa_set_tracked_align_guards(bwa_engine* e, float dead_zone_m,
                                              float slew_frames_per_s);

/* ---- offline panner evaluation (no engine handle; for layout scoring/optimization in tools) ----
 * The per-speaker gains the given `panner` produces for `nsrc` source positions heard from one
 * listener `lis`, over a layout of `n` speaker positions (`positions` = n*3 floats, room space). Writes
 * out[i*n + s] (nsrc*n floats); returns nsrc. Default DBAP/distance tuning. Shares the SPCAP/VBAP
 * per-listener cache across the batch (efficient over a grid). Pure AND reentrant — the cache is
 * per-call stack state, no globals, so any thread may call it, concurrently with a running engine.
 * Uses the same panner solves the audio path does, so a tool scores a layout against the ACTUAL
 * panner, not a copy.
 *
 * `focus` / `density` are SPCAP's tuning knobs, the SAME two bwa_set_spcap_focus sets live and with
 * the SAME <= 0 sentinel: either one <= 0 reverts that one to the default for THIS array (focus to
 * the geometry-derived value bwa_spcap_focus_default returns for `positions`, density to 2.0). Pass
 * 0, 0 for the array's own defaults. Both are INERT under BWA_PAN_DBAP and BWA_PAN_VBAP — those
 * panners have no lobe to sharpen, so any value scores the same. Score the panner you will ship at
 * the tuning you will ship it at: a layout graded at the derived focus says nothing about how it
 * behaves once you dial the knob. */
BWA_API uint32_t bwa_panner_gains_batch(bwa_panner panner, const float* positions, uint32_t n,
                                      const float lis[3], const float* srcs, uint32_t nsrc,
                                      float focus, float density, float* out);

/* The BED counterpart: the per-speaker gains the diffuse-bed decode produces for `ndir` plane-wave
 * DIRECTIONS (`dirs` = ndir*3 unit vectors, room space; a bed is content at infinity, so directions,
 * not positions), over a layout of `n` speaker positions. Builds the same SH->speaker decode the
 * engine builds for this layout — AllRAD or EPAD per `decoder`, the sampling decode as the automatic
 * degenerate fallback — encodes each direction (3rd order ACN/SN3D), applies max-rE weighting when
 * `max_re`, and multiplies through. Writes out[i*n + s] (ndir*n floats; gains may be negative — SH
 * sidelobes); returns ndir. Pure and reentrant, same as the panner batch. A layout that scores well
 * here is a good QUADRATURE for the sphere, which is what ambisonic content wants from an array and
 * what the point-source panners cannot see. */
BWA_API uint32_t bwa_bed_gains_batch(bwa_bed_decoder decoder, bool max_re,
                                     const float* positions, uint32_t n,
                                     const float* dirs, uint32_t ndir, float* out);

/* ---- listener (control thread; skip when a tracker is connected) ---- */
/* Position in room space. Quaternion is head orientation; used by the headphone
 * renders only — the array render ignores orientation (real speakers, real ears).
 * ROOM FRAME: right-handed, +y up, meters, identity orientation faces +z (so the
 * right ear is at -x). The origin sits ON THE FLOOR at the working-area center
 * (x/z) — Motive's ground-plane calibration — so OptiTrack rigid-body poses pass
 * through unchanged and y is height above the floor. The engine's world-locked
 * decodes reference the ARRAY CENTROID (the nominal listening point, also the
 * default listener position), not the origin, so nothing breaks if a survey
 * places the origin elsewhere. */
/* The identity-listener basis, as data — derive "forward"/"right" from these
 * (the engine's own orientation seams do) rather than re-hardcoding the
 * convention. An identity quaternion's ahead is BWA_ROOM_AHEAD, etc.
 * (BWA__UNUSED keeps -Wunused-const-variable quiet in TUs that include but
 * don't reference them; each TU gets its own copy — they are data, not ABI.) */
#if defined(__GNUC__) || defined(__clang__)
  #define BWA__UNUSED __attribute__((unused))
#else
  #define BWA__UNUSED
#endif
static const float BWA_ROOM_AHEAD[3] BWA__UNUSED = {  0.0f, 0.0f, 1.0f };
static const float BWA_ROOM_UP[3]    BWA__UNUSED = {  0.0f, 1.0f, 0.0f };
static const float BWA_ROOM_RIGHT[3] BWA__UNUSED = { -1.0f, 0.0f, 0.0f };
/* Commit-gated: takes effect at the next bwa_commit. */
BWA_API void bwa_set_listener_pose(bwa_engine* e, float px, float py, float pz,
                                              float qx, float qy, float qz, float qw);

/* Read back the listener pose the engine is currently rendering with — the committed pose, or,
 * with a tracker connected, the freshest tracked pose. For visuals/logging/debugging tracking;
 * safe to poll from the control thread. Fills p[3] (position) and q[4] (orientation xyzw).
 * Reads identity until the first rendered block / tracked frame. */
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

/* Liveness of a connected tracker's stream — for status displays and dropout alarms. A successful
 * bwa_tracker_connect only means the socket opened; on multicast there is no handshake, so it says
 * nothing about whether Motive is actually streaming or whether your rigid body is in view. This
 * reports what the wire is doing right now, split so the two operator-actionable failures are
 * distinct: NO_DATA -> check the stream/network/port; NO_BODY -> check the rigid body id/name, or
 * the person is occluded. "Recent" means within ~250 ms; a live stream at 100-360 Hz never trips
 * that. Derived from local arrival timing, not the pose stamps (those ride the server clock). */
typedef enum {
    BWA_TRACKER_DISCONNECTED = 0,  /* no tracker connected on this engine (or NULL engine) */
    BWA_TRACKER_NO_DATA      = 1,  /* connected, but no frames arriving — Motive not streaming, a
                                    * network/interface problem, or the wrong data port/multicast group */
    BWA_TRACKER_NO_BODY      = 2,  /* frames ARE arriving, but the followed rigid body has no recent
                                    * valid pose — wrong rigid_body_id/name, or the body is occluded now */
    BWA_TRACKER_LIVE         = 3,  /* the followed rigid body's pose is arriving and current */
} bwa_tracker_state;
/* Control thread; never blocks; cheap enough to poll every frame. */
BWA_API bwa_tracker_state bwa_tracker_status(bwa_engine* e);

/* Pose prediction (internal tracking only — needs a connected tracker; 0 = off, the default):
 * extrapolate the tracked POSITION by
 * `lead_s` SECONDS along a velocity estimated from the tracker's own frame timestamps (smoothed
 * ~100 ms, speed-capped, reset across drop-outs). The tracking chain — Motive solve, network, the
 * audio block, the DAC — puts the rendered pose 20-40 ms behind the head; at walking speed that is a
 * few cm of panning lag this hides. Set lead_s to your measured motion-to-ears latency (in seconds);
 * too much lead OVERSHOOTS on direction changes (clamped at 0.2 s). Live-safe. */
BWA_API void bwa_set_pose_prediction(bwa_engine* e, float lead_s);

/* Extra listeners (multi-occupant compromise panning; 0 = off, the default). A CAVE usually holds
 * more than one person, and single-listener panning is exact for the tracked head and wrong for
 * everyone else. Give the OTHER occupants' positions here (up to BWA_EXTRA_LIS, `xyz` = count*3
 * floats, copied at call; the primary listener stays bwa_set_listener_pose / tracking): every source's gains become
 * the per-speaker ENERGY MEAN of the per-listener solves — each occupant hears the image biased
 * toward their own solve instead of one exact + N wrong. Constant-power; works with every panner
 * (each extra gets its own SPCAP/VBAP cache). Spread/Doppler/air and the headphone renders stay
 * primary-relative; BWA_PROFILE_BINAURAL ignores extras entirely (headphones: one head, one
 * decode). Per-frame-safe, commit-gated like the pose; count 0 restores single-listener panning. */
#define BWA_EXTRA_LIS 3
BWA_API void bwa_set_extra_listeners(bwa_engine* e, const float* xyz, uint32_t count);

/* ---- frame boundary ----
 * Promotes this frame's position/pose updates as one coherent snapshot and drains
 * the event ring (voice-ended, sound-retired). Call once per frame after pushing
 * all source + listener updates. Only position/pose-class state is commit-gated
 * (bwa_source_set_pos, bwa_set_listener_pose, bwa_set_extra_listeners — each says
 * so at its declaration); every other call lands on the next audio block without
 * a commit. Forgetting commit therefore renders everything at its LAST committed
 * (or default) position — sounds play, nothing moves. */
BWA_API void bwa_commit(bwa_engine* e);

#ifdef __cplusplus
}
#endif
#endif /* BW_AUDIO_H */
