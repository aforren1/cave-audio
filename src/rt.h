/*
 * rt.h — the real-time core: the two SPSC rings, the voice table, the commit
 * snapshot, and generation-counted handles (docs/concurrency.md, docs/internal-types.md).
 * NOT part of the public ABI. engine.c owns one RtCore and forwards the bwa_* API to it.
 *
 * Thread split (docs/CLAUDE.md): the CONTROL thread calls the rt_source_* / rt_set_listener
 * / rt_commit functions (enqueue only, plus handle allocation); the AUDIO thread calls
 * rt_render (drain + mix), which must not allocate/lock/block. They share state only
 * through the two rings and the staging->active snapshot.
 *
 * The mixing is the full production path: mix_voice plays sound->pcm through the
 * listener-relative DBAP/SPCAP/VBAP 26-gain solve with per-block gain ramps, plus occlusion,
 * directivity, Doppler, air absorption, reflection/pathing sends, dual-band panning, the
 * ambisonic bed, streaming, pause/seek, and the output limiter. See docs/roadmap.md for history.
 */
#ifndef BWA_RT_H
#define BWA_RT_H

#include "sink.h"          /* bwa_timestamp, BWA_CHANNELS */
#include "layout.h"        /* Layout (for rt_set_layout) */
#include "pose.h"          /* PoseSlot (for rt_set_tracker) */
#include "ism.h"           /* IsmRoom (for rt_set_ism_room) */

#include <stdbool.h>
#include <stdint.h>

/* Handles are (index | generation<<16); 0 is invalid (see include/bw_audio.h). */
#define BWA_H_IDX(h)   ((uint16_t)((h) & 0xFFFFu))
#define BWA_H_GEN(h)   ((uint16_t)((h) >> 16))
#define BWA_MK_H(i, g) ((uint32_t)(i) | ((uint32_t)(g) << 16))

/* Command ring payload (control -> audio). Fixed-size POD, no framing. */
#define BWA_EXTRA_LIS 3   /* extra (compromise) listener positions beyond the primary */
#define BWA_GROUPS    8   /* mix groups (per-voice group id 0..7; group 0 is the default) */
#define BWA_QUEUE     7   /* per-voice gapless play-queue depth (rt_source_queue / chaining) */

