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
 * ambisonic bed, streaming, pause/seek, the direct output-channel route, and the output limiter.
 * See NOTES.md for history.
 */
#ifndef BWA_RT_H
/* Upper bound for any linear gain the ABI accepts (+80 dB). Guards the finite-but-absurd case:
 * a merely `isfinite` gain can still overflow the bus to Inf and poison downstream filter state. */
#define BWA_MAX_GAIN 1.0e4f

/* Upper bound for any SAMPLE that enters from outside — decoded files (sound.c) and the push/stream
 * rings (stream.c). Same finite-but-absurd class as BWA_MAX_GAIN: a float wav can carry 3e38
 * verbatim, and the align delay lines + room-EQ biquads sit BEFORE the limiter, so an overflow
 * lands in filter state that never recovers. +30 dBFS clears any legitimately hot float master. */
#define BWA_MAX_SAMPLE 32.0f

/* Upper bound (meters, per axis) for any room COORDINATE the ABI accepts: listener pose, source
 * position, extra listeners, the tracked head pose. Third member of the same finite-but-absurd
 * family as BWA_MAX_GAIN and BWA_MAX_SAMPLE, and the one the guards were missing. Every spatial
 * solve starts from a SQUARED difference (dbap.c's dist2, the spread frame's normalize, the ISM
 * path length), so a merely `isfinite` coordinate near FLT_MAX makes dx*dx overflow to +Inf; the
 * normalize that follows is then Inf/Inf or Inf*0, which is NaN, and the NaN lands in the gain
 * vector where the gcur ramp x + (t - x) * k can never leave it. 1e6 m keeps 3 * (2e6)^2 = 1.2e13
 * far inside float range with room for every downstream multiply, and it is well past any real
 * scene (the CAVE is 3 m across). */
#define BWA_MAX_COORD 1.0e6f

/* Upper bound (meters) for an ISM room DIMENSION (bwa_scene_set_ism_room / bwa_scene_set_box).
 * Same finite-but-absurd family, but a dimension is NOT a coordinate: it is a positive extent, and
 * ism.c MULTIPLIES it. Order 1 is the whole model (one mirror per face, six images) and the mirror
 * is `2*plane - src`, so the +y face's image reaches 2h while the box's own inside test already
 * pins src between the faces (|image_x| <= 1.5w, |image_y| <= 2h, |image_z| <= 1.5d). Half of
 * BWA_MAX_COORD is exactly the largest of those factors, so every image ism.c hands to the panner
 * lands back inside the envelope the ABI guarantees for a coordinate, instead of being an
 * UNBOUNDED value derived from bounded ones. Without it, w = h = d = 3e38 passes the inside test
 * and mirrors to +Inf, whose dist2 in the panner normalizes to NaN and sticks in the ISM gain ramp
 * exactly as an absurd listener pose did. 500 km is far past any room anyone will model.
 *
 * bwa_scene_set_ground's `y` takes BWA_MAX_COORD instead, not this: a plane HEIGHT is a room
 * coordinate, the same kind and units as a source position. plane_only has no inside test, so its
 * one image can reach 3 * BWA_MAX_COORD (2*y - src_y); that is still 4.8e13 once squared and
 * summed over three axes, which is where the headroom in BWA_MAX_COORD goes. */
#define BWA_MAX_ROOM_DIM (0.5f * BWA_MAX_COORD)

#define BWA_RT_H

#include "sink.h"          /* bwa_timestamp, BWA_CHANNELS */
#include "sound.h"         /* SoundData (for the async staging calls at the end of the assets block) */
#include "layout.h"        /* Layout (for rt_set_layout) */
#include "pose.h"          /* PoseSlot (for rt_set_tracker) */
#include "ism.h"           /* IsmRoom (for rt_set_ism_room) */

#include <stdbool.h>
#include <stdint.h>

