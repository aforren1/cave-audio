/*
 * sound.c — wav decode via dr_wav (control thread, load time). Decodes to mono float at
 * the engine sample rate; rejects rate mismatches (no resampling in M3).
 */
#include "sound.h"

#include <stdlib.h>
#include <string.h>

#define DR_WAV_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)        /* dr_wav is a third-party single-header; silence its warnings */
#endif
#include "dr_wav.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

bool sound_load(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (!path) { set_err(err, errcap, "sound: null path"); return false; }

    unsigned int  channels = 0, rate = 0;
    drwav_uint64  frames = 0;
    float* interleaved = drwav_open_file_and_read_pcm_frames_f32(path, &channels, &rate, &frames, NULL);
    if (!interleaved)               { set_err(err, errcap, "sound: cannot open/decode wav"); return false; }
    if (channels == 0 || frames == 0) { drwav_free(interleaved, NULL); set_err(err, errcap, "sound: empty wav"); return false; }
    if (rate != want_rate)          { drwav_free(interleaved, NULL); set_err(err, errcap, "sound: sample-rate mismatch (no resampling)"); return false; }
    if (frames > 0xFFFFFFFFu)        { drwav_free(interleaved, NULL); set_err(err, errcap, "sound: too many frames (>4G; SoundData.frames is 32-bit)"); return false; }

    float* mono = (float*)malloc((size_t)frames * sizeof(float));
    if (!mono) { drwav_free(interleaved, NULL); set_err(err, errcap, "sound: out of memory"); return false; }

    if (channels == 1) {
        memcpy(mono, interleaved, (size_t)frames * sizeof(float));
    } else {
        const float inv = 1.0f / (float)channels;
        for (drwav_uint64 i = 0; i < frames; ++i) {
            float acc = 0.f;
            for (unsigned int ch = 0; ch < channels; ++ch) acc += interleaved[i * channels + ch];
            mono[i] = acc * inv;                  /* equal-weight downmix to mono */
        }
    }
    drwav_free(interleaved, NULL);

    out->pcm = mono;
    out->frames = (uint32_t)frames;
    out->sample_rate = rate;
    return true;
}

void sound_unload(SoundData* s) {
    if (!s) return;
    free(s->pcm);
    s->pcm = NULL;
    s->frames = 0;
    s->sample_rate = 0;
}
