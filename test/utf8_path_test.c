/*
 * utf8_path_test.c — every file the engine opens, opened through a path that is NOT ASCII.
 *
 * Why this is its own test: the ABI speaks UTF-8, but the Windows C runtime reads a narrow path in
 * the process ANSI codepage, so a path with an accent or a CJK character used to fail to open with
 * a plain "cannot open file". The fix (src/os/os.h: os_fopen, os_utf8_to_wide, and the dr_libs `_w`
 * openers) is spread across four subsystems that share nothing else, so the check belongs where it
 * can name all four: the whole-file decoder (sound.c), the streaming decoder (stream.c), the layout
 * JSON reader (layout.c), and the headphone-EQ parser (hpeq.c).
 *
 * The fixture directory and every file in it are created THROUGH THE SHIM, so the test does not
 * depend on the bug it is testing: a broken os_fopen fails at the write, not silently at the read.
 *
 * The literals below are \x escapes, not source-file UTF-8. MSVC reads a source file without a BOM
 * in the ANSI codepage unless it is given /utf-8, and this repo passes no such flag, so spelling
 * the bytes is the only way the test means the same thing on every compiler. The name is
 * "bwa_utf8_<e-acute>_<U+97F3>": one Latin accented letter (2 UTF-8 bytes) and one CJK character
 * (3 bytes), split across string literals so no hex escape can swallow the character after it.
 */
#include "binaural/hpeq.h"
#include "core/layout.h"
#include "os/os.h"
#include "core/sound.h"
#include "core/stream.h"

#include <stdio.h>
#include <string.h>

#define RATE 48000u

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", (msg)); \
                           else { printf("  FAIL %s\n", (msg)); ++fails; } } while (0)

/* "bwa_utf8_é_音" as UTF-8 bytes. Each escape run is its own literal so maximal-munch
 * cannot pull a following hex digit into it. */
#define DIR  "bwa_utf8_" "\xC3\xA9" "_" "\xE9\x9F\xB3"
#define WAVF DIR "/" "t" "\xC3\xA9" "st.wav"
#define JSNF DIR "/" "l" "\xC3\xA9" "ayout.json"
#define EQF  DIR "/" "\xE9\x9F\xB3" "_eq.txt"

/* A 16-bit PCM mono wav, written byte by byte through os_fopen. dr_wav's writer would do, but it
 * opens the path ITSELF - and that opener is half of what this test exists to pin, so the fixture
 * must not go through it. */
static int write_wav(const char* path, uint32_t frames, uint32_t rate) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    const uint32_t data = frames * 2u, riff = 36u + data, byte_rate = rate * 2u;
    const uint32_t fmt_size = 16u;
    const uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t v = (int16_t)(((i * 7u) % 256u) * 64u) - 8192;   /* anything but silence */
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return 1;
}

static int write_text(const char* path, const char* body) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    fputs(body, f);
    fclose(f);
    return 1;
}

/* Four speakers on a square: the smallest layout layout.c accepts. */
static const char* LAYOUT_JSON =
    "{ \"speakers\": ["
    "  { \"index\": 0, \"position\": [-2.0, 0.0, -2.0] },"
    "  { \"index\": 1, \"position\": [ 2.0, 0.0, -2.0] },"
    "  { \"index\": 2, \"position\": [ 2.0, 0.0,  2.0] },"
    "  { \"index\": 3, \"position\": [-2.0, 0.0,  2.0] } ] }";

static const char* EQ_TEXT =
    "Preamp: -6.0 dB\n"
    "Filter 1: ON PK Fc 1000 Hz Gain -3.0 dB Q 1.00\n"
    "Filter 2: ON LSC Fc 105 Hz Gain 4.0 dB Q 0.70\n";

