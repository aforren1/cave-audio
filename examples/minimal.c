/*
 * minimal.c — the smallest realistic bw_audio client: the calls an engine binding
 * (the Unity/Unreal glue) makes, in the order it makes them.
 *
 *   load time:  bwa_create -> bwa_start -> bwa_load_sound -> bwa_source_create
 *   per frame:  bwa_source_set_pos / bwa_set_listener_pose ... then ONE bwa_commit
 *   teardown:   bwa_source_destroy -> bwa_unload_sound -> bwa_stop -> bwa_destroy
 *
 * Runs anywhere: the binaural profile auto-picks a 2-ch ASIO device (headphones)
 * and falls back to the silent offline sink without one -- bwa_get_audio_backend says
 * which you got. Room frame is right-handed, +y up, +z forward, meters, origin on
 * the FLOOR (Motive's default; an identity listener faces +z, right ear at -x, and
 * y is height above the floor); with no layout file the engine pans over its default
 * grid, a 3 m cube of 26 speakers with its center (the ear point) at (0, 1.5, 0).
 *
 * This is the CORE tier throughout: load, create, a setter per knob, commit. bwa_convenience
 * walks the calls layered over it (shared/async assets, a source's whole config as one struct,
 * click-free group stops) and shows which core calls each one replaces.
 *
 * The last two steps cover the two EVENT drains. A binding runs both every frame, right after its
 * commit: bwa_poll_ended (a voice finished) and bwa_poll_looped (a voice wrapped at its loop
 * point). They get a step each here so each one is legible, and they are the only self-checked
 * claims in the file -- see the check() note below.
 *
 *   bwa_minimal [sound.wav] [--driver name]
 *     (no wav: a short ping is synthesized and used; no --driver: auto-pick the first
 *      ASIO driver with enough channels — name one to test a specific device. The rig's
 *      Digiface registers under RME's own driver name, which is NOT the product name:
 *      read it off bwa_calibrate --list-drivers rather than guessing.)
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* -- scaffolding, not part of the client pattern: hand-write a 0.5 s ping wav so
 *    the example runs with no assets. A real client ships files and skips this. -- */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }  /* x64 Windows: little-endian */
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static const char* ensure_ping(const char* wav_arg) {
    if (wav_arg) return wav_arg;
    static const char* path = "bwa_minimal_ping.wav";
    enum { RATE = 48000, N = RATE / 2 };
    static int16_t pcm[N];
    for (int i = 0; i < N; ++i) {
        double t = (double)i / RATE;                       /* 880 Hz, exponential decay */
        pcm[i] = (int16_t)(sin(6.283185307179586 * 880.0 * t) * exp(-6.0 * t) * 0.8 * 32767.0);
    }
    FILE* f = fopen(path, "wb");
    if (!f) return path;                                   /* let bwa_load_sound report it */
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + sizeof pcm); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 1);  /* PCM, mono */
    put_u32(f, RATE); put_u32(f, RATE * 2); put_u16(f, 2); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, sizeof pcm); fwrite(pcm, 1, sizeof pcm, f);
    fclose(f);
    return path;
}

/* --tests: force the offline sink and cut the orbit short, so ctest runs this without a device.
 * The CALLS are identical either way; only the listening time goes. */
static int g_tests = 0;

/* The event drains at the end are CHECKED, so ctest running this catches a broken drain and not
 * only a crash. Everything above them is a walkthrough with nothing to assert; these two have a
 * definite right answer, so they get one. A check that cannot fail is worse than no check, so the
 * loop count below is demanded with a margin rather than as "more than zero". */
static int g_bad = 0;
static void check(int ok, const char* what) {
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_bad;
}

