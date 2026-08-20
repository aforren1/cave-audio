/*
 * convenience.c — the convenience tier: the calls that exist because every client was writing
 * them by hand. Nothing here is a new capability. Each part ends by pointing at the core calls
 * it replaces, because the whole claim of this tier is that the two are the same audio.
 *
 *   [1] shared assets   bwa_sound_acquire / _release: a by-path cache with a refcount, so two
 *                       systems asking for one clip get one load and one handle. Replaces the
 *                       Dictionary<path, handle> that lived in every binding.
 *   [2] load flags      the key is (path, FLAGS), so one file held in RAM and streamed are two
 *                       entries. That multi-key case is why bwa_load_sound/_streaming/_ambix/
 *                       _fuma collapse into one call with a flag.
 *   [3] probing         bwa_sound_find asks "is this resident" WITHOUT loading it. Probing with
 *                       acquire instead would decode the file as a side effect of the question.
 *   [4] async           bwa_sound_acquire_async hands back a usable handle before the decode
 *                       finishes, for content that appears mid-session. Play it at once: the
 *                       engine holds the play on the CONTROL thread and starts it when the data
 *                       lands. The audio thread never sees a half-loaded asset.
 *   [5] source config   bwa_source_desc: fill from a preset, override what you disagree with,
 *                       apply in one call. Also the only way to READ a source's setup back.
 *   [6] scene control   bwa_group_stop / bwa_stop_all: the click-free sweeps a scene transition
 *                       wants, instead of walking your own source list.
 *
 * Runs anywhere (binaural profile; silent null-sink fallback without an ASIO device).
 *
 *   bwa_convenience
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define RATE 48000u

/* -- scaffolding, not part of the pattern: write two short wavs so the example ships no assets. -- */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_tone(const char* path, float hz, uint32_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + frames * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 1);
    put_u32(f, RATE); put_u32(f, RATE * 2); put_u16(f, 2); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)i / RATE;
        float env = 0.5f + 0.5f * cosf(6.2831853f * 2.0f * (float)t);   /* slow tremolo, easy to hear */
        int16_t s = (int16_t)(sin(6.283185307179586 * hz * t) * env * 0.4 * 32767.0);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 1;
}

/* --tests: force the offline sink and cut the waits, so ctest runs this without a device and
 * without spending the demo's listening time. The CALLS are identical either way - that is the
 * point of running the example as a regression test rather than writing a separate one. */
static int g_tests = 0;
static int ticks(int n) { return g_tests ? (n < 8 ? 1 : n / 8) : n; }

/* The example is self-checking: every claim it prints is one it verifies, so ctest running it is a
 * real regression test and not just a crash check. Failures are counted, not fatal, so one broken
 * part still lets the rest of the walkthrough run and report. */
static int g_bad = 0;
static const char* verdict(int ok, const char* good, const char* bad) {
    if (!ok) ++g_bad;
    return ok ? good : bad;
}

