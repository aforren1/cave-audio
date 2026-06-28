/*
 * sound.c — audio file decode via dr_libs (control thread, load time). Decodes WAV / FLAC / MP3 to
 * mono float, RESAMPLING to the engine sample rate when the file's rate differs (a load-time
 * windowed-sinc pass — handy for the 44.1 kHz MP3s people drag along). Never runs on the audio thread.
 */
#include "sound.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)        /* dr_libs are third-party single-headers; silence their warnings */
#endif
#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#define BW_PI 3.14159265358979323846

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Decode the whole file to interleaved float by extension (default WAV). Channels/rate/frames are
 * filled on success; the returned buffer is the default-allocator (NULL callbacks) so plain free()
 * releases it for all three decoders. NULL on failure. */
static float* decode_any(const char* path, unsigned int* ch, unsigned int* rate, uint64_t* frames) {
    const char* ext = strrchr(path, '.');
    if (ext && _stricmp(ext, ".flac") == 0) {
        drflac_uint64 f = 0;
        float* p = drflac_open_file_and_read_pcm_frames_f32(path, ch, rate, &f, NULL);
        *frames = (uint64_t)f; return p;
    }
    if (ext && _stricmp(ext, ".mp3") == 0) {
        drmp3_config cfg; drmp3_uint64 f = 0;
        float* p = drmp3_open_file_and_read_pcm_frames_f32(path, &cfg, &f, NULL);
        if (p) { *ch = cfg.channels; *rate = cfg.sampleRate; }
        *frames = (uint64_t)f; return p;
    }
    drwav_uint64 f = 0;                               /* default / .wav */
    float* p = drwav_open_file_and_read_pcm_frames_f32(path, ch, rate, &f, NULL);
    *frames = (uint64_t)f; return p;
}

static double sinc(double x) { return (x == 0.0) ? 1.0 : sin(BW_PI * x) / (BW_PI * x); }

/* Resample mono `in` (n_in frames at fin) to fout. Windowed-sinc (Blackman), normalized, with the
 * cutoff dropped to the lower Nyquist on downsampling so it band-limits. Returns a new malloc'd
 * buffer (caller frees) + sets *n_out; NULL on OOM. Quality is for load-time asset prep, not the RT path. */
static float* resample_mono(const float* in, uint64_t n_in, uint32_t fin, uint32_t fout, uint64_t* n_out) {
    const double ratio = (double)fout / (double)fin;
    uint64_t no = (uint64_t)((double)n_in * ratio + 0.5);
    if (no == 0) no = 1;
    float* out = (float*)malloc((size_t)no * sizeof(float));
    if (!out) return NULL;

    const double fc   = ratio < 1.0 ? ratio : 1.0;   /* normalized cutoff (anti-alias when downsampling) */
    const int    TAPS = 16;                           /* half-width in input samples (at fc = 1) */
    const double half = (double)TAPS / fc;            /* wider support when downsampling */
    for (uint64_t i = 0; i < no; ++i) {
        const double t = (double)i / ratio;           /* position in input samples */
        int64_t lo = (int64_t)ceil(t - half), hi = (int64_t)floor(t + half);
        if (lo < 0) lo = 0;
        if (hi >= (int64_t)n_in) hi = (int64_t)n_in - 1;
        double acc = 0.0, wsum = 0.0;
        for (int64_t j = lo; j <= hi; ++j) {
            const double x = t - (double)j;            /* distance in input samples */
            const double wn = (x + half) / (2.0 * half);                 /* window position in [0,1] */
            const double w  = 0.42 - 0.5 * cos(2.0 * BW_PI * wn) + 0.08 * cos(4.0 * BW_PI * wn); /* Blackman */
            const double h  = fc * sinc(fc * x) * w;
            acc += (double)in[j] * h; wsum += h;
        }
        out[i] = (float)(wsum != 0.0 ? acc / wsum : 0.0);
    }
    *n_out = no;
    return out;
}

