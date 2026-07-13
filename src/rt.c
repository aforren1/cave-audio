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
#include "align.h"
#include "ism.h"          /* image-source early reflections (shoebox geometry; phonon-free) */
#include "biquad.h"       /* shared RBJ cookbook (also used by align.c's room_eq) */
#include "ambisonics.h"   /* SH->26 decode for ambisonic beds */
#include "allrad.h"       /* robust SH->26 decode for irregular arrays */
#include "profile.h"      /* Tracy zones/plots (no-ops unless BWAUDIO_TRACY=ON) */

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
#define BW_RT_MAX_BLOCK 8192   /* aux-send scratch cap; must be >= any device block (matches engine's BW_MAX_BLOCK) */

/* propagation effects (opt-in, per voice) */
#define BW_SPEED_OF_SOUND   343.0f    /* m/s */
#define BW_DOPPLER_MAX_DIST 8.0f      /* propagation delay saturates past this (bounds the per-voice ring) */
#define BW_DOPPLER_TAU      0.032f    /* per-pole delay low-pass time (s); FFT-tuned, see mix_voice */
#define BW_AIR_FC_NEAR   18000.0f     /* air-absorption low-pass cutoff (Hz) at zero distance ... */
#define BW_AIR_FC_PER_M    650.0f     /* ... falling this many Hz per metre ... */
#define BW_AIR_FC_FLOOR   1200.0f     /* ... down to this floor */
/* distance->reverb send: the wet-send factor ramps from NEAR_SEND at NEAR_DIST to 1.0 at FAR_DIST */
#define BW_REFL_NEAR_DIST  1.0f
#define BW_REFL_FAR_DIST   6.0f
#define BW_REFL_NEAR_SEND  0.25f
#define BW_DUALBAND_FC     700.0f     /* dual-band panning crossover (Hz): amplitude below, power above */
/* decorrelation (bw_set_decorrelation): per-channel velvet-noise filters over BW_DECOR_MS with
 * BW_DECOR_TAPS sparse taps — ~30 MACs/sample/channel, time-domain, no FFT, no onset latency
 * (Valimaki/Schlecht et al., Velvet-Noise Decorrelator, DAFx-17/18). */
#define BW_DECOR_TAPS 30
#define BW_DECOR_MS   30.0f
/* parametric bed renderer (bw_set_bed_renderer): FOA intensity-vector analysis in BW_PARA_BANDS
 * time-domain bands (one-pole crossovers at BW_PARA_XOVER Hz), direction + diffuseness per band —
 * first-order DirAC (Pulkki) with block-rate parameters instead of an STFT. */
#define BW_PARA_BANDS 4
#define BW_PARA_TAU   0.060f          /* intensity/energy smoothing time (s) */
static const float BW_PARA_XOVER[3] = { 200.f, 800.f, 3200.f };
#define BW_BED_YAW_RATE 6.2831853f    /* bed-rotation glide (rad/s): one full turn per second */
/* image-source early reflections (bw_source_set_early_reflections; ism.c): each of the room's six
 * first-order images is rendered as a POINT SOURCE through the listener-relative panner — correct
 * direction AND parallax as the listener walks. The late tail is the FDN's job (fdn.c). */
#define BW_ISM_MAX_M    60.0f         /* longest reflection path the per-voice ring holds (bounds the delay) */
#define BW_ISM_TAU      0.020f        /* per-image delay glide (s): a moving source bends, never steps */

typedef struct { Cmd slots[RING_CAP]; _Atomic uint32_t write, read; } CmdRing;
typedef struct { Evt slots[EVT_CAP];  _Atomic uint32_t write, read; } EvtRing;
typedef struct { uint32_t handle; float sh[BW_AMBI_CH]; float eq[3]; } PathPub;   /* one double-buffer slot of a voice's path field (directions + bending-loss band tilt) */

typedef struct {
    uint16_t gen;
    bool     active, playing, loop, dirty, oneshot;
    const SoundData* sound;                 /* bound sound (NULL when idle); audio reads pcm */
    uint32_t cursor;                        /* sample cursor into sound->pcm (in-memory sounds) */
    uint64_t stream_pos;                    /* absolute sample position into a streamed sound's ring */
    uint64_t start_sample;                  /* dsp-sample to begin output (0 = immediate); for scheduled play */
    float    pos_pending[3], pos_active[3];
    float    gain_user;
    float    gtarget[BW_CHANNELS], gcur[BW_CHANNELS];
    float    gtarget_lo[BW_CHANNELS], gcur_lo[BW_CHANNELS];   /* dual-band low (amplitude-norm) band gains */
    float    xover_lp;                                        /* dual-band crossover one-pole LP state */
    float    dual_mix;                                        /* 0 = single .. 1 = dual; ramps on an A/B toggle
                                                              * so the LF re-weighting crossfades (no step) */
    int      path_on;                                         /* gated into the pathing (indirect) render */
    float    path_sh_cur[BW_AMBI_CH];                         /* ramped path shCoeffs (toward the published target) */
    /* pathing bending-loss EQ (audio-thread-only): a 3-band tilt on the indirect signal before the
     * SH-encode, structurally identical to the occlusion EQ above but on the un-occluded s_raw and
     * its own filter state. Bypassed while the tilt is flat (path_eq_engaged == 0). */
    float    path_eqg_cur[3];
    float    path_eq_co[3][5];
    float    path_eq_x1[3], path_eq_x2[3], path_eq_y1[3], path_eq_y2[3];
    int      path_eq_engaged;
    /* occlusion ramp state (audio-thread-only). The published target lives in the RtCore.occ_*
     * atomic arrays (outside this memset'd struct, so the off-thread sim never races a voice
     * create). occ_cur ramps toward the gated published value, applied to the mono signal pre-pan. */
    float    occ_cur;
    float    dir_cur;                        /* directivity ramp (source-radiation gain, pre-pan) */
    bool     refl_send;                      /* opted into the reflection aux send (CMD_SET_REFLECTIONS) */
    bool     refl_dist;                      /* scale the wet send by distance (far = wetter) */
    float    refl_gain;                      /* per-voice wet-send level (default 1) */
    float    refl_g_cur;                     /* ramped effective send gain (audio-thread-only) */
    /* per-band transmission EQ state (audio-thread-only). eqg_cur are the slewed band gains; eq_co
     * are the 3 sections' live coefficients {b0,b1,b2,a1,a2}, INTERPOLATED toward the block's target
     * per sample so the spectral envelope never steps at a block boundary (invariant 4). The 4
     * history arrays are the Direct-Form-I state; eq_engaged gates the chain (bypassed when settled flat). */
    float    eqg_cur[3];
    float    eq_co[3][5];
    float    eq_x1[3], eq_x2[3], eq_y1[3], eq_y2[3];
    int      eq_engaged;
    /* propagation effects (audio-thread-only, opt-in). air_a_cur is the slewed one-pole coeff, air_y1
     * the filter memory. dop_delay is the current fractional delay (samples), gliding toward distance/c
     * each block - the glide IS the pitch shift; dop_w indexes this voice's slice of RtCore.dop_ring;
     * dop_init snaps the delay to distance/c on the first block after enable (no enable glitch). */
    bool     air_on, dop_on, dop_init;
    float    air_a_cur, air_y1;
    float    dop_delay, dop_dtgt;            /* read delay + its smoothed target (2-pole, per-sample) */
    uint32_t dop_w;
    float    spread;                         /* source angular width 0..1 (0 = point); blends the pan gains */
    float    size_m;                         /* source METRIC radius (m; 0 = point): floors the spread at the
                                              * angle the radius subtends from the listener, so physical size
                                              * stays constant as the listener walks */
    float    spread_eff;                     /* last solve's EFFECTIVE spread (user, floored by size + near-
                                              * listener widening) — the decor split follows it (audio-thread) */
    float    dc_amp;                         /* decorrelated-split amplitude sqrt(spread*toggle), ramped (audio-thread) */
    /* equal-loudness distance compensation (opt-in): a one-pole LF shelf whose boost tracks the
     * distance attenuation, so an attenuated source keeps its body (ISO-226-motivated; audio-thread). */
    bool     ldc_on;
    float    ldc_g_cur;                      /* current shelf gain (linear; 1 = flat), ramped */
    float    ldc_lp;                         /* shelf one-pole state */
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
    /* bed yaw (CMD_BED_YAW): rotate a bed's soundfield about the room's vertical axis. yaw_cur glides
     * toward yaw at BW_BED_YAW_RATE; mix_bed rotates the ±m SH pairs with a per-sample phasor. */
    float    yaw, yaw_cur;
    /* image-source early reflections (CMD_SET_ISM). Per image: a gliding fractional read into this
     * voice's ism_ring slice, a one-pole HF damping state (walls absorb HF harder), and a ramped
     * 26-gain vector from the panner solved AT THE IMAGE POSITION (so reflections are directional
     * and walk-correct). All audio-thread-owned; ism_w is the shared ring write index. */
    bool     ism_on, ism_init, ism_tail;   /* enabled / snap the delays this block / ramping out */
    uint32_t ism_w;
    float    ism_delay[ISM_IMAGES];                  /* current fractional read delay (samples) */
    float    ism_lp[ISM_IMAGES];                     /* HF-damping one-pole state */
    float    ism_g[ISM_IMAGES][BW_CHANNELS];         /* ramped per-image speaker gains */
} Voice;

typedef struct {
    float p_pending[3], q_pending[4];
    float p_active[3],  q_active[4];
    /* extra (compromise) listener positions — multi-listener panning. Commit-gated like the pose. */
    float   ex_pending[BW_EXTRA_LIS][3], ex_active[BW_EXTRA_LIS][3];
    uint8_t nex_pending, nex_active;
} Listener;

/* Per-voice parametric-bed state (bw_set_bed_renderer), a PARALLEL array to `voices` (only bed
 * voices use it; reset by CMD_SRC_CREATE). Audio-thread-only. */
typedef struct {
    float lp[3][4];                                   /* band-splitter one-pole states x FOA channel */
    float I[BW_PARA_BANDS][3];                        /* smoothed intensity vector (ambi axes) */
    float E[BW_PARA_BANDS];                           /* smoothed energy */
    float g_cur[BW_PARA_BANDS][BW_CHANNELS];          /* direct-stream pan gains (ramped) */
    float g_tgt[BW_PARA_BANDS][BW_CHANNELS];
    float da_cur[BW_PARA_BANDS], fa_cur[BW_PARA_BANDS];   /* sqrt(1-psi)*pref / sqrt(psi), ramped */
    float mix;                                        /* matrix <-> parametric crossfade (live A/B) */
} ParaBed;

