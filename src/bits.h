/*
 * bits.h — tiny bit-twiddling utilities shared across the engine and the offline DSP. Header-only
 * (static inline) so it links into any translation unit with no separate object and no layering
 * pull-in (only <stdint.h>). Not part of the public ABI.
 */
#ifndef BWA_BITS_H
#define BWA_BITS_H

#include <stdint.h>

/* Smallest power of two >= x (round up). x == 0 or 1 -> 1; an already-power-of-two x -> x. This is
 * the `p = 1; while (p < x) p <<= 1` idiom the ring/delay-line sizing all used inline. */
static inline uint32_t bwa_pow2_ge(uint32_t x) { uint32_t p = 1; while (p < x) p <<= 1; return p; }

#endif /* BWA_BITS_H */