enum {
    CMD_SRC_CREATE = 0, CMD_SRC_DESTROY, CMD_SET_POS, CMD_SET_GAIN,
    CMD_PLAY, CMD_STOP, CMD_SET_LISTENER, CMD_COMMIT, CMD_SOUND_RETIRE,
    CMD_SET_REFLECTIONS, CMD_TEST_SIGNAL, CMD_SET_DOPPLER, CMD_SET_AIR, CMD_SET_SPREAD,
    CMD_SET_REFL_SEND, CMD_SET_REFL_DIST, CMD_SET_PATHING, CMD_SET_PAUSED, CMD_SEEK,
    CMD_SRC_STEAL,  /* fade a stolen voice out on its own slot, then free it (click-free voice-steal) */
    CMD_SET_LDC,    /* per-voice equal-loudness distance compensation enable */
    CMD_SET_EXTRA_LIS,  /* extra listener positions for multi-listener compromise panning */
    CMD_SET_SIZE,   /* per-voice metric source size (radius, meters) */
    CMD_FADE,       /* per-voice timed gain fade (optionally stop at the end) */
    CMD_SET_GROUP,  /* per-voice mix-group assignment */
    CMD_GROUP_GAIN, /* mix-group gain (re-dirties the group's voices) */
    CMD_GROUP_PAUSED,   /* mix-group pause gate */
    CMD_SET_PITCH,  /* per-voice playback rate (in-memory sounds) */
    CMD_BED_ROT,    /* per-bed soundfield orientation (yaw about up; + pitch/roll for the full rotation) */
    CMD_SET_ISM,    /* per-voice image-source early reflections */
    CMD_SET_ATTEN,  /* per-voice distance-attenuation override (replaces the layout curve for one source) */
    CMD_STOP_AT,    /* schedule a click-free stop when the dsp clock reaches a sample (rt_source_stop_at) */
    CMD_QUEUE,      /* append a sound to a voice's gapless play queue (rt_source_queue / chaining) */
    CMD_QUEUE_CLEAR /* drop a voice's pending play queue (rt_source_clear_queue) */
};
typedef struct {
    uint8_t  type;
    uint32_t handle;                 /* source or sound handle */
    union {
        struct { float x, y, z; }                      pos;
        struct { float g; }                            gain;
        struct { uint64_t start, loop_beg, loop_end; uint32_t sound; uint8_t loop, oneshot; } play;
                                         /* start = dsp-sample to begin (0 = now); loop_beg/loop_end =
                                          * loop region in frames (0/0 = whole clip; used only when loop) */
        struct { uint64_t sample; }                    stopat;/* dsp-sample to begin the click-free stop fade */
        struct { uint32_t sound; uint8_t loop; }       enq;   /* queue a sound to chain after the current one */
        struct { float px, py, pz, qx, qy, qz, qw; }   lis;
        struct { uint8_t on; }                         refl;
        struct { uint8_t on; }                         dop;   /* per-voice Doppler enable */
        struct { uint8_t on; }                         air;   /* per-voice air absorption enable */
        struct { float amount, height; }               spread;/* per-voice source angular width 0..1; height
                                                                * < 0 = isotropic, else the vertical extent
                                                                * (rt_source_set_extent, BS.2127-style w/h) */
        struct { float gain; }                         rsend; /* per-voice reverb wet-send level */
        struct { uint8_t on; }                         rdist; /* per-voice distance->wet scaling enable */
        struct { uint8_t on; }                         path;  /* per-voice pathing enable */
        struct { uint8_t on; }                         pause; /* per-voice pause gate (ramped; playhead freezes) */
        struct { uint64_t frame; }                     seek;  /* content position to jump to (in-memory sounds) */
        struct { uint32_t channel; uint8_t kind; float gain; } test;  /* debug channel injection */
        struct { uint8_t on; }                         ldc;   /* per-voice loudness-compensated attenuation */
        struct { float p[BWA_EXTRA_LIS][3]; uint8_t n; } exlis; /* extra listeners (compromise panning) */
        struct { float radius; }                       size;  /* per-voice source radius (meters; 0 = point) */
        struct { float target, seconds; uint8_t stop; } fade; /* timed gain fade (stop = stop when landed) */
        struct { uint8_t id; }                         group; /* mix-group assignment */
        struct { uint8_t id; float gain; }             ggain; /* mix-group gain */
        struct { uint8_t id, on; }                     gpause;/* mix-group pause */
        struct { float rate; }                         pitch; /* playback rate (1 = native) */
        struct { float yaw, pitch, roll; }             brot;  /* bed soundfield orientation (radians) */
        struct { uint8_t on; }                         ism;   /* per-voice early reflections */
        struct { float ref, rolloff, min_lin; }        atten; /* per-voice curve; ref <= 0 clears the override */
    } u;
} Cmd;

/* Event ring payload (audio -> control). */
enum { EVT_VOICE_ENDED = 0, EVT_SOUND_RETIRED };
typedef struct { uint8_t type; uint32_t handle; } Evt;

typedef struct RtCore RtCore;   /* opaque */

/* ---- lifecycle (control thread; allocates) ---- */
RtCore* rt_create(uint32_t voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels);
void    rt_destroy(RtCore* c);
void    rt_set_layout(RtCore* c, const Layout* L);   /* call before bwa_start / while stopped */
void    rt_set_panner(RtCore* c, int panner);        /* 0 = DBAP, 1 = SPCAP, 2 = VBAP; atomic, live-switchable */
void    rt_set_bed_decoder(RtCore* c, int decoder);  /* 1 = AllRAD, 2 = EPAD (0 = the SAD fallback — internal only,
                                                      * no longer reachable from the public enum); before bwa_start */
void    rt_set_dual_band(RtCore* c, int on);         /* dual-band panning (amplitude LF / power HF); live A/B */
void    rt_set_spread_mode(RtCore* c, int mode);     /* spread render: 0 = lobe, 1 = MDAP ring, 2 = spectral; live A/B */
void    rt_set_max_re(RtCore* c, int on);            /* max-rE bed-decode weighting (matrix paths + FDN); live A/B */
void    rt_set_max_re_split(RtCore* c, int on);      /* band-split max-rE: taper only > ~700 Hz (rV decode below); live A/B */
void    rt_set_room_eq_dyn(RtCore* c, int on);       /* tracked room EQ (room_eq_grid layouts): default on; live A/B */
void    rt_set_decorrelation(RtCore* c, int on);     /* velvet-noise wide-part decorrelation; live A/B */
void    rt_set_bed_renderer(RtCore* c, int parametric);   /* bed: 0 = matrix decode, 1 = parametric (DirAC); live A/B */
void    rt_set_pose_prediction(RtCore* c, float lead_s);  /* tracked-pose lead (0 = off); live */
void    rt_set_near_spread(RtCore* c, float radius_m);    /* near-listener widening radius (0 = off); live */
void    rt_set_extra_listeners(RtCore* c, const float* xyz, uint32_t n);   /* compromise panning; commit-gated */
void    rt_set_master_gain(RtCore* c, float linear);      /* one ramped scalar over the whole mix; live */
/* Image-source early reflections: the shoebox room (NULL/invalid = no reflections; set while stopped)
 * and the live reflection level. Voices opt in with rt_source_set_ism. */
