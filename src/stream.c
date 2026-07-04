/* stream.c — see stream.h. Background-thread file streaming into per-stream SPSC rings. */
#include "stream.h"

#include "dr_wav.h"      /* implementations live in sound.c; here we include the headers only */
#include "dr_flac.h"
#include "dr_mp3.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STREAMS 16
#define CHUNK       4096        /* frames decoded per read */
#define RING_SIZE   65536       /* per-stream ring samples (~1.37 s @ 48k); power of two */
#define PREBUFFER   8192        /* samples to buffer before playback may begin */
#define EOF_NONE    UINT64_MAX  /* eof_w sentinel for "end not reached" (0 is a valid EOF position) */

enum { ST_FREE = 0, ST_IDLE, ST_RESTART, ST_ACTIVE, ST_CLOSING };
enum { DT_WAV = 0, DT_FLAC, DT_MP3 };

struct Stream {
    void*    dec;               /* drwav* / drflac* / drmp3*, owned by the streaming thread after start */
    int      dtype;
    uint32_t channels, file_rate;
    uint64_t total_frames;

    float*   ring;              /* RING_SIZE samples */
    uint32_t ring_mask;
    float*   inter;             /* CHUNK * channels interleaved decode scratch (streaming thread) */
    float*   mono;              /* CHUNK mono downmix (streaming thread) */

    _Atomic uint64_t r;         /* consumed position (audio thread writes, streaming thread reads) */
    _Atomic uint64_t w;         /* filled position (streaming thread writes, audio thread reads) */
    _Atomic uint64_t eof_w;     /* file-end position (non-loop; EOF_NONE = not reached / looping) */
    _Atomic int      state;
    _Atomic int      loop_req;  /* control thread sets the loop flag here; the thread adopts it on ST_RESTART */
    int      loop;              /* streaming-thread-private once active */
    uint64_t w_priv;            /* streaming-thread-private write cursor */
    int      done;              /* streaming-thread-private: reached EOF, stop filling */
};

struct StreamSet {
    uint32_t        rate;
    Stream*         slots[MAX_STREAMS];
    int             nslots;
    CRITICAL_SECTION lock;      /* guards the slot array (open/close vs the thread's snapshot) */
    HANDLE          thread;
    volatile LONG   stop;
};

/* ---- decoder (streaming thread, except dec_open which is control-thread at stream_open) ---- */

static int dec_open(Stream* s, const char* path) {
    const char* ext = strrchr(path, '.');
    if (ext && _stricmp(ext, ".flac") == 0) {
        drflac* f = drflac_open_file(path, NULL);
        if (!f) return 0;
        s->dec = f; s->dtype = DT_FLAC;
        s->channels = f->channels; s->file_rate = f->sampleRate; s->total_frames = f->totalPCMFrameCount;
    } else if (ext && _stricmp(ext, ".mp3") == 0) {
        drmp3* m = (drmp3*)malloc(sizeof *m);
        if (!m || !drmp3_init_file(m, path, NULL)) { free(m); return 0; }
        s->dec = m; s->dtype = DT_MP3;
        s->channels = m->channels; s->file_rate = m->sampleRate; s->total_frames = drmp3_get_pcm_frame_count(m);
    } else {
        drwav* w = (drwav*)malloc(sizeof *w);
        if (!w || !drwav_init_file(w, path, NULL)) { free(w); return 0; }
        s->dec = w; s->dtype = DT_WAV;
        s->channels = w->channels; s->file_rate = w->sampleRate; s->total_frames = w->totalPCMFrameCount;
    }
    return s->channels > 0;
}

static uint64_t dec_read(Stream* s, uint64_t frames) {   /* read into s->inter, downmix into s->mono; returns frames read */
    uint64_t got = 0;
    switch (s->dtype) {
    case DT_WAV:  got = drwav_read_pcm_frames_f32 ((drwav*) s->dec, frames, s->inter); break;
    case DT_FLAC: got = drflac_read_pcm_frames_f32((drflac*)s->dec, frames, s->inter); break;
    case DT_MP3:  got = drmp3_read_pcm_frames_f32 ((drmp3*) s->dec, frames, s->inter); break;
    }
    const uint32_t ch = s->channels;
    if (ch == 1) {
        memcpy(s->mono, s->inter, (size_t)got * sizeof(float));
    } else {
        const float inv = 1.0f / (float)ch;
        for (uint64_t i = 0; i < got; ++i) {
            float a = 0.f;
            for (uint32_t c = 0; c < ch; ++c) a += s->inter[i * ch + c];
            s->mono[i] = a * inv;
        }
    }
    return got;
}

