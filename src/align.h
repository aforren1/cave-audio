/*
 * align.h — the output alignment stage: per-speaker gain trim + delay line, to align
 * arrival times from unequally-distant CAVE speakers (docs/spatialization.md). align_create
 * allocates on the control thread; align_process runs on the audio thread (no alloc/lock).
 */
#ifndef BWA_ALIGN_H
#define BWA_ALIGN_H

#include "layout.h"

#include <stdint.h>

typedef struct Aligner Aligner;

/* sample_rate derives the room_eq biquad coefficients (the layout stores rate-independent fc/Q). */
Aligner* align_create(uint32_t channels, const Layout* L, uint32_t sample_rate);   /* NULL on alloc failure */
void     align_destroy(Aligner* a);
void     align_process(Aligner* a, float* bus, uint32_t nframes);  /* in place; planar bus */

/* Tracked room EQ (layouts with a room_eq_grid): set the section-gain targets (dB, <= 0) the next
 * align_process blocks slew toward — rt.c interpolates them from the grid at the live listener
 * position. gain_db is [channel][section] over the grid's fc/q ladder. AUDIO thread (the same thread
 * as align_process — plain stores, no atomics needed); no-op for a gridless layout. */
void     align_room_eq_targets(Aligner* a, const float (*gain_db)[BWA_ROOM_EQ_MAX]);

#endif /* BWA_ALIGN_H */
