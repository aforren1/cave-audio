/*
 * streaming.c — the two ways audio reaches a voice WITHOUT decoding a whole file into RAM:
 *
 *   bwa_load_sound_streaming   long files (music/ambience): a background thread decodes chunks
 *                              into a per-stream ring, the voice reads the ring. RAM stays flat
 *                              no matter how long the file is.
 *   bwa_source_create_push   PUSH (procedural) sources: YOU generate PCM on the control thread
 *                              and push it into the same kind of ring — synthesis, VoIP, a
 *                              game-engine submix, anything without a file.
 *
 * The rules this example demonstrates:
 *   - streamed files must be at the engine rate already (no load-time resample on this path;
 *     bwa_load_sound resamples, bwa_load_sound_streaming refuses) — mono, or downmixed;
 *   - one voice per stream (a second play steals the ring from the first);
 *   - push sources start consuming at CREATE: silence until the first push, and an underrun
 *     renders silence without losing your place — pace with bwa_source_push_space, stay a
 *     frame or two ahead;
 *   - push sources are one-way: bwa_source_push_end (or stop/fade_out) ends the voice once the
 *     ring drains; create a new one to restart;
 *   - protect a music stream from voice-stealing with priority 255.
 *
 * Runs anywhere (binaural profile; silent null-sink fallback without an ASIO device).
 *
 *   bwa_streaming
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define RATE 48000u

/* -- scaffolding: write a 20 s "music" wav (16-bit PCM mono) so the example ships no assets.
 *    A pentatonic arpeggio with a soft envelope — long enough that streaming it matters. -- */
static void put_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_music(const char* path, uint32_t secs) {
    const uint32_t frames = RATE * secs;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + frames * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, 1);
    put_u32(f, RATE); put_u32(f, RATE * 2); put_u16(f, 2); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, frames * 2);
    static const float scale[5] = { 220.0f, 261.63f, 293.66f, 329.63f, 392.0f };
    const uint32_t note = RATE / 4;                       /* 250 ms per note */
    double ph = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t n = (i / note) % 8;
        float hz = scale[(n * 3) % 5] * ((n & 4) ? 2.0f : 1.0f);
        ph += 6.283185307179586 * hz / RATE;
        float env = expf(-3.0f * (float)(i % note) / note);
        int16_t s = (int16_t)(sin(ph) * env * 0.5f * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 1;
}

int main(void) {
    if (!write_music("bwa_demo_music.wav", 20)) { fprintf(stderr, "cannot write demo wav\n"); return 1; }

    bwa_desc cfg = { 0 };
    cfg.profile     = BWA_PROFILE_BINAURAL;
    cfg.sample_rate = RATE;                     /* streamed files must match this rate */
    cfg.block_size  = 256;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "bwa_create failed\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }
    const char* be = bwa_get_audio_backend(e);
    printf("backend: %s%s\n", be, strncmp(be, "null", 4) == 0 ? "  (no ASIO device - silent run)" : "");

    /* ---- part 1: stream a long file from disk ---- */
    printf("\n[1] file streaming: the 20 s file is NOT decoded into RAM - a background\n"
           "    thread feeds the voice as it plays. Orbiting it for 8 s...\n");
    bwa_sound music = bwa_load_sound_streaming(e, "bwa_demo_music.wav");
    if (!music) { fprintf(stderr, "stream open: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return 1; }

    bwa_source s1 = bwa_source_create(e);
    bwa_source_set_priority(e, s1, 255);        /* music: never let an SFX overload steal this voice */
    bwa_source_set_gain(e, s1, 0.9f);
    bwa_source_play(e, s1, music, false);       /* the streaming thread re-seeks + fills the ring now */
    for (int t = 0; t < 8 * 60; ++t) {
        float a = 6.2831853f * t / (5.0f * 60.0f);
        bwa_source_set_pos(e, s1, 2.0f * cosf(a), 1.5f, 2.0f * sinf(a));
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
        bwa_commit(e);
        Sleep(16);
    }
    printf("    still playing: %s (12 s of file left - stopping early)\n",
           bwa_source_is_playing(e, s1) ? "yes" : "no");
    bwa_source_fade_out(e, s1, 0.4f);           /* click-free stop */
    Sleep(600);
    bwa_source_destroy(e, s1);
    bwa_unload_sound(e, music);                 /* closes the stream (retire-acked internally) */

    /* ---- part 2: a push (procedural) source ---- */
    printf("\n[2] push source: generating an FM sweep on the control thread and pushing it\n"
           "    into the voice's ring (~1.3 s deep). 6 s, then push_end + drain...\n");
    bwa_source s2 = bwa_source_create_push(e);
    if (!s2) { fprintf(stderr, "create_stream: %s\n", bwa_last_error(e)); bwa_stop(e); bwa_destroy(e); return 1; }
    bwa_source_set_gain(e, s2, 0.7f);
    bwa_source_set_pos(e, s2, -2.0f, 1.5f, 1.0f);

    double ph = 0.0, mod = 0.0;
    uint64_t pushed = 0;
    const uint64_t total = RATE * 6;
    while (pushed < total) {
        /* generate up to one tick's worth (plus initial priming), capped by the ring's free space —
         * the data-driven clock slips on underrun rather than dropping, but pacing keeps it clean */
        uint32_t space = bwa_source_push_space(e, s2);
        uint32_t want  = (uint32_t)(pushed == 0 ? RATE / 10 : RATE / 50);   /* prime 100 ms, then 20 ms/tick */
        if (want > space) want = space;
        if (want > total - pushed) want = (uint32_t)(total - pushed);
        float chunk[RATE / 10];
        for (uint32_t i = 0; i < want; ++i) {
            mod += 6.283185307179586 * 0.15 / RATE;                 /* slow sweep LFO */
            double hz = 300.0 + 220.0 * sin(mod) + 40.0 * sin(mod * 7.3);
            ph += 6.283185307179586 * hz / RATE;
            chunk[i] = (float)(sin(ph) * 0.5);
        }
        uint32_t took = bwa_source_push(e, s2, chunk, want);
        pushed += took;                          /* took < want only when the ring is full */
        bwa_commit(e);
        Sleep(16);
    }
    bwa_source_push_end(e, s2);                  /* end-of-data: the voice ends once the ring drains */
    printf("    pushed %.1f s; waiting for the ring to drain...\n", (double)pushed / RATE);
    while (bwa_source_is_playing(e, s2)) { bwa_commit(e); Sleep(16); }
    printf("    drained - the push source ended itself (one-way: a new take needs a new source)\n");
    bwa_source_destroy(e, s2);

    bwa_stop(e);
    bwa_destroy(e);
    remove("bwa_demo_music.wav");
    printf("\ndone\n");
    return 0;
}