/* Extra physical voice slots beyond the caller's requested pool (rt_create allocates
 * req_voice_cap + BWA_FADE_RESERVE). A full-pool steal fades the victim out on its own slot and
 * places the NEW source on a reserve slot — so a live handle's index can reach the physical count,
 * not just the requested pool. Any per-source table indexed by BWA_H_IDX must be sized to the
 * PHYSICAL count, or exactly the sources that survived pool pressure silently lose features. */
#define BWA_FADE_RESERVE 8

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
    CMD_QUEUE_CLEAR,/* drop a voice's pending play queue (rt_source_clear_queue) */
    CMD_SET_NF,     /* per-voice near-field proximity boost enable */
    CMD_SET_DIR,    /* per-voice MANUAL directivity: forward axis + weighted-dipole pattern (no-sim path) */
    CMD_SRC_CFG,    /* per-voice CONFIGURATION in one command (rt_source_apply_cfg / bwa_source_apply):
                     * every ring-carried per-source knob at once, so a struct apply costs ONE ring
                     * slot instead of fifteen. The payload is packed (bitfield flags) to keep the
                     * Cmd union at the width `exlis` already sets — a wider Cmd would tax every
                     * command in the ring for the benefit of this one. */
    CMD_GROUP_STOP, /* click-free stop of every voice in one mix group (rt_group_stop) */
    CMD_STOP_ALL,   /* click-free stop of every voice, whatever its group (rt_stop_all) */
    CMD_SET_CHANNEL,/* per-voice DIRECT output-channel route (rt_source_set_channel): the solve installs a
                     * one-hot gain vector instead of the panner's, so the reference condition stays on the
                     * same ramp + output stage as the phantom it is A/B'd against */
    CMD_SET_REGION  /* per-voice play region [start,end) in content frames (rt_source_set_region) */
};
typedef struct {
    uint8_t  type;
    uint32_t handle;                 /* source or sound handle */
    union {
        struct { float x, y, z; }                      pos;
        struct { float g; }                            gain;
        struct { uint64_t start, loop_beg, loop_end; uint32_t sound; uint8_t loop, oneshot, seq; } play;
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
        struct { uint8_t on, ch; }                     outch; /* direct output-channel route (on = 0 restores panning) */
        struct { uint64_t beg, end; }                  region;/* play region in content frames (end 0 = asset end) */
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
        struct { uint8_t on; }                         nf;    /* per-voice near-field proximity boost enable */
        struct { float fwd[3], weight, power; }        dir;   /* manual directivity: forward axis (room, unit)
                                                               * + dipole weight 0..1 / power (weight 0 = off) */
        /* One-command per-voice configuration (CMD_SRC_CFG). Sanitized on the control thread by
         * rt_source_apply_cfg, exactly like the individual setters it replaces. 9 floats + 2 bytes
         * = 40, which is what `exlis` already costs, so this member does NOT widen Cmd. */
        struct { float gain, pitch, spread, extent_h, size_m, rsend, aref, aroll, amin;
                 uint8_t group, flags; }               cfg;
    } u;
} Cmd;

/* CMD_SRC_CFG boolean payload. Packed rather than nine bytes so the cfg member stays inside the
 * union's existing width (see the member's comment). */
#define BWA_CFG_DOPPLER 0x01u
#define BWA_CFG_AIR     0x02u
#define BWA_CFG_LDC     0x04u
#define BWA_CFG_NF      0x08u
#define BWA_CFG_ISM     0x10u
#define BWA_CFG_REVERB  0x20u
#define BWA_CFG_RDIST   0x40u
#define BWA_CFG_PATH    0x80u

/* Event ring payload (audio -> control). */
/* EVT_VOICE_ENDED means RECYCLE this transient handle (a oneshot finishing, or a stolen slot).
 * EVT_VOICE_DONE is a pure NOTIFICATION that a caller-owned voice stopped playing: same ring, but
 * the handle stays the caller's, so drain_events records it for rt_poll_ended and recycles nothing.
 * They had to be separate: completion is not ownership, and the existing event only ever fired for
 * the handles the engine was taking back. */