typedef struct {
    SoundData data;
    uint16_t  gen;
    uint8_t   inuse;
    uint8_t   retiring;                     /* unload requested; awaiting EVT_SOUND_RETIRED ack */
} SoundSlot;

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

    /* per-slot playback state for control-thread readback (rt_source_is_playing): packed
     * (gen<<1 | playing-bit), republished by the audio thread each block. The gen guards a stale
     * or recycled handle (a mismatched gen reads as not-playing). */
    _Atomic uint32_t* play_pub;

    /* debug channel test signal (bw_test_signal): audio-thread DSP state, set by CMD_TEST_SIGNAL,
     * generated + summed onto each channel AFTER align (raw channel). 0 kind = off. */
    uint8_t  test_kind[BW_CHANNELS];
    float    test_gain[BW_CHANNELS];
    float    test_phase[BW_CHANNELS];   /* sine phase accumulator, radians */
    uint32_t test_noise;                /* shared LCG state for the noise kind */
    struct { float cw0, alpha; int type; } eq_proto[3];   /* per-band biquad prototypes, rate-derived at create */

    /* per-channel output meter: each block's peak |sample| at the END of rt_render (post align/test
     * signal/limiter = exactly what the device channel received), relaxed-published for control-thread
     * readback (rt_bus_peaks -> bw_get_bus_levels: channel meters / speaker-activity displays). */
    _Atomic float chan_peak[BW_CHANNELS];

    /* control-thread-owned voice handle allocation */
    uint16_t* gen;                          /* current generation per voice slot */
    uint8_t*  inuse;                        /* 1 while a voice slot is allocated */
    uint8_t*  priority;                     /* per-source steal priority (control-side; 0=expendable..255=protected) */
    uint8_t*  stealing;                     /* 1 while a slot is fading out from a steal (skip it in the next scan) */
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
    _Atomic int panner;      /* 0 = DBAP (moving observer); 1 = SPCAP; 2 = VBAP (both fixed observer); atomic for A/B */
    _Atomic int dual_band;   /* 0 = single (power) panning; 1 = dual-band (amplitude LF / power HF); atomic for A/B */
    _Atomic int spread_mode; /* 0 = lobe reshape (spread_gains); 1 = MDAP virtual-source ring; atomic for A/B */
    _Atomic int decor_on;    /* decorrelate spread sources' wide part (velvet-noise path); atomic for A/B */

    /* decorrelation path: voices (the wide part of spread sources) and the parametric bed's diffuse
     * stream accumulate into dc_bus; rt_render then convolves each channel through its own sparse
     * velvet-noise filter into the main bus. Incoherent speaker feeds stop a wide source collapsing
     * to phantom images / comb-filtering as the tracked listener walks. All fixed-size, built at
     * create (audio thread only touches it). dc_tail keeps the convolution running one filter-length
     * past the last write, then the history is wiped so a re-engage never replays stale samples. */
    float*   dc_bus;                     /* BW_CHANNELS * BW_RT_MAX_BLOCK accumulation scratch */
    float*   dc_hist;                    /* BW_CHANNELS * dc_histlen input-history rings */
    uint32_t dc_histlen, dc_hmask, dc_w; /* shared ring geometry/write index (like the aligner's) */
    uint32_t dc_ntaps;
    uint16_t dc_off[BW_CHANNELS][BW_DECOR_TAPS];  /* per-channel tap offsets (samples back) */
    float    dc_tamp[BW_CHANNELS][BW_DECOR_TAPS]; /* per-channel tap amplitudes (signed, unit energy) */
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
    float    group_gain[BW_GROUPS];      /* mix-group gain multipliers (default 1; CMD_GROUP_GAIN) */
    uint8_t  group_paused[BW_GROUPS];    /* mix-group pause gates (CMD_GROUP_PAUSED) */
    _Atomic uint32_t active_pub;         /* last block's active voice count (rt_active_voices readback) */

    /* output protection limiter (final stage, after align + test signal — everything passes through).
     * LINKED across channels: one gain from the block's cross-channel peak, so engaging never shifts
     * the spatial image. ~1 ms attack / ~120 ms release one-poles + a hard safety clamp at the ceiling
     * (the attack is not lookahead, so brief overshoot clips — deterministic protection, not mastering). */
    _Atomic int   lim_on;         /* default 1 */
    _Atomic float lim_ceiling;    /* linear (default -1 dBFS) */
    float lim_gain;               /* audio-thread envelope: current applied gain (starts at 1) */
    float lim_att_a, lim_rel_a;   /* one-pole coefficients, rate-derived at create */
    float       xover_a;     /* one-pole LP coeff for the dual-band crossover (BW_DUALBAND_FC), rate-derived */
    uint64_t    dsp_block;   /* audio-thread: next block's dsp-sample (fallback clock when no device timestamp) */
    _Atomic uint64_t dsp_now;/* published block-start dsp-sample; control thread reads via rt_dsp_time for scheduling */
    uint32_t   layout_gen;   /* bumped on rt_set_layout; the SPCAP/VBAP caches compare it to self-invalidate */
    SpcapState spcap;        /* SPCAP cache (audio-thread-owned; rebuilt on listener/layout change) */
    VbapState  vbap;         /* VBAP cache (same) */
    SpcapState spcap_x[BW_EXTRA_LIS];   /* per-extra-listener caches (compromise panning): each cache is
                                         * keyed to ONE listener, so each extra gets its own */
    VbapState  vbap_x[BW_EXTRA_LIS];
    _Atomic float near_spread;  /* near-listener widening radius (m); 0 = off. An approaching source's
                                 * spread is floored by 1 - dist/radius (it subtends a growing angle
                                 * instead of collapsing into the nearest speaker). */
    float      ldc_a;        /* loudness-comp shelf one-pole coeff (~250 Hz), rate-derived at create */
    int        bed_decoder;  /* 0 = sampling decode (SAD); 1 = AllRAD (robust on irregular arrays) */
    /* ambisonic bed decode: [speaker][ACN] = (2l+1)*Y_k^SN3D(speaker_dir)/L (sampling decode, SN3D),
     * rebuilt from the layout whenever it changes. A bed voice decodes its SH channels through this. */
    float    bed_decode[BW_CHANNELS][BW_AMBI_CH];
    /* parametric bed renderer (bw_set_bed_renderer): per-band FOA direction+diffuseness analysis;
     * the non-diffuse stream re-pans through the LISTENER-RELATIVE panner (a walkable bed), the
     * diffuse stream decodes through bed_decode into the decorrelators. bed_radius anchors the
     * direct stream's virtual sources on the array shell around ref; bed_pref matches its loudness
     * to the matrix decode's plane-wave rendering (both derived in build_bed_decode). */
    _Atomic int bed_param;   /* 0 = matrix decode (default); 1 = parametric; atomic for A/B */
    ParaBed*  para;          /* voice_cap entries */
    float     para_xa[3];    /* band-splitter one-pole coeffs (BW_PARA_XOVER at the engine rate) */
    float     bed_radius;    /* mean speaker distance from ref (virtual-source shell) */
    float     bed_pref;      /* plane-wave loudness reference: sqrt(mean rendered power of the FOA matrix decode) */

    /* internal tracker (track_internal): the audio thread samples this each block, overriding
     * the committed listener. Set while the audio thread is stopped; NULL = no internal tracker. */
    const PoseSlot* tracker;
    /* pose prediction (rt_set_pose_prediction): extrapolate the tracked position by a fixed lead
     * along a velocity estimated from the tracker's OWN timestamps (pose.h t_ns — one clock, only
     * ever differenced), hiding the tracker->ears latency. Audio-thread state. */
    _Atomic float pred_lead;     /* seconds of lead; 0 = off (default) */
    float    pp_p[3], pp_vel[3]; /* last distinct raw pose + smoothed velocity (m/s) */
    uint64_t pp_tns;
    int      pp_valid;
    float    pp_quiet;           /* seconds since the last NEW tracker frame (stall detection) */

    /* readback of the active listener pose, published by the audio thread each block so the
     * control thread can sample it race-free (bw_get_listener_pose — visuals/logging). */
    PoseSlot readback;

    /* post-mix aux-send tap (the reflection bed): a phonon-free hook the audio thread calls after the
     * voice loop. `aux` is the summed mono send (opted-in voices). The tap pointer is published with
     * release AFTER its user-data (bus_tap_ud), and the audio thread acquire-loads it — so it is
     * registered SAFELY even though bw_start opens the sink (starting the callback) before it registers
     * the tap: a render seeing a non-NULL tap always sees a consistent ud. */
    _Atomic RtBusTap bus_tap;
    void*    bus_tap_ud;
    float*   aux;                /* BW_RT_MAX_BLOCK mono samples; the per-block aux send scratch */
    StreamSet* streams;          /* background file-streaming thread + ring pool (control thread owns lifecycle) */
    float*   stream_scratch;     /* BW_RT_MAX_BLOCK mono samples; a streaming voice's block, pulled before the mix */

    /* pathing: rt_render SH-encodes pathing voices into path_accum, then path_tap decodes it to the bus.
     * Per voice the sim publishes shCoeffs via a handle-gated double buffer (path_pub[idx*2 + path_idx]). */
    _Atomic RtPathTap path_tap;  /* published release-after-ud/ambi_ch; acquire-loaded (see bus_tap) */
    void*    path_tap_ud;
    uint32_t path_ambi_ch;       /* (order+1)^2 of the pathing field; 0 = no path tap */
    float*   path_accum;         /* BW_AMBI_CH * BW_RT_MAX_BLOCK; summed ambisonic indirect field */
    PathPub* path_pub;           /* voice_cap * 2 (double-buffered per voice) */
    _Atomic int* path_idx;       /* voice_cap: front-buffer index the sim flips after writing the back */

    /* image-source early reflections (ism.c): the room + a live gain, plus one delay ring per voice
     * (the reflected copies are the direct signal, delayed by their longer paths). The room is set
     * while stopped (bw_scene_set_box); the gain and the per-voice enables are live. */
    IsmRoom  ism_room;
    _Atomic float ism_gain;
    float*   ism_ring;           /* voice_cap contiguous power-of-two rings of ism_ringlen floats */
    uint32_t ism_ringlen;

    /* per-voice Doppler delay rings: voice_cap contiguous power-of-two rings of dop_ringlen floats each
     * (slice idx = dop_ring + idx*dop_ringlen), sized to BW_DOPPLER_MAX_DIST at the engine rate.
     * Allocated once at create (control thread); the audio thread writes/reads its voice's slice. */
    float*   dop_ring;
    uint32_t dop_ringlen;
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
    c->stealing[idx] = 0;                                     /* a fresh slot is not mid-steal (clears a leaked flag
                                                              * if the app destroyed a voice while it faded) */
    return BW_MK_H(idx, g);
}

static void recycle_handle(RtCore* c, uint32_t h) {
    uint16_t idx = BW_H_IDX(h);
    /* Gen-checked + idempotent: return the slot to the free-list only if THIS handle is still its
     * current occupant. A stale handle (slot already recycled, then re-allocated at a higher gen)
     * is dropped, so a double-destroy OR a late EVT_VOICE_ENDED for a since-stolen slot can never
     * free a live source's slot (invariant 5). The inuse check catches a double-free before reuse;
     * the gen check catches one after reuse. */
    if (idx < c->voice_cap && c->inuse[idx] && c->gen[idx] == BW_H_GEN(h)) {
        c->inuse[idx] = 0;
        c->freelist[c->free_count++] = idx;
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
    return BW_MK_H(idx, g);
}

static void srecycle_sound(RtCore* c, uint16_t idx) {
    if (idx < c->sound_cap && c->sounds[idx].inuse) {
        c->sounds[idx].inuse = 0;
        c->sounds[idx].retiring = 0;
        c->sfreelist[c->sfree_count++] = idx;
    }
}

/* control-thread resolve: the slot iff the handle is its current occupant */
static SoundSlot* sound_slot_ctrl(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BW_H_GEN(h)) ? s : NULL;
}

static void drain_events(RtCore* c) {                          /* control thread */
    EvtRing* r = &c->events;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Evt* ev = &r->slots[rd & (EVT_CAP - 1)];
        switch (ev->type) {
        case EVT_VOICE_ENDED:    recycle_handle(c, ev->handle); c->stealing[BW_H_IDX(ev->handle)] = 0; break;
        case EVT_SOUND_RETIRED: {        /* audio dropped all refs: free pcm / close the stream + recycle the slot */
            SoundSlot* s = sound_slot_ctrl(c, ev->handle);
            if (s) {
                if (s->data.stream) { stream_close(c->streams, s->data.stream); s->data.stream = NULL; }
                sound_unload(&s->data);
                srecycle_sound(c, BW_H_IDX(ev->handle));
            }
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* ---- audio thread: drain + mix ---- */

static Voice* voice_for(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->voice_cap) return NULL;
    Voice* v = &c->voices[i];
    return (v->active && v->gen == BW_H_GEN(h)) ? v : NULL;    /* stale gen => dropped */
}

/* audio-thread resolve: handle -> published SoundData (NULL if stale/retired) */
static const SoundData* sound_for(RtCore* c, uint32_t h) {
    uint16_t i = BW_H_IDX(h);
    if (i >= c->sound_cap) return NULL;
    SoundSlot* s = &c->sounds[i];
    return (s->inuse && s->gen == BW_H_GEN(h)) ? &s->data : NULL;
}

/* ---- per-band transmission EQ (matches Steam Audio's default direct-effect EQ) ----
 * Three RBJ biquads in series — low-shelf @800, peaking @~2530, high-shelf @8000 — applied to the
 * mono voice signal. The band *gains* are the spectral tilt of occluded sound; the broadband level
 * rides the existing occ_cur scalar. fc's are fixed, so the trig (cos w0 / alpha) is precomputed
 * per sample-rate at rt_create; only A=sqrt(g) varies per update => no transcendentals per sample. */
enum { EQ_LOWSHELF = BW_BIQUAD_LOWSHELF, EQ_PEAK = BW_BIQUAD_PEAK, EQ_HIGHSHELF = BW_BIQUAD_HIGHSHELF };
#define EQ_SLEW     0.5f       /* per-block band-gain glide toward the published target */
#define EQ_FLAT     0xFFFFFFFFFFFFull /* eq_pack({1,1,1}): the flat (passthrough) tilt, as a constant */
#define EQ_FLAT_EPS 0.001f     /* band gains within +/-0.1% (~0.009 dB) of unity count as flat -> EQ bypassed
                                * (a deliberately inaudible threshold that keeps un-occluded voices off the
                                * biquad path; the per-band attenuation floor lives in steam_scene.c). */

/* pack 3 band gains (each clamped to [0,1]) into one u64 = 3x16-bit, for a tear-free atomic publish */
static inline uint64_t eq_pack(const float g[3]) {
    uint64_t p = 0;
    for (int i = 0; i < 3; ++i) {
        float v = g[i] < 0.f ? 0.f : (g[i] > 1.f ? 1.f : g[i]);
        p |= (uint64_t)(uint16_t)(v * 65535.f + 0.5f) << (16 * i);
    }
    return p;
}
static inline void eq_unpack(uint64_t p, float g[3]) {
    for (int i = 0; i < 3; ++i) g[i] = (float)((p >> (16 * i)) & 0xFFFFu) * (1.f / 65535.f);
}

/* g is the linear band gain (A = sqrt(g)); cw0/alpha are precomputed per filter (see eq_proto). */
static void eq_coeffs(int type, float cw0, float alpha, float g, float out[5]) {
    bw_biquad_rbj(type, cw0, alpha, sqrt((double)g), out);
}

/* (re)start a voice's Doppler delay line clean: clear its ring slice + snap the delay next block, so a
 * fresh enable or a replay doesn't bleed the previous tail through the line. Audio thread (bounded). */
static void dop_line_reset(RtCore* c, Voice* v, uint16_t idx) {
    v->dop_w = 0; v->dop_delay = 0.f; v->dop_dtgt = 0.f; v->dop_init = true;
    memset(c->dop_ring + (size_t)idx * c->dop_ringlen, 0, (size_t)c->dop_ringlen * sizeof(float));
}

static void drain_commands(RtCore* c) {
    CmdRing* r = &c->cmds;
    uint32_t rd = atomic_load_explicit(&r->read,  memory_order_relaxed);
    uint32_t w  = atomic_load_explicit(&r->write, memory_order_acquire);
    for (; rd != w; ++rd) {
        const Cmd* cmd = &r->slots[rd & (RING_CAP - 1)];
        switch (cmd->type) {
        case CMD_SRC_CREATE: {
            uint16_t idx = BW_H_IDX(cmd->handle);
            Voice* v = &c->voices[idx];
            memset(v, 0, sizeof *v);
            v->gen = BW_H_GEN(cmd->handle);
            v->active = true; v->gain_user = 1.f; v->dirty = true;
            v->occ_cur = 1.f;                       /* clear (un-occluded) by default */
            v->dir_cur = 1.f;                       /* on-axis/omni by default */
            v->air_a_cur = 1.f;                     /* air low-pass passthrough by default */
            v->ldc_g_cur = 1.f;                     /* loudness-comp shelf flat by default */
            v->pitch = v->pitch_cur = 1.f;          /* native playback rate by default */
            v->refl_gain = 1.f;                     /* full wet-send level by default (gated by refl_send) */
            v->pause_g = 1.f;                       /* pause gate open (running) by default */
            v->eqg_cur[0] = v->eqg_cur[1] = v->eqg_cur[2] = 1.f;   /* flat EQ (history zeroed by memset) */
            for (int b = 0; b < 3; ++b) v->eq_co[b][0] = 1.f;     /* passthrough coeffs {1,0,0,0,0} */
            v->path_eqg_cur[0] = v->path_eqg_cur[1] = v->path_eqg_cur[2] = 1.f;  /* flat pathing EQ */
            for (int b = 0; b < 3; ++b) v->path_eq_co[b][0] = 1.f;
            memset(&c->para[idx], 0, sizeof c->para[idx]);   /* fresh parametric-bed state for the slot */
            /* drop any publish the sim left for the prior occupant of this slot (the stores are
             * atomic, so a concurrent sim publish for the old handle can't tear; either way the new
             * gen won't match it). */
            atomic_store_explicit(&c->occ_handle[idx], 0u,   memory_order_relaxed);
            atomic_store_explicit(&c->occ_val[idx],    1.f,  memory_order_relaxed);
            atomic_store_explicit(&c->occ_eq[idx], eq_pack((float[3]){1.f,1.f,1.f}), memory_order_relaxed);
            atomic_store_explicit(&c->occ_dir[idx],    1.f,  memory_order_relaxed);
        } break;
        case CMD_SRC_DESTROY: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->active = false; v->playing = false; v->sound = NULL; } } break;
        case CMD_SET_POS: { Voice* v = voice_for(c, cmd->handle);
            if (v) memcpy(v->pos_pending, &cmd->u.pos, sizeof v->pos_pending); } break;
        case CMD_SET_GAIN: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->gain_user = cmd->u.gain.g; v->dirty = true;
                     v->fade_rate = 0.f; v->fade_stop = 0; } } break;   /* an explicit set cancels a fade */
        case CMD_FADE: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                float tgt = cmd->u.fade.target < 0.f ? 0.f : cmd->u.fade.target;
                if (cmd->u.fade.seconds <= 0.f || tgt == v->gain_user) {   /* instant (or already there) */
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
            if (v) { v->group = cmd->u.group.id < BW_GROUPS ? cmd->u.group.id : 0; v->dirty = true; } } break;
        case CMD_GROUP_GAIN: {
            uint8_t id = cmd->u.ggain.id;
            if (id < BW_GROUPS) {
                c->group_gain[id] = cmd->u.ggain.gain < 0.f ? 0.f : cmd->u.ggain.gain;
                for (uint32_t i = 0; i < c->voice_cap; ++i)          /* the group's voices re-solve (ramped) */
                    if (c->voices[i].active && c->voices[i].group == id) c->voices[i].dirty = true;
            } } break;
        case CMD_GROUP_PAUSED: {
            uint8_t id = cmd->u.gpause.id;
            if (id < BW_GROUPS) c->group_paused[id] = cmd->u.gpause.on;   /* pause_gate ramps/freezes */
            } break;
        case CMD_SET_PITCH: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float r2 = cmd->u.pitch.rate;
                     v->pitch = r2 < 0.25f ? 0.25f : (r2 > 4.f ? 4.f : r2); } } break;   /* mixer glides */
        case CMD_BED_YAW: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->yaw = cmd->u.byaw.yaw; } break;                     /* mix_bed glides toward it */
        case CMD_SET_ISM: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                const bool on = cmd->u.ism.on != 0;
                if (on && !v->ism_on && c->ism_ring) {   /* fresh enable: a clean ring (a recycled slot must
                                                          * never replay the previous occupant), zeroed filter
                                                          * state, gains from 0 (the reflections fade in), and
                                                          * delays snapped on the first render (ism_init) */
                    memset(c->ism_ring + (size_t)BW_H_IDX(cmd->handle) * c->ism_ringlen, 0,
                           sizeof(float) * c->ism_ringlen);
                    memset(v->ism_g, 0, sizeof v->ism_g);
                    memset(v->ism_lp, 0, sizeof v->ism_lp);
                    v->ism_w = 0; v->ism_init = true;
                }
                if (!on && v->ism_on) v->ism_tail = 1;   /* ramp the reflections out over one block */
                v->ism_on = on;
            } } break;
        case CMD_PLAY: { Voice* v = voice_for(c, cmd->handle);
            const SoundData* s = sound_for(c, cmd->u.play.sound);
            if (v && s) {
                if (s->stream)                       /* one voice per stream: the ring is SPSC. Detach any OTHER */
                    for (uint32_t j = 0; j < c->voice_cap; ++j)   /* voice on this stream so two consumers can't corrupt it */
                        if (&c->voices[j] != v && c->voices[j].sound == s) { c->voices[j].playing = false; c->voices[j].sound = NULL; }
                v->sound = s; v->cursor = 0; v->cur_frac = 0.f; v->stream_pos = 0; v->loop = cmd->u.play.loop != 0;
                          v->oneshot = cmd->u.play.oneshot != 0; v->playing = true; v->dirty = true;
                          v->start_sample = cmd->u.play.start;  /* 0 = now; else hold output until this dsp-sample */
                          v->refl_g_cur = 0.f;                  /* fresh start: ramp the wet send up from 0, no stale burst */
                          v->xover_lp = 0.f;                    /* fresh dual-band crossover state */
                          v->dual_mix = atomic_load_explicit(&c->dual_band, memory_order_relaxed) ? 1.f : 0.f;  /* start in the current mode */
                          v->paused = false; v->pause_g = 1.f; v->seek_pending = 0; v->stopping = 0;   /* play always starts running */
                          if (v->dop_on) dop_line_reset(c, v, BW_H_IDX(cmd->handle)); } } break;
        case CMD_STOP: { Voice* v = voice_for(c, cmd->handle);
            /* Fade the gate to 0 over one block, then finalize (playing=false) in pause_gate — a
             * click-free explicit stop. Don't downgrade a steal-in-progress (2), which must still free
             * its slot. (CMD_SRC_DESTROY hard-cuts; automatic steal fades via CMD_SRC_STEAL below.) */
            if (v && v->playing && v->stopping != 2) v->stopping = 1; } break;
        case CMD_SRC_STEAL: { Voice* v = voice_for(c, cmd->handle);
            /* Fade the stolen voice out on its own slot, then free it (pause_gate finalize pushes
             * EVT_VOICE_ENDED so the control thread recycles the slot). The new source already started
             * on a reserve slot, so the steal is click-free. */
            if (v && v->playing) v->stopping = 2; } break;
        case CMD_SET_PAUSED: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->paused = cmd->u.pause.on != 0; } break;    /* the mixer's gate does the ramp/freeze */
        case CMD_SEEK: { Voice* v = voice_for(c, cmd->handle);
            if (v && v->sound && !v->sound->stream) {            /* streams: the ring can't jump — ignored */
                v->seek_pos = cmd->u.seek.frame;
                v->seek_pending = 1;                             /* lands once the gate is silent (click-free) */
            } } break;
        case CMD_SET_REFLECTIONS: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_send = cmd->u.refl.on != 0; } break;
        case CMD_SET_PATHING: { Voice* v = voice_for(c, cmd->handle);
            if (v) { v->path_on = cmd->u.path.on != 0;
                     if (!v->path_on) {                          /* clean restart: zero the ramp + flatten the EQ */
                         for (int k = 0; k < BW_AMBI_CH; ++k) v->path_sh_cur[k] = 0.f;
                         v->path_eq_engaged = 0;
                         for (int b = 0; b < 3; ++b) { v->path_eqg_cur[b] = 1.f;
                             v->path_eq_x1[b]=v->path_eq_x2[b]=v->path_eq_y1[b]=v->path_eq_y2[b]=0.f; } } } } break;
        case CMD_SET_REFL_SEND: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float g = cmd->u.rsend.gain; v->refl_gain = g < 0.f ? 0.f : g; } } break;
        case CMD_SET_REFL_DIST: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->refl_dist = cmd->u.rdist.on != 0; } break;
        case CMD_SET_DOPPLER: { Voice* v = voice_for(c, cmd->handle);
            if (v) {
                if (cmd->u.dop.on && !v->dop_on) dop_line_reset(c, v, BW_H_IDX(cmd->handle));  /* fresh enable */
                v->dop_on = cmd->u.dop.on != 0;
            } } break;
        case CMD_SET_AIR: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->air_on = cmd->u.air.on != 0; } break;
        case CMD_SET_LDC: { Voice* v = voice_for(c, cmd->handle);
            if (v) v->ldc_on = cmd->u.ldc.on != 0; } break;      /* the mixer ramps the shelf in/out */
        case CMD_SET_EXTRA_LIS:
            c->lis.nex_pending = cmd->u.exlis.n > BW_EXTRA_LIS ? BW_EXTRA_LIS : cmd->u.exlis.n;
            memcpy(c->lis.ex_pending, cmd->u.exlis.p, sizeof c->lis.ex_pending);
            break;
        case CMD_SET_SPREAD: { Voice* v = voice_for(c, cmd->handle);
            if (v) { float a = cmd->u.spread.amount; v->spread = a < 0.f ? 0.f : (a > 1.f ? 1.f : a); v->dirty = true; } } break;
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
                for (uint32_t i = 0; i < c->voice_cap; ++i)
                    if (c->voices[i].sound == s) { c->voices[i].playing = false; c->voices[i].sound = NULL; }
            }
            Evt ev = { .type = EVT_SOUND_RETIRED, .handle = cmd->handle };
            evt_push(&c->events, &ev);
        } break;
        }
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}

