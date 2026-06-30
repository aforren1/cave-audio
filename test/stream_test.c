/*
 * stream_test.c — verifies the background streaming ring without the engine: stream a file longer than
 * the ring (so it refills several times), and confirm the pulled samples match the file exactly, EOF
 * is reported at the end, and a looping stream wraps. The audio side here pulls as fast as it can, so
 * it deliberately outruns the decode thread and exercises the underrun path too.
 */
#include "stream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static float patt(uint64_t i) { return (float)((i * 7u) % 256u) / 256.0f - 0.5f; }   /* exact in float32 */

static void write_wav_f32_mono(const char* path, int n, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int br = rate * 4, ds = n * 4, sz16 = 16, rc = 36 + ds; short fmt = 3, ch = 1, bps = 32, ba = 4;
    fwrite("RIFF",1,4,f); fwrite(&rc,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sz16,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&rate,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f);
    for (int i = 0; i < n; ++i) { float v = patt((uint64_t)i); fwrite(&v,4,1,f); }
    fclose(f);
}

static int wait_prebuffer(Stream* s) { for (int t = 0; t < 2000; ++t) { if (stream_prebuffered(s)) return 1; Sleep(1); } return 0; }

int main(void) {
    const uint32_t RATE = 48000; const int N = 200000;   /* ~4 s, well past the 65536-sample ring */
    const char* WAV = "bw_stream_test.wav";
    char err[256] = {0};
    write_wav_f32_mono(WAV, N, (int)RATE);

    StreamSet* set = stream_set_create(RATE);
    CHECK(set != NULL, "stream_set_create");
    if (!set) { remove(WAV); return 1; }

    /* a wrong-rate file is rejected */
    write_wav_f32_mono("bw_stream_44k.wav", 1000, 44100);
    Stream* bad = stream_open(set, "bw_stream_44k.wav", err, sizeof err);
    CHECK(bad == NULL, "rejects a sample-rate mismatch");
    remove("bw_stream_44k.wav");

    /* --- stream the whole file, sample-exact --- */
    Stream* s = stream_open(set, WAV, err, sizeof err);
    CHECK(s != NULL, err[0] ? err : "stream_open");
    if (s) {
        stream_start(s, 0);
        CHECK(wait_prebuffer(s), "prebuffered");
        uint64_t pos = 0; int mismatch = 0; float blk[256];
        for (int guard = 0; pos < (uint64_t)N && guard < 2000000; ++guard) {
            uint32_t got = stream_pull(s, pos, blk, 256);
            if (got == 0) { if (stream_ended(s, pos)) break; Sleep(1); continue; }   /* underrun: wait */
            for (uint32_t k = 0; k < got; ++k) if (fabsf(blk[k] - patt(pos + k)) > 1e-6f) ++mismatch;
            pos += got;
        }
        CHECK(mismatch == 0, "streamed samples match the file exactly");
        CHECK(pos == (uint64_t)N, "streamed the whole file");
        CHECK(stream_ended(s, pos), "reports EOF at the end");
        stream_close(set, s);
    }

    /* --- looping stream wraps past the end --- */
    Stream* sl = stream_open(set, WAV, err, sizeof err);
    CHECK(sl != NULL, err[0] ? err : "stream_open (loop)");
    if (sl) {
        stream_start(sl, 1);
        CHECK(wait_prebuffer(sl), "prebuffered (loop)");
        uint64_t pos = 0; int mismatch = 0; float blk[256];
        uint64_t target = (uint64_t)N + 5000;          /* read past the file end */
        for (int guard = 0; pos < target && guard < 2000000; ++guard) {
            uint32_t got = stream_pull(sl, pos, blk, 256);
            if (got == 0) { Sleep(1); continue; }
            for (uint32_t k = 0; k < got; ++k) if (fabsf(blk[k] - patt((pos + k) % (uint64_t)N)) > 1e-6f) ++mismatch;
            pos += got;
        }
        CHECK(mismatch == 0, "looping stream wraps at the file boundary");
        CHECK(!stream_ended(sl, pos), "looping stream never reports EOF");
        stream_close(set, sl);
    }

    stream_set_destroy(set);
    remove(WAV);
    if (fails) { printf("stream_test: %d FAILURES\n", fails); return 1; }
    printf("stream_test OK (long-file streaming, sample-exact, rate-reject, EOF, loop-wrap verified)\n");
    return 0;
}