/* EVT_VOICE_LOOPED is the third kind, and it is a NOTICE like DONE: one per loop WRAP (the mixer's
 * cursor reaching the region/clip end and jumping back), so it is unbounded per block and yields the
 * ring's reserve exactly as DONE does. Same seq/gen gate on the control side, its own drop counter. */
enum { EVT_VOICE_ENDED = 0, EVT_SOUND_RETIRED, EVT_VOICE_DONE, EVT_VOICE_LOOPED };
typedef struct { uint8_t type, seq; uint32_t handle; } Evt;   /* seq: the play a DONE belongs to */

typedef struct RtCore RtCore;   /* opaque */

/* ---- lifecycle (control thread; allocates) ---- */
RtCore* rt_create(uint32_t voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels);
void    rt_destroy(RtCore* c);
void    rt_set_layout(RtCore* c, const Layout* L);   /* call before bwa_start / while stopped */
void    rt_set_panner(RtCore* c, int panner);        /* 0 = DBAP, 1 = SPCAP, 2 = VBAP; atomic, live-switchable */
/* Direct-binaural render (BWA_PROFILE_BINAURAL): point voices SH-encode at their true listener-
 * relative direction into a 16-ch ambisonic accumulator (phonon monitor basis, ambi_encode_phonon)
 * instead of panning to the speaker bus; beds pass SH->SH into the same field and the pathing
 * accumulator sums in raw (both already that basis); the speaker bus keeps the synthesized-diffuse
 * layer (FDN / reflection-bed taps). Mode:
 *   0 = off   1 = SH field only   2 = SH field + PER-VOICE point taps (mode 2 needs a phonon
 * consumer: each point voice's post-DSP mono block + room-frame direction is exposed via
 * rt_direct_voices for one IPLBinauralEffect per voice; spread power-splits point vs field, so
 * the two render paths crossfade by solve, never switch). Set while STOPPED (engine create picks
 * 1, bwa_start upgrades to 2 when the steam monitor is live) — not a live toggle; a mode change
 * re-dirties every voice so the gain vectors re-solve in the new meaning.
 * rt_direct_ambi returns the block's summed field right after rt_render (same thread;
 * BWA_AMBI_CH planar channels of nframes), NULL when the mode is off. rt_direct_voices fills
 * *out with the voice_cap-long per-slot view (mode 2; returns 0 / NULL otherwise). */
typedef struct {
    const float* mono;   /* the voice's point-share block (nframes samples; gain + atten applied) */
    float    dir[3];     /* room-frame unit dir listener->source (block-rate, solved with the gains) */
    uint32_t gen;        /* voice generation — the consumer resets its effect state on change */
    uint8_t  active;     /* rendered this block (cleared at block start) */
} RtDirectVoice;
void    rt_set_direct_ambi(RtCore* c, int mode);
const float* rt_direct_ambi(RtCore* c);
uint32_t rt_direct_voices(RtCore* c, const RtDirectVoice** out);
void    rt_set_bed_decoder(RtCore* c, int decoder);  /* 1 = AllRAD, 2 = EPAD (0 = the SAD fallback — internal only,
                                                      * no longer reachable from the public enum); before bwa_start */