/* Build the ambisonic bed decode matrix from the layout: for each speaker, sample the SH basis at
 * its direction (room -> ambisonic axes) and scale by (2l+1)/L. Room convention (post +z-forward
 * flip): identity listener faces +z, right ear at -x, +y up; AmbiX axes are x=front/y=left/z=up, so
 * ambi front = room +z (where the listener faces / the main content sits), ambi left = room +x,
 * ambi up = room +y. World-locked: directions are from the array centroid, not the moving listener.
 * This is the projection/sampling decode, which assumes a roughly UNIFORM speaker distribution; the
 * cave grid is only approximately uniform, so it is good for a diffuse bed but a pseudo-inverse
 * (mode-matching) decode would be exact — a refinement, not needed for v1's diffuse content. */
static void build_bed_decode_sad(RtCore* c) {
    const float invL = 1.0f / (float)c->channels;
    for (uint32_t s = 0; s < c->channels; ++s) {
        /* speaker direction from the layout's nominal listening point (the array centroid) —
         * NOT from the origin, which canonically sits on the floor (Motive ground plane) */
        float p[3] = { c->layout.speakers[s].pos[0] - c->layout.ref[0],
                       c->layout.speakers[s].pos[1] - c->layout.ref[1],
                       c->layout.speakers[s].pos[2] - c->layout.ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        float ad[3];
        if (len < 1e-6f) { ad[0] = 1.f; ad[1] = 0.f; ad[2] = 0.f; }          /* degenerate: face front */
        else { ad[0] = p[2]/len; ad[1] = p[0]/len; ad[2] = p[1]/len; }       /* (z,x,y): ambi front=+z, left=+x, up=+y */
        float y[BW_AMBI_CH];
        ambi_encode_sn3d(ad, y);
        for (int k = 0; k < BW_AMBI_CH; ++k) {
            int l = (int)floorf(sqrtf((float)k));                            /* ACN order of channel k */
            c->bed_decode[s][k] = (float)(2*l + 1) * y[k] * invL;
        }
    }
}

/* Dispatch the bed decode: AllRAD if selected (and the array triangulates), else the sampling decode.
 * Also derives the parametric bed's constants from the result: bed_radius (the virtual-source shell
 * for the re-panned direct stream) and bed_pref (so a plane wave renders at the same loudness through
 * either bed renderer: the mean power the FOA matrix decode produces, averaged over the speaker
 * directions as direction samples). */
static void build_bed_decode(RtCore* c) {
    if (!(c->bed_decoder == 1 && allrad_build_decode(&c->layout, c->bed_decode)))
        build_bed_decode_sad(c);
    double rsum = 0.0, psum = 0.0;
    for (uint32_t s = 0; s < c->channels; ++s) {
        float p[3] = { c->layout.speakers[s].pos[0] - c->layout.ref[0],
                       c->layout.speakers[s].pos[1] - c->layout.ref[1],
                       c->layout.speakers[s].pos[2] - c->layout.ref[2] };
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        rsum += len;
        float ad[3] = { len > 1e-6f ? p[2]/len : 1.f, len > 1e-6f ? p[0]/len : 0.f, len > 1e-6f ? p[1]/len : 0.f };
        float y[BW_AMBI_CH]; ambi_encode_sn3d(ad, y);
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

/* Source spread/size: blend the panner's point gains toward a width-controlled lobe centred on the
 * source direction (from the listener), then renormalise constant-power. spread 0 = the point gains;
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
    float lobe[BW_CHANNELS]; double ln = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) {
        const float* sp = c->layout.speakers[k].pos;
        float sd[3] = { sp[0]-c->lis.p_active[0], sp[1]-c->lis.p_active[1], sp[2]-c->lis.p_active[2] };
        float sl = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        float dot = sl > 1e-6f ? (sd[0]*d[0] + sd[1]*d[1] + sd[2]*d[2]) / sl : 0.f;
        float w = 0.5f * (1.f + dot); if (w < 0.f) w = 0.f;   /* [0,1], 1 toward the source */
        lobe[k] = powf(w, q);
        ln += (double)lobe[k] * lobe[k];
    }
    if (ln < 1e-12) return;
    float lnorm = (float)(P / sqrt(ln));                 /* scale the lobe to the same power P */
    double bn = 0.0;
    for (uint32_t k = 0; k < c->channels; ++k) { g[k] = (1.f - s) * g[k] + s * lobe[k] * lnorm; bn += (double)g[k]*g[k]; }
    if (bn < 1e-12) return;
    float bnorm = P / (float)sqrt(bn);                   /* the blend isn't norm-P -> renormalise back to P */
    for (uint32_t k = 0; k < c->channels; ++k) g[k] *= bnorm;
}

/* The selected panner's point solve at an arbitrary source position for an arbitrary listener.
 * `p` is the panner id, loaded once per gain solve so one solve never mixes panners mid-ring. The
 * SPCAP/VBAP caches are keyed to ONE listener each — the caller supplies the pair (the primary's,
 * or an extra listener's own for compromise panning). */
static void panner_gains_at(RtCore* c, int p, const float lis[3], SpcapState* ss, VbapState* vs,
                            const float src[3], float user_gain, float* out) {
    if (p == 1)
        spcap_gains(ss, src, lis, &c->layout, c->layout_gen, user_gain, out);
    else if (p == 2)
        vbap_gains(vs, src, lis, &c->layout, c->layout_gen, user_gain, out);
    else
        dbap_gains(src, lis, &c->layout, user_gain, out);
}

/* The primary listener's solve (the voice solve below, MDAP's virtual sources, the parametric bed). */
static void panner_gains(RtCore* c, int p, const float src[3], float user_gain, float* out) {
    panner_gains_at(c, p, c->lis.p_active, &c->spcap, &c->vbap, src, user_gain, out);
}

/* Source spread/size, MDAP mode (Pulkki 1999: multiple-direction amplitude panning): pan a ring of
 * VIRTUAL SOURCES around the source direction with the selected panner and sum, instead of reshaping
 * the point gains (spread_gains above). The extent is made of real panner solves, so it inherits the
 * panner's own character (VBAP stays sparse per direction, SPCAP stays placement-corrected). Cone
 * half-angle = spread * 90°; the virtual sources sit at the source's own distance so the distance
 * attenuation is untouched; the sum is renormalised to the point solve's power P (widening never
 * re-levels). At spread->0 the ring collapses onto the source direction and the result IS the point
 * solve, so the two spread modes meet continuously. 12 extra panner solves, per-block + dirty-gated. */
static void mdap_gains(RtCore* c, int p, const Voice* v, float spread, float user_gain, float* g) {
    float d[3] = { v->pos_active[0]-c->lis.p_active[0], v->pos_active[1]-c->lis.p_active[1], v->pos_active[2]-c->lis.p_active[2] };
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dl < 1e-6f) return;                              /* source on the listener: no direction to spread around */
    d[0]/=dl; d[1]/=dl; d[2]/=dl;
    double p0 = 0.0; for (uint32_t k = 0; k < c->channels; ++k) p0 += (double)g[k]*g[k];
    float P = (float)sqrt(p0);                           /* preserve the panner's own power (never re-level) */
    if (P < 1e-9f) return;
    float up[3] = { 0.f, 1.f, 0.f };                     /* orthonormal frame (u, w) around d */
    if (d[1] > 0.9f || d[1] < -0.9f) { up[0] = 1.f; up[1] = 0.f; }
    float u[3] = { up[1]*d[2]-up[2]*d[1], up[2]*d[0]-up[0]*d[2], up[0]*d[1]-up[1]*d[0] };
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0]/=ul; u[1]/=ul; u[2]/=ul;
    float w[3] = { d[1]*u[2]-d[2]*u[1], d[2]*u[0]-d[0]*u[2], d[0]*u[1]-d[1]*u[0] };
    float s = spread; if (s > 1.f) s = 1.f;
    float acc[BW_CHANNELS], gd[BW_CHANNELS];
    for (uint32_t k = 0; k < c->channels; ++k) acc[k] = g[k];   /* the point solve is the ring centre */
    /* two rings — 8 at the full cone angle, 4 offset at half — approximate a uniform spherical cap */
    static const int   ring_n[2]   = { 8, 4 };
    static const float ring_a[2]   = { 1.f, 0.5f };
    static const float ring_off[2] = { 0.f, 0.7853982f };      /* half-ring offset: pi/4 */
    for (int r = 0; r < 2; ++r) {
        float a = s * 1.5707963f * ring_a[r];                  /* cone half-angle: spread 1 = 90° */
        float ca = cosf(a), sa = sinf(a);
        for (int i = 0; i < ring_n[r]; ++i) {
            float phi = ring_off[r] + 6.2831853f * (float)i / (float)ring_n[r];
            float cp = cosf(phi), sp = sinf(phi);
            float pos[3];
            for (int k = 0; k < 3; ++k)
                pos[k] = c->lis.p_active[k] + dl * (ca * d[k] + sa * (cp * u[k] + sp * w[k]));
            panner_gains(c, p, pos, user_gain, gd);
            for (uint32_t k = 0; k < c->channels; ++k) acc[k] += gd[k];
        }
    }
    double an = 0.0; for (uint32_t k = 0; k < c->channels; ++k) an += (double)acc[k]*acc[k];
    if (an < 1e-12) return;
    float norm = (float)(P / sqrt(an));                        /* back to the point solve's power */
    for (uint32_t k = 0; k < c->channels; ++k) g[k] = acc[k] * norm;
}

