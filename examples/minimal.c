/*
 * minimal.c — the smallest realistic bwaudio client: the calls an engine binding
 * (the Unity/Unreal glue) makes, in the order it makes them.
 *
 *   load time:  bw_create -> bw_start -> bw_load_sound -> bw_source_create
 *   per frame:  bw_source_set_pos / bw_set_listener_pose ... then ONE bw_commit
 *   teardown:   bw_source_destroy -> bw_unload_sound -> bw_stop -> bw_destroy
 *
 * Runs anywhere: the binaural profile auto-picks a 2-ch ASIO device (headphones)
 * and falls back to the silent offline sink without one -- bw_audio_backend says
 * which you got. Room frame is right-handed, +y up, +z forward, metres, origin on
 * the FLOOR (Motive's default; an identity listener faces +z, right ear at -x, and
 * y is height above the floor); with no layout file the engine pans over its default
 * grid, a 3 m cube of 26 speakers with its centre (the ear point) at (0, 1.5, 0).
 *
 *   bw_minimal [sound.wav]     (no argument: a short ping is synthesized and used)
 */
#include "bwaudio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* -- scaffolding, not part of the client pattern: hand-write a 0.5 s ping wav so
 *    the example runs with no assets. A real client ships files and skips this. -- */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }  /* x64 Windows: little-endian */
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static const char* ensure_ping(int argc, char** argv) {
    if (argc > 1) return argv[1];
    static const char* path = "bw_minimal_ping.wav";
    enum { RATE = 48000, N = RATE / 2 };
    static int16_t pcm[N];
    for (int i = 0; i < N; ++i) {
        double t = (double)i / RATE;                       /* 880 Hz, exponential decay */
        pcm[i] = (int16_t)(sin(6.283185307179586 * 880.0 * t) * exp(-6.0 * t) * 0.8 * 32767.0);
    }
    FILE* f = fopen(path, "wb");
    if (!f) return path;                                   /* let bw_load_sound report it */
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + sizeof pcm); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 1);  /* PCM, mono */
    put_u32(f, RATE); put_u32(f, RATE * 2); put_u16(f, 2); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, sizeof pcm); fwrite(pcm, 1, sizeof pcm, f);
    fclose(f);
    return path;
}

int main(int argc, char** argv) {
    const char* wav = ensure_ping(argc, argv);

    /* ---- load time: these may block, allocate, and touch disk ---- */
    BwConfig cfg = { 0 };
    cfg.profile        = BW_PROFILE_BINAURAL;  /* desk profile: 26-ch bus -> HRTF -> stereo */
    cfg.sample_rate    = 48000;
    cfg.block_size     = 256;
    cfg.track_internal = false;                /* this "game" pushes the listener pose itself */

    BwEngine* e = bw_create(&cfg);
    if (!e) { fprintf(stderr, "bw_create failed\n"); return 1; }
    if (bw_start(e) != 0) {
        fprintf(stderr, "bw_start: %s\n", bw_last_error(e));
        bw_destroy(e); return 1;
    }
    const char* be = bw_audio_backend(e);
    printf("backend: %s%s\n", be, strcmp(be, "null") == 0 ? "  (no ASIO device - silent run)" : "");

    BwSound ping = bw_load_sound(e, wav);      /* decoded + resampled now, off the hot path */
    if (!ping) {
        fprintf(stderr, "%s: %s\n", wav, bw_last_error(e));
        bw_stop(e); bw_destroy(e); return 1;
    }

    BwSource src = bw_source_create(e);
    bw_source_set_gain(e, src, 0.8f);
    bw_source_set_pos(e, src, 2.f, 1.5f, 0.f); /* room space: ear height, 2 m to the listener's left (+x) */
    bw_source_play(e, src, ping, true);        /* loop while we move it */

    /* ---- the game loop: push updates every frame, then ONE commit ---- */
    printf("orbiting a looping ping around the listener (6 s)...\n");
    for (int frame = 0; frame < 6 * 60; ++frame) {
        float a = (float)(6.283185307179586 * frame / (3.0 * 60.0));   /* one lap / 3 s */
        bw_source_set_pos(e, src, 2.f * cosf(a), 1.5f, -2.f * sinf(a));
        bw_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);   /* standing at the array centre */
        bw_commit(e);                          /* promote this frame's updates as one snapshot */
        Sleep(16);                             /* ~60 Hz, like an engine tick */
    }
    bw_source_stop(e, src);

    /* fire-and-forget at a fixed point: no handle to manage, the voice recycles itself */
    bw_play_oneshot(e, ping, -2.f, 1.5f, 0.f, 1.f);

    /* completion: there are no callbacks -- play, then poll bw_source_is_playing.
     * The readback publishes per audio block, so give the play command a moment to land
     * before trusting a false answer (this is what the Unity binding's poll does too). */
    bw_source_play(e, src, ping, false);
    bw_commit(e);
    Sleep(50);
    while (bw_source_is_playing(e, src)) { bw_commit(e); Sleep(16); }
    printf("finished\n");

    /* ---- teardown ---- */
    bw_source_destroy(e, src);
    bw_unload_sound(e, ping);                  /* safe order: retire is acked internally */
    bw_stop(e);
    bw_destroy(e);
    return 0;
}