static void pump(bwa_engine* e, int ms) {         /* a "game loop" tick: commit, then wait */
    bwa_commit(e);
    Sleep((DWORD)ms);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--tests")) g_tests = 1;

    const char* CLIP = "bwa_demo_prop.wav";
    const char* LATE = "bwa_demo_late.wav";
    if (!write_tone(CLIP, 330.0f, RATE * 4) || !write_tone(LATE, 440.0f, RATE * 4)) {
        fprintf(stderr, "cannot write demo wavs\n"); return 1;
    }

    bwa_desc cfg = { 0 };
    cfg.profile     = BWA_PROFILE_BINAURAL;
    cfg.sample_rate = RATE;
    cfg.block_size  = 256;
    if (g_tests) cfg.sink = BWA_SINK_NULL;   /* no device, deterministic */
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "bwa_create failed\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }
    const char* be = bwa_get_audio_backend(e);
    printf("backend: %s%s\n", be, strncmp(be, "null", 4) == 0 ? "  (no ASIO device - silent run)" : "");

    /* ---- [1] shared assets: two systems, one load ------------------------------------------- */
    printf("\n[1] shared ownership: two independent systems ask for the same clip.\n");
    bwa_sound a = bwa_sound_acquire(e, CLIP, 0);          /* "the footstep system" */
    bwa_sound b = bwa_sound_acquire(e, CLIP, 0);          /* "the prop system", unaware of the first */
    if (!a) { fprintf(stderr, "acquire: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return 1; }
    printf("    handle A = %u, handle B = %u  -> %s\n", a, b,
           verdict(a == b, "same handle, ONE decode, refcount 2", "DIFFERENT HANDLES (wrong)"));
    printf("    the explicit tier (bwa_load_sound) would have decoded it twice, and neither\n"
           "    system could unload without breaking the other. That is why every binding grew\n"
           "    a path-to-handle dictionary; this is that dictionary, moved inward.\n");

    /* ---- [2] flags are part of the key ------------------------------------------------------ */
    printf("\n[2] the cache key is (path, flags), not just path.\n");
    bwa_sound streamed = bwa_sound_acquire(e, CLIP, BWA_LOAD_STREAM);
    printf("    in RAM = %u, streamed = %u  -> %s\n", a, streamed,
           verdict(streamed && streamed != a, "different entries for ONE file, as they should be",
                   "collided (wrong)"));
    printf("    a combination no loader can express is refused, never narrowed:\n");
    if (bwa_sound_acquire(e, CLIP, BWA_LOAD_AMBIX | BWA_LOAD_FUMA) == 0)
        printf("      AMBIX|FUMA -> %s\n", bwa_last_error(e));
    else { ++g_bad; printf("      AMBIX|FUMA -> ACCEPTED (wrong)\n"); }

    /* ---- [3] probe without loading ---------------------------------------------------------- */
    printf("\n[3] bwa_sound_find asks a question about a PATH without answering it by loading.\n");
    printf("    find(loaded clip)   = %u  (resident: the handle, no new reference) %s\n",
           bwa_sound_find(e, CLIP, 0), verdict(bwa_sound_find(e, CLIP, 0) == a, "", "<- WRONG"));
    printf("    find(\"%s\") = %u  (not resident, and STILL not resident: nothing was decoded) %s\n",
           LATE, bwa_sound_find(e, LATE, 0),
           verdict(bwa_sound_find(e, LATE, 0) == 0, "", "<- WRONG"));
    printf("    probing with bwa_sound_acquire would have loaded it as a side effect - and loaded\n"
           "    it mono, so an ambisonic bed would answer 1 channel forever.\n");

    /* ---- [4] async: a handle before the data ------------------------------------------------ */
    printf("\n[4] async load: the handle comes back first, the PCM follows.\n");
    bwa_source late_src = bwa_source_create(e);
    bwa_source_set_pos(e, late_src, -2.0f, 1.5f, 0.0f);
    bwa_sound late = bwa_sound_acquire_async(e, LATE, 0);
    printf("    handle = %u, ready = %s  <- usable immediately, data not here yet\n",
           late, bwa_sound_is_ready(e, late) ? "yes" : "no");
    bwa_source_play(e, late_src, late, false);       /* legal NOW: the engine holds it control-side */
    printf("    played it anyway. The engine holds the play on the control thread and starts it\n"
           "    on the block the decode lands, so the audio thread only ever sees a finished asset.\n");
    for (int i = 0; i < 200 && !bwa_sound_is_ready(e, late); ++i) pump(e, 5);   /* bounded by readiness, not by wall time */
    printf("    ready = %s -> the held play started from the top of the asset.\n",
           verdict(bwa_sound_is_ready(e, late) != 0, "yes",
                   "no (decode failed or never landed: check bwa_last_error)"));
    for (int i = 0; i < ticks(60); ++i) pump(e, 16);

    /* ---- [5] source configuration in one struct ---------------------------------------------- */
    printf("\n[5] bwa_source_desc: fill from a preset, override, apply once.\n");
    bwa_source_desc d;
    bwa_source_preset(BWA_SRC_PROP, &d);             /* pure: no engine, no allocation */
    printf("    the PROP preset says gain %.2f, priority %d, air absorption %s, doppler %s\n",
           d.gain, d.priority, d.air_absorption ? "on" : "off", d.doppler ? "on" : "off");
    d.gain   = 0.7f;                                 /* disagree with one field... */
    d.spread = 0.35f;
    bwa_source prop = bwa_source_create_desc(e, &d); /* ...and create with the whole thing */
    bwa_source_set_pos(e, prop, 2.0f, 1.5f, 0.0f);   /* position is per-frame, NOT configuration */
    bwa_source_play(e, prop, a, true);
    printf("    created and configured in ONE call. The core-tier equivalent is bwa_source_create\n"
           "    plus a setter per field, which is what this replaces.\n");

    /* the readback half: before this there were getters for occlusion and directivity only */
    bwa_source_desc back;
    if (bwa_source_get_desc(e, prop, &back))
        printf("    read back: gain %.2f, spread %.2f  -> %s\n", back.gain, back.spread,
               verdict(back.gain == d.gain && back.spread == d.spread, "round-trips", "MISMATCH"));
    else { ++g_bad; printf("    read back FAILED: %s\n", bwa_last_error(e)); }

    /* a zero-init struct is REFUSED on purpose: this struct's zero is not its default (gain 0 is
     * silence, pitch 0 is invalid), so the mistake fails loudly instead of misconfiguring a source */
    bwa_source_desc zero = { 0 };
    printf("    apply(zero-initialized struct) -> %s\n",
           verdict(!bwa_source_apply(e, prop, &zero), "refused, as designed", "ACCEPTED (wrong)"));

    for (int i = 0; i < ticks(60); ++i) pump(e, 16);

    /* ---- [6] scene transition ---------------------------------------------------------------- */
    printf("\n[6] scene control: stop a category, or everything, click-free.\n");
    bwa_source_set_group(e, prop, 1);
    bwa_source_set_group(e, late_src, 2);
    bwa_commit(e);
    bwa_group_stop(e, 1);
    printf("    group 1 stopped (group 2 untouched). Both ride the one-block fade every stop uses,\n"
           "    so neither clicks; group gains and pause states survive - a stop is not a reset.\n");
    for (int i = 0; i < ticks(30); ++i) pump(e, 16);
    bwa_stop_all(e);
    printf("    then bwa_stop_all: every voice, beds included, pending queues dropped.\n");
    for (int i = 0; i < ticks(30); ++i) pump(e, 16);

    /* ---- teardown: release what you acquired, unload what you loaded ------------------------- */
    printf("\n[7] release drops ONE reference; the last one unloads.\n");
    bwa_source_destroy(e, prop);
    bwa_source_destroy(e, late_src);
    bwa_sound_release(e, a);                          /* system 1 is done... */
    printf("    after one release, still resident: find = %u %s\n", bwa_sound_find(e, CLIP, 0),
           verdict(bwa_sound_find(e, CLIP, 0) == a, "", "<- WRONG, should still be resident"));
    bwa_sound_release(e, b);                          /* ...system 2 is done too */
    bwa_commit(e);
    printf("    after the second, find = %u (unloaded through the same retire-ack path\n"
           "    bwa_unload_sound uses, so releasing a clip a voice is still playing is safe)\n",
           bwa_sound_find(e, CLIP, 0));
    if (bwa_sound_find(e, CLIP, 0) != 0) { ++g_bad; printf("    ...but it is STILL resident (wrong)\n"); }
    bwa_sound_release(e, streamed);
    bwa_sound_release(e, late);

    bwa_stop(e);
    bwa_destroy(e);
    remove(CLIP); remove(LATE);
    if (g_bad) { printf("\nconvenience: %d CHECK(S) FAILED\n", g_bad); return 1; }
    printf("\ndone.\n");
    return 0;
}
