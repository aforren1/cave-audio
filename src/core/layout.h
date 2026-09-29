/*
 * layout.h — the surveyed speaker geometry + DBAP/alignment parameters, loaded from
 * cave_layout.json (see docs/layout-schema.md). Control thread / load time only; the
 * audio thread reads a const Layout owned by rt.c. Not part of the public ABI.
 */
#ifndef BWA_LAYOUT_H
#define BWA_LAYOUT_H

#include "sink/sink.h"          /* BWA_CHANNELS */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BWA_EQ_TAPS     512  /* max per-speaker correction-FIR length */
#define BWA_ROOM_EQ_MAX 8    /* max per-speaker LF modal-cut sections (static-listener room correction) */
#define BWA_RQ_GRID_MAX 16   /* max tracked-room-EQ measurement positions (room_eq_grid) */

/* One parametric peaking section (RBJ), rate-independent in the file; align.c derives the biquad
 * coefficients at the engine rate. Cut-only by schema (gain_db <= 0). */
typedef struct { float fc, gain_db, q; } RoomEqSection;

typedef struct {
    float    pos[3];        /* room space, right-handed, meters */
    float    aim[3];        /* unit vector along the acoustic axis (the `aim` field, else toward ref).
                             * Only the directivity model reads it (layout_speaker_off_axis_deg). */
    float    gain_lin;      /* per-speaker level trim (linear) */
    uint32_t delay_samples; /* per-speaker delay for arrival-time alignment */
    uint16_t eq_len;        /* per-speaker correction-FIR length (0 = none) */
    float    eq[BWA_EQ_TAPS];/* minimum-phase speaker-correction taps (gated direct-sound inverse) */
    uint8_t  room_eq_count; /* LF modal cuts — STATIC-listener room correction only (docs/calibration.md) */
    RoomEqSection room_eq[BWA_ROOM_EQ_MAX];
} Speaker;

/* Tracked room EQ (docs/calibration.md): the same LF modal cuts as room_eq, but measured at a GRID
 * of listener positions (bwa_calibrate --room-eq-grid) so they survive a MOVING listener. The room's
 * mode frequencies don't move with the listener — only how strongly each mode reads at a position —
 * so per speaker there is ONE shared fc/q ladder, and per grid position that ladder's cut depths.
 * rt.c interpolates the depths at the live listener position each block (inverse-distance weights)
 * and align.c slews its biquads toward them. Mutually exclusive with per-speaker room_eq. */
typedef struct {
    uint8_t  npos;                                     /* measurement positions (0 = no grid) */
    float    pos[BWA_RQ_GRID_MAX][3];                   /* mic positions, room meters */
    uint8_t  nsec[BWA_CHANNELS];                        /* ladder size per speaker */
    float    fc[BWA_CHANNELS][BWA_ROOM_EQ_MAX];          /* ladder: mode center frequencies (Hz) */
    float    q [BWA_CHANNELS][BWA_ROOM_EQ_MAX];          /* ladder: mode Qs */
    float    gain_db[BWA_RQ_GRID_MAX][BWA_CHANNELS][BWA_ROOM_EQ_MAX];  /* per-position cut depths (<= 0) */
} RoomEqGrid;

/* Speaker directivity model (docs/layout-schema.md `directivity`, written by
 * tools/directivity/clf_to_json.py from the vendor's CLF simulation file): the off-axis loss of ONE
 * speaker model, in dB relative to on-axis, per third-octave band and per degree off the acoustic
 * axis, assumed the same for every speaker in the layout and axisymmetric about its aim. Three
 * consumers: bwa_calibrate corrects a trim measured off a speaker's axis back to what the listening
 * point hears (calib_solve_corr); rt.c's tracked directivity compensation re-references the loss
 * from the listening point onto the live listener each block (directivity_track), as a broadband
 * gain plus a high shelf at split_hz; and the array-sim audition (arraysim.c) plays each virtual
 * speaker's loss toward the listener through a graphic EQ fitted to the full table. lo_db/hi_db
 * are the two half-band curves the comp uses, derived at load (directivity_derive) so the comp
 * never touches the table on the audio thread. */
#define BWA_DIR_MAX_BANDS  32
#define BWA_DIR_MAX_ANGLES 37
typedef struct {
    uint8_t nband;                            /* 0 = no model (every consumer bypasses) */
    uint8_t nang;                             /* 2..37, ang_deg ascending from 0 */
    float   band_hz[BWA_DIR_MAX_BANDS];       /* ascending band centers */
    float   ang_deg[BWA_DIR_MAX_ANGLES];      /* [0] = 0 (on axis) .. last <= 180 */
    float   loss_db[BWA_DIR_MAX_BANDS][BWA_DIR_MAX_ANGLES];   /* <= 0 in practice; [band][angle] */
    float   split_hz;                         /* the two-band split the runtime comp shelves at */
    float   lo_db[BWA_DIR_MAX_ANGLES];        /* derived: power-mean loss over bands BELOW split_hz */
    float   hi_db[BWA_DIR_MAX_ANGLES];        /* derived: power-mean loss over bands AT/ABOVE it */
} Directivity;

