/*
 * bwa_web.c - the web binding's C glue, and deliberately the only C it has.
 *
 * WHY IT EXISTS. Everything in the public ABI that takes scalars crosses to JS for free through
 * the generated raw layer (src/abi.js, src/raw.js). What does NOT cross for free is a STRUCT: JS
 * has no offsetof, so a binding that built a `bwa_desc` by hand in the wasm heap would be
 * hard-coding field offsets a header change could move without a word. Three calls take or fill
 * one, so three functions here take the fields apart and let the C compiler do the arithmetic.
 * Nothing else belongs in this file: a scalar call added to bw_audio.h must reach JS through the
 * generator, not through a hand-written wrapper here.
 *
 * The 64-bit health counters come back as DOUBLES on purpose. emcc's default is -sWASM_BIGINT, so
 * a uint64_t return is a BigInt in JS, and six BigInts per health poll is allocation on a path a
 * page polls every frame. A double is exact to 2^53, which at 48 kHz is 5800 years of blocks.
 *
 * This file is compiled into the MODULE, not into libbw_audio.a: EMSCRIPTEN_KEEPALIVE only keeps
 * an object the link already pulled in, and nothing references an archive member that only JS
 * calls. bindings/web/CMakeLists.txt lists it as a source of the bwa_web target for that reason.
 */
#include "bw_audio.h"

#include <emscripten/emscripten.h>
#include <string.h>

/* bwa_desc, field by field. The order is the header's, so a reader can diff the two. `reserved`
 * and the fields a browser cannot use (embree: there is no Embree in wasm) are left zeroed by the
 * memset rather than given an argument nobody could pass usefully. */
EMSCRIPTEN_KEEPALIVE
bwa_engine* bwaw_create(int profile, uint32_t sample_rate, uint32_t block_size, int sink,
                        const char* device, uint32_t sink_flags, const char* layout_path,
                        const char* hrtf_path, int bed_decoder, int enable_pathing) {
    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.profile        = (bwa_profile)profile;
    d.layout_path    = (layout_path && *layout_path) ? layout_path : NULL;
    d.hrtf_path      = (hrtf_path && *hrtf_path) ? hrtf_path : NULL;
    d.sample_rate    = sample_rate;
    d.block_size     = block_size;
    d.sink           = (bwa_sink_type)sink;
    d.device         = (device && *device) ? device : NULL;
    d.bed_decoder    = (bwa_bed_decoder)bed_decoder;
    d.enable_pathing = enable_pathing != 0;
    d.sink_flags     = sink_flags;
    return bwa_create(&d);
}

/* bwa_health into eight doubles, in the struct's own order:
 * blocks, xruns, dropped_frames, driver_resyncs, late_blocks, stream_starves, peak_load,
 * device_lost. Returns bwa_get_health's own answer, which is "do these numbers MEAN anything" -
 * false on the manual sink and on a backend that cannot observe a dropout, and the web sink is
 * one of those (docs/backends.md rule 4, and worklet_sink.c says why). */
EMSCRIPTEN_KEEPALIVE
int bwaw_health(bwa_engine* e, double* out8) {
    bwa_health h;
    memset(&h, 0, sizeof h);
    const bool ok = bwa_get_health(e, &h);
    if (!out8) return ok ? 1 : 0;
    out8[0] = (double)h.blocks;
    out8[1] = (double)h.xruns;
    out8[2] = (double)h.dropped_frames;
    out8[3] = (double)h.driver_resyncs;
    out8[4] = (double)h.late_blocks;
    out8[5] = (double)h.stream_starves;
    out8[6] = (double)h.peak_load;
    out8[7] = (double)h.device_lost;
    return ok ? 1 : 0;
}

/* The engine's own ABI version, so the JS side can check it before it has an engine. bwa_get_version
 * already does that and the raw layer calls it; this exists only to give the glue a symbol the
 * module test can resolve without creating anything. */
EMSCRIPTEN_KEEPALIVE
uint32_t bwaw_version(void) { return bwa_get_version(); }