static void dec_seek0(Stream* s) {
    switch (s->dtype) {
    case DT_WAV:  drwav_seek_to_pcm_frame ((drwav*) s->dec, 0); break;
    case DT_FLAC: drflac_seek_to_pcm_frame((drflac*)s->dec, 0); break;
    case DT_MP3:  drmp3_seek_to_pcm_frame ((drmp3*) s->dec, 0); break;
    }
}

static void dec_close(Stream* s) {
    if (!s->dec) return;
    switch (s->dtype) {
    case DT_WAV:  drwav_uninit((drwav*)s->dec); free(s->dec); break;
    case DT_FLAC: drflac_close((drflac*)s->dec);               break;
    case DT_MP3:  drmp3_uninit((drmp3*)s->dec); free(s->dec); break;
    }
    s->dec = NULL;
}

/* ---- streaming thread ---- */

static void fill(Stream* s) {
    uint64_t r = atomic_load_explicit(&s->r, memory_order_acquire);
    uint64_t w = s->w_priv;
    uint64_t target = r + s->ring_mask + 1;          /* keep the ring's worth ahead of the consumer */
    int seek_retries = 0;                            /* consecutive loop-seeks with no data read (spin guard) */
    while (w < target && !s->done) {
        uint64_t want = target - w; if (want > CHUNK) want = CHUNK;
        uint64_t got = dec_read(s, want);
        if (got > 0) seek_retries = 0;
        for (uint64_t k = 0; k < got; ++k) s->ring[(w + k) & s->ring_mask] = s->mono[k];
        w += got;
        atomic_store_explicit(&s->w, w, memory_order_release);   /* publish as we go, so the consumer can start */
        if (got < want) {                            /* end of file */
            if (s->loop) {
                if (++seek_retries > 1) {            /* two seeks yielding nothing: file is unreadable, don't spin */
                    atomic_store_explicit(&s->eof_w, w, memory_order_release); s->done = 1; break;
                }
                dec_seek0(s);
            } else { atomic_store_explicit(&s->eof_w, w, memory_order_release); s->done = 1; }
        }
    }
    s->w_priv = w;
}

static DWORD WINAPI stream_thread(LPVOID arg) {
    StreamSet* set = (StreamSet*)arg;
    while (!set->stop) {
        Stream* snap[MAX_STREAMS]; int n;
        EnterCriticalSection(&set->lock);
        n = set->nslots;
        for (int i = 0; i < n; ++i) snap[i] = set->slots[i];
        LeaveCriticalSection(&set->lock);

        for (int i = 0; i < n; ++i) {
            Stream* s = snap[i];
            if (!s) continue;
            int st = atomic_load_explicit(&s->state, memory_order_acquire);
            if (st == ST_CLOSING) { dec_close(s); atomic_store_explicit(&s->state, ST_FREE, memory_order_release); continue; }
            if (st == ST_RESTART) {
                /* the streaming thread owns ALL the reset (the control thread only flips the state),
                 * so no streaming-private field (w_priv/done/loop) is written cross-thread and fill's
                 * trailing w_priv store can't republish a stale cursor over the reset. */
                s->loop = atomic_load_explicit(&s->loop_req, memory_order_relaxed);
                dec_seek0(s);
                s->w_priv = 0; s->done = 0;
                atomic_store_explicit(&s->r, 0, memory_order_release);       /* reset the consumer cursor too */
                atomic_store_explicit(&s->w, 0, memory_order_release);
                atomic_store_explicit(&s->eof_w, EOF_NONE, memory_order_release);
                atomic_store_explicit(&s->state, ST_ACTIVE, memory_order_release);
                st = ST_ACTIVE;
            }
            if (st == ST_ACTIVE && !s->done) fill(s);
        }
        Sleep(3);
    }
    return 0;
}

/* ---- API ---- */

StreamSet* stream_set_create(uint32_t engine_rate) {
    StreamSet* set = (StreamSet*)calloc(1, sizeof *set);
    if (!set) return NULL;
    set->rate = engine_rate;
    InitializeCriticalSection(&set->lock);
    set->thread = CreateThread(NULL, 0, stream_thread, set, 0, NULL);
    if (!set->thread) { DeleteCriticalSection(&set->lock); free(set); return NULL; }
    return set;
}

void stream_set_destroy(StreamSet* set) {
    if (!set) return;
    if (set->thread) { InterlockedExchange(&set->stop, 1); WaitForSingleObject(set->thread, INFINITE); CloseHandle(set->thread); }
    for (int i = 0; i < set->nslots; ++i) {              /* thread is gone: release any survivors directly */
        Stream* s = set->slots[i];
        if (!s) continue;
        dec_close(s);
        free(s->ring); free(s->inter); free(s->mono); free(s);
    }
    DeleteCriticalSection(&set->lock);
    free(set);
}

static void set_err(char* err, size_t cap, const char* msg) { if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; } }