/* NEVER A STACK LOCAL (see CLAUDE.md, Traps). At BWA_CHANNELS = 64 a Layout is ~176 KB, almost all
 * of it the 512-tap FIR each Speaker embeds, and a binding may call in from a 512 KB thread. Keep it
 * a plain value type, but give it heap or static residency: a member of a heap struct (RtCore,
 * bwa_engine), a calloc, or a static in non-reentrant code. That is also why layout_default fills
 * in place rather than returning by value: a by-value return is a hidden stack temporary. */
typedef struct {
    Speaker  speakers[BWA_CHANNELS];
    uint32_t count;                 /* 4..BWA_CHANNELS once validated (the engine's channel count) */
    /* nominal listening point = the file's `listening_point_m` when it declares one, else the array
     * CENTROID, computed at load. The world-locked decodes
     * (ambisonic/reflection/pathing beds, the monitor's virtual-speaker encode) take their speaker
     * DIRECTIONS from here, and it is the engine's default listener pose — so the room origin can
     * sit anywhere (canonically on the floor, Motive-style) without skewing a decode. */
    float    ref[3];
    float    rolloff_r;             /* DBAP spatial blur (meters) */
    /* SPCAP defaults (dimensionless exponents, spcap.c). DERIVED, never parsed: the layout file is a
     * geometry/calibration artifact and nothing measures a lobe width. focus = lobe sharpness
     * ((1+cos)/2)^focus, derived from the array's speaker spacing; density = the
     * placement-correction kernel exponent, a plain constant (no geometry link). bwa_set_spcap_focus
     * overrides both at runtime like the other live knobs; these are what it reverts to. */
    float    spcap_focus;
    float    spcap_density;
    /* distance attenuation, source -> listener (inverse model):
     *   atten = clamp( (ref / max(d, ref))^rolloff , min_lin, 1 ) */
    float    atten_ref_m;
    float    atten_rolloff;
    float    atten_min_lin;
    uint32_t max_delay_samples;     /* max over speakers; sizes the alignment delay lines */
    RoomEqGrid rq_grid;             /* tracked room EQ grid (npos = 0 when the layout has none) */
    Directivity dir;                /* speaker directivity model (nband = 0 when the layout has none) */
} Layout;

/* A sane default: a 3 m-cube 3x3x3 boundary grid (minus center), FLOOR-origin — x/z at
 * +/-1.5 m around the room center, y from 0 (floor) to 3 m, ref (ear point) at (0,1.5,0).
 * Unity trim, no delay. Lets the engine run with no layout file (binaural / desk dev / tests). */
void layout_default(Layout* out);

/* Load + validate cave_layout.json. `sample_rate` converts delay_ms -> samples and the
 * gains from dB. Fails (false + message in `err`) on a missing/unparseable file, a wrong
 * speaker count, or out-of-range values. */
bool layout_load(const char* path, uint32_t sample_rate, Layout* out, char* err, size_t errcap);

/* (Re)compute `ref` from the speaker positions — for callers that build a Layout by hand. */
void layout_compute_ref(Layout* L);

/* Point every speaker's aim at `ref` (the schema's default when a record has no `aim`). Needs
 * `count` + `ref` set. A speaker sitting exactly on ref keeps (0,0,1). For hand-built layouts. */
void layout_default_aims(Layout* L);

/* Degrees between speaker k's acoustic axis and the direction from it to `p` (0 = p is on axis,
 * 180 = directly behind). Pure, alloc-free, audio-thread safe; 0 when p sits on the speaker. */
float layout_speaker_off_axis_deg(const Layout* L, uint32_t k, const float p[3]);
/* The same angle from a bare position + unit aim, for a consumer that copied them out of the
 * Layout (the array-sim room stage, arraysim.c). The one implementation both go through. */
float directivity_off_axis_deg(const float pos[3], const float aim[3], const float p[3]);

/* Directivity model helpers (see the struct). All pure and alloc-free. */
/* Fill lo_db/hi_db from band_hz/loss_db/split_hz. Call after building a Directivity by hand;
 * layout_load does it. No bands below the split reads as omni there (lo = 0); none above it makes
 * the shelf 0 (hi = lo). */
void  directivity_derive(Directivity* d);
/* The two half-band losses (dB) at `angle_deg` off axis, linear in angle over the table and
 * clamped at its ends. Both 0 when the model is empty. Audio thread OK (rt.c calls it per block). */
