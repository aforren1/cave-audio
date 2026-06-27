/*
 * align.h — the output alignment stage: per-speaker gain trim + delay line, to align
 * arrival times from unequally-distant CAVE speakers (docs/spatialization.md). align_create
 * allocates on the control thread; align_process runs on the audio thread (no alloc/lock).
 */
#ifndef BW_ALIGN_H
#define BW_ALIGN_H

#include "layout.h"

#include <stdint.h>

typedef struct Aligner Aligner;

Aligner* align_create(uint32_t channels, const Layout* L);   /* NULL on alloc failure */
void     align_destroy(Aligner* a);
void     align_process(Aligner* a, float* bus, uint32_t nframes);  /* in place; planar bus */

#endif /* BW_ALIGN_H */
