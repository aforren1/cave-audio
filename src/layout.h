/*
 * layout.h — the surveyed speaker geometry + DBAP/alignment parameters, loaded from
 * cave_layout.json (see docs/layout-schema.md). Control thread / load time only; the
 * audio thread reads a const Layout owned by rt.c. Not part of the public ABI.
 */
#ifndef BWA_LAYOUT_H
#define BWA_LAYOUT_H

#include "sink.h"          /* BWA_CHANNELS */

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
    float    fc[BWA_CHANNELS][BWA_ROOM_EQ_MAX];          /* ladder: mode centre frequencies (Hz) */
    float    q [BWA_CHANNELS][BWA_ROOM_EQ_MAX];          /* ladder: mode Qs */
    float    gain_db[BWA_RQ_GRID_MAX][BWA_CHANNELS][BWA_ROOM_EQ_MAX];  /* per-position cut depths (<= 0) */
} RoomEqGrid;

typedef struct {
    Speaker  speakers[BWA_CHANNELS];
    uint32_t count;                 /* == BWA_CHANNELS once validated */
    /* nominal listening point = the array CENTROID, computed at load. The world-locked decodes
     * (ambisonic/reflection/pathing beds, the monitor's virtual-speaker encode) take their speaker
     * DIRECTIONS from here, and it is the engine's default listener pose — so the room origin can
     * sit anywhere (canonically on the floor, Motive-style) without skewing a decode. */
    float    ref[3];
    float    rolloff_r;             /* DBAP spatial blur (meters) */
    /* distance attenuation, source -> listener (inverse model):
     *   atten = clamp( (ref / max(d, ref))^rolloff , min_lin, 1 ) */
    float    atten_ref_m;
    float    atten_rolloff;
    float    atten_min_lin;
    uint32_t max_delay_samples;     /* max over speakers; sizes the alignment delay lines */
    RoomEqGrid rq_grid;             /* tracked room EQ grid (npos = 0 when the layout has none) */
} Layout;

/* A sane default: a 3 m-cube 3x3x3 boundary grid (minus centre), FLOOR-origin — x/z at
 * +/-1.5 m around the room centre, y from 0 (floor) to 3 m, ref (ear point) at (0,1.5,0).
 * Unity trim, no delay. Lets the engine run with no layout file (binaural / desk dev / tests). */
Layout layout_default(void);

/* Load + validate cave_layout.json. `sample_rate` converts delay_ms -> samples and the
 * gains from dB. Fails (false + message in `err`) on a missing/unparseable file, a wrong
 * speaker count, or out-of-range values. */
bool layout_load(const char* path, uint32_t sample_rate, Layout* out, char* err, size_t errcap);

/* (Re)compute `ref` from the speaker positions — for callers that build a Layout by hand. */
void layout_compute_ref(Layout* L);

#endif /* BWA_LAYOUT_H */