void    rt_set_dual_band(RtCore* c, int on);         /* dual-band panning (amplitude LF / power HF); live A/B */
void    rt_set_cap(RtCore* c, int on);               /* CAP on the dual-band low band (ITD-exact); live A/B */
void    rt_set_spread_mode(RtCore* c, int mode);     /* spread render: 0 = lobe, 1 = MDAP ring, 2 = spectral; live A/B */
void    rt_set_max_re(RtCore* c, int on);            /* max-rE bed-decode weighting (matrix paths + FDN); live A/B */
void    rt_set_max_re_split(RtCore* c, int on);      /* band-split max-rE: taper only > ~700 Hz (rV decode below); live A/B */
void    rt_set_room_eq_dyn(RtCore* c, int on);       /* tracked room EQ (room_eq_grid layouts): default on; live A/B */
/* Tracked listener alignment (align.h): re-reference the output stage's per-speaker delay + gain from
 * Layout.ref onto the live listener. Default OFF. `dead_zone_m` is how far the listener must move
 * before anything is recomputed, `slew_frames_per_s` the ceiling on delay change (the resampling
 * ratio, hence the pitch shift); either <= 0 reverts that one to its default. Live A/B. */
void    rt_set_tracked_align(RtCore* c, int on);
void    rt_set_tracked_align_guards(RtCore* c, float dead_zone_m, float slew_frames_per_s);
void    rt_set_decorrelation(RtCore* c, int on);     /* velvet-noise wide-part decorrelation; live A/B */
void    rt_set_bed_renderer(RtCore* c, int parametric);   /* bed: 0 = matrix decode, 1 = parametric (DirAC); live A/B */
void    rt_set_pose_prediction(RtCore* c, float lead_s);  /* tracked-pose lead (0 = off); live */
void    rt_set_near_spread(RtCore* c, float radius_m);    /* near-listener widening radius (0 = off); live */
/* Hole-aware spread floor (hole.h): a source aimed where the array has no speaker is floored WIDE
 * rather than rendered as a split image. `strength` scales the derived floor; 0 = off (default).
 * Live: bumps the panner generation so even a motionless voice re-solves (rt.c, RtCore.pan_gen). */
void    rt_set_hole_spread(RtCore* c, float strength);
/* SPCAP lobe sharpness + placement-correction density exponent; either <= 0 reverts that knob to the
 * layout's default (derived focus, constant density). Live: bumps the panner generation so even a
 * motionless voice re-solves (rt.c, RtCore.pan_gen). */
void    rt_set_spcap_focus(RtCore* c, float focus, float density);
void    rt_set_extra_listeners(RtCore* c, const float* xyz, uint32_t n);   /* compromise panning; commit-gated */
void    rt_set_master_gain(RtCore* c, float linear);      /* one ramped scalar over the whole mix; live */
/* Image-source early reflections: the shoebox room (NULL/invalid = no reflections) and the live
 * reflection level. The room is LIVE too — it publishes through a single-slot seqlock and the
 * mixer adopts a stable copy at block start, then each opted-in voice re-solves its images next
 * block (gains ramp, delays glide). Voices opt in with rt_source_set_ism. */
void    rt_set_ism_room(RtCore* c, const IsmRoom* room);
void    rt_set_ism_gain(RtCore* c, float linear);
void    rt_set_all_paused(RtCore* c, int paused);         /* global pause gate (rides pause_gate); live */
void    rt_group_set_gain(RtCore* c, uint32_t group, float linear);   /* mix-group gain (enqueue) */
void    rt_group_set_paused(RtCore* c, uint32_t group, bool paused);  /* mix-group pause (enqueue) */
/* Click-free stop of a whole group / of everything (enqueue; one command each). Every matching
 * voice takes rt_source_stop's one-block fade and drops its pending chain. An out-of-range group
 * is ignored, as with the gain/pause calls.
 * Both also discard the matching HELD plays (the ones still waiting on an async decode), or those
 * would start by themselves once their data landed. A held play has no voice, so rt_group_stop
 * matches it on the control-side group mirror (RtCore.group) instead. Both drop the held plays
 * only AFTER the command push landed, so a full ring leaves no half-effect. */