bool sound_load(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (!path) { set_err(err, errcap, "sound: null path"); return false; }

    unsigned int channels = 0, rate = 0;
    uint64_t frames = 0;
    float* interleaved = decode_any(path, &channels, &rate, &frames);
    if (!interleaved)                 { set_err(err, errcap, "sound: cannot open/decode (wav/flac/mp3)"); return false; }
    if (channels == 0 || frames == 0) { free(interleaved); set_err(err, errcap, "sound: empty file"); return false; }

    /* downmix to mono (equal weight) */
    float* mono = (float*)malloc((size_t)frames * sizeof(float));
    if (!mono) { free(interleaved); set_err(err, errcap, "sound: out of memory"); return false; }
    if (channels == 1) {
        memcpy(mono, interleaved, (size_t)frames * sizeof(float));
    } else {
        const float inv = 1.0f / (float)channels;
        for (uint64_t i = 0; i < frames; ++i) {
            float acc = 0.f;
            for (unsigned int c = 0; c < channels; ++c) acc += interleaved[i * channels + c];
            mono[i] = acc * inv;
        }
    }
    free(interleaved);

    /* resample to the engine rate if needed (a load-time pass; the audio thread assumes want_rate) */
    if (rate != want_rate) {
        uint64_t rframes = 0;
        float* res = resample_mono(mono, frames, rate, want_rate, &rframes);
        free(mono);
        if (!res) { set_err(err, errcap, "sound: out of memory (resample)"); return false; }
        mono = res; frames = rframes;
    }

    if (frames > 0xFFFFFFFFu) { free(mono); set_err(err, errcap, "sound: too many frames (>4G; SoundData.frames is 32-bit)"); return false; }

    out->pcm = mono;
    out->frames = (uint32_t)frames;
    out->sample_rate = want_rate;                     /* now at the engine rate */
    out->channels = 1;
    out->order = 0;
    return true;
}

/* Resample an interleaved [n_in x ch] buffer to fout (each channel via resample_mono). New buffer +
 * *n_out frames; NULL on OOM. */
static float* resample_interleaved(const float* in, uint64_t n_in, unsigned int ch,
                                   uint32_t fin, uint32_t fout, uint64_t* n_out) {
    float* mono = (float*)malloc((size_t)n_in * sizeof(float));
    if (!mono) return NULL;
    float* outbuf = NULL; uint64_t no = 0;
    for (unsigned int k = 0; k < ch; ++k) {
        for (uint64_t i = 0; i < n_in; ++i) mono[i] = in[i * ch + k];
        uint64_t nk = 0;
        float* rk = resample_mono(mono, n_in, fin, fout, &nk);
        if (!rk) { free(mono); free(outbuf); return NULL; }
        if (!outbuf) {                                /* first channel sets the output length */
            no = nk;
            outbuf = (float*)malloc((size_t)no * ch * sizeof(float));
            if (!outbuf) { free(rk); free(mono); return NULL; }
        }
        uint64_t lim = nk < no ? nk : no;
        for (uint64_t i = 0; i < lim; ++i) outbuf[i * ch + k] = rk[i];
        for (uint64_t i = lim; i < no; ++i) outbuf[i * ch + k] = 0.f;   /* pad if a channel rounded short */
        free(rk);
    }
    free(mono);
    *n_out = no;
    return outbuf;
}

bool sound_load_ambix(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (!path) { set_err(err, errcap, "ambix: null path"); return false; }

    unsigned int channels = 0, rate = 0;
    uint64_t frames = 0;
    float* inter = decode_any(path, &channels, &rate, &frames);
    if (!inter)               { set_err(err, errcap, "ambix: cannot open/decode (wav/flac/mp3)"); return false; }
    if (frames == 0)          { free(inter); set_err(err, errcap, "ambix: empty file"); return false; }
    if (channels != 4 && channels != 9 && channels != 16) {
        free(inter); set_err(err, errcap, "ambix: channel count must be 4, 9, or 16 (1st/2nd/3rd order)"); return false;
    }
    uint16_t order = (channels == 4) ? 1 : (channels == 9) ? 2 : 3;

    if (rate != want_rate) {                          /* resample each SH channel to the engine rate */
        uint64_t nout = 0;
        float* res = resample_interleaved(inter, frames, channels, rate, want_rate, &nout);
        free(inter);
        if (!res) { set_err(err, errcap, "ambix: out of memory (resample)"); return false; }
        inter = res; frames = nout;
    }
    if (frames > 0xFFFFFFFFu) { free(inter); set_err(err, errcap, "ambix: too many frames (>4G)"); return false; }

    out->pcm = inter;
    out->frames = (uint32_t)frames;
    out->sample_rate = want_rate;
    out->channels = (uint16_t)channels;
    out->order = order;
    return true;
}

void sound_unload(SoundData* s) {
    if (!s) return;
    free(s->pcm);
    s->pcm = NULL;
    s->frames = 0;
    s->sample_rate = 0;
}