void  directivity_lookup(const Directivity* d, float angle_deg, float* lo_db, float* hi_db);
/* The AMPLITUDE loss (linear, <= 1 typically) averaged the way measure.c's level is: uniformly in
 * frequency over [f_lo, f_hi] Hz (log-interpolated between band centers, clamped outside them).
 * Control thread (bwa_calibrate). 1 when the model is empty. */
float directivity_loss_lin(const Directivity* d, float angle_deg, float f_lo, float f_hi);
/* The loss (dB) at one angle and one frequency: linear in angle over the table, linear in log f
 * between band centers, clamped at the table's ends. 0 when the model is empty. Pure. */
float directivity_loss_db_at(const Directivity* d, float angle_deg, float f_hz);

/* The array's angular scale: the mean nearest-neighbor angular separation (RADIANS) of the speaker
 * directions seen from `ref`. 37.5 deg on the default 26-speaker cube grid. Returns 0 on a
 * degenerate survey (fewer than 2 speakers). Needs `count` + `ref` set — call after
 * layout_compute_ref. O(N^2) but pure and alloc-free, and hole.c calls it from the AUDIO thread on a
 * layout-generation change (not per block; the knee is cached with the direction set). So it is not
 * "load-time only" any more: it must stay allocation-free, lock-free and logging-free, or hole_block
 * breaks invariant 1. Keep it pure.
 *
 * Two features scale themselves off it, which is why it is exported rather than inlined into either:
 * SPCAP's default lobe width (below) and the hole-aware spread floor's knee (hole.h). */
float layout_mean_speaker_spacing(const Layout* L);

/* SPCAP's default lobe sharpness for THIS array: the mean nearest-neighbor angular separation delta
 * above, then the exponent that puts the lobe ((1+cos)/2)^n 6 dB down in energy at delta —
 * n = ln(0.25)/ln((1+cos delta)/2). A wide array (few speakers, far apart) wants a broad lobe, a
 * dense one a tight one; the hard-coded 12 only ever fit the 26-speaker cave. Clamped to 1..64, and
 * falls back to 12 on a degenerate survey (< 2 speakers, delta ~ 0 or ~ pi). Same preconditions and
 * cost as layout_mean_speaker_spacing. */
float layout_derive_spcap_focus(const Layout* L);

#define BWA_SPCAP_DENSITY_DEFAULT 2.0f   /* placement-correction density exponent (no geometry link) */

/* The distance-attenuation formula with arbitrary parameters — the layout curve, the per-source
 * override, and loudness comp's tracker all share it: clamp((ref/max(d,ref))^rolloff, min, 1).
 * ref <= 0 = attenuation off (1 everywhere); rolloff 0 = constant 1 (the formula's own limit). The
 * per-block gain solve (dbap/spcap/vbap + rt.c) all call this — one source of truth for the curve. */
static inline float atten_curve(float d, float ref, float rolloff, float min_lin) {
    if (ref <= 0.f) return 1.f;
    float dd = d > ref ? d : ref;
    float a = powf(ref / dd, rolloff);
    if (a < min_lin) a = min_lin;
    return a > 1.f ? 1.f : a;
}

/* Unit direction from `from` to `to` (out = normalize(to - from)); degenerate (|to-from| <= 1e-6)
 * falls back to (0,0,1). Reciprocal-multiply, 1e-6 guard: shared by the sites whose per-speaker
 * normalization is op-for-op this (vbap.c, epad.c). Sites with a DIFFERENT degenerate fallback or a
 * division form (spcap 0,0,0; allrad 1,0,0; steam_decode 0,0,-1 + division; rt.c's division-form
 * bed_pref loop) keep their own inline — merging would change their numerics. */
static inline void unit_dir(const float from[3], const float to[3], float out[3]) {
    float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len > 1e-6f) { float inv = 1.f / len; out[0] = dx * inv; out[1] = dy * inv; out[2] = dz * inv; }
    else             { out[0] = 0.f; out[1] = 0.f; out[2] = 1.f; }
}

/* Panner cache-invalidation predicate shared by spcap.c and vbap.c: stale when never built, the
 * layout generation changed, or the listener moved more than 1e-4 m on any axis. */
static inline int panner_cache_stale(int valid, uint32_t cached_gen, const float cached_lis[3],
                                     uint32_t gen, const float lis[3]) {
    return !valid || cached_gen != gen ||
           fabsf(cached_lis[0] - lis[0]) > 1e-4f ||
           fabsf(cached_lis[1] - lis[1]) > 1e-4f ||
           fabsf(cached_lis[2] - lis[2]) > 1e-4f;
}

#endif /* BWA_LAYOUT_H */