void    rt_set_ism_room(RtCore* c, const IsmRoom* room);
void    rt_set_ism_gain(RtCore* c, float linear);
void    rt_set_all_paused(RtCore* c, int paused);         /* global pause gate (rides pause_gate); live */
void    rt_group_set_gain(RtCore* c, uint32_t group, float linear);   /* mix-group gain (enqueue) */
void    rt_group_set_paused(RtCore* c, uint32_t group, bool paused);  /* mix-group pause (enqueue) */
uint32_t rt_active_voices(RtCore* c);                     /* control thread: last block's active voice count */
void    rt_set_limiter(RtCore* c, int on);           /* output protection limiter (final stage; default ON); live */
void    rt_set_limiter_ceiling(RtCore* c, float ceiling_linear);   /* limit/clamp ceiling, linear (default -1 dBFS); live */

/* Attach the internal tracker's pose slot (bwa_tracker_connect). When set, rt_render samples the
 * freshest head pose from it at block time, overriding the committed listener. NULL detaches.
 * Set before the audio thread starts / after it stops (the audio thread reads the pointer). */
void    rt_set_tracker(RtCore* c, const PoseSlot* slot);

/* Post-mix aux-send tap: rt_render calls it on the AUDIO thread AFTER the voice loop and BEFORE
 * align_process, handing it the 26-ch bus, the block size, the active listener pose, and the summed
 * mono aux send (the post-occlusion/directivity signal of voices opted in via rt_source_set_reflections).
 * A phonon-free seam — the reflection bed registers a tap that convolves `aux` and sums onto `bus`.
 * Set while the audio thread is stopped (the audio thread reads the pointer). NULL detaches. */
typedef void (*RtBusTap)(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux);
void    rt_set_bus_tap(RtCore* c, RtBusTap tap, void* ud);
void    rt_source_set_reflections(RtCore* c, uint32_t h, bool on);   /* gate this voice into the aux send */
void    rt_source_set_reflection_send(RtCore* c, uint32_t h, float gain);    /* per-voice wet-send level (default 1) */
void    rt_source_set_reflection_distance(RtCore* c, uint32_t h, bool on);   /* scale the send by distance (far = wetter) */

/* Pathing seam: rt_render SH-encodes every pathing voice's signal (s * shCoeffs[k]) into a shared
 * ambisonic accumulator, then hands it to this tap AFTER the voice loop. The tap (steam_path) decodes
 * the ambisonic field to the 26-ch bus via phonon's own decoder, so phonon's convention is consistent
 * end-to-end. `ambi` is ambi_ch planar channels of n samples each. Set while stopped; NULL detaches. */
typedef void (*RtPathTap)(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* ambi, uint32_t ambi_ch);
void    rt_set_path_tap(RtCore* c, RtPathTap tap, void* ud, uint32_t ambi_ch);
void    rt_source_set_pathing(RtCore* c, uint32_t h, bool on);      /* gate this voice into the pathing render */
/* Off-thread pathing sim publishes a voice's path field: shCoeffs[ambi_ch] (the indirect arrival
 * directions) + eq[3] (the bending-loss band tilt, linear per-band gains in [0,1], normalized so the
 * loudest band = 1 — a pure spectral shape; the level rides shCoeffs). Pass eq=NULL for flat (no
 * bending loss). Handle-gated + double-buffered; the audio thread ramps to both (no zipper). */
void    rt_set_pathing(RtCore* c, uint32_t handle, const float* sh, const float* eq, uint32_t ambi_ch);
/* Debug: drive output `channel` with a built-in test signal (kind 0=off/1=sine/2=noise), injected
 * AFTER the per-speaker align stage (raw channel). Control thread; takes effect next block. */