Stream* stream_open(StreamSet* set, const char* path, char* err, size_t errcap) {
    if (!set || !path) { set_err(err, errcap, "stream: bad arguments"); return NULL; }
    Stream* s = (Stream*)calloc(1, sizeof *s);
    if (!s) { set_err(err, errcap, "stream: out of memory"); return NULL; }
    if (!dec_open(s, path))                 { set_err(err, errcap, "stream: cannot open/decode (wav/flac/mp3)"); free(s); return NULL; }
    if (s->file_rate != set->rate)          { set_err(err, errcap, "stream: file sample rate != engine rate (pre-convert or load in memory)"); dec_close(s); free(s); return NULL; }
    if (s->total_frames == 0)               { set_err(err, errcap, "stream: empty file"); dec_close(s); free(s); return NULL; }

    s->ring_mask = RING_SIZE - 1;
    s->ring  = (float*)malloc((size_t)RING_SIZE * sizeof(float));
    s->inter = (float*)malloc((size_t)CHUNK * s->channels * sizeof(float));
    s->mono  = (float*)malloc((size_t)CHUNK * sizeof(float));
    if (!s->ring || !s->inter || !s->mono) { set_err(err, errcap, "stream: ring alloc failed"); dec_close(s); free(s->ring); free(s->inter); free(s->mono); free(s); return NULL; }
    atomic_store_explicit(&s->eof_w, EOF_NONE, memory_order_release);   /* not "reached at position 0" (calloc gave 0) */
    atomic_store_explicit(&s->state, ST_IDLE, memory_order_release);

    EnterCriticalSection(&set->lock);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; ++i) if (!set->slots[i]) { slot = i; break; }
    if (slot >= 0) { set->slots[slot] = s; if (slot + 1 > set->nslots) set->nslots = slot + 1; }
    LeaveCriticalSection(&set->lock);
    if (slot < 0) { set_err(err, errcap, "stream: too many open streams"); dec_close(s); free(s->ring); free(s->inter); free(s->mono); free(s); return NULL; }
    return s;
}

void stream_close(StreamSet* set, Stream* s) {
    if (!set || !s) return;
    /* hand the decoder release to the streaming thread, then reclaim the slot + memory */
    atomic_store_explicit(&s->state, ST_CLOSING, memory_order_release);
    for (int spins = 0; atomic_load_explicit(&s->state, memory_order_acquire) != ST_FREE; ++spins) {
        Sleep(1);
        if (spins > 2000) { dec_close(s); break; }   /* thread wedged: release here as a last resort */
    }
    EnterCriticalSection(&set->lock);
    for (int i = 0; i < set->nslots; ++i) if (set->slots[i] == s) set->slots[i] = NULL;
    LeaveCriticalSection(&set->lock);
    free(s->ring); free(s->inter); free(s->mono); free(s);
}

void stream_start(Stream* s, int loop) {
    if (!s) return;
    /* Control thread: publish the loop flag, then hand the WHOLE reset (seek + r/w/eof_w/w_priv/done)
     * to the streaming thread via ST_RESTART. Writing the streaming-private fields here would race
     * a concurrent fill(). */
    atomic_store_explicit(&s->loop_req, loop, memory_order_relaxed);
    atomic_store_explicit(&s->state, ST_RESTART, memory_order_release);
}

int stream_prebuffered(const Stream* s) {
    if (!s) return 0;
    uint64_t w = atomic_load_explicit(&((Stream*)s)->w, memory_order_acquire);
    uint64_t e = atomic_load_explicit(&((Stream*)s)->eof_w, memory_order_acquire);
    return (w >= PREBUFFER) || (e != EOF_NONE);   /* enough buffered, or a short file already fully read */
}

uint32_t stream_pull(Stream* s, uint64_t pos, float* dst, uint32_t n) {
    uint64_t w = atomic_load_explicit(&s->w, memory_order_acquire);
    uint64_t avail = (w > pos) ? (w - pos) : 0;
    uint32_t got = (avail < (uint64_t)n) ? (uint32_t)avail : n;
    for (uint32_t k = 0; k < got; ++k) dst[k] = s->ring[(pos + k) & s->ring_mask];
    /* Publish the consumed cursor ONLY when we actually read. A zero-sample pull (a still-bound voice
     * whose stale pos is ahead of w after a restart) must NOT store r = pos, or it would poison the
     * streaming thread's refill target with a position the ring never filled. */
    if (got > 0) atomic_store_explicit(&s->r, pos + got, memory_order_release);
    return got;
}

int stream_ended(const Stream* s, uint64_t pos) {
    uint64_t e = atomic_load_explicit(&((Stream*)s)->eof_w, memory_order_acquire);
    return e != EOF_NONE && pos >= e;
}
