/* stream.c — see stream.h. Background-thread file streaming into per-stream SPSC rings. */
#include "core/stream.h"
#include "core/rt.h"          /* BWA_MAX_SAMPLE: the outside-sample cap (dec_read / stream_push) */

#include "dr_wav.h"      /* implementations live in sound.c; here we include the headers only */
#include "dr_flac.h"
#include "dr_mp3.h"

#include "os/os.h"
#include <math.h>
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
    struct StreamSet* set;      /* owner, so stream_start can wake the refill thread (control thread) */
    void*    dec;               /* drwav* / drflac* / drmp3*, owned by the streaming thread after start */
    int      dtype;
    int      push;              /* 1 = push stream: no decoder, the CONTROL thread is the producer */
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
    os_mutex    lock;           /* guards the slot array (open/close vs the thread's snapshot) */
    os_thread   thread;
    os_event    wake;           /* control -> refill thread: a stream opened, started, closed, or stopped */
    _Atomic int stop;
};

/* ---- decoder (streaming thread, except dec_open which is control-thread at stream_open) ---- */

/* The `_w` openers on Windows: a narrow path reaches the CRT in the process ANSI codepage, so a
 * UTF-8 path with a non-ASCII character never opens there (docs/backends.md, "UTF-8 file paths").
 * All three decoders publish a wide OPEN, which is all streaming needs - only sound.c's whole-file
 * read has to go deeper. Off Windows the bytes already mean what they say. */