void    rt_test_signal(RtCore* c, uint32_t channel, uint8_t kind, float gain);

/* Publish a voice's occlusion transmittance (1 = clear .. 0 = blocked), applied to its mono signal
 * before the DBAP pan. Called from the off-thread occlusion sim (not the control thread). Stale/
 * recycled handles are dropped; the audio thread ramps to it (no zipper). */
void    rt_set_occlusion(RtCore* c, uint32_t handle, float transmittance);
/* Occlusion with a 3-band transmission tilt: broadband `level` + normalized `band_gains[3]` (the
 * audio thread applies a 3-biquad EQ). Same off-thread publisher + handle gate as rt_set_occlusion. */
void    rt_set_occlusion_eq(RtCore* c, uint32_t handle, float level, const float band_gains[3]);
/* Full direct-effect publish: broadband level + 3-band tilt + directivity gain, in one handle-gated
 * store set. The off-thread sim calls this per source; the audio thread ramps level/dir and EQ. */
void    rt_set_direct(RtCore* c, uint32_t handle, float level, const float bands[3], float dir);
float   rt_get_occlusion(RtCore* c, uint32_t handle);   /* control thread: published factor (1 = clear) */
float   rt_get_directivity(RtCore* c, uint32_t handle); /* control thread: published gain (1 = on-axis) */

/* ---- assets (control thread; file I/O + alloc) ---- */
uint32_t rt_load_sound  (RtCore* c, const char* path, char* err, size_t errcap); /* 0 on failure */
uint32_t rt_load_sound_streaming(RtCore* c, const char* path, char* err, size_t errcap); /* mono, engine rate, streamed */
uint32_t rt_load_ambix  (RtCore* c, const char* path, char* err, size_t errcap); /* multichannel bed asset */
uint32_t rt_load_fuma   (RtCore* c, const char* path, char* err, size_t errcap); /* FuMa bed, converted to AmbiX at load */
uint16_t rt_sound_channels(RtCore* c, uint32_t sound);   /* 1 = mono, 4/9/16 = bed, 0 = invalid */
uint64_t rt_sound_frames  (RtCore* c, uint32_t sound);   /* length in frames at the engine rate;
                                                          * 0 = invalid, or unknown (push streams) */
bool     rt_sound_is_stream(RtCore* c, uint32_t sound);  /* streamed/push (reports 1 channel like mono) */
bool     rt_unload_sound(RtCore* c, uint32_t sound);  /* safe any time; retire-acked internally.
                                                       * false = command ring full, nothing enqueued:
                                                       * retry later (internal sounds park in rt.c) */

/* ---- control thread: handle allocation is synchronous, the rest enqueue ---- */
uint32_t rt_source_create(RtCore* c);                 /* steals the lowest-priority source if the table is full */
/* PUSH source (procedural audio): a source whose voice plays caller-pushed PCM through a per-source
 * ring instead of a loaded sound — same voice, same spatial path, second feeding path. Consuming
 * starts at create (an empty ring renders silence, it never ends the voice); rt_source_push feeds it
 * (returns frames accepted; pace with rt_source_push_space), rt_source_push_end marks end-of-data
 * (the voice ends once the ring drains; not restartable). rt_source_stop / a stop_at_end fade also
 * END it (one-way, like push_end — pushes are refused after; pause is the temporary silence). The
 * internal sound slot retires with the source handle. Push from the control thread — the ring is SPSC. */
uint32_t rt_source_create_stream(RtCore* c, char* err, size_t errcap);   /* 0 + err on failure */
uint32_t rt_source_push(RtCore* c, uint32_t h, const float* frames, uint32_t n);
uint32_t rt_source_push_space(RtCore* c, uint32_t h);
void     rt_source_push_end(RtCore* c, uint32_t h);
bool     rt_source_is_push(RtCore* c, uint32_t h);    /* control thread: is this a live push source? */
bool     rt_source_live(RtCore* c, uint32_t h);       /* control thread: live source handle of ANY kind
                                                       * (distinguishes wrong-kind misuse from a stale
                                                       * handle's documented silent no-op) */