void    rt_group_stop(RtCore* c, uint32_t group);
void    rt_stop_all(RtCore* c);
uint32_t rt_active_voices(RtCore* c);                     /* control thread: last block's active voice count */
uint64_t rt_stream_starves(RtCore* c);                    /* streamed voices that ran dry without ending */
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
/* Control-thread readbacks. Both take the SAME liveness gate rt_source_is_playing and
 * rt_source_get_position take: a destroyed, stale, or recycled handle reads the neutral value
 * immediately (1 = clear / on-axis), not the dead voice's last publish. Publish for a handle that
 * never came from rt_source_create and these report nothing back — the publish slot still holds it,
 * but the readback refuses to speak for a slot with no live occupant. */
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

/* ---- async staging (control thread; the asset cache's half of bwa_sound_acquire_async) ----
 * A RESERVED slot is a live, generation-counted sound handle whose PCM has not arrived yet. The
 * audio thread must never hold a pointer into a slot the control thread is still going to write,
 * so every bind path refuses a reserved handle; a play issued against one is HELD here and
 * re-issued by rt_sound_publish, which makes it an ordinary CMD_PLAY carrying finished data. That
 * is the whole synchronization: the existing command-ring release/acquire publishes the buffer,
 * exactly as it does for a synchronous load. Nothing new reaches the audio thread. */
uint32_t rt_sound_reserve(RtCore* c, char* err, size_t errcap);  /* 0 = sound table full */
bool     rt_sound_pending(RtCore* c, uint32_t sound);            /* reserved, data not published yet */
/* Publish decoded PCM into a reserved slot and release every play held against it. Takes OWNERSHIP
 * of *d. false = stale or not-reserved handle, and then the CALLER still owns *d (sound_unload it).
 * A held play whose KIND disagrees with the decoded asset (a bed play that resolved to mono, or a
 * point-source play that resolved to 4/9/16 channels) is DROPPED here, not bound, and counted in
 * rt_held_kind_drops. A reserved slot reports 0 channels, so this is the first moment the kind can
 * be checked at all. */
bool     rt_sound_publish(RtCore* c, uint32_t sound, const SoundData* d);
/* Running total of the held plays rt_sound_publish dropped for a kind mismatch (control thread).
 * Monotonic, so a caller compares it against what it last saw; engine.c does exactly that in
 * bwa_commit and reports each new drop through bwa_last_error. */
uint64_t rt_held_kind_drops(RtCore* c);
/* Drop a reserved slot whose load failed or was cancelled: discard the held plays and recycle the
 * handle. No retire-ack is needed because the audio thread was never given the slot. */
void     rt_sound_abandon(RtCore* c, uint32_t sound);

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
/* The same binds as rt_source_play/_play_at/_play_loop, tagged as an ambisonic BED. A bed is the
 * same voice on the same path, so the only difference is the kind recorded for an async (held)
 * play — the voice itself cannot be asked later which kind the caller meant. engine.c's
 * bwa_bed_play/_play_at/_play_loop call these. */
void rt_bed_play       (RtCore* c, uint32_t h, uint32_t sound, bool loop);
void rt_bed_play_at    (RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample);
void rt_bed_play_loop  (RtCore* c, uint32_t h, uint32_t sound, uint64_t loop_beg, uint64_t loop_end);
void rt_source_stop    (RtCore* c, uint32_t h);
void rt_source_stop_at (RtCore* c, uint32_t h, uint64_t stop_sample);   /* click-free stop when the dsp clock reaches it */
void rt_source_queue   (RtCore* c, uint32_t h, uint32_t sound, bool loop);  /* chain: play after the current sound ends (gapless) */
void rt_source_clear_queue(RtCore* c, uint32_t h);                     /* drop the pending chain */
void rt_source_set_paused(RtCore* c, uint32_t h, bool paused);   /* ramped gate; the playhead freezes once silent */
void rt_source_seek    (RtCore* c, uint32_t h, uint64_t frame);  /* click-free jump (in-memory sounds; streams ignore) */
/* Play region [start,end) in CONTENT frames (in-memory + bed sounds; streams/push ignore, as seek does).
 * end 0 = the asset end. It is the same voice state rt_source_play_loop resolves at play time, so a
 * later play RESETS it; set it after the play. A non-looping voice ENDS at `end` (EVT_VOICE_DONE, the
 * queue chains as at any end), a looping one wraps to `start` (EVT_VOICE_LOOPED). A degenerate
 * end <= start (with end != 0) is refused on the control thread. */
