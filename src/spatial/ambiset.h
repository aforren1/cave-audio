/*
 * ambiset.h - the ambisonic CHANNEL SETS a bed can carry, and that an imported bed decoder
 * (layout `bed_decode`, docs/layout-schema.md) can be designed for. Header-only, no includes past
 * <stdint.h>/<string.h>, so layout.h, sound.c, rt.c and the tools can all share it without a cycle
 * (layout.h cannot include ambisonics.h: that header includes layout.h).
 *
 * A FULL set is every spherical harmonic up to the order (4/9/16 ACN channels). A MIXED set is the
 * #H#V shape the .amb (FuMa) format identifies by channel count (Dobson 2001, "The AMB Ambisonic
 * File Format": a file's channels are WXYZ,RSTUV,KLMNOPQ with the unused ones omitted): the
 * horizontal harmonics (|m| = l) to the H order plus the full set to the V order. Only those six
 * mixed sets exist here, because only they have a channel-count convention; AmbiX has none.
 *
 * The engine stores a mixed bed in the (H+1)^2 ACN slots of its highest order with zeros in the
 * absent channels, so every bed is still 4, 9 or 16 channels downstream and the set travels beside
 * it as a tag (SoundData.chset). The masks below are ACN bit masks (bit k = ACN k), the same
 * convention as AmbDec's /dec/chan_mask, which is what lets tools/decoder/ambdec_to_json.py name a
 * decoder's set from its mask.
 */
#ifndef BWA_AMBISET_H
#define BWA_AMBISET_H

#include <stdint.h>
#include <string.h>

enum {
    AMBI_SET_FULL = 0,   /* every channel to the bed's order (4/9/16) */
    AMBI_SET_1H,         /* 3 ch:  W X Y                    */
    AMBI_SET_2H,         /* 5 ch:  W X Y U V                */
    AMBI_SET_2H1V,       /* 6 ch:  W X Y Z U V              */
    AMBI_SET_3H,         /* 7 ch:  W X Y U V P Q            */
    AMBI_SET_3H1V,       /* 8 ch:  W X Y Z U V P Q          */
    AMBI_SET_3H2V,       /* 11 ch: W X Y Z R S T U V P Q    */
    AMBI_SET_COUNT
};

/* Display name of each set. A FULL set has no name of its own: a layout decoder and
 * bwa_bed_gains_layout spell it with its order ("full1", "full2", "full3"; ambi_chan_parse). */
static const char* const ambi_set_names[AMBI_SET_COUNT] = { "full", "1H", "2H", "2H1V", "3H", "3H1V", "3H2V" };

/* ACN bit mask of each MIXED set. FULL has no fixed mask (it depends on the order): 0 here. */
static const uint16_t ambi_set_masks[AMBI_SET_COUNT] = {
    0x0000u,   /* full */
    0x000Bu,   /* 1H:   ACN 0 1 3 */
    0x011Bu,   /* 2H:   ACN 0 1 3 4 8 */
    0x011Fu,   /* 2H1V: ACN 0 1 2 3 4 8 */
    0x831Bu,   /* 3H:   ACN 0 1 3 4 8 9 15 */
    0x831Fu,   /* 3H1V: ACN 0 1 2 3 4 8 9 15 */
    0x83FFu,   /* 3H2V: ACN 0..8 9 15 */
};

/* Highest order of each MIXED set (FULL: 0, the bed's own order applies). */
static const uint8_t ambi_set_orders[AMBI_SET_COUNT] = { 0, 1, 2, 2, 3, 3, 3 };

/* A `channels` spelling -> (set, order): "full1".."full3" is the FULL set of that order, a mixed
 * name is its set at its own highest order. 1 on success, 0 when unknown. Case-sensitive, like the
 * rest of the layout schema. A bare "full" is NOT accepted: a full-order decoder is designed for one
 * order, and the order is what picks it (layout.h layout_bed_decoder_for). */
static inline int ambi_chan_parse(const char* s, int* set, int* order) {
    if (!s) return 0;
    if (strncmp(s, "full", 4) == 0 && s[4] >= '1' && s[4] <= '3' && s[5] == 0) {
        *set = AMBI_SET_FULL; *order = s[4] - '0'; return 1;
    }
    for (int i = 1; i < AMBI_SET_COUNT; ++i)
        if (strcmp(s, ambi_set_names[i]) == 0) { *set = i; *order = ambi_set_orders[i]; return 1; }
    return 0;
}

/* (set, order) -> its `channels` spelling ("full2", "3H1V"); "?" when out of range. */
static inline const char* ambi_chan_name(int set, int order) {
    static const char* const full[3] = { "full1", "full2", "full3" };
    if (set == AMBI_SET_FULL) return (order >= 1 && order <= 3) ? full[order - 1] : "?";
    return (set > 0 && set < AMBI_SET_COUNT) ? ambi_set_names[set] : "?";
}

/* The ACN mask a set actually occupies inside a bed of `order` (full = every channel to it). */
static inline uint16_t ambi_set_mask_for(int set, int order) {
    if (set > AMBI_SET_FULL && set < AMBI_SET_COUNT) return ambi_set_masks[set];
    const int n = (order + 1) * (order + 1);
    return (uint16_t)(n >= 16 ? 0xFFFFu : ((1u << n) - 1u));
}

#endif /* BWA_AMBISET_H */