int main(void) {
    char err[256] = { 0 };

    /* The directory NAME is deliberately not printed: runtime-printed strings stay ASCII
     * (CLAUDE.md), and a UTF-8 name on a Windows console codepage is mojibake. */
    printf("utf8 paths (fixture directory bwa_utf8_<U+00E9>_<U+97F3>)\n");
    CHECK(os_mkdir(DIR) == 0, "os_mkdir creates a directory with a non-ASCII name");
    CHECK(write_wav(WAVF, 24000, RATE) != 0, "os_fopen writes a wav into it");
    CHECK(write_text(JSNF, LAYOUT_JSON) != 0, "os_fopen writes the layout json");
    CHECK(write_text(EQF, EQ_TEXT) != 0, "os_fopen writes the headphone EQ text");

    /* Reading back through os_fopen is the shim's own round trip: if this fails, nothing below
     * means anything, because the fixture never landed where the test thinks it did. */
    {
        FILE* f = os_fopen(WAVF, "rb");
        CHECK(f != NULL, "os_fopen reopens the wav for reading");
        if (f) {
            char tag[4] = { 0 };
            CHECK(fread(tag, 1, 4, f) == 4 && memcmp(tag, "RIFF", 4) == 0,
                  "the bytes read back are the ones written (same file, not a same-named twin)");
            fclose(f);
        }
    }

    /* 1. the whole-file decoder (sound.c -> dr_wav) */
    {
        SoundData sd;
        memset(&sd, 0, sizeof sd);
        const bool ok = sound_load(WAVF, RATE, &sd, err, sizeof err);
        CHECK(ok, ok ? "sound_load decodes a non-ASCII path" : err);
        if (ok) {
            CHECK(sd.frames == 24000 && sd.channels == 1, "the decoded asset is the file that was written");
            sound_unload(&sd);
        }
    }

    /* 2. the streaming decoder (stream.c -> dr_wav's incremental opener) */
    {
        StreamSet* set = stream_set_create(RATE);
        CHECK(set != NULL, "stream_set_create");
        if (set) {
            err[0] = 0;
            Stream* s = stream_open(set, WAVF, err, sizeof err);
            CHECK(s != NULL, s ? "stream_open opens a non-ASCII path" : err);
            if (s) {
                CHECK(stream_total_frames(s) == 24000, "the stream reports the file's length");
                stream_close(set, s);
            }
            stream_set_destroy(set);
        }
    }

    /* 3. the layout json reader (layout.c -> os_fopen) */
    {
        static Layout L;
        memset(&L, 0, sizeof L);
        err[0] = 0;
        const bool ok = layout_load(JSNF, RATE, &L, err, sizeof err);
        CHECK(ok, ok ? "layout_load reads a non-ASCII path" : err);
        if (ok) CHECK(L.count == 4, "the loaded layout has the four speakers written");
    }

    /* 4. the headphone EQ parser (hpeq.c -> os_fopen) */
    {
        HpEqDesign d;
        memset(&d, 0, sizeof d);
        err[0] = 0;
        const int ok = hpeq_parse(EQF, RATE, &d, err, sizeof err);
        CHECK(ok != 0, ok ? "hpeq_parse reads a non-ASCII path" : err);
        if (ok) CHECK(d.nsec == 2, "both filter sections parsed");
    }

    /* A path that is NOT valid UTF-8 must fail cleanly rather than open something else. 0xFF can
     * never appear in a UTF-8 sequence. */
    CHECK(os_fopen(DIR "/" "\xFF" "bad.wav", "rb") == NULL, "an invalid-UTF-8 path fails to open");

    CHECK(os_remove(WAVF) == 0 && os_remove(JSNF) == 0 && os_remove(EQF) == 0,
          "os_remove deletes the non-ASCII fixture files");
    CHECK(os_rmdir(DIR) == 0, "os_rmdir removes the fixture directory");

    if (fails) { printf("utf8_path_test: %d FAILURES\n", fails); return 1; }
    printf("utf8_path_test OK (sound / stream / layout / headphone EQ all open a non-ASCII path)\n");
    return 0;
}
