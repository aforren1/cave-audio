/*
 * layout.h — the surveyed speaker geometry + DBAP/alignment parameters, loaded from
 * cave_layout.json (see docs/layout-schema.md). Control thread / load time only; the
 * audio thread reads a const Layout owned by rt.c. Not part of the public ABI.
 */
#ifndef BW_LAYOUT_H
#define BW_LAYOUT_H

#include "sink.h"          /* BW_CHANNELS */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BW_EQ_TAPS     512  /* max per-speaker correction-FIR length */
#define BW_ROOM_EQ_MAX 8    /* max per-speaker LF modal-cut sections (static-listener room correction) */

/* One parametric peaking section (RBJ), rate-independent in the file; align.c derives the biquad
 * coefficients at the engine rate. Cut-only by schema (gain_db <= 0). */
typedef struct { float fc, gain_db, q; } RoomEqSection;

typedef struct {
    float    pos[3];        /* room space, right-handed, meters */
    float    gain_lin;      /* per-speaker level trim (linear) */
    uint32_t delay_samples; /* per-speaker delay for arrival-time alignment */
    uint16_t eq_len;        /* per-speaker correction-FIR length (0 = none) */
    float    eq[BW_EQ_TAPS];/* minimum-phase speaker-correction taps (gated direct-sound inverse) */
    uint8_t  room_eq_count; /* LF modal cuts — STATIC-listener room correction only (docs/calibration.md) */
    RoomEqSection room_eq[BW_ROOM_EQ_MAX];
} Speaker;

typedef struct {
    Speaker  speakers[BW_CHANNELS];
    uint32_t count;                 /* == BW_CHANNELS once validated */
    float    rolloff_r;             /* DBAP spatial blur (meters) */
    /* distance attenuation, source -> listener (inverse model):
     *   atten = clamp( (ref / max(d, ref))^rolloff , min_lin, 1 ) */
    float    atten_ref_m;
    float    atten_rolloff;
    float    atten_min_lin;
    uint32_t max_delay_samples;     /* max over speakers; sizes the alignment delay lines */
} Layout;

/* A sane default: the 3x3x3 boundary grid (minus centre) at +/-1.5 m, unity trim, no
 * delay. Lets the engine run with no layout file (binaural / desk dev / tests). */
Layout layout_default(void);

/* Load + validate cave_layout.json. `sample_rate` converts delay_ms -> samples and the
 * gains from dB. Fails (false + message in `err`) on a missing/unparseable file, a wrong
 * speaker count, or out-of-range values. */
bool layout_load(const char* path, uint32_t sample_rate, Layout* out, char* err, size_t errcap);

#endif /* BW_LAYOUT_H */