/* DBAP gain solve (M4): listener-relative, dirty-gated. CMD_COMMIT re-dirties a voice on a
 * position change and dirties all voices on a listener move (gains are listener-relative). A bed
 * voice (multi-channel asset) has no DBAP position — its master gain rides gtarget[0]. */
static void compute_gains(RtCore* c, Voice* v) {
    const float ug = v->gain_user * c->group_gain[v->group];   /* mix-group gain folds into the solve */
    if (v->sound && v->sound->channels > 1) { v->gtarget[0] = ug; return; }
    int p = atomic_load_explicit(&c->panner, memory_order_acquire);
    panner_gains(c, p, v->pos_active, ug, v->gtarget);

    /* multi-listener compromise (rt_set_extra_listeners): solve the same point for each extra
     * listener (each with its own SPCAP/VBAP cache) and take the per-speaker ENERGY MEAN — the L2
     * barycentre of the individual renderings. Constant power is preserved (the mean of the solves'
     * powers), and each occupant hears the image biased toward their own solve rather than one
     * person's being exact and the others' wrong. Spread/dual-band derive from the result. */
    const uint8_t nex = c->lis.nex_active;
    if (nex) {
        double acc[BW_CHANNELS];
        for (uint32_t k = 0; k < c->channels; ++k) acc[k] = (double)v->gtarget[k] * v->gtarget[k];
        float gx[BW_CHANNELS];
        for (uint8_t j = 0; j < nex; ++j) {
            panner_gains_at(c, p, c->lis.ex_active[j], &c->spcap_x[j], &c->vbap_x[j],
                            v->pos_active, ug, gx);
            for (uint32_t k = 0; k < c->channels; ++k) acc[k] += (double)gx[k] * gx[k];
        }
        const double inv = 1.0 / (double)(nex + 1);
        for (uint32_t k = 0; k < c->channels; ++k) v->gtarget[k] = (float)sqrt(acc[k] * inv);
    }

    /* effective spread: the user's angular width, floored by the source's METRIC size (the angle its
     * radius subtends from the listener — physical size stays constant as the listener walks, and a
     * source that engulfs the listener goes fully wide) and by the engine-wide NEAR-LISTENER widening
     * policy (rt_set_near_spread; a per-source size subsumes it for sized sources). The decor split
     * follows spread_eff, so the widened part decorrelates too when enabled. */
    float s_eff = v->spread; if (s_eff > 1.f) s_eff = 1.f; else if (s_eff < 0.f) s_eff = 0.f;
    const float nearR = atomic_load_explicit(&c->near_spread, memory_order_relaxed);
    if (v->size_m > 0.f || nearR > 0.f) {
        float dx = v->pos_active[0]-c->lis.p_active[0], dy = v->pos_active[1]-c->lis.p_active[1],
              dz = v->pos_active[2]-c->lis.p_active[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (v->size_m > 0.f) {
            float ratio  = dist > 1e-6f ? v->size_m / dist : 2.f;
            float s_size = ratio >= 1.f ? 1.f : asinf(ratio) * 0.636619772f;   /* subtended half-angle / (pi/2) */
            if (s_size > s_eff) s_eff = s_size;
        }
        if (nearR > 0.f) {
            float s_near = 1.f - dist / nearR;
            if (s_near > s_eff) s_eff = s_near > 1.f ? 1.f : s_near;
        }
    }
    v->spread_eff = s_eff;
    if (s_eff > 1e-3f) {                                     /* widen the image if this source has size */
        if (atomic_load_explicit(&c->spread_mode, memory_order_acquire) == 1)
            mdap_gains(c, p, v, s_eff, ug, v->gtarget);
        else
            spread_gains(c, v, s_eff, v->gtarget);
    }

    /* dual-band low band: the SAME gain directions, renormalised to amplitude (pressure) sum instead of
     * power. The target sum is the power gains' OWN magnitude ||g||_2 (= gain_user * distance_atten, set
     * by the panner) — NOT bare gain_user, which would cancel the distance attenuation and leave a
     * distant source's bass at full level. So the LF coherent pressure sum matches the HF energy level
     * at every distance. Always computed (cheap) so dual_band A/Bs live; the mixer reads it only when on. */
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
            if (how == 2) {                          /* steal: free the slot (control thread recycles on the ack) */
                v->active = false;
                Evt ev = { .type = EVT_VOICE_ENDED, .handle = BW_MK_H(idx, v->gen) };
                evt_push(&c->events, &ev);
            }
            return 0;
        }
        if (v->seek_pending) {                       /* land the seek while inaudible */
            const SoundData* snd = v->sound;
            uint64_t f = v->seek_pos;
            if (snd->frames == 0) f = 0;
            else if (f >= snd->frames) f = v->loop ? (f % snd->frames) : snd->frames;  /* wrap, or end naturally */
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

static void mix_voice(RtCore* c, Voice* v, uint16_t idx, float* bus, uint32_t n, uint32_t start, float* aux) {
    const SoundData* snd = v->sound;
    const uint32_t nr = n - start;      /* rendered samples this block: every per-sample ramp spans the AUDIBLE
                                         * part [start,n), so a scheduled start (start>0) lands its gains exactly
                                         * instead of snapping the unspent start/n fraction at the block end */
    float pg, pg_step;
    if (!pause_gate(c, v, idx, nr, &pg, &pg_step)) return;
    float step[BW_CHANNELS];
    for (uint32_t ch = 0; ch < c->channels; ++ch)
        step[ch] = (v->gtarget[ch] - v->gcur[ch]) / (float)nr;
    /* dual-band panning: split each sample at BW_DUALBAND_FC; the low band uses amplitude-normalised
     * gains (gcur_lo, better LF velocity vector), the high band the power gains. The complementary
     * 1st-order crossover (hi = s - lo) sums flat. dual = gcur*s + (gcur_lo-gcur)*lo, so a `dual_mix`
     * factor ramped 0<->1 on an A/B toggle CROSSFADES the LF re-weighting instead of stepping it
     * (invariant 4). The full path runs only while dual is on OR mid-crossfade; settled-single stays
     * cheap and keeps xover_lp at 0 for a clean re-enable. */
    const int dual = atomic_load_explicit(&c->dual_band, memory_order_acquire);
    const float target_mix = dual ? 1.f : 0.f;
    const int use_dual = (v->dual_mix > 0.f) || dual;
    float dmix = v->dual_mix;
    const float dmix_step = (target_mix - v->dual_mix) / (float)nr;
    const float xover_a = c->xover_a;
    float step_lo[BW_CHANNELS];
    if (use_dual) for (uint32_t ch = 0; ch < c->channels; ++ch)
        step_lo[ch] = (v->gtarget_lo[ch] - v->gcur_lo[ch]) / (float)nr;
    /* gate the sim's publish on our own generation (we own v->gen, so this is race-free): apply the
     * published transmittance only if it was published for THIS occupant, else treat as clear. Read
     * once into a local so the ramp aims at and lands on the same value (invariant 4 — no jump). */
    const uint32_t myh = BW_MK_H(idx, v->gen);
    const bool mine = atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == myh;
    const float occ_tgt = mine ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.0f;
    const float occ_step = (occ_tgt - v->occ_cur) / (float)nr;   /* occlusion ramp (invariant 4) */
    /* directivity (source-radiation gain): own ramp — it tracks source/listener motion, so a raw
     * per-block jump would zipper (invariant 4). Gated on the same handle. */
    const float dir_tgt = mine ? atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed) : 1.0f;
    const float dir_step = (dir_tgt - v->dir_cur) / (float)nr;

    /* per-band EQ: read the gated tilt once + glide the band gains; compute this block's TARGET
     * biquad coeffs and interpolate the live coeffs (eq_co) toward them per sample, so the spectral
     * envelope never steps at a block boundary (invariant 4). Target is passthrough when flat; the
     * chain is bypassed once it has fully settled flat. */
    float gt[3];
    eq_unpack(mine ? atomic_load_explicit(&c->occ_eq[idx], memory_order_relaxed) : EQ_FLAT, gt);
    bool flat = true;
    for (int b = 0; b < 3; ++b) {
        v->eqg_cur[b] += (gt[b] - v->eqg_cur[b]) * EQ_SLEW;
        if (v->eqg_cur[b] < 1.f - EQ_FLAT_EPS || v->eqg_cur[b] > 1.f + EQ_FLAT_EPS) flat = false;
    }
    if (!flat) v->eq_engaged = 1;
    float co_tgt[3][5], co_step[3][5];
    if (v->eq_engaged) {
        for (int b = 0; b < 3; ++b) {
            if (flat) { co_tgt[b][0] = 1.f; co_tgt[b][1] = co_tgt[b][2] = co_tgt[b][3] = co_tgt[b][4] = 0.f; }
            else eq_coeffs(c->eq_proto[b].type, c->eq_proto[b].cw0, c->eq_proto[b].alpha, v->eqg_cur[b], co_tgt[b]);
            for (int k = 0; k < 5; ++k) co_step[b][k] = (co_tgt[b][k] - v->eq_co[b][k]) / (float)nr;
        }
    }

    /* propagation (opt-in): a distance-driven air-absorption low-pass + a glided Doppler delay line.
     * Both ride the block: the air coeff ramps (invariant 4); the Doppler delay glides toward
     * distance/c and the glide rate IS the pitch shift. Indices stay integer (the ring is masked,
     * the delay's frac is a separate small float) so a long-lived voice never loses sample precision. */
    float dist = 0.f;
    if (v->air_on || v->dop_on || v->ldc_on || (v->refl_send && v->refl_dist)) {
        float dx = v->pos_active[0]-c->lis.p_active[0], dy = v->pos_active[1]-c->lis.p_active[1], dz = v->pos_active[2]-c->lis.p_active[2];
        dist = sqrtf(dx*dx + dy*dy + dz*dz);
    }
    /* reverb wet-send level: refl_gain, optionally scaled by distance (near = drier, far = wetter); ramped
     * (so motion + on/off don't zipper the send). do_send keeps ramping a just-disabled voice down to 0. */
    float refl_tgt = 0.f;
    if (aux && v->refl_send) {
        refl_tgt = v->refl_gain;
        if (v->refl_dist) {
            float t = (dist - BW_REFL_NEAR_DIST) / (BW_REFL_FAR_DIST - BW_REFL_NEAR_DIST);
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            refl_tgt *= BW_REFL_NEAR_SEND + (1.f - BW_REFL_NEAR_SEND) * t;
        }
    }
    const float refl_step = (refl_tgt - v->refl_g_cur) / (float)nr;
    const bool do_send = aux && (v->refl_send || v->refl_g_cur > 1e-6f);
    float air_a_tgt = 1.f, air_a_step = 0.f;
    if (v->air_on) {
        float fc = BW_AIR_FC_NEAR - dist * BW_AIR_FC_PER_M;
        if (fc < BW_AIR_FC_FLOOR) fc = BW_AIR_FC_FLOOR;
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
        float dref = dist > L->atten_ref_m ? dist : L->atten_ref_m;
        float att = powf(L->atten_ref_m / dref, L->atten_rolloff);
        if (att < L->atten_min_lin) att = L->atten_min_lin;
        float sh_db = -20.f * log10f(att) * 0.4f;
        if (sh_db > 8.f) sh_db = 8.f; else if (sh_db < 0.f) sh_db = 0.f;
        ldc_tgt = powf(10.f, sh_db / 20.f);
    }
    const int use_ldc = v->ldc_on || v->ldc_g_cur > 1.f + 1e-4f;   /* keep ramping a just-disabled voice flat */
    if (use_ldc) ldc_step = (ldc_tgt - v->ldc_g_cur) / (float)nr;
    float dop_ds = 0.f, dop_k = 0.f, *dring = NULL; uint32_t dmask = 0;
    if (v->dop_on && c->dop_ring) {
        dop_ds = dist / BW_SPEED_OF_SOUND * (float)c->sample_rate;     /* raw propagation delay (samples) */
        float maxd = (float)(c->dop_ringlen - 2);          /* keep both interpolation taps in-ring */
        if (dop_ds > maxd) dop_ds = maxd;
        if (v->dop_init) { v->dop_delay = dop_ds; v->dop_dtgt = dop_ds; v->dop_init = false; }  /* snap: no enable glitch */
        /* The read delay is low-passed toward distance/c PER SAMPLE with a 2-pole filter (target then
         * delay, BW_DOPPLER_TAU each). Position is committed per video frame (~60 Hz) but we render many
         * samples per frame, so the raw target is a staircase; its fundamental (the commit rate) would
         * FM-modulate the carrier into audible sidebands (worse at HF). The cutoff (~5 Hz, BW_DOPPLER_TAU
         * = 32 ms) was set with test_doppler_fft: it keeps the commit-rate sidebands below ~ -45 dB at
         * 1 kHz / -38 dB at 4 kHz. Low-passing the delay preserves the ramp's SLOPE, so steady-motion
         * pitch is exact; it only offsets the delay value by ~v*group_delay (sub-millisecond), and
         * rounds pitch transitions over the group delay (natural). A plain velocity tracker is worse
         * here (it overshoots each staircase step). */
        dop_k = 1.f / (BW_DOPPLER_TAU * (float)c->sample_rate);
        if (dop_k > 0.5f) dop_k = 0.5f;
        dring = c->dop_ring + (size_t)idx * c->dop_ringlen; dmask = c->dop_ringlen - 1;
    }

    /* decorrelated wide-part routing (bw_set_decorrelation): a spread source's energy splits into a
     * coherent copy on the main bus and an incoherent copy on the decor bus (per-channel velvet
     * filters, convolved in rt_render) — the same wide gain DISTRIBUTION, but the speaker feeds
     * decorrelate, so the extent stops collapsing to phantom images / comb-filtering as the tracked
     * listener walks. dc_amp = sqrt(spread·toggle) ramps per sample (invariant 4); power is conserved
     * because incoherent energy ADDS: coherent² + decor² = 1. A just-toggled-off voice keeps writing
     * while dc_amp ramps out. */
    float dc_a = v->dc_amp, dc_step = 0.f;
    int use_dc = 0;
    {
        const int dc_on = c->dc_on_blk;                   /* the block's single load (rt_render) */
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
     * direction (a walked reflection changes direction, as it must). Delays glide (BW_ISM_TAU) and
     * gains ramp per sample: motion bends the reflections, never steps them. */
    const int ism_want = v->ism_on && c->ism_room.valid && c->ism_ring;
    const int ism_on   = ism_want || v->ism_tail;      /* a just-disabled voice ramps its reflections out */
    int   ism_n = 0;
    float ism_gtgt[ISM_IMAGES][BW_CHANNELS], ism_gstep[ISM_IMAGES][BW_CHANNELS];
    float ism_dtgt[ISM_IMAGES], ism_a[ISM_IMAGES];
    float ism_k = 0.f;
    float *iring = NULL; uint32_t imask = 0;
    if (ism_on) {
        IsmImage img[ISM_IMAGES];
        const int nimg = ism_want ? ism_images(&c->ism_room, v->pos_active, img) : 0;   /* 0 = outside the room */
        const float scale = atomic_load_explicit(&c->ism_gain, memory_order_relaxed);
        ism_n = ISM_IMAGES;                                      /* every slot ramps: a dropped image fades out */
        ism_k = 1.f / (BW_ISM_TAU * (float)c->sample_rate);      /* per-sample delay glide */
        if (ism_k > 0.5f) ism_k = 0.5f;
        const int p = atomic_load_explicit(&c->panner, memory_order_acquire);
        const float maxd = (float)(c->ism_ringlen - 2);          /* keep both interpolation taps in-ring */
        for (int m = 0; m < ISM_IMAGES; ++m) {
            if (m >= nimg) {                                     /* disabled / outside the room: fade this slot */
                ism_dtgt[m] = v->ism_delay[m]; ism_a[m] = 1.f;
                for (uint32_t k = 0; k < c->channels; ++k) {
                    ism_gtgt[m][k]  = 0.f;
                    ism_gstep[m][k] = -v->ism_g[m][k] / (float)nr;
                }
                continue;
            }
            float dx = img[m].pos[0]-c->lis.p_active[0], dy = img[m].pos[1]-c->lis.p_active[1],
                  dz = img[m].pos[2]-c->lis.p_active[2];
            float path = sqrtf(dx*dx + dy*dy + dz*dz);       /* the reflection's path length to the ears */
            float dtg = path / BW_SPEED_OF_SOUND * (float)c->sample_rate;
            ism_dtgt[m] = dtg > maxd ? maxd : dtg;
            if (v->ism_init) v->ism_delay[m] = ism_dtgt[m];      /* fresh enable: snap (a glide from 0 would sweep) */
            /* the panner gives direction + its own distance attenuation; the mid-band coefficient is
             * the broadband level, and the high-vs-mid ratio becomes a one-pole HF damping (a wall
             * absorbs treble harder — why a reflection sounds duller than the direct sound) */
            panner_gains(c, p, img[m].pos, v->gain_user * c->group_gain[v->group] * img[m].refl[1] * scale,
                         ism_gtgt[m]);
            float hf = img[m].refl[1] > 1e-6f ? img[m].refl[2] / img[m].refl[1] : 1.f;
            if (hf > 1.f) hf = 1.f; else if (hf < 0.02f) hf = 0.02f;
            ism_a[m] = hf;                                       /* 1 = no damping .. 0 = fully dull */
            for (uint32_t k = 0; k < c->channels; ++k)
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
    }

    /* pathing: SH-encode the UN-occluded source (the indirect path goes around the occluder, so it is
     * not occluded by the direct-path occlusion) into the shared ambisonic accumulator. Read the sim's
     * published field (handle-gated double buffer) once and ramp toward it (invariant 4). */
    const int path_on = v->path_on && c->path_tap && c->path_ambi_ch;
    const uint32_t pac = c->path_ambi_ch;
    float path_tgt[BW_AMBI_CH], path_step[BW_AMBI_CH];
    float pco_tgt[3][5], pco_step[3][5];      /* pathing bending-loss EQ: block target coeffs + per-sample glide */
    int   path_flat = 1;
    if (path_on) {
        int pf = atomic_load_explicit(&c->path_idx[idx], memory_order_acquire);
        const PathPub* pp = &c->path_pub[(size_t)idx * 2 + (size_t)pf];
        const int mine_path = (pp->handle == myh);
        for (uint32_t k = 0; k < pac; ++k) path_tgt[k] = mine_path ? pp->sh[k] : 0.f;
        for (uint32_t k = 0; k < pac; ++k) path_step[k] = (path_tgt[k] - v->path_sh_cur[k]) / (float)nr;
        /* bending-loss EQ: glide the band gains toward the published tilt (flat when not mine), then
         * compute this block's target biquad coeffs and interpolate the live coeffs per sample — the
         * same low-shelf/peak/high-shelf cascade the occlusion EQ uses, applied to s_raw pre-encode
         * (phonon's own path effect: EQ the mono signal, then scale each SH channel — path_effect.cpp). */
        float peq_tgt[3];
        for (int b = 0; b < 3; ++b) {
            peq_tgt[b] = mine_path ? pp->eq[b] : 1.f;
            v->path_eqg_cur[b] += (peq_tgt[b] - v->path_eqg_cur[b]) * EQ_SLEW;
            if (v->path_eqg_cur[b] < 1.f - EQ_FLAT_EPS || v->path_eqg_cur[b] > 1.f + EQ_FLAT_EPS) path_flat = 0;
        }
        if (!path_flat) v->path_eq_engaged = 1;
        if (v->path_eq_engaged) for (int b = 0; b < 3; ++b) {
            if (path_flat) { pco_tgt[b][0] = 1.f; pco_tgt[b][1] = pco_tgt[b][2] = pco_tgt[b][3] = pco_tgt[b][4] = 0.f; }
            else eq_coeffs(c->eq_proto[b].type, c->eq_proto[b].cw0, c->eq_proto[b].alpha, v->path_eqg_cur[b], pco_tgt[b]);
            for (int k = 0; k < 5; ++k) pco_step[b][k] = (pco_tgt[b][k] - v->path_eq_co[b][k]) / (float)nr;
        }
    }

    uint32_t cur = v->cursor;
    bool ended = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (i < start) continue;               /* scheduled start: this voice stays silent (and frozen) until the in-block offset */
        if (!streaming && cur >= snd->frames) {
            if (v->loop) do { cur -= snd->frames; } while (cur >= snd->frames);   /* pitch can overshoot the seam */
            else         ended = true;
        }
        float s;
        if (streaming)      s = c->stream_scratch[i];
        else if (ended)     s = 0.f;
        else if (use_pitch) {                  /* linear interp between cur and its successor */
            uint32_t i2 = cur + 1;
            if (i2 >= snd->frames) i2 = v->loop ? 0 : snd->frames - 1;
            s = snd->pcm[cur] + v->cur_frac * (snd->pcm[i2] - snd->pcm[cur]);
        } else s = snd->pcm[cur];
        s *= pg;                                               /* pause/seek gate (also gates the sends below) */
        const float s_raw = s;                                 /* pre-occlusion source, for the indirect (pathing) field */
        if (v->eq_engaged) {                                    /* 3 biquads (DF-I), coeffs interpolated per sample */
            for (int b = 0; b < 3; ++b) {
                float* co = v->eq_co[b];
                float y = co[0]*s + co[1]*v->eq_x1[b] + co[2]*v->eq_x2[b] - co[3]*v->eq_y1[b] - co[4]*v->eq_y2[b];
                v->eq_x2[b]=v->eq_x1[b]; v->eq_x1[b]=s; v->eq_y2[b]=v->eq_y1[b]; v->eq_y1[b]=y; s=y;
                for (int k = 0; k < 5; ++k) co[k] += co_step[b][k];   /* glide toward the block target */
            }
        }
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
                for (uint32_t ch = 0; ch < c->channels; ++ch) {
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
            if (v->path_eq_engaged) for (int b = 0; b < 3; ++b) {   /* ... but takes the bending-loss tilt (3 biquads, DF-I) */
                float* co = v->path_eq_co[b];
                float y = co[0]*sp + co[1]*v->path_eq_x1[b] + co[2]*v->path_eq_x2[b] - co[3]*v->path_eq_y1[b] - co[4]*v->path_eq_y2[b];
                v->path_eq_x2[b]=v->path_eq_x1[b]; v->path_eq_x1[b]=sp; v->path_eq_y2[b]=v->path_eq_y1[b]; v->path_eq_y1[b]=y; sp=y;
                for (int k = 0; k < 5; ++k) co[k] += pco_step[b][k];   /* glide toward the block target */
            }
            for (uint32_t k = 0; k < pac; ++k) {
                c->path_accum[(size_t)k * n + i] += sp * v->path_sh_cur[k];
                v->path_sh_cur[k] += path_step[k];
            }
        }
        if (use_dc) {                                          /* decor split: incoherent share to dc_bus ... */
            const float sd = s * dc_a;
            const float cs = 1.f - dc_a * dc_a;                /* ... coherent share stays on the main path */
            s *= cs > 0.f ? sqrtf(cs) : 0.f;                   /* (ramp float error can graze cs < 0) */
            for (uint32_t ch = 0; ch < c->channels; ++ch)
                c->dc_bus[(size_t)ch * n + i] += v->gcur[ch] * sd;
            dc_a += dc_step;
        }
        if (use_dual) {
            float lo = v->xover_lp + xover_a * (s - v->xover_lp); v->xover_lp = lo;   /* LP @ 700 Hz */
            for (uint32_t ch = 0; ch < c->channels; ++ch) {                           /* single + dmix-scaled LF re-weight */
                bus[(size_t)ch * n + i] += v->gcur[ch] * s + dmix * (v->gcur_lo[ch] - v->gcur[ch]) * lo;
                v->gcur_lo[ch] += step_lo[ch]; v->gcur[ch] += step[ch];
            }
            dmix += dmix_step;
        } else {
            for (uint32_t ch = 0; ch < c->channels; ++ch) {
                bus[(size_t)ch * n + i] += v->gcur[ch] * s;
                v->gcur[ch] += step[ch];
            }
        }
    }
    v->cursor = cur;
    if (ism_on) {                                               /* land the image gains exactly (invariant 4) */
        for (int m = 0; m < ism_n; ++m)
            for (uint32_t ch = 0; ch < c->channels; ++ch) v->ism_g[m][ch] = ism_gtgt[m][ch];
        v->ism_tail = 0;                                        /* the targets above were 0 when !ism_want, so
                                                                 * one block of ramp-out finishes the fade */
    }
    if (use_pitch) v->pitch_cur = v->pitch;                     /* land the rate glide exactly */
    if (path_on) for (uint32_t k = 0; k < pac; ++k) v->path_sh_cur[k] = path_tgt[k];   /* land exactly */
    if (path_on && v->path_eq_engaged) {                        /* land the pathing-EQ coeffs; bypass once settled flat */
        for (int b = 0; b < 3; ++b) for (int k = 0; k < 5; ++k) v->path_eq_co[b][k] = pco_tgt[b][k];
        if (path_flat) { v->path_eq_engaged = 0;
            for (int b = 0; b < 3; ++b) { v->path_eq_x1[b]=v->path_eq_x2[b]=v->path_eq_y1[b]=v->path_eq_y2[b]=0.f; } }
    }
    if (streaming) {                                            /* advance the stream position; end at a true EOF (not underrun) */
        v->stream_pos += strm_got;
        if (stream_ended(snd->stream, v->stream_pos)) ended = true;
    }
    v->occ_cur = occ_tgt;                                        /* land exactly (same local) */
    v->dir_cur = dir_tgt;
    if (v->air_on) v->air_a_cur = air_a_tgt;                     /* land the ramped propagation params */
    if (use_ldc) { v->ldc_g_cur = ldc_tgt;                       /* land the shelf; reset once settled flat */
                   if (!v->ldc_on && ldc_tgt == 1.f) v->ldc_lp = 0.f; }
    if (do_send)   v->refl_g_cur = refl_tgt;                     /* (the Doppler delay self-tracks per sample) */
    if (use_dual) {
        for (uint32_t ch = 0; ch < c->channels; ++ch) v->gcur_lo[ch] = v->gtarget_lo[ch];  /* land lo band */
        v->dual_mix = target_mix;                                    /* land the crossfade factor */
        if (target_mix == 0.f) v->xover_lp = 0.f;                    /* settled single next block: clean LP restart */
    }
    if (v->eq_engaged) {
        for (int b = 0; b < 3; ++b) for (int k = 0; k < 5; ++k) v->eq_co[b][k] = co_tgt[b][k];   /* land coeffs */
        if (flat) {                                              /* settled to passthrough: bypass + reset history */
            v->eq_engaged = 0;
            for (int b = 0; b < 3; ++b) { v->eq_x1[b]=v->eq_x2[b]=v->eq_y1[b]=v->eq_y2[b]=0.f; }
        }
    }
    for (uint32_t ch = 0; ch < c->channels; ++ch) v->gcur[ch] = v->gtarget[ch]; /* land exactly */

    if (ended) {
        v->playing = false;
        if (v->oneshot) {
            v->active = false;                 /* transient voice is finished */
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BW_MK_H(idx, v->gen) };
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

/* Mix an ambisonic BED voice: decode its SH channels straight onto the 26-ch bus through the static
 * decode matrix (world-locked — no DBAP, occlusion, or directivity), with a master-gain ramp on
 * gcur[0]. Looping / natural end / oneshot-ack are identical to mix_voice.
 *
 * With the PARAMETRIC renderer selected (bw_set_bed_renderer — first-order DirAC in coarse
 * time-domain bands), the bed's FOA channels are analyzed per band into a direction + diffuseness
 * (smoothed intensity vector vs energy): the NON-DIFFUSE stream re-pans W through the
 * listener-relative panner at a virtual source on the array shell (ref + bed_radius*doa — the bed
 * becomes walkable: off-centre listeners get parallax a matrix decode can't give), and the DIFFUSE
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
    const int want_p  = nch >= 4 && c->bed_param_blk;     /* the block's single load (rt_render) */
    const int use_p   = want_p || pb->mix > 0.f;
    /* bed yaw (rt_bed_set_rotation): glide toward the target at BW_BED_YAW_RATE and rotate the field
     * with a per-sample phasor recurrence (no per-sample trig; m = 2,3 by angle addition). Rotation
     * happens BEFORE either renderer, so the matrix decode and the parametric analysis see the same
     * turned field. Settled at 0 -> fully bypassed. */
    const int use_rot = v->yaw != 0.f || v->yaw_cur != 0.f;
    float rc1 = 1.f, rs1 = 0.f, rdc = 1.f, rds = 0.f;
    if (use_rot) {
        const float dmax = BW_BED_YAW_RATE * (float)nr / (float)c->sample_rate;
        float dtot = v->yaw - v->yaw_cur;
        if (dtot > dmax) dtot = dmax; else if (dtot < -dmax) dtot = -dmax;
        const float dphi = dtot / (float)nr;
        rc1 = cosf(v->yaw_cur); rs1 = sinf(v->yaw_cur);
        rdc = cosf(dphi); rds = sinf(dphi);
        v->yaw_cur += dtot;                               /* land this block's glide */
    }
    uint32_t cur = v->cursor;
    bool ended = false;

    if (!use_p) {                       /* pure matrix decode: the settled default stays this cheap */
        for (uint32_t i = 0; i < n; ++i) {
            if (i < start) continue;               /* scheduled start: silent until the in-block offset */
            if (cur >= snd->frames) { if (v->loop) cur = 0; else ended = true; }
            if (!ended) {
                const float* sh = &snd->pcm[(size_t)cur * nch];
                float shr[BW_AMBI_CH];
                if (use_rot) {                     /* turn the field, then advance the yaw phasor */
                    const float cm[3] = { rc1, rc1*rc1 - rs1*rs1, rc1*(rc1*rc1 - rs1*rs1) - rs1*(2.f*rc1*rs1) };
                    const float sm[3] = { rs1, 2.f*rc1*rs1,       rs1*(rc1*rc1 - rs1*rs1) + rc1*(2.f*rc1*rs1) };
                    bed_rotate_z(sh, nch, cm, sm, shr);
                    sh = shr;
                    const float t = rc1*rdc - rs1*rds; rs1 = rs1*rdc + rc1*rds; rc1 = t;
                }
                const float g = v->gcur[0] * pg;   /* master gain x the pause/seek gate */
                for (uint32_t s = 0; s < c->channels; ++s) {
                    const float* D = c->bed_decode[s];
                    float acc = 0.f;
                    for (int k = 0; k < nch; ++k) acc += sh[k] * D[k];
                    bus[(size_t)s * n + i] += g * acc;
                }
                ++cur;
            }
            pg += pg_step;
            v->gcur[0] += g_step;
        }
    } else {
        /* block-rate parameter update from the SMOOTHED analysis state (last blocks' field): per band
         * psi = 1 - |I|/E (0 = a plane wave, 1 = isotropic/incoherent), direct gains from the panner
         * at ref + R*doa. Targets ramp across the block; this block's samples update the smoothing. */
        const int p = atomic_load_explicit(&c->panner, memory_order_acquire);
        float da_tgt[BW_PARA_BANDS], fa_tgt[BW_PARA_BANDS];
        for (int b = 0; b < BW_PARA_BANDS; ++b) {
            const float E = pb->E[b];
            const float In = sqrtf(pb->I[b][0]*pb->I[b][0] + pb->I[b][1]*pb->I[b][1] + pb->I[b][2]*pb->I[b][2]);
            float psi = (E > 1e-12f) ? 1.f - In / E : 1.f;
            if (psi < 0.f) psi = 0.f; else if (psi > 1.f) psi = 1.f;
            da_tgt[b] = sqrtf(1.f - psi) * c->bed_pref;
            fa_tgt[b] = sqrtf(psi);
            if (In > 1e-9f) {                          /* doa (ambi) -> room -> virtual source on the shell */
                float ax = pb->I[b][0]/In, ay = pb->I[b][1]/In, az = pb->I[b][2]/In;
                float pos[3] = { c->layout.ref[0] + c->bed_radius * ay,     /* room x = ambi y */
                                 c->layout.ref[1] + c->bed_radius * az,     /* room y = ambi z */
                                 c->layout.ref[2] + c->bed_radius * ax };   /* room z = ambi x */
                float gt[BW_CHANNELS];
                panner_gains(c, p, pos, 1.f, gt);
                double gp = 0.0; for (uint32_t ch = 0; ch < c->channels; ++ch) gp += (double)gt[ch]*gt[ch];
                float gn = gp > 1e-12 ? (float)(1.0 / sqrt(gp)) : 0.f;      /* unit power: no distance atten
                                                                             * (a bed has direction, not range) */
                for (uint32_t ch = 0; ch < c->channels; ++ch) pb->g_tgt[b][ch] = gt[ch] * gn;
            }                                          /* |I| ~ 0: direction undefined; keep the last gains
                                                        * (da -> 0 there anyway, psi -> 1) */
        }
        float g_stp[BW_PARA_BANDS][BW_CHANNELS], da_stp[BW_PARA_BANDS], fa_stp[BW_PARA_BANDS];
        for (int b = 0; b < BW_PARA_BANDS; ++b) {
            da_stp[b] = (da_tgt[b] - pb->da_cur[b]) / (float)nr;
            fa_stp[b] = (fa_tgt[b] - pb->fa_cur[b]) / (float)nr;
            for (uint32_t ch = 0; ch < c->channels; ++ch)
                g_stp[b][ch] = (pb->g_tgt[b][ch] - pb->g_cur[b][ch]) / (float)nr;
        }
        const float pm_tgt  = want_p ? 1.f : 0.f;
        const float pm_step = (pm_tgt - pb->mix) / (float)nr;
        float pmix = pb->mix;
        const int   run_matrix = !(pb->mix >= 1.f && pm_tgt >= 1.f);   /* both paths only mid-crossfade */
        float aE[BW_PARA_BANDS] = { 0 }, aI[BW_PARA_BANDS][3] = {{ 0 }};
        c->dc_wrote = 1;                               /* the diffuse stream lands in the decor bus */

        for (uint32_t i = 0; i < n; ++i) {
            if (i < start) continue;
            if (cur >= snd->frames) { if (v->loop) cur = 0; else ended = true; }
            if (!ended) {
                const float* sh = &snd->pcm[(size_t)cur * nch];
                float shr[BW_AMBI_CH];
                if (use_rot) {                         /* turn the field before EITHER renderer sees it */
                    const float cm[3] = { rc1, rc1*rc1 - rs1*rs1, rc1*(rc1*rc1 - rs1*rs1) - rs1*(2.f*rc1*rs1) };
                    const float sm[3] = { rs1, 2.f*rc1*rs1,       rs1*(rc1*rc1 - rs1*rs1) + rc1*(2.f*rc1*rs1) };
                    bed_rotate_z(sh, nch, cm, sm, shr);
                    sh = shr;
                    const float t = rc1*rdc - rs1*rds; rs1 = rs1*rdc + rc1*rds; rc1 = t;
                }
                const float g = v->gcur[0] * pg;
                if (run_matrix) {                      /* matrix share of the crossfade */
                    const float gm = g * (1.f - pmix);
                    for (uint32_t s = 0; s < c->channels; ++s) {
                        const float* D = c->bed_decode[s];
                        float acc = 0.f;
                        for (int k = 0; k < nch; ++k) acc += sh[k] * D[k];
                        bus[(size_t)s * n + i] += gm * acc;
                    }
                }
                /* FOA band split (ACN: W Y Z X), complementary one-poles -> BW_PARA_BANDS bands */
                float bnd[4][BW_PARA_BANDS];           /* [foa channel][band] */
                for (int f = 0; f < 4; ++f) {
                    const float s0 = sh[f];
                    float l0 = (pb->lp[0][f] += c->para_xa[0] * (s0 - pb->lp[0][f]));
                    float l1 = (pb->lp[1][f] += c->para_xa[1] * (s0 - pb->lp[1][f]));
                    float l2 = (pb->lp[2][f] += c->para_xa[2] * (s0 - pb->lp[2][f]));
                    bnd[f][0] = l0; bnd[f][1] = l1 - l0; bnd[f][2] = l2 - l1; bnd[f][3] = s0 - l2;
                }
                float sd[BW_PARA_BANDS], dfo[4] = { 0, 0, 0, 0 };
                for (int b = 0; b < BW_PARA_BANDS; ++b) {
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
                    for (int b = 0; b < BW_PARA_BANDS; ++b) {
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
        }
        /* land the ramps exactly + fold this block's analysis into the smoothed field */
        pb->mix = pm_tgt;
        for (int b = 0; b < BW_PARA_BANDS; ++b) {
            pb->da_cur[b] = da_tgt[b]; pb->fa_cur[b] = fa_tgt[b];
            for (uint32_t ch = 0; ch < c->channels; ++ch) pb->g_cur[b][ch] = pb->g_tgt[b][ch];
        }
        const float a = 1.f - expf(-(float)nr / (BW_PARA_TAU * (float)c->sample_rate));
        const float inv_nr = 1.f / (float)nr;
        for (int b = 0; b < BW_PARA_BANDS; ++b) {
            pb->E[b] += a * (aE[b] * inv_nr - pb->E[b]);
            for (int j = 0; j < 3; ++j) pb->I[b][j] += a * (aI[b][j] * inv_nr - pb->I[b][j]);
        }
    }

    v->cursor = cur;
    v->gcur[0] = v->gtarget[0];
    if (ended) {
        v->playing = false;
        if (v->oneshot) {
            v->active = false;
            Evt ev = { .type = EVT_VOICE_ENDED, .handle = BW_MK_H(idx, v->gen) };
            evt_push(&c->events, &ev);
        }
    }
}

/* Tracked room EQ (layout room_eq_grid): interpolate the grid's per-speaker section-cut depths at
 * the live listener position — inverse-distance weighting over the measurement positions, smoothed
 * by an epsilon so standing exactly on a mic point still blends its neighbours — and hand them to
 * the aligner as slew targets (align_room_eq_targets; the slew makes the walk click-free). Audio
 * thread, no alloc/locks; recomputed only when the listener moved > ~1 cm since the last solve
 * (align keeps slewing toward the standing targets meanwhile). The live kill switch
 * (rt_set_room_eq_dyn) aims every section at flat instead of stepping the EQ out. */
static void room_eq_track(RtCore* c) {
    const RoomEqGrid* g = &c->layout.rq_grid;
    if (!g->npos || !c->aligner) return;
    float tgt[BW_CHANNELS][BW_ROOM_EQ_MAX];
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
    double w[BW_RQ_GRID_MAX], wsum = 0.0;
    for (uint8_t i = 0; i < g->npos; ++i) {           /* IDW: w = 1/(d² + eps²), eps = 15 cm */
        float dx = lp[0]-g->pos[i][0], dy = lp[1]-g->pos[i][1], dz = lp[2]-g->pos[i][2];
        w[i] = 1.0 / ((double)(dx*dx + dy*dy + dz*dz) + 0.0225);
        wsum += w[i];
    }
    double inv = 1.0 / wsum;
    for (uint32_t k = 0; k < c->channels; ++k)
        for (uint8_t s = 0; s < g->nsec[k]; ++s) {
            double acc = 0.0;
            for (uint8_t i = 0; i < g->npos; ++i) acc += w[i] * (double)g->gain_db[i][k][s];
            tgt[k][s] = (float)(acc * inv);
        }
    align_room_eq_targets(c->aligner, tgt);
    memcpy(c->rq_lis, lp, sizeof c->rq_lis);
    c->rq_state = 1;
}

void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts) {
    /* dsp clock for sample-accurate scheduling: prefer the device sample position (ASIO/null); fall
     * back to an internal block counter when no timestamp is supplied (e.g. direct rt_render in tests).
     * block_start is the absolute dsp-sample of bus[0]; publish it so the control thread can schedule
     * relative to "now" (rt_dsp_time). */
    uint64_t block_start = ts ? ts->sample_pos : c->dsp_block;
    c->dsp_block = block_start + nframes;
    atomic_store_explicit(&c->dsp_now, block_start, memory_order_relaxed);
#if defined(_MSC_VER)
    /* Flush denormals to zero on the audio thread: gain ramps toward 0 (e.g. a voice
     * moving off a channel) otherwise produce subnormals that stall the FP pipeline. */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    BW_ZONE_BEGIN(zr, "rt_render");
    drain_commands(c);

    /* Every RT scratch buffer (aux / stream_scratch / path_accum) is sized BW_RT_MAX_BLOCK, which
     * matches the engine's BW_MAX_BLOCK device-block ceiling. If a driver ever presents a larger
     * block, fail safe to silence rather than overflow the scratch on the audio thread. (Commands
     * are still drained above so the control ring never backs up.) */
    if (nframes > BW_RT_MAX_BLOCK) {
        memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
        BW_ZONE_END(zr);
        return;
    }

    /* track_internal: sample the freshest tracked head pose at block time, overriding the
     * committed listener (lower latency than routing pose through the command ring). A position
     * change dirties every voice, since DBAP gains are all listener-relative. */
    if (c->tracker) {
        float tp[3], tq[4];
        uint64_t tns = 0;
        if (pose_read_t(c->tracker, tp, tq, &tns)) {
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
            memcpy(c->lis.p_active, tp, sizeof tp);
            memcpy(c->lis.q_active, tq, sizeof tq);
            if (moved) for (uint32_t i = 0; i < c->voice_cap; ++i) c->voices[i].dirty = true;
        }
    }

    memset(bus, 0, sizeof(float) * (size_t)nframes * c->channels);
    /* Acquire-load the taps once (paired with the release stores in rt_set_*_tap): a non-NULL tap
     * guarantees its ud/ambi_ch are visible, so registration mid-run (bw_start) can't tear. */
    const RtBusTap  bus_tap  = atomic_load_explicit(&c->bus_tap,  memory_order_acquire);
    const RtPathTap path_tap = atomic_load_explicit(&c->path_tap, memory_order_acquire);
    /* the reflection aux send: collected this block if a tap is registered + the block fits the scratch */
    float* aux = (bus_tap && nframes <= BW_RT_MAX_BLOCK) ? c->aux : NULL;
    if (aux) memset(aux, 0, sizeof(float) * (size_t)nframes);
    const int path_active = (path_tap && c->path_ambi_ch && nframes <= BW_RT_MAX_BLOCK);
    if (path_active) memset(c->path_accum, 0, sizeof(float) * (size_t)c->path_ambi_ch * nframes);  /* pathing accumulator */
    /* decor bus: zeroed whenever a mixer might write it (spread toggle on, the parametric bed's
     * diffuse stream, or something still ramping/flushing out). The toggles are loaded ONCE for the
     * whole block — gate and mixers must agree, or a toggle landing mid-block drops a block of the
     * incoherent share into a bus that never gets convolved. */
    c->dc_on_blk     = atomic_load_explicit(&c->decor_on,  memory_order_acquire);
    c->bed_param_blk = atomic_load_explicit(&c->bed_param, memory_order_acquire);
    c->all_paused_blk = atomic_load_explicit(&c->all_paused, memory_order_acquire);   /* one load: every
                                                                                       * gate agrees this block */
    const int dc_live = c->dc_on_blk || c->bed_param_blk || c->dc_tail > 0;
    if (dc_live) memset(c->dc_bus, 0, sizeof(float) * (size_t)c->channels * nframes);
    c->dc_wrote = 0;
    int rt_active = 0;
    BW_ZONE_BEGIN(zmix, "mix voices");
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
        if (v->dirty) { compute_gains(c, v); v->dirty = false; }
        if (v->sound->channels > 1) mix_bed  (c, v, (uint16_t)i, bus, nframes, off);        /* ambisonic bed */
        else                        mix_voice(c, v, (uint16_t)i, bus, nframes, off, aux);   /* mono point source */
    }
    BW_ZONE_END(zmix);
    BW_PLOT("rt voices", rt_active);
    atomic_store_explicit(&c->active_pub, (uint32_t)rt_active, memory_order_relaxed);   /* rt_active_voices */
    /* decorrelation: convolve the decor bus through each channel's sparse velvet filter into the main
     * bus. Runs while anything wrote this block, plus one filter-length of flush so the tail rings
     * out; on going idle the history is wiped, so a later re-engage can never replay stale samples. */
    if (c->dc_wrote) c->dc_tail = c->dc_histlen;
    if (dc_live && c->dc_tail > 0) {
        BW_ZONE_BEGIN(zdc, "decorrelate");
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
        BW_ZONE_END(zdc);
    }
    for (uint32_t i = 0; i < c->voice_cap; ++i) {   /* publish playback state for rt_source_is_playing (control thread) */
        const Voice* v = &c->voices[i];
        uint32_t st = ((uint32_t)v->gen << 1) | ((v->active && v->playing && v->sound) ? 1u : 0u);
        atomic_store_explicit(&c->play_pub[i], st, memory_order_release);
    }
    if (aux) {   /* reflection bed: convolve the aux send + sum onto the bus BEFORE align (so it gets trim+delay too) */
        BW_ZONE_BEGIN(zt, "reflect tap");
        bus_tap(c->bus_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, aux);
        BW_ZONE_END(zt);
    }
    if (path_active) {   /* pathing: decode the summed indirect ambisonic field onto the bus (also pre-align) */
        BW_ZONE_BEGIN(zp, "path tap");
        path_tap(c->path_tap_ud, bus, nframes, c->lis.p_active, c->lis.q_active, c->path_accum, c->path_ambi_ch);
        BW_ZONE_END(zp);
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
    BW_ZONE_BEGIN(za, "align");
    align_process(c->aligner, bus, nframes);   /* per-speaker gain trim + delay (output stage) */
    BW_ZONE_END(za);

    /* debug channel test (bw_test_signal): inject a built-in signal onto a raw output channel AFTER
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
                if (s >  lim_c) s =  lim_c;
                if (s < -lim_c) s = -lim_c;
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
    BW_ZONE_END(zr);
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
 * a finished non-loop voice, reads as not-playing). Best-effort: a sound shorter than a poll interval
 * may never be observed playing. */
bool rt_source_is_playing(RtCore* c, uint32_t h) {
    if (!c || h == 0) return false;
    uint32_t idx = BW_H_IDX(h);
    if (idx >= c->voice_cap) return false;
    uint32_t st = atomic_load_explicit(&c->play_pub[idx], memory_order_acquire);
    return ((st >> 1) == BW_H_GEN(h)) && (st & 1u) != 0u;
}

/* Control thread: the engine's current dsp-sample clock (the most recently rendered block's first
 * sample). Schedule a sample-accurate play with rt_source_play_at(.., rt_dsp_time(c) + delay_samples). */
uint64_t rt_dsp_time(RtCore* c) {
    return c ? atomic_load_explicit(&c->dsp_now, memory_order_relaxed) : 0;
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
            uint32_t vh = BW_MK_H((uint16_t)victim, c->gen[victim]);
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

/* Steal priority (control-side only — the audio thread never reads it): 0 = first to be stolen when
 * the pool is full .. 255 = protected. Take effect immediately; safe any time. */
void rt_source_set_priority(RtCore* c, uint32_t h, int priority) {
    if (!c) return;
    uint16_t idx = BW_H_IDX(h);
    if (idx < c->voice_cap && c->inuse[idx] && c->gen[idx] == BW_H_GEN(h))
        c->priority[idx] = (uint8_t)(priority < 0 ? 0 : priority > 255 ? 255 : priority);
}

void rt_source_destroy(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_SRC_DESTROY, .handle = h };
    /* Recycle only if the destroy was actually enqueued, so a dropped command can't leave
     * the voice active while the index is handed out again. recycle is idempotent, so a
     * double-destroy is harmless. */
    if (cmd_push(&c->cmds, &cmd)) recycle_handle(c, h);
}

void rt_source_set_pos(RtCore* c, uint32_t h, float x, float y, float z) {
    if (!(isfinite(x) && isfinite(y) && isfinite(z))) return;  /* keep NaN/Inf off the audio thread */
    Cmd cmd = { .type = CMD_SET_POS, .handle = h };
    cmd.u.pos.x = x; cmd.u.pos.y = y; cmd.u.pos.z = z;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_gain(RtCore* c, uint32_t h, float linear) {
    if (!isfinite(linear) || linear < 0.f) return;             /* reject NaN/Inf/negative gain */
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
    c->path_ambi_ch = (ambi_ch > BW_AMBI_CH) ? BW_AMBI_CH : ambi_ch;
    atomic_store_explicit(&c->path_tap, tap, memory_order_release);  /* ... acquire-load of a non-NULL tap sees both */
}

/* Off-thread pathing sim publishes a voice's path field: write the back buffer, flip the index (release).
 * `sh` = shCoeffs (directions + level); `eq` = the 3-band bending-loss tilt (NULL = flat). Handle stored
 * alongside so the audio thread drops a stale/recycled slot. */
void rt_set_pathing(RtCore* c, uint32_t handle, const float* sh, const float* eq, uint32_t ambi_ch) {
    if (!c || !sh) return;
    uint32_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    if (ambi_ch > BW_AMBI_CH) ambi_ch = BW_AMBI_CH;
    int cur = atomic_load_explicit(&c->path_idx[idx], memory_order_relaxed);
    PathPub* back = &c->path_pub[(size_t)idx * 2 + (size_t)(1 - cur)];
    back->handle = handle;
    for (uint32_t k = 0; k < ambi_ch; ++k)        back->sh[k] = sh[k];
    for (uint32_t k = ambi_ch; k < BW_AMBI_CH; ++k) back->sh[k] = 0.f;
    for (int b = 0; b < 3; ++b) back->eq[b] = eq ? eq[b] : 1.f;   /* NULL eq = flat (no bending loss) */
    atomic_store_explicit(&c->path_idx[idx], 1 - cur, memory_order_release);
}

void rt_source_set_reflection_send(RtCore* c, uint32_t h, float gain) {
    if (!isfinite(gain) || gain < 0.f) return;
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
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_SIZE, .handle = h };
    cmd.u.size.radius = radius_m;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_fade_to(RtCore* c, uint32_t h, float gain, float seconds, bool stop_at_end) {
    if (!c) return;
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

/* The shoebox for the image-source reflections. Set while the audio thread is stopped (bw_start
 * reads it); NULL or an invalid room disables early reflections engine-wide. */
void rt_set_ism_room(RtCore* c, const IsmRoom* room) {
    if (!c) return;
    if (room) c->ism_room = *room;
    else      memset(&c->ism_room, 0, sizeof c->ism_room);
}

void rt_set_ism_gain(RtCore* c, float linear) {
    if (!c) return;
    if (linear < 0.f) linear = 0.f;
    atomic_store_explicit(&c->ism_gain, linear, memory_order_relaxed);   /* the solve re-reads it per block */
}

void rt_bed_set_rotation(RtCore* c, uint32_t h, float yaw_rad) {
    if (!c) return;
    Cmd cmd = { .type = CMD_BED_YAW, .handle = h };
    cmd.u.byaw.yaw = yaw_rad;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_group(RtCore* c, uint32_t h, uint32_t group) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_GROUP, .handle = h };
    cmd.u.group.id = group < BW_GROUPS ? (uint8_t)group : 0;
    cmd_push(&c->cmds, &cmd);
}

void rt_group_set_gain(RtCore* c, uint32_t group, float linear) {
    if (!c || group >= BW_GROUPS) return;
    Cmd cmd = { .type = CMD_GROUP_GAIN, .handle = 0 };
    cmd.u.ggain.id = (uint8_t)group;
    cmd.u.ggain.gain = linear;
    cmd_push(&c->cmds, &cmd);
}

void rt_group_set_paused(RtCore* c, uint32_t group, bool paused) {
    if (!c || group >= BW_GROUPS) return;
    Cmd cmd = { .type = CMD_GROUP_PAUSED, .handle = 0 };
    cmd.u.gpause.id = (uint8_t)group;
    cmd.u.gpause.on = paused ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_spread(RtCore* c, uint32_t h, float amount) {
    if (!isfinite(amount)) return;
    Cmd cmd = { .type = CMD_SET_SPREAD, .handle = h };
    cmd.u.spread.amount = amount;
    cmd_push(&c->cmds, &cmd);
}

void rt_test_signal(RtCore* c, uint32_t channel, uint8_t kind, float gain) {
    if (!c) return;
    Cmd cmd = { .type = CMD_TEST_SIGNAL };
    cmd.u.test.channel = channel; cmd.u.test.kind = kind; cmd.u.test.gain = gain;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_play(RtCore* c, uint32_t h, uint32_t sound, bool loop) {
    rt_source_play_at(c, h, sound, loop, 0);   /* 0 = start immediately */
}

void rt_source_play_at(RtCore* c, uint32_t h, uint32_t sound, bool loop, uint64_t start_sample) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;          /* invalid or being unloaded: drop the play so the
                                             * audio thread can never bind a retiring sound */
    /* streamed sound: kick the background decode (re-seek + fill) now, off the audio thread. The
     * voice reads its ring from sample 0; the first blocks may be silent until the ring fills (~ms). */
    if (s->data.stream) stream_start(s->data.stream, loop ? 1 : 0);
    Cmd cmd = { .type = CMD_PLAY, .handle = h };
    cmd.u.play.sound = sound; cmd.u.play.loop = loop ? 1u : 0u; cmd.u.play.oneshot = 0u;
    cmd.u.play.start = start_sample;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_stop(RtCore* c, uint32_t h) {
    Cmd cmd = { .type = CMD_STOP, .handle = h };
    cmd_push(&c->cmds, &cmd);
}

void rt_source_set_paused(RtCore* c, uint32_t h, bool paused) {
    Cmd cmd = { .type = CMD_SET_PAUSED, .handle = h };
    cmd.u.pause.on = paused ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

void rt_source_seek(RtCore* c, uint32_t h, uint64_t frame) {
    Cmd cmd = { .type = CMD_SEEK, .handle = h };
    cmd.u.seek.frame = frame;
    cmd_push(&c->cmds, &cmd);
}

void rt_set_listener(RtCore* c, const float p[3], const float q[4]) {
    Cmd cmd = { .type = CMD_SET_LISTENER };
    cmd.u.lis.px = p[0]; cmd.u.lis.py = p[1]; cmd.u.lis.pz = p[2];
    cmd.u.lis.qx = q[0]; cmd.u.lis.qy = q[1]; cmd.u.lis.qz = q[2]; cmd.u.lis.qw = q[3];
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
        if (err && errcap) { strncpy(err, "sound: table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    c->sounds[BW_H_IDX(h)].data = d;     /* published to the audio thread when a CMD_PLAY references it */
    return h;
}

/* Load a sound for STREAMING (mono point source, engine rate): the file is not decoded into RAM —
 * a background thread feeds its ring as the voice plays (rt_source_play). 0 + err on failure. */
uint32_t rt_load_sound_streaming(RtCore* c, const char* path, char* err, size_t errcap) {
    if (!c) return 0;
    Stream* st = stream_open(c->streams, path, err, errcap);
    if (!st) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        stream_close(c->streams, st);
        if (err && errcap) { strncpy(err, "stream: sound table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    SoundData d; memset(&d, 0, sizeof d);
    d.stream = st; d.channels = 1; d.sample_rate = c->sample_rate;   /* pcm NULL, frames 0: the stream tracks EOF */
    c->sounds[BW_H_IDX(h)].data = d;
    return h;
}

/* Load a multichannel AmbiX asset into the sound table (plays as an ambisonic bed via mix_bed). */
uint32_t rt_load_ambix(RtCore* c, const char* path, char* err, size_t errcap) {
    SoundData d;
    if (!sound_load_ambix(path, c->sample_rate, &d, err, errcap)) return 0;
    uint32_t h = salloc_sound(c);
    if (!h) {
        sound_unload(&d);
        if (err && errcap) { strncpy(err, "ambix: sound table full", errcap - 1); err[errcap - 1] = 0; }
        return 0;
    }
    c->sounds[BW_H_IDX(h)].data = d;
    return h;
}

/* Channel count of a loaded asset (control thread): 1 = mono point source, 4/9/16 = ambisonic bed.
 * 0 if the handle is invalid. Lets the engine reject a type-mismatched play (mono on a bed / vice versa). */
uint16_t rt_sound_channels(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    return s ? s->data.channels : 0;
}

void rt_unload_sound(RtCore* c, uint32_t sound) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;       /* invalid or already retiring: idempotent no-op */
    s->retiring = 1;                     /* refuse new binds (rt_source_play checks this) */
    Cmd cmd = { .type = CMD_SOUND_RETIRE, .handle = sound };
    if (!cmd_push(&c->cmds, &cmd)) s->retiring = 0;   /* ring full: revert so it can be retried */
}

/* Fire-and-forget: a transient voice that recycles itself on EVT_VOICE_ENDED. Its position
 * takes effect on the next rt_commit (the engine's per-frame commit). */
void rt_play_oneshot(RtCore* c, uint32_t sound, float x, float y, float z, float gain) {
    SoundSlot* s = sound_slot_ctrl(c, sound);
    if (!s || s->retiring) return;
    if (!(isfinite(x) && isfinite(y) && isfinite(z) && isfinite(gain) && gain >= 0.f)) return;
    /* A oneshot enqueues 4 commands (CREATE/SET_POS/SET_GAIN/PLAY) that must all land, or
     * the transient voice is created-but-never-played and leaks (it never ends -> never
     * acks EVT_VOICE_ENDED -> never recycled). Reserve room for all 4 up front so a
     * ring-full case drops the whole oneshot rather than half of it. */
    if (cmd_free(&c->cmds) < 4) return;
    uint32_t h = (c->free_count > c->fade_reserve) ? alloc_handle(c) : 0;   /* don't spend the steal reserve */
    if (!h) return;                          /* full pool: the fire-and-forget oneshot is simply dropped */
    Cmd create = { .type = CMD_SRC_CREATE, .handle = h };
    cmd_push(&c->cmds, &create);              /* the 4 pushes are guaranteed by cmd_free >= 4 */
    rt_source_set_pos(c, h, x, y, z);
    rt_source_set_gain(c, h, gain);
    Cmd play = { .type = CMD_PLAY, .handle = h };
    play.u.play.sound = sound; play.u.play.loop = 0u; play.u.play.oneshot = 1u;
    cmd_push(&c->cmds, &play);
}

/* ---- lifecycle ---- */

/* Extra physical voice slots beyond the caller's pool. A full-pool steal fades the victim out on its
 * own slot (one block) and places the new source on a reserve slot, so the steal is click-free; the
 * victim's slot returns to the pool when the fade completes. Bounds how many steals per frame can be
 * click-free (beyond it, a steal falls back to a hard cut). */
#define BW_FADE_RESERVE 8

RtCore* rt_create(uint32_t req_voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels) {
    if (req_voice_cap == 0 || req_voice_cap > 0xFFFFu - BW_FADE_RESERVE || sound_cap == 0 || sound_cap > 0xFFFFu ||
        channels  == 0 || channels  > BW_CHANNELS || sample_rate == 0)
        return NULL;
    const uint32_t voice_cap = req_voice_cap + BW_FADE_RESERVE;   /* physical slots (user pool + fade reserve) */
    /* Bound the event ring: between two control-thread drains the audio thread emits at
     * most one EVT_VOICE_ENDED per voice plus one EVT_SOUND_RETIRED per sound, so
     * EVT_CAP >= voice_cap + sound_cap makes the event ring un-overflowable (acks can
     * never be silently dropped on the audio thread). */
    if ((uint64_t)voice_cap + sound_cap > EVT_CAP) return NULL;
    RtCore* c = (RtCore*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->voice_cap   = voice_cap;
    c->fade_reserve = BW_FADE_RESERVE;
    c->sound_cap   = sound_cap;
    c->channels    = channels;
    c->sample_rate = sample_rate;
    c->xover_a     = 1.f - expf(-6.2831853f * BW_DUALBAND_FC / (float)sample_rate);   /* dual-band crossover */
    c->test_noise  = 0x9e3779b9u;       /* non-zero LCG seed for the channel-test noise */
    c->voices    = (Voice*)    calloc(voice_cap, sizeof(Voice));
    c->occ_handle = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->occ_val    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->occ_eq     = (_Atomic uint64_t*)calloc(voice_cap, sizeof(_Atomic uint64_t));
    c->occ_dir    = (_Atomic float*)   calloc(voice_cap, sizeof(_Atomic float));
    c->play_pub   = (_Atomic uint32_t*)calloc(voice_cap, sizeof(_Atomic uint32_t));
    c->aux        = (float*)   calloc(BW_RT_MAX_BLOCK, sizeof(float));   /* reflection aux-send scratch */
    c->stream_scratch = (float*)calloc(BW_RT_MAX_BLOCK, sizeof(float));  /* per-block streaming pull scratch */
    c->path_accum = (float*)calloc((size_t)BW_AMBI_CH * BW_RT_MAX_BLOCK, sizeof(float));  /* pathing ambisonic accumulator */
    c->path_pub   = calloc((size_t)voice_cap * 2, sizeof *c->path_pub);  /* per-voice double-buffered path field */
    c->path_idx   = (_Atomic int*)calloc(voice_cap, sizeof(_Atomic int));
    c->streams    = stream_set_create(sample_rate);                     /* background file streaming */
    c->gen       = (uint16_t*) calloc(voice_cap, sizeof(uint16_t));
    c->inuse     = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->priority  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->stealing  = (uint8_t*)  calloc(voice_cap, sizeof(uint8_t));
    c->freelist  = (uint32_t*) calloc(voice_cap, sizeof(uint32_t));
    c->sounds    = (SoundSlot*)calloc(sound_cap, sizeof(SoundSlot));
    c->sfreelist = (uint32_t*) calloc(sound_cap, sizeof(uint32_t));
    {   /* Doppler ring pool: power-of-two ring per voice, sized to BW_DOPPLER_MAX_DIST at this rate */
        uint32_t need = (uint32_t)(BW_DOPPLER_MAX_DIST / BW_SPEED_OF_SOUND * (float)sample_rate) + 2;
        uint32_t rl = 1; while (rl < need) rl <<= 1;
        c->dop_ringlen = rl;
        c->dop_ring = (float*)calloc((size_t)voice_cap * rl, sizeof(float));
    }
    {   /* image-source reflection rings: one power-of-two ring per voice, sized to the longest
         * reflection path the renderer supports (BW_ISM_MAX_M). Allocated with the voice pool — a
         * reflection is the direct signal delayed by its own path, so each voice needs its own. */
        uint32_t need = (uint32_t)(BW_ISM_MAX_M / BW_SPEED_OF_SOUND * (float)sample_rate) + 2;
        uint32_t rl = 1; while (rl < need) rl <<= 1;
        c->ism_ringlen = rl;
        c->ism_ring = (float*)calloc((size_t)voice_cap * rl, sizeof(float));
        atomic_store_explicit(&c->ism_gain, 1.f, memory_order_relaxed);
    }
    {   /* per-channel velvet-noise decorrelators (bw_set_decorrelation): BW_DECOR_TAPS taps on a
         * jittered grid over BW_DECOR_MS, exponential decay envelope, random signs, normalized to
         * unit ENERGY (decorrelated copies must carry the power the coherent copy gave up). A
         * different LCG seed per channel makes the speaker feeds mutually incoherent. */
        uint32_t span = (uint32_t)(BW_DECOR_MS * 1e-3f * (float)sample_rate);
        uint32_t hl = 1; while (hl < span + BW_RT_MAX_BLOCK) hl <<= 1;
        c->dc_histlen = hl;
        c->dc_hmask   = hl - 1;
        c->dc_ntaps   = BW_DECOR_TAPS;
        c->dc_bus  = (float*)calloc((size_t)BW_CHANNELS * BW_RT_MAX_BLOCK, sizeof(float));
        c->dc_hist = (float*)calloc((size_t)BW_CHANNELS * hl, sizeof(float));
        const float grid = (float)span / (float)BW_DECOR_TAPS;
        for (uint32_t ch = 0; ch < BW_CHANNELS; ++ch) {
            uint32_t rng = 0x9E3779B9u * (ch + 1) + 12345u;
            double e2 = 0.0;
            for (uint32_t t = 0; t < BW_DECOR_TAPS; ++t) {
                rng = rng * 1664525u + 1013904223u;
                float u = (float)(rng >> 8) * (1.0f / 16777216.0f);          /* [0,1) jitter */
                uint32_t off = (uint32_t)((float)t * grid + u * grid);
                if (off >= span) off = span - 1;
                c->dc_off[ch][t] = (uint16_t)off;
                rng = rng * 1664525u + 1013904223u;
                float sign = (rng & 0x10000u) ? 1.f : -1.f;
                float amp  = expf(-2.3f * (float)t / (float)BW_DECOR_TAPS);  /* ~-20 dB across the span */
                c->dc_tamp[ch][t] = sign * amp;
                e2 += (double)amp * amp;
            }
            float norm = (float)(1.0 / sqrt(e2));
            for (uint32_t t = 0; t < BW_DECOR_TAPS; ++t) c->dc_tamp[ch][t] *= norm;
        }
    }
    c->para = (ParaBed*)calloc(voice_cap, sizeof(ParaBed));   /* parametric-bed state (parallel to voices) */
    for (int x = 0; x < 3; ++x)                               /* band-splitter crossovers at the engine rate */
        c->para_xa[x] = 1.f - expf(-6.2831853f * BW_PARA_XOVER[x] / (float)sample_rate);
    c->ldc_a = 1.f - expf(-6.2831853f * 250.f / (float)sample_rate);   /* loudness-comp shelf corner */
    if (!c->voices || !c->occ_handle || !c->occ_val || !c->occ_eq || !c->occ_dir || !c->play_pub || !c->aux ||
        !c->stream_scratch || !c->streams || !c->path_accum || !c->path_pub || !c->path_idx ||
        !c->gen || !c->inuse || !c->priority || !c->stealing || !c->freelist || !c->sounds || !c->sfreelist ||
        !c->dop_ring || !c->dc_bus || !c->dc_hist || !c->para || !c->ism_ring) {
        rt_destroy(c); return NULL;
    }
    const uint64_t eq_flat = eq_pack((float[3]){ 1.f, 1.f, 1.f });
    for (uint32_t i = 0; i < voice_cap; ++i) {   /* occlusion/directivity start clear (handle 0 = no publish) */
        atomic_store_explicit(&c->occ_val[i], 1.0f,    memory_order_relaxed);
        atomic_store_explicit(&c->occ_eq[i],  eq_flat, memory_order_relaxed);
        atomic_store_explicit(&c->occ_dir[i], 1.0f,    memory_order_relaxed);
    }
    for (uint32_t ch = 0; ch < BW_CHANNELS; ++ch)   /* output meter starts silent */
        atomic_store_explicit(&c->chan_peak[ch], 0.f, memory_order_relaxed);
    /* precompute the 3 EQ band prototypes from the runtime sample rate (so the EQ is correct at
     * 48/96/192k). fc's: low-shelf 800, peaking at the geometric centre, high-shelf 8000. */
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
    atomic_store_explicit(&c->master_gain, 1.f, memory_order_relaxed);
    c->master_g_cur = 1.f;
    for (int j = 0; j < BW_GROUPS; ++j) c->group_gain[j] = 1.f;        /* mix groups start at unity, unpaused */
    /* protection limiter: ON at -1 dBFS by default; ~1 ms attack / ~120 ms release one-poles */
    atomic_store_explicit(&c->lim_on, 1, memory_order_relaxed);
    atomic_store_explicit(&c->lim_ceiling, 0.891251f, memory_order_relaxed);   /* 10^(-1/20) */
    c->lim_gain  = 1.0f;
    c->lim_att_a = 1.0f - expf(-1.0f / (0.001f * (float)sample_rate));
    c->lim_rel_a = 1.0f - expf(-1.0f / (0.120f * (float)sample_rate));
    c->layout  = layout_default();
    /* default listener POSITION = the layout's nominal listening point (re-set when the real
     * layout arrives) — a pose-less client listens from the array centre, not the floor origin */
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

/* Replace the speaker layout. Control thread, call BEFORE bw_start (or while stopped) —
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
     * pushed after start overwrite this every frame; a pose-less client hears from the centre) */
    memcpy(c->lis.p_active,  c->layout.ref, sizeof c->lis.p_active);
    memcpy(c->lis.p_pending, c->layout.ref, sizeof c->lis.p_pending);
    memcpy(c->readback.p,    c->layout.ref, sizeof c->readback.p);
    build_bed_decode(c);                         /* re-derive the bed decode for the new geometry */
    c->rq_state = 0;                             /* new aligner starts flat: re-send the room-EQ targets */
    c->layout_gen++;                             /* the SPCAP cache self-invalidates on the next gains call */
    for (uint32_t i = 0; i < c->voice_cap; ++i)
        if (c->voices[i].active) c->voices[i].dirty = true;
}

/* Select the panner: 0 = DBAP (default, moving observer), 1 = SPCAP, 2 = VBAP (both fixed observer).
 * Atomic-release store, and the SPCAP/VBAP caches self-invalidate on listener/layout change, so this
 * is safe to call at runtime (live A/B) as well as before bw_start. */
void rt_set_panner(RtCore* c, int panner) {
    if (!c) return;
    atomic_store_explicit(&c->panner, (panner >= 0 && panner <= 2) ? panner : 0, memory_order_release);
}

/* Dual-band panning: 0 = off (power panning across the band), 1 = on (amplitude below BW_DUALBAND_FC,
 * power above). The low-band gains are computed every gain solve, so this is a live A/B atomic. */
void rt_set_dual_band(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->dual_band, on ? 1 : 0, memory_order_release);
}

/* Spread rendering: 0 = lobe reshape (default), 1 = MDAP virtual-source ring. Read per gain solve,
 * so it is a live A/B atomic like the panner; voices with spread 0 are unaffected either way. */
void rt_set_spread_mode(RtCore* c, int mode) {
    if (!c) return;
    atomic_store_explicit(&c->spread_mode, mode == 1 ? 1 : 0, memory_order_release);
}

/* Tracked room EQ (layouts with a room_eq_grid): default ON; off slews every section to flat, so
 * the toggle is a click-free live A/B. A no-op for layouts without a grid. */
void rt_set_room_eq_dyn(RtCore* c, int on) {
    if (!c) return;
    atomic_store_explicit(&c->room_eq_dyn, on ? 1 : 0, memory_order_release);
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
    if (lead_s < 0.f) lead_s = 0.f; else if (lead_s > 0.2f) lead_s = 0.2f;   /* a lead past ~200 ms is overshoot, not latency hiding */
    atomic_store_explicit(&c->pred_lead, lead_s, memory_order_relaxed);
}

/* Master gain: one ramped scalar over the whole mix (pre-align). Live-safe atomic; the render ramps
 * toward it across the block, so a slider drag never zippers. */
void rt_set_master_gain(RtCore* c, float linear) {
    if (!c) return;
    if (linear < 0.f) linear = 0.f;
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

/* Near-listener widening: floor every source's spread at 1 - dist/radius. 0 disables (default).
 * Live-safe atomic; the gains it changes ramp like any solve. */
void rt_set_near_spread(RtCore* c, float radius_m) {
    if (!c) return;
    if (radius_m < 0.f) radius_m = 0.f; else if (radius_m > 10.f) radius_m = 10.f;
    atomic_store_explicit(&c->near_spread, radius_m, memory_order_relaxed);
}

/* Per-voice equal-loudness distance compensation (enqueue-only; the mixer ramps the shelf). */
void rt_source_set_loudness_comp(RtCore* c, uint32_t h, bool on) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_LDC, .handle = h };
    cmd.u.ldc.on = on ? 1 : 0;
    cmd_push(&c->cmds, &cmd);
}

/* Extra (compromise) listener positions — multi-listener panning. Latest-wins, commit-gated like
 * the pose; n = 0 restores single-listener panning. Control thread, enqueue-only. */
void rt_set_extra_listeners(RtCore* c, const float* xyz, uint32_t n) {
    if (!c) return;
    Cmd cmd = { .type = CMD_SET_EXTRA_LIS, .handle = 0 };
    if (n > BW_EXTRA_LIS) n = BW_EXTRA_LIS;
    memset(cmd.u.exlis.p, 0, sizeof cmd.u.exlis.p);
    if (xyz) for (uint32_t j = 0; j < n; ++j) memcpy(cmd.u.exlis.p[j], &xyz[j * 3], sizeof(float) * 3);
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
 * the audio thread reads, so call BEFORE bw_start (or while stopped), like rt_set_layout. */
void rt_set_bed_decoder(RtCore* c, int decoder) {
    if (!c) return;
    c->bed_decoder = (decoder == 1) ? 1 : 0;
    build_bed_decode(c);
}

void rt_set_tracker(RtCore* c, const PoseSlot* slot) {
    if (c) c->tracker = slot;               /* audio thread reads it; set while stopped */
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
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return;
    if (level < 0.f) level = 0.f; if (level > 1.f) level = 1.f;
    if (dir   < 0.f) dir   = 0.f; if (dir   > 1.f) dir   = 1.f;
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
 * publish was for a different occupant. Race-free: reads only the atomic publish slot. */
float rt_get_occlusion(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    return (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
         ? atomic_load_explicit(&c->occ_val[idx], memory_order_relaxed) : 1.f;
}

/* Read back the published directivity gain (1 = on-axis/omni) — for HUD/diagnostics. */
float rt_get_directivity(RtCore* c, uint32_t handle) {
    if (!c) return 1.f;
    uint16_t idx = BW_H_IDX(handle);
    if (idx >= c->voice_cap) return 1.f;
    return (atomic_load_explicit(&c->occ_handle[idx], memory_order_acquire) == handle)
         ? atomic_load_explicit(&c->occ_dir[idx], memory_order_relaxed) : 1.f;
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
    free(c->inuse);
    free(c->gen);
    free(c->dop_ring);
    free(c->ism_ring);
    free(c->dc_bus);
    free(c->dc_hist);
    free(c->para);
    stream_set_destroy(c->streams);     /* stops the streaming thread, releases every open stream + ring */
    free(c->stream_scratch);
    free(c->path_accum);
    free(c->path_pub);
    free((void*)c->path_idx);
    free(c->aux);
    free((void*)c->occ_dir);            /* cast drops the _Atomic qualifier for free() */
    free((void*)c->play_pub);
    free((void*)c->occ_eq);
    free((void*)c->occ_val);
    free((void*)c->occ_handle);
    free(c->voices);
    free(c);
}