static int dec_open(Stream* s, const char* path) {
    const char* ext = strrchr(path, '.');
#if defined(_WIN32)
    wchar_t wpath[OS_PATH_WIDE_MAX];
    if (os_utf8_to_wide(path, wpath, OS_PATH_WIDE_MAX) != 0) return 0;
    #define BWA_FLAC_OPEN()  drflac_open_file_w(wpath, NULL)
    #define BWA_MP3_INIT(m)  drmp3_init_file_w((m), wpath, NULL)
    #define BWA_WAV_INIT(w)  drwav_init_file_w((w), wpath, NULL)
#else
    #define BWA_FLAC_OPEN()  drflac_open_file(path, NULL)
    #define BWA_MP3_INIT(m)  drmp3_init_file((m), path, NULL)
    #define BWA_WAV_INIT(w)  drwav_init_file((w), path, NULL)
#endif
    if (ext && os_strcasecmp(ext, ".flac") == 0) {
        drflac* f = BWA_FLAC_OPEN();
        if (!f) return 0;
        s->dec = f; s->dtype = DT_FLAC;
        s->channels = f->channels; s->file_rate = f->sampleRate; s->total_frames = f->totalPCMFrameCount;
    } else if (ext && os_strcasecmp(ext, ".mp3") == 0) {
        drmp3* m = (drmp3*)malloc(sizeof *m);
        if (!m || !BWA_MP3_INIT(m)) { free(m); return 0; }
        s->dec = m; s->dtype = DT_MP3;
        s->channels = m->channels; s->file_rate = m->sampleRate; s->total_frames = drmp3_get_pcm_frame_count(m);
    } else {
        drwav* w = (drwav*)malloc(sizeof *w);
        if (!w || !BWA_WAV_INIT(w)) { free(w); return 0; }
        s->dec = w; s->dtype = DT_WAV;
        s->channels = w->channels; s->file_rate = w->sampleRate; s->total_frames = w->totalPCMFrameCount;
    }
    #undef BWA_FLAC_OPEN
    #undef BWA_MP3_INIT
    #undef BWA_WAV_INIT
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
    /* scrub before the ring: an IEEE-float file delivers its bit patterns verbatim, and this is
     * the last stop before the audio thread — non-finite becomes 0 (the stream_push contract),
     * finite-but-absurd is capped (BWA_MAX_SAMPLE, rt.h: the align/room-EQ state sits before the
     * limiter, so one 3e38 sample would poison it for the session) */
    for (uint64_t i = 0; i < got; ++i) {
        float x = s->mono[i];
        if (!isfinite(x)) x = 0.f;
        else if (x >  BWA_MAX_SAMPLE) x =  BWA_MAX_SAMPLE;
        else if (x < -BWA_MAX_SAMPLE) x = -BWA_MAX_SAMPLE;
        s->mono[i] = x;
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

/* Tear a closed stream down: decoder, slot registration, memory. Runs on the STREAMING thread while
 * the set is live — its pass snapshot may still hold the pointer, so only it may free — and on the
 * control thread solely from stream_set_destroy, after the thread has been joined. The slot is
 * cleared under the lock BEFORE the free, so no later snapshot can capture a dangling pointer. */
static void stream_reap(StreamSet* set, Stream* s) {
    dec_close(s);
    os_mutex_lock(&set->lock);
    for (int i = 0; i < set->nslots; ++i) if (set->slots[i] == s) set->slots[i] = NULL;
    os_mutex_unlock(&set->lock);
    free(s->ring); free(s->inter); free(s->mono); free(s);
}

/* TEST HOOK (see stream.h): passes of the refill loop, process-wide. It used to be a flat 3 ms
 * sleep-poll, so an engine with nothing streaming still woke ~330 times a second. */
static _Atomic uint64_t g_stream_wakeups;

uint64_t bwa_stream_thread_wakeups(void) {
    return atomic_load_explicit(&g_stream_wakeups, memory_order_relaxed);
}

/* How long the refill thread may sleep before it MUST look again.
 *
 * The audio thread is the consumer (stream_pull), and it may not signal anything - no syscalls on
 * the audio thread, concurrency.md invariant 1 - so an active stream cannot be purely event-driven.
 * The bound instead comes from the rings: the shortest time any fillable ring could drain to empty
 * at the engine rate. Floored at 1 ms (a ring that just reset is empty and wants refilling NOW) and
 * capped at STREAM_POLL_MS, the old flat cadence, so an active stream is never served later than it
 * was before this became an event wait.
 *
 * NO fillable stream returns -1: wait forever. That is the whole saving. A push stream is fed by
 * the control thread and a finished ("done") one has nothing left to read, so neither counts; both
 * still get reaped on close, which signals. */
#define STREAM_POLL_MS 3

static int refill_timeout_ms(const StreamSet* set, Stream* const* snap, int n) {
    uint64_t least = UINT64_MAX;                     /* samples of headroom on the hungriest ring */
    for (int i = 0; i < n; ++i) {
        Stream* s = snap[i];
        if (!s || s->push || s->done) continue;
        if (atomic_load_explicit(&s->state, memory_order_acquire) != ST_ACTIVE) continue;
        const uint64_t r = atomic_load_explicit(&s->r, memory_order_acquire);
        const uint64_t buffered = (s->w_priv > r) ? (s->w_priv - r) : 0;
        if (buffered < least) least = buffered;
    }
    if (least == UINT64_MAX) return -1;              /* nothing to fill: sleep until something changes */
    const uint64_t ms = (least * 1000ull) / (set->rate ? set->rate : 1u);
    if (ms < 1) return 1;
    return (ms > STREAM_POLL_MS) ? STREAM_POLL_MS : (int)ms;
}

static void stream_thread(void* arg) {
    StreamSet* set = (StreamSet*)arg;
    while (!atomic_load_explicit(&set->stop, memory_order_acquire)) {
        atomic_fetch_add_explicit(&g_stream_wakeups, 1, memory_order_relaxed);
        Stream* snap[MAX_STREAMS]; int n;
        os_mutex_lock(&set->lock);
        n = set->nslots;
        for (int i = 0; i < n; ++i) snap[i] = set->slots[i];
        os_mutex_unlock(&set->lock);

        for (int i = 0; i < n; ++i) {
            Stream* s = snap[i];
            if (!s) continue;
            int st = atomic_load_explicit(&s->state, memory_order_acquire);
            if (st == ST_CLOSING) { stream_reap(set, s); snap[i] = NULL; continue; }
            if (s->push) continue;   /* caller-fed: nothing to fill, and touching w_priv here would
                                      * race the control-thread producer (close is handled above) */
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
        /* Timeout AFTER the pass, so it reads the ring depths this pass just restored. The reaped
         * slots were cleared from `snap` above: refill_timeout_ms must never touch a freed Stream. */
        os_event_wait(&set->wake, refill_timeout_ms(set, snap, n));
    }
}

/* ---- API ---- */

StreamSet* stream_set_create(uint32_t engine_rate) {
    StreamSet* set = (StreamSet*)calloc(1, sizeof *set);
    if (!set) return NULL;
    set->rate = engine_rate;
    if (os_mutex_init(&set->lock) != 0) { free(set); return NULL; }
    if (os_event_init(&set->wake) != 0) { os_mutex_destroy(&set->lock); free(set); return NULL; }
    if (os_thread_create(&set->thread, stream_thread, set) != 0) {
        os_event_destroy(&set->wake); os_mutex_destroy(&set->lock); free(set); return NULL;
    }
    return set;
}

void stream_set_destroy(StreamSet* set) {
    if (!set) return;
    atomic_store_explicit(&set->stop, 1, memory_order_release);
    os_event_signal(&set->wake);                         /* it may be waiting forever with no stream open */
    os_thread_join(&set->thread);
    for (int i = 0; i < set->nslots; ++i) {              /* thread is gone: reap any survivors directly
                                                          * (including closes the thread hadn't reached) */
        Stream* s = set->slots[i];
        if (s) stream_reap(set, s);
    }
    os_event_destroy(&set->wake);
    os_mutex_destroy(&set->lock);
    free(set);
}

static void set_err(char* err, size_t cap, const char* msg) { if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; } }

/* register a stream in the set's slot array; 0 = no slot free */
static int reg_slot(StreamSet* set, Stream* s) {
    os_mutex_lock(&set->lock);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; ++i) if (!set->slots[i]) { slot = i; break; }
    if (slot >= 0) { set->slots[slot] = s; if (slot + 1 > set->nslots) set->nslots = slot + 1; }
    os_mutex_unlock(&set->lock);
    return slot >= 0;
}

Stream* stream_open(StreamSet* set, const char* path, char* err, size_t errcap) {
    if (!set || !path) { set_err(err, errcap, "stream: bad arguments"); return NULL; }
    Stream* s = (Stream*)calloc(1, sizeof *s);
    if (!s) { set_err(err, errcap, "stream: out of memory"); return NULL; }
    s->set = set;
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

    if (!reg_slot(set, s)) { set_err(err, errcap, "stream: too many open streams"); dec_close(s); free(s->ring); free(s->inter); free(s->mono); free(s); return NULL; }
    os_event_signal(&set->wake);        /* the refill thread may be parked with nothing open */
    return s;
}

/* A push stream: no decoder — the caller produces. Born ST_ACTIVE (the audio thread may pull as soon
 * as a voice binds it; an empty ring is an underrun, which renders silence, never an end). */
Stream* stream_open_push(StreamSet* set, char* err, size_t errcap) {
    if (!set) { set_err(err, errcap, "stream: bad arguments"); return NULL; }
    Stream* s = (Stream*)calloc(1, sizeof *s);
    if (!s) { set_err(err, errcap, "stream: out of memory"); return NULL; }
    s->set = set; s->push = 1; s->channels = 1; s->file_rate = set->rate;
    s->ring_mask = RING_SIZE - 1;
    s->ring = (float*)malloc((size_t)RING_SIZE * sizeof(float));   /* no inter/mono: nothing decodes */
    if (!s->ring) { set_err(err, errcap, "stream: ring alloc failed"); free(s); return NULL; }
    atomic_store_explicit(&s->eof_w, EOF_NONE, memory_order_release);
    atomic_store_explicit(&s->state, ST_ACTIVE, memory_order_release);
    if (!reg_slot(set, s)) { set_err(err, errcap, "stream: too many open streams"); free(s->ring); free(s); return NULL; }
    os_event_signal(&set->wake);
    return s;
}

uint32_t stream_push_space(Stream* s) {
    if (!s || !s->push) return 0;
    if (atomic_load_explicit(&s->eof_w, memory_order_relaxed) != EOF_NONE) return 0;   /* ended: refuse */
    uint64_t r = atomic_load_explicit(&s->r, memory_order_acquire);
    return (uint32_t)((uint64_t)s->ring_mask + 1u - (s->w_priv - r));
}

uint32_t stream_push(Stream* s, const float* src, uint32_t n) {
    if (!s || !src) return 0;
    uint32_t space = stream_push_space(s);           /* the ONE ring-occupancy formula (+ push/ended gates) */
    uint32_t put = n < space ? n : space;
    if (!put) return 0;
    uint64_t w = s->w_priv;                          /* producer-private cursor (control thread here) */
    for (uint32_t k = 0; k < put; ++k) {
        float x = src[k];
        if (!isfinite(x)) x = 0.f;                       /* nothing may hand NaN to the audio thread */
        else if (x >  BWA_MAX_SAMPLE) x =  BWA_MAX_SAMPLE;   /* finite is not enough: 3e38 overflows the
                                                              * bus into pre-limiter filter state (rt.h) */
        else if (x < -BWA_MAX_SAMPLE) x = -BWA_MAX_SAMPLE;
        s->ring[(w + k) & s->ring_mask] = x;
    }
    s->w_priv = w + put;
    atomic_store_explicit(&s->w, s->w_priv, memory_order_release);
    return put;
}

void stream_push_end(Stream* s) {
    if (!s || !s->push) return;
    if (atomic_load_explicit(&s->eof_w, memory_order_relaxed) != EOF_NONE) return;   /* idempotent */
    atomic_store_explicit(&s->eof_w, s->w_priv, memory_order_release);
}

void stream_close(StreamSet* set, Stream* s) {
    if (!set || !s) return;
    /* NON-BLOCKING by design: this runs inside drain_events, on the per-frame bwa_commit path, and
     * must never wait on the streaming thread's ~3 ms cadence (or its in-progress disk reads). The
     * whole teardown is handed to the streaming thread (stream_reap on its next pass) — it may still
     * hold this pointer in its current slot snapshot, so it is also the only thread that may free it
     * (the old spin-until-freed close both blocked AND raced that snapshot). Do not touch the
     * pointer after this call; the slot stays occupied until the reap (~ms). */
    atomic_store_explicit(&s->state, ST_CLOSING, memory_order_release);
    os_event_signal(&set->wake);        /* AFTER the state store: the thread wakes to a reapable slot */
}

void stream_start(Stream* s, int loop) {
    if (!s || s->push) return;   /* push streams are born active and cannot restart (no decoder to seek) */
    /* Control thread: publish the loop flag, then hand the WHOLE reset (seek + r/w/eof_w/w_priv/done)
     * to the streaming thread via ST_RESTART. Writing the streaming-private fields here would race
     * a concurrent fill(). */
    atomic_store_explicit(&s->loop_req, loop, memory_order_relaxed);
    atomic_store_explicit(&s->state, ST_RESTART, memory_order_release);
    if (s->set) os_event_signal(&s->set->wake);   /* the seek + first fill start NOW, not at the next
                                                   * poll: this is the prebuffer the caller waits on */
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

uint64_t stream_total_frames(const Stream* s) {
    return s ? s->total_frames : 0;      /* fixed at open (0 for push streams); safe from any thread */
}
