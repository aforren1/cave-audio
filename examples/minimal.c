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
 * which you got. Room frame is right-handed, +y up, +z forward, metres, origin on
 * the FLOOR (Motive's default; an identity listener faces +z, right ear at -x, and
 * y is height above the floor); with no layout file the engine pans over its default
 * grid, a 3 m cube of 26 speakers with its centre (the ear point) at (0, 1.5, 0).
 *
 *   bwa_minimal [sound.wav] [--driver name]
 *     (no wav: a short ping is synthesized and used; no --driver: auto-pick the first
 *      ASIO driver with enough channels — name one to test a specific device, e.g.
 *      "Dante Virtual Soundcard" on the rig. bwa_calibrate --list-drivers enumerates.)
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

int main(int argc, char** argv) {
    const char* wav_arg = NULL, * driver = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else wav_arg = argv[i];
    }
    const char* wav = ensure_ping(wav_arg);

    /* ---- load time: these may block, allocate, and touch disk ---- */
    bwa_desc cfg = { 0 };
    cfg.profile        = BWA_PROFILE_BINAURAL;  /* desk profile: 26-ch bus -> HRTF -> stereo */
    cfg.sample_rate    = 48000;
    cfg.block_size     = 256;
    cfg.asio_driver    = driver;                /* NULL = auto-pick by channel count */
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
    for (int frame = 0; frame < 6 * 60; ++frame) {
        if (frame == 30 && !have_clk)          /* baseline ~0.5 s in: skip the start-of-stream
                                                * prefill burst (drivers fill their ring with a few
                                                * back-to-back callbacks at start, which would read
                                                * as a fake rate error) */
            have_clk = bwa_get_clock(e, &cs0, &ct0);
        float a = (float)(6.283185307179586 * frame / (3.0 * 60.0));   /* one lap / 3 s */
        bwa_source_set_pos(e, src, 2.f * cosf(a), 1.5f, -2.f * sinf(a));
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);   /* standing at the array centre */
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

    /* fire-and-forget at a fixed point: no handle to manage, the voice recycles itself */
    bwa_play_oneshot(e, ping, -2.f, 1.5f, 0.f, 1.f);
    Sleep(600);                                 /* let the 0.5 s ping ring out — a oneshot has no
                                                 * handle to poll, and without the wait the play
                                                 * below starts on top of it (two overlapping dings,
                                                 * which by ear reads as a bug) */

    /* completion: there are no callbacks -- play, then poll bwa_source_is_playing.
     * The readback publishes per audio block, so give the play command a moment to land
     * before trusting a false answer (this is what the Unity binding's poll does too). */
    bwa_source_play(e, src, ping, false);
    bwa_commit(e);
    Sleep(50);
    while (bwa_source_is_playing(e, src)) { bwa_commit(e); Sleep(16); }
    printf("finished\n");

    /* ---- teardown ---- */
    bwa_source_destroy(e, src);
    bwa_unload_sound(e, ping);                  /* safe order: retire is acked internally */
    bwa_stop(e);
    bwa_destroy(e);
    return 0;
}