int main(int argc, char** argv) {
    const char* wav_arg = NULL, * driver = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--tests")) g_tests = 1;
        else wav_arg = argv[i];
    }
    const char* wav = ensure_ping(wav_arg);

    /* ---- load time: these may block, allocate, and touch disk ---- */
    bwa_desc cfg = { 0 };
    cfg.profile        = BWA_PROFILE_BINAURAL;  /* desk profile: direct per-source HRTF -> stereo
                                                 * (BWA_PROFILE_CAVE_SIM auditions the array instead) */
    cfg.sample_rate    = 48000;
    cfg.block_size     = 256;
    cfg.asio_driver    = driver;                /* NULL = auto-pick by channel count */
    if (g_tests) cfg.sink = BWA_SINK_NULL;      /* no device, deterministic */
    /* no tracker connected: this "game" pushes the listener pose itself */

    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "bwa_create failed\n"); return 1; }
    if (bwa_start(e) != 0) {
        fprintf(stderr, "bwa_start: %s\n", bwa_last_error(e));
        bwa_destroy(e); return 1;
    }
    const char* be = bwa_get_audio_backend(e);
    printf("backend: %s%s\n", be, strcmp(be, "null") == 0 ? "  (no ASIO device - silent run)" : "");

    bwa_sound ping = bwa_load_sound(e, wav);      /* decoded + resampled now, off the hot path */
    if (!ping) {
        fprintf(stderr, "%s: %s\n", wav, bwa_last_error(e));
        bwa_stop(e); bwa_destroy(e); return 1;
    }

    bwa_source src = bwa_source_create(e);
    bwa_source_set_gain(e, src, 0.8f);
    bwa_source_set_pos(e, src, 2.f, 1.5f, 0.f); /* room space: ear height, 2 m to the listener's left (+x) */
    bwa_source_play(e, src, ping, true);        /* loop while we move it */

    /* ---- the game loop: push updates every frame, then ONE commit ---- */
    uint64_t cs0 = 0, ct0 = 0;                 /* baseline device clock pair (rate check below) */
    bool have_clk = false;
    printf("orbiting a looping ping around the listener (6 s)...\n");
    const int frames_total = g_tests ? 45 : 6 * 60;
    for (int frame = 0; frame < frames_total; ++frame) {
        if (frame == 30 && !have_clk)          /* baseline ~0.5 s in: skip the start-of-stream
                                                * prefill burst (drivers fill their ring with a few
                                                * back-to-back callbacks at start, which would read
                                                * as a fake rate error) */
            have_clk = bwa_get_clock(e, &cs0, &ct0);
        float a = (float)(6.283185307179586 * frame / (3.0 * 60.0));   /* one lap / 3 s */
        bwa_source_set_pos(e, src, 2.f * cosf(a), 1.5f, -2.f * sinf(a));
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);   /* standing at the array center */
        bwa_commit(e);                          /* promote this frame's updates as one snapshot */
        Sleep(16);                             /* ~60 Hz, like an engine tick */
    }
    bwa_source_stop(e, src);
    Sleep(400);                                 /* breathe: the looping ping restarts every 0.5 s, so
                                                 * whichever iteration is in flight gets cut by the
                                                 * (one-block, click-free) stop — without a gap its
                                                 * truncated attack lands right under the oneshot
                                                 * below and reads as a doubled beep */

    /* device clock sanity: the audio stack stamps a (sample position, host time) pair per block
     * (bwa_get_clock — the wall<->dsp bridge for AV sync), so two pairs a few seconds apart measure
     * the device's TRUE sample rate. On a rig, a Dante clocking problem — wrong nominal rate,
     * unlocked clock domain — shows up here as far more than a few ppm of deviation. */
    uint64_t cs1 = 0, ct1 = 0;
    if (have_clk && bwa_get_clock(e, &cs1, &ct1) && ct1 > ct0) {
        double rate = (double)(cs1 - cs0) * 1e9 / (double)(ct1 - ct0);
        printf("device clock: %.2f Hz measured over the run (nominal %u, %+.1f ppm)\n",
               rate, cfg.sample_rate, (rate / (double)cfg.sample_rate - 1.0) * 1e6);
    }

    /* fire-and-forget at a fixed point: no handle to manage, the voice recycles itself. The
     * return is the ONLY signal a oneshot gives you -- false means it never sounded, so check
     * it rather than assuming silence is the mix. */
    if (!bwa_play_oneshot(e, ping, -2.f, 1.5f, 0.f, 1.f))
        printf("oneshot dropped: %s\n", bwa_last_error(e));
    Sleep(600);                                 /* let the 0.5 s ping ring out — a oneshot has no
                                                 * handle to poll, and without the wait the play
                                                 * below starts on top of it (two overlapping dings,
                                                 * which by ear reads as a bug) */

    /* completion is an EVENT, and this is the drain a binding runs every frame. bwa_poll_ended
     * hands back the sources whose voices finished since the last call, oldest first, and
     * bwa_commit is the pass that fills it -- so commit, then drain. The handles come back exactly
     * as you knew them, so comparing against `src` is how you dispatch.
     * Polling bwa_source_is_playing instead is the weaker path: it republishes per AUDIO block, so
     * a sound shorter than one frame can begin and end without ever once reading as playing. Keep
     * is_playing for "is this still going", not for "did it finish". */
    printf("waiting for a one-shot to finish, off the event drain...\n");
    bwa_source_play(e, src, ping, false);
    bwa_commit(e);
    int ended = 0;
    for (int t = 0; t < 200 && !ended; ++t) {    /* ~3 s cap; the ping is 0.5 s */
        bwa_commit(e);                           /* the drain point */
        bwa_source done[8];
        uint32_t n = bwa_poll_ended(e, done, 8, NULL);
        for (uint32_t i = 0; i < n; ++i) if (done[i] == src) ended = 1;
        Sleep(16);
    }
    check(ended, "bwa_poll_ended reported the ping's end");

    /* the OTHER boundary, and the one this installation runs on: a looping voice never ends, so
     * bwa_poll_ended reports it exactly never. bwa_poll_looped is its sibling -- one entry per WRAP,
     * from the same drain -- which is how you pace a trial off the content instead of off a frame
     * timer. bwa_source_set_region picks where the wrap lands: it bounds the voice to
     * [start, end) content frames, here the ping's first 100 ms, so it comes round about ten times
     * a second. Set the region AFTER the play -- a play resolves the bounds against the asset and
     * resets any region already set (bwa_source_play_loop is the same state set at play time). */
    printf("pacing off the loop boundary (a 100 ms region of the ping, ~1 s)...\n");
    bwa_source_play(e, src, ping, true);
    bwa_source_set_region(e, src, 0, cfg.sample_rate / 10);
    /* Drain first, and count only after. Both rings are ENGINE-WIDE and DESTRUCTIVE, and nothing
     * above drained the loop one, so the orbit's own wraps are still queued -- counted here they
     * would let this check pass on a region that never took. Drain what you did not ask for
     * before you measure what you did. */
    { bwa_source flush[64]; while (bwa_poll_looped(e, flush, 64, NULL) == 64) { } }
    int wraps = 0;
    for (int t = 0; t < 63; ++t) {               /* ~1 s at a 16 ms tick */
        bwa_commit(e);
        bwa_source hit[16];
        uint32_t n = bwa_poll_looped(e, hit, 16, NULL);
        for (uint32_t i = 0; i < n; ++i) if (hit[i] == src) ++wraps;
        Sleep(16);
    }
    bwa_source_stop(e, src);
    printf("  %d wraps\n", wraps);
    /* ~10 expected. Demanded with a margin, not as "more than zero": if the region never reached
     * the core the voice loops the whole 0.5 s ping and still wraps twice in this window, so a
     * >0 check would pass against a region that did nothing at all. */
    check(wraps >= 5, "bwa_poll_looped reported one entry per wrap");

    /* ---- teardown ---- */
    bwa_source_destroy(e, src);
    bwa_unload_sound(e, ping);                  /* safe order: retire is acked internally */
    bwa_stop(e);
    bwa_destroy(e);
    if (g_bad) { printf("\nminimal: %d CHECK(S) FAILED\n", g_bad); return 1; }
    printf("\nfinished\n");
    return 0;
}
