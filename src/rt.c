/*
 * rt.c — real-time core. Two SPSC rings + voice table + commit snapshot + generation
 * handles, exactly as specified in docs/concurrency.md, plus the Sound table and the
 * retire-ack handshake. M3 mixes wav playback (mix_voice reads sound->pcm); routing is
 * still the M2 placeholder (one position-derived channel) until M4's DBAP solve. Nothing
 * here allocates/locks on rt_render.
 */
#include "rt.h"
#include "sound.h"
#include "stream.h"
#include "layout.h"
#include "dbap.h"
#include "spcap.h"
#include "vbap.h"
#include "cap.h"          /* compensated amplitude panning: the dual-band low band's ITD correction */
#include "hole.h"         /* hole-aware spread floor: array holes widen a source instead of splitting it */
#include "align.h"
#include "ism.h"          /* image-source early reflections (shoebox geometry; phonon-free) */
#include "biquad.h"       /* shared RBJ cookbook (also used by align.c's room_eq) */
#include "ambisonics.h"   /* SH->26 decode for ambisonic beds (+ room_to_ambi, ambi_sad_decode) */
#include "allrad.h"       /* robust SH->26 decode for irregular arrays */
#include "epad.h"         /* energy-preserving SH->26 decode (bed_decoder = 2) */
#include "bits.h"         /* bwa_pow2_ge */
#include "sane.h"         /* bwa_quat_unit, bwa_finite3_bounded, bwa_finite_clamp */
#include "profile.h"      /* Tracy zones/plots (no-ops unless BWA_TRACY=ON) */

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <xmmintrin.h>     /* _MM_SET_FLUSH_ZERO_MODE */
#include <pmmintrin.h>     /* _MM_SET_DENORMALS_ZERO_MODE */
#endif

#define RING_CAP 4096          /* power of two; sized for a worst-case frame burst */
#define EVT_CAP  1024          /* power of two */
#define BWA_RT_MAX_BLOCK 8192   /* aux-send scratch cap; must be >= any device block (matches engine's BWA_MAX_BLOCK) */

/* clock-drift fit (RtCore.fit_*): the exponential window and the point at which the slope is worth
 * reporting. TAU trades convergence against precision — 120 s is ~22k stamps at 256/48k, enough to
 * pull sub-ppm out of stamp jitter, and short enough to follow a crystal warming up. Below MIN_SPAN
 * the lever arm is too short for the number to mean anything, so rt_get_clock_model reports nothing. */
#define BWA_CLK_TAU_S      120.0
#define BWA_CLK_MIN_SPAN_S   1.0

/* propagation effects (opt-in, per voice) */
#define BWA_SPEED_OF_SOUND   343.0f    /* m/s — the DEFAULT; live via rt_set_speed_of_sound (RtCore.sos) */
#define BWA_SOS_MIN           30.0f    /* settable range: below ~30 m/s every delay saturates its ring */
#define BWA_SOS_MAX        20000.0f
#define BWA_DOPPLER_MAX_DIST 8.0f      /* propagation delay saturates past this (bounds the per-voice ring) */
#define BWA_DOPPLER_TAU      0.032f    /* per-pole delay low-pass time (s); FFT-tuned, see mix_voice */
#define BWA_AIR_FC_NEAR   18000.0f     /* air-absorption low-pass cutoff (Hz) at zero distance ... */
#define BWA_AIR_FC_PER_M    650.0f     /* ... falling this many Hz per meter ... */
#define BWA_AIR_FC_FLOOR   1200.0f     /* ... down to this floor */
/* near-field proximity boost (opt-in, per voice): an LF shelf that rises as the source closes inside
 * BWA_NF_RADIUS — the spherical-wavefront proximity effect, the near-distance mirror of the
 * loudness-comp shelf (which restores body FAR away). Perceptually load-bearing in a walkable
 * volume: "at arm's length" reads as bass, not just level. */
#define BWA_NF_RADIUS  1.0f            /* boost region (m): 0 dB at the radius ... */
#define BWA_NF_MAX_DB  6.0f            /* ... rising linearly to this at distance 0 */
#define BWA_NF_FC    300.0f            /* shelf corner (Hz; one-pole, like the loudness-comp shelf) */
/* distance->reverb send: the wet-send factor ramps from NEAR_SEND at NEAR_DIST to 1.0 at FAR_DIST */
#define BWA_REFL_NEAR_DIST  1.0f
#define BWA_REFL_FAR_DIST   6.0f
#define BWA_REFL_NEAR_SEND  0.25f
#define BWA_DUALBAND_FC     700.0f     /* dual-band panning crossover (Hz): amplitude below, power above */
/* spectral widening (BWA_SPREAD_SPECTRAL): split a spread voice into BWA_FS_BANDS complementary
 * one-pole bands and pan EACH BAND to its own direction inside the spread cone — frequency-dependent
 * panning (Zotter & Frank's phantom-source widening, ambix_widening's idea, applied per source): the
 * ear integrates the spectrally scattered directions into WIDTH with no decorrelation noise and no
 * phantom collapse, and every band gain is a real panner solve, so the extent is panner-true. */
#define BWA_FS_BANDS  6
#define BWA_FS_XOVERS 5
static const float BWA_FS_XOVER[BWA_FS_XOVERS] = { 250.f, 700.f, 1800.f, 4500.f, 10000.f };
/* decorrelation (bwa_set_decorrelation): per-channel velvet-noise filters over BWA_DECOR_MS with
 * BWA_DECOR_TAPS sparse taps — ~30 MACs/sample/channel, time-domain, no FFT, no onset latency
 * (Valimaki/Schlecht et al., Velvet-Noise Decorrelator, DAFx-17/18). */
#define BWA_DECOR_TAPS 30
#define BWA_DECOR_MS   30.0f
/* parametric bed renderer (bwa_set_bed_renderer): FOA intensity-vector analysis in BWA_PARA_BANDS
 * time-domain bands (one-pole crossovers at BWA_PARA_XOVER Hz), direction + diffuseness per band —
 * first-order DirAC (Pulkki) with block-rate parameters instead of an STFT. */
#define BWA_PARA_BANDS 4
#define BWA_PARA_TAU   0.060f          /* intensity/energy smoothing time (s) */
static const float BWA_PARA_XOVER[3] = { 200.f, 800.f, 3200.f };
#define BWA_BED_YAW_RATE 6.2831853f    /* bed-rotation glide (rad/s): one full turn per second */
/* image-source early reflections (bwa_source_set_early_reflections; ism.c): each of the room's six
 * first-order images is rendered as a POINT SOURCE through the listener-relative panner — correct
 * direction AND parallax as the listener walks. The late tail is the FDN's job (fdn.c). */
#define BWA_ISM_MAX_M    60.0f         /* longest reflection path the per-voice ring holds (bounds the delay) */
#define BWA_ISM_TAU      0.020f        /* per-image delay glide (s): a moving source bends, never steps */

typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
typedef struct { Evt slots[EVT_CAP];  _Atomic uint32_t write, read; } EvtRing;
typedef struct { uint32_t handle; float sh[BWA_AMBI_CH]; float eq[3]; } PathPub;   /* one double-buffer slot of a voice's path field (directions + bending-loss band tilt) */

typedef struct { float cw0, alpha; int type; } EqProto;   /* per-band biquad prototype, rate-derived at create */

/* 3-band per-voice EQ state (audio-thread-only): the transmission (occlusion) EQ and the pathing
 * bending-loss EQ are structurally identical, so both are a VoiceEq. g_cur are the slewed band gains;
 * co the 3 sections' live biquad coeffs {b0,b1,b2,a1,a2}, INTERPOLATED per sample toward the block
 * target so the envelope never steps (invariant 4); x1/x2/y1/y2 are the Direct-Form-I history;
 * engaged gates the chain (bypassed once settled flat). See eq_block_setup/apply/land. */
typedef struct {
    float g_cur[3];
    float co[3][5];
    float x1[3], x2[3], y1[3], y2[3];
    int   engaged;
} VoiceEq;

typedef struct {
    uint16_t gen;
    bool     active, playing, loop, dirty, oneshot;
    uint8_t  play_seq;                      /* the control-side play counter this voice was started at */
    uint32_t pan_gen;                       /* RtCore.pan_gen this voice last solved at; a mismatch
                                             * re-solves it (see RtCore.pan_gen). Audio thread only. */
    const SoundData* sound;                 /* bound sound (NULL when idle); audio reads pcm */
    uint32_t cursor;                        /* sample cursor into sound->pcm (in-memory sounds) */
    uint32_t loop_beg, loop_end;            /* loop region [beg,end) in frames (in-memory/bed sounds); resolved
                                             * at CMD_PLAY, 0/0 = whole clip. Used only when v->loop: on reaching
                                             * loop_end the cursor wraps to loop_beg (the intro->loop pattern). */
    uint64_t stream_pos;                    /* absolute sample position into a streamed sound's ring */
    uint64_t start_sample;                  /* dsp-sample to begin output (0 = immediate); for scheduled play */
    uint64_t stop_at;                       /* scheduled click-free stop: dsp-sample at which the stop fade begins */
    bool     stop_sched;                    /* a stop_at is armed (rt_source_stop_at); cleared once it fires or on replay */
    /* gapless play queue (rt_source_queue / chaining): a FIFO of sounds to play, seamlessly, after the
     * current one ends. Entries are resolved SoundData pointers (like `sound`), so CMD_SOUND_RETIRE must
     * NULL any entry it frees (tombstone; queue_pop_valid skips NULLs). In-memory mono sounds only. */
    const SoundData* queue[BWA_QUEUE];
    uint8_t  queue_loop[BWA_QUEUE];         /* per-entry loop flag (a looping entry is the terminal item) */
    uint32_t queue_head, queue_len;         /* ring indices into queue[] */
    float    pos_pending[3], pos_active[3];
    float    dir_active[3];                 /* direct mode 2: room-frame unit dir listener->source,
                                             * solved with the gains (dirty-gated); feeds the per-voice
                                             * HRTF consumer via the RtDirectVoice view */
    float    gain_user;
    /* DIRECT output-channel route (CMD_SET_CHANNEL): out_ch_on = 0 is the normal panned voice, so a
     * voice-create memset leaves routing OFF — which is why the enable is its own byte rather than a
     * signed index whose zero would mean "channel 0". The route is applied where the panner's gain
     * vector is installed (compute_gains), so it ramps like any other gain change and every output
     * stage downstream still runs. NOT the bwa_set_test_signal path, which injects after align. */
    uint8_t  out_ch_on, out_ch;
    float    gtarget[BWA_CHANNELS], gcur[BWA_CHANNELS];
    float    gtarget_lo[BWA_CHANNELS], gcur_lo[BWA_CHANNELS];   /* dual-band low (amplitude-norm) band gains */
    float    xover_lp;                                        /* dual-band crossover one-pole LP state */
    float    dual_mix;                                        /* 0 = single .. 1 = dual; ramps on an A/B toggle
                                                              * so the LF re-weighting crossfades (no step) */
    int      path_on;                                         /* gated into the pathing (indirect) render */
    float    path_sh_cur[BWA_AMBI_CH];                         /* ramped path shCoeffs (toward the published target) */
    /* pathing bending-loss EQ: a 3-band tilt on the indirect signal before the SH-encode, on the
     * un-occluded s_raw. Same VoiceEq shape as the occlusion EQ below (structurally identical). */
    VoiceEq  path_eq;
    /* occlusion ramp state (audio-thread-only). The published target lives in the RtCore.occ_*
     * atomic arrays (outside this memset'd struct, so the off-thread sim never races a voice
     * create). occ_cur ramps toward the gated published value, applied to the mono signal pre-pan. */
    float    occ_cur;
    float    dir_cur;                        /* directivity ramp (source-radiation gain, pre-pan) */
    bool     refl_send;                      /* opted into the reflection aux send (CMD_SET_REFLECTIONS) */
    bool     refl_dist;                      /* scale the wet send by distance (far = wetter) */
    float    refl_gain;                      /* per-voice wet-send level (default 1) */
    float    refl_g_cur;                     /* ramped effective send gain (audio-thread-only) */
    /* per-band transmission (occlusion) EQ state: a wall muffles, not just attenuates. Same shape as
     * the pathing EQ above; both driven by eq_block_setup/apply/land. */
    VoiceEq  eq;
    /* propagation effects (audio-thread-only, opt-in). air_a_cur is the slewed one-pole coeff, air_y1
     * the filter memory. dop_delay is the current fractional delay (samples), gliding toward distance/c
     * each block - the glide IS the pitch shift; dop_w indexes this voice's slice of RtCore.dop_ring;
     * dop_init snaps the delay to distance/c on the first block after enable (no enable glitch). */
    bool     air_on, dop_on, dop_init;
    float    air_a_cur, air_y1;
    float    dop_delay, dop_dtgt;            /* read delay + its smoothed target (2-pole, per-sample) */
    uint32_t dop_w;
    /* spectral widening (spread mode 2, audio-thread-only): per-band pan gains replace the single-
     * path output stage while engaged. fs_on: 0 = off, 1 = active, 2 = retiring (every band aims at
     * the single-path target; the mixer hands back once they land). Engage seeds fs_g from gcur and
     * retire lands on gtarget, so both handoffs are exact (no crossfade machinery needed). */
    uint8_t  fs_on;
    float    fs_lp[BWA_FS_XOVERS];                            /* band-splitter one-pole states */
    float    fs_g[BWA_FS_BANDS][BWA_CHANNELS];                 /* live per-band gains (ramped) */
    float    fs_t[BWA_FS_BANDS][BWA_CHANNELS];                 /* per-band targets (compute_gains) */
    float    sp_base[3];                     /* spread ring-frame base, parallel-transported along the source
                                              * trajectory (audio-thread; zero = unset). Projecting the previous
                                              * base off the new direction keeps the ring orientation continuous —
                                              * a fixed-up frame flips ~180° in one solve at the |d.y| = 0.9
                                              * branch, teleporting the spectral bands' directions. */
    float    spread;                         /* source angular width 0..1 (0 = point); blends the pan gains */
    float    spread_h;                       /* vertical extent (rt_source_set_extent): < 0 = isotropic (follow
                                              * spread); else the height 0..1 — BS.2127-style anisotropic w/h */
    float    ext_u, ext_w;                   /* this solve's horizontal/vertical extent RATIOS (max of the two
                                              * is exactly 1; both 1 = isotropic). Audio-thread, set per solve. */
    float    size_m;                         /* source METRIC radius (m; 0 = point): floors the spread at the
                                              * angle the radius subtends from the listener, so physical size
                                              * stays constant as the listener walks */
    float    spread_eff;                     /* last solve's EFFECTIVE spread (user, floored by size + near-
                                              * listener widening) — the decor split follows it (audio-thread) */
    float    dc_amp;                         /* decorrelated-split amplitude sqrt(spread*toggle), ramped (audio-thread) */
    /* per-source distance-attenuation override (CMD_SET_ATTEN): swap the LAYOUT's curve for this
     * voice's own — the solve rescales its gains by the RATIO of the two curves at the solved
     * distance (exact through any clamping, panner-agnostic; primary-listener distance, like every
     * distance effect). att_ref <= 0 = no override; rolloff 0 = constant level at any distance. */
    float    att_ref, att_rolloff, att_min;
    /* equal-loudness distance compensation (opt-in): a one-pole LF shelf whose boost tracks the
     * distance attenuation, so an attenuated source keeps its body (ISO-226-motivated; audio-thread). */
    bool     ldc_on;
    float    ldc_g_cur;                      /* current shelf gain (linear; 1 = flat), ramped */
    float    ldc_lp;                         /* shelf one-pole state */
    /* near-field proximity boost (opt-in): the same one-pole LF shelf shape, boosting as the source
     * closes inside BWA_NF_RADIUS (the near-distance mirror of loudness comp; audio-thread). */
    bool     nf_on;
    float    nf_g_cur;                       /* current shelf gain (linear; 1 = flat), ramped */
    float    nf_lp;                          /* shelf one-pole state */
    /* MANUAL directivity (CMD_SET_DIR; no sim): forward axis + weighted-dipole pattern. weight 0 =
     * off. The block preamble evaluates |(1-w) + w*cos|^p toward the active listener and feeds the
     * SAME dir_cur ramp the sim's publish drives (the sim wins while it republishes — see mix_voice). */
    float    dir_fwd[3];
    float    dir_w, dir_pow;
    /* pause/seek gate: pause_g ramps 1 (running) <-> 0 (silent) across one block; the playhead only
     * freezes (and a pending seek only lands) once fully silent, so both are click-free (invariant 4).
     * A seek on a RUNNING voice is ramp-out -> jump -> ramp-in (two blocks, ~10 ms at 256/48k). */
    bool     paused;                         /* target state (CMD_SET_PAUSED) */
    float    pause_g;                        /* current gate value (1 = running) */
    uint8_t  stopping;                       /* CMD_STOP: fade the gate to 0 over one block, THEN playing=false
                                              * (click-free explicit stop; steal/destroy still hard-cut, see CMD_STOP) */
    uint8_t  seek_pending;
    uint64_t seek_pos;                       /* content frame to land on (in-memory sounds only) */
    /* timed fade (CMD_FADE): gain_user glides toward fade_target at fade_rate per sample, re-dirtying
     * the solve each block; on landing, optionally take the click-free stop path (audio-thread). */
    float    fade_target, fade_rate;
    uint8_t  fade_stop;
    uint8_t  group;                          /* mix-group id (0 = default); scales the solve by group_gain */
    /* pitch (CMD_SET_PITCH; in-memory sounds): fractional playback cursor. pitch is the target rate,
     * pitch_cur glides per sample (a change BENDS, never steps); cur_frac is the read position's
     * fractional part (cursor stays integer + frac — a long-lived voice never loses precision). */
    float    pitch, pitch_cur, cur_frac;
    /* bed orientation (CMD_BED_ROT): rotate a bed's soundfield. Yaw-only runs the exact per-sample
     * phasor path (yaw_cur glides toward yaw at BWA_BED_YAW_RATE); any pitch/roll engages the full
     * Ivanic-Ruedenberg matrix path (rot_full): rot_m is the LIVE packed SH rotation, rebuilt per
     * block from the glided angles and interpolated per sample toward the block target (invariant 4).
     * Settling back to pitch = roll = 0 hands off to the phasor (the two agree exactly there). */
    float    yaw, yaw_cur;
    float    bpitch, bpitch_cur, broll, broll_cur;
    uint8_t  rot_full;
    float    rot_m[BWA_SH_ROT_N];
    /* max-rE bed-decode weighting (rt_set_max_re): ramps 0<->1 so the A/B crossfades (like dual_mix). */
    float    re_mix;
    /* band-split max-rE (rt_set_max_re_split): re_sm ramps the split share 0<->1 (broadband <-> taper
     * only above the 700 Hz crossover — the rV-optimal plain decode keeps the low band); re_lp is the
     * split's per-SH-channel one-pole LP state (xover_a). Both wiped while the taper is disengaged. */
    float    re_sm;
    float    re_lp[BWA_AMBI_CH];
    /* image-source early reflections (CMD_SET_ISM). Per image: a gliding fractional read into this
     * voice's ism_ring slice, a one-pole HF damping state (walls absorb HF harder), and a ramped
     * 26-gain vector from the panner solved AT THE IMAGE POSITION (so reflections are directional
     * and walk-correct). All audio-thread-owned; ism_w is the shared ring write index. */
    bool     ism_on, ism_init, ism_tail;   /* enabled / snap the delays this block / ramping out */
    uint32_t ism_w;
    float    ism_delay[ISM_IMAGES];                  /* current fractional read delay (samples) */
    float    ism_lp[ISM_IMAGES];                     /* HF-damping one-pole state */
    float    ism_g[ISM_IMAGES][BWA_CHANNELS];         /* ramped per-image speaker gains */
} Voice;

typedef struct {
    float p_pending[3], q_pending[4];
    float p_active[3],  q_active[4];
    /* extra (compromise) listener positions — multi-listener panning. Commit-gated like the pose. */
    float   ex_pending[BWA_EXTRA_LIS][3], ex_active[BWA_EXTRA_LIS][3];
    uint8_t nex_pending, nex_active;
} Listener;

/* Per-voice parametric-bed state (bwa_set_bed_renderer), a PARALLEL array to `voices` (only bed
 * voices use it; reset by CMD_SRC_CREATE). Audio-thread-only. */
typedef struct {
    float lp[3][4];                                   /* band-splitter one-pole states x FOA channel */
    float I[BWA_PARA_BANDS][3];                        /* smoothed intensity vector (ambi axes) */
    float E[BWA_PARA_BANDS];                           /* smoothed energy */
    float g_cur[BWA_PARA_BANDS][BWA_CHANNELS];          /* direct-stream pan gains (ramped) */
    float g_tgt[BWA_PARA_BANDS][BWA_CHANNELS];
    float da_cur[BWA_PARA_BANDS], fa_cur[BWA_PARA_BANDS];   /* sqrt(1-psi)*pref / sqrt(psi), ramped */
    float mix;                                        /* matrix <-> parametric crossfade (live A/B) */
} ParaBed;

typedef struct {
    SoundData data;
    uint16_t  gen;
    uint8_t   inuse;
    uint8_t   retiring;                     /* unload requested; awaiting EVT_SOUND_RETIRED ack */
    uint8_t   pending;                      /* RESERVED for an async load: the handle is live but
                                             * `data` is still empty, so no bind may reference it
                                             * (rt_sound_reserve / rt_sound_publish) */
} SoundSlot;

/* A play issued against a still-reserved (async) sound. Held on the CONTROL thread and re-issued
 * by rt_sound_publish once the slot carries real PCM — the audio thread never sees a half-written
 * slot because it never sees the slot at all until the publish. Fixed capacity: this is a
 * mid-session convenience, not a queue anything should depend on at depth. */
#define BWA_ASYNC_HOLD 32
typedef struct {
    uint32_t src, snd;
    uint64_t start, loop_beg, loop_end;      /* the region: set at the play, or later by
                                              * rt_source_set_region while the play is still held */
    uint64_t seek;                           /* rt_source_seek issued while held; re-played after the bind */
    uint8_t  loop;
    uint8_t  bed;                           /* the KIND this play was issued as: 1 = bed (rt_bed_play),
                                             * 0 = point source. A pending slot reports 0 channels, so
                                             * the caller's kind cannot be checked against the asset
                                             * until the publish — this byte is what it is checked
                                             * against there (it fits the struct's existing padding) */
    uint8_t  seek_set;                      /* whether `seek` carries one (0 is a legal target) */
} HeldPlay;

struct RtCore {
    uint32_t voice_cap, channels, sample_rate;
    Voice*   voices;
    Listener lis;
    CmdRing  cmds;
    EvtRing  events;

    /* occlusion handoff (off-thread sim -> audio thread), parallel to `voices` but OUTSIDE the
     * Voice struct so CMD_SRC_CREATE's memset never races a concurrent sim publish. The sim stores
     * (occ_handle, occ_val); the audio thread gates on its own v->gen (which it owns) and applies. */
    _Atomic uint32_t* occ_handle;           /* handle the sim last published for (0 = none) */
    _Atomic float*    occ_val;              /* published broadband level (1 = clear) */
    _Atomic uint64_t* occ_eq;               /* published 3-band transmission tilt (3x16-bit, gated by occ_handle) */
    _Atomic float*    occ_dir;              /* published directivity gain (1 = on-axis/omni, gated by occ_handle) */
    /* MANUAL-directivity readback (audio thread -> control, the reverse of occ_dir): the mixer
     * packs (handle << 32 | float bits of the block's dipole gain) so rt_get_directivity can report
     * the manual path too — one word, handle gate and value can never tear apart. */
    _Atomic uint64_t* dir_pub;

    /* per-slot playback state for control-thread readback (rt_source_is_playing): packed
     * (gen<<9 | consumed-play-seq<<1 | playing-bit), republished by the audio thread each block. The
     * gen guards a stale or recycled handle (a mismatched gen reads as not-playing), and the SEQ is
     * what makes a RE-play on a handle whose voice already ended read as playing immediately: the
     * published word alone cannot tell "not playing, before your play" from "after it", since both
     * carry the same gen and the same 0 bit. Comparing the published seq against the control-side
     * counter makes a queued-but-unconsumed play unambiguous. */
    _Atomic uint32_t* play_pub;
    /* per-slot playhead for control-thread readback (rt_source_get_position): packed
     * (gen<<48 | frames<<0, 48-bit position), republished alongside play_pub. One word, so the gen
     * gate and the position can never tear apart. The position is the voice's CONTENT playhead —
     * cursor for in-memory/bed voices, stream_pos (frames actually consumed) for stream/push — so
     * it freezes under pause, lands where seek lands, and slips with a stream underrun. */
    _Atomic uint64_t* pos_pub;

    /* debug channel test signal (bwa_set_test_signal): audio-thread DSP state, set by CMD_TEST_SIGNAL,
     * generated + summed onto each channel AFTER align (raw channel). 0 kind = off. */
    uint8_t  test_kind[BWA_CHANNELS];
    float    test_gain[BWA_CHANNELS];
    float    test_phase[BWA_CHANNELS];   /* sine phase accumulator, radians */
    uint32_t test_noise;                /* shared LCG state for the noise kind */
    EqProto  eq_proto[3];               /* per-band biquad prototypes, rate-derived at create */

    /* per-channel output meter: each block's peak |sample| at the END of rt_render (post align/test
     * signal/limiter = exactly what the device channel received), relaxed-published for control-thread
     * readback (rt_bus_peaks -> bwa_get_bus_levels: channel meters / speaker-activity displays). */
    _Atomic float chan_peak[BWA_CHANNELS];

    /* control-thread-owned voice handle allocation */
    uint16_t* gen;                          /* current generation per voice slot */
    uint8_t*  inuse;                        /* 1 while a voice slot is allocated */
    uint8_t*  priority;                     /* per-source steal priority (control-side; 0=expendable..255=protected) */
    uint8_t*  group;                        /* per-source mix group: a control-side MIRROR of the voice's own group
                                             * (the audio thread owns that one and never reads this one). It exists
                                             * for rt_group_stop: a HELD play has no voice yet, so the audio thread's
                                             * copy is out of reach and a group stop could not tell which pending
                                             * plays it owns. Written wherever a group reaches the ring
                                             * (rt_source_set_group, rt_source_apply_cfg), and only once that push
                                             * LANDED, so the mirror can never claim a group the voice did not take. */
    uint8_t*  stealing;                     /* 1 while a slot is fading out from a steal (skip it in the next scan) */
    uint32_t* push_sound;                   /* per voice slot: the PUSH source's internal sound handle (0 = not
                                             * push-fed; control-side — the sound retires with the source handle) */
    uint32_t* retire_park;                  /* internal-sound retires whose CMD_SOUND_RETIRE hit a full command
                                             * ring: parked here and re-tried at every drain_events. The handle is
                                             * internal (the user never saw it), so nobody else can retry — dropping
                                             * it would leak the slot + a push stream's ring for the engine's life. */
    uint32_t  retire_parked;
    /* Plays issued against a sound whose async decode has not landed yet (control thread only;
     * see HeldPlay). Fixed array, linear scan: it is empty except while an async load is in
     * flight, and a source can hold at most one entry. */
    HeldPlay  held[BWA_ASYNC_HOLD];
    uint32_t  held_n;
    uint64_t  held_kind_drops;              /* held plays rt_sound_publish refused because the decoded
                                             * asset turned out to be the other KIND (control thread;
                                             * engine.c turns each new one into a bwa_last_error notice) */
    /* Voice-ended handles held for rt_poll_ended (control thread). drain_events already sees every
     * EVT_VOICE_ENDED and recycles the slot; without this they were discarded, so every client
     * rebuilt completion detection out of is_playing polling. Fixed ring, drop-OLDEST on overflow:
     * a caller who never polls must not grow it, and the newest completions are the useful ones. */
    uint32_t* ended;
    uint32_t  ended_cap, ended_head, ended_len;
    uint64_t  ended_dropped;
    _Atomic uint64_t done_dropped;   /* completions the AUDIO thread refused to post, to protect the
                                      * ownership acks. Folded into rt_poll_ended's dropped total. */
    /* Loop-wrap handles held for rt_poll_looped (control thread). Same ring, same drop-OLDEST rule,
     * separate counters: a wrap is a different event from a completion, and a caller who polls one
     * must not drain or distort the other. Sized deeper than `ended` because ONE voice can wrap many
     * times between two polls (a short region at 60 Hz polling), where it can only end once. */
    uint32_t* looped;
    uint32_t  looped_cap, looped_head, looped_len;
    uint64_t  looped_dropped;
    _Atomic uint64_t loop_dropped;   /* wraps the AUDIO thread refused to post (same reserve as DONE) */
    uint8_t*  play_seq;                     /* control-side play COUNTER (wrapping, never 0 once used), bumped when a CMD_PLAY enqueues for this
                                             * slot's current gen: rt_source_is_playing reads it until the audio
                                             * thread first PUBLISHES that gen, so a fresh play/create_stream never
                                             * reads not-playing in the one-block window before the next render. */
    uint32_t* freelist;
    uint32_t  free_count;
    uint32_t  fade_reserve;                 /* physical slots beyond the user pool, kept free so a stolen voice can
                                             * fade out on its OWN slot while the new source starts on a reserve one */

    /* control-thread-owned sound table + handle allocation */
    SoundSlot* sounds;
    uint32_t   sound_cap;
    uint32_t*  sfreelist;
    uint32_t   sfree_count;

    /* spatialization (set at create/load time; read by the audio thread) */
    Layout    layout;
    Aligner*  aligner;
    /* tracked room EQ (layouts with a room_eq_grid): per block the audio thread interpolates the
     * grid's section-cut depths at the live listener position (inverse-distance weights) and hands
     * them to the aligner to slew toward (align_room_eq_targets). rq_state remembers what was last
     * sent: 0 = nothing yet, 1 = targets for rq_lis, 2 = flat (the live toggle is off). */
    _Atomic int room_eq_dyn;     /* tracked room EQ enable (default ON when a grid is present); live A/B */
    float     rq_lis[3];
    int       rq_state;
    /* tracked listener alignment (rt_set_tracked_align; OFF by default): the same shape one layer down
     * in the output stage — per block the audio thread re-derives the aligner's per-speaker delay/gain
     * from the LIVE listener instead of Layout.ref (listener_align_track) and align_process slews
     * toward it. lc_state mirrors rq_state: 0 = nothing sent, 1 = targets for lc_lis, 2 = identity. */
    _Atomic int   lc_on;
    _Atomic float lc_dead_m;     /* dead zone (m; <= 0 = LC_DEAD_ZONE_M) */
    _Atomic float lc_slew;       /* rate limit (frames/s; <= 0 = derived from LC_SLEW_SPEED_MS) */
    float     lc_lis[3];
    int       lc_state;
    _Atomic int panner;      /* 0 = DBAP (moving observer); 1 = SPCAP; 2 = VBAP (both fixed observer); atomic for A/B */
    _Atomic int dual_band;   /* 0 = single (power) panning; 1 = dual-band (amplitude LF / power HF); atomic for A/B */
    _Atomic int cap_on;      /* compensated amplitude panning on the LF band (cap.c). INERT unless dual_band
                              * is on — the mixer only reads gtarget_lo there. Atomic for A/B. */
    _Atomic int spread_mode; /* 0 = lobe reshape (spread_gains); 1 = MDAP virtual-source ring; atomic for A/B */
    _Atomic int decor_on;    /* decorrelate spread sources' wide part (velvet-noise path); atomic for A/B */

    /* decorrelation path: voices (the wide part of spread sources) and the parametric bed's diffuse
     * stream accumulate into dc_bus; rt_render then convolves each channel through its own sparse
     * velvet-noise filter into the main bus. Incoherent speaker feeds stop a wide source collapsing
     * to phantom images / comb-filtering as the tracked listener walks. All fixed-size, built at
     * create (audio thread only touches it). dc_tail keeps the convolution running one filter-length
     * past the last write, then the history is wiped so a re-engage never replays stale samples. */
    float*   dc_bus;                     /* BWA_CHANNELS * BWA_RT_MAX_BLOCK accumulation scratch */
    float*   dc_hist;                    /* BWA_CHANNELS * dc_histlen input-history rings */
    uint32_t dc_histlen, dc_hmask, dc_w; /* shared ring geometry/write index (like the aligner's) */
    uint32_t dc_ntaps;
    uint16_t dc_off[BWA_CHANNELS][BWA_DECOR_TAPS];  /* per-channel tap offsets (samples back) */
    float    dc_tamp[BWA_CHANNELS][BWA_DECOR_TAPS]; /* per-channel tap amplitudes (signed, unit energy) */
    uint32_t dc_tail;                    /* samples of flush left after the last write */
    int      dc_wrote;                   /* set by the mixers when anything landed in dc_bus this block */
    int      dc_on_blk, bed_param_blk;   /* this block's decor_on/bed_param, loaded ONCE in rt_render so
                                          * the zero/convolve gate and the mixers can never see a toggle
                                          * land mid-block and drop a block of the incoherent share */

    /* master gain: one ramped scalar over the whole mix, applied pre-align (per-speaker trims and the
     * raw channel-test signal stay calibrated; the limiter still guards the sum). */
    _Atomic float master_gain;           /* target (control thread; default 1) */
    float    master_g_cur;               /* per-block ramp state (audio-thread) */
    /* global + per-group pause gates: ride pause_gate's existing ramp/freeze machinery. all_paused is
     * an atomic loaded once per block; the group arrays are audio-thread-owned (set via commands). */
    _Atomic int all_paused;
    int      all_paused_blk;
    float    group_gain[BWA_GROUPS];      /* mix-group gain multipliers (default 1; CMD_GROUP_GAIN) */
    uint8_t  group_paused[BWA_GROUPS];    /* mix-group pause gates (CMD_GROUP_PAUSED) */
    _Atomic uint32_t active_pub;         /* last block's active voice count (rt_active_voices readback) */

    /* output protection limiter (final stage, after align + test signal — everything passes through).
     * LINKED across channels: one gain from the block's cross-channel peak, so engaging never shifts
     * the spatial image. ~1 ms attack / ~120 ms release one-poles + a hard safety clamp at the ceiling
     * (the attack is not lookahead, so brief overshoot clips — deterministic protection, not mastering). */
    _Atomic int   lim_on;         /* default 1 */
    _Atomic float lim_ceiling;    /* linear (default -1 dBFS) */
    float lim_gain;               /* audio-thread envelope: current applied gain (starts at 1) */
    float lim_att_a, lim_rel_a;   /* one-pole coefficients, rate-derived at create */
    float       xover_a;     /* one-pole LP coeff for the dual-band crossover (BWA_DUALBAND_FC), rate-derived */
    uint64_t    dsp_block;   /* audio-thread: next block's dsp-sample (fallback clock when no device timestamp) */
    _Atomic uint64_t dsp_now;/* published block-start dsp-sample; control thread reads via rt_dsp_time for scheduling */
    /* Streamed voices that came up short without having ended: the disk (or push) thread did not
     * refill the ring in time and the block's tail rendered silence. A DIFFERENT starvation from a
     * device dropout — the device kept its deadline, we had nothing to give it — so it is counted
     * separately and reported as its own field (bwa_health.stream_starves). */
    _Atomic uint64_t strm_starves;
    /* device clock pair for rt_get_clock: the sink's (sample_pos, systemTime) stamp for the last
     * rendered block, published seqlock-style (odd seq = write in progress) so the control thread
     * always reads a CONSISTENT pair — the driver's own statement of "this output sample
     * corresponds to this host time", the jitter-free wall->dsp bridge AV sync needs. Only blocks
     * carrying a valid host stamp publish (ts NULL / time 0 keeps the last good pair). */
    _Atomic uint32_t clk_seq;
    _Atomic uint64_t clk_sample, clk_time;
    /* Long-horizon drift fit. The pair above is an EXACT instant, but extrapolating it at the
     * NOMINAL rate drifts: the device crystal and the host clock are different oscillators, and
     * 10 ppm is 36 ms per hour — the whole long-show AV-sync problem. So fit the slope too, by
     * exponentially weighted least squares over the same per-block stamps (rate against the host
     * clock, plus its own standard error). Audio-thread-owned accumulators updated with the stable
     * weighted-Welford form (~30 flops a block, no transcendentals); the derived model publishes
     * inside the SAME seqlock window as the pair, bit-cast through uint64 because this core keeps
     * its published fields in _Atomic slots. */
    double   fit_x0, fit_y0;              /* fit origin (host seconds, sample position) */
    double   fit_w, fit_mx, fit_my;       /* weight sum + weighted means of (x-x0, y-y0) */
    double   fit_cxx, fit_cxy;            /* weighted central moments */
    double   fit_b;                        /* last fitted slope (samples per host second) */
    double   fit_sse;                      /* weighted sum of squared prediction errors (see below) */
    double   fit_span;                    /* host seconds since the fit was seeded */
    double   fit_lam;                     /* per-block forgetting factor (derived for fit_nframes) */
    uint32_t fit_nframes;                 /* block size fit_lam was derived for (0 = not yet) */
    uint64_t fit_prev_s, fit_prev_t;      /* previous stamp, for discontinuity detection */
    _Atomic uint64_t fit_ppm, fit_sigma, fit_rate, fit_spanp, fit_jit;  /* published, bit-cast doubles */
    _Atomic uint32_t fit_stamps;          /* published effective stamp count; 0 = no usable fit */
    uint32_t   layout_gen;   /* bumped on rt_set_layout; the SPCAP/VBAP caches compare it to self-invalidate */
    SpcapState spcap;        /* SPCAP cache (audio-thread-owned; rebuilt on listener/layout change) */
    VbapState  vbap;         /* VBAP cache (same) */
    CapState   cap;          /* CAP interaural cache (same, plus head ORIENTATION — the one piece of
                              * DSP state a pure head turn invalidates; see CMD_COMMIT) */
    HoleState  hole;         /* hole-aware spread-floor cache (same self-invalidation as SPCAP's) */
    SpcapState spcap_x[BWA_EXTRA_LIS];   /* per-extra-listener caches (compromise panning): each cache is
                                         * keyed to ONE listener, so each extra gets its own */
    VbapState  vbap_x[BWA_EXTRA_LIS];
    /* SPCAP tuning overrides (rt_set_spcap_focus; both dimensionless). <= 0 means "use the layout's
     * default" — the geometry-derived focus, the constant density — and the fallback is resolved at
     * SOLVE time, not latched at set time, so a later rt_set_layout can't strand a stale value.
     * Resolved ONCE per block into spcap_focus_blk/_density_blk so no block mixes two values. */
    _Atomic float spcap_focus, spcap_density;
    float      spcap_focus_blk, spcap_density_blk;
    /* Panner-parameter generation. A live SPCAP knob changes the gain vector of EVERY source,
     * static ones included, so a dirty-only gate would leave a still scene deaf to the knob. The
     * two dirty-all sites (rt_set_layout, rt_set_direct_ambi) write v->dirty from the CONTROL
     * thread and are legal only because the audio thread is stopped there; a LIVE setter must not
     * (invariant 3: the audio thread owns the voice table). So the setter's only write is this
     * counter, and the mixer re-solves any voice whose stored pan_gen lags — on the audio thread.
     * Release/acquire, not relaxed: a voice that saw the new generation with the old focus would
     * stamp itself current and swallow the change until the next bump. */
    _Atomic uint32_t pan_gen;
    _Atomic float near_spread;  /* near-listener widening radius (m); 0 = off. An approaching source's
                                 * spread is floored by 1 - dist/radius (it subtends a growing angle
                                 * instead of collapsing into the nearest speaker). */
    /* hole-aware spread floor (rt_set_hole_spread; dimensionless scale, 0 = off): a source aimed
     * where the array has NO speaker is floored wide instead of rendered as a split image (hole.h).
     * Resolved once per block into hole_spread_blk so no block mixes two values, and the setter
     * bumps pan_gen because it moves the gains of sources that never move. */
    _Atomic float hole_spread;
    float      hole_spread_blk;
    float      ldc_a;        /* loudness-comp shelf one-pole coeff (~250 Hz), rate-derived at create */
    float      nf_a;         /* near-field proximity shelf one-pole coeff (BWA_NF_FC), rate-derived at create */
    _Atomic float sos;       /* engine-wide speed of sound (m/s; default BWA_SPEED_OF_SOUND) — live
                              * (rt_set_speed_of_sound); Doppler + ISM delays derive from it per block */
    int        bed_decoder;  /* 1 = AllRAD (robust on irregular arrays); 2 = EPAD (energy-preserving,
                              * epad.c); 0 = the sampling decode (SAD) — internal-only: the engine
                              * never selects it, it is the fallback when a build fails (and what a
                              * bare rt core without rt_set_bed_decoder uses, which the rt tests do) */
    /* ambisonic bed decode: [speaker][ACN] = (2l+1)*Y_k^SN3D(speaker_dir)/L (sampling decode, SN3D),
     * rebuilt from the layout whenever it changes. A bed voice decodes its SH channels through this. */
    float    bed_decode[BWA_CHANNELS][BWA_AMBI_CH];
    /* parametric bed renderer (bwa_set_bed_renderer): per-band FOA direction+diffuseness analysis;
     * the non-diffuse stream re-pans through the LISTENER-RELATIVE panner (a walkable bed), the
     * diffuse stream decodes through bed_decode into the decorrelators. bed_radius anchors the
     * direct stream's virtual sources on the array shell around ref; bed_pref matches its loudness
     * to the matrix decode's plane-wave rendering (both derived in build_bed_decode). */
    _Atomic int bed_param;   /* 0 = matrix decode (default); 1 = parametric; atomic for A/B */
    /* max-rE decode weighting (rt_set_max_re, live A/B): per-content-order SH channel tapers
     * (gamma * P_l(r), diffuse-energy-normalized — ambisonics.h), applied wherever bed_decode
     * matrix-decodes a bed's signal (decode(w*sh) == the max-rE decode). The parametric ANALYSIS and
     * its re-panned direct stream see the raw field (max-rE is a decode-side taper, not a field
     * property). The FDN carries its own weighted render pair (fdn.c); phonon's decodes are its own. */
    _Atomic int max_re;
    _Atomic int max_re_split;/* band-split max-rE (rt_set_max_re_split): taper only above ~700 Hz (the
                              * dual-band crossover), the rV-optimal unweighted decode below — the
                              * literature-standard Gerzon split. Only meaningful with max_re on; the
                              * FDN stays broadband (a diffuse tail has no LF image to sharpen). */
    float     re_w[3][BWA_AMBI_CH];   /* [content order - 1][ACN] */
    ParaBed*  para;          /* voice_cap entries */
    float     para_xa[3];    /* band-splitter one-pole coeffs (BWA_PARA_XOVER at the engine rate) */
    float     fs_xa[BWA_FS_XOVERS];   /* spectral-widening splitter one-poles (BWA_FS_XOVER) */
    /* spectral-widening band-overlap correlations W[a][b] = white-noise E[b_a * b_b] (the digital
     * splitter's responses, integrated at rt_create; rows sum to the band's share, sum(W) == 1).
     * The one-pole bands overlap heavily, so panning them APART loses the correlated cross terms;
     * fs_solve rescales the whole band set by 1/sqrt(sum W_ab * (g_a.g_b)/P^2) — white-noise-exact
     * constant power, content-typical otherwise (all-parallel bands -> exactly 1). */
    float     fs_w[BWA_FS_BANDS][BWA_FS_BANDS];
    float     bed_radius;    /* mean speaker distance from ref (virtual-source shell) */
    float     bed_pref;      /* plane-wave loudness reference: sqrt(mean rendered power of the FOA matrix decode) */

    /* internal tracker (bwa_tracker_connect): the audio thread samples this each block, overriding
     * the committed listener. ATOMIC — the control thread connects/disconnects at runtime (the
     * pointer swaps between blocks; the caller delays freeing the old slot's owner until any
     * in-flight block is done). NULL = no internal tracker. */
    _Atomic(const PoseSlot*) tracker;
    /* pose prediction (rt_set_pose_prediction): extrapolate the tracked position by a fixed lead
     * along a velocity estimated from the tracker's OWN timestamps (pose.h t_ns — one clock, only
     * ever differenced), hiding the tracker->ears latency. Audio-thread state. */
    _Atomic float pred_lead;     /* seconds of lead; 0 = off (default) */
    float    pp_p[3], pp_vel[3]; /* last distinct raw pose + smoothed velocity (m/s) */
    uint64_t pp_tns;
    int      pp_valid;
    float    pp_quiet;           /* seconds since the last NEW tracker frame (stall detection) */

    /* readback of the active listener pose, published by the audio thread each block so the
     * control thread can sample it race-free (bwa_get_listener_pose — visuals/logging). */
    PoseSlot readback;

    /* post-mix aux-send tap (the reflection bed): a phonon-free hook the audio thread calls after the
     * voice loop. `aux` is the summed mono send (opted-in voices). The tap pointer is published with
     * release AFTER its user-data (bus_tap_ud), and the audio thread acquire-loads it — so it is
     * registered SAFELY even though bwa_start opens the sink (starting the callback) before it registers
     * the tap: a render seeing a non-NULL tap always sees a consistent ud. */
    _Atomic RtBusTap bus_tap;
    void*    bus_tap_ud;
    float*   aux;                /* BWA_RT_MAX_BLOCK mono samples; the per-block aux send scratch */
    StreamSet* streams;          /* background file-streaming thread + ring pool (control thread owns lifecycle) */
    float*   stream_scratch;     /* BWA_RT_MAX_BLOCK mono samples; a streaming voice's block, pulled before the mix */

    /* pathing: rt_render SH-encodes pathing voices into path_accum, then path_tap decodes it to the bus.
     * Per voice the sim publishes shCoeffs via a handle-gated double buffer (path_pub[idx*2 + path_idx]). */
    _Atomic RtPathTap path_tap;  /* published release-after-ud/ambi_ch; acquire-loaded (see bus_tap) */
    void*    path_tap_ud;
    uint32_t path_ambi_ch;       /* (order+1)^2 of the pathing field; 0 = no path tap */
    float*   path_accum;         /* BWA_AMBI_CH * BWA_RT_MAX_BLOCK; summed ambisonic indirect field */
    PathPub* path_pub;           /* voice_cap * 2 (double-buffered per voice) */
    _Atomic int* path_idx;       /* voice_cap: front-buffer index the sim flips after writing the back */

    /* image-source early reflections (ism.c): the room + a live gain, plus one delay ring per voice
     * (the reflected copies are the direct signal, delayed by their longer paths). The room, the
     * gain, and the per-voice enables are all LIVE: the control thread publishes the room through
     * a single-slot seqlock (pose.h's protocol — the struct is too wide for one atomic store) and
     * rt_render adopts a stable copy at block start, so a mid-scene bwa_scene_set_box/_set_ground/
     * _set_pressure_release never tears under the mixer; the images then re-solve next block
     * (gains ramp, delays glide — the room change bends the reflections, no click). */
    IsmRoom  ism_room;           /* audio-thread block copy (the mixer reads only this) */
    IsmRoom  ism_room_sh;        /* seqlock-shared slot (rt_set_ism_room writes) */
    _Atomic uint32_t ism_seq;    /* even = stable, odd = write in progress */
    uint32_t ism_seen;           /* audio-thread-only: last adopted seq */
    _Atomic float ism_gain;
    float*   ism_ring;           /* voice_cap contiguous power-of-two rings of ism_ringlen floats */
    uint32_t ism_ringlen;

    /* per-voice Doppler delay rings: voice_cap contiguous power-of-two rings of dop_ringlen floats each
     * (slice idx = dop_ring + idx*dop_ringlen), sized to BWA_DOPPLER_MAX_DIST at the engine rate.
     * Allocated once at create (control thread); the audio thread writes/reads its voice's slice. */
    float*   dop_ring;
    uint32_t dop_ringlen;

    /* direct-binaural render (BWA_PROFILE_BINAURAL): point voices SH-encode at their TRUE
     * listener-relative direction into this ambisonic accumulator (phonon monitor basis,
     * ambi_encode_phonon) instead of panning to the speaker bus — the gain-ramp machinery ramps
     * SH coefficients, and the monitor decodes bus + accumulator in one pass. Beds pass SH->SH
     * (one diagonal, ambi_canon_to_phonon) and the pathing accumulator sums in raw (same basis);
     * the FDN / reflection-bed taps still render to the speaker bus (synthesized-diffuse content
     * rides the virtual-speaker encode). Mode 2 additionally exposes each point voice's post-DSP
     * mono block + direction (dv_*) so a phonon consumer can run one IPLBinauralEffect per voice —
     * spread power-splits between that point tap (sqrt(1-s)) and the SH field (sqrt(s)), so both
     * paths always exist and a spread change never switches paths. Set while stopped
     * (rt_set_direct_ambi); never toggled while the audio thread runs. */
    int      direct_on;          /* 0 = off, 1 = SH field, 2 = SH field + per-voice point taps */
    int      direct_blk;         /* the block's effective gate (rt_render; buffers verified) */
    uint32_t mix_nch;            /* point-voice gain width: BWA_AMBI_CH when direct, else channels */
    float*   ambi_direct;        /* BWA_AMBI_CH * BWA_RT_MAX_BLOCK; zeroed + accumulated per block */
    float*   dv_mono;            /* mode 2: voice_cap slots x BWA_RT_MAX_BLOCK (each voice's point share) */
    RtDirectVoice* dv_view;      /* mode 2: per-slot view published each block (rt_direct_voices) */
};

/* ---- ring primitives ---- */

static bool cmd_push(CmdRing* r, const Cmd* c) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= RING_CAP) return false;                       /* full: should never happen */
    r->slots[w & (RING_CAP - 1)] = *c;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}

/* Free slots, from the producer's view. The audio thread only advances `read` (frees
 * space), so once the single producer sees >= k free it can push k commands atomically. */
static uint32_t cmd_free(const CmdRing* r) {
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    return RING_CAP - (w - rd);
}

/* Push a BEST-EFFORT notice. EVT_VOICE_DONE and EVT_VOICE_LOOPED are unlike the ownership events: a
 * plain voice re-arms with just a CMD_PLAY (and a loop wrap needs no control-side action at all), so
 * they need no drain in between and can be emitted without bound — a short loop region wraps several
 * times in ONE block. That breaks the "voice_cap + sound_cap <= EVT_CAP means un-overflowable"
 * reasoning the ring is sized on, and a full ring would then drop the ownership-carrying events
 * instead: a missed EVT_VOICE_ENDED leaks a voice slot, a missed EVT_SOUND_RETIRED never frees the
 * buffer. So the notices yield. Each is pushed only while that reserve remains free, and a refusal is
 * COUNTED (in its OWN counter, so the two polls report their own losses) rather than silently lost. */
static void evt_push_notice(RtCore* c, const Evt* e, _Atomic uint64_t* drops) {   /* audio thread */
    EvtRing* r = &c->events;
    const uint32_t reserve = c->voice_cap + c->sound_cap;     /* headroom the critical events own */
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= EVT_CAP - reserve) {
        atomic_fetch_add_explicit(drops, 1u, memory_order_relaxed);
        return;
    }
    r->slots[w & (EVT_CAP - 1)] = *e;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
}

static bool evt_push(EvtRing* r, const Evt* e) {              /* audio thread */
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_acquire);
    if (w - rd >= EVT_CAP) return false;
    r->slots[w & (EVT_CAP - 1)] = *e;
    atomic_store_explicit(&r->write, w + 1, memory_order_release);
    return true;
}

/* ---- control thread: handle allocation ---- */

static uint32_t alloc_handle(RtCore* c) {
    if (c->free_count == 0) return 0;                          /* table full */
    uint32_t idx = c->freelist[--c->free_count];
    uint16_t g = (uint16_t)(c->gen[idx] + 1);
    if (g == 0) g = 1;                                        /* skip 0 (invalid handle) */
    c->gen[idx] = g;
    c->inuse[idx] = 1;
    c->priority[idx] = 128;                                   /* defined default for EVERY alloc (sources AND oneshots),
                                                              * so a recycled slot never leaks its prior priority */
    c->group[idx] = 0;                                        /* group 0, matching the memset CMD_SRC_CREATE gives the
                                                               * voice, so a recycled slot never inherits the previous
                                                               * occupant's group and a group stop cannot sweep a held
                                                               * play the new owner never put there */
    c->stealing[idx] = 0;                                     /* a fresh slot is not mid-steal (clears a leaked flag
                                                              * if the app destroyed a voice while it faded) */
    c->push_sound[idx] = 0;                                   /* a fresh slot is not push-fed */
    c->play_seq[idx] = 0;                                     /* no play enqueued for the new gen yet */
    return BWA_MK_H(idx, g);
}

/* Control-thread liveness: is h the CURRENT occupant of its voice slot? This is the invariant-5
 * gen guard — every control-side act on a voice handle goes through it, so a stale handle (slot
 * recycled, then re-allocated at a higher gen) can never touch the slot's next occupant. */
static bool voice_live_ctrl(const RtCore* c, uint32_t h) {
    uint16_t idx = BWA_H_IDX(h);
    return idx < c->voice_cap && c->inuse[idx] && c->gen[idx] == BWA_H_GEN(h);
}

static void recycle_handle(RtCore* c, uint32_t h) {
    /* Gen-checked + idempotent: return the slot to the free-list only if THIS handle is still its
     * current occupant, so a double-destroy OR a late EVT_VOICE_ENDED for a since-stolen slot can
     * never free a live source's slot (invariant 5). The inuse check catches a double-free before
     * reuse; the gen check catches one after reuse. */
    if (voice_live_ctrl(c, h)) {
        c->inuse[BWA_H_IDX(h)] = 0;
        c->freelist[c->free_count++] = BWA_H_IDX(h);
    }
}

/* sound-handle allocation (mirrors the voice allocator) */
static uint32_t salloc_sound(RtCore* c) {
    if (c->sfree_count == 0) return 0;
    uint32_t idx = c->sfreelist[--c->sfree_count];
    uint16_t g = (uint16_t)(c->sounds[idx].gen + 1);
    if (g == 0) g = 1;
    c->sounds[idx].gen = g;
    c->sounds[idx].inuse = 1;
    c->sounds[idx].retiring = 0;
    c->sounds[idx].pending  = 0;
    return BWA_MK_H(idx, g);
}

static void srecycle_sound(RtCore* c, uint16_t idx) {
    if (idx < c->sound_cap && c->sounds[idx].inuse) {
        c->sounds[idx].inuse = 0;
        c->sounds[idx].retiring = 0;
        c->sounds[idx].pending  = 0;   /* a reserved slot that was abandoned must not come back pending */
        c->sfreelist[c->sfree_count++] = idx;
    }
}

/* ---- held plays (control thread; see HeldPlay). Swap-remove, so order is not preserved: nothing
 * depends on it, a source can only hold one entry and each entry fires at most once. ---- */
static void held_drop_src(RtCore* c, uint32_t src) {
    for (uint32_t i = 0; i < c->held_n; )
        if (c->held[i].src == src) c->held[i] = c->held[--c->held_n];
        else ++i;
}
static void held_drop_snd(RtCore* c, uint32_t snd) {
    for (uint32_t i = 0; i < c->held_n; )
        if (c->held[i].snd == snd) c->held[i] = c->held[--c->held_n];
        else ++i;
}
/* Every held play whose source sits in this mix group, read off the control-side mirror
 * (RtCore.group) because a held play has no voice to read a group from. An entry whose source is
 * no longer its slot's occupant is LEFT ALONE: it belongs to nobody, so matching it against the
 * slot's next occupant's group would be a coincidence, and rt_sound_publish drops it on the
 * liveness check anyway. */
static void held_drop_group(RtCore* c, uint8_t group) {
    for (uint32_t i = 0; i < c->held_n; ) {
        const uint32_t src = c->held[i].src;
        if (voice_live_ctrl(c, src) && c->group[BWA_H_IDX(src)] == group)
            c->held[i] = c->held[--c->held_n];
        else ++i;
    }
}
static void held_add(RtCore* c, uint32_t src, uint32_t snd, bool loop,
                     uint64_t start, uint64_t loop_beg, uint64_t loop_end, bool bed) {
    if (c->held_n >= BWA_ASYNC_HOLD) return;   /* full: the play is simply dropped, like a full command ring */
    HeldPlay* p = &c->held[c->held_n++];
    p->src = src; p->snd = snd; p->loop = loop ? 1u : 0u;
    p->start = start; p->loop_beg = loop_beg; p->loop_end = loop_end;
    p->bed = bed ? 1u : 0u;                    /* re-checked against the real channel count at publish */
    p->seek = 0; p->seek_set = 0;              /* a new play supersedes any seek held for the old one */
}
/* The entry a source is holding, or NULL. At most one: every play calls held_drop_src first. */
static HeldPlay* held_find_src(RtCore* c, uint32_t src) {
    for (uint32_t i = 0; i < c->held_n; ++i) if (c->held[i].src == src) return &c->held[i];
    return NULL;
}

/* control-thread resolve: the slot iff the handle is its current occupant */
static SoundSlot* sound_slot_ctrl(RtCore* c, uint32_t h) {
    uint16_t i = BWA_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BWA_H_GEN(h)) ? s : NULL;
}

static void set_err(char* err, size_t cap, const char* msg) { if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; } }

/* Retire an INTERNAL sound (a handle the user never saw, so nobody else can retry it): if the
 * command ring is full right now the retire is parked and re-tried at every drain_events —
 * rt_unload_sound's revert-and-retry contract needs SOMEONE holding the handle, and here that
 * someone has to be us. */
static void retire_internal_sound(RtCore* c, uint32_t snd) {
    if (!rt_unload_sound(c, snd) && c->retire_parked < c->sound_cap)   /* bound is structural: each
                                             * parked handle pins a distinct sound slot */
        c->retire_park[c->retire_parked++] = snd;
}

/* A push source's handle is dying (destroy, or a steal completed): retire its internal sound
 * so the ring closes once the audio thread has let go. Gen-guarded, so a stale handle can never
 * touch the slot's next occupant. Call BEFORE recycle_handle (recycle clears inuse). */
static void push_sound_release(RtCore* c, uint32_t h) {
    uint16_t idx = BWA_H_IDX(h);
    if (voice_live_ctrl(c, h) && c->push_sound[idx]) {
        retire_internal_sound(c, c->push_sound[idx]);
        c->push_sound[idx] = 0;
    }
}

/* Append an ended handle to the poll ring (control thread). Drop-OLDEST when full, counting the loss,
 * so a caller who never polls cannot grow it and the newest completions are the ones kept. */
static void ended_note(RtCore* c, uint32_t h) {
    if (!c->ended_cap) return;
    if (c->ended_len == c->ended_cap) {
        c->ended_head = (c->ended_head + 1u) % c->ended_cap;
        --c->ended_len; ++c->ended_dropped;
    }
    c->ended[(c->ended_head + c->ended_len) % c->ended_cap] = h;
    ++c->ended_len;
}

/* The loop-wrap twin of ended_note (control thread), on its own ring and its own drop counter. */
static void looped_note(RtCore* c, uint32_t h) {
    if (!c->looped_cap) return;
    if (c->looped_len == c->looped_cap) {
        c->looped_head = (c->looped_head + 1u) % c->looped_cap;
        --c->looped_len; ++c->looped_dropped;
    }
    c->looped[(c->looped_head + c->looped_len) % c->looped_cap] = h;
    ++c->looped_len;
}

static void drain_events(RtCore* c) {                          /* control thread */
    for (uint32_t i = 0; i < c->retire_parked; )               /* re-try parked internal retires (the
                                                                * command ring was full when they died) */
        if (rt_unload_sound(c, c->retire_park[i])) c->retire_park[i] = c->retire_park[--c->retire_parked];
        else ++i;
    EvtRing* r = &c->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_DONE:                                          /* pure notice: recycle nothing */
            /* Report only if this completion belongs to the LATEST play of that slot. Between the
             * audio thread posting it and the control thread draining, the caller may already have
             * re-played the same handle; announcing the old play's end while the new one is audible
             * is how a client frees or re-triggers the wrong thing. A superseded completion is
             * dropped, not counted: nothing was lost, the caller moved on. */
            {   /* The generation must match too. Seq alone is per-SLOT, so a completion left
                 * undrained while the caller destroyed the source and created a new one on the same
                 * slot would match the new occupant's counter (both plays are seq 1) and report a
                 * handle the caller has already freed. Seq 0 means "no play was ever enqueued at
                 * this generation", which no real completion can carry. */
                const uint16_t i2 = BWA_H_IDX(ev->handle);
                if (ev->seq != 0 && c->inuse[i2] && c->gen[i2] == BWA_H_GEN(ev->handle)
                    && c->play_seq[i2] == ev->seq) ended_note(c, ev->handle);
            }
            break;
        case EVT_VOICE_LOOPED:
            /* Same gate as DONE, for the same reasons: a wrap belongs to ONE play of ONE generation,
             * and announcing a superseded play's wrap is how a client paces a trial off the wrong
             * stimulus. Seq 0 (a oneshot) can never reach here — the mixer does not post for one. */
            {   const uint16_t i3 = BWA_H_IDX(ev->handle);
                if (ev->seq != 0 && c->inuse[i3] && c->gen[i3] == BWA_H_GEN(ev->handle)
                    && c->play_seq[i3] == ev->seq) looped_note(c, ev->handle);
            }
            break;
        case EVT_VOICE_ENDED:    push_sound_release(c, ev->handle);   /* a stolen push source dies here */
                                 /* NOT reported here. A naturally ending ONESHOT posts DONE as well,
                                  * and reporting both surfaced it twice — with a handle the caller
                                  * never held, since bwa_play_oneshot returns no handle. A STEAL
                                  * posts only ENDED, and a stolen voice is not a completion either:
                                  * the engine took the slot, the sound did not finish. */
                                 /* Record BEFORE recycle_handle bumps the generation, so the handle
                                  * handed to rt_poll_ended is the one the caller still holds. */
                                 recycle_handle(c, ev->handle); c->stealing[BWA_H_IDX(ev->handle)] = 0; break;
        case EVT_SOUND_RETIRED: {        /* audio dropped all refs: free pcm / close the stream + recycle the slot */
            SoundSlot* s = sound_slot_ctrl(c, ev->handle);
            if (s) {
                if (s->data.stream) { stream_close(c->streams, s->data.stream); s->data.stream = NULL; }
                sound_unload(&s->data);
                srecycle_sound(c, BWA_H_IDX(ev->handle));
            }
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* ---- audio thread: drain + mix ---- */

static Voice* voice_for(RtCore* c, uint32_t h) {
    uint16_t i = BWA_H_IDX(h);
    if (i >= c->voice_cap) return NULL;
    Voice* v = &c->voices[i];
    return (v->active && v->gen == BWA_H_GEN(h)) ? v : NULL;    /* stale gen => dropped */
}

/* Post one LOOP WRAP notice (audio thread). Per WRAP, not per block: a short region can wrap several
 * times inside one block and the caller pacing trials off it needs every one. Best-effort through the
 * notice reserve (evt_push_notice) so it can never displace an ownership ack. A oneshot is skipped for
 * the same reason it is skipped for DONE: its handle is engine-internal, and its seq 0 would sail
 * through the drain's seq gate. */
static void loop_note(RtCore* c, const Voice* v, uint16_t idx) {
    if (v->oneshot) return;
    Evt ev = { .type = EVT_VOICE_LOOPED, .seq = v->play_seq, .handle = BWA_MK_H(idx, v->gen) };
    evt_push_notice(c, &ev, &c->loop_dropped);
}

/* audio-thread resolve: handle -> published SoundData (NULL if stale/retired) */
static const SoundData* sound_for(RtCore* c, uint32_t h) {
    uint16_t i = BWA_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BWA_H_GEN(h)) ? &s->data : NULL;
}

/* ---- per-band transmission EQ (matches Steam Audio's default direct-effect EQ) ----
 * Three RBJ biquads in series — low-shelf @800, peaking @~2530, high-shelf @8000 — applied to the
 * mono voice signal. The band *gains* are the spectral tilt of occluded sound; the broadband level
 * rides the existing occ_cur scalar. fc's are fixed, so the trig (cos w0 / alpha) is precomputed
 * per sample-rate at rt_create; only A=sqrt(g) varies per update => no transcendentals per sample. */
enum { EQ_LOWSHELF = BWA_BIQUAD_LOWSHELF, EQ_PEAK = BWA_BIQUAD_PEAK, EQ_HIGHSHELF = BWA_BIQUAD_HIGHSHELF };
#define EQ_SLEW     0.5f       /* per-block band-gain glide toward the published target */
#define EQ_FLAT     0xFFFFFFFFFFFFull /* eq_pack({1,1,1}): the flat (passthrough) tilt, as a constant */
#define EQ_FLAT_EPS 0.001f     /* band gains within +/-0.1% (~0.009 dB) of unity count as flat -> EQ bypassed
                                * (a deliberately inaudible threshold that keeps un-occluded voices off the
                                * biquad path; the per-band attenuation floor lives in steam_scene.c). */

/* pack 3 band gains (each clamped to [0,1]) into one u64 = 3x16-bit, for a tear-free atomic publish.
 * The clamp is the NaN-CLEANSING form: the bands arrive from the sim thread (whatever the ray
 * tracer computed), and a NaN through `< 0` would reach a float->uint16 conversion, which is UB
 * out of range. NULL reads as flat (occlusion clear). */
static inline uint64_t eq_pack(const float g[3]) {
    if (!g) return 0xFFFFFFFFFFFFull;                  /* 3x 0xFFFF = all-clear */
    uint64_t p = 0;
    for (int i = 0; i < 3; ++i) {
        float v = !(g[i] > 0.f) ? 0.f : (g[i] > 1.f ? 1.f : g[i]);
        p |= (uint64_t)(uint16_t)(v * 65535.f + 0.5f) << (16 * i);
    }
    return p;
}
static inline void eq_unpack(uint64_t p, float g[3]) {
    for (int i = 0; i < 3; ++i) g[i] = (float)((p >> (16 * i)) & 0xFFFFu) * (1.f / 65535.f);
}

/* g is the linear band gain (A = sqrt(g)); cw0/alpha are precomputed per filter (see eq_proto).
 * Floored well above 0: band 0 is legal at the ABI ([0,1]), the EQ_SLEW glide halves toward it and
 * (under the FTZ mode rt_render sets) REACHES exact 0, and A = sqrt(0) turns the RBJ peak design
 * into Inf*0 = NaN — coefficients that poison every IIR downstream (FDN feedback, align room EQ).
 * eq_pack's [0,1] clamp cannot catch this: 0 is in range. -60 dB per band is inaudibly far below
 * the broadband cut, which rides the occ_cur scalar, not the tilt. */
#define EQ_GAIN_FLOOR 1e-3f
static void eq_coeffs(int type, float cw0, float alpha, float g, float out[5]) {
    if (!(g > EQ_GAIN_FLOOR)) g = EQ_GAIN_FLOOR;         /* NaN-safe */
    bwa_biquad_rbj(type, cw0, alpha, sqrt((double)g), out);
}

/* Per-block setup for a 3-band VoiceEq (the occlusion + pathing bending-loss EQs share this exactly).
 * Glide the band gains toward tgt[3], detect flat, (re)engage the chain, and — when engaged — compute
 * this block's target biquad coeffs (co_tgt) and per-sample glide steps (co_step). Returns whether the
 * tilt is flat this block. Bit-identical to the two former inline copies (the caller reads the target
 * first — occlusion via eq_unpack, pathing via pp->eq — since the gain glide has no cross-band deps). */
static inline int eq_block_setup(VoiceEq* e, const float tgt[3], const EqProto proto[3],
                                 uint32_t nr, float co_tgt[3][5], float co_step[3][5]) {
    int flat = 1;
    for (int b = 0; b < 3; ++b) {
        e->g_cur[b] += (tgt[b] - e->g_cur[b]) * EQ_SLEW;
        if (e->g_cur[b] < 1.f - EQ_FLAT_EPS || e->g_cur[b] > 1.f + EQ_FLAT_EPS) flat = 0;
    }
    if (!flat) e->engaged = 1;
    if (e->engaged) {
        for (int b = 0; b < 3; ++b) {
            if (flat) { co_tgt[b][0] = 1.f; co_tgt[b][1] = co_tgt[b][2] = co_tgt[b][3] = co_tgt[b][4] = 0.f; }
            else eq_coeffs(proto[b].type, proto[b].cw0, proto[b].alpha, e->g_cur[b], co_tgt[b]);
            for (int k = 0; k < 5; ++k) co_step[b][k] = (co_tgt[b][k] - e->co[b][k]) / (float)nr;
        }
    }
    return flat;
}

/* Per-sample apply: 3 biquads (Direct-Form-I), each section's coeffs glided by co_step. Returns the
 * filtered sample. Bit-identical to the two former inline per-sample copies. */
static inline float eq_block_apply(VoiceEq* e, float s, const float co_step[3][5]) {
    for (int b = 0; b < 3; ++b) {
        float* co = e->co[b];
        float y = co[0]*s + co[1]*e->x1[b] + co[2]*e->x2[b] - co[3]*e->y1[b] - co[4]*e->y2[b];
        e->x2[b]=e->x1[b]; e->x1[b]=s; e->y2[b]=e->y1[b]; e->y1[b]=y; s=y;
        for (int k = 0; k < 5; ++k) co[k] += co_step[b][k];
    }
    return s;
}

/* End-of-block landing: snap the live coeffs to the block target; when settled flat, bypass + reset
 * the DF-I history. Bit-identical to the two former inline landing copies. */
static inline void eq_block_land(VoiceEq* e, const float co_tgt[3][5], int flat) {
    for (int b = 0; b < 3; ++b) for (int k = 0; k < 5; ++k) e->co[b][k] = co_tgt[b][k];
    if (flat) {
        e->engaged = 0;
        for (int b = 0; b < 3; ++b) { e->x1[b]=e->x2[b]=e->y1[b]=e->y2[b]=0.f; }
    }
}

/* (re)start a voice's Doppler delay line clean: clear its ring slice + snap the delay next block, so a
 * fresh enable or a replay doesn't bleed the previous tail through the line. Audio thread (bounded). */
static void dop_line_reset(RtCore* c, Voice* v, uint16_t idx) {
    v->dop_w = 0; v->dop_delay = 0.f; v->dop_dtgt = 0.f; v->dop_init = true;
    memset(c->dop_ring + (size_t)idx * c->dop_ringlen, 0, (size_t)c->dop_ringlen * sizeof(float));
}

/* Begin ONE voice's click-free stop — the CMD_STOP body, reused by the group/global sweeps
 * (CMD_GROUP_STOP / CMD_STOP_ALL). Fades the gate to 0 over one block; pause_gate finalizes.
 * Never downgrades a steal-in-progress (2), which must still free its slot.
 * The sweeps additionally drop the pending chain: CMD_STOP can leave it (the mix seam's !stopping
 * guard suppresses chaining anyway, and the next CMD_PLAY clears it), but a scene transition is a
 * one-shot gesture with no later play to do the clearing, so it must not leave a queue behind that
 * a re-play of the same handle would inherit. Audio thread, bounded, no allocation. */
static void voice_begin_stop(Voice* v) {
    if (!v->playing || v->stopping == 2) return;
    v->stopping = 1;
    v->stop_sched = false;                     /* a pending scheduled stop is moot now */
    for (uint32_t k = 0; k < BWA_QUEUE; ++k) v->queue[k] = NULL;
    v->queue_head = v->queue_len = 0;
}

/* ---- shared per-voice flag transitions -------------------------------------------------------
 * Three of the per-voice enables are NOT plain flags: each edge carries state work (clear a
 * recycled slot's delay ring, reset a Doppler line, ring the reflections/pathing out instead of
 * cutting them). CMD_SRC_CFG applies the same knobs as the single-knob commands, so both paths go
 * through these helpers rather than keeping two copies that can drift apart. */
static void ism_set(RtCore* c, Voice* v, uint16_t idx, bool on) {
    if (on && !v->ism_on && c->ism_ring) {   /* fresh enable: a clean ring (a recycled slot must
                                              * never replay the previous occupant), zeroed filter
                                              * state, gains from 0 (the reflections fade in), and
                                              * delays snapped on the first render (ism_init) */
        memset(c->ism_ring + (size_t)idx * c->ism_ringlen, 0, sizeof(float) * c->ism_ringlen);
        memset(v->ism_g,  0, sizeof v->ism_g);
        memset(v->ism_lp, 0, sizeof v->ism_lp);
        v->ism_w = 0; v->ism_init = true;
    }
    if (!on && v->ism_on) v->ism_tail = 1;   /* ramp the reflections out over one block */
    v->ism_on = on;
}
static void dop_set(RtCore* c, Voice* v, uint16_t idx, bool on) {
    if (on && !v->dop_on) dop_line_reset(c, v, idx);   /* fresh enable */
    v->dop_on = on;
}
static void path_set(Voice* v, bool on) {
    v->path_on = on;
    if (!on) {                                          /* clean restart: zero the ramp + flatten the EQ */
        for (int k = 0; k < BWA_AMBI_CH; ++k) v->path_sh_cur[k] = 0.f;
        v->path_eq.engaged = 0;
        for (int b = 0; b < 3; ++b) { v->path_eq.g_cur[b] = 1.f;
            v->path_eq.x1[b] = v->path_eq.x2[b] = v->path_eq.y1[b] = v->path_eq.y2[b] = 0.f; }
    }
}

static void drain_commands(RtCore* c) {
    CmdRing* r = &c->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* cmd = &r->slots[rd & (RING_CAP - 1)];
        switch (cmd->type) {
        case CMD_SRC_CREATE: {
            uint16_t idx = BWA_H_IDX(cmd->handle);
            Voice* v = &c->voices[idx];
            memset(v, 0, sizeof *v);
            v->gen = BWA_H_GEN(cmd->handle);
            v->active = true; v->gain_user = 1.f; v->dirty = true;
            /* start coherent with the live panner generation (it is created dirty, so it solves
             * either way; this just keeps the memset zero from reading as a stale generation) */
            v->pan_gen = atomic_load_explicit(&c->pan_gen, memory_order_acquire);
            v->occ_cur = 1.f;                       /* clear (un-occluded) by default */
            v->spread_h = -1.f;                     /* isotropic until rt_source_set_extent */
            v->dir_cur = 1.f;                       /* on-axis/omni by default */
            v->air_a_cur = 1.f;                     /* air low-pass passthrough by default */
            v->ldc_g_cur = 1.f;                     /* loudness-comp shelf flat by default */
            v->nf_g_cur  = 1.f;                     /* near-field shelf flat by default */
            v->dir_fwd[2] = 1.f;                    /* manual-directivity forward = room ahead (+z); weight 0 = off */
            v->dir_pow   = 1.f;
            v->pitch = v->pitch_cur = 1.f;          /* native playback rate by default */
            v->refl_gain = 1.f;                     /* full wet-send level by default (gated by refl_send) */
            v->pause_g = 1.f;                       /* pause gate open (running) by default */
            v->eq.g_cur[0] = v->eq.g_cur[1] = v->eq.g_cur[2] = 1.f;   /* flat EQ (history zeroed by memset) */
            for (int b = 0; b < 3; ++b) v->eq.co[b][0] = 1.f;        /* passthrough coeffs {1,0,0,0,0} */
            v->path_eq.g_cur[0] = v->path_eq.g_cur[1] = v->path_eq.g_cur[2] = 1.f;  /* flat pathing EQ */
            for (int b = 0; b < 3; ++b) v->path_eq.co[b][0] = 1.f;
            memset(&c->para[idx], 0, sizeof c->para[idx]);   /* fresh parametric-bed state for the slot */
            /* drop any publish the sim left for the prior occupant of this slot (the stores are
             * atomic, so a concurrent sim publish for the old handle can't tear; either way the new
             * gen won't match it). */
            atomic_store_explicit(&c->occ_handle[idx], 0u,   memory_order_relaxed);
            atomic_store_explicit(&c->occ_val[idx],    1.f,  memory_order_relaxed);
            atomic_store_explicit(&c->occ_eq[idx], eq_pack((float[3]){1.f,1.f,1.f}), memory_order_relaxed);
            atomic_store_explicit(&c->occ_dir[idx],    1.f,  memory_order_relaxed);
            atomic_store_explicit(&c->dir_pub[idx],    0u,   memory_order_relaxed);
        } break;
        case CMD_SRC_DESTROY: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->active = false; v->playing = false; v->sound = NULL;
                     /* drop the manual-directivity readback with the voice — a stale handle must
                      * read as omni (1), not the dead voice's last dipole gain */
                     atomic_store_explicit(&c->dir_pub[BWA_H_IDX(cmd->handle)], 0u, memory_order_relaxed); } } break;
        case CMD_SET_POS: { Voice* v = voice_for(c, cmd->handle);
            if (v) memcpy(v->pos_pending, &cmd->u.pos, sizeof v->pos_pending); } break;
        case CMD_SET_GAIN: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->gain_user = cmd->u.gain.g; v->dirty = true;
                     v->fade_rate = 0.f; v->fade_stop = 0; } } break;   /* an explicit set cancels a fade */
        case CMD_FADE: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                float tgt = !(cmd->u.fade.target > 0.f) ? 0.f : cmd->u.fade.target;   /* NaN-safe */
                /* NaN-safe: `seconds <= 0` is FALSE for NaN, so a NaN duration used to fall through
                 * to the ramp and make fade_rate = (tgt-g)/(NaN*rate) NaN. Treat it as instant. */
                if (!(cmd->u.fade.seconds > 0.f) || tgt == v->gain_user) {   /* instant (or already there) */
                    v->gain_user = tgt; v->dirty = true;
                    v->fade_rate = 0.f; v->fade_stop = 0;
                    if (cmd->u.fade.stop && v->playing && v->stopping != 2) v->stopping = 1;
                } else {
                    v->fade_target = tgt;
                    v->fade_rate   = (tgt - v->gain_user) / (cmd->u.fade.seconds * (float)c->sample_rate);
                    v->fade_stop   = cmd->u.fade.stop;
                }
            } } break;
        case CMD_SET_GROUP: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->group = cmd->u.group.id < BWA_GROUPS ? cmd->u.group.id : 0; v->dirty = true; } } break;
        case CMD_GROUP_GAIN: {
            uint8_t id = cmd->u.ggain.id;
            if (id < BWA_GROUPS) {
                c->group_gain[id] = !(cmd->u.ggain.gain > 0.f) ? 0.f : cmd->u.ggain.gain;   /* NaN-safe */
                for (uint32_t i = 0; i < c->voice_cap; ++i)          /* the group's voices re-solve (ramped) */
                    if (c->voices[i].active && c->voices[i].group == id) c->voices[i].dirty = true;
            } } break;
        case CMD_GROUP_PAUSED: {
            uint8_t id = cmd->u.gpause.id;
            if (id < BWA_GROUPS) c->group_paused[id] = cmd->u.gpause.on;   /* pause_gate ramps/freezes */
            } break;
        case CMD_GROUP_STOP: {
            /* Same sweep shape as CMD_GROUP_GAIN, but the members STOP instead of re-solving: each
             * takes rt_source_stop's one-block fade, so a category-wide stop is as click-free as a
             * single one. Bounded by voice_cap, no allocation — invariant 1 holds. */
            uint8_t id = cmd->u.group.id;
            if (id < BWA_GROUPS)
                for (uint32_t i = 0; i < c->voice_cap; ++i)
                    if (c->voices[i].active && c->voices[i].group == id) voice_begin_stop(&c->voices[i]);
            } break;
        case CMD_STOP_ALL:
            /* The scene transition: every voice, whatever its group. Beds are voices, so they stop
             * here too. Group gains, group/global pause and the master gain are deliberately left
             * alone — this stops sound, it does not reset the mixer. */
            for (uint32_t i = 0; i < c->voice_cap; ++i)
                if (c->voices[i].active) voice_begin_stop(&c->voices[i]);
            break;
        case CMD_SET_PITCH: { Voice* v = voice_for(c, cmd->handle);
            if (v && isfinite(cmd->u.pitch.rate)) {   /* NaN passes a two-sided clamp and sticks forever */
                     float r2 = cmd->u.pitch.rate;
                     v->pitch = r2 < 0.25f ? 0.25f : (r2 > 4.f ? 4.f : r2); } } break;   /* mixer glides */
        case CMD_BED_ROT: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->yaw = cmd->u.brot.yaw; v->bpitch = cmd->u.brot.pitch;
                     v->broll = cmd->u.brot.roll; } } break;              /* mix_bed glides toward them */
        case CMD_SET_ISM: { Voice* v = voice_for(c, cmd->handle);
            if (v) ism_set(c, v, BWA_H_IDX(cmd->handle), cmd->u.ism.on != 0); } break;
        case CMD_PLAY: { Voice* v = voice_for(c, cmd->handle);
            const SoundData* s = sound_for(c, cmd->u.play.sound);
            if (v && s) {
                if (s->stream)                       /* one voice per stream: the ring is SPSC. Detach any OTHER */
                    for (uint32_t j = 0; j < c->voice_cap; ++j)   /* voice on this stream so two consumers can't corrupt it */
                        if (&c->voices[j] != v && c->voices[j].sound == s) { c->voices[j].playing = false; c->voices[j].sound = NULL; }
                v->play_seq = cmd->u.play.seq;   /* what rt_source_is_playing compares against */
                v->sound = s; v->cursor = 0; v->cur_frac = 0.f; v->stream_pos = 0; v->loop = cmd->u.play.loop != 0;
                          v->oneshot = cmd->u.play.oneshot != 0; v->playing = true; v->dirty = true;
                          /* loop region: resolve against the asset so the mix seam sees in-bounds values.
                           * loop_end 0 (or out of range) = whole clip; a degenerate beg >= end = whole clip. */
                          v->loop_beg = (cmd->u.play.loop_beg < (uint64_t)s->frames) ? (uint32_t)cmd->u.play.loop_beg : 0;
                          v->loop_end = (cmd->u.play.loop_end != 0 && cmd->u.play.loop_end <= (uint64_t)s->frames)
                                        ? (uint32_t)cmd->u.play.loop_end : 0;
                          v->stop_sched = false; v->stop_at = 0;   /* a fresh play cancels any pending scheduled stop */
                          for (uint32_t k = 0; k < BWA_QUEUE; ++k) v->queue[k] = NULL;
                          v->queue_head = v->queue_len = 0;      /* and the pending chain (queue AFTER play) */
                          v->start_sample = cmd->u.play.start;  /* 0 = now; else hold output until this dsp-sample */
                          v->refl_g_cur = 0.f;                  /* fresh start: ramp the wet send up from 0, no stale burst */
                          /* Click-free start: ramp the per-channel gains up from silence over the first
                           * block. gcur is the FINAL per-channel multiply, so 0 -> gtarget fades in the
                           * whole direct path (every pre-pan filter included) — a REPLAYED slot otherwise
                           * keeps the prior solve's gains and hits the asset's first sample at full level
                           * (a click if it isn't near zero). A fresh voice is already zero here (create
                           * memset), so first-plays/goldens are unchanged; fs_on = 0 makes a replayed
                           * spectral-spread voice re-engage and re-seed from the zeroed gains too. */
                          memset(v->gcur,    0, sizeof v->gcur);
                          memset(v->gcur_lo, 0, sizeof v->gcur_lo);
                          v->fs_on = 0;
                          v->xover_lp = 0.f;                    /* fresh dual-band crossover state */
                          v->dual_mix = (!c->direct_on &&                                                       /* start in the current mode; direct
                                                                                                                 * has no dual-band (SH gains, and
                                                                                                                 * gtarget_lo is never solved there) */
                                         atomic_load_explicit(&c->dual_band, memory_order_relaxed)) ? 1.f : 0.f;
                          v->re_mix   = (!c->direct_on &&                                                       /* (beds) likewise; direct mode
                                                                                                                 * never engages the taper */
                                         atomic_load_explicit(&c->max_re, memory_order_relaxed)) ? 1.f : 0.f;
                          v->paused = false; v->pause_g = 1.f; v->seek_pending = 0;
                          /* Play always starts running, EXCEPT that it must not downgrade a
                           * steal-in-progress (2) - the same rule CMD_STOP follows below. A steal
                           * has already handed the caller a replacement handle on a reserve slot
                           * and is counting on this voice to fade, free, and ack. Resurrecting it
                           * cancels that ack, so stealing[] stays set and the source can never be
                           * stolen again while it lives, leaving the pool a slot short. A client
                           * playing a mid-steal handle could always reach this; rt_sound_publish
                           * made the engine able to do it to itself, at a decode's timing. */
                          if (v->stopping != 2) v->stopping = 0;
                          if (v->dop_on) dop_line_reset(c, v, BWA_H_IDX(cmd->handle)); } } break;
        case CMD_STOP: { Voice* v = voice_for(c, cmd->handle);
            /* Fade the gate to 0 over one block, then finalize (playing=false) in pause_gate — a
             * click-free explicit stop. Don't downgrade a steal-in-progress (2), which must still free
             * its slot. (CMD_SRC_DESTROY hard-cuts; automatic steal fades via CMD_SRC_STEAL below.) */
            if (v && v->playing && v->stopping != 2) v->stopping = 1; } break;   /* the seam's !stopping guard suppresses the chain */
        case CMD_STOP_AT: { Voice* v = voice_for(c, cmd->handle);
            /* Arm a scheduled stop; rt_render fires the click-free fade once the block reaches stop_at. */
            if (v) { v->stop_at = cmd->u.stopat.sample; v->stop_sched = true; } } break;
        case CMD_QUEUE: { Voice* v = voice_for(c, cmd->handle);
            const SoundData* s = sound_for(c, cmd->u.enq.sound);
            /* Append to the gapless queue. In-memory mono only (pcm set, not a stream, one channel);
             * a full queue drops silently (BWA_QUEUE depth). The control side already gates these, so
             * the checks here are defensive — a stale/incompatible handle just never enqueues. */
            if (v && s && s->pcm && s->channels == 1 && !s->stream && v->queue_len < BWA_QUEUE) {
                uint32_t slot = (v->queue_head + v->queue_len) % BWA_QUEUE;
                v->queue[slot] = s; v->queue_loop[slot] = cmd->u.enq.loop; v->queue_len++;
            } } break;
        case CMD_QUEUE_CLEAR: { Voice* v = voice_for(c, cmd->handle);
            if (v) { for (uint32_t k = 0; k < BWA_QUEUE; ++k) v->queue[k] = NULL; v->queue_head = v->queue_len = 0; } } break;
        case CMD_SRC_STEAL: { Voice* v = voice_for(c, cmd->handle);
            /* Fade the stolen voice out on its own slot, then free it (pause_gate finalize pushes
             * EVT_VOICE_ENDED so the control thread recycles the slot). The new source already started
             * on a reserve slot, so the steal is click-free. */
            if (v && v->playing) v->stopping = 2;
            else if (v) {                /* already silent (ended, stopped, or a drained push source —
                                          * its normal terminal state): nothing to fade, so finalize and
                                          * ack NOW. Without this the control side waits forever on an
                                          * event that never comes: stealing[] sticks, the slot never
                                          * recycles, and a push victim's internal sound never retires. */
                v->stopping = 0; v->playing = false; v->active = false; v->sound = NULL;
                Evt ev = { .type = EVT_VOICE_ENDED, .handle = cmd->handle };
                evt_push(&c->events, &ev);
            } } break;
        case CMD_SET_PAUSED: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->paused = cmd->u.pause.on != 0; } break;    /* the mixer's gate does the ramp/freeze */
        case CMD_SEEK: { Voice* v = voice_for(c, cmd->handle);
            if (v && v->sound && !v->sound->stream) {            /* streams: the ring can't jump — ignored */
                v->seek_pos = cmd->u.seek.frame;
                v->seek_pending = 1;                             /* lands once the gate is silent (click-free) */
            } } break;
        case CMD_SET_REGION: { Voice* v = voice_for(c, cmd->handle);
            /* Resolved against the BOUND asset, exactly as CMD_PLAY resolves the play-time region
             * into the same two fields — one mechanism, so a region and an intro->loop cannot drift
             * apart. Streams keep seek's rule (the ring is sequential, nothing to bound), and an
             * unbound voice has no frame count to judge against, so the region must be set after the
             * play. Out-of-range end = the asset end; a degenerate pair falls back to the whole clip
             * in resolve_loop_region, and is already refused on the control thread. */
            if (v && v->sound && !v->sound->stream) {
                const uint64_t b = cmd->u.region.beg, e2 = cmd->u.region.end;
                v->loop_beg = (b < (uint64_t)v->sound->frames) ? (uint32_t)b : 0;
                v->loop_end = (e2 != 0 && e2 <= (uint64_t)v->sound->frames) ? (uint32_t)e2 : 0;
            } } break;
        case CMD_SET_CHANNEL: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->out_ch_on = cmd->u.outch.on; v->out_ch = cmd->u.outch.ch;
                     v->dirty = true; }   /* re-solve: the gain vector IS the route (compute_gains) */
            } break;
        case CMD_SET_REFLECTIONS: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_send = cmd->u.refl.on != 0; } break;
        case CMD_SET_PATHING: { Voice* v = voice_for(c, cmd->handle);
            if (v) path_set(v, cmd->u.path.on != 0); } break;
        case CMD_SET_REFL_SEND: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float g = cmd->u.rsend.gain; v->refl_gain = !(g > 0.f) ? 0.f : g; } } break;   /* NaN-safe */
        case CMD_SET_REFL_DIST: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_dist = cmd->u.rdist.on != 0; } break;
        case CMD_SET_DOPPLER: { Voice* v = voice_for(c, cmd->handle);
            if (v) dop_set(c, v, BWA_H_IDX(cmd->handle), cmd->u.dop.on != 0); } break;
        case CMD_SET_AIR: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->air_on = cmd->u.air.on != 0; } break;
        case CMD_SET_LDC: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->ldc_on = cmd->u.ldc.on != 0; } break;      /* the mixer ramps the shelf in/out */
        case CMD_SET_NF: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->nf_on = cmd->u.nf.on != 0; } break;        /* the mixer ramps the shelf in/out */
        case CMD_SET_DIR: { Voice* v = voice_for(c, cmd->handle);
            if (v) { memcpy(v->dir_fwd, cmd->u.dir.fwd, sizeof v->dir_fwd);
                     v->dir_w = cmd->u.dir.weight; v->dir_pow = cmd->u.dir.power;
                     if (!(v->dir_w > 0.f))         /* pattern off: the mixer stops republishing, so
                                                     * drop the readback here or rt_get_directivity
                                                     * would report the last dipole gain forever */
                         atomic_store_explicit(&c->dir_pub[BWA_H_IDX(cmd->handle)], 0u, memory_order_relaxed); } } break;
        /* One command, every ring-carried per-source knob (bwa_source_apply). rt_source_apply_cfg
         * already sanitized the payload on the control thread with the SAME guards the single-knob
         * setters use, so this handler only stores — and it routes the three stateful enables
         * through ism_set/dop_set/path_set so the two paths cannot drift. Latest-wins against the
         * single-knob commands: they share one ring, so program order IS apply order. */
        case CMD_SRC_CFG: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                const uint16_t idx = BWA_H_IDX(cmd->handle);
                const uint8_t  f   = cmd->u.cfg.flags;
                v->gain_user = cmd->u.cfg.gain;
                v->fade_rate = 0.f; v->fade_stop = 0;    /* an explicit set cancels a fade (CMD_SET_GAIN) */
                v->pitch     = cmd->u.cfg.pitch;
                v->group     = cmd->u.cfg.group;
                v->spread    = cmd->u.cfg.spread;
                v->spread_h  = cmd->u.cfg.extent_h;
                v->size_m    = cmd->u.cfg.size_m;
                v->refl_gain = cmd->u.cfg.rsend;
                v->att_ref   = cmd->u.cfg.aref;          /* 0 = no override (back to the layout curve) */
                v->att_rolloff = cmd->u.cfg.aroll;
                v->att_min   = cmd->u.cfg.amin;
                v->air_on    = (f & BWA_CFG_AIR)    != 0;
                v->ldc_on    = (f & BWA_CFG_LDC)    != 0;
                v->nf_on     = (f & BWA_CFG_NF)     != 0;
                v->refl_send = (f & BWA_CFG_REVERB) != 0;
                v->refl_dist = (f & BWA_CFG_RDIST)  != 0;
                dop_set (c, v, idx, (f & BWA_CFG_DOPPLER) != 0);
                ism_set (c, v, idx, (f & BWA_CFG_ISM)     != 0);
                path_set(v,         (f & BWA_CFG_PATH)    != 0);
                v->dirty = true;                         /* gain/group/spread/size/atten all re-solve */
            } } break;
        case CMD_SET_ATTEN: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                float ref = cmd->u.atten.ref;
                if (ref <= 0.f) v->att_ref = 0.f;                /* clear: back to the layout curve */
                else {
                    v->att_ref     = ref;
                    v->att_rolloff = cmd->u.atten.rolloff < 0.f ? 0.f : cmd->u.atten.rolloff;
                    float m = cmd->u.atten.min_lin;
                    v->att_min     = m < 0.f ? 0.f : (m > 1.f ? 1.f : m);
                }
                v->dirty = true;                                 /* re-solve with the new curve */
            } } break;
        case CMD_SET_EXTRA_LIS:
            c->lis.nex_pending = cmd->u.exlis.n > BWA_EXTRA_LIS ? BWA_EXTRA_LIS : cmd->u.exlis.n;
            memcpy(c->lis.ex_pending, cmd->u.exlis.p, sizeof c->lis.ex_pending);
            break;
        case CMD_SET_SPREAD: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float a = cmd->u.spread.amount, hh = cmd->u.spread.height;
                     v->spread   = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
                     v->spread_h = hh < 0.f ? -1.f : (hh > 1.f ? 1.f : hh);   /* < 0 = isotropic */
                     v->dirty = true; } } break;
        case CMD_SET_SIZE: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float rad = cmd->u.size.radius; v->size_m = rad < 0.f ? 0.f : rad; v->dirty = true; } } break;
        case CMD_TEST_SIGNAL: {
            uint32_t ch = cmd->u.test.channel;
            if (ch < c->channels) { c->test_kind[ch] = cmd->u.test.kind; c->test_gain[ch] = cmd->u.test.gain; }
        } break;
        case CMD_SET_LISTENER:
            memcpy(c->lis.p_pending, &cmd->u.lis.px, sizeof(float) * 3);
            memcpy(c->lis.q_pending, &cmd->u.lis.qx, sizeof(float) * 4);
            break;
        case CMD_COMMIT: {
            bool lis_moved = memcmp(c->lis.p_active, c->lis.p_pending, sizeof c->lis.p_active) != 0 ||
                             c->lis.nex_active != c->lis.nex_pending ||
                             memcmp(c->lis.ex_active, c->lis.ex_pending, sizeof c->lis.ex_active) != 0;
            /* Head ORIENTATION moves the gains only under CAP (cap.c): every other panner is
             * position-relative, and orientation reaches them not at all — it enters at the binaural
             * decode instead. So this comparison is gated, not unconditional: without the gate a
             * tracked head would re-solve every voice every block for a rotation nothing downstream
             * reads, which is precisely the per-block resolve cost DBAP exists to avoid. */
            if (!lis_moved && atomic_load_explicit(&c->cap_on, memory_order_acquire))
                lis_moved = memcmp(c->lis.q_active, c->lis.q_pending, sizeof c->lis.q_active) != 0;
            memcpy(c->lis.p_active, c->lis.p_pending, sizeof c->lis.p_active);
            memcpy(c->lis.q_active, c->lis.q_pending, sizeof c->lis.q_active);
            memcpy(c->lis.ex_active, c->lis.ex_pending, sizeof c->lis.ex_active);
            c->lis.nex_active = c->lis.nex_pending;
            for (uint32_t i = 0; i < c->voice_cap; ++i) {
                Voice* v = &c->voices[i];
                if (!v->active) continue;
                if (memcmp(v->pos_active, v->pos_pending, sizeof v->pos_active)) {
                    memcpy(v->pos_active, v->pos_pending, sizeof v->pos_active);
                    v->dirty = true;
                }
                if (lis_moved) v->dirty = true;                /* gains are listener-relative */
            }
        } break;
        case CMD_SOUND_RETIRE: {
            /* Detach every voice still bound to this sound, then ack so the control thread
             * frees the buffer exactly once the audio thread has provably let go. */
            const SoundData* s = sound_for(c, cmd->handle);
            if (s) {
                for (uint32_t i = 0; i < c->voice_cap; ++i) {
                    if (c->voices[i].sound == s) { c->voices[i].playing = false; c->voices[i].sound = NULL; }
                    for (uint32_t k = 0; k < BWA_QUEUE; ++k)     /* a queued (chained) reference dangles otherwise: */
                        if (c->voices[i].queue[k] == s) c->voices[i].queue[k] = NULL;   /* tombstone (pop skips NULLs) */
                }
            }
            Evt ev = { .type = EVT_SOUND_RETIRED, .handle = cmd->handle };
            evt_push(&c->events, &ev);
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* Dispatch the bed decode: AllRAD or EPAD if selected (and the build succeeds), else the shared
 * sampling decode (ambi_sad_decode — assumes a roughly uniform array, fine for a diffuse bed).
 * World-locked: directions are from the array centroid, not the moving listener. Room convention
 * (post +z-forward flip): identity listener faces +z, right ear at -x, +y up; AmbiX axes are
 * x=front/y=left/z=up, so ambi front = room +z (where the listener faces / the main content sits),
 * ambi left = room +x, ambi up = room +y.
 * Also derives the parametric bed's constants from the result: bed_radius (the virtual-source shell
 * for the re-panned direct stream) and bed_pref (so a plane wave renders at the same loudness through
 * either bed renderer: the mean power the FOA matrix decode produces, averaged over the speaker
 * directions as direction samples). */
static void build_bed_decode(RtCore* c) {
    int built = 0;
    if      (c->bed_decoder == 1) built = allrad_build_decode(&c->layout, c->bed_decode);
    else if (c->bed_decoder == 2) built = epad_build_decode(&c->layout, c->bed_decode);
    if (!built)
        ambi_sad_decode(&c->layout, c->channels, c->bed_decode);
    double rsum = 0.0, psum = 0.0;
    for (uint32_t s = 0; s < c->channels; ++s) {
        float p[3] = { c->layout.speakers[s].pos[0] - c->layout.ref[0],
                       c->layout.speakers[s].pos[1] - c->layout.ref[1],
                       c->layout.speakers[s].pos[2] - c->layout.ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        rsum += len;
        float dr[3] = { len > 1e-6f ? p[0]/len : 0.f, len > 1e-6f ? p[1]/len : 0.f, len > 1e-6f ? p[2]/len : 1.f };
        float ad[3]; room_to_ambi(dr, ad);                     /* (z,x,y): ambi front=+z, left=+x, up=+y */
        float y[BWA_AMBI_CH]; ambi_encode_sn3d(ad, y);
        for (uint32_t ch = 0; ch < c->channels; ++ch) {        /* FOA plane wave from dir s -> power */
            float acc = 0.f;
            for (int k = 0; k < 4; ++k) acc += c->bed_decode[ch][k] * y[k];
            psum += (double)acc * acc;
        }
    }
    c->bed_radius = c->channels ? (float)(rsum / c->channels) : 1.f;
    if (c->bed_radius < 0.5f) c->bed_radius = 0.5f;
    c->bed_pref = (float)sqrt(psum / (c->channels ? c->channels : 1));
}

/* Up-anchored tangent frame for ANISOTROPIC extent: u = the horizontal tangent, w = the
 * vertical-ish one. Width/height are room-referenced, so the orientation-free transported frame
 * (spread_frame below) cannot carry them — this keeps the pole branch the transported frame exists
 * to avoid, because an anisotropic extent straight overhead is inherently ill-defined (BS.2127's
 * polar extent has the same singularity); the snap is accepted for anisotropic sources there. */
static void up_frame(const float d[3], float u[3], float w[3]) {
    float up[3] = { 0.f, 1.f, 0.f };
    if (d[1] > 0.9f || d[1] < -0.9f) { up[0] = 1.f; up[1] = 0.f; }
    u[0] = up[1]*d[2]-up[2]*d[1]; u[1] = up[2]*d[0]-up[0]*d[2]; u[2] = up[0]*d[1]-up[1]*d[0];
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0]/=ul; u[1]/=ul; u[2]/=ul;
    w[0] = d[1]*u[2]-d[2]*u[1]; w[1] = d[2]*u[0]-d[0]*u[2]; w[2] = d[0]*u[1]-d[1]*u[0];
}

/* Polar radius of the extent ellipse at tangent azimuth (cp toward u, sp toward w): r(0°) = eu
 * (the width ratio), r(90°) = ew (the height ratio) — used by the LOBE mode to stretch a speaker's
 * angle from the source direction. The ratios come from compute_gains (max of the two is exactly
 * 1); flooring them keeps a zero extent well-defined (the raw polar form's numerator eu·ew would
 * kill the on-axis limit too — a 1×0 extent must keep its width, not collapse to a point). The
 * ring modes use the affine tangent squash instead (see mdap_gains). */
static float ext_scale(float eu, float ew, float cp, float sp) {
    if (eu < 1e-3f) eu = 1e-3f;
    if (ew < 1e-3f) ew = 1e-3f;
    float a = ew * cp, b = eu * sp;
    float dn = sqrtf(a * a + b * b);
    return dn > 1e-9f ? eu * ew / dn : 1.f;
}

/* Source spread/size: blend the panner's point gains toward a width-controlled lobe centered on the
 * source direction (from the listener), then renormalize constant-power. spread 0 = the point gains;
 * 1 = a wide lobe. Panner-agnostic; runs only in the per-block gain solve (not the sample loop). */
static void spread_gains(RtCore* c, const Voice* v, float spread, float* g) {
    float d[3] = { v->pos_active[0]-c->lis.p_active[0], v->pos_active[1]-c->lis.p_active[1], v->pos_active[2]-c->lis.p_active[2] };
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return;                              /* source on the listener: no direction to spread around */
    d[0]/=dl; d[1]/=dl; d[2]/=dl;
    double p0 = 0.0; for (uint32_t k = 0; k < c->channels; ++k) p0 += (double)g[k]*g[k];
    float P = (float)sqrt(p0);                           /* preserve the panner's own power (never re-level) */
    if (P < 1e-9f) return;
    float s = spread; if (s > 1.f) s = 1.f;
    float q = 1.5f + (1.f - s) * 6.f;                    /* lobe exponent: wide at s=1, tight as s->0 */
    const int aniso = v->ext_u != 1.f || v->ext_w != 1.f;
    float au[3], aw[3];
    if (aniso) up_frame(d, au, aw);                      /* room-referenced width/height (see up_frame) */
    float lobe[BWA_CHANNELS]; double ln = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) {
        const float* sp = c->layout.speakers[k].pos;
        float sd[3] = { sp[0]-c->lis.p_active[0], sp[1]-c->lis.p_active[1], sp[2]-c->lis.p_active[2] };
        float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        float dot = sl > 1e-6f ? (sd[0]*d[0] + sd[1]*d[1] + sd[2]*d[2]) / sl : 0.f;
        if (aniso && sl > 1e-6f && dot < 0.99999f) {
            /* elliptical lobe: stretch the angle to the speaker by the extent ellipse's inverse
             * polar radius at its tangent azimuth — narrower along the smaller extent */
            float su  = (sd[0]*au[0] + sd[1]*au[1] + sd[2]*au[2]) / sl;
            float sw2 = (sd[0]*aw[0] + sd[1]*aw[1] + sd[2]*aw[2]) / sl;
            float t = sqrtf(su*su + sw2*sw2);
            if (t > 1e-6f) {
                float cl = dot < -1.f ? -1.f : dot;
                float th = acosf(cl) / ext_scale(v->ext_u, v->ext_w, su / t, sw2 / t);
                dot = (th >= 3.14159265f) ? -1.f : cosf(th);
            }
        }
        float w = 0.5f * (1.f + dot); if (w < 0.f) w = 0.f;   /* [0,1], 1 toward the source */
        lobe[k] = powf(w, q);
        ln += (double)lobe[k] * lobe[k];
    }
    if (ln < 1e-12) return;
    float lnorm = (float)(P / sqrt(ln));                 /* scale the lobe to the same power P */
    double bn = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) { g[k] = (1.f - s) * g[k] + s * lobe[k] * lnorm; bn += (double)g[k]*g[k]; }
    if (bn < 1e-12) return;
    float bnorm = P / (float)sqrt(bn);                   /* the blend isn't norm-P -> renormalize back to P */
    for (uint32_t k = 0; k < c->channels; ++k) g[k] *= bnorm;
}

/* The selected panner's point solve at an arbitrary source position for an arbitrary listener.
 * `p` is the panner id, loaded once per gain solve so one solve never mixes panners mid-ring. The
 * SPCAP/VBAP caches are keyed to ONE listener each — the caller supplies the pair (the primary's,
 * or an extra listener's own for compromise panning). */
static void panner_gains_at(RtCore* c, int p, const float lis[3], SpcapState* ss, VbapState* vs,
                            const float src[3], float user_gain, float* out) {
    if (p == 1)
        spcap_gains(ss, src, lis, &c->layout, c->layout_gen,
                    c->spcap_focus_blk, c->spcap_density_blk, user_gain, out);
    else if (p == 2)
        vbap_gains(vs, src, lis, &c->layout, c->layout_gen, user_gain, out);
    else
        dbap_gains(src, lis, &c->layout, user_gain, out);
}

/* The primary listener's solve (the voice solve below, MDAP's virtual sources, the parametric bed). */
static void panner_gains(RtCore* c, int p, const float src[3], float user_gain, float* out) {
    panner_gains_at(c, p, c->lis.p_active, &c->spcap, &c->vbap, src, user_gain, out);
}

/* atten_curve (the shared distance-attenuation formula) now lives in layout.h. */

/* Orthonormal ring frame (u, w) around the source direction d, PARALLEL-TRANSPORTED per voice:
 * project the stored base off the new d rather than deriving u from a fixed up-vector, whose
 * branch flip at |d.y| = 0.9 turns the frame ~180° in one solve when a moving source leaves the
 * pole zone — the ramps mask the level step, but every spectral band's direction (and the MDAP
 * ring's sampling) teleports (Pulkki's reference vbap external transports the same state through
 * its spread ring). First solve — or a degenerate base after a >90° direction jump — reseeds from
 * the old fixed-up heuristic; MDAP and spectral share the base, so an A/B stays continuous. */
static void spread_frame(Voice* v, const float d[3], float u[3], float w[3]) {
    const float* b = v->sp_base;
    float dot = b[0]*d[0] + b[1]*d[1] + b[2]*d[2];
    u[0] = b[0] - dot*d[0]; u[1] = b[1] - dot*d[1]; u[2] = b[2] - dot*d[2];
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-4f) {                                    /* unset (zero) or parallel to d: (re)seed */
        up_frame(d, u, w);                               /* the fixed-up heuristic — same ops as the fall-through below */
        v->sp_base[0] = u[0]; v->sp_base[1] = u[1]; v->sp_base[2] = u[2];
        return;
    }
    u[0]/=ul; u[1]/=ul; u[2]/=ul;
    v->sp_base[0] = u[0]; v->sp_base[1] = u[1]; v->sp_base[2] = u[2];
    w[0] = d[1]*u[2]-d[2]*u[1]; w[1] = d[2]*u[0]-d[0]*u[2]; w[2] = d[0]*u[1]-d[1]*u[0];
}

/* Source spread/size, MDAP mode (Pulkki 1999: multiple-direction amplitude panning): pan a ring of
 * VIRTUAL SOURCES around the source direction with the selected panner and sum, instead of reshaping
 * the point gains (spread_gains above). The extent is made of real panner solves, so it inherits the
 * panner's own character (VBAP stays sparse per direction, SPCAP stays placement-corrected). Cone
 * half-angle = spread * 90°; the virtual sources sit at the source's own distance so the distance
 * attenuation is untouched; the sum is renormalized to the point solve's power P (widening never
 * re-levels). At spread->0 the ring collapses onto the source direction and the result IS the point
 * solve, so the two spread modes meet continuously. 12 extra panner solves, per-block + dirty-gated. */
static void mdap_gains(RtCore* c, int p, Voice* v, float spread, float user_gain, float* g) {
    float d[3] = { v->pos_active[0]-c->lis.p_active[0], v->pos_active[1]-c->lis.p_active[1], v->pos_active[2]-c->lis.p_active[2] };
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return;                              /* source on the listener: no direction to spread around */
    d[0]/=dl; d[1]/=dl; d[2]/=dl;
    double p0 = 0.0; for (uint32_t k = 0; k < c->channels; ++k) p0 += (double)g[k]*g[k];
    float P = (float)sqrt(p0);                           /* preserve the panner's own power (never re-level) */
    if (P < 1e-9f) return;
    const int aniso = v->ext_u != 1.f || v->ext_w != 1.f;
    float u[3], w[3];
    if (aniso) up_frame(d, u, w);                        /* room-referenced width/height (see up_frame) */
    else       spread_frame(v, d, u, w);                 /* transported frame: no pole snap */
    float s = spread; if (s > 1.f) s = 1.f;
    float acc[BWA_CHANNELS], gd[BWA_CHANNELS];
    for (uint32_t k = 0; k < c->channels; ++k) acc[k] = g[k];   /* the point solve is the ring center */
    /* two rings — 8 at the full cone angle, 4 offset at half — approximate a uniform spherical cap */
    static const int   ring_n[2]   = { 8, 4 };
    static const float ring_a[2]   = { 1.f, 0.5f };
    static const float ring_off[2] = { 0.f, 0.7853982f };      /* half-ring offset: pi/4 */
    for (int r = 0; r < 2; ++r) {
        const float a = s * 1.5707963f * ring_a[r];            /* cone half-angle: spread 1 = 90° */
        const float ca = cosf(a), sa = sinf(a);
        for (int i = 0; i < ring_n[r]; ++i) {
            float phi = ring_off[r] + 6.2831853f * (float)i / (float)ring_n[r];
            float cp = cosf(phi), sp = sinf(phi);
            float pos[3];
            if (aniso) {
                /* affine tangent squash: scale the ring offset per axis and renormalize — a 1×0
                 * extent becomes a sampled horizontal ARC (the polar-ellipse form would collapse
                 * every off-axis point to the center), matching BS.2127's squashed-cap weighting */
                const float ou = sa * cp * v->ext_u, ow = sa * sp * v->ext_w;
                const float inv = 1.f / sqrtf(ca * ca + ou * ou + ow * ow);
                for (int k = 0; k < 3; ++k)
                    pos[k] = c->lis.p_active[k] + dl * ((ca * d[k] + ou * u[k] + ow * w[k]) * inv);
            } else {
                for (int k = 0; k < 3; ++k)
                    pos[k] = c->lis.p_active[k] + dl * (ca * d[k] + sa * (cp * u[k] + sp * w[k]));
            }
            panner_gains(c, p, pos, user_gain, gd);
            for (uint32_t k = 0; k < c->channels; ++k) acc[k] += gd[k];
        }
    }
    double an = 0.0; for (uint32_t k = 0; k < c->channels; ++k) an += (double)acc[k]*acc[k];
    if (an < 1e-12) return;
    float norm = (float)(P / sqrt(an));                        /* back to the point solve's power */
    for (uint32_t k = 0; k < c->channels; ++k) g[k] = acc[k] * norm;
}

/* Source spread, SPECTRAL mode (frequency-dependent panning — see BWA_FS_BANDS above): one real
 * panner solve per band at a direction inside the spread cone. Band 0 (LF) stays on the source
 * direction (LF localization is what dual-band fights for; scattering it buys nothing), the upper
 * bands scatter around the cone ring golden-angle style at alternating radii, and each band is
 * renormalized to the point solve's power P — complementary bands partition the signal, so the sum
 * stays constant-power to within the crossover overlap (< ~1 dB). gtarget KEEPS the point solve:
 * the band path replaces the output stage while engaged, seeding from gcur on engage and handing
 * back onto gtarget on retire, so both transitions are exact (no crossfade machinery). With
 * dual-band on, the sub-crossover bands (0..1, < 700 Hz) take the amplitude norm instead — the two
 * A/Bs compose (folded in at the solve, so a dual toggle lands on the next re-solve). */
static void fs_solve(RtCore* c, Voice* v, int p, float spread, float ug) {
    float d[3] = { v->pos_active[0]-c->lis.p_active[0], v->pos_active[1]-c->lis.p_active[1], v->pos_active[2]-c->lis.p_active[2] };
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return;                              /* source on the listener: keep the last solve */
    d[0]/=dl; d[1]/=dl; d[2]/=dl;
    double p0 = 0.0; for (uint32_t k = 0; k < c->channels; ++k) p0 += (double)v->gtarget[k]*v->gtarget[k];
    const float P = (float)sqrt(p0);                     /* the point solve's power (never re-level) */
    if (P < 1e-9f) return;
    const int aniso = v->ext_u != 1.f || v->ext_w != 1.f;
    float u[3], w[3];
    if (aniso) up_frame(d, u, w);                        /* room-referenced width/height (see up_frame) */
    else       spread_frame(v, d, u, w);                 /* transported frame, shared with MDAP */
    if (!v->fs_on) {                                     /* engage seamlessly: bands start at the live
                                                          * gains; the splitter (and the idled dual-band
                                                          * crossover) restart clean */
        for (int b = 0; b < BWA_FS_BANDS; ++b)
            for (uint32_t k = 0; k < c->channels; ++k) v->fs_g[b][k] = v->gcur[k];
        memset(v->fs_lp, 0, sizeof v->fs_lp);
        v->xover_lp = 0.f;
    }
    v->fs_on = 1;
    static const float fs_ca[BWA_FS_BANDS] = { 0.f, 1.f, 0.55f, 1.f, 0.55f, 1.f };   /* cone-angle scale */
    static const float fs_az[BWA_FS_BANDS] = { 0.f, 0.f, 2.4f, 4.8f, 0.917f, 3.317f };  /* golden-ish ring */
    for (int b = 0; b < BWA_FS_BANDS; ++b) {
        float* t = v->fs_t[b];
        if (b == 0) {
            for (uint32_t k = 0; k < c->channels; ++k) t[k] = v->gtarget[k];
        } else {
            const float cp = cosf(fs_az[b]), sp = sinf(fs_az[b]);
            const float a = spread * 1.5707963f * fs_ca[b];              /* cone half-angle: spread 1 = 90 deg */
            const float ca = cosf(a), sa = sinf(a);
            float pos[3];
            if (aniso) {                                                 /* affine band scatter (as mdap_gains) */
                const float ou = sa * cp * v->ext_u, ow = sa * sp * v->ext_w;
                const float inv = 1.f / sqrtf(ca * ca + ou * ou + ow * ow);
                for (int k = 0; k < 3; ++k)
                    pos[k] = c->lis.p_active[k] + dl * ((ca * d[k] + ou * u[k] + ow * w[k]) * inv);
            } else {
                for (int k = 0; k < 3; ++k)
                    pos[k] = c->lis.p_active[k] + dl * (ca * d[k] + sa * (cp * u[k] + sp * w[k]));
            }
            panner_gains(c, p, pos, ug, t);
            double bn = 0.0; for (uint32_t k = 0; k < c->channels; ++k) bn += (double)t[k]*t[k];
            if (bn > 1e-12) { const float sc = (float)(P / sqrt(bn));
                for (uint32_t k = 0; k < c->channels; ++k) t[k] *= sc; }
        }
    }
    {   /* overlap compensation (see fs_w): predicted white-noise power = P^2 * sum W_ab (g_a.g_b)/P^2;
         * scattering the correlated one-pole bands apart loses the cross terms, so rescale the whole
         * set to put it back — exact for white input, 1 when the bands are parallel (spread -> 0). */
        double q = 0.0;
        for (int a = 0; a < BWA_FS_BANDS; ++a)
            for (int b = a; b < BWA_FS_BANDS; ++b) {
                double dot = 0.0;
                for (uint32_t k = 0; k < c->channels; ++k) dot += (double)v->fs_t[a][k] * v->fs_t[b][k];
                q += (a == b ? 1.0 : 2.0) * c->fs_w[a][b] * dot;
            }
        q /= (double)P * P;
        if (q > 1e-6) {
            const float comp = (float)(1.0 / sqrt(q));
            for (int b = 0; b < BWA_FS_BANDS; ++b)
                for (uint32_t k = 0; k < c->channels; ++k) v->fs_t[b][k] *= comp;
        }
    }
    /* NOTE: CAP (cap.c) does NOT reach this path. Spectral spread replaces the single-path output
     * stage wholesale, so a spread source under smode 2 gets the PLAIN amplitude normalization below
     * for its low bands — the exact A-side CAP exists to replace — rather than CAP's fade-with-spread.
     * Projecting here is not a one-liner: band 1 is scattered off the point bearing by the cone, so
     * it needs its own ITD target, and the overlap compensation above has already rescaled the set.
     * Left as a known exclusion (both modes are off by default); documented in bw_audio.h and
     * docs/spatialization.md rather than silently wrong. */
    if (atomic_load_explicit(&c->dual_band, memory_order_acquire)) {
        for (int b = 0; b <= 1; ++b) {                   /* < 700 Hz: amplitude (pressure) norm, as gtarget_lo */
            float* t = v->fs_t[b];
            double gs = 0.0, gp = 0.0;
            for (uint32_t k = 0; k < c->channels; ++k) { gs += t[k]; gp += (double)t[k]*t[k]; }
            if (gs > 1e-9) { const float sc = (float)(sqrt(gp) / gs);
                for (uint32_t k = 0; k < c->channels; ++k) t[k] *= sc; }
        }
    }
}

/* Effective spread (shared by the panner and direct-binaural solves): the user's angular width,
 * floored by the source's METRIC size (the angle its radius subtends from the listener — physical
 * size stays constant as the listener walks, and a source that engulfs the listener goes fully
 * wide), by the engine-wide NEAR-LISTENER widening policy (rt_set_near_spread; a per-source size
 * subsumes it for sized sources), and by the ARRAY-HOLE policy (rt_set_hole_spread; hole.h — a
 * source with no speaker near its bearing is not a point and is floored wide). The floors compose
 * as a max: each states a width the render cannot honestly go below, so the widest one wins.
 * Sets v->spread_eff (the decor split follows it) and the anisotropy ratios v->ext_u/ext_w (an
 * extent carries a separate height: the physical floors apply to BOTH axes, the LARGER axis drives
 * the mode gate). Returns s_eff. */
static float solve_spread(RtCore* c, Voice* v) {
    float s_w = v->spread; if (s_w > 1.f) s_w = 1.f; else if (s_w < 0.f) s_w = 0.f;
    float s_h = v->spread_h; if (s_h > 1.f) s_h = 1.f;      /* < 0 = isotropic (follow the width) */
    const float nearR = atomic_load_explicit(&c->near_spread, memory_order_relaxed);
    /* the hole floor is a SPEAKER-ARRAY property: the direct-binaural render has no speakers and so
     * no holes, and its gain vector is an SH encode the array geometry never touches. */
    const float holeS = c->direct_on ? 0.f : c->hole_spread_blk;
    if (v->size_m > 0.f || nearR > 0.f || holeS > 0.f) {
        float dx = v->pos_active[0]-c->lis.p_active[0], dy = v->pos_active[1]-c->lis.p_active[1],
              dz = v->pos_active[2]-c->lis.p_active[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        float s_floor = 0.f;
        if (v->size_m > 0.f) {
            float ratio  = dist > 1e-6f ? v->size_m / dist : 2.f;
            float s_size = ratio >= 1.f ? 1.f : asinf(ratio) * 0.636619772f;   /* subtended half-angle / (pi/2) */
            if (s_size > s_floor) s_floor = s_size;
        }
        if (nearR > 0.f) {
            float s_near = 1.f - dist / nearR;
            if (s_near > 1.f) s_near = 1.f;
            if (s_near > s_floor) s_floor = s_near;
        }
        if (holeS > 0.f) {
            float u[3];
            unit_dir(c->lis.p_active, v->pos_active, u);        /* degenerate (at the listener) -> (0,0,1) */
            hole_block(&c->hole, &c->layout, c->lis.p_active, c->layout_gen);   /* cached; early-outs */
            float s_hole = holeS * hole_floor(&c->hole, u);
            if (s_hole > 1.f) s_hole = 1.f;
            if (s_hole > s_floor) s_floor = s_hole;
        }
        if (s_floor > s_w) s_w = s_floor;
        if (s_h >= 0.f && s_floor > s_h) s_h = s_floor;
    }
    float s_eff = (s_h > s_w) ? s_h : s_w;
    v->spread_eff = s_eff;
    v->ext_u = (s_eff > 1e-6f) ? s_w / s_eff : 1.f;         /* == 1 exactly when isotropic (s_w == s_eff) */
    v->ext_w = (s_eff > 1e-6f && s_h >= 0.f) ? s_h / s_eff : v->ext_u;
    return s_eff;
}

/* ACN channel -> SH degree l, order 3 (l = floor(sqrt(k))). */
static const int acn_deg[BWA_AMBI_CH] = { 0, 1,1,1, 2,2,2,2,2, 3,3,3,3,3,3,3 };

/* Direct-binaural gain solve (BWA_PROFILE_BINAURAL): the voice's gain vector becomes 16 SH
 * coefficients (phonon monitor basis, ambi_encode_phonon) at the TRUE direction from the live
 * listener to `pos`, times the same user gain and distance curve the speaker panner would apply —
 * so cave and binaural renders of one scene agree in loudness, and the gcur->gtarget machinery
 * ramps SH coefficients instead of speaker gains (invariant 4 holds unchanged). Head orientation
 * enters at the DECODE (phonon's orientation param / the fallback's ear vectors); the encode stays
 * room-frame. Spread tapers the high degrees toward omni (cos^l per degree), renormalized so the
 * SH-field energy matches the point encode — a widened source keeps its decoded loudness.
 * `v` supplies the per-voice attenuation override; NULL (the ISM images) takes the layout curve,
 * matching the speaker path where panner_gains bakes the layout curve into each image. */
/* distance attenuation + unit direction of the direct render, shared by the SH solve and the
 * mode-2 point tap (override-aware; NULL v = the layout curve, as the speaker path bakes it). */
static float direct_atten_dir(RtCore* c, const Voice* v, const float pos[3], float dir[3]) {
    float d[3] = { pos[0] - c->lis.p_active[0], pos[1] - c->lis.p_active[1],
                   pos[2] - c->lis.p_active[2] };
    float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    dir[0] = 0.f; dir[1] = 0.f; dir[2] = 1.f;               /* on the listener: room ahead */
    if (dist > 1e-6f) { dir[0] = d[0]/dist; dir[1] = d[1]/dist; dir[2] = d[2]/dist; }
    return (v && v->att_ref > 0.f)
         ? atten_curve(dist, v->att_ref, v->att_rolloff, v->att_min)
         : atten_curve(dist, c->layout.atten_ref_m, c->layout.atten_rolloff,
                       c->layout.atten_min_lin);
}

static void direct_gains_at(RtCore* c, const Voice* v, const float pos[3], float ug,
                            float spread, float* g) {
    float dir[3];
    const float att = direct_atten_dir(c, v, pos, dir);
    float y[BWA_AMBI_CH];
    ambi_encode_phonon(dir, y);
    if (spread > 1e-3f) {
        const float cw = cosf(spread * 1.5707963f);         /* 1 at point .. 0 (W-only) at full */
        const float wl[4] = { 1.f, cw, cw*cw, cw*cw*cw };
        double e = 0.0;
        for (int l = 0; l <= 3; ++l) e += (double)(2*l + 1) * wl[l] * wl[l];
        const float nrm = (float)sqrt(16.0 / e);            /* orthonormal-basis energy match */
        for (int k = 0; k < BWA_AMBI_CH; ++k) y[k] *= wl[acn_deg[k]] * nrm;
    }
    const float s = ug * att;
    for (int k = 0; k < BWA_AMBI_CH; ++k) g[k] = y[k] * s;
}

/* DIRECT output-channel route (rt_source_set_channel): install a ONE-HOT gain vector where the
 * panner's solve would go, instead of injecting after the output stage the way bwa_set_test_signal
 * does. Everything follows from that choice. The route is just another gain target, so switching in
 * and out RAMPS (invariant 4) instead of clicking; and align's per-speaker trims and delays, the room
 * EQ, the master gain and the limiter all still run, so the single-speaker reference is level- and
 * path-comparable with the phantom it is being A/B'd against. Nothing distance- or direction-derived
 * survives: no attenuation (the panner is not consulted at all), no spread/extent/size, no CAP, no
 * dual-band (gtarget_lo mirrors gtarget, which makes the mixer's LF re-weighting a no-op). mix_voice
 * suppresses the per-sample chain (occlusion, directivity, sends, propagation) to match.
 *
 * Under BWA_PROFILE_BINAURAL point voices never reach the bus, so "that channel" has to mean
 * something else: encode at the assigned SPEAKER's direction from the listener, dry. That is exactly
 * what the cave_sim monitor does with a bus channel, so the two headphone profiles agree. */
static void direct_channel_gains(RtCore* c, Voice* v, float ug) {
    memset(v->gtarget, 0, sizeof v->gtarget);
    v->spread_eff = 0.f;                 /* no widening, and the decor split follows spread_eff to 0 */
    v->ext_u = v->ext_w = 1.f;
    const uint32_t ch = v->out_ch < c->channels ? v->out_ch : 0u;   /* set-time clamped; belt and braces */
    if (c->direct_on) {
        float d[3] = { c->layout.speakers[ch].pos[0] - c->lis.p_active[0],
                       c->layout.speakers[ch].pos[1] - c->lis.p_active[1],
                       c->layout.speakers[ch].pos[2] - c->lis.p_active[2] };
        const float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        float dir[3] = { 0.f, 0.f, 1.f };
        if (dist > 1e-6f) { dir[0] = d[0]/dist; dir[1] = d[1]/dist; dir[2] = d[2]/dist; }
        memcpy(v->dir_active, dir, sizeof v->dir_active);
        if (c->direct_on == 2) {         /* per-voice HRTF: the whole voice rides its own point tap */
            v->gtarget[BWA_AMBI_CH] = ug;
        } else {
            float y[BWA_AMBI_CH];
            ambi_encode_phonon(dir, y);
            for (int k = 0; k < BWA_AMBI_CH; ++k) v->gtarget[k] = y[k] * ug;
        }
    } else {
        v->gtarget[ch] = ug;
    }
    if (v->fs_on) {                      /* spectral spread was engaged: retire every band onto this
                                          * target so the mixer hands back exactly (fs_on 2 -> 0) */
        for (int b = 0; b < BWA_FS_BANDS; ++b)
            for (uint32_t k = 0; k < c->channels; ++k) v->fs_t[b][k] = v->gtarget[k];
        v->fs_on = 2;
    }
    memcpy(v->gtarget_lo, v->gtarget, sizeof v->gtarget_lo);   /* dual-band reduces to a no-op */
}

/* DBAP gain solve (M4): listener-relative, dirty-gated. CMD_COMMIT re-dirties a voice on a
 * position change and dirties all voices on a listener move (gains are listener-relative). A bed
 * voice (multi-channel asset) has no DBAP position — its master gain rides gtarget[0]. */
static void compute_gains(RtCore* c, Voice* v) {
    const float ug = v->gain_user * c->group_gain[v->group];   /* mix-group gain folds into the solve */
    if (v->sound && v->sound->channels > 1) { v->gtarget[0] = ug; return; }   /* beds: no route to apply */
    if (v->out_ch_on) { direct_channel_gains(c, v, ug); return; }
    if (c->direct_on) {                                     /* direct-binaural: SH gains, not speaker
                                                             * gains. Extra listeners, dual-band, and
                                                             * the speaker spread modes don't apply —
                                                             * one head, one decode. */
        const float s_eff = solve_spread(c, v);
        if (c->direct_on == 2) {
            /* mode 2 (per-voice HRTF): power-split the dry by spread — the point share
             * sqrt(1-s) rides the per-voice tap (gtarget[BWA_AMBI_CH], a scalar the mixer ramps
             * like any gain), the wide share sqrt(s) takes the tapered field encode. Both paths
             * always exist, so a spread change crossfades through the solve instead of switching
             * render paths. The direction lands beside the gains (same dirty gating). */
            direct_gains_at(c, v, v->pos_active, ug * sqrtf(s_eff), s_eff, v->gtarget);
            v->gtarget[BWA_AMBI_CH] = ug * sqrtf(1.f - s_eff)
                                    * direct_atten_dir(c, v, v->pos_active, v->dir_active);
        } else {
            direct_gains_at(c, v, v->pos_active, ug, s_eff, v->gtarget);
        }
        return;
    }
    int p = atomic_load_explicit(&c->panner, memory_order_acquire);
    panner_gains(c, p, v->pos_active, ug, v->gtarget);

    /* multi-listener compromise (rt_set_extra_listeners): solve the same point for each extra
     * listener (each with its own SPCAP/VBAP cache) and take the per-speaker ENERGY MEAN — the L2
     * barycenter of the individual renderings. Constant power is preserved (the mean of the solves'
     * powers), and each occupant hears the image biased toward their own solve rather than one
     * person's being exact and the others' wrong. Spread/dual-band derive from the result. */
    const uint8_t nex = c->lis.nex_active;
    if (nex) {
        double acc[BWA_CHANNELS];
        for (uint32_t k = 0; k < c->channels; ++k) acc[k] = (double)v->gtarget[k] * v->gtarget[k];
        float gx[BWA_CHANNELS];
        for (uint8_t j = 0; j < nex; ++j) {
            panner_gains_at(c, p, c->lis.ex_active[j], &c->spcap_x[j], &c->vbap_x[j],
                            v->pos_active, ug, gx);
            for (uint32_t k = 0; k < c->channels; ++k) acc[k] += (double)gx[k] * gx[k];
        }
        const double inv = 1.0 / (double)(nex + 1);
        for (uint32_t k = 0; k < c->channels; ++k) v->gtarget[k] = (float)sqrt(acc[k] * inv);
    }

    /* per-source attenuation override (rt_source_set_attenuation): the panner baked the LAYOUT
     * curve into the solve (gains ∝ user_gain · atten · a unit-power distribution), so swap it by
     * RATIO — exact through any clamping, panner-agnostic, primary-listener distance like every
     * other distance effect. Everything downstream follows automatically: the spread modes
     * renormalize to this solve's power, the dual-band low band derives from it, the decor split
     * rides the gains, and loudness comp tracks the override's own curve (mix_voice). rolloff 0
     * gives a constant-level source (a direction-only cue that never fades). */
    if (v->att_ref > 0.f) {
        float dx = v->pos_active[0]-c->lis.p_active[0], dy = v->pos_active[1]-c->lis.p_active[1],
              dz = v->pos_active[2]-c->lis.p_active[2];
        float ds = sqrtf(dx*dx + dy*dy + dz*dz);
        float a_lay = atten_curve(ds, c->layout.atten_ref_m, c->layout.atten_rolloff, c->layout.atten_min_lin);
        float a_ovr = atten_curve(ds, v->att_ref, v->att_rolloff, v->att_min);
        if (a_lay > 1e-9f && a_ovr != a_lay) {
            const float r = a_ovr / a_lay;
            for (uint32_t k = 0; k < c->channels; ++k) v->gtarget[k] *= r;
        }
    }

    /* effective spread (solve_spread above): user width + metric-size / near-listener floors +
     * the anisotropy ratios. The decor split follows spread_eff, so the widened part decorrelates
     * too when enabled. */
    const float s_eff = solve_spread(c, v);
    const int smode = (s_eff > 1e-3f)                        /* widen the image if this source has size */
                    ? atomic_load_explicit(&c->spread_mode, memory_order_acquire) : -1;
    if      (smode == 2) fs_solve(c, v, p, s_eff, ug);       /* spectral: per-band targets; gtarget stays the point */
    else if (smode == 1) mdap_gains(c, p, v, s_eff, ug, v->gtarget);
    else if (smode == 0) spread_gains(c, v, s_eff, v->gtarget);
    if (v->fs_on && smode != 2) {
        /* the mode or the spread left spectral: aim every band at the (possibly widened) single-path
         * target and let the mixer hand back once they land (fs_on 2 -> 0, one block). */
        for (int b = 0; b < BWA_FS_BANDS; ++b)
            for (uint32_t k = 0; k < c->channels; ++k) v->fs_t[b][k] = v->gtarget[k];
        v->fs_on = 2;
    }

    /* dual-band low band: the SAME gain directions, renormalized to amplitude (pressure) sum instead of
     * power. The target sum is the power gains' OWN magnitude ||g||_2 (= gain_user * distance_atten, set
     * by the panner) — NOT bare gain_user, which would cancel the distance attenuation and leave a
     * distant source's bass at full level. So the LF coherent pressure sum matches the HF energy level
     * at every distance. Always computed (cheap) so dual_band A/Bs live; the mixer reads it only when on. */
    if (atomic_load_explicit(&c->cap_on, memory_order_acquire)) {
        /* CAP (cap.c): same band, same level convention, but the low gains are the point solve
         * PROJECTED so the rendered ITD matches a real source at this bearing, for the head's
         * CURRENT orientation. The correction fades out with spread — an engulfing source has no
         * one bearing to fix. Facing the source it is a no-op, so this reduces to the panner. */
        cap_block(&c->cap, &c->layout, c->lis.p_active, c->lis.q_active, c->layout_gen);
        float u[3];
        unit_dir(c->lis.p_active, v->pos_active, u);       /* degenerate (at the listener) -> (0,0,1) */
        cap_gains_lo(&c->cap, v->gtarget, u, 1.f - s_eff, v->gtarget_lo);
        return;
    }
    double gs = 0.0, gp = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) { gs += v->gtarget[k]; gp += (double)v->gtarget[k] * v->gtarget[k]; }
    if (gs > 1e-9) { float sc = (float)(sqrt(gp) / gs);
        for (uint32_t k = 0; k < c->channels; ++k) v->gtarget_lo[k] = v->gtarget[k] * sc; }
    else for (uint32_t k = 0; k < c->channels; ++k) v->gtarget_lo[k] = v->gtarget[k];
}

/* Mix one voice: read its sound at the cursor (looping or ending), spatialize through the
 * per-channel block-linear gcur->gtarget ramp (invariant 4), and accumulate into the bus.
 * Routing (gtarget) is still the M2 placeholder until M4. On a non-looping end the voice
 * stops; a oneshot additionally acks EVT_VOICE_ENDED so the control thread recycles its
 * transient handle. */
/* forward: the seek landing below resolves against the play region (defined with the mixers). */
static void resolve_loop_region(const Voice* v, uint32_t frames, uint32_t* lbeg, uint32_t* lend);

/* pause/seek gate for this block. Returns 0 when the voice is fully paused (playhead frozen, nothing
 * to mix); else fills the block's starting gate value + per-sample step. The gate ramps across one
 * whole block (invariant 4); a pending seek lands only once the gate is silent, so a seek on a
 * running voice is ramp-out -> jump -> ramp-in and never clicks. */
static int pause_gate(RtCore* c, Voice* v, uint16_t idx, uint32_t n, float* pg, float* pg_step) {
    float tgt = (v->paused || c->all_paused_blk || c->group_paused[v->group] ||
                 v->seek_pending || v->stopping) ? 0.f : 1.f;
    if (v->pause_g == 0.f && tgt == 0.f) {
        if (v->stopping) {                           /* faded to silence: finalize the stop/steal */
            uint8_t how = v->stopping; v->stopping = 0; v->playing = false;
            /* Free the slot for a steal (2), and for ANY stopped oneshot. A oneshot's handle is
             * engine-internal (bwa_play_oneshot returns nothing), so only an EVT_VOICE_ENDED ever
             * recycles it — the natural-end path does exactly this. Before bwa_group_stop /
             * bwa_stop_all no public call could stop one, so how == 1 on a oneshot was unreachable;
             * the voice-table sweeps reach it, and without this the slot would leak forever. */
            if (how == 2 || v->oneshot) {
                v->active = false;
                Evt ev = { .type = EVT_VOICE_ENDED, .handle = BWA_MK_H(idx, v->gen) };
                evt_push(&c->events, &ev);
            }
            return 0;
        }
        if (v->seek_pending) {                       /* land the seek while inaudible */
            const SoundData* snd = v->sound;
            /* The seek lands INSIDE the play region (rt_source_set_region), which with the default
             * region [0, frames) is the historical behavior unchanged: a loop wraps modulo the clip,
             * a one-shot lands on the end and finishes. A target below the region start clamps up to
             * it — the region is the voice's world, and a seek is a move within it, not out of it. */
            uint32_t lbeg, lend;
            resolve_loop_region(v, snd->frames, &lbeg, &lend);
            uint64_t f = v->seek_pos;
            if (snd->frames == 0) f = 0;
            else if (f < lbeg) f = lbeg;
            else if (f >= lend) f = (v->loop && lend > lbeg) ? lbeg + (f - lbeg) % (lend - lbeg) : lend;
            v->cursor = (uint32_t)f;
            v->cur_frac = 0.f;                       /* the fractional cursor lands with it */
            v->seek_pending = 0;
            if (!v->paused) tgt = 1.f;               /* not paused: ramp back in this block */
        }
        if (tgt == 0.f) return 0;                    /* paused: skip mixing, playhead stays frozen */
    }
    *pg      = v->pause_g;
    *pg_step = (tgt - v->pause_g) / (float)n;
    v->pause_g = tgt;                                /* land exactly (the loop advances a local copy) */
    return 1;
}

/* Resolve a voice's loop region [*lbeg,*lend) against its bound sound's frame count: loop_end 0 or
 * out of range = whole clip, and a degenerate beg >= end falls back to the whole clip too. For a
 * streamed voice `frames` is 0, which yields 0/0 — unused, since the stream path ignores the region. */
static void resolve_loop_region(const Voice* v, uint32_t frames, uint32_t* lbeg, uint32_t* lend) {
    uint32_t e = v->loop_end, b = v->loop_beg;
    if (e == 0 || e > frames) e = frames;
    if (b >= e) b = 0;
    *lbeg = b; *lend = e;
}

/* The most spans ONE seam crossing can cover through ordinary playback. The cursor advances by at
 * most the pitch ceiling (4.0, CMD_SET_PITCH's clamp) per sample and the seam is tested every
 * sample, so a crossing lands at most 3 frames past lend; over the shortest legal region (1 frame)
 * that is 4 spans. A larger skip did not come from playback. */
#define BWA_LOOP_ADVANCE_SPANS 4u

/* Wrap a looping cursor that has reached or overshot the region end, post the wrap notices, and
 * return the new cursor. Every mixer seam goes through here, so mix_voice and mix_bed cannot drift.
 * Callers guarantee lend > lbeg and cur >= lend.
 *
 * O(1) BY CONSTRUCTION. bwa_source_set_region can move lend an unbounded distance BELOW the live
 * cursor, so subtracting one span at a time would run (cur - lbeg) / span iterations inside a single
 * bufferSwitch — order 1e8 for an hour-long clip re-regioned to one frame, which is seconds of stall
 * on the audio thread. The modulo is the same answer in constant time.
 *
 * Notices are bounded for the same reason, and the bound is where the two cases part. Up to
 * BWA_LOOP_ADVANCE_SPANS the skip is playback, so every wrap is posted: bwa_poll_looped promises one
 * entry per wrap, and a region shorter than the pitch step really does wrap several times between
 * two seam tests. Beyond it the REGION moved under a cursor that was sitting still, which wrapped
 * nothing the caller can pace off, so it collapses to the ONE wrap the header already promises for
 * that case ("a cursor already past end_frame ends or wraps on the next block"). Millions of notices
 * for one set_region would flood the ring and the dropped counter and report a history that never
 * happened. */
static uint32_t loop_wrap(RtCore* c, const Voice* v, uint16_t idx, uint32_t cur, uint32_t lbeg, uint32_t lend) {
    const uint32_t span = lend - lbeg;
    const uint32_t over = cur - lbeg;                 /* >= span, so wraps >= 1 */
    uint32_t wraps = over / span;
    if (wraps > BWA_LOOP_ADVANCE_SPANS) wraps = 1;    /* a region change, not playback */
    for (uint32_t k = 0; k < wraps; ++k) loop_note(c, v, idx);
    return lbeg + over % span;
}

/* Gapless chaining: pop the next queued sound that still resolves to a playable in-memory mono asset,
 * skipping tombstones a retire left behind (NULL) or any incompatible entry, and set v->loop from it.
 * Returns NULL when the queue is exhausted (the voice then ends normally). Audio thread only. */
static const SoundData* queue_pop_valid(Voice* v) {
    while (v->queue_len > 0) {
        const SoundData* nx = v->queue[v->queue_head];
        uint8_t lp = v->queue_loop[v->queue_head];
        v->queue[v->queue_head] = NULL;
        v->queue_head = (v->queue_head + 1u) % BWA_QUEUE;
        v->queue_len--;
        if (nx && nx->pcm && nx->frames > 0 && nx->channels == 1 && !nx->stream) { v->loop = lp != 0; return nx; }
    }
    return NULL;
}

static void mix_voice(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n, uint32_t start, float* aux) {
    const SoundData* snd = v->sound;
    const uint32_t nr = n - start;      /* rendered samples this block: every per-sample ramp spans the AUDIBLE
                                         * part [start,n), so a scheduled start (start>0) lands its gains exactly
                                         * instead of snapping the unspent start/n fraction at the block end */
    float pg, pg_step;
    if (!pause_gate(c, v, idx, nr, &pg, &pg_step)) return;
    /* DIRECT output-channel route (compute_gains installed the one-hot vector): this voice is the
     * dry single-speaker REFERENCE, so every per-sample spatial and propagation stage below is
     * suppressed as well. Each suppression drives the stage's own TARGET rather than switching it
     * off, so entering and leaving the route ramps/glides like any other change and never steps a
     * filter or a delay line (invariant 4). What survives is the content itself: the pause gate,
     * the playback rate, and the per-source + group gain the one-hot vector carries. */
    const int dch = v->out_ch_on != 0;
    /* point-voice gain width: the speaker count, or BWA_AMBI_CH when direct-binaural routes this
     * voice onto the SH accumulator instead (`bus` here IS c->ambi_direct then — rt_render picks
     * the target). The gain arrays are BWA_CHANNELS-wide either way; nch == c->channels outside
     * direct mode, so nothing changes for the speaker render. */
    const uint32_t nch = c->mix_nch;
    float step[BWA_CHANNELS];
    for (uint32_t ch = 0; ch < nch; ++ch)
        step[ch] = (v->gtarget[ch] - v->gcur[ch]) / (float)nr;
    /* direct mode 2: the voice's point share renders into its OWN mono slot (per-voice HRTF needs
     * unsummed signals) with a scalar ramp — gcur[BWA_AMBI_CH], solved beside the SH gains and
     * landed with them. Published to the dv_view at the end of the mix (consumer: steam_decode). */
    const int pv = c->direct_blk && c->direct_on == 2;
    float* pv_slot = NULL;
    float  pv_g = 0.f, pv_step = 0.f;
    if (pv) {
        pv_slot = c->dv_mono + (size_t)idx * BWA_RT_MAX_BLOCK;
        if (start) memset(pv_slot, 0, sizeof(float) * start);   /* scheduled start: silent lead-in */
        pv_g = v->gcur[BWA_AMBI_CH];
        pv_step = (v->gtarget[BWA_AMBI_CH] - pv_g) / (float)nr;
    }
    /* dual-band panning: split each sample at BWA_DUALBAND_FC; the low band uses amplitude-normalized
     * gains (gcur_lo, better LF velocity vector), the high band the power gains. The complementary
     * 1st-order crossover (hi = s - lo) sums flat. dual = gcur*s + (gcur_lo-gcur)*lo, so a `dual_mix`
     * factor ramped 0<->1 on an A/B toggle CROSSFADES the LF re-weighting instead of stepping it
     * (invariant 4). The full path runs only while dual is on OR mid-crossfade; settled-single stays
     * cheap and keeps xover_lp at 0 for a clean re-enable. */
    const int dual = c->direct_on ? 0                       /* dual-band is a speaker-array concern:
                                                             * SH gains have no amplitude/power split */
                                  : atomic_load_explicit(&c->dual_band, memory_order_acquire);
    const float target_mix = dual ? 1.f : 0.f;
    const int use_dual = (v->dual_mix > 0.f) || dual;
    float dmix = v->dual_mix;
    const float dmix_step = (target_mix - v->dual_mix) / (float)nr;
    const float xover_a = c->xover_a;
    float step_lo[BWA_CHANNELS];
    if (use_dual) for (uint32_t ch = 0; ch < nch; ++ch)
        step_lo[ch] = (v->gtarget_lo[ch] - v->gcur_lo[ch]) / (float)nr;
    /* spectral widening (spread mode 2): while engaged the per-band gains REPLACE the single-path
     * output stage (dual-band included — its < 700 Hz share is folded into the band targets by
     * fs_solve). Bands ramp per sample like gcur (invariant 4); engage/retire hand off exactly. */
    const int fs = v->fs_on;
    float fs_step[BWA_FS_BANDS][BWA_CHANNELS];
    if (fs) for (int b = 0; b < BWA_FS_BANDS; ++b)
        for (uint32_t ch = 0; ch < nch; ++ch)
            fs_step[b][ch] = (v->fs_t[b][ch] - v->fs_g[b][ch]) / (float)nr;
    /* gate the sim's publish on our own generation (we own v->gen, so this is race-free): apply the
     * published transmittance only if it was published for THIS occupant, else treat as clear. Read
     * once into a local so the ramp aims at and lands on the same value (invariant 4 — no jump). */
    const uint32_t myh = BWA_MK_H(idx, v->gen);
    const bool mine = atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == myh;
    const float occ_tgt = (mine && !dch) ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.0f;
    const float occ_step = (occ_tgt - v->occ_cur) / (float)nr;   /* occlusion ramp (invariant 4) */

    /* per-band EQ: read the gated tilt once + glide the band gains; compute this block's TARGET
     * biquad coeffs and interpolate the live coeffs toward them per sample, so the spectral envelope
     * never steps at a block boundary (invariant 4). Target is passthrough when flat; the chain is
     * bypassed once it has fully settled flat. eq_block_* share this with the pathing EQ below. */
    float gt[3];
    eq_unpack((mine && !dch) ? atomic_load_explicit(&c->occ_eq[idx], memory_order_relaxed) : EQ_FLAT, gt);
    float co_tgt[3][5], co_step[3][5];
    int flat = eq_block_setup(&v->eq, gt, c->eq_proto, nr, co_tgt, co_step);

    /* propagation (opt-in): a distance-driven air-absorption low-pass + a glided Doppler delay line.
     * Both ride the block: the air coeff ramps (invariant 4); the Doppler delay glides toward
     * distance/c and the glide rate IS the pitch shift. Indices stay integer (the ring is masked,
     * the delay's frac is a separate small float) so a long-lived voice never loses sample precision. */
    const int dir_manual = !mine && !dch && v->dir_w > 0.f;   /* manual dipole: only while no sim publish owns the voice */
    float dist = 0.f, slx = 0.f, sly = 0.f, slz = 0.f;             /* source -> listener */
    if (v->air_on || v->dop_on || v->ldc_on || v->nf_on || dir_manual || (v->refl_send && v->refl_dist)) {
        slx = c->lis.p_active[0]-v->pos_active[0]; sly = c->lis.p_active[1]-v->pos_active[1]; slz = c->lis.p_active[2]-v->pos_active[2];
        dist = sqrtf(slx*slx + sly*sly + slz*slz);
    }
    /* directivity (source-radiation gain): own ramp — it tracks source/listener motion, so a raw
     * per-block jump would zipper (invariant 4). The sim's publish wins while it owns the voice
     * (gated on the same handle); otherwise a MANUAL pattern (CMD_SET_DIR) evaluates right here —
     * phonon's weighted dipole, |(1-w) + w cos|^p toward the active listener — so builds without
     * the sim get walk-correct directivity, per block, from pure math. */
    float dir_tgt = 1.0f;
    if (mine && !dch) dir_tgt = atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed);
    else if (dir_manual && dist > 1e-4f) {
        const float cosb = (v->dir_fwd[0]*slx + v->dir_fwd[1]*sly + v->dir_fwd[2]*slz) / dist;
        dir_tgt = powf(fabsf(1.f - v->dir_w + v->dir_w * cosb), v->dir_pow);
    }
    if (dir_manual) {                                            /* readback publish (rt_get_directivity) */
        uint32_t fb; memcpy(&fb, &dir_tgt, sizeof fb);
        atomic_store_explicit(&c->dir_pub[idx], ((uint64_t)myh << 32) | fb, memory_order_relaxed);
    }
    const float dir_step = (dir_tgt - v->dir_cur) / (float)nr;
    /* engine-wide speed of sound (live, atomic): Doppler + ISM delays derive from it this block */
    const float sos = atomic_load_explicit(&c->sos, memory_order_relaxed);
    /* reverb wet-send level: refl_gain, optionally scaled by distance (near = drier, far = wetter); ramped
     * (so motion + on/off don't zipper the send). do_send keeps ramping a just-disabled voice down to 0. */
    float refl_tgt = 0.f;
    if (aux && v->refl_send && !dch) {
        refl_tgt = v->refl_gain;
        if (v->refl_dist) {
            float t = (dist - BWA_REFL_NEAR_DIST) / (BWA_REFL_FAR_DIST - BWA_REFL_NEAR_DIST);
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            refl_tgt *= BWA_REFL_NEAR_SEND + (1.f - BWA_REFL_NEAR_SEND) * t;
        }
    }
    const float refl_step = (refl_tgt - v->refl_g_cur) / (float)nr;
    const bool do_send = aux && (v->refl_send || v->refl_g_cur > 1e-6f);
    float air_a_tgt = 1.f, air_a_step = 0.f;
    if (v->air_on) {
        float fc = BWA_AIR_FC_NEAR - (dch ? 0.f : dist) * BWA_AIR_FC_PER_M;   /* routed: glide to the near (transparent) end */
        if (fc < BWA_AIR_FC_FLOOR) fc = BWA_AIR_FC_FLOOR;
        air_a_tgt = 1.f - expf(-6.28318530718f * fc / (float)c->sample_rate);
        if (air_a_tgt > 1.f) air_a_tgt = 1.f;
        air_a_step = (air_a_tgt - v->air_a_cur) / (float)nr;
    }
    /* equal-loudness distance compensation (opt-in): as the panner's distance attenuation takes
     * level away, the ear also loses LF sensitivity (ISO 226), so an attenuated source reads THIN,
     * not just far. Restore part of the body with a one-pole LF shelf whose boost tracks the same
     * attenuation curve the panner applied: +0.4 dB of shelf per dB of attenuation, capped +8 dB.
     * Ramped (invariant 4); ramps back to flat after opt-out. Direct path only, like air/Doppler. */
    float ldc_tgt = 1.f, ldc_step = 0.f;
    if (v->ldc_on) {
        const Layout* L = &c->layout;
        /* track the curve the solve actually applied: the per-source override when set (so the
         * two compose — a constant-level source gets no compensation), else the layout's */
        const float ldc_d = dch ? 0.f : dist;             /* routed: zero distance = no attenuation = flat shelf */
        float att = (v->att_ref > 0.f)
                  ? atten_curve(ldc_d, v->att_ref, v->att_rolloff, v->att_min)
                  : atten_curve(ldc_d, L->atten_ref_m, L->atten_rolloff, L->atten_min_lin);
        float sh_db = -20.f * log10f(att) * 0.4f;
        if (sh_db > 8.f) sh_db = 8.f; else if (sh_db < 0.f) sh_db = 0.f;
        ldc_tgt = powf(10.f, sh_db / 20.f);
    }
    const int use_ldc = v->ldc_on || v->ldc_g_cur > 1.f + 1e-4f;   /* keep ramping a just-disabled voice flat */
    if (use_ldc) ldc_step = (ldc_tgt - v->ldc_g_cur) / (float)nr;
    /* near-field proximity boost (opt-in): the LF shelf rises linearly to BWA_NF_MAX_DB as the
     * source closes from BWA_NF_RADIUS to the head — the spherical-wavefront proximity effect,
     * the near-distance mirror of the loudness-comp shelf above (that one restores body FAR away).
     * Ramped (invariant 4); ramps back to flat after opt-out. Direct path only, like air/Doppler. */
    float nf_tgt = 1.f, nf_step = 0.f;
    if (v->nf_on && !dch && dist < BWA_NF_RADIUS) {
        const float nf_db = BWA_NF_MAX_DB * (1.f - dist / BWA_NF_RADIUS);
        nf_tgt = powf(10.f, nf_db / 20.f);
    }
    const int use_nf = v->nf_on || v->nf_g_cur > 1.f + 1e-4f;      /* keep ramping a just-disabled voice flat */
    if (use_nf) nf_step = (nf_tgt - v->nf_g_cur) / (float)nr;
    float dop_ds = 0.f, dop_k = 0.f, *dring = NULL; uint32_t dmask = 0;
    if (v->dop_on && c->dop_ring) {
        dop_ds = (dch ? 0.f : dist) / sos * (float)c->sample_rate;      /* raw propagation delay (samples;
                                                                         * routed: glide the line out to 0
                                                                         * rather than dropping it, which
                                                                         * would step the waveform) */
        float maxd = (float)(c->dop_ringlen - 2);          /* keep both interpolation taps in-ring */
        if (dop_ds > maxd) dop_ds = maxd;
        if (v->dop_init) { v->dop_delay = dop_ds; v->dop_dtgt = dop_ds; v->dop_init = false; }  /* snap: no enable glitch */
        /* The read delay is low-passed toward distance/c PER SAMPLE with a 2-pole filter (target then
         * delay, BWA_DOPPLER_TAU each). Position is committed per video frame (~60 Hz) but we render many
         * samples per frame, so the raw target is a staircase; its fundamental (the commit rate) would
         * FM-modulate the carrier into audible sidebands (worse at HF). The cutoff (~5 Hz, BWA_DOPPLER_TAU
         * = 32 ms) was set with test_doppler_fft: it keeps the commit-rate sidebands below ~ -45 dB at
         * 1 kHz / -38 dB at 4 kHz. Low-passing the delay preserves the ramp's SLOPE, so steady-motion
         * pitch is exact; it only offsets the delay value by ~v*group_delay (sub-millisecond), and
         * rounds pitch transitions over the group delay (natural). A plain velocity tracker is worse
         * here (it overshoots each staircase step). */
        dop_k = 1.f / (BWA_DOPPLER_TAU * (float)c->sample_rate);
        if (dop_k > 0.5f) dop_k = 0.5f;
        dring = c->dop_ring + (size_t)idx * c->dop_ringlen; dmask = c->dop_ringlen - 1;
    }

    /* decorrelated wide-part routing (bwa_set_decorrelation): a spread source's energy splits into a
     * coherent copy on the main bus and an incoherent copy on the decor bus (per-channel velvet
     * filters, convolved in rt_render) — the same wide gain DISTRIBUTION, but the speaker feeds
     * decorrelate, so the extent stops collapsing to phantom images / comb-filtering as the tracked
     * listener walks. dc_amp = sqrt(spread·toggle) ramps per sample (invariant 4); power is conserved
     * because incoherent energy ADDS: coherent² + decor² = 1. A just-toggled-off voice keeps writing
     * while dc_amp ramps out. */
    float dc_a = v->dc_amp, dc_step = 0.f;
    int use_dc = 0;
    {
        const int dc_on = c->direct_on ? 0 : c->dc_on_blk;   /* the block's single load (rt_render);
                                                              * decor is per-SPEAKER velvet filters —
                                                              * meaningless on SH channels (direct) */
        const float sp = v->spread_eff;                   /* the SOLVED width (user + near-listener floor) */
        const float dc_tgt = dc_on ? sqrtf(sp) : 0.f;
        use_dc  = (dc_tgt > 0.f || v->dc_amp > 0.f);
        dc_step = (dc_tgt - v->dc_amp) / (float)nr;
        v->dc_amp = dc_tgt;                       /* land exactly (the loop advances the local) */
        if (use_dc) c->dc_wrote = 1;
    }

    /* pitch (in-memory sounds only): fractional-cursor linear-interp resample. The rate glides per
     * sample so a change BENDS the pitch rather than stepping it; settled at exactly 1 the integer
     * path runs untouched. Streams can't resample (the ring is sequential) — pitch is ignored there. */
    const int streaming_pre = (snd->stream != NULL);
    const int use_pitch = !streaming_pre && (v->pitch != 1.f || v->pitch_cur != 1.f);
    float pit = v->pitch_cur;
    const float pit_step = use_pitch ? (v->pitch - v->pitch_cur) / (float)nr : 0.f;

    /* image-source early reflections: solve this block's six images (positions from the room + the
     * source; ism.c), then per image derive the target delay (path/c), the per-band reflection
     * coefficient, and the speaker gains from the panner AT THE IMAGE POSITION — so each reflection
     * is a real point source, with the panner's own distance attenuation and the listener-relative
     * direction (a walked reflection changes direction, as it must). Delays glide (BWA_ISM_TAU) and
     * gains ramp per sample: motion bends the reflections, never steps them. */
    const int ism_want = v->ism_on && !dch && c->ism_room.valid && c->ism_ring;
    const int ism_on   = ism_want || v->ism_tail;      /* a just-disabled voice ramps its reflections out */
    int   ism_n = 0;
    float ism_gtgt[ISM_IMAGES][BWA_CHANNELS], ism_gstep[ISM_IMAGES][BWA_CHANNELS];
    float ism_dtgt[ISM_IMAGES], ism_a[ISM_IMAGES];
    float ism_k = 0.f;
    float *iring = NULL; uint32_t imask = 0;
    if (ism_on) {
        IsmImage img[ISM_IMAGES];
        const int nimg = ism_want ? ism_images(&c->ism_room, v->pos_active, img) : 0;   /* 0 = outside the room */
        const float scale = atomic_load_explicit(&c->ism_gain, memory_order_relaxed);
        ism_n = ISM_IMAGES;                                      /* every slot ramps: a dropped image fades out */
        ism_k = 1.f / (BWA_ISM_TAU * (float)c->sample_rate);      /* per-sample delay glide */
        if (ism_k > 0.5f) ism_k = 0.5f;
        const int p = atomic_load_explicit(&c->panner, memory_order_acquire);
        const float maxd = (float)(c->ism_ringlen - 2);          /* keep both interpolation taps in-ring */
        for (int m = 0; m < ISM_IMAGES; ++m) {
            if (m >= nimg) {                                     /* disabled / outside the room: fade this slot */
                ism_dtgt[m] = v->ism_delay[m]; ism_a[m] = 1.f;
                for (uint32_t k = 0; k < nch; ++k) {
                    ism_gtgt[m][k]  = 0.f;
                    ism_gstep[m][k] = -v->ism_g[m][k] / (float)nr;
                }
                continue;
            }
            float dx = img[m].pos[0]-c->lis.p_active[0], dy = img[m].pos[1]-c->lis.p_active[1],
                  dz = img[m].pos[2]-c->lis.p_active[2];
            float path = sqrtf(dx*dx + dy*dy + dz*dz);       /* the reflection's path length to the ears */
            float dtg = path / sos * (float)c->sample_rate;
            ism_dtgt[m] = dtg > maxd ? maxd : dtg;
            if (v->ism_init) v->ism_delay[m] = ism_dtgt[m];      /* fresh enable: snap (a glide from 0 would sweep) */
            /* the panner gives direction + its own distance attenuation; the mid-band coefficient is
             * the broadband level, and the high-vs-mid ratio becomes a one-pole HF damping (a wall
             * absorbs treble harder — why a reflection sounds duller than the direct sound). In
             * direct-binaural mode each image SH-encodes at its own true direction instead — the
             * same point-source treatment, headphone-rendered (v = NULL: images take the layout
             * curve, as the speaker path bakes it). */
            if (c->direct_on)
                direct_gains_at(c, NULL, img[m].pos,
                                v->gain_user * c->group_gain[v->group] * img[m].refl[1] * scale,
                                0.f, ism_gtgt[m]);
            else
                panner_gains(c, p, img[m].pos, v->gain_user * c->group_gain[v->group] * img[m].refl[1] * scale,
                             ism_gtgt[m]);
            float hf = fabsf(img[m].refl[1]) > 1e-6f ? img[m].refl[2] / img[m].refl[1] : 1.f;   /* fabsf: a
                                                                 * pressure-release face negates BOTH bands —
                                                                 * the damping ratio must stay positive */
            if (hf > 1.f) hf = 1.f; else if (hf < 0.02f) hf = 0.02f;
            ism_a[m] = hf;                                       /* 1 = no damping .. 0 = fully dull */
            for (uint32_t k = 0; k < nch; ++k)
                ism_gstep[m][k] = (ism_gtgt[m][k] - v->ism_g[m][k]) / (float)nr;
        }
        v->ism_init = false;
        iring = c->ism_ring + (size_t)idx * c->ism_ringlen;
        imask = c->ism_ringlen - 1;
    }

    /* streaming sounds: pull this block's mono samples from the background ring (no I/O on the audio
     * thread). want covers [start, n); a short pull (underrun or EOF) leaves the tail silent. */
    const int streaming = streaming_pre;
    uint32_t strm_got = 0;
    if (streaming) {
        uint32_t want = (start < n) ? (n - start) : 0;
        strm_got = stream_pull(snd->stream, v->stream_pos, c->stream_scratch + start, want);
        for (uint32_t i = start + strm_got; i < n; ++i) c->stream_scratch[i] = 0.f;
        /* A short pull is either the end of the asset or a STARVE. Only the second is a fault, and
         * telling them apart is the whole value of the counter: the silence sounds identical. */
        if (strm_got < want && !stream_ended(snd->stream, v->stream_pos + strm_got))
            atomic_fetch_add_explicit(&c->strm_starves, 1, memory_order_relaxed);
    }

    /* pathing: SH-encode the UN-occluded source (the indirect path goes around the occluder, so it is
     * not occluded by the direct-path occlusion) into the shared ambisonic accumulator. Read the sim's
     * published field (handle-gated double buffer) once and ramp toward it (invariant 4). */
    const int path_on = v->path_on && c->path_tap && c->path_ambi_ch;
    const uint32_t pac = c->path_ambi_ch;
    float path_tgt[BWA_AMBI_CH], path_step[BWA_AMBI_CH];
    float pco_tgt[3][5], pco_step[3][5];      /* pathing bending-loss EQ: block target coeffs + per-sample glide */
    int   path_flat = 1;
    if (path_on) {
        int pf = atomic_load_explicit(&c->path_idx[idx], memory_order_acquire);
        const PathPub* pp = &c->path_pub[(size_t)idx * 2 + (size_t)pf];
        const int mine_path = (pp->handle == myh) && !dch;   /* routed: ramp the indirect field out */
        for (uint32_t k = 0; k < pac; ++k) path_tgt[k] = mine_path ? pp->sh[k] : 0.f;
        for (uint32_t k = 0; k < pac; ++k) path_step[k] = (path_tgt[k] - v->path_sh_cur[k]) / (float)nr;
        /* bending-loss EQ: the same low-shelf/peak/high-shelf cascade the occlusion EQ uses, applied
         * to s_raw pre-encode (phonon's own path effect: EQ the mono signal, then scale each SH
         * channel — path_effect.cpp). Target is flat when the field isn't ours. Same eq_block_setup. */
        float peq_tgt[3];
        for (int b = 0; b < 3; ++b) peq_tgt[b] = mine_path ? pp->eq[b] : 1.f;
        path_flat = eq_block_setup(&v->path_eq, peq_tgt, c->eq_proto, nr, pco_tgt, pco_step);
    }

    uint32_t cur = v->cursor;
    /* loop region [lbeg,lend): on reaching lend the cursor wraps to lbeg (not the clip end) — the
     * intro->loop pattern. Streaming voices ignore it (frames unknown; the ring is sequential). */
    uint32_t lbeg, lend;
    resolve_loop_region(v, streaming ? 0u : snd->frames, &lbeg, &lend);
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (i < start) continue;               /* scheduled start: this voice stays silent (and frozen) until the in-block offset */
        if (!streaming && cur >= lend) {
            if (v->loop && lend > lbeg) cur = loop_wrap(c, v, idx, cur, lbeg, lend);   /* pitch can overshoot the
                                                                                        * seam, and a set_region can
                                                                                        * leave cur far past it */
            else {
                const SoundData* nx = v->stopping ? NULL : queue_pop_valid(v);   /* never chain while stopping/
                                                             * fading/steal-fading — the voice is on its way out */
                if (nx) { snd = nx; v->sound = nx; cur = 0; v->cur_frac = 0.f; lbeg = 0; lend = snd->frames; }
                else ended = true;                          /* queue empty (or stopping): natural end */
            }
        }
        float s;
        if (streaming)      s = c->stream_scratch[i];
        else if (ended)     s = 0.f;
        else if (use_pitch) {                  /* linear interp between cur and its successor */
            uint32_t i2 = cur + 1;
            if (v->loop) { if (i2 >= lend) i2 = lbeg; }      /* wrap the interp partner to the loop start */
            else if (i2 >= snd->frames) i2 = snd->frames - 1;
            s = snd->pcm[cur] + v->cur_frac * (snd->pcm[i2] - snd->pcm[cur]);
        } else s = snd->pcm[cur];
        s *= pg;                                               /* pause/seek gate (also gates the sends below) */
        const float s_raw = s;                                 /* pre-occlusion source, for the indirect (pathing) field */
        if (v->eq.engaged) s = eq_block_apply(&v->eq, s, co_step);   /* 3 biquads (DF-I), coeffs interpolated per sample */
        s *= v->occ_cur * v->dir_cur;                           /* occlusion level + directivity, pre-pan */
        if (do_send) { aux[i] += s * v->refl_g_cur; v->refl_g_cur += refl_step; }  /* reverb send: pre-propagation, distance/level-scaled */
        if (ism_on) {                                           /* early reflections: each image is a delayed,
                                                                 * damped, PANNED copy of the source (the
                                                                 * reflections carry their own propagation, so
                                                                 * they tap s BEFORE Doppler/air, like the send) */
            iring[v->ism_w & imask] = s;
            for (int m = 0; m < ism_n; ++m) {
                uint32_t di = (uint32_t)v->ism_delay[m];        /* integer part; the read index stays integer */
                float    df = v->ism_delay[m] - (float)di;
                float newer = iring[(v->ism_w - di)     & imask];
                float older = iring[(v->ism_w - di - 1) & imask];
                float r = newer * (1.f - df) + older * df;      /* fractional read: no zipper as the path glides */
                v->ism_lp[m] += ism_a[m] * (r - v->ism_lp[m]);  /* wall HF damping (one-pole) */
                r = v->ism_lp[m];
                for (uint32_t ch = 0; ch < nch; ++ch) {
                    bus[(size_t)ch * n + i] += v->ism_g[m][ch] * r;
                    v->ism_g[m][ch] += ism_gstep[m][ch];
                }
                v->ism_delay[m] += (ism_dtgt[m] - v->ism_delay[m]) * ism_k;   /* glide toward the new path */
            }
            v->ism_w++;
        }
        if (v->air_on) {                                        /* air absorption: distance one-pole LPF (direct path) */
            v->air_y1 += v->air_a_cur * (s - v->air_y1); s = v->air_y1; v->air_a_cur += air_a_step;
        }
        if (use_ldc) {                                          /* loudness comp: one-pole LF shelf (direct path) */
            v->ldc_lp += c->ldc_a * (s - v->ldc_lp);
            s += (v->ldc_g_cur - 1.f) * v->ldc_lp;
            v->ldc_g_cur += ldc_step;
        }
        if (use_nf) {                                           /* near-field proximity: one-pole LF shelf (direct path) */
            v->nf_lp += c->nf_a * (s - v->nf_lp);
            s += (v->nf_g_cur - 1.f) * v->nf_lp;
            v->nf_g_cur += nf_step;
        }
        if (dring) {                                            /* Doppler: write, read at the gliding fractional delay */
            dring[v->dop_w & dmask] = s;
            uint32_t di = (uint32_t)v->dop_delay;               /* integer delay; (dop_w-di) stays integer (no float index) */
            float    df = v->dop_delay - (float)di;             /* fractional delay [0,1) */
            float newer = dring[(v->dop_w - di)     & dmask];   /* di samples ago */
            float older = dring[(v->dop_w - di - 1) & dmask];   /* di+1 samples ago */
            s = newer * (1.f - df) + older * df;
            v->dop_w++;
            v->dop_dtgt  += (dop_ds      - v->dop_dtgt)  * dop_k;   /* 2-pole, per sample: smooth the target ... */
            v->dop_delay += (v->dop_dtgt - v->dop_delay) * dop_k;   /* ... then the read delay -> continuous rate */
        }
        if (!streaming && !ended) {
            if (use_pitch) {
                v->cur_frac += pit; pit += pit_step;
                while (v->cur_frac >= 1.f) { v->cur_frac -= 1.f; ++cur; }
            } else ++cur;
        }
        pg += pg_step;
        v->occ_cur += occ_step;
        v->dir_cur += dir_step;
        if (path_on) {                                         /* SH-encode the indirect field (decoded later by the tap) */
            float sp = s_raw;                                  /* the indirect path is un-occluded (it bends around) ... */
            if (v->path_eq.engaged) sp = eq_block_apply(&v->path_eq, sp, pco_step);   /* ... but takes the bending-loss tilt */
            for (uint32_t k = 0; k < pac; ++k) {
                c->path_accum[(size_t)k * n + i] += sp * v->path_sh_cur[k];
                v->path_sh_cur[k] += path_step[k];
            }
        }
        if (use_dc) {                                          /* decor split: incoherent share to dc_bus ... */
            const float sd = s * dc_a;
            const float cs = 1.f - dc_a * dc_a;                /* ... coherent share stays on the main path */
            s *= cs > 0.f ? sqrtf(cs) : 0.f;                   /* (ramp float error can graze cs < 0) */
            const float* dcg = fs ? v->fs_g[0] : v->gcur;      /* spectral mode: band 0 IS the source direction */
            for (uint32_t ch = 0; ch < nch; ++ch)              /* (never direct: dc_on gated above) */
                c->dc_bus[(size_t)ch * n + i] += dcg[ch] * sd;
            dc_a += dc_step;
        }
        if (fs) {                                              /* spectral widening: band-split, each band on its
                                                                * own gain vector (complementary one-poles sum to
                                                                * s, so equal band gains == the single path) */
            float bnd[BWA_FS_BANDS], prev = 0.f;
            for (int x = 0; x < BWA_FS_XOVERS; ++x) {
                v->fs_lp[x] += c->fs_xa[x] * (s - v->fs_lp[x]);
                bnd[x] = v->fs_lp[x] - prev; prev = v->fs_lp[x];
            }
            bnd[BWA_FS_XOVERS] = s - prev;
            for (uint32_t ch = 0; ch < nch; ++ch) {
                float acc = 0.f;
                for (int b = 0; b < BWA_FS_BANDS; ++b) {
                    acc += v->fs_g[b][ch] * bnd[b];
                    v->fs_g[b][ch] += fs_step[b][ch];
                }
                bus[(size_t)ch * n + i] += acc;
            }
        } else if (use_dual) {
            float lo = v->xover_lp + xover_a * (s - v->xover_lp); v->xover_lp = lo;   /* LP @ 700 Hz */
            for (uint32_t ch = 0; ch < nch; ++ch) {                                   /* single + dmix-scaled LF re-weight */
                bus[(size_t)ch * n + i] += v->gcur[ch] * s + dmix * (v->gcur_lo[ch] - v->gcur[ch]) * lo;
                v->gcur_lo[ch] += step_lo[ch]; v->gcur[ch] += step[ch];
            }
            dmix += dmix_step;
        } else {
            for (uint32_t ch = 0; ch < nch; ++ch) {
                bus[(size_t)ch * n + i] += v->gcur[ch] * s;
                v->gcur[ch] += step[ch];
            }
        }
        if (pv_slot) { pv_slot[i] = pv_g * s; pv_g += pv_step; }   /* the point share (own slot: =, not +=) */
    }
    v->cursor = cur;
    if (ism_on) {                                               /* land the image gains exactly (invariant 4) */
        for (int m = 0; m < ism_n; ++m)
            for (uint32_t ch = 0; ch < nch; ++ch) v->ism_g[m][ch] = ism_gtgt[m][ch];
        v->ism_tail = 0;                                        /* the targets above were 0 when !ism_want, so
                                                                 * one block of ramp-out finishes the fade */
    }
    if (use_pitch) v->pitch_cur = v->pitch;                     /* land the rate glide exactly */
    if (path_on) for (uint32_t k = 0; k < pac; ++k) v->path_sh_cur[k] = path_tgt[k];   /* land exactly */
    if (path_on && v->path_eq.engaged) eq_block_land(&v->path_eq, pco_tgt, path_flat);   /* land pathing-EQ coeffs; bypass once flat */
    if (streaming) {                                            /* advance the stream position; end at a true EOF (not underrun) */
        v->stream_pos += strm_got;
        if (stream_ended(snd->stream, v->stream_pos)) ended = true;
    }
    v->occ_cur = occ_tgt;                                        /* land exactly (same local) */
    v->dir_cur = dir_tgt;
    if (v->air_on) v->air_a_cur = air_a_tgt;                     /* land the ramped propagation params */
    if (use_ldc) { v->ldc_g_cur = ldc_tgt;                       /* land the shelf; reset once settled flat */
                   if (!v->ldc_on && ldc_tgt == 1.f) v->ldc_lp = 0.f; }
    if (use_nf)  { v->nf_g_cur = nf_tgt;                         /* land the near-field shelf the same way */
                   if (!v->nf_on && nf_tgt == 1.f) v->nf_lp = 0.f; }
    if (do_send)   v->refl_g_cur = refl_tgt;                     /* (the Doppler delay self-tracks per sample) */
    if (use_dual) {
        for (uint32_t ch = 0; ch < nch; ++ch) v->gcur_lo[ch] = v->gtarget_lo[ch];  /* land lo band */
        v->dual_mix = target_mix;                                    /* land the crossfade factor */
        if (target_mix == 0.f) v->xover_lp = 0.f;                    /* settled single next block: clean LP restart */
    }
    if (fs) {
        for (int b = 0; b < BWA_FS_BANDS; ++b)                       /* land the band gains exactly */
            for (uint32_t ch = 0; ch < nch; ++ch) v->fs_g[b][ch] = v->fs_t[b][ch];
        if (v->fs_on == 2) v->fs_on = 0;   /* retiring: every band landed on the single-path target, and
                                            * gcur lands on the same gtarget below — exact handoff */
    }
    if (v->eq.engaged) eq_block_land(&v->eq, co_tgt, flat);      /* land occlusion-EQ coeffs; bypass once settled flat */
    for (uint32_t ch = 0; ch < nch; ++ch) v->gcur[ch] = v->gtarget[ch]; /* land exactly */
    if (pv) {
        v->gcur[BWA_AMBI_CH] = v->gtarget[BWA_AMBI_CH];          /* land the point-share scalar */
        RtDirectVoice* dv = &c->dv_view[idx];                    /* publish this block's point tap */
        dv->mono = pv_slot;
        memcpy(dv->dir, v->dir_active, sizeof dv->dir);
        dv->gen = v->gen;
        dv->active = 1;
    }

    if (ended) {
        v->playing = false;
        if (!v->oneshot) {                     /* a oneshot has no caller handle to report AGAINST:
                                                * rt_play_oneshot never bumps play_seq, so its 0 would
                                                * also sail through the drain's 0 == 0 seq gate. */
            Evt done = { .type = EVT_VOICE_DONE, .seq = v->play_seq, .handle = BWA_MK_H(idx, v->gen) };
            evt_push_notice(c, &done, &c->done_dropped);   /* best-effort; never displaces an ownership ack */
        }
        if (v->oneshot) {
            v->active = false;                 /* transient voice is finished */
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BWA_MK_H(idx, v->gen) };
            evt_push(&c->events, &ev);
        }
    }
}

/* Rotate real ACN/SN3D SH coefficients about the vertical (ambi z / room +y) axis: each degree's
 * ±m pair rotates as a 2D vector by m*yaw (the closed form yaw-only rotation — no Wigner matrices).
 * cm/sm are cos/sin of {yaw, 2yaw, 3yaw}; positive yaw turns the FIELD from room +z (front) toward
 * room +x. Zonal (m = 0) channels are untouched. */
static void bed_rotate_z(const float* sh, int nch, const float cm[3], const float sm[3], float* out) {
    for (int k = 0; k < nch; ++k) out[k] = sh[k];
    const int maxl = nch >= 16 ? 3 : (nch >= 9 ? 2 : 1);
    for (int l = 1; l <= maxl; ++l)
        for (int m = 1; m <= l; ++m) {
            const int kp = l*l + l + m, kn = l*l + l - m;   /* cos(m·az) / sin(m·az) components */
            const float cp = sh[kp], cn = sh[kn];
            /* a source at azimuth a moves to a + yaw: cos(m(a+yaw)) / sin(m(a+yaw)) expansions */
            out[kp] = cm[m-1] * cp - sm[m-1] * cn;
            out[kn] = sm[m-1] * cp + cm[m-1] * cn;
        }
}

/* Room-frame bed orientation -> the ambi-axes FIELD rotation matrix (rt_bed_set_orientation). Yaw
 * about room up (+y; positive turns the field from +z toward +x — the yaw-only orientation
 * convention), pitch about the room's right axis (positive tilts the field's front upward), roll
 * about room forward (+z; positive tilts the field's top toward room -x = the room's right).
 * Applied roll-first, yaw-last (aircraft order), then conjugated into ambi axes (x=front=room z,
 * y=left=room x, z=up=room y) — for a pure permutation that is an index remap through the shared
 * room->ambi gather table (BWA_ROOM2AMBI, ambisonics.h). */
static void bed_rot_ambi(float yaw, float pitch, float roll, float R[3][3]) {
    const float cy = cosf(yaw),  sy = sinf(yaw);
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cr = cosf(roll),  sr = sinf(roll);
    const float rx[3][3] = { { 1, 0, 0 }, { 0, cp, sp }, { 0, -sp, cp } };   /* Rx(-pitch): +z -> +y */
    const float rz[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };   /* Rz(roll):   +y -> -x */
    const float ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };   /* Ry(yaw):    +z -> +x */
    float t[3][3], rm[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        float a = 0.f; for (int k = 0; k < 3; ++k) a += rx[i][k] * rz[k][j];
        t[i][j] = a;
    }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        float a = 0.f; for (int k = 0; k < 3; ++k) a += ry[i][k] * t[k][j];
        rm[i][j] = a;
    }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        R[i][j] = rm[BWA_ROOM2AMBI[i]][BWA_ROOM2AMBI[j]];    /* ambi component i <- room component */
}

/* clamp-glide toward a target by at most dmax this block (lands exactly once within reach) */
static float rot_glide(float cur, float tgt, float dmax) {
    float d = tgt - cur;
    if (d > dmax) d = dmax; else if (d < -dmax) d = -dmax;
    return cur + d;
}

/* Mix an ambisonic BED voice: decode its SH channels straight onto the 26-ch bus through the static
 * decode matrix (world-locked — no DBAP, occlusion, or directivity), with a master-gain ramp on
 * gcur[0]. Looping / natural end / oneshot-ack are identical to mix_voice.
 *
 * With the PARAMETRIC renderer selected (bwa_set_bed_renderer — first-order DirAC in coarse
 * time-domain bands), the bed's FOA channels are analyzed per band into a direction + diffuseness
 * (smoothed intensity vector vs energy): the NON-DIFFUSE stream re-pans W through the
 * listener-relative panner at a virtual source on the array shell (ref + bed_radius*doa — the bed
 * becomes walkable: off-center listeners get parallax a matrix decode can't give), and the DIFFUSE
 * stream decodes through the matrix into the DECORRELATORS (incoherent envelopment). Both are
 * loudness-matched to the matrix decode (bed_pref); a `mix` factor ramped 0<->1 on the toggle
 * crossfades the whole renderer, so the A/B is click-free (invariant 4). */
static void mix_bed(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n, uint32_t start) {
    const SoundData* snd = v->sound;
    const uint32_t nr = n - start;      /* rendered samples: ramp over the audible part (see mix_voice) */
    float pg, pg_step;
    if (!pause_gate(c, v, idx, nr, &pg, &pg_step)) return;
    const int nch = (int)snd->channels;
    const float g_step = (v->gtarget[0] - v->gcur[0]) / (float)nr;   /* master gain ramp (invariant 4) */
    ParaBed* pb = &c->para[idx];
    /* direct-binaural: the bed passes SH->SH into the direct field (one diagonal per channel —
     * ambi_canon_to_phonon — instead of decode-to-speakers + virtual-speaker re-encode). The
     * parametric renderer and the max-rE taper are SPEAKER-decode concerns and gate off with it
     * (rotation still applies: it turns the field before either destination). */
    const int direct  = c->direct_blk;
    const int want_p  = nch >= 4 && !direct && c->bed_param_blk;  /* the block's single load (rt_render) */
    const int use_p   = want_p || pb->mix > 0.f;
    /* bed orientation (rt_bed_set_orientation): angles glide at BWA_BED_YAW_RATE, and rotation
     * happens BEFORE either renderer, so the matrix decode and the parametric analysis see the same
     * turned field. Yaw-only runs the exact per-sample phasor recurrence (mode 1 — no per-sample
     * trig; m = 2,3 by angle addition); any pitch/roll engages the full Ivanic-Ruedenberg matrix
     * (mode 2): the live matrix rot_m is rebuilt per block from the glided angles and interpolated
     * per sample toward the block target (invariant 4). Settling back to pitch = roll = 0 hands off
     * to the phasor exactly (the matrix IS the yaw rotation there). Settled at 0/0/0 -> bypassed. */
    const int rot_needs_full = v->bpitch != 0.f || v->bpitch_cur != 0.f || v->broll != 0.f || v->broll_cur != 0.f;
    int rot_mode = 0, rot_n = 0;
    float rc1 = 1.f, rs1 = 0.f, rdc = 1.f, rds = 0.f;
    float rot_end[BWA_SH_ROT_N], rot_step[BWA_SH_ROT_N];
    if (rot_needs_full && !v->rot_full) {                 /* engage: seed the live matrix at the current angles */
        float R[3][3]; bed_rot_ambi(v->yaw_cur, v->bpitch_cur, v->broll_cur, R);
        ambi_rot_matrix(R, v->rot_m);
        v->rot_full = 1;
    }
    if (v->rot_full) {
        rot_mode = 2;
        const float dmax = BWA_BED_YAW_RATE * (float)nr / (float)c->sample_rate;
        v->yaw_cur    = rot_glide(v->yaw_cur,    v->yaw,    dmax);
        v->bpitch_cur = rot_glide(v->bpitch_cur, v->bpitch, dmax);
        v->broll_cur  = rot_glide(v->broll_cur,  v->broll,  dmax);
        float R[3][3]; bed_rot_ambi(v->yaw_cur, v->bpitch_cur, v->broll_cur, R);
        ambi_rot_matrix(R, rot_end);
        rot_n = nch >= 16 ? BWA_SH_ROT_N : (nch >= 9 ? 34 : 9);   /* only the blocks the bed uses */
        for (int j = 0; j < rot_n; ++j) rot_step[j] = (rot_end[j] - v->rot_m[j]) / (float)nr;
        if (v->bpitch == 0.f && v->broll == 0.f && v->bpitch_cur == 0.f && v->broll_cur == 0.f)
            v->rot_full = 0;                              /* landed flat: the phasor takes over next block */
    } else if (v->yaw != 0.f || v->yaw_cur != 0.f) {
        rot_mode = 1;
        const float dmax = BWA_BED_YAW_RATE * (float)nr / (float)c->sample_rate;
        float dtot = v->yaw - v->yaw_cur;
        if (dtot > dmax) dtot = dmax; else if (dtot < -dmax) dtot = -dmax;
        const float dphi = dtot / (float)nr;
        rc1 = cosf(v->yaw_cur); rs1 = sinf(v->yaw_cur);
        rdc = cosf(dphi); rds = sinf(dphi);
        v->yaw_cur += dtot;                               /* land this block's glide */
    }
    /* max-rE decode weighting (rt_set_max_re): taper the SH signal per order before the MATRIX
     * decode (decode(w*sh) == the max-rE decode); re_mix ramps 0<->1 per sample so the A/B
     * crossfades. The parametric analysis, its re-panned direct stream, and the decorrelated
     * diffuse stream see the raw field (see RtCore.max_re). */
    const int   re_on    = direct ? 0 : atomic_load_explicit(&c->max_re, memory_order_acquire);
    const float re_tgt   = re_on ? 1.f : 0.f;
    const int   use_re   = re_on || v->re_mix > 0.f;
    float       rem      = v->re_mix;
    const float rem_step = (re_tgt - rem) / (float)nr;
    /* band-split share (rt_set_max_re_split): 1 = taper only the band ABOVE the 700 Hz crossover
     * (sh - sm*lo), 0 = broadband (the current incumbent). Ramps like re_mix so the toggle is
     * click-free; the splitter state wipes while the taper is disengaged (re-engage is clean, and
     * the rem ramp from 0 masks the one-pole's ~1.4 ms settle anyway). */
    const float sm_tgt   = (re_on && atomic_load_explicit(&c->max_re_split, memory_order_acquire)) ? 1.f : 0.f;
    float       resm     = v->re_sm;
    const float sm_step  = (sm_tgt - resm) / (float)nr;
    const float re_xa    = c->xover_a;
    if (!use_re) {
        v->re_sm = 0.f;
        memset(v->re_lp, 0, sizeof v->re_lp);
    }
    const float* rw = c->re_w[(snd->order >= 1 && snd->order <= 3) ? snd->order - 1 : 2];
    uint32_t cur = v->cursor;
    /* loop region [lbeg,lend) (see mix_voice); 0/0 = whole clip. A bed advances one frame per sample
     * (no pitch), so playback lands cur exactly on lend and loop_wrap returns lbeg. It still goes
     * through loop_wrap, because a mid-play set_region can leave cur far past lend and the two
     * mixers must answer the same call the same way. */
    uint32_t lbeg, lend;
    resolve_loop_region(v, snd->frames, &lbeg, &lend);
    bool ended = false;

    if (!use_p) {                       /* pure matrix decode: the settled default stays this cheap */
        for (uint32_t i = 0; i < n; ++i) {
            if (i < start) continue;               /* scheduled start: silent until the in-block offset */
            if (cur >= lend) { if (v->loop && lend > lbeg) cur = loop_wrap(c, v, idx, cur, lbeg, lend); else ended = true; }
            if (!ended) {
                const float* sh = &snd->pcm[(size_t)cur * nch];
                float shr[BWA_AMBI_CH];
                if (rot_mode == 1) {               /* turn the field, then advance the yaw phasor */
                    const float cm[3] = { rc1, rc1*rc1 - rs1*rs1, rc1*(rc1*rc1 - rs1*rs1) - rs1*(2.f*rc1*rs1) };
                    const float sm[3] = { rs1, 2.f*rc1*rs1,       rs1*(rc1*rc1 - rs1*rs1) + rc1*(2.f*rc1*rs1) };
                    bed_rotate_z(sh, nch, cm, sm, shr);
                    sh = shr;
                    const float t = rc1*rdc - rs1*rds; rs1 = rs1*rdc + rc1*rds; rc1 = t;
                } else if (rot_mode == 2) {        /* full 3-axis: live matrix, glided toward the block target */
                    ambi_rot_apply(v->rot_m, sh, nch, shr);
                    sh = shr;
                    for (int j = 0; j < rot_n; ++j) v->rot_m[j] += rot_step[j];
                }
                float shw[BWA_AMBI_CH];
                if (use_re) {                      /* max-rE taper, crossfaded by rem; resm splits the band */
                    for (int k = 0; k < nch; ++k) {
                        const float lo = (v->re_lp[k] += re_xa * (sh[k] - v->re_lp[k]));
                        shw[k] = sh[k] + rem * (rw[k] - 1.f) * (sh[k] - resm * lo);
                    }
                    sh = shw;
                }
                const float g = v->gcur[0] * pg;   /* master gain x the pause/seek gate */
                if (direct) {                      /* SH->SH: the (rotated) field, basis-converted */
                    float* ad = c->ambi_direct;
                    for (int k = 0; k < nch; ++k)
                        ad[(size_t)k * n + i] += g * sh[k] * ambi_canon_to_phonon[k];
                } else {
                    for (uint32_t s = 0; s < c->channels; ++s) {
                        const float* D = c->bed_decode[s];
                        float acc = 0.f;
                        for (int k = 0; k < nch; ++k) acc += sh[k] * D[k];
                        bus[(size_t)s * n + i] += g * acc;
                    }
                }
                ++cur;
            }
            pg += pg_step;
            v->gcur[0] += g_step;
            rem += rem_step;
            resm += sm_step;
        }
    } else {
        /* block-rate parameter update from the SMOOTHED analysis state (last blocks' field): per band
         * psi = 1 - |I|/E (0 = a plane wave, 1 = isotropic/incoherent), direct gains from the panner
         * at ref + R*doa. Targets ramp across the block; this block's samples update the smoothing. */
        const int p = atomic_load_explicit(&c->panner, memory_order_acquire);
        float da_tgt[BWA_PARA_BANDS], fa_tgt[BWA_PARA_BANDS];
        for (int b = 0; b < BWA_PARA_BANDS; ++b) {
            const float E = pb->E[b];
            const float In = sqrtf(pb->I[b][0]*pb->I[b][0] + pb->I[b][1]*pb->I[b][1] + pb->I[b][2]*pb->I[b][2]);
            float psi = (E > 1e-12f) ? 1.f - In / E : 1.f;
            if (psi < 0.f) psi = 0.f; else if (psi > 1.f) psi = 1.f;
            da_tgt[b] = sqrtf(1.f - psi) * c->bed_pref;
            fa_tgt[b] = sqrtf(psi);
            if (In > 1e-9f) {                          /* doa (ambi) -> room -> virtual source on the shell */
                float a[3] = { pb->I[b][0]/In, pb->I[b][1]/In, pb->I[b][2]/In };
                float dr[3]; ambi_to_room(a, dr);      /* the shared (z,x,y) permutation, inverted */
                float pos[3] = { c->layout.ref[0] + c->bed_radius * dr[0],
                                 c->layout.ref[1] + c->bed_radius * dr[1],
                                 c->layout.ref[2] + c->bed_radius * dr[2] };
                float gt[BWA_CHANNELS];
                panner_gains(c, p, pos, 1.f, gt);
                double gp = 0.0; for (uint32_t ch = 0; ch < c->channels; ++ch) gp += (double)gt[ch]*gt[ch];
                float gn = gp > 1e-12 ? (float)(1.0 / sqrt(gp)) : 0.f;      /* unit power: no distance atten
                                                                             * (a bed has direction, not range) */
                for (uint32_t ch = 0; ch < c->channels; ++ch) pb->g_tgt[b][ch] = gt[ch] * gn;
            }                                          /* |I| ~ 0: direction undefined; keep the last gains
                                                        * (da -> 0 there anyway, psi -> 1) */
        }
        float g_stp[BWA_PARA_BANDS][BWA_CHANNELS], da_stp[BWA_PARA_BANDS], fa_stp[BWA_PARA_BANDS];
        for (int b = 0; b < BWA_PARA_BANDS; ++b) {
            da_stp[b] = (da_tgt[b] - pb->da_cur[b]) / (float)nr;
            fa_stp[b] = (fa_tgt[b] - pb->fa_cur[b]) / (float)nr;
            for (uint32_t ch = 0; ch < c->channels; ++ch)
                g_stp[b][ch] = (pb->g_tgt[b][ch] - pb->g_cur[b][ch]) / (float)nr;
        }
        const float pm_tgt  = want_p ? 1.f : 0.f;
        const float pm_step = (pm_tgt - pb->mix) / (float)nr;
        float pmix = pb->mix;
        const int   run_matrix = !(pb->mix >= 1.f && pm_tgt >= 1.f);   /* both paths only mid-crossfade */
        float aE[BWA_PARA_BANDS] = { 0 }, aI[BWA_PARA_BANDS][3] = {{ 0 }};
        c->dc_wrote = 1;                               /* the diffuse stream lands in the decor bus */

        for (uint32_t i = 0; i < n; ++i) {
            if (i < start) continue;
            if (cur >= lend) { if (v->loop && lend > lbeg) cur = loop_wrap(c, v, idx, cur, lbeg, lend); else ended = true; }
            if (!ended) {
                const float* sh = &snd->pcm[(size_t)cur * nch];
                float shr[BWA_AMBI_CH];
                if (rot_mode == 1) {                   /* turn the field before EITHER renderer sees it */
                    const float cm[3] = { rc1, rc1*rc1 - rs1*rs1, rc1*(rc1*rc1 - rs1*rs1) - rs1*(2.f*rc1*rs1) };
                    const float sm[3] = { rs1, 2.f*rc1*rs1,       rs1*(rc1*rc1 - rs1*rs1) + rc1*(2.f*rc1*rs1) };
                    bed_rotate_z(sh, nch, cm, sm, shr);
                    sh = shr;
                    const float t = rc1*rdc - rs1*rds; rs1 = rs1*rdc + rc1*rds; rc1 = t;
                } else if (rot_mode == 2) {            /* full 3-axis: live matrix, glided per sample */
                    ambi_rot_apply(v->rot_m, sh, nch, shr);
                    sh = shr;
                    for (int j = 0; j < rot_n; ++j) v->rot_m[j] += rot_step[j];
                }
                const float g = v->gcur[0] * pg;
                if (run_matrix) {                      /* matrix share of the crossfade */
                    const float gm = g * (1.f - pmix);
                    float shw[BWA_AMBI_CH];
                    const float* shd = sh;
                    if (use_re) {                      /* max-rE taper on the MATRIX share only */
                        for (int k = 0; k < nch; ++k) {
                            const float lo = (v->re_lp[k] += re_xa * (sh[k] - v->re_lp[k]));
                            shw[k] = sh[k] + rem * (rw[k] - 1.f) * (sh[k] - resm * lo);
                        }
                        shd = shw;
                    }
                    for (uint32_t s = 0; s < c->channels; ++s) {
                        const float* D = c->bed_decode[s];
                        float acc = 0.f;
                        for (int k = 0; k < nch; ++k) acc += shd[k] * D[k];
                        bus[(size_t)s * n + i] += gm * acc;
                    }
                }
                /* FOA band split (ACN: W Y Z X), complementary one-poles -> BWA_PARA_BANDS bands */
                float bnd[4][BWA_PARA_BANDS];           /* [foa channel][band] */
                for (int f = 0; f < 4; ++f) {
                    const float s0 = sh[f];
                    float l0 = (pb->lp[0][f] += c->para_xa[0] * (s0 - pb->lp[0][f]));
                    float l1 = (pb->lp[1][f] += c->para_xa[1] * (s0 - pb->lp[1][f]));
                    float l2 = (pb->lp[2][f] += c->para_xa[2] * (s0 - pb->lp[2][f]));
                    bnd[f][0] = l0; bnd[f][1] = l1 - l0; bnd[f][2] = l2 - l1; bnd[f][3] = s0 - l2;
                }
                float sd[BWA_PARA_BANDS], dfo[4] = { 0, 0, 0, 0 };
                for (int b = 0; b < BWA_PARA_BANDS; ++b) {
                    const float wb = bnd[0][b], yb = bnd[1][b], zb = bnd[2][b], xb = bnd[3][b];
                    aE[b]    += 0.5f * (wb*wb + xb*xb + yb*yb + zb*zb);   /* SN3D FOA: |I|/E = 1 for a plane wave */
                    aI[b][0] += wb * xb; aI[b][1] += wb * yb; aI[b][2] += wb * zb;
                    sd[b] = wb * pb->da_cur[b];        /* direct stream: W, scaled sqrt(1-psi)*pref */
                    for (int k = 0; k < 4; ++k) dfo[k] += pb->fa_cur[b] * bnd[k][b];   /* diffuse FOA */
                    pb->da_cur[b] += da_stp[b]; pb->fa_cur[b] += fa_stp[b];
                }
                const float gp2 = g * pmix;
                for (uint32_t s = 0; s < c->channels; ++s) {
                    float dacc = 0.f, macc = 0.f;
                    const float* D = c->bed_decode[s];
                    for (int b = 0; b < BWA_PARA_BANDS; ++b) {
                        dacc += sd[b] * pb->g_cur[b][s];                  /* re-panned direct */
                        pb->g_cur[b][s] += g_stp[b][s];
                    }
                    for (int k = 0; k < 4; ++k) macc += dfo[k] * D[k];    /* diffuse -> decorrelators */
                    bus[(size_t)s * n + i]       += gp2 * dacc;
                    c->dc_bus[(size_t)s * n + i] += gp2 * macc;
                }
                ++cur;
            }
            pg += pg_step;
            v->gcur[0] += g_step;
            pmix += pm_step;
            rem += rem_step;
            resm += sm_step;
        }
        /* land the ramps exactly + fold this block's analysis into the smoothed field */
        pb->mix = pm_tgt;
        for (int b = 0; b < BWA_PARA_BANDS; ++b) {
            pb->da_cur[b] = da_tgt[b]; pb->fa_cur[b] = fa_tgt[b];
            for (uint32_t ch = 0; ch < c->channels; ++ch) pb->g_cur[b][ch] = pb->g_tgt[b][ch];
        }
        const float a = 1.f - expf(-(float)nr / (BWA_PARA_TAU * (float)c->sample_rate));
        const float inv_nr = 1.f / (float)nr;
        for (int b = 0; b < BWA_PARA_BANDS; ++b) {
            pb->E[b] += a * (aE[b] * inv_nr - pb->E[b]);
            for (int j = 0; j < 3; ++j) pb->I[b][j] += a * (aI[b][j] * inv_nr - pb->I[b][j]);
        }
    }

    v->cursor = cur;
    v->gcur[0] = v->gtarget[0];
    v->re_mix = re_tgt;                                            /* land the max-rE crossfade */
    if (use_re) v->re_sm = sm_tgt;                                 /* land the split share too */
    if (rot_mode == 2) memcpy(v->rot_m, rot_end, sizeof(float) * rot_n);   /* land the live matrix */
    if (ended) {
        v->playing = false;
        if (!v->oneshot) {                     /* a oneshot has no caller handle to report AGAINST:
                                                * rt_play_oneshot never bumps play_seq, so its 0 would
                                                * also sail through the drain's 0 == 0 seq gate. */
            Evt done = { .type = EVT_VOICE_DONE, .seq = v->play_seq, .handle = BWA_MK_H(idx, v->gen) };
            evt_push_notice(c, &done, &c->done_dropped);   /* best-effort; never displaces an ownership ack */
        }
        if (v->oneshot) {
            v->active = false;
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BWA_MK_H(idx, v->gen) };
            evt_push(&c->events, &ev);
        }
    }
}

/* Tracked room EQ (layout room_eq_grid): interpolate the grid's per-speaker section-cut depths at
 * the live listener position — inverse-distance weighting over the measurement positions, smoothed
 * by an epsilon so standing exactly on a mic point still blends its neighbors — and hand them to
 * the aligner as slew targets (align_room_eq_targets; the slew makes the walk click-free). Audio
 * thread, no alloc/locks; recomputed only when the listener moved > ~1 cm since the last solve
 * (align keeps slewing toward the standing targets meanwhile). The live kill switch
 * (rt_set_room_eq_dyn) aims every section at flat instead of stepping the EQ out. */
static void room_eq_track(RtCore* c) {
    const RoomEqGrid* g = &c->layout.rq_grid;
    if (!g->npos || !c->aligner) return;
    float tgt[BWA_CHANNELS][BWA_ROOM_EQ_MAX];
    if (!atomic_load_explicit(&c->room_eq_dyn, memory_order_acquire)) {
        if (c->rq_state != 2) {                       /* toggled off: slew to flat, once */
            memset(tgt, 0, sizeof tgt);
            align_room_eq_targets(c->aligner, tgt);
            c->rq_state = 2;
        }
        return;
    }
    const float* lp = c->lis.p_active;
    if (c->rq_state == 1) {
        float dx = lp[0]-c->rq_lis[0], dy = lp[1]-c->rq_lis[1], dz = lp[2]-c->rq_lis[2];
        if (dx*dx + dy*dy + dz*dz < 1e-4f) return;    /* < 1 cm: the standing targets hold */
    }
    double w[BWA_RQ_GRID_MAX], wsum = 0.0;
    for (uint8_t i = 0; i < g->npos; ++i) {           /* IDW: w = 1/(d² + eps²), eps = 15 cm */
        /* squares in DOUBLE: a large-but-finite listener coordinate (isfinite-gated only) would
         * overflow the float square to Inf, zero every weight, and make inv below 1/0 — and
         * 0 * Inf = NaN would land in the align biquad targets permanently */
        double dx = (double)lp[0]-g->pos[i][0], dy = (double)lp[1]-g->pos[i][1], dz = (double)lp[2]-g->pos[i][2];
        w[i] = 1.0 / (dx*dx + dy*dy + dz*dz + 0.0225);
        wsum += w[i];
    }
    if (!(wsum > 0.0)) return;                        /* unreachable with the double math; belt for the NaN class */
    double inv = 1.0 / wsum;
    for (uint32_t k = 0; k < c->channels; ++k)
        for (uint8_t s = 0; s < g->nsec[k] && s < BWA_ROOM_EQ_MAX; ++s) {
            double acc = 0.0;
            for (uint8_t i = 0; i < g->npos; ++i) acc += w[i] * (double)g->gain_db[i][k][s];
            tgt[k][s] = (float)(acc * inv);
        }
    align_room_eq_targets(c->aligner, tgt);
    memcpy(c->rq_lis, lp, sizeof c->rq_lis);
    c->rq_state = 1;
}

/* Tracked listener alignment (rt_set_tracked_align, OFF by default). The layout's per-speaker delay
 * and gain trims align the array at ONE point, Layout.ref, so the array is time-coherent there and
 * progressively less so as the listener walks away. This re-references that alignment onto the live
 * listener: per speaker the correction is the geometry, nothing more —
 *
 *     dref = |speaker - ref|,  dlis = |speaker - listener|
 *     extra delay (frames) = (dref - dlis) * rate / sos      > 0 when the listener moved TOWARD it
 *     extra gain  (linear) =  dlis / dref                    < 1 when the listener moved TOWARD it
 *
 * SIGN: walking toward a speaker makes its wavefront arrive EARLY and LOUD, so the fix delays it more
 * and turns it down. The delay set is then shifted so its per-block MINIMUM is zero, which makes the
 * correction purely relative (a common delay is just latency), keeps every target non-negative so the
 * aligner only ever reads further back in its ring, and makes a listener standing exactly at `ref`
 * exact identity rather than approximate.
 *
 * The whole feature is opt-in because a moving delay line is a resampling event: N speakers gliding
 * continuously is N simultaneous Doppler shifts on everything the array plays, which is a GLOBAL
 * audible failure, not a local one. Two guards, both live knobs:
 *   - a DEAD ZONE: nothing is recomputed until the listener has moved further than `dead` from where
 *     the standing targets were solved, so Motive's position jitter changes nothing at all;
 *   - a RATE LIMIT on the slew (align_tracked_slew), so a listener who moves faster than the limit
 *     gets a lagging alignment instead of a pitch-shifted array.
 * Audio thread, no alloc/locks. Reads c->lis.p_active, which BOTH listener paths write (CMD_COMMIT
 * and the internal tracker block at the top of rt_render) — the same reason room_eq_track above lives
 * here rather than hanging off commit.
 *
 * Note it deliberately does NOT bump pan_gen: this is the output stage, downstream of every gain
 * solve, so no voice's panning changes and there is nothing to re-solve. */
#define LC_DEAD_ZONE_M   0.05f  /* 5 cm. Motive's jitter is sub-millimeter, so this is pure noise
                                 * rejection; the residual it allows is 5 cm / 343 m/s = 0.15 ms of
                                 * arrival error, an order under the ~1 ms scale where precedence and
                                 * comb-filtering start to read, and ~3% of the correction the feature
                                 * is applying at a 1.5 m excursion. */
#define LC_SLEW_SPEED_MS 0.45f  /* The default rate limit, stated as the listener closing speed it can
                                 * follow: 0.45 m/s (a slow walk) = 0.45/343 = 0.13% resampling = ~2.3
                                 * cents of pitch shift at worst, which sits under the JND for a
                                 * sustained tone. A brisk walk (1.4 m/s) therefore OUTRUNS it on
                                 * purpose: stale alignment is the cheap failure, warble is not.
                                 * Converted to frames/s against the live rate and speed of sound, so
                                 * it means the same thing at 96 kHz or in a different medium. */
static void listener_align_track(RtCore* c) {
    if (!c->aligner) return;
    /* Publish-then-flag, reader side (CLAUDE.md): acquire the ENABLE first, then load the knobs the
     * setter published before it. The knobs are re-read every block rather than stamped by a
     * generation, so a lost update self-corrects on the next block — but the ordering still has to
     * hold for the enable itself. */
    if (!atomic_load_explicit(&c->lc_on, memory_order_acquire)) {
        if (c->lc_state != 2) {                       /* toggled off: slew back to identity, once */
            align_tracked_targets(c->aligner, NULL, NULL);
            c->lc_state = 2;
        }
        return;
    }
    const float sos  = atomic_load_explicit(&c->sos,       memory_order_relaxed);
    float       slew = atomic_load_explicit(&c->lc_slew,   memory_order_relaxed);
    float       dead = atomic_load_explicit(&c->lc_dead_m, memory_order_relaxed);
    if (!(sos > 1.f)) return;                         /* nonsense medium: leave the alignment alone */
    if (slew <= 0.f) slew = LC_SLEW_SPEED_MS / sos * (float)c->sample_rate;
    if (dead <= 0.f) dead = LC_DEAD_ZONE_M;
    align_tracked_slew(c->aligner, slew);             /* cheap, and keeps a live slider honest */
    const float* lp = c->lis.p_active;
    if (c->lc_state == 1) {
        float dx = lp[0]-c->lc_lis[0], dy = lp[1]-c->lc_lis[1], dz = lp[2]-c->lc_lis[2];
        if (dx*dx + dy*dy + dz*dz < dead*dead) return;   /* inside the dead zone: nothing moves */
    }
    const Layout* L = &c->layout;
    float dly[BWA_CHANNELS], gn[BWA_CHANNELS];
    const float k_frames = (float)c->sample_rate / sos;
    float dmin = 0.f;
    for (uint32_t k = 0; k < c->channels; ++k) {
        const float* s = L->speakers[k].pos;
        float ax = s[0]-L->ref[0], ay = s[1]-L->ref[1], az = s[2]-L->ref[2];
        float bx = s[0]-lp[0],     by = s[1]-lp[1],     bz = s[2]-lp[2];
        float dref = sqrtf(ax*ax + ay*ay + az*az);
        float dlis = sqrtf(bx*bx + by*by + bz*bz);
        float t = (dref - dlis) * k_frames;
        dly[k] = t;
        gn[k]  = (dref > 1e-3f && dlis > 1e-3f) ? dlis / dref : 1.f;   /* degenerate: leave it alone */
        if (k == 0 || t < dmin) dmin = t;
    }
    const float cap = (float)align_tracked_max_frames(c->aligner);
    for (uint32_t k = 0; k < c->channels; ++k) {
        float d = dly[k] - dmin;                      /* re-zero: relative correction only */
        dly[k] = d > cap ? cap : d;                   /* past the reserved headroom: degrade, don't wrap */
    }
    align_tracked_targets(c->aligner, dly, gn);
    memcpy(c->lc_lis, lp, sizeof c->lc_lis);
    c->lc_state = 1;
}

/* A seqlock's payload is ordinary data, but this core keeps every published field in an _Atomic
 * slot; bit-cast doubles through uint64 so they ride the same relaxed load/store. */
static inline void   pub_d(_Atomic uint64_t* slot, double v) {
    uint64_t u; memcpy(&u, &v, sizeof u); atomic_store_explicit(slot, u, memory_order_relaxed);
}
static inline double ld_d(const _Atomic uint64_t* slot) {
    uint64_t u = atomic_load_explicit(slot, memory_order_relaxed); double v; memcpy(&v, &u, sizeof v); return v;
}

/* Audio thread: fold one device stamp into the drift fit and republish the model. Called from
 * rt_render INSIDE the clock seqlock's write window, so the pair and the model a control-thread
 * reader sees always describe the same block.
 *
 * The regression is host seconds -> sample position, both taken relative to the fit origin so the
 * accumulators never carry the raw magnitudes (a host-time ns count is ~1e18; squaring it would eat
 * the mantissa). Weighted-Welford updates keep it stable indefinitely: means move first, then the
 * central moments accumulate against the UPDATED mean, all in deviations. */
static void clk_fit_update(RtCore* c, uint64_t sample, uint64_t time_ns, uint32_t nframes) {
    /* Discontinuity check. A stop/start cycle re-bases the device's sample position while the host
     * clock keeps running, and a line fitted through that jump reports pure fiction — so reseed
     * instead. Backwards on either axis is nonsense; an implied rate far off nominal means the two
     * stamps do not belong to the same run. A genuine gap (a stalled callback) leaves the rate
     * plausible and is harmless to a line fit, so it survives. */
    if (c->fit_w > 0.0) {
        bool ok = sample > c->fit_prev_s && time_ns > c->fit_prev_t;
        if (ok) {
            double dy = (double)(sample - c->fit_prev_s);
            double dx = (double)(time_ns - c->fit_prev_t) * 1e-9;
            double r  = dy / dx;                               /* samples per host second, this interval */
            ok = r > 0.5 * (double)c->sample_rate && r < 2.0 * (double)c->sample_rate;
        }
        if (!ok) c->fit_w = 0.0;                               /* drop the fit; reseed below */
    }
    if (c->fit_w <= 0.0) {                                     /* seed: one point, at the origin */
        c->fit_x0  = (double)time_ns * 1e-9;
        c->fit_y0  = (double)sample;
        c->fit_mx  = c->fit_my = c->fit_cxx = c->fit_cxy = c->fit_sse = c->fit_span = 0.0;
        c->fit_b   = (double)c->sample_rate;                   /* prior: nominal, until the fit has a slope */
        c->fit_w   = 1.0;
        c->fit_prev_s = sample; c->fit_prev_t = time_ns;
        atomic_store_explicit(&c->fit_stamps, 0, memory_order_relaxed);   /* no slope from one point */
        return;
    }
    if (nframes != c->fit_nframes) {         /* forgetting factor per block; exp(-dt/TAU) to first order
                                              * (dt/TAU ~ 4e-5 at 256/48k, so the linear form is exact
                                              * to a part in 1e9 — and costs no libm on the audio thread) */
        double dt = (double)nframes / (double)c->sample_rate;
        double lam = 1.0 - dt / BWA_CLK_TAU_S;
        c->fit_lam     = lam > 0.0 ? lam : 0.0;
        c->fit_nframes = nframes;
    }
    const double lam = c->fit_lam;
    const double x = (double)time_ns * 1e-9 - c->fit_x0;       /* host seconds since the origin */
    const double y = (double)sample - c->fit_y0;               /* samples since the origin */
    const double w = lam * c->fit_w + 1.0;
    const double dx = x - c->fit_mx, dy = y - c->fit_my;
    /* Residual of this stamp against the fit as it stood BEFORE it — a one-step prediction error,
     * and the only numerically sound route to a residual here. The textbook closed form
     * (Cyy - b*Cxy) subtracts two quantities of order 1e16 samples^2 to land on ~1e-7 and returns
     * nothing but rounding noise, because the fit is near-perfect over a lever arm of millions of
     * samples. This differs by two terms in comparable magnitudes, so it stays exact: the fit line
     * passes through (mx, my), making the prediction at x exactly my + b*dx. Held off until the
     * slope has a few points behind it, since a 2-point fit's b is meaningless. */
    if (w >= 8.0) { const double r = dy - c->fit_b * dx; c->fit_sse = lam * c->fit_sse + r * r; }
    c->fit_mx  += dx / w;
    c->fit_my  += dy / w;
    c->fit_cxx  = lam * c->fit_cxx + dx * (x - c->fit_mx);
    c->fit_cxy  = lam * c->fit_cxy + dx * (y - c->fit_my);
    c->fit_w    = w;
    c->fit_span = x;
    c->fit_prev_s = sample; c->fit_prev_t = time_ns;
    if (c->fit_cxx > 0.0) c->fit_b = c->fit_cxy / c->fit_cxx;  /* fitted samples per host second */

    if (c->fit_span < BWA_CLK_MIN_SPAN_S || c->fit_w < 16.0 || c->fit_cxx <= 0.0) {
        atomic_store_explicit(&c->fit_stamps, 0, memory_order_relaxed);
        return;                                                /* too short a lever arm to mean anything */
    }
    const double b   = c->fit_b;
    const double var = c->fit_sse / (c->fit_w - 2.0);          /* residual variance, samples^2 */
    const double rate = (double)c->sample_rate;
    pub_d(&c->fit_rate,  b);
    pub_d(&c->fit_ppm,   (b / rate - 1.0) * 1e6);
    pub_d(&c->fit_sigma, sqrt(var / c->fit_cxx) / rate * 1e6); /* std error of the slope, in ppm */
    pub_d(&c->fit_jit,   sqrt(var) / rate * 1e9);              /* rms residual, in ns */
    pub_d(&c->fit_spanp, c->fit_span);
    atomic_store_explicit(&c->fit_stamps, (uint32_t)(c->fit_w + 0.5), memory_order_relaxed);
}


void rt_render(RtCore* c, float* bus, uint32_t nframes, const bwa_timestamp* ts) {
    /* dsp clock for sample-accurate scheduling: prefer the device sample position (ASIO/null); fall
     * back to an internal block counter when no timestamp is supplied (e.g. direct rt_render in tests).
     * block_start is the absolute dsp-sample of bus[0]; publish it so the control thread can schedule
     * relative to "now" (rt_dsp_time). */
    uint64_t block_start = ts ? ts->sample_pos : c->dsp_block;
    c->dsp_block = block_start + nframes;
    atomic_store_explicit(&c->dsp_now, block_start, memory_order_relaxed);
    if (ts && ts->system_time_ns) {   /* publish the device clock pair (seqlock; this thread is the only writer) */
        uint32_t cs = atomic_load_explicit(&c->clk_seq, memory_order_relaxed);
        atomic_store_explicit(&c->clk_seq, cs + 1, memory_order_relaxed);       /* odd: write window opens */
        atomic_thread_fence(memory_order_release);                              /* the odd seq lands before the data */
        atomic_store_explicit(&c->clk_sample, block_start, memory_order_relaxed);
        atomic_store_explicit(&c->clk_time, ts->system_time_ns, memory_order_relaxed);
        clk_fit_update(c, block_start, ts->system_time_ns, nframes);            /* + the drift model, same window */
        atomic_store_explicit(&c->clk_seq, cs + 2, memory_order_release);       /* even: pair + model consistent */
    }
#if defined(_MSC_VER)
    /* Flush denormals to zero on the audio thread: gain ramps toward 0 (e.g. a voice
     * moving off a channel) otherwise produce subnormals that stall the FP pipeline. */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    BWA_ZONE_BEGIN(zr, "rt_render");
    drain_commands(c);

    /* Every RT scratch buffer (aux / stream_scratch / path_accum) is sized BWA_RT_MAX_BLOCK, which
     * matches the engine's BWA_MAX_BLOCK device-block ceiling. If a driver ever presents a larger
     * block, fail safe to silence rather than overflow the scratch on the audio thread. (Commands
     * are still drained above so the control ring never backs up.) */
    if (nframes > BWA_RT_MAX_BLOCK) {
        memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
        BWA_ZONE_END(zr);
        return;
    }

    /* internal tracking: sample the freshest tracked head pose at block time, overriding the
     * committed listener (lower latency than routing pose through the command ring). A position
     * change dirties every voice, since DBAP gains are all listener-relative. Loaded ONCE per
     * block (acquire): the control thread may swap the tracker between blocks. */
    const PoseSlot* trk = atomic_load_explicit(&c->tracker, memory_order_acquire);
    if (trk) {
        float tp[3], tq[4];
        uint64_t tns = 0;
        if (pose_read_t(trk, tp, tq, &tns)) {
            /* SANITIZE THE WIRE POSE. This is the production path and it does NOT go through
             * rt_set_listener, so that call's finite guard and quaternion normalization reach it not
             * at all: natnet.c copies wire floats into the seqlock unvalidated. One corrupt Motive
             * packet would otherwise NaN every panner solve permanently, because a NaN in gcur can
             * never leave the ramp x + (t - x) * k, and an un-normalized wire quat overflows
             * frame_qrot exactly as a large one does through the client path. Drop a non-finite
             * frame outright (keep the last good pose, which is what a dropped frame means anyway)
             * and normalize the quaternion, degenerate falling back to identity. */
            if (!(bwa_finite3_bounded(tp, BWA_MAX_COORD) && bwa_quat_unit(tq))) goto pose_done;
            /* pose prediction: lead the position along a velocity estimated from the tracker's own
             * timestamps (differences only — never compared to the device clock). The estimate
             * smooths over ~100 ms (Motive's frame-to-frame velocity is jittery), resets across a
             * drop-out (no stale extrapolation), and the speed is capped so a tracking glitch can't
             * fling the listener. Walking at 1.5 m/s with a 30 ms lead recovers ~4.5 cm of lag. */
            const float lead = atomic_load_explicit(&c->pred_lead, memory_order_relaxed);
            if (lead > 0.f && tns) {
                if (c->pp_valid && tns > c->pp_tns) {
                    const double dt = (double)(tns - c->pp_tns) * 1e-9;
                    if (dt < 0.25) {                             /* contiguous: update the velocity */
                        const float a = 1.f - expf(-(float)dt / 0.1f);
                        for (int j = 0; j < 3; ++j) {
                            float vr = (tp[j] - c->pp_p[j]) / (float)dt;
                            if (vr > 5.f) vr = 5.f; else if (vr < -5.f) vr = -5.f;
                            c->pp_vel[j] += a * (vr - c->pp_vel[j]);
                        }
                    } else memset(c->pp_vel, 0, sizeof c->pp_vel);   /* gap: restart the estimate */
                }
                if (!c->pp_valid || tns != c->pp_tns) {
                    memcpy(c->pp_p, tp, sizeof c->pp_p);
                    c->pp_tns = tns; c->pp_valid = 1;
                    c->pp_quiet = 0.f;
                } else {
                    /* no new frame: a STALLED tracker must not keep extrapolating a frozen pose.
                     * After ~0.25 s of silence, glide the velocity out (a hard zero would step the
                     * rendered position by vel*lead). Uses only the audio clock — no cross-clock math. */
                    c->pp_quiet += (float)nframes / (float)c->sample_rate;
                    if (c->pp_quiet > 0.25f)
                        for (int j = 0; j < 3; ++j) c->pp_vel[j] *= 0.9f;
                }
                for (int j = 0; j < 3; ++j) tp[j] += c->pp_vel[j] * lead;
            } else if (c->pp_valid) { c->pp_valid = 0; memset(c->pp_vel, 0, sizeof c->pp_vel); }
            bool moved = memcmp(c->lis.p_active, tp, sizeof tp) != 0;
            /* Same orientation gate as CMD_COMMIT, and it MUST live here too: the tracker path does
             * not go through commit at all, it overwrites the active pose directly. Without this a
             * pure head TURN from Motive never dirties a voice, so CAP stays solved for whatever
             * orientation the listener last happened to translate at — the seated case the feature
             * is aimed at is exactly the one where position holds still. Tested BEFORE q_active is
             * overwritten below, and gated on cap_on so a tracked head does not re-solve every voice
             * every block for a rotation nothing downstream reads. */
            if (!moved && atomic_load_explicit(&c->cap_on, memory_order_acquire))
                moved = memcmp(c->lis.q_active, tq, sizeof tq) != 0;
            memcpy(c->lis.p_active, tp, sizeof tp);
            memcpy(c->lis.q_active, tq, sizeof tq);
            if (moved) for (uint32_t i = 0; i < c->voice_cap; ++i) c->voices[i].dirty = true;
        }
        pose_done: ;
    }

    memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
    /* Acquire-load the taps once (paired with the release stores in rt_set_*_tap): a non-NULL tap
     * guarantees its ud/ambi_ch are visible, so registration mid-run (bwa_start) can't tear. */
    const RtBusTap  bus_tap  = atomic_load_explicit(&c->bus_tap,  memory_order_acquire);
    const RtPathTap path_tap = atomic_load_explicit(&c->path_tap, memory_order_acquire);
    /* the reflection aux send: collected this block if a tap is registered + the block fits the scratch */
    float* aux = (bus_tap && nframes <= BWA_RT_MAX_BLOCK) ? c->aux : NULL;
    if (aux) memset(aux, 0, sizeof(float) * (size_t)nframes);
    const int path_active = (path_tap && c->path_ambi_ch && nframes <= BWA_RT_MAX_BLOCK);
    if (path_active) memset(c->path_accum, 0, sizeof(float) * (size_t)c->path_ambi_ch * nframes);  /* pathing accumulator */
    /* decor bus: zeroed whenever a mixer might write it (spread toggle on, the parametric bed's
     * diffuse stream, or something still ramping/flushing out). The toggles are loaded ONCE for the
     * whole block — gate and mixers must agree, or a toggle landing mid-block drops a block of the
     * incoherent share into a bus that never gets convolved. */
    c->dc_on_blk     = atomic_load_explicit(&c->decor_on,  memory_order_acquire);
    c->bed_param_blk = atomic_load_explicit(&c->bed_param, memory_order_acquire);
    c->all_paused_blk = atomic_load_explicit(&c->all_paused, memory_order_acquire);   /* one load: every
                                                                                       * gate agrees this block */
    /* The block's panner-parameter generation: ONE acquire load for every voice (a live SPCAP knob
     * moves every source's gains, so a lagging generation re-solves the voice — see RtCore.pan_gen).
     * This load MUST come before the knob loads below. The setter publishes focus/density and THEN
     * release-bumps pan_gen, so only a reader that acquires the generation first is guaranteed to
     * see the values that belong to it. Read in the other order, a block can pair the NEW generation
     * with the OLD focus, stamp every voice current, and swallow the change until the next bump —
     * which for the last set of a slider drag means the knob silently never takes. */
    const uint32_t pan_gen_blk = atomic_load_explicit(&c->pan_gen, memory_order_acquire);
    /* SPCAP knobs, resolved once for the block (<= 0 = the layout's derived/constant default) */
    {
        const float sf = atomic_load_explicit(&c->spcap_focus,   memory_order_relaxed);
        const float sd = atomic_load_explicit(&c->spcap_density, memory_order_relaxed);
        c->spcap_focus_blk   = sf > 0.f ? sf : c->layout.spcap_focus;
        c->spcap_density_blk = sd > 0.f ? sd : c->layout.spcap_density;
    }
    /* hole-aware spread floor, same rule: one load, after the pan_gen acquire above, so every voice
     * this block agrees on the strength AND on which generation it belongs to (rt_set_hole_spread). */
    c->hole_spread_blk = atomic_load_explicit(&c->hole_spread, memory_order_relaxed);
    const int dc_live = c->dc_on_blk || c->bed_param_blk || c->dc_tail > 0;
    if (dc_live) memset(c->dc_bus, 0, sizeof(float) * (size_t)c->channels * nframes);
    c->dc_wrote = 0;
    /* direct-binaural: point voices accumulate into the SH bus this block (mix_voice's `bus`
     * argument), leaving the speaker bus to the synthesized-diffuse layer. nframes <=
     * BWA_RT_MAX_BLOCK here (the guard above), so the accumulator always fits. Mode 2 also
     * clears the per-voice point-tap views; each rendered voice re-marks its own. */
    c->direct_blk = c->direct_on && c->ambi_direct != NULL;
    if (c->direct_blk) {
        memset(c->ambi_direct, 0, sizeof(float) * (size_t)BWA_AMBI_CH * nframes);
        if (c->direct_on == 2 && c->dv_view)
            for (uint32_t i = 0; i < c->voice_cap; ++i) c->dv_view[i].active = 0;
    }
    /* adopt a published ISM-room change (rt_set_ism_room's seqlock): one attempt per block — on a
     * torn read keep the previous copy and retry next block (wait-free; a lost race just delays
     * adoption ~one block, inaudible under the image gain/delay ramps). */
    {
        const uint32_t is0 = atomic_load_explicit(&c->ism_seq, memory_order_seq_cst);
        if (is0 != c->ism_seen && !(is0 & 1)) {
            const IsmRoom tmp = c->ism_room_sh;
            if (atomic_load_explicit(&c->ism_seq, memory_order_seq_cst) == is0) {
                c->ism_room = tmp;
                c->ism_seen = is0;
            }
        }
    }
    int rt_active = 0;
    BWA_ZONE_BEGIN(zmix, "mix voices");
    for (uint32_t i = 0; i < c->voice_cap; ++i) {
        Voice* v = &c->voices[i];
        if (!v->active || !v->playing || !v->sound) continue;
        ++rt_active;
        /* scheduled start: hold the voice until the block that contains its start_sample, then begin
         * at the exact in-block offset (sample-accurate). 0 = play immediately. */
        uint32_t off = 0;
        if (v->start_sample > block_start) {
            uint64_t d = v->start_sample - block_start;
            if (d >= nframes) continue;                /* starts in a later block: nothing to mix yet */
            off = (uint32_t)d;
        }
        /* scheduled stop (rt_source_stop_at): once this block reaches stop_at, begin the click-free
         * one-block fade — the SAME path as an explicit rt_source_stop, never a hard cut, so it can't
         * pop. Block-granular: silence lands within one block of stop_at. */
        if (v->stop_sched && block_start + nframes >= v->stop_at) {
            v->stop_sched = false;
            if (v->playing && v->stopping != 2) v->stopping = 1;
        }
        if (v->fade_rate != 0.f) {                     /* timed fade: glide gain_user, re-solve each block
                                                        * (the per-channel gcur ramp keeps it click-free) */
            v->gain_user += v->fade_rate * (float)nframes;
            if ((v->fade_rate > 0.f && v->gain_user >= v->fade_target) ||
                (v->fade_rate < 0.f && v->gain_user <= v->fade_target)) {
                v->gain_user = v->fade_target;         /* land exactly */
                v->fade_rate = 0.f;
                if (v->fade_stop) { v->fade_stop = 0;
                    if (v->playing && v->stopping != 2) v->stopping = 1; }   /* click-free stop path */
            }
            v->dirty = true;
        }
        if (v->dirty || v->pan_gen != pan_gen_blk) { compute_gains(c, v); v->dirty = false; v->pan_gen = pan_gen_blk; }
        if (v->sound->channels > 1) mix_bed  (c, v, (uint16_t)i, bus, nframes, off);        /* ambisonic bed */
        else                        mix_voice(c, v, (uint16_t)i,                            /* mono point source */
                                              c->direct_blk ? c->ambi_direct : bus, nframes, off, aux);
    }
    BWA_ZONE_END(zmix);
    BWA_PLOT("rt voices", rt_active);
    atomic_store_explicit(&c->active_pub, (uint32_t)rt_active, memory_order_relaxed);   /* rt_active_voices */
    /* decorrelation: convolve the decor bus through each channel's sparse velvet filter into the main
     * bus. Runs while anything wrote this block, plus one filter-length of flush so the tail rings
     * out; on going idle the history is wiped, so a later re-engage can never replay stale samples. */
    if (dc_live)    if (dc_live)    if (c->dc_wrote) c->dc_tail = c->dc_histlen;
    if (dc_live && c->dc_tail > 0) {
        BWA_ZONE_BEGIN(zdc, "decorrelate");
        const uint32_t hm = c->dc_hmask, hl = c->dc_histlen, w = c->dc_w, nt = c->dc_ntaps;
        for (uint32_t ch = 0; ch < c->channels; ++ch) {
            float* h = &c->dc_hist[(size_t)ch * hl];
            const float* in = &c->dc_bus[(size_t)ch * nframes];
            for (uint32_t i = 0; i < nframes; ++i) h[(w + i) & hm] = in[i];
            const uint16_t* off = c->dc_off[ch];
            const float*    amp = c->dc_tamp[ch];
            float* out = &bus[(size_t)ch * nframes];
            for (uint32_t i = 0; i < nframes; ++i) {
                float acc = 0.f;
                for (uint32_t t = 0; t < nt; ++t) acc += amp[t] * h[(w + i - off[t]) & hm];
                out[i] += acc;
            }
        }
        c->dc_w = (w + nframes) & hm;
        c->dc_tail = c->dc_tail > nframes ? c->dc_tail - nframes : 0;
        if (c->dc_tail == 0)
            memset(c->dc_hist, 0, sizeof(float) * (size_t)c->channels * hl);
        BWA_ZONE_END(zdc);
    }
    for (uint32_t i = 0; i < c->voice_cap; ++i) {   /* publish playback state for rt_source_is_playing (control thread) */
        const Voice* v = &c->voices[i];
        uint32_t st = ((uint32_t)v->gen << 9) | ((uint32_t)v->play_seq << 1)
                    | ((v->active && v->playing && v->sound) ? 1u : 0u);
        atomic_store_explicit(&c->play_pub[i], st, memory_order_release);
        uint64_t pos = v->sound ? (v->sound->stream ? v->stream_pos : (uint64_t)v->cursor) : 0;
        atomic_store_explicit(&c->pos_pub[i], ((uint64_t)v->gen << 48) | (pos & 0xFFFFFFFFFFFFULL),
                              memory_order_release);
    }
    if (aux) {   /* reflection bed: convolve the aux send + sum onto the bus BEFORE align (so it gets trim+delay too) */
        BWA_ZONE_BEGIN(zt, "reflect tap");
        bus_tap(c->bus_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, aux);
        BWA_ZONE_END(zt);
    }
    if (path_active) {
        if (c->direct_blk) {
            /* direct mode: the indirect field is already phonon-basis SH (steam_path publishes
             * phonon's own shCoeffs) — sum it straight into the direct field. The speaker decode
             * + virtual-speaker re-encode would be pure loss, and joining the HRTF decode means
             * head orientation lands on the indirect arrivals too, as it should. */
            const uint32_t kc = c->path_ambi_ch < BWA_AMBI_CH ? c->path_ambi_ch : BWA_AMBI_CH;
            for (uint32_t k = 0; k < kc; ++k) {
                float* ad = c->ambi_direct + (size_t)k * nframes;
                const float* pa = c->path_accum + (size_t)k * nframes;
                for (uint32_t i = 0; i < nframes; ++i) ad[i] += pa[i];
            }
        } else {   /* pathing: decode the summed indirect ambisonic field onto the bus (pre-align) */
            BWA_ZONE_BEGIN(zp, "path tap");
            path_tap(c->path_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, c->path_accum, c->path_ambi_ch);
            BWA_ZONE_END(zp);
        }
    }
    /* master gain: one ramped scalar over everything mixed so far (voices, beds, reverb/path taps),
     * applied PRE-align so the per-speaker trims and the raw channel-test signal stay calibrated.
     * Skipped entirely while settled at unity. */
    {
        const float mg_tgt = atomic_load_explicit(&c->master_gain, memory_order_relaxed);
        if (mg_tgt != 1.f || c->master_g_cur != 1.f) {
            const float step = (mg_tgt - c->master_g_cur) / (float)nframes;
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                float g = c->master_g_cur;
                float* p = &bus[(size_t)ch * nframes];
                for (uint32_t i = 0; i < nframes; ++i) { p[i] *= g; g += step; }
            }
            c->master_g_cur = mg_tgt;                  /* land exactly */
        }
    }
    room_eq_track(c);                          /* tracked room EQ: re-aim the align biquads at the pose */
    listener_align_track(c);                   /* tracked alignment: re-aim the align delays at the pose */
    BWA_ZONE_BEGIN(za, "align");
    align_process(c->aligner, bus, nframes);   /* per-speaker gain trim + delay (output stage) */
    BWA_ZONE_END(za);

    /* debug channel test (bwa_set_test_signal): inject a built-in signal onto a raw output channel AFTER
     * align, so it is independent of the per-speaker trim/delay — a clean speaker-check / wiring tool. */
    for (uint32_t ch = 0; ch < c->channels; ++ch) {
        uint8_t k = c->test_kind[ch];
        if (!k) continue;
        float g = c->test_gain[ch];
        float* out = &bus[(size_t)ch * nframes];
        if (k == 1) {                              /* sine, ~660 Hz */
            const float inc = 6.2831853f * 660.0f / (float)c->sample_rate;
            float ph = c->test_phase[ch];
            for (uint32_t i = 0; i < nframes; ++i) { out[i] += g * sinf(ph); ph += inc; if (ph > 6.2831853f) ph -= 6.2831853f; }
            c->test_phase[ch] = ph;
        } else {                                   /* white noise (shared LCG) */
            uint32_t n = c->test_noise;
            for (uint32_t i = 0; i < nframes; ++i) { n = n * 1664525u + 1013904223u; out[i] += g * ((float)(n >> 9) * (1.0f / 4194304.0f) - 1.0f); }
            c->test_noise = n;
        }
    }

    /* protection limiter (final stage): everything that reaches the device — voices, beds, taps,
     * align, test signal — passes through. One gain from the cross-channel peak (linked: engaging
     * never shifts the spatial image), attack/release one-poles, then a hard clamp at the ceiling
     * (the attack is not lookahead, so a transient's first ~ms can clip — protection, not mastering). */
    if (atomic_load_explicit(&c->lim_on, memory_order_acquire)) {
        const float lim_c = atomic_load_explicit(&c->lim_ceiling, memory_order_relaxed);
        float g = c->lim_gain;
        for (uint32_t i = 0; i < nframes; ++i) {
            float peak = 0.f;
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                float a = bus[(size_t)ch * nframes + i];
                a = a < 0.f ? -a : a;
                if (a > peak) peak = a;
            }
            float want = (peak > lim_c) ? lim_c / peak : 1.f;
            g += (want - g) * (want < g ? c->lim_att_a : c->lim_rel_a);
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                float* p = &bus[(size_t)ch * nframes + i];
                float s = *p * g;
                /* the hard clamp is the designated last line of defense, so it must not pass the two
                 * things a clamp can't see: NaN fails both compares, and an Inf bus sample drives
                 * `want` to 0 so g underflows and Inf * 0 = NaN right here. Scrub to 0 (silence),
                 * never to the ceiling (a full-scale DC step). */
                if (!isfinite(s)) s = 0.f;
                else if (s >  lim_c) s =  lim_c;
                else if (s < -lim_c) s = -lim_c;
                *p = s;
            }
        }
        c->lim_gain = g;
    }
    /* per-channel output meter: publish this block's peak |sample| — measured after align, the test
     * signal, and the limiter, i.e. exactly what the device channel receives (rt_bus_peaks). */
    for (uint32_t ch = 0; ch < c->channels; ++ch) {
        const float* p = &bus[(size_t)ch * nframes];
        float m = 0.f;
        for (uint32_t i = 0; i < nframes; ++i) { float a = p[i] < 0.f ? -p[i] : p[i]; if (a > m) m = a; }
        atomic_store_explicit(&c->chan_peak[ch], m, memory_order_relaxed);
    }
    pose_write(&c->readback, c->lis.p_active, c->lis.q_active);   /* publish for control-thread readback */
    BWA_ZONE_END(zr);
}

void rt_read_pose(RtCore* c, float p[3], float q[4]) {
    if (!c) return;
    if (!pose_read(&c->readback, p, q)) {       /* lost the seqlock race (rare): best-effort direct read */
        memcpy(p, c->lis.p_active, sizeof(float) * 3);
        memcpy(q, c->lis.q_active, sizeof(float) * 4);
    }
}

uint32_t rt_bus_peaks(RtCore* c, float* out, uint32_t cap) {
    if (!c || !out) return 0;
    uint32_t n = c->channels < cap ? c->channels : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = atomic_load_explicit(&c->chan_peak[i], memory_order_relaxed);
    return n;
}

/* Control-thread readback: is the source's voice still producing audio? Reads the per-slot state the
 * audio thread republishes each block, gated on the handle's generation (a stale/recycled handle, or
 * a finished non-loop voice, reads as not-playing). Until the audio thread has published this
 * generation once (the window between a play — or bwa_source_create_push's internal one — and the
 * next rendered block), the control-side pending-play flag answers instead, so a fresh voice never
 * reads not-playing and a poll-then-destroy can't drop it. Best-effort: a sound shorter than a poll
 * interval may never be observed playing. */
bool rt_source_is_playing(RtCore* c, uint32_t h) {
    if (!c || h == 0) return false;
    uint32_t idx = BWA_H_IDX(h);
    if (idx >= c->voice_cap) return false;
    if (!(c->inuse[idx] && c->gen[idx] == BWA_H_GEN(h))) return false;   /* stale or recycled handle */
    const uint8_t seq = c->play_seq[idx];
    uint32_t st = atomic_load_explicit(&c->play_pub[idx], memory_order_acquire);
    if ((st >> 9) != BWA_H_GEN(h)) return seq != 0;          /* gen never published: true iff a play is queued */
    if ((uint8_t)((st >> 1) & 0xFFu) != seq) return true;    /* a NEWER play is queued, not yet consumed */
    return (st & 1u) != 0u;                                  /* the audio thread has our play: authoritative */
}

/* Control-thread: drain the handles whose voices have ENDED since the last call, oldest first.
 * Writes at most `cap` of them to `out` and returns how many. The events already existed (the audio
 * thread posts EVT_VOICE_ENDED and drain_events consumes it); this just stops throwing them away, so
 * a client can react to completion instead of rebuilding it from is_playing polling.
 *
 * Handles are reported as the caller knew them, so a straight compare against a stored handle works.
 * A plain source's handle stays VALID and re-playing it is normal; only a oneshot's transient handle
 * is recycled, and those are not reported (the caller was never given one). Steals are not reported
 * either: the engine took the slot, the sound did not finish.
 *
 * Fills from the same drain rt_commit runs, so poll after commit. Unpolled events are bounded and
 * drop OLDEST; `dropped_out` (optional) reports the running total so a client can tell "nothing
 * finished" from "I did not poll often enough". */
/* Control thread: the live values of the knobs rt SANITIZES, so a readback reports what will be
 * rendered rather than what the caller passed. Every one of these is stored post-clamp by its setter,
 * so this is a plain read; keeping it here rather than re-clamping in engine.c means there is exactly
 * one copy of each range. A 0 that is a "use the default" sentinel stays 0, which is what was set. */
/* The SPCAP knobs post-clamp, for the same reason as the rest: rt bounds focus to [1,64] and density
 * to (0,16], and a readback that reported the raw argument would contradict its own contract. 0 stays
 * 0, because 0 is the "use this array's derived default" sentinel and that IS what was set. */
void rt_get_spcap_sanitized(RtCore* c, float* focus, float* density) {
    if (!c) return;
    if (focus)   *focus   = atomic_load_explicit(&c->spcap_focus,   memory_order_relaxed);
    if (density) *density = atomic_load_explicit(&c->spcap_density, memory_order_relaxed);
}

void rt_get_tuning_sanitized(RtCore* c, int* panner, int* spread_mode, float* near_spread,
                             float* hole_spread, float* dead_zone_m, float* slew_frames_per_s) {
    if (!c) return;
    if (panner)       *panner       = atomic_load_explicit(&c->panner,       memory_order_relaxed);
    if (spread_mode)  *spread_mode  = atomic_load_explicit(&c->spread_mode,  memory_order_relaxed);
    if (near_spread)  *near_spread  = atomic_load_explicit(&c->near_spread,  memory_order_relaxed);
    if (hole_spread)  *hole_spread  = atomic_load_explicit(&c->hole_spread,  memory_order_relaxed);
    if (dead_zone_m)  *dead_zone_m  = atomic_load_explicit(&c->lc_dead_m,    memory_order_relaxed);
    if (slew_frames_per_s) *slew_frames_per_s = atomic_load_explicit(&c->lc_slew, memory_order_relaxed);
}

uint32_t rt_poll_ended(RtCore* c, uint32_t* out, uint32_t cap, uint64_t* dropped_out) {
    if (!c) return 0;
    if (dropped_out) *dropped_out = c->ended_dropped
                                  + atomic_load_explicit(&c->done_dropped, memory_order_relaxed);
    if (!out || cap == 0u) return 0;
    uint32_t n = c->ended_len < cap ? c->ended_len : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = c->ended[(c->ended_head + i) % c->ended_cap];
    c->ended_head = (c->ended_head + n) % c->ended_cap;
    c->ended_len -= n;
    return n;
}

/* The loop-wrap twin of rt_poll_ended: same drain point (rt_commit), same oldest-drops-first bound,
 * same "handles come back as you knew them" contract. One entry per WRAP. */
uint32_t rt_poll_looped(RtCore* c, uint32_t* out, uint32_t cap, uint64_t* dropped_out) {
    if (!c) return 0;
    if (dropped_out) *dropped_out = c->looped_dropped
                                  + atomic_load_explicit(&c->loop_dropped, memory_order_relaxed);
    if (!out || cap == 0u) return 0;
    uint32_t n = c->looped_len < cap ? c->looped_len : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = c->looped[(c->looped_head + i) % c->looped_cap];
    c->looped_head = (c->looped_head + n) % c->looped_cap;
    c->looped_len -= n;
    return n;
}

/* Control-thread readback: the voice's content playhead in engine-rate frames, as of the last
 * rendered block. In-memory/bed voices report the sample cursor (frozen under pause, landed after a
 * seek, wrapped by a loop); stream/push voices report frames actually CONSUMED — the data-driven
 * clock, which an underrun slips rather than drops, so this is the truth the caller cannot derive
 * from the dsp clock. A finished non-loop voice keeps reporting its final position until the next
 * play; a destroyed, stale, or recycled handle, an idle voice, or a scheduled play still held
 * silent reads 0.
 * Latest-wins at block granularity: a just-issued play/seek is reflected one block later. */
uint64_t rt_source_get_position(RtCore* c, uint32_t h) {
    if (!c || h == 0) return 0;
    uint32_t idx = BWA_H_IDX(h);
    if (idx >= c->voice_cap) return 0;
    /* Control-side liveness FIRST, the same gate rt_source_is_playing uses, because the published
     * word below only refreshes when a block actually renders. Destroy a voice and read before the
     * next block and the last publish still carries the SAME generation, so the gen check cannot
     * see the destroy and a dead handle reports its final cursor while is_playing already reports
     * false — the two readbacks contradicting each other about one handle. The handle table is
     * control-thread-owned, so it is authoritative the instant rt_source_destroy runs, and reading
     * it costs the audio thread nothing. A voice that merely ENDED keeps its slot and still passes
     * here, which is what keeps "a finished non-loop voice reports its final position" true. */
    if (!voice_live_ctrl(c, h)) return 0;                 /* destroyed, stale, or recycled handle */
    uint64_t st = atomic_load_explicit(&c->pos_pub[idx], memory_order_acquire);
    if ((uint32_t)(st >> 48) != BWA_H_GEN(h)) return 0;   /* slot re-let, or not yet published */
    return st & 0xFFFFFFFFFFFFULL;
}

/* Control thread: the engine's current dsp-sample clock (the most recently rendered block's first
 * sample). Schedule a sample-accurate play with rt_source_play_at(.., rt_dsp_time(c) + delay_samples). */
uint64_t rt_dsp_time(RtCore* c) {
    return c ? atomic_load_explicit(&c->dsp_now, memory_order_relaxed) : 0;
}

/* Control-thread readback: the device's (output sample position, host time) correspondence stamped
 * at the top of the last rendered block — ASIO's ASIOGetSamplePosition pair; the null sink's QPC
 * synthesis. Unlike pairing rt_dsp_time with a caller-side clock read (jittered by up to a block
 * plus scheduling), this pair is stamped inside the audio stack itself, so the wall->dsp mapping
 * dsp(T) = sample + (T - time_ns) * rate / 1e9 is exact. The epoch of time_ns is backend-defined —
 * anchor it against your own monotonic clock and track the constant offset. False (outputs
 * untouched) until a host-stamped block has rendered: before start, or under a driver that reports
 * no systemTime. The manual sink DOES stamp, but a nominal time derived from sample_pos rather than
 * a wall clock (manual_sink.c) — reproducible by design, so it is exact for arithmetic and
 * meaningless as wall time. Note the stamp gate is `system_time_ns != 0`, so the very first block
 * of a manual render (whose nominal time IS 0) publishes nothing; the clock goes valid on block 2. */
/* Invalidate the published device-clock pair. Called when the engine stops, because a restart
 * RE-BASES the sample clock to 0 while the last pair still describes the previous session: an
 * AV-sync client reading it would map into the old epoch. `time_ns == 0` is already rt_get_clock's
 * "nothing published yet", which is the honest answer between sessions, and the manual sink's first
 * re-based block legitimately stamps 0 so it cannot clear this by itself. */
void rt_reset_clock(RtCore* c) {
    if (!c) return;
    uint32_t s1 = atomic_load_explicit(&c->clk_seq, memory_order_relaxed);
    atomic_store_explicit(&c->clk_seq, s1 + 1u, memory_order_release);   /* odd: write in progress */
    atomic_store_explicit(&c->clk_sample, 0u, memory_order_relaxed);
    atomic_store_explicit(&c->clk_time,   0u, memory_order_relaxed);
    atomic_store_explicit(&c->clk_seq, s1 + 2u, memory_order_release);
}

bool rt_get_clock(RtCore* c, uint64_t* sample, uint64_t* time_ns) {
    if (!c) return false;
    for (int tries = 0; tries < 16; ++tries) {          /* seqlock read: retry a torn snapshot */
        uint32_t s1 = atomic_load_explicit(&c->clk_seq, memory_order_acquire);
        if (s1 & 1u) continue;                          /* write in progress */
        uint64_t sm = atomic_load_explicit(&c->clk_sample, memory_order_relaxed);
        uint64_t tm = atomic_load_explicit(&c->clk_time, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);      /* the data loads land before the re-check */
        if (atomic_load_explicit(&c->clk_seq, memory_order_relaxed) != s1) continue;
        if (tm == 0) return false;                      /* nothing published yet */
        if (sample)  *sample  = sm;
        if (time_ns) *time_ns = tm;
        return true;
    }
    return false;   /* persistently torn — cannot happen at block cadence (one write per ~5 ms) */
}

/* Control-thread readback of the drift fit (same seqlock as the pair, so the model and the pair a
 * caller reads describe the same block). False with `out` untouched until the fit has BWA_CLK_MIN_SPAN_S
 * of stamps behind it — before that the slope is dominated by stamp jitter and reporting it would
 * invite a caller to act on noise. Reseeds (device restart, re-based sample position) take it back
 * to false until the new fit earns its span. */
bool rt_get_clock_model(RtCore* c, RtClockFit* out) {
    if (!c || !out) return false;
    for (int tries = 0; tries < 16; ++tries) {
        uint32_t s1 = atomic_load_explicit(&c->clk_seq, memory_order_acquire);
        if (s1 & 1u) continue;                          /* write in progress */
        uint32_t n = atomic_load_explicit(&c->fit_stamps, memory_order_relaxed);
        RtClockFit f = { ld_d(&c->fit_ppm), ld_d(&c->fit_sigma), ld_d(&c->fit_rate),
                         ld_d(&c->fit_spanp), ld_d(&c->fit_jit), n };
        atomic_thread_fence(memory_order_acquire);      /* the data loads land before the re-check */
        if (atomic_load_explicit(&c->clk_seq, memory_order_relaxed) != s1) continue;
        if (n == 0) return false;                       /* no usable fit yet */
        *out = f;
        return true;
    }
    return false;
}

/* Active listener pose, for the binaural monitor. Audio thread only (same thread as
 * rt_render, which is the sole writer of the active fields) — no extra synchronization. */
void rt_get_listener(RtCore* c, float p[3], float q[4]) {
    memcpy(p, c->lis.p_active, sizeof(float) * 3);
    memcpy(q, c->lis.q_active, sizeof(float) * 4);
}

/* ---- control-thread API (enqueue) ---- */

uint32_t rt_source_create(RtCore* c) {
    /* Normal alloc draws the pool down only to the fade reserve; the reserve slots are kept free so a
     * steal can place the new source there while the victim fades out on its own slot. */
    uint32_t h = (c->free_count > c->fade_reserve) ? alloc_handle(c) : 0;
    if (!h) {                               /* user pool full: steal the lowest-priority active source */
        int victim = -1, lowest = 256;
        for (uint32_t i = 0; i < c->voice_cap; ++i)   /* 255 = protected; skip a slot already fading from a steal */
            if (c->inuse[i] && !c->stealing[i] && c->priority[i] < 255 && (int)c->priority[i] < lowest)
                { lowest = c->priority[i]; victim = (int)i; }
        if (victim >= 0) {
            uint32_t vh = BWA_MK_H((uint16_t)victim, c->gen[victim]);
            /* Preferred: click-free steal — the new source takes a RESERVE slot and the victim fades out
             * on its own (CMD_SRC_STEAL), freeing its slot via EVT_VOICE_ENDED once silent. */
            uint32_t nh = alloc_handle(c);            /* a reserve slot (victim keeps its own, so nh != victim) */
            if (nh) {
                Cmd steal = { .type = CMD_SRC_STEAL, .handle = vh };
                if (cmd_push(&c->cmds, &steal)) { c->stealing[victim] = 1; h = nh; }
                else recycle_handle(c, nh);           /* ring full: undo, fall through to the hard-cut path */
            }
            if (!h) {                                 /* reserve exhausted (steal burst) / ring full: hard-cut + reuse */
                rt_source_destroy(c, vh);
                h = alloc_handle(c);
            }
        }
        if (!h) return 0;                   /* nothing to steal (or the destroy didn't enqueue): genuinely full */
    }
    Cmd cmd = { .type = CMD_SRC_CREATE, .handle = h };
    if (!cmd_push(&c->cmds, &cmd)) {        /* ring full (should never happen): don't leak the slot */
        recycle_handle(c, h);
        return 0;
    }
    return h;
}

/* The unguarded bind: enqueue CMD_PLAY + flag the pending play. rt_source_play_at wraps this in
 * the public guards (retiring sound, push source); rt_source_create_stream calls it directly for
 * its internal bind — an internal caller skips the guards by construction, not by write ordering.
 * loop_beg/loop_end are the loop region (0/0 = whole clip); the audio thread resolves them. */
static void source_bind(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample,
                        uint64_t loop_beg, uint64_t loop_end) {
    Cmd cmd = { .type = CMD_PLAY, .handle = h };
    cmd.u.play.sound = sound; cmd.u.play.loop = loop ? 1u : 0u; cmd.u.play.oneshot = 0u;
    cmd.u.play.start = start_sample;
    cmd.u.play.loop_beg = loop_beg; cmd.u.play.loop_end = loop_end;
    if (voice_live_ctrl(c, h)) {
        /* Bump BEFORE the push, so the audio thread can never consume a play carrying a seq the
         * control side has not recorded yet (that would read as "a newer play is queued" forever).
         * 0 is reserved for "no play enqueued at this generation", so the counter skips it.
         * If the ring REFUSES the push the play never happens, so the bump has to be undone: a
         * counter left ahead of the published seq makes is_playing answer true forever with no
         * voice. Both are same-thread, so the rollback is safe. */
        uint8_t* q = &c->play_seq[BWA_H_IDX(h)];
        const uint8_t prev = *q;
        if (++(*q) == 0) *q = 1;
        cmd.u.play.seq = *q;
        if (!cmd_push(&c->cmds, &cmd)) *q = prev;
        return;
    }
    cmd_push(&c->cmds, &cmd);
}

/* Bind an open stream to a fresh sound slot (shared by file streaming and push sources); on a
 * full table the stream is closed and 0 returned with `full_msg` in err. */
static uint32_t bind_stream_sound(RtCore* c, Stream* st, const char* full_msg, char* err, size_t errcap) {
    uint32_t snd = salloc_sound(c);
    if (!snd) {
        stream_close(c->streams, st);
        set_err(err, errcap, full_msg);
        return 0;
    }
    SoundData d; memset(&d, 0, sizeof d);
    d.stream = st; d.channels = 1; d.sample_rate = c->sample_rate;   /* pcm NULL, frames 0: the ring is
                                                                      * the content, the stream tracks EOF */
    c->sounds[BWA_H_IDX(snd)].data = d;
    return snd;
}

/* PUSH source: a source whose voice plays caller-pushed PCM through a per-source ring
 * (stream_open_push) instead of a loaded sound — the "second feeding path" (docs/api.md). The
 * internal sound slot never leaves rt.c: it binds here and retires when the source handle dies
 * (rt_source_destroy / a steal's EVT_VOICE_ENDED — push_sound_release). */
uint32_t rt_source_create_stream(RtCore* c, char* err, size_t errcap) {
    if (!c) return 0;
    /* create + play must BOTH land (a bound-but-never-fed voice is fine; a created-but-never-bound
     * push source would sit silent forever looking alive). The worst case is EXACTLY 4, no slack:
     * a full pool whose fade reserve is exhausted hard-cuts a victim that is itself a push source —
     * CMD_SRC_DESTROY + CMD_SOUND_RETIRE (the victim's internal sound) + CMD_SRC_CREATE + CMD_PLAY.
     * The click-free steal path needs 3 (CMD_SRC_STEAL + create + play). */
    if (cmd_free(&c->cmds) < 4) {
        set_err(err, errcap, "push: command ring full");
        return 0;
    }
    Stream* st = stream_open_push(c->streams, err, errcap);
    if (!st) return 0;
    uint32_t snd = bind_stream_sound(c, st, "push: sound table full", err, errcap);
    if (!snd) return 0;
    uint32_t h = rt_source_create(c);
    if (!h) {
        retire_internal_sound(c, snd);      /* nothing ever bound: the retire-ack closes the ring */
        set_err(err, errcap, "push: voice pool full");
        return 0;
    }
    /* Bind + start consuming NOW: an empty ring is an underrun (renders silence, never ends the
     * voice), so the source is live from the first pushed sample. source_bind skips the public
     * play guards, so the push mapping can be installed first (no loop region — a ring is sequential). */
    c->push_sound[BWA_H_IDX(h)] = snd;
    source_bind(c, h, snd, false, 0, 0, 0);
    return h;
}

/* control-thread resolve: the push source's ring iff h is its live handle */
static Stream* push_stream_ctrl(RtCore* c, uint32_t h) {
    if (!rt_source_is_push(c, h)) return NULL;
    SoundSlot* s = sound_slot_ctrl(c, c->push_sound[BWA_H_IDX(h)]);
    return s ? s->data.stream : NULL;
}

/* Feed a push source (control thread — the ring's single producer). Returns frames accepted:
 * short/0 when the ring is full (~1.3 s at 48 kHz), after rt_source_push_end, or on a stale handle. */
uint32_t rt_source_push(RtCore* c, uint32_t h, const float* frames, uint32_t n) {
    Stream* st = push_stream_ctrl(c, h);
    return st ? stream_push(st, frames, n) : 0;
}

uint32_t rt_source_push_space(RtCore* c, uint32_t h) {
    Stream* st = push_stream_ctrl(c, h);
    return st ? stream_push_space(st) : 0;
}

void rt_source_push_end(RtCore* c, uint32_t h) {
    Stream* st = push_stream_ctrl(c, h);
    if (st) stream_push_end(st);
}

bool rt_source_is_push(RtCore* c, uint32_t h) {
    return c && h != 0 && voice_live_ctrl(c, h) && c->push_sound[BWA_H_IDX(h)] != 0;
}

/* Control thread: is h a live source handle at all (any kind)? Lets the engine tell a WRONG-KIND
 * call on a live handle (report an error) from a stale handle (the documented silent no-op). */
bool rt_source_live(RtCore* c, uint32_t h) {
    return c && h != 0 && voice_live_ctrl(c, h);
}

/* Steal priority (control-side only — the audio thread never reads it): 0 = first to be stolen when
 * the pool is full .. 255 = protected. Take effect immediately; safe any time. */
void rt_source_set_priority(RtCore* c, uint32_t h, int priority) {
    if (!c) return;
    if (voice_live_ctrl(c, h))
        c->priority[BWA_H_IDX(h)] = (uint8_t)(priority < 0 ? 0 : priority > 255 ? 255 : priority);
}

void rt_source_destroy(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_SRC_DESTROY, .handle = h };
    /* Recycle only if the destroy was actually enqueued, so a dropped command can't leave
     * the voice active while the index is handed out again. recycle is idempotent, so a
     * double-destroy is harmless. */
    if (cmd_push(&c->cmds, &cmd)) {
        push_sound_release(c, h);        /* a push source owns its internal sound: retire it too */
        recycle_handle(c, h);
    }
}

void rt_source_set_pos(RtCore* c, uint32_t h, float x, float y, float z) {
    /* keep NaN/Inf off the audio thread — and finite-but-absurd with it, because dist2 SQUARES the
     * difference, so a coordinate near FLT_MAX is Inf before any later guard can see it (BWA_MAX_COORD) */
    const float p3[3] = { x, y, z };
    if (!bwa_finite3_bounded(p3, BWA_MAX_COORD)) return;
    Cmd cmd = { .type = CMD_SET_POS, .handle = h };
    cmd.u.pos.x = x; cmd.u.pos.y = y; cmd.u.pos.z = z;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_gain(RtCore* c, uint32_t h, float linear) {
    if (!isfinite(linear) || linear < 0.f) return;             /* reject NaN/Inf/negative gain */
    if (linear > BWA_MAX_GAIN) linear = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    Cmd cmd = { .type = CMD_SET_GAIN, .handle = h };
    cmd.u.gain.g = linear;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_reflections(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_REFLECTIONS, .handle = h };
    cmd.u.refl.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_pathing(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_PATHING, .handle = h };
    cmd.u.path.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_set_path_tap(RtCore* c, RtPathTap tap, void* ud, uint32_t ambi_ch) {
    if (!c) return;
    c->path_tap_ud  = ud;                                        /* publish ud + ambi_ch BEFORE the tap ... */
    c->path_ambi_ch = (ambi_ch > BWA_AMBI_CH) ? BWA_AMBI_CH : ambi_ch;
    atomic_store_explicit(&c->path_tap, tap, memory_order_release);  /* ... acquire-load of a non-NULL tap sees both */
}

/* Off-thread pathing sim publishes a voice's path field: write the back buffer, flip the index (release).
 * `sh` = shCoeffs (directions + level); `eq` = the 3-band bending-loss tilt (NULL = flat). Handle stored
 * alongside so the audio thread drops a stale/recycled slot. */
void rt_set_pathing(RtCore* c, uint32_t handle, const float* sh, const float* eq, uint32_t ambi_ch) {
    if (!c || !sh) return;
    uint32_t idx = BWA_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    if (ambi_ch > BWA_AMBI_CH) ambi_ch = BWA_AMBI_CH;
    int cur = atomic_load_explicit(&c->path_idx[idx], memory_order_relaxed);
    PathPub* back = &c->path_pub[(size_t)idx * 2 + (size_t)(1 - cur)];
    back->handle = handle;
    /* Same backstop as rt_set_direct, for the same reason: these values arrive from the SIM thread,
     * so they fence whatever the ray tracer computed from a degenerate scene. A NaN shCoeff ramps
     * into path_sh_cur (a ramp never sheds NaN) and decodes onto the bus; a finite-but-absurd one
     * overflows it — the BWA_MAX_GAIN class. The eq bands get the eq_pack cleanse ([0,1], NaN-safe);
     * eq_coeffs floors an exact 0 before the biquad design. */
    for (uint32_t k = 0; k < ambi_ch; ++k) {
        float v = sh[k];
        if (!isfinite(v))            v = 0.f;
        else if (v >  BWA_MAX_GAIN)  v =  BWA_MAX_GAIN;
        else if (v < -BWA_MAX_GAIN)  v = -BWA_MAX_GAIN;
        back->sh[k] = v;
    }
    for (uint32_t k = ambi_ch; k < BWA_AMBI_CH; ++k) back->sh[k] = 0.f;
    for (int b = 0; b < 3; ++b) {
        float g = eq ? eq[b] : 1.f;                       /* NULL eq = flat (no bending loss) */
        back->eq[b] = !(g > 0.f) ? 0.f : (g > 1.f ? 1.f : g);
    }
    atomic_store_explicit(&c->path_idx[idx], 1 - cur, memory_order_release);
}

void rt_source_set_reflection_send(RtCore* c, uint32_t h, float gain) {
    if (!isfinite(gain) || gain < 0.f) return;
    if (gain > BWA_MAX_GAIN) gain = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    Cmd cmd = { .type = CMD_SET_REFL_SEND, .handle = h };
    cmd.u.rsend.gain = gain;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_reflection_distance(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_REFL_DIST, .handle = h };
    cmd.u.rdist.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_doppler(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_DOPPLER, .handle = h };
    cmd.u.dop.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_air_absorption(RtCore* c, uint32_t h, bool on) {
    Cmd cmd = { .type = CMD_SET_AIR, .handle = h };
    cmd.u.air.on = on ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_size(RtCore* c, uint32_t h, float radius_m) {
    if (!c || !isfinite(radius_m)) return;      /* keep NaN out of audio-thread state (handler clamp passes it) */
    Cmd cmd = { .type = CMD_SET_SIZE, .handle = h };
    cmd.u.size.radius = radius_m;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_fade_to(RtCore* c, uint32_t h, float gain, float seconds, bool stop_at_end) {
    if (!c) return;
    /* The handler's `!(target > 0)` cleanses NaN but not an absurd finite target: seconds <= 0 takes
     * the INSTANT branch, so gain_user = 3e38 lands on the bus in the very next block. Same cap as
     * rt_source_set_gain. Non-finite/huge seconds would make fade_rate 0 and a fade_out that never
     * stops the voice, so they degrade to instant / an hour. */
    if (!isfinite(gain) || gain < 0.f) gain = 0.f;
    if (gain > BWA_MAX_GAIN) gain = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    if (!isfinite(seconds)) seconds = 0.f;
    else if (seconds > 3600.f) seconds = 3600.f;
    if (stop_at_end) {                 /* fade-out: one-way for a push source, same as stop above */
        Stream* st = push_stream_ctrl(c, h);
        if (st) stream_push_end(st);
    }
    Cmd cmd = { .type = CMD_FADE, .handle = h };
    cmd.u.fade.target  = gain;
    cmd.u.fade.seconds = seconds;
    cmd.u.fade.stop    = stop_at_end ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_pitch(RtCore* c, uint32_t h, float rate) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_PITCH, .handle = h };
    cmd.u.pitch.rate = rate;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_ism(RtCore* c, uint32_t h, bool on) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_ISM, .handle = h };
    cmd.u.ism.on = on ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

/* The shoebox for the image-source reflections. Set while the audio thread is stopped (bwa_start
 * reads it); NULL or an invalid room disables early reflections engine-wide. */
/* Publish the room to the audio thread (single-slot seqlock, pose.h's protocol: the struct is too
 * wide for one atomic store, and the seq_cst stores are full barriers that order the plain copy
 * between them on this target). LIVE-safe — rt_render adopts a stable copy at block start and the
 * opted-in voices re-solve their images next block (gains ramp, delays glide: a room change bends
 * the reflections rather than clicking). Single writer: the control thread. */
void rt_set_ism_room(RtCore* c, const IsmRoom* room) {
    if (!c) return;
    const uint32_t s0 = atomic_load_explicit(&c->ism_seq, memory_order_relaxed);
    atomic_store_explicit(&c->ism_seq, s0 + 1, memory_order_seq_cst);   /* enter (odd) */
    if (room) c->ism_room_sh = *room;
    else      memset(&c->ism_room_sh, 0, sizeof c->ism_room_sh);
    atomic_store_explicit(&c->ism_seq, s0 + 2, memory_order_seq_cst);   /* leave (even) */
}

void rt_set_ism_gain(RtCore* c, float linear) {
    if (!c) return;
    if (!(linear > 0.f)) linear = 0.f;          /* NaN-safe */
    if (linear > BWA_MAX_GAIN) linear = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    atomic_store_explicit(&c->ism_gain, linear, memory_order_relaxed);   /* the solve re-reads it per block */
}

void rt_bed_set_orientation(RtCore* c, uint32_t h, float yaw, float pitch, float roll) {
    if (!c) return;
    if (!(isfinite(yaw) && isfinite(pitch) && isfinite(roll))) return;
    Cmd cmd = { .type = CMD_BED_ROT, .handle = h };
    cmd.u.brot.yaw = yaw; cmd.u.brot.pitch = pitch; cmd.u.brot.roll = roll;
    cmd_push(&c->cmds, &cmd);
}

/* The group lands on the voice (audio thread) AND on the control-side mirror rt_group_stop reads
 * to find this source's held plays. Same rollback discipline as source_bind's play counter: a
 * refused push means the voice keeps its old group, so the mirror has to keep it too, or a later
 * group stop would sweep the held plays of a group the voice is not actually in. */
void rt_source_set_group(RtCore* c, uint32_t h, uint32_t group) {
    if (!c) return;
    const uint8_t id = group < BWA_GROUPS ? (uint8_t)group : 0;   /* out of range = the default group */
    Cmd cmd = { .type = CMD_SET_GROUP, .handle = h };
    cmd.u.group.id = id;
    if (!cmd_push(&c->cmds, &cmd)) return;
    if (voice_live_ctrl(c, h)) c->group[BWA_H_IDX(h)] = id;
}

void rt_group_set_gain(RtCore* c, uint32_t group, float linear) {
    if (!c || group >= BWA_GROUPS) return;
    if (!isfinite(linear) || linear < 0.f) return;             /* reject NaN/Inf/negative gain */
    if (linear > BWA_MAX_GAIN) linear = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    Cmd cmd = { .type = CMD_GROUP_GAIN, .handle = 0 };
    cmd.u.ggain.id = (uint8_t)group;
    cmd.u.ggain.gain = linear;
    cmd_push(&c->cmds, &cmd);
}

void rt_group_set_paused(RtCore* c, uint32_t group, bool paused) {
    if (!c || group >= BWA_GROUPS) return;
    Cmd cmd = { .type = CMD_GROUP_PAUSED, .handle = 0 };
    cmd.u.gpause.id = (uint8_t)group;
    cmd.u.gpause.on = paused ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

/* Stop a whole mix group / everything: ONE command each, the sweep runs on the audio thread (which
 * owns the voice table and the per-voice group id — the control side never sees either). Every
 * member takes the same click-free one-block fade rt_source_stop uses. Out-of-range group ignored,
 * matching rt_group_set_gain / rt_group_set_paused.
 * NOT the same as looping rt_source_stop over your handles: that one also ENDS a push source's feed
 * ring, which needs the control-side handle these sweeps do not have. A push voice stops here; its
 * ring stays open (rt_source_push_end). */
void rt_group_stop(RtCore* c, uint32_t group) {
    if (!c || group >= BWA_GROUPS) return;
    /* Push FIRST, then drop this group's held plays — rt_stop_all's rollback discipline, and for
     * the same reason: on a momentarily full ring the stop never reaches the audio thread, and
     * clearing first would leave a half-effect (voices still playing, but the pending plays
     * silently gone) with no way for the caller to tell. */
    Cmd cmd = { .type = CMD_GROUP_STOP, .handle = 0 };
    cmd.u.group.id = (uint8_t)group;
    if (!cmd_push(&c->cmds, &cmd)) return;
    held_drop_group(c, (uint8_t)group);   /* the group's plays still waiting on an async decode, or
                                           * they would start by themselves once their data lands */
}

void rt_stop_all(RtCore* c) {
    if (!c) return;
    /* Push FIRST, then drop the held plays: on a momentarily full ring the stop never reaches the
     * audio thread, and clearing first would leave a half-effect (voices still playing, but the
     * pending plays silently gone) with no way for the caller to tell. Same rollback discipline as
     * rt_source_destroy / rt_unload_sound. */
    Cmd cmd = { .type = CMD_STOP_ALL, .handle = 0 };
    if (!cmd_push(&c->cmds, &cmd)) return;
    c->held_n = 0;      /* "stop everything" includes the plays still waiting on an async decode,
                         * or they would start by themselves the moment their data lands. A GROUP
                         * stop does the same for its own members, off the control-side group
                         * mirror (held_drop_group) — this one needs no mirror because it takes
                         * every entry whatever group it belongs to. */
}

void rt_source_set_spread(RtCore* c, uint32_t h, float amount) {
    if (!isfinite(amount)) return;
    Cmd cmd = { .type = CMD_SET_SPREAD, .handle = h };
    cmd.u.spread.amount = amount;
    cmd.u.spread.height = -1.f;                       /* scalar spread = isotropic (resets any extent) */
    cmd_push(&c->cmds, &cmd);
}

/* Per-source distance-attenuation override: same formula as the layout's curve with this source's
 * own parameters; ref_m <= 0 clears it (back to the layout curve), rolloff 0 = constant level.
 * Applied by ratio in the gain solve (see compute_gains) — panner-agnostic, ramps like any gain. */
void rt_source_set_attenuation(RtCore* c, uint32_t h, float ref_m, float rolloff, float min_lin) {
    if (!c || !isfinite(ref_m) || !isfinite(rolloff) || !isfinite(min_lin)) return;
    Cmd cmd = { .type = CMD_SET_ATTEN, .handle = h };
    cmd.u.atten.ref = ref_m; cmd.u.atten.rolloff = rolloff; cmd.u.atten.min_lin = min_lin;
    cmd_push(&c->cmds, &cmd);
}

/* Anisotropic extent (BS.2127-style width/height): the horizontal and vertical angular extents,
 * each 0..1. Equal values behave as the isotropic spread; the room's up axis anchors which way is
 * "width", so straight overhead the split is inherently ill-defined (BS.2127's polar extent carries
 * the same singularity). rt_source_set_spread resets a voice to isotropic. */
void rt_source_set_extent(RtCore* c, uint32_t h, float w, float hgt) {
    if (!isfinite(w) || !isfinite(hgt)) return;
    Cmd cmd = { .type = CMD_SET_SPREAD, .handle = h };
    cmd.u.spread.amount = w;
    cmd.u.spread.height = hgt < 0.f ? 0.f : hgt;      /* an explicit extent is never "unset" */
    cmd_push(&c->cmds, &cmd);
}

/* Every ring-carried per-source knob in ONE command (bwa_source_apply). Fourteen single-knob
 * commands per spawn is real ring pressure — bwa_play_oneshot already documents dropping when the
 * ring is momentarily full — so a struct apply costs one slot, not fifteen.
 *
 * Sanitizing is the point of this half: the payload lands on the audio thread verbatim, so every
 * field takes the SAME control-thread guard its individual setter applies (isfinite first, then the
 * range clamp). A single-knob setter can afford to DROP a bad value, because dropping loses one
 * knob; dropping here would lose the whole configuration, so out-of-range values clamp to the
 * documented range instead. engine.c refuses a non-finite desc at the ABI boundary before it ever
 * reaches this — these guards are the backstop, not the diagnosis. */
void rt_source_apply_cfg(RtCore* c, uint32_t h, const RtSrcCfg* cfg) {
    if (!c || !cfg) return;
    Cmd cmd = { .type = CMD_SRC_CFG, .handle = h };

    float g = cfg->gain;
    if (!isfinite(g) || g < 0.f) g = 0.f; else if (g > BWA_MAX_GAIN) g = BWA_MAX_GAIN;
    cmd.u.cfg.gain = g;

    float p = cfg->pitch;
    if (!isfinite(p)) p = 1.f;                 /* NaN passes a two-sided clamp and sticks forever */
    cmd.u.cfg.pitch = p < 0.25f ? 0.25f : (p > 4.f ? 4.f : p);

    float s = cfg->spread;
    if (!isfinite(s) || s < 0.f) s = 0.f; else if (s > 1.f) s = 1.f;
    cmd.u.cfg.spread = s;

    float eh = cfg->extent_h;
    if (!isfinite(eh) || eh < 0.f) eh = -1.f;  /* < 0 = isotropic (the width covers both axes) */
    else if (eh > 1.f) eh = 1.f;
    cmd.u.cfg.extent_h = eh;

    float rad = cfg->size_m;
    if (!isfinite(rad) || rad < 0.f) rad = 0.f;
    cmd.u.cfg.size_m = rad;

    float rs = cfg->reverb_send;
    if (!isfinite(rs) || rs < 0.f) rs = 0.f; else if (rs > BWA_MAX_GAIN) rs = BWA_MAX_GAIN;
    cmd.u.cfg.rsend = rs;

    /* the attenuation override travels as a unit: a ref that does not enable it zeroes the rest, so
     * the audio thread never holds a half-set curve */
    float ar = cfg->atten_ref;
    if (!isfinite(ar) || ar <= 0.f) { cmd.u.cfg.aref = 0.f; cmd.u.cfg.aroll = 0.f; cmd.u.cfg.amin = 0.f; }
    else {
        float ro = cfg->atten_rolloff, mn = cfg->atten_min;
        if (!isfinite(ro) || ro < 0.f) ro = 0.f;
        if (!isfinite(mn) || mn < 0.f) mn = 0.f; else if (mn > 1.f) mn = 1.f;
        cmd.u.cfg.aref = ar; cmd.u.cfg.aroll = ro; cmd.u.cfg.amin = mn;
    }

    cmd.u.cfg.group = cfg->group < BWA_GROUPS ? (uint8_t)cfg->group : 0u;
    cmd.u.cfg.flags = (uint8_t)((cfg->doppler           ? BWA_CFG_DOPPLER : 0u) |
                                (cfg->air               ? BWA_CFG_AIR     : 0u) |
                                (cfg->loudness_comp     ? BWA_CFG_LDC     : 0u) |
                                (cfg->proximity         ? BWA_CFG_NF      : 0u) |
                                (cfg->early_reflections ? BWA_CFG_ISM     : 0u) |
                                (cfg->reverb            ? BWA_CFG_REVERB  : 0u) |
                                (cfg->reverb_distance   ? BWA_CFG_RDIST   : 0u) |
                                (cfg->pathing           ? BWA_CFG_PATH    : 0u));
    /* The group travels in this command too, so the control-side mirror rt_group_stop reads has to
     * move with it — on a push that LANDED, exactly as rt_source_set_group does. */
    if (cmd_push(&c->cmds, &cmd) && voice_live_ctrl(c, h)) c->group[BWA_H_IDX(h)] = cmd.u.cfg.group;
}

void rt_test_signal(RtCore* c, uint32_t channel, uint8_t kind, float gain) {
    if (!c) return;
    if (!isfinite(gain)) return;                /* injected post-align: a NaN here reaches the device */
    if (gain > BWA_MAX_GAIN) gain = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    Cmd cmd = { .type = CMD_TEST_SIGNAL };
    cmd.u.test.channel = channel; cmd.u.test.kind = kind; cmd.u.test.gain = gain;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    rt_source_play_at(c, h, sound, loop, 0);   /* 0 = start immediately */
}

/* The shared body of rt_source_play_at and rt_bed_play. `bed` is the kind the CALLER issued the
 * play as, and it exists only for the held (async) case: a reserved slot reports 0 channels, so
 * the kind guards at the ABI boundary have nothing to judge and the check has to happen again at
 * the publish. See rt_sound_publish. */
static void play_at_kind(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample, bool bed) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;          /* invalid or being unloaded: drop the play so the
                                             * audio thread can never bind a retiring sound */
    if (rt_source_is_push(c, h)) return;    /* a PUSH source plays what is pushed; rebinding it to an
                                             * asset would orphan its ring (engine.c reports the error) */
    held_drop_src(c, h);                    /* any newer play supersedes a play held for an async load */
    if (s->pending) { held_add(c, h, sound, loop, start_sample, 0, 0, bed); return; }   /* data not in yet */
    /* streamed sound: kick the background decode (re-seek + fill) now, off the audio thread. The
     * voice reads its ring from sample 0; the first blocks may be silent until the ring fills (~ms). */
    if (s->data.stream) stream_start(s->data.stream, loop ? 1 : 0);
    source_bind(c, h, sound, loop, start_sample, 0, 0);
}

void rt_source_play_at(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample) {
    play_at_kind(c, h, sound, loop, start_sample, false);   /* point source: a mono asset */
}

/* A bed is the SAME voice on the same path (engine.c's bwa_bed_* facade), so these differ from
 * rt_source_play/_play_at only in the kind they record. They have to be their own entry points
 * precisely because the voice cannot be asked afterwards which kind the caller meant. */
void rt_bed_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    play_at_kind(c, h, sound, loop, 0, true);
}
void rt_bed_play_at(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample) {
    play_at_kind(c, h, sound, loop, start_sample, true);
}

/* The shared body of rt_source_play_loop and rt_bed_play_loop. `bed` carries the caller's kind for
 * the same held-play reason play_at_kind does: a reserved slot reports 0 channels, so the ABI-level
 * kind guard has nothing to judge and rt_sound_publish re-checks at the publish. */
static void play_loop_kind(RtCore* c, uint32_t h, uint32_t sound, uint64_t loop_beg, uint64_t loop_end, bool bed) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;          /* invalid or being unloaded: drop the play (as play_at) */
    if (rt_source_is_push(c, h)) return;    /* a PUSH source plays what is pushed (engine.c reports the error) */
    held_drop_src(c, h);
    if (s->pending) { held_add(c, h, sound, true, 0, loop_beg, loop_end, bed); return; }   /* data not in yet */
    if (s->data.stream) stream_start(s->data.stream, 1);   /* streams loop the whole file — no region */
    source_bind(c, h, sound, true, 0, loop_beg, loop_end);  /* always looping; the region is the point */
}

void rt_source_play_loop(RtCore* c, uint32_t h, uint32_t sound, uint64_t loop_beg, uint64_t loop_end) {
    play_loop_kind(c, h, sound, loop_beg, loop_end, false);   /* point source: a mono asset */
}
void rt_bed_play_loop(RtCore* c, uint32_t h, uint32_t sound, uint64_t loop_beg, uint64_t loop_end) {
    play_loop_kind(c, h, sound, loop_beg, loop_end, true);
}

void rt_source_stop(RtCore* c, uint32_t h) {
    held_drop_src(c, h);               /* a stop must also cancel a play still waiting on an async load,
                                        * or the asset would start by itself when the decode lands */
    Stream* st = push_stream_ctrl(c, h);
    if (st) stream_push_end(st);       /* a push source cannot re-arm (play is refused), so stop ENDS it
                                        * like push_end: further pushes are refused instead of silently
                                        * feeding a ring nothing will ever drain again */
    Cmd cmd = { .type = CMD_STOP, .handle = h };
    cmd_push(&c->cmds, &cmd);
}

void rt_source_stop_at(RtCore* c, uint32_t h, uint64_t stop_sample) {
    Cmd cmd = { .type = CMD_STOP_AT, .handle = h };
    cmd.u.stopat.sample = stop_sample;
    cmd_push(&c->cmds, &cmd);           /* the audio thread fires the click-free fade at stop_sample */
}

void rt_source_queue(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring || s->pending) return;              /* invalid, being unloaded, or still loading:
                                                               * drop. A queue entry resolves to a SoundData
                                                               * pointer at bind time, so there is nothing
                                                               * to hold it against (engine.c reports it) */
    if (s->data.stream || s->data.channels != 1) return;      /* chaining is in-memory mono only */
    if (rt_source_is_push(c, h)) return;                      /* a push source plays what is pushed */
    Cmd cmd = { .type = CMD_QUEUE, .handle = h };
    cmd.u.enq.sound = sound; cmd.u.enq.loop = loop ? 1u : 0u;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_clear_queue(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_QUEUE_CLEAR, .handle = h };
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_paused(RtCore* c, uint32_t h, bool paused) {
    Cmd cmd = { .type = CMD_SET_PAUSED, .handle = h };
    cmd.u.pause.on = paused ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_seek(RtCore* c, uint32_t h, uint64_t frame) {
    if (!c) return;
    /* Held play: record it instead of enqueueing. Same reason as rt_source_set_region below. */
    HeldPlay* hp = held_find_src(c, h);
    if (hp) { hp->seek = frame; hp->seek_set = 1; return; }
    Cmd cmd = { .type = CMD_SEEK, .handle = h };
    cmd.u.seek.frame = frame;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_region(RtCore* c, uint32_t h, uint64_t start_frame, uint64_t end_frame) {
    if (!c) return;
    if (end_frame != 0 && end_frame <= start_frame) return;   /* degenerate: refused, not silently
                                                               * reinterpreted as the whole clip */
    /* A play HELD on a still-decoding asset has not reached the audio thread, so CMD_SET_REGION
     * would be consumed against an unbound voice (or the PREVIOUS asset), and the adoption's
     * source_bind would then reset both fields from the play call. Carry the region on the held
     * record instead, so the order every layer documents — play, THEN set the region — works on an
     * async handle too. It resolves identically either way: CMD_PLAY and CMD_SET_REGION bound
     * loop_beg/loop_end against the asset with the same two expressions. */
    HeldPlay* hp = held_find_src(c, h);
    if (hp) { hp->loop_beg = start_frame; hp->loop_end = end_frame; return; }
    Cmd cmd = { .type = CMD_SET_REGION, .handle = h };
    cmd.u.region.beg = start_frame; cmd.u.region.end = end_frame;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_channel(RtCore* c, uint32_t h, int channel) {
    if (!c) return;
    /* -1 is BWA_CHANNEL_AUTO (back to spatial panning) and is the ONLY negative accepted; any other
     * negative is refused rather than folded into AUTO, so a bad index cannot masquerade as
     * "restore panning" (engine.c reports the refusal). The channel count is fixed for the engine's
     * life, so the whole range is decidable right here on the control thread. */
    if (channel < -1 || channel >= (int)c->channels) return;
    Cmd cmd = { .type = CMD_SET_CHANNEL, .handle = h };
    cmd.u.outch.on = channel < 0 ? 0u : 1u;                       /* < 0 = back to spatial panning */
    cmd.u.outch.ch = channel < 0 ? 0u : (uint8_t)channel;
    cmd_push(&c->cmds, &cmd);
}

void rt_set_listener(RtCore* c, const float p[3], const float q[4]) {
    if (!p || !q) return;
    /* Same finite guard rt_source_set_pos has, and for a worse reason: a NaN listener NaNs EVERY
     * panner solve, and once NaN is in gcur the ramp x + (t-x)*k can never leave it, so it survives
     * until the engine restarts. The asymmetry with the source setter was the whole bug. */
    /* bwa_quat_unit normalizes, because finite is not enough here — see sane.h for why, and for the
     * degenerate-to-identity rule that keeps an unset orientation facing room-ahead. */
    float nq[4] = { q[0], q[1], q[2], q[3] };
    /* Bounded, not merely finite, for the same reason the quaternion is normalized rather than
     * merely finite-checked: the consumers square it. BWA_MAX_COORD (rt.h) carries the argument. */
    if (!(bwa_finite3_bounded(p, BWA_MAX_COORD) && bwa_quat_unit(nq))) return;
    Cmd cmd = { .type = CMD_SET_LISTENER };
    cmd.u.lis.px = p[0]; cmd.u.lis.py = p[1]; cmd.u.lis.pz = p[2];
    cmd.u.lis.qx = nq[0]; cmd.u.lis.qy = nq[1]; cmd.u.lis.qz = nq[2]; cmd.u.lis.qw = nq[3];
    cmd_push(&c->cmds, &cmd);
}

void rt_commit(RtCore* c) {
    Cmd cmd = { .type = CMD_COMMIT };
    cmd_push(&c->cmds, &cmd);
    drain_events(c);                    /* consume VOICE_ENDED / SOUND_RETIRED acks */
}

/* ---- assets (control thread; file I/O + alloc) ---- */

uint32_t rt_load_sound(RtCore* c, const char* path, char* err, size_t errcap) {
    SoundData d;
    if (!sound_load(path, c->sample_rate, &d, err, errcap)) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        sound_unload(&d);
        set_err(err, errcap, "sound: table full");
        return 0;
    }
    c->sounds[BWA_H_IDX(h)].data = d;     /* published to the audio thread when a CMD_PLAY references it */
    return h;
}

/* Load a sound for STREAMING (mono point source, engine rate): the file is not decoded into RAM —
 * a background thread feeds its ring as the voice plays (rt_source_play). 0 + err on failure. */
uint32_t rt_load_sound_streaming(RtCore* c, const char* path, char* err, size_t errcap) {
    if (!c) return 0;
    Stream* st = stream_open(c->streams, path, err, errcap);
    if (!st) return 0;
    return bind_stream_sound(c, st, "stream: sound table full", err, errcap);
}

/* Load a multichannel AmbiX asset into the sound table (plays as an ambisonic bed via mix_bed). */
static uint32_t load_bed_asset(RtCore* c, const char* path, char* err, size_t errcap, int fuma) {
    SoundData d;
    if (!(fuma ? sound_load_fuma(path, c->sample_rate, &d, err, errcap)
               : sound_load_ambix(path, c->sample_rate, &d, err, errcap))) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        sound_unload(&d);
        set_err(err, errcap, "ambix: sound table full");
        return 0;
    }
    c->sounds[BWA_H_IDX(h)].data = d;
    return h;
}
uint32_t rt_load_ambix(RtCore* c, const char* path, char* err, size_t errcap) {
    return load_bed_asset(c, path, err, errcap, 0);
}
uint32_t rt_load_fuma(RtCore* c, const char* path, char* err, size_t errcap) {
    return load_bed_asset(c, path, err, errcap, 1);   /* converted at load: past here a FuMa bed IS an AmbiX bed */
}

/* Channel count of a loaded asset (control thread): 1 = mono point source, 4/9/16 = ambisonic bed.
 * 0 if the handle is invalid. Lets the engine reject a type-mismatched play (mono on a bed / vice versa). */
uint16_t rt_sound_channels(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    return s ? s->data.channels : 0;
}

/* Length of a loaded asset in frames at the engine rate (control thread): in-memory = the decoded
 * length; streamed = the length the decoder reported at open (fixed there, so reading it here
 * doesn't race the streaming thread; 0 for push sources — open-ended). 0 also = invalid handle. */
uint64_t rt_sound_frames(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s) return 0;
    if (s->data.stream) return stream_total_frames(s->data.stream);
    return s->data.frames;
}

/* Is this a streamed (or push) sound (control thread)? A stream reports 1 channel like an in-memory
 * mono sound, so the channel count alone can't tell them apart — the queue path needs this to reject
 * a stream (chaining is in-memory only). False for an invalid handle. */
bool rt_sound_is_stream(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    return s && s->data.stream != NULL;
}

bool rt_unload_sound(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return true;  /* invalid or already retiring: idempotent no-op (nothing to retry) */
    /* A RESERVED (async, not yet published) slot must never ride CMD_SOUND_RETIRE - that is the
     * invariant the staging comment below states, and this is the function it names. Unreachable
     * through the public ABI today (a pending handle is always cache-owned, and bwa_unload_sound
     * refuses cache-owned handles), so this guards the next internal caller rather than a live bug.
     * rt_sound_abandon is the correct way to drop one. */
    if (s->pending) return true;
    held_drop_snd(c, sound);             /* a play waiting on this sound must not fire after the unload */
    s->retiring = 1;                     /* refuse new binds (rt_source_play checks this) */
    Cmd cmd = { .type = CMD_SOUND_RETIRE, .handle = sound };
    if (!cmd_push(&c->cmds, &cmd)) {     /* ring full: revert so the caller can retry (internal
                                          * handles park in retire_park — see retire_internal_sound) */
        s->retiring = 0;
        return false;
    }
    return true;
}

/* ---- async staging (control thread; see rt.h). The ONE ordering rule here: a reserved slot is
 * never handed to the audio thread. No CMD_PLAY / CMD_QUEUE / CMD_SOUND_RETIRE can carry a
 * pending handle, so the audio thread cannot hold a pointer into a slot rt_sound_publish is about
 * to write. The publish therefore needs no barrier of its own — the CMD_PLAY it re-issues is the
 * same release/acquire hand-off a synchronous load has always used. ---- */

uint32_t rt_sound_reserve(RtCore* c, char* err, size_t errcap) {
    if (!c) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) { set_err(err, errcap, "sound: table full"); return 0; }
    SoundSlot* s = &c->sounds[BWA_H_IDX(h)];
    memset(&s->data, 0, sizeof s->data);   /* empty until the publish; nothing may bind it meanwhile */
    s->pending = 1;
    return h;
}

bool rt_sound_pending(RtCore* c, uint32_t sound) {
    if (!c) return false;
    SoundSlot* s = sound_slot_ctrl(c, sound);
    return s && s->pending;
}

bool rt_sound_publish(RtCore* c, uint32_t sound, const SoundData* d) {
    if (!c || !d) return false;
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || !s->pending || s->retiring) return false;   /* stale/abandoned: the caller keeps the buffer */
    s->data = *d;                        /* ownership moves to the slot; the retire-ack frees it */
    s->pending = 0;
    /* The asset's KIND is only knowable now: a reserved slot reports 0 channels, which passes both
     * of the ABI's kind guards, so a play held against it was accepted on trust. Judge it here and
     * DROP a mismatch rather than bind it — binding a 4/9/16-channel bed to what the caller built as
     * a point source would render it as a bed (the mixer dispatches on the asset), silently voiding
     * spread, directivity and the panner the caller asked for. engine.c refuses this at the call
     * itself for every public path (the cache knows the load flags), so this is the backstop for an
     * asset whose channel count disagrees with the flags it was acquired under. */
    const bool is_bed = s->data.channels > 1;
    for (uint32_t i = 0; i < c->held_n; ) {               /* release the plays held for this sound */
        if (c->held[i].snd != sound) { ++i; continue; }
        HeldPlay p = c->held[i];
        c->held[i] = c->held[--c->held_n];                /* swap-remove: do NOT advance i */
        /* Liveness FIRST, then kind. A play whose source died would not have bound either way, so
         * counting it as a kind drop would raise a "the play was dropped" notice for a play that
         * was never going to sound - the counter has to mean exactly what the notice claims. */
        if (!voice_live_ctrl(c, p.src) || rt_source_is_push(c, p.src)) continue;   /* source died meanwhile */
        if ((p.bed != 0) != is_bed) { ++c->held_kind_drops; continue; }   /* wrong kind: drop, never bind */
        if (s->data.stream) stream_start(s->data.stream, p.loop ? 1 : 0);
        source_bind(c, p.src, p.snd, p.loop != 0, p.start, p.loop_beg, p.loop_end);
        /* A seek issued while the play was held replays here, AFTER the bind — the ring keeps the
         * order, so the audio thread sees exactly what a synchronous play-then-seek produces.
         * The entry is already off the held list, so this takes the ordinary command path. */
        if (p.seek_set) rt_source_seek(c, p.src, p.seek);
    }
    return true;
}

uint64_t rt_held_kind_drops(RtCore* c) {
    return c ? c->held_kind_drops : 0;
}

void rt_sound_abandon(RtCore* c, uint32_t sound) {
    if (!c) return;
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || !s->pending) return;       /* only a RESERVED slot may be dropped this way; a published
                                          * one has to go through the retire-ack (rt_unload_sound) */
    held_drop_snd(c, sound);
    srecycle_sound(c, BWA_H_IDX(sound));
}

/* Fire-and-forget: a transient voice that recycles itself on EVT_VOICE_ENDED. Its position
 * takes effect on the next rt_commit (the engine's per-frame commit). Returns whether the
 * oneshot was ACCEPTED — every early return here is a drop the caller cannot otherwise see. */
bool rt_play_oneshot(RtCore* c, uint32_t sound, float x, float y, float z, float gain) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring || s->pending) return false;   /* a oneshot owns no handle the caller could
                                                          * re-play, so a still-loading asset has nothing
                                                          * to hold the play against: it is refused */
    const float p3[3] = { x, y, z };   /* bounded, not merely finite: see BWA_MAX_COORD (rt.h). Checked
                                        * HERE as well as in rt_source_set_pos, because a rejected set_pos
                                        * would leave the transient voice playing at the origin instead. */
    if (!(bwa_finite3_bounded(p3, BWA_MAX_COORD) && isfinite(gain) && gain >= 0.f)) return false;
    if (gain > BWA_MAX_GAIN) gain = BWA_MAX_GAIN;   /* finite is not enough: an absurd gain overflows the bus to Inf */
    /* A oneshot enqueues 4 commands (CREATE/SET_POS/SET_GAIN/PLAY) that must all land, or
     * the transient voice is created-but-never-played and leaks (it never ends -> never
     * acks EVT_VOICE_ENDED -> never recycled). Reserve room for all 4 up front so a
     * ring-full case drops the whole oneshot rather than half of it. */
    if (cmd_free(&c->cmds) < 4) return false;
    uint32_t h = (c->free_count > c->fade_reserve) ? alloc_handle(c) : 0;   /* don't spend the steal reserve */
    if (!h) return false;                    /* full pool: the fire-and-forget oneshot is simply dropped */
    Cmd create = { .type = CMD_SRC_CREATE, .handle = h };
    cmd_push(&c->cmds, &create);              /* the 4 pushes are guaranteed by cmd_free >= 4 */
    rt_source_set_pos(c, h, x, y, z);
    rt_source_set_gain(c, h, gain);
    Cmd play = { .type = CMD_PLAY, .handle = h };
    play.u.play.sound = sound; play.u.play.loop = 0u; play.u.play.oneshot = 1u;
    cmd_push(&c->cmds, &play);
    return true;
}

/* ---- lifecycle ---- */

/* BWA_FADE_RESERVE (rt.h): extra physical slots beyond the caller's pool. A full-pool steal fades
 * the victim out on its own slot (one block) and places the new source on a reserve slot, so the
 * steal is click-free; the victim's slot returns to the pool when the fade completes. Bounds how
 * many steals per frame can be click-free (beyond it, a steal falls back to a hard cut). */

RtCore* rt_create(uint32_t req_voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels) {
    if (req_voice_cap == 0 || req_voice_cap > 0xFFFFu - BWA_FADE_RESERVE || sound_cap == 0 || sound_cap > 0xFFFFu ||
        channels  == 0 || channels  > BWA_CHANNELS || sample_rate == 0)
        return NULL;
    const uint32_t voice_cap = req_voice_cap + BWA_FADE_RESERVE;   /* physical slots (user pool + fade reserve) */
    /* Bound the event ring for the OWNERSHIP acks: between two control-thread drains the audio thread
     * emits at most one EVT_VOICE_ENDED per voice plus one EVT_SOUND_RETIRED per sound, so
     * EVT_CAP >= voice_cap + sound_cap keeps those un-overflowable (an ack can never be silently
     * dropped on the audio thread). EVT_VOICE_DONE is NOT bounded that way, which is exactly why it
     * goes through evt_push_notice and yields this much headroom instead of sharing it (so does
     * EVT_VOICE_LOOPED, which is unbounded for a stronger reason: several wraps per block). */
    if ((uint64_t)voice_cap + sound_cap > EVT_CAP) return NULL;
    RtCore* c = (RtCore*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->voice_cap   = voice_cap;
    c->fade_reserve = BWA_FADE_RESERVE;
    c->sound_cap   = sound_cap;
    c->channels    = channels;
    c->mix_nch     = channels;          /* point-voice gain width; rt_set_direct_ambi widens to SH */
    c->sample_rate = sample_rate;
    c->xover_a     = 1.f - expf(-6.2831853f * BWA_DUALBAND_FC / (float)sample_rate);   /* dual-band crossover */
    c->test_noise  = 0x9e3779b9u;       /* non-zero LCG seed for the channel-test noise */
    c->voices    = (Voice*)    calloc(voice_cap, sizeof(Voice));
    c->occ_handle = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->occ_val    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->occ_eq     = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->occ_dir    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->dir_pub    = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->play_pub   = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->pos_pub    = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->aux        = (float*)   calloc(BWA_RT_MAX_BLOCK, sizeof(float));   /* reflection aux-send scratch */
    c->stream_scratch = (float*)calloc(BWA_RT_MAX_BLOCK, sizeof(float));  /* per-block streaming pull scratch */
    c->path_accum = (float*)calloc((size_t)BWA_AMBI_CH * BWA_RT_MAX_BLOCK, sizeof(float));  /* pathing ambisonic accumulator */
    c->path_pub   = calloc((size_t)voice_cap * 2, sizeof *c->path_pub);  /* per-voice double-buffered path field */
    c->path_idx   = (_Atomic int*)calloc(voice_cap, sizeof(_Atomic int));
    c->streams    = stream_set_create(sample_rate);                     /* background file streaming */
    c->gen       = (uint16_t*) calloc(voice_cap, sizeof(uint16_t));
    c->inuse     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->priority  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->group     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->stealing  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->push_sound = (uint32_t*)calloc(voice_cap, sizeof(uint32_t));
    c->retire_park = (uint32_t*)calloc(sound_cap, sizeof(uint32_t));
    c->play_seq  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->ended_cap = voice_cap * 2u;   /* two generations of every slot between polls is ample */
    c->ended     = (uint32_t*)  calloc(c->ended_cap, sizeof(uint32_t));
    c->looped_cap = voice_cap * 4u;  /* deeper: a voice ENDS once, but a short region WRAPS repeatedly
                                      * between two polls. Past this it drops oldest and counts. */
    c->looped    = (uint32_t*)  calloc(c->looped_cap, sizeof(uint32_t));
    c->freelist  = (uint32_t*) calloc(voice_cap, sizeof(uint32_t));
    c->sounds    = (SoundSlot*)calloc(sound_cap, sizeof(SoundSlot));
    c->sfreelist = (uint32_t*) calloc(sound_cap, sizeof(uint32_t));
    {   /* Doppler ring pool: power-of-two ring per voice, sized to BWA_DOPPLER_MAX_DIST at this rate */
        uint32_t need = (uint32_t)(BWA_DOPPLER_MAX_DIST / BWA_SPEED_OF_SOUND * (float)sample_rate) + 2;
        uint32_t rl = bwa_pow2_ge(need);
        c->dop_ringlen = rl;
        c->dop_ring = (float*)calloc((size_t)voice_cap * rl, sizeof(float));
    }
    {   /* image-source reflection rings: one power-of-two ring per voice, sized to the longest
         * reflection path the renderer supports (BWA_ISM_MAX_M). Allocated with the voice pool — a
         * reflection is the direct signal delayed by its own path, so each voice needs its own. */
        uint32_t need = (uint32_t)(BWA_ISM_MAX_M / BWA_SPEED_OF_SOUND * (float)sample_rate) + 2;
        uint32_t rl = bwa_pow2_ge(need);
        c->ism_ringlen = rl;
        c->ism_ring = (float*)calloc((size_t)voice_cap * rl, sizeof(float));
        atomic_store_explicit(&c->ism_gain, 1.f, memory_order_relaxed);
    }
    {   /* per-channel velvet-noise decorrelators (bwa_set_decorrelation): BWA_DECOR_TAPS taps on a
         * jittered grid over BWA_DECOR_MS, exponential decay envelope, random signs, normalized to
         * unit ENERGY (decorrelated copies must carry the power the coherent copy gave up). A
         * different LCG seed per channel makes the speaker feeds mutually incoherent. */
        uint32_t span = (uint32_t)(BWA_DECOR_MS * 1e-3f * (float)sample_rate);
        uint32_t hl = bwa_pow2_ge(span + BWA_RT_MAX_BLOCK);
        c->dc_histlen = hl;
        c->dc_hmask   = hl - 1;
        c->dc_ntaps   = BWA_DECOR_TAPS;
        c->dc_bus  = (float*)calloc((size_t)BWA_CHANNELS * BWA_RT_MAX_BLOCK, sizeof(float));
        c->dc_hist = (float*)calloc((size_t)BWA_CHANNELS * hl, sizeof(float));
        const float grid = (float)span / (float)BWA_DECOR_TAPS;
        for (uint32_t ch = 0; ch < BWA_CHANNELS; ++ch) {
            uint32_t rng = 0x9E3779B9u * (ch + 1) + 12345u;
            double e2 = 0.0;
            for (uint32_t t = 0; t < BWA_DECOR_TAPS; ++t) {
                rng = rng * 1664525u + 1013904223u;
                float u = (float)(rng >> 8) * (1.0f / 16777216.0f);          /* [0,1) jitter */
                uint32_t off = (uint32_t)((float)t * grid + u * grid);
                if (off >= span) off = span - 1;
                c->dc_off[ch][t] = (uint16_t)off;
                rng = rng * 1664525u + 1013904223u;
                float sign = (rng & 0x10000u) ? 1.f : -1.f;
                float amp  = expf(-2.3f * (float)t / (float)BWA_DECOR_TAPS);  /* ~-20 dB across the span */
                c->dc_tamp[ch][t] = sign * amp;
                e2 += (double)amp * amp;
            }
            float norm = (float)(1.0 / sqrt(e2));
            for (uint32_t t = 0; t < BWA_DECOR_TAPS; ++t) c->dc_tamp[ch][t] *= norm;
        }
    }
    c->para = (ParaBed*)calloc(voice_cap, sizeof(ParaBed));   /* parametric-bed state (parallel to voices) */
    for (int x = 0; x < 3; ++x)                               /* band-splitter crossovers at the engine rate */
        c->para_xa[x] = 1.f - expf(-6.2831853f * BWA_PARA_XOVER[x] / (float)sample_rate);
    for (int x = 0; x < BWA_FS_XOVERS; ++x)                   /* spectral-widening splitter crossovers */
        c->fs_xa[x] = 1.f - expf(-6.2831853f * BWA_FS_XOVER[x] / (float)sample_rate);
    {   /* fs_w: integrate the DIGITAL splitter bands' cross-spectra over white noise (see fs_w decl).
         * B_k(w) from the actual one-poles, bands as the mixer forms them; 512 linear points span the
         * band. Runs once at create — plain double math, no DSP-path cost. */
        enum { FSW_M = 512 };
        double W[BWA_FS_BANDS][BWA_FS_BANDS] = { { 0 } };
        for (int m = 0; m < FSW_M; ++m) {
            const double wq = 3.14159265358979 * ((double)m + 0.5) / (double)FSW_M;   /* 0..pi (Nyquist) */
            double br[BWA_FS_BANDS], bi[BWA_FS_BANDS], pr = 0.0, pi_ = 0.0;
            for (int x = 0; x < BWA_FS_XOVERS; ++x) {
                const double a = c->fs_xa[x], b1 = 1.0 - a;
                const double dr = 1.0 - b1 * cos(wq), di = b1 * sin(wq);   /* 1 - (1-a) e^-jw */
                const double dn = dr * dr + di * di;
                const double lr = a * dr / dn, li = -a * di / dn;          /* LP_x(w) */
                br[x] = lr - pr; bi[x] = li - pi_;
                pr = lr; pi_ = li;
            }
            br[BWA_FS_XOVERS] = 1.0 - pr; bi[BWA_FS_XOVERS] = -pi_;
            for (int a2 = 0; a2 < BWA_FS_BANDS; ++a2)
                for (int b2 = 0; b2 < BWA_FS_BANDS; ++b2)
                    W[a2][b2] += br[a2] * br[b2] + bi[a2] * bi[b2];
        }
        for (int a2 = 0; a2 < BWA_FS_BANDS; ++a2)
            for (int b2 = 0; b2 < BWA_FS_BANDS; ++b2)
                c->fs_w[a2][b2] = (float)(W[a2][b2] / (double)FSW_M);
    }
    for (int o = 1; o <= 3; ++o)                              /* max-rE tapers per content order */
        ambi_max_re_weights(o, c->re_w[o - 1]);
    c->ldc_a = 1.f - expf(-6.2831853f * 250.f / (float)sample_rate);   /* loudness-comp shelf corner */
    c->nf_a  = 1.f - expf(-6.2831853f * BWA_NF_FC / (float)sample_rate);   /* near-field shelf corner */
    atomic_store_explicit(&c->sos, BWA_SPEED_OF_SOUND, memory_order_relaxed);
    if (!c->voices || !c->occ_handle || !c->occ_val || !c->occ_eq || !c->occ_dir || !c->dir_pub || !c->play_pub || !c->pos_pub || !c->aux ||
        !c->stream_scratch || !c->streams || !c->path_accum || !c->path_pub || !c->path_idx ||
        !c->gen || !c->inuse || !c->priority || !c->group || !c->stealing || !c->push_sound || !c->retire_park ||
        !c->play_seq || !c->ended || !c->looped || !c->freelist || !c->sounds || !c->sfreelist ||
        !c->dop_ring || !c->dc_bus || !c->dc_hist || !c->para || !c->ism_ring) {
        rt_destroy(c); return NULL;
    }
    const uint64_t eq_flat = eq_pack((float[3]){ 1.f, 1.f, 1.f });
    for (uint32_t i = 0; i < voice_cap; ++i) {   /* occlusion/directivity start clear (handle 0 = no publish) */
        atomic_store_explicit(&c->occ_val[i], 1.0f,    memory_order_relaxed);
        atomic_store_explicit(&c->occ_eq[i],  eq_flat, memory_order_relaxed);
        atomic_store_explicit(&c->occ_dir[i], 1.0f,    memory_order_relaxed);
    }
    for (uint32_t ch = 0; ch < BWA_CHANNELS; ++ch)   /* output meter starts silent */
        atomic_store_explicit(&c->chan_peak[ch], 0.f, memory_order_relaxed);
    /* precompute the 3 EQ band prototypes from the runtime sample rate (so the EQ is correct at
     * 48/96/192k). fc's: low-shelf 800, peaking at the geometric center, high-shelf 8000. */
    {
        const float fc[3]  = { 800.f, sqrtf(800.f * 8000.f), 8000.f };
        const int   ty[3]  = { EQ_LOWSHELF, EQ_PEAK, EQ_HIGHSHELF };
        const float two_pi = 6.28318530717958648f;
        for (int i = 0; i < 3; ++i) {
            float w0 = two_pi * fc[i] / (float)sample_rate, sw0 = sinf(w0);
            c->eq_proto[i].cw0   = cosf(w0);
            c->eq_proto[i].alpha = (ty[i] == EQ_PEAK) ? sw0 * ((8000.f - 800.f) / fc[1]) * 0.5f   /* band-edge Q */
                                                      : sw0 / (2.f * 0.70710678f);                /* shelf Q=0.707 */
            c->eq_proto[i].type  = ty[i];
        }
    }
    c->lis.q_active[3]  = 1.0f;        /* default head orientation = identity (facing +z) */
    c->lis.q_pending[3] = 1.0f;
    c->readback.q[3]    = 1.0f;        /* readback identity until the first block publishes */
    atomic_store_explicit(&c->room_eq_dyn, 1, memory_order_relaxed);   /* tracked room EQ: on when a grid is present */
    atomic_store_explicit(&c->lc_on, 0, memory_order_relaxed);         /* tracked alignment: OFF (opt-in) */
    atomic_store_explicit(&c->lc_dead_m, 0.f, memory_order_relaxed);   /* 0 = the built-in defaults */
    atomic_store_explicit(&c->lc_slew,   0.f, memory_order_relaxed);
    atomic_store_explicit(&c->master_gain, 1.f, memory_order_relaxed);
    atomic_store_explicit(&c->spcap_focus,   0.f, memory_order_relaxed);   /* 0 = the layout's own */
    atomic_store_explicit(&c->spcap_density, 0.f, memory_order_relaxed);   /* derived / constant default */
    atomic_store_explicit(&c->pan_gen, 0u, memory_order_relaxed);
    c->master_g_cur = 1.f;
    for (int j = 0; j < BWA_GROUPS; ++j) c->group_gain[j] = 1.f;        /* mix groups start at unity, unpaused */
    /* protection limiter: ON at -1 dBFS by default; ~1 ms attack / ~120 ms release one-poles */
    /* max-rE bed taper: ON. It was off through the bake-off and the offline evidence flipped it. The
     * layout tool's bed metric (bwa_bed_gains_batch, the engine's real AllRAD/EPAD builds) has the
     * taper winning EVERY axis on this array, under both decoders and both observer models, including
     * AT the sweet spot where classical theory says the plain decode should win: an irregular
     * 26-array's decode sidelobes bend rE even at center, and the taper suppresses them. The rig trial
     * now confirms rather than gates (docs/hardware-validation.md). Point-source panning is untouched;
     * this is the diffuse layer only. */
    atomic_store_explicit(&c->max_re, 1, memory_order_relaxed);
    atomic_store_explicit(&c->lim_on, 1, memory_order_relaxed);
    atomic_store_explicit(&c->lim_ceiling, 0.891251f, memory_order_relaxed);   /* 10^(-1/20) */
    c->lim_gain  = 1.0f;
    c->lim_att_a = 1.0f - expf(-1.0f / (0.001f * (float)sample_rate));
    c->lim_rel_a = 1.0f - expf(-1.0f / (0.120f * (float)sample_rate));
    c->layout  = layout_default();
    c->spcap_focus_blk   = c->layout.spcap_focus;     /* seed the block-resolved pair; rt_render */
    c->spcap_density_blk = c->layout.spcap_density;   /* re-resolves it every block */
    /* default listener POSITION = the layout's nominal listening point (re-set when the real
     * layout arrives) — a pose-less client listens from the array center, not the floor origin */
    memcpy(c->lis.p_active,  c->layout.ref, sizeof c->lis.p_active);
    memcpy(c->lis.p_pending, c->layout.ref, sizeof c->lis.p_pending);
    memcpy(c->readback.p,    c->layout.ref, sizeof c->readback.p);
    c->aligner = align_create(channels, &c->layout, sample_rate);
    if (!c->aligner) { rt_destroy(c); return NULL; }
    build_bed_decode(c);                        /* ambisonic bed decode from the default layout */
    /* push indices so the first alloc hands out slot 0, then 1, ... */
    for (uint32_t i = 0; i < voice_cap; ++i) c->freelist[c->free_count++]   = voice_cap - 1 - i;
    for (uint32_t i = 0; i < sound_cap; ++i) c->sfreelist[c->sfree_count++] = sound_cap - 1 - i;
    return c;
}

/* Replace the speaker layout. Control thread, call BEFORE bwa_start (or while stopped) —
 * it swaps the aligner the audio thread reads, so it is not safe concurrently with
 * rt_render. Voices recompute their DBAP gains on the next render (created dirty). */
void rt_set_layout(RtCore* c, const Layout* L) {
    if (!c || !L) return;
    Aligner* a = align_create(c->channels, L, c->sample_rate);
    if (!a) return;                         /* keep the old layout on alloc failure */
    c->layout = *L;
    align_destroy(c->aligner);
    c->aligner = a;
    /* re-default the listener to the new layout's nominal listening point (load-time: poses
     * pushed after start overwrite this every frame; a pose-less client hears from the center) */
    memcpy(c->lis.p_active,  c->layout.ref, sizeof c->lis.p_active);
    memcpy(c->lis.p_pending, c->layout.ref, sizeof c->lis.p_pending);
    memcpy(c->readback.p,    c->layout.ref, sizeof c->readback.p);
    build_bed_decode(c);                         /* re-derive the bed decode for the new geometry */
    c->rq_state = 0;                             /* new aligner starts flat: re-send the room-EQ targets */
    c->lc_state = 0;                             /* ... and the tracked-alignment targets */
    c->layout_gen++;                             /* the SPCAP cache self-invalidates on the next gains call */
    for (uint32_t i = 0; i < c->voice_cap; ++i)
        if (c->voices[i].active) c->voices[i].dirty = true;
}

/* Direct-binaural mode (BWA_PROFILE_BINAURAL): route point voices onto the 16-ch SH accumulator
 * (and, mode 2, their point share onto per-voice mono taps) instead of the speaker panner — see
 * the rt.h contract. Call while the audio thread is STOPPED: the engine sets mode 1 at create and
 * re-decides 1-vs-2 at each bwa_start (per the steam monitor); voices carry per-channel gain state
 * whose MEANING changes with the mode, so a change re-dirties every voice (the ramp machinery then
 * crossfades old-meaning gcur onto new-meaning targets in one block — click-free). */
void rt_set_direct_ambi(RtCore* c, int mode) {
    if (!c) return;
    if (mode < 0) mode = 0; else if (mode > 2) mode = 2;
    if (mode > 0 && !c->ambi_direct)
        c->ambi_direct = (float*)calloc((size_t)BWA_AMBI_CH * BWA_RT_MAX_BLOCK, sizeof(float));
    if (mode == 2 && !c->dv_mono) {
        c->dv_mono = (float*)calloc((size_t)c->voice_cap * BWA_RT_MAX_BLOCK, sizeof(float));
        c->dv_view = (RtDirectVoice*)calloc(c->voice_cap, sizeof *c->dv_view);
    }
    if (mode > 0 && !c->ambi_direct) mode = 0;              /* alloc failure: stay on the speaker path */
    if (mode == 2 && (!c->dv_mono || !c->dv_view)) mode = 1;
    if (mode != c->direct_on)
        for (uint32_t i = 0; i < c->voice_cap; ++i)
            if (c->voices[i].active) c->voices[i].dirty = true;
    c->direct_on = mode;
    c->mix_nch = mode ? BWA_AMBI_CH : c->channels;
}

/* Audio thread, after rt_render: the block's summed direct-binaural SH field (phonon monitor
 * basis, BWA_AMBI_CH planar channels of the block's nframes). NULL unless direct mode is on. */
const float* rt_direct_ambi(RtCore* c) {
    return (c && c->direct_on) ? c->ambi_direct : NULL;
}

/* Audio thread, after rt_render (mode 2): the per-slot point-tap view. Slots marked active carry
 * this block's mono point share + direction; consume before the next rt_render. */
uint32_t rt_direct_voices(RtCore* c, const RtDirectVoice** out) {
    if (!c || c->direct_on != 2 || !c->dv_view) { if (out) *out = NULL; return 0; }
    if (out) *out = c->dv_view;
    return c->voice_cap;
}

/* Select the panner: 0 = DBAP (default, moving observer), 1 = SPCAP, 2 = VBAP (both fixed observer).
 * Atomic-release store, and the SPCAP/VBAP caches self-invalidate on listener/layout change, so this
 * is safe to call at runtime (live A/B) as well as before bwa_start. */
void rt_set_panner(RtCore* c, int panner) {
    if (!c) return;
    atomic_store_explicit(&c->panner, (panner >= 0 && panner <= 2) ? panner : 0, memory_order_release);
}

/* Dual-band panning: 0 = off (power panning across the band), 1 = on (amplitude below BWA_DUALBAND_FC,
 * power above). The low-band gains are computed every gain solve, so this is a live A/B atomic. */
void rt_set_dual_band(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->dual_band, on ? 1 : 0, memory_order_release);
}

/* Compensated amplitude panning on the dual-band low band (cap.c). Inert unless dual-band is on —
 * that is the only consumer of gtarget_lo — so the two toggles compose rather than one implying the
 * other.
 *
 * Two reasons this needs the pan_gen bump rather than a plain store (same argument as
 * rt_set_spcap_focus): the flag rewrites the low band of EVERY source, so a motionless scene would
 * otherwise stay deaf to the toggle; and turning CAP ON is what starts head ORIENTATION dirtying
 * voices at CMD_COMMIT, so without the bump the first re-solve would wait for the listener to
 * translate. Release-ordered so a voice cannot see the new generation with the old flag. */
void rt_set_cap(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->cap_on, on ? 1 : 0, memory_order_release);
    atomic_fetch_add_explicit(&c->pan_gen, 1u, memory_order_release);
}

/* Spread rendering: 0 = lobe reshape (default), 1 = MDAP virtual-source ring, 2 = spectral
 * (frequency-dependent panning). Read per gain solve, so it is a live A/B atomic like the panner;
 * voices with spread 0 are unaffected either way. */
void rt_set_spread_mode(RtCore* c, int mode) {
    if (!c) return;
    atomic_store_explicit(&c->spread_mode, (mode >= 0 && mode <= 2) ? mode : 0, memory_order_release);
}

/* max-rE bed-decode weighting: crossfaded per bed voice (re_mix) and in the FDN's render pair, so
 * this is a click-free live A/B like dual-band. Off by default (the unweighted decode is the
 * incumbent); bake the winner after the hardware bake-off. */
void rt_set_max_re(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->max_re, on ? 1 : 0, memory_order_release);
}

/* Band-split max-rE: with max_re on, taper only above the 700 Hz crossover and keep the rV-optimal
 * unweighted decode below (the literature-standard Gerzon basic-LF/max-rE-HF split; the broadband
 * taper is the incumbent A/B side). The split share ramps per bed voice (re_sm), so the toggle is
 * click-free. No effect while max_re is off; the FDN's render stays broadband either way. */
void rt_set_max_re_split(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->max_re_split, on ? 1 : 0, memory_order_release);
}

/* Tracked room EQ (layouts with a room_eq_grid): default ON; off slews every section to flat, so
 * the toggle is a click-free live A/B. A no-op for layouts without a grid. */
void rt_set_room_eq_dyn(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->room_eq_dyn, on ? 1 : 0, memory_order_release);
}

/* Tracked listener alignment (listener_align_track): default OFF; off slews every channel back to the
 * layout's own trims, so the toggle is a click-free live A/B. Either knob <= 0 reverts it to the
 * built-in default. Publish-then-flag: the knobs land BEFORE the release store on the enable, and
 * listener_align_track acquires the enable before loading them (CLAUDE.md's ordering trap). */
void rt_set_tracked_align(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->lc_on, on ? 1 : 0, memory_order_release);
}

/* The guards, separately from the enable (bwa_set_tracked_align_guards). Safe in either order and
 * safe while running: listener_align_track acquires lc_on and then re-reads both of these RELAXED
 * every block, so setting a guard after the enable costs at most one block of the old value. That is
 * unlike the SPCAP knobs, where a lagging read would be stamped current and swallowed until the next
 * bump, which is why those need the pan_gen generation and these do not. */
void rt_set_tracked_align_guards(RtCore* c, float dead_zone_m, float slew_frames_per_s) {
    if (!c) return;
    if (!(dead_zone_m > 0.f))       dead_zone_m = 0.f;         /* NaN-safe: 0 = the default */
    else if (dead_zone_m > 0.5f)    dead_zone_m = 0.5f;        /* half a meter of slack is already useless */
    if (!(slew_frames_per_s > 0.f)) slew_frames_per_s = 0.f;
    else if (slew_frames_per_s > 4096.f) slew_frames_per_s = 4096.f;
    atomic_store_explicit(&c->lc_dead_m, dead_zone_m,       memory_order_relaxed);
    atomic_store_explicit(&c->lc_slew,   slew_frames_per_s, memory_order_relaxed);
}

/* Decorrelation (velvet-noise path) for spread sources' wide part (and the parametric bed's diffuse
 * stream, which uses the same bus unconditionally). The per-voice split amplitude ramps toward the
 * toggle, so this is a click-free live A/B like the panner/dual-band switches. */
void rt_set_decorrelation(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->decor_on, on ? 1 : 0, memory_order_release);
}

/* Bed renderer: 0 = matrix decode (default), 1 = parametric (first-order DirAC in coarse bands, the
 * direct stream re-panned listener-relative). Each bed voice's `mix` ramps toward the selection, so
 * the switch is a click-free live A/B. Beds with fewer than 4 channels stay on the matrix. */
void rt_set_bed_renderer(RtCore* c, int parametric) {
    if (!c) return;
    atomic_store_explicit(&c->bed_param, parametric ? 1 : 0, memory_order_release);
}

/* Pose prediction: extrapolate the tracked position by `lead_s` seconds along the smoothed velocity
 * (velocity from the tracker's own timestamps). 0 disables (default). Live-safe atomic. */
void rt_set_pose_prediction(RtCore* c, float lead_s) {
    if (!c) return;
    if (!(lead_s > 0.f)) lead_s = 0.f; else if (lead_s > 0.2f) lead_s = 0.2f;   /* a lead past ~200 ms is overshoot, not latency hiding */
    atomic_store_explicit(&c->pred_lead, lead_s, memory_order_relaxed);
}

/* Master gain: one ramped scalar over the whole mix (pre-align). Live-safe atomic; the render ramps
 * toward it across the block, so a slider drag never zippers. */
void rt_set_master_gain(RtCore* c, float linear) {
    if (!c) return;
    if (!isfinite(linear)) return;              /* NaN slips a `< 0` clamp and NaNs the whole bus */
    if (linear < 0.f) linear = 0.f;
    /* Finite is not enough: `bus * 3e38` OVERFLOWS to +/-Inf. The gain is sticky, so EVERY later
     * block overflows too, and the Inf reaches the align delay line and (when room EQ is on) the
     * biquad state, where an IIR feedback path holds it past any later correction. Cap at +80 dB —
     * far past any musical use, and overflow-safe against any sample the bus legitimately carries.
     * Found by test_fuzz_api seed 12648430: isfinite(3e38) is TRUE, so the reject-guard above
     * passes it. Finite-but-absurd is a separate class from non-finite; see BWA_MAX_GAIN. */
    if (linear > BWA_MAX_GAIN) linear = BWA_MAX_GAIN;
    atomic_store_explicit(&c->master_gain, linear, memory_order_relaxed);
}

/* Global pause: every voice's pause_gate ramps out and freezes its playhead (memory/stream/bed alike);
 * resume continues exactly. Paused voices still read as playing, like per-voice pause. Live. */
void rt_set_all_paused(RtCore* c, int paused) {
    if (!c) return;
    atomic_store_explicit(&c->all_paused, paused ? 1 : 0, memory_order_release);
}

uint32_t rt_active_voices(RtCore* c) {
    return c ? atomic_load_explicit(&c->active_pub, memory_order_relaxed) : 0;
}

uint64_t rt_stream_starves(RtCore* c) {
    return c ? atomic_load_explicit(&c->strm_starves, memory_order_relaxed) : 0;
}

/* Near-listener widening: floor every source's spread at 1 - dist/radius. 0 disables (default).
 * Live-safe atomic; the gains it changes ramp like any solve. */
void rt_set_near_spread(RtCore* c, float radius_m) {
    if (!c) return;
    if (!(radius_m > 0.f)) radius_m = 0.f; else if (radius_m > 10.f) radius_m = 10.f;   /* NaN-safe */
    atomic_store_explicit(&c->near_spread, radius_m, memory_order_relaxed);
}

/* Hole-aware spread floor (hole.h): floor a source's spread by how far its bearing sits from the
 * nearest speaker, so a source aimed into an array hole renders as an honest WIDE source instead of
 * a split image across the hull triangle that closes the hole. `strength` scales the derived floor;
 * 0 disables (the default, so an array with no holes and a caller who never sets it are both
 * bit-identical to before). Clamped to 2 — past that the floor stops being an honest width.
 *
 * Bumps pan_gen for the same reason rt_set_spcap_focus does: this changes the gain vector of every
 * source whose bearing is in a hole, INCLUDING ones that never move, and a live setter must not
 * write v->dirty (invariant 3 — the audio thread owns the voice table). The store is release-ordered
 * ahead of the bump and rt_render acquires pan_gen before latching hole_spread_blk, so a block can
 * never pair the new generation with the old strength and swallow the change. */
void rt_set_hole_spread(RtCore* c, float strength) {
    if (!c) return;
    if (!(strength > 0.f)) strength = 0.f; else if (strength > 2.f) strength = 2.f;   /* NaN-safe */
    atomic_store_explicit(&c->hole_spread, strength, memory_order_release);
    atomic_fetch_add_explicit(&c->pan_gen, 1u, memory_order_release);
}

/* SPCAP tuning: lobe sharpness + placement-correction density exponent, both dimensionless. Either
 * <= 0 reverts THAT knob to the layout's default (the geometry-derived focus, the constant density);
 * the fallback is resolved per block, so it survives a later rt_set_layout. Live-safe: the values
 * are plain atomics and the gains they move ramp like any solve.
 *
 * The bump is the point. Focus rewrites the gain vector of every source, static ones included, so a
 * dirty-only gate would leave a motionless scene deaf to the knob — and this setter cannot write
 * v->dirty the way rt_set_layout does, because that is a control-thread write to audio-thread state
 * and is legal there only because the audio thread is stopped. Bumping pan_gen instead lets the
 * MIXER notice the change and re-solve, on its own thread. Release-ordered so a voice can never see
 * the new generation with the old focus (it would stamp itself current and swallow the change). */
void rt_set_spcap_focus(RtCore* c, float focus, float density) {
    if (!c) return;
    if (focus > 0.f)   { if (focus < 1.f) focus = 1.f; else if (focus > 64.f) focus = 64.f; }
    else                 focus = 0.f;                    /* sentinel: back to the derived default */
    if (density > 0.f) { if (density > 16.f) density = 16.f; }
    else                 density = 0.f;                  /* sentinel: back to the constant default */
    atomic_store_explicit(&c->spcap_focus,   focus,   memory_order_relaxed);
    atomic_store_explicit(&c->spcap_density, density, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->pan_gen, 1u, memory_order_release);
}

/* Per-voice equal-loudness distance compensation (enqueue-only; the mixer ramps the shelf). */
void rt_source_set_loudness_comp(RtCore* c, uint32_t h, bool on) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_LDC, .handle = h };
    cmd.u.ldc.on = on ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

/* Per-voice near-field proximity boost (enqueue-only; the mixer ramps the shelf). */
void rt_source_set_proximity(RtCore* c, uint32_t h, bool on) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_NF, .handle = h };
    cmd.u.nf.on = on ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

/* Manual directivity (enqueue-only): the forward axis is normalized here (control thread), the
 * pattern clamped to phonon's ranges; weight 0 disables. The mixer evaluates + ramps per block. */
void rt_source_set_directivity_manual(RtCore* c, uint32_t h, const float fwd[3], float weight, float power) {
    if (!c || !fwd || !isfinite(weight) || !isfinite(power)) return;
    Cmd cmd = { .type = CMD_SET_DIR, .handle = h };
    const float nrm = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (!(nrm > 1e-6f)) {                                       /* degenerate/NaN forward -> room ahead */
        cmd.u.dir.fwd[0] = 0.f; cmd.u.dir.fwd[1] = 0.f; cmd.u.dir.fwd[2] = 1.f;
    } else {
        cmd.u.dir.fwd[0] = fwd[0]/nrm; cmd.u.dir.fwd[1] = fwd[1]/nrm; cmd.u.dir.fwd[2] = fwd[2]/nrm;
    }
    if (weight < 0.f) weight = 0.f; else if (weight > 1.f)  weight = 1.f;
    if (power  < 0.f) power  = 0.f; else if (power  > 64.f) power  = 64.f;
    cmd.u.dir.weight = weight; cmd.u.dir.power = power;
    cmd_push(&c->cmds, &cmd);
}

/* Engine-wide speed of sound (m/s; live). Atomic store; mix_voice re-reads it per block, and both
 * delay users glide toward their new targets — a change bends, never steps (invariant 4). */
void rt_set_speed_of_sound(RtCore* c, float mps) {
    if (!c || !bwa_finite_clamp(&mps, BWA_SOS_MIN, BWA_SOS_MAX)) return;
    atomic_store_explicit(&c->sos, mps, memory_order_relaxed);
}

/* Extra (compromise) listener positions — multi-listener panning. Latest-wins, commit-gated like
 * the pose; n = 0 restores single-listener panning. Control thread, enqueue-only. */
void rt_set_extra_listeners(RtCore* c, const float* xyz, uint32_t n) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_EXTRA_LIS, .handle = 0 };
    if (n > BWA_EXTRA_LIS) n = BWA_EXTRA_LIS;
    memset(cmd.u.exlis.p, 0, sizeof cmd.u.exlis.p);
    /* Same finite guard the primary listener and the source setter have. INF is the dangerous one
     * here rather than NaN: dbap_gains computes inv = 1/Inf = 0 and then -Inf * 0 = NaN, which
     * survives both clamp branches and lands in the gain vector. */
    if (xyz) for (uint32_t j = 0; j < n; ++j) {
        if (!bwa_finite3_bounded(&xyz[j*3], BWA_MAX_COORD)) return;
        memcpy(cmd.u.exlis.p[j], &xyz[j * 3], sizeof(float) * 3);
    }
    cmd.u.exlis.n = (uint8_t)(xyz ? n : 0);
    cmd_push(&c->cmds, &cmd);
}

void rt_set_limiter(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->lim_on, on ? 1 : 0, memory_order_release);
}

void rt_set_limiter_ceiling(RtCore* c, float ceiling_linear) {
    if (!c) return;
    if (ceiling_linear < 0.001f) ceiling_linear = 0.001f;   /* -60 dB floor: 0 would silence everything */
    if (ceiling_linear > 1.0f)   ceiling_linear = 1.0f;
    atomic_store_explicit(&c->lim_ceiling, ceiling_linear, memory_order_release);
}

/* Select the diffuse-bed decoder: 0 = sampling (SAD, default), 1 = AllRAD. Rebuilds the decode matrix
 * the audio thread reads, so call BEFORE bwa_start (or while stopped), like rt_set_layout. */
void rt_set_bed_decoder(RtCore* c, int decoder) {
    if (!c) return;
    c->bed_decoder = (decoder == 1 || decoder == 2) ? decoder : 0;
    build_bed_decode(c);
}

void rt_set_tracker(RtCore* c, const PoseSlot* slot) {
    /* runtime-safe: the audio thread loads this once per block (acquire pairs with this release).
     * The CALLER must keep the old slot's memory alive until any in-flight block has finished
     * (engine.c sleeps a couple of block periods before closing the old NatNet). */
    if (c) atomic_store_explicit(&c->tracker, slot, memory_order_release);
}

void rt_set_bus_tap(RtCore* c, RtBusTap tap, void* ud) {
    if (!c) return;
    c->bus_tap_ud = ud;                                          /* publish ud BEFORE the tap ... */
    atomic_store_explicit(&c->bus_tap, tap, memory_order_release);   /* ... so an acquire-load of a non-NULL tap sees it */
}

/* Publish a voice's occlusion transmittance (1 = clear, 0 = blocked). Called from the off-thread
 * occlusion sim, NOT the control thread — so it touches no rings/free-list and (crucially) NONE of
 * the audio-thread-owned Voice fields: it just deposits (handle, value) into the atomic publish
 * slot. The audio thread gates on its own generation when it consumes, so a stale/recycled handle
 * is dropped there (race-free, since the sim never reads v->gen/v->active). */
/* Unified per-source direct-effect publish: broadband `level`, 3-band transmission `bands` (each in
 * [0,1], normalized so the loudest is 1 — the audio thread's EQ tilt), and a `dir` directivity gain.
 * occ_handle is written LAST with release, so a publish for a DIFFERENT occupant is never half-applied
 * (the audio thread's gen-gate flips atomically with the handle). For a LIVE voice the handle is
 * unchanged across re-publishes, so the three fields can be observed one 30 Hz tick apart (e.g. level
 * from update N, bands/dir from N+1) — a bounded cross-field staleness that is harmless because the
 * audio thread ramps/glides all three toward their targets every block (no jump). */
void rt_set_direct(RtCore* c, uint32_t handle, float level, const float bands[3], float dir) {
    if (!c) return;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    /* NaN-safe, and this is the backstop that matters: these values arrive from the SIM thread,
     * so they fence whatever the ray tracer computed from a degenerate scene as well as whatever
     * the caller passed. `x < lo` reads false for NaN and let it straight into a mixer gain. */
    if (!(level > 0.f)) level = 0.f; else if (level > 1.f) level = 1.f;
    if (!(dir   > 0.f)) dir   = 0.f; else if (dir   > 1.f) dir   = 1.f;
    atomic_store_explicit(&c->occ_val[idx],  level,          memory_order_relaxed);
    atomic_store_explicit(&c->occ_eq[idx],   eq_pack(bands), memory_order_relaxed);
    atomic_store_explicit(&c->occ_dir[idx],  dir,            memory_order_relaxed);
    atomic_store_explicit(&c->occ_handle[idx], handle,       memory_order_release);
}

void rt_set_occlusion(RtCore* c, uint32_t handle, float transmittance) {
    const float flat[3] = { 1.f, 1.f, 1.f };
    rt_set_direct(c, handle, transmittance, flat, 1.f);    /* broadband level, no tilt, omni */
}
void rt_set_occlusion_eq(RtCore* c, uint32_t handle, float level, const float band_gains[3]) {
    rt_set_direct(c, handle, level, band_gains, 1.f);      /* level + tilt, omni */
}

/* Read back a voice's published occlusion factor (1 = clear) — for HUD/diagnostics; 1 if the latest
 * publish was for a different occupant. Race-free: it reads the atomic publish slot plus the
 * control-thread-owned handle table, and touches no audio-thread state.
 *
 * Control-side liveness FIRST, the same gate rt_source_is_playing and rt_source_get_position use,
 * and for a WORSE version of their reason. Nothing ever rewrites occ_val on a destroy: the slot
 * keeps whatever the sim last wrote under this exact handle, so an ungated read reports a
 * destroyed source's occlusion FOREVER, not merely until the next block. The publishers all key
 * off a handle that came from rt_source_create — the occlusion sim snapshots steam_scene's shadow,
 * whose handles arrive through the bwa_source_* setters, and manual occlusion takes a bwa_source
 * too — so the gate discards nothing a caller could still want. A sim tick that lands just after a
 * destroy is describing a source the caller has already given up, and 1 (clear) is the same answer
 * the sim itself publishes when it tears a source down. */
float rt_get_occlusion(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    if (!voice_live_ctrl(c, handle)) return 1.f;              /* destroyed, stale, or recycled handle */
    return (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
         ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.f;
}

/* Read back the published directivity gain (1 = on-axis/omni) — for HUD/diagnostics. The sim's
 * publish (occ_dir) wins while it owns the voice; otherwise the mixer's manual-dipole publish
 * (dir_pub, handle-gated in the same word) reports the audio thread's own evaluation. */
float rt_get_directivity(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BWA_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    /* Control-side liveness FIRST, once, for BOTH branches — see rt_get_occlusion for why the sim's
     * slot needs it and rt_source_get_position for why the mixer's does. Both publish slots outlive
     * the voice (the sim never republishes for a handle it stopped simulating; CMD_SRC_DESTROY
     * clears dir_pub only when the audio thread gets to it), so gating here is what makes a
     * destroyed source read omni IMMEDIATELY, the same instant is_playing goes false. */
    if (!voice_live_ctrl(c, handle)) return 1.f;              /* destroyed, stale, or recycled handle */
    if (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
        return atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed);
    const uint64_t dp = atomic_load_explicit(&c->dir_pub[idx], memory_order_relaxed);
    if ((uint32_t)(dp >> 32) == handle) {
        float g; uint32_t fb = (uint32_t)dp; memcpy(&g, &fb, sizeof g);
        return g;
    }
    return 1.f;
}

void rt_destroy(RtCore* c) {
    if (!c) return;
    align_destroy(c->aligner);
    if (c->sounds)                                  /* free any pcm still loaded */
        for (uint32_t i = 0; i < c->sound_cap; ++i)
            if (c->sounds[i].inuse) sound_unload(&c->sounds[i].data);
    free(c->sfreelist);
    free(c->sounds);
    free(c->freelist);
    free(c->stealing);
    free(c->priority);
    free(c->group);
    free(c->push_sound);
    free(c->retire_park);
    free(c->play_seq);
    free(c->ended);
    free(c->looped);
    free(c->inuse);
    free(c->gen);
    free(c->dop_ring);
    free(c->ism_ring);
    free(c->dc_bus);
    free(c->dc_hist);
    free(c->para);
    stream_set_destroy(c->streams);     /* stops the streaming thread, releases every open stream + ring */
    free(c->stream_scratch);
    free(c->ambi_direct);
    free(c->path_accum);
    free(c->path_pub);
    free((void*)c->path_idx);
    free(c->aux);
    free((void*)c->dir_pub);
    free((void*)c->occ_dir);            /* cast drops the _Atomic qualifier for free() */
    free((void*)c->pos_pub);
    free((void*)c->play_pub);
    free((void*)c->occ_eq);
    free((void*)c->occ_val);
    free((void*)c->occ_handle);
    free(c->voices);
    free(c);
}