void rt_source_set_region(RtCore* c, uint32_t h, uint64_t start_frame, uint64_t end_frame);
/* DIRECT output-channel route: send this voice's mono content to ONE bus channel with NO spatial
 * processing (the psychophysics ground-truth / reference condition). channel < 0 = back to the panner.
 * Implemented as a one-hot GAIN VECTOR at the panner's output, not a post-align injection, so the
 * route ramps like any other gain change (invariant 4) and the per-speaker align trims/delays, room
 * EQ, master gain and the limiter apply exactly as they do to a panned voice — which is what makes
 * the reference LEVEL-COMPARABLE with the phantom. An out-of-range channel is refused here (the
 * control thread; c->channels is fixed for the engine's life). Beds ignore it. */
void rt_source_set_channel(RtCore* c, uint32_t h, int channel);
void rt_source_set_doppler(RtCore* c, uint32_t h, bool on);          /* propagation: glided delay -> pitch from radial motion */
void rt_source_set_air_absorption(RtCore* c, uint32_t h, bool on);   /* propagation: distance-driven HF low-pass */
void rt_source_set_loudness_comp(RtCore* c, uint32_t h, bool on);    /* equal-loudness LF shelf vs attenuation */
void rt_source_set_proximity(RtCore* c, uint32_t h, bool on);        /* propagation: near-field LF boost (dist < ~1 m) */
/* MANUAL directivity (no sim): the audio thread evaluates |(1-w) + w*cos(theta)|^p per block from
 * the voice's forward axis and the active listener — walk-correct without the Steam sim. weight 0
 * disables. Same ramped dir_cur the sim's publish drives; do not run both on one source. */
void rt_source_set_directivity_manual(RtCore* c, uint32_t h, const float fwd[3], float weight, float power);
/* Engine-wide speed of sound (m/s; default 343). Live + atomic: Doppler and ISM path delays derive
 * from it next block (both glide, so a change bends, never steps). Delays saturate at each ring's
 * capacity, so a c far below air's mostly shows up as pitch/glide behavior, not unbounded delay. */
void rt_set_speed_of_sound(RtCore* c, float mps);
void rt_source_set_spread(RtCore* c, uint32_t h, float amount);      /* source angular width: 0 = point .. 1 = wide */
void rt_source_set_extent(RtCore* c, uint32_t h, float w, float hgt);/* anisotropic width/height extent (room-referenced) */
void rt_source_set_attenuation(RtCore* c, uint32_t h, float ref_m, float rolloff, float min_lin);  /* per-source curve;
                                                                      * ref <= 0 = back to the layout's; rolloff 0 = constant */
void rt_source_set_size  (RtCore* c, uint32_t h, float radius_m);    /* source METRIC size: spread from subtended angle */
/* Every ring-carried per-source knob at once (bwa_source_apply). Same fields, same units, and the
 * same control-thread sanitizing as the individual setters above — it is one COMMAND, not one
 * semantics. Anything per-frame (position, orientation, playback state) is deliberately absent, and
 * so is anything that never reaches the ring (steal priority, the occlusion/directivity sims —
 * engine.c owns those). extent_h < 0 = isotropic (the spread field is then the whole width);
 * atten_ref <= 0 clears the distance-attenuation override. */