void rt_source_destroy (RtCore* c, uint32_t h);
void rt_source_set_priority(RtCore* c, uint32_t h, int priority);   /* 0 = expendable .. 255 = protected (default 128) */
void rt_source_set_pos (RtCore* c, uint32_t h, float x, float y, float z);
void rt_source_set_gain(RtCore* c, uint32_t h, float linear);
void rt_source_play    (RtCore* c, uint32_t h, uint32_t sound, bool loop);
void rt_source_play_at (RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample);  /* sample-accurate */
void rt_source_play_loop(RtCore* c, uint32_t h, uint32_t sound, uint64_t loop_beg, uint64_t loop_end);  /* intro->loop region */
void rt_source_stop    (RtCore* c, uint32_t h);
void rt_source_stop_at (RtCore* c, uint32_t h, uint64_t stop_sample);   /* click-free stop when the dsp clock reaches it */
void rt_source_queue   (RtCore* c, uint32_t h, uint32_t sound, bool loop);  /* chain: play after the current sound ends (gapless) */
void rt_source_clear_queue(RtCore* c, uint32_t h);                     /* drop the pending chain */
void rt_source_set_paused(RtCore* c, uint32_t h, bool paused);   /* ramped gate; the playhead freezes once silent */
void rt_source_seek    (RtCore* c, uint32_t h, uint64_t frame);  /* click-free jump (in-memory sounds; streams ignore) */
void rt_source_set_doppler(RtCore* c, uint32_t h, bool on);          /* propagation: glided delay -> pitch from radial motion */
void rt_source_set_air_absorption(RtCore* c, uint32_t h, bool on);   /* propagation: distance-driven HF low-pass */
void rt_source_set_loudness_comp(RtCore* c, uint32_t h, bool on);    /* equal-loudness LF shelf vs attenuation */
void rt_source_set_spread(RtCore* c, uint32_t h, float amount);      /* source angular width: 0 = point .. 1 = wide */
void rt_source_set_extent(RtCore* c, uint32_t h, float w, float hgt);/* anisotropic width/height extent (room-referenced) */
void rt_source_set_attenuation(RtCore* c, uint32_t h, float ref_m, float rolloff, float min_lin);  /* per-source curve;
                                                                      * ref <= 0 = back to the layout's; rolloff 0 = constant */
void rt_source_set_size  (RtCore* c, uint32_t h, float radius_m);    /* source METRIC size: spread from subtended angle */
void rt_source_fade_to   (RtCore* c, uint32_t h, float gain, float seconds, bool stop_at_end);  /* timed fade */
void rt_source_set_group (RtCore* c, uint32_t h, uint32_t group);    /* mix-group assignment (0 = default) */
void rt_source_set_pitch (RtCore* c, uint32_t h, float rate);        /* playback rate [0.25, 4]; glided */
void rt_bed_set_rotation (RtCore* c, uint32_t h, float yaw_rad);     /* bed soundfield yaw (= orientation yaw,0,0) */
void rt_bed_set_orientation(RtCore* c, uint32_t h, float yaw, float pitch, float roll);  /* full 3-axis; glided */
void rt_source_set_ism   (RtCore* c, uint32_t h, bool on);           /* image-source early reflections */
void rt_play_oneshot   (RtCore* c, uint32_t sound, float x, float y, float z, float gain);
void rt_set_listener   (RtCore* c, const float p[3], const float q[4]);
void rt_commit         (RtCore* c);                   /* enqueue CMD_COMMIT + drain events */

/* ---- audio thread: drain the command ring, then mix one block into `bus` ---- */
void rt_render(RtCore* c, float* bus, uint32_t nframes, const bwa_timestamp* ts);
void rt_get_listener(RtCore* c, float p[3], float q[4]);   /* audio thread: active pose */
void rt_read_pose(RtCore* c, float p[3], float q[4]);      /* control thread: active pose (seqlock readback) */
bool rt_source_is_playing(RtCore* c, uint32_t h);         /* control thread: is the source's voice still playing? */
uint64_t rt_source_get_position(RtCore* c, uint32_t h);   /* control thread: content playhead (engine-rate frames) */
uint64_t rt_dsp_time(RtCore* c);                          /* control thread: current dsp-sample clock (for scheduling) */
bool rt_get_clock(RtCore* c, uint64_t* sample, uint64_t* time_ns); /* control thread: device (sample, host-ns) pair;
                                                                    * false until a host-stamped block renders */
uint32_t rt_bus_peaks(RtCore* c, float* out, uint32_t cap); /* control thread: last block's per-channel output peak
                                                             * (post align/test/limiter); returns the count filled */

#endif /* BWA_RT_H */