typedef struct RtSrcCfg {
    float gain, pitch, spread, extent_h, size_m, reverb_send;
    float atten_ref, atten_rolloff, atten_min;
    uint32_t group;
    bool  doppler, air, loudness_comp, proximity, early_reflections, reverb, reverb_distance, pathing;
} RtSrcCfg;
void rt_source_apply_cfg(RtCore* c, uint32_t h, const RtSrcCfg* cfg);
void rt_source_fade_to   (RtCore* c, uint32_t h, float gain, float seconds, bool stop_at_end);  /* timed fade */
void rt_source_set_group (RtCore* c, uint32_t h, uint32_t group);    /* mix-group assignment (0 = default) */
void rt_source_set_pitch (RtCore* c, uint32_t h, float rate);        /* playback rate [0.25, 4]; glided */
void rt_bed_set_orientation(RtCore* c, uint32_t h, float yaw, float pitch, float roll);  /* full 3-axis; glided (yaw,0,0 = the exact phasor yaw path) */
void rt_source_set_ism   (RtCore* c, uint32_t h, bool on);           /* image-source early reflections */
bool rt_play_oneshot   (RtCore* c, uint32_t sound, float x, float y, float z, float gain);
                                                     /* false = dropped (bad handle/args, full
                                                      * voice pool, or full command ring) */
void rt_set_listener   (RtCore* c, const float p[3], const float q[4]);
void rt_commit         (RtCore* c);                   /* enqueue CMD_COMMIT + drain events */

/* ---- audio thread: drain the command ring, then mix one block into `bus` ---- */
void rt_render(RtCore* c, float* bus, uint32_t nframes, const bwa_timestamp* ts);
void rt_get_listener(RtCore* c, float p[3], float q[4]);   /* audio thread: active pose */
void rt_read_pose(RtCore* c, float p[3], float q[4]);      /* control thread: active pose (seqlock readback) */
bool rt_source_is_playing(RtCore* c, uint32_t h);         /* control thread: is the source's voice still playing? */
/* control thread: drain handles whose voices ENDED since the last call (see rt.c) */
uint32_t rt_poll_ended(RtCore* c, uint32_t* out, uint32_t cap, uint64_t* dropped_out);
/* control thread: drain handles whose voices WRAPPED at a loop point since the last call. Same ring
 * shape, same drop-oldest bound, its own counters (see rt.c). */
uint32_t rt_poll_looped(RtCore* c, uint32_t* out, uint32_t cap, uint64_t* dropped_out);
/* control thread: the live POST-CLAMP values of the knobs rt sanitizes (see rt.c) */
void rt_get_spcap_sanitized(RtCore* c, float* focus, float* density);
void rt_get_tuning_sanitized(RtCore* c, int* panner, int* spread_mode, float* near_spread,
                             float* hole_spread, float* dead_zone_m, float* slew_frames_per_s);
uint64_t rt_source_get_position(RtCore* c, uint32_t h);   /* control thread: content playhead (engine-rate frames) */
uint64_t rt_dsp_time(RtCore* c);                          /* control thread: current dsp-sample clock (for scheduling) */
void rt_reset_clock(RtCore* c);   /* drop the device-clock pair: a restart re-bases the sample clock */
bool rt_get_clock(RtCore* c, uint64_t* sample, uint64_t* time_ns); /* control thread: device (sample, host-ns) pair;
                                                                    * false until a host-stamped block renders */
/* Device-vs-host clock drift, fitted over the same block stamps (rt.c, RtCore.fit_*). Mirrors the
 * public bwa_clock_model; engine.c copies it across so rt.h stays off the ABI. */
typedef struct RtClockFit {
    double   ppm, ppm_sigma, rate_hz, span_s, jitter_ns;
    uint32_t stamps;
} RtClockFit;
bool rt_get_clock_model(RtCore* c, RtClockFit* out);      /* control thread: false until the fit has span */
uint32_t rt_bus_peaks(RtCore* c, float* out, uint32_t cap); /* control thread: last block's per-channel output peak
                                                             * (post align/test/limiter); returns the count filled */

#endif /* BWA_RT_H */
