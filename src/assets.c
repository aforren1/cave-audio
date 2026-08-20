/*
 * assets.c — see assets.h. The by-path asset cache (bwa_sound_acquire / bwa_sound_release) plus
 * the one loader thread behind bwa_sound_acquire_async.
 *
 * Two pieces:
 *   1. An open-addressed table keyed by (normalized path, flags) -> {rt sound handle, refcount}.
 *      Everything here runs on the control thread and calls exactly the loaders the explicit tier
 *      calls, so the audio thread sees nothing it has not always seen.
 *   2. A loader thread with a job ring (control -> worker) and a result ring (worker -> control),
 *      both SPSC and both following rt.c's rings. The OWNERSHIP HANDOFF is the point of the
 *      design: the worker mallocs the PCM and owns it until its release-store into the result
 *      ring; from the matching acquire-load in assets_pump the CONTROL thread owns it, and it is
 *      the control thread that hands it to the sound table (rt_sound_publish) and the control
 *      thread that frees it (the retire-ack path, as always). No other thread ever holds it, and
 *      the audio thread only reaches it through the ordinary CMD_PLAY release the publish issues.
 *      Invariant 3 (control thread owns handle allocation and asset memory) is unbroken: the
 *      handle is allocated on the control thread at acquire time, and the worker only fills a
 *      buffer it alone can see.
 */
#include "assets.h"
#include "sound.h"
#include "bits.h"          /* bwa_pow2_ge */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define AJOB_CAP 64u                 /* in-flight async loads; power of two (both rings) */
#define TOMBSTONE ((char*)(uintptr_t)1)   /* erased table slot: probing continues past it */

typedef struct {
    char*    key;            /* normalized path (malloc'd); NULL = empty, TOMBSTONE = erased */
    char*    errmsg;         /* decode failure reason (malloc'd); NULL unless `failed` */
    uint32_t hash, flags;
    uint32_t snd;            /* rt sound handle this entry owns */
    uint32_t refs;
    uint8_t  loading;        /* async decode in flight (the rt slot is reserved) */
    uint8_t  failed;         /* async decode failed: never becomes ready, release it */
} AssetEntry;

typedef struct { char* path; uint32_t flags, snd; } AJob;
typedef struct { uint32_t snd; int ok; SoundData data; char err[160]; } ARes;

typedef struct { _Atomic uint32_t r, w; AJob  s[AJOB_CAP]; } JobRing;
typedef struct { _Atomic uint32_t r, w; ARes  s[AJOB_CAP]; } ResRing;

struct AssetCache {
    RtCore*  rt;
    uint32_t rate, sound_cap;

    AssetEntry* tab;         /* open-addressed, `cap` slots (power of two, >= 2*sound_cap) */
    uint32_t    cap, live, used;   /* used = live + tombstones (drives the rehash) */
    uint32_t*   by_idx;      /* BWA_H_IDX(snd) -> table slot + 1 (0 = none): O(1) handle lookup */

    uint32_t* park;          /* unloads whose CMD_SOUND_RETIRE hit a full command ring; retried in pump */
    uint32_t  park_n;

    JobRing  jobs;
    ResRing  res;
    uint32_t inflight;       /* control-side count of jobs enqueued whose result is not drained yet.
                              * Capped at AJOB_CAP, which is also the result ring's size, so the
                              * worker's result push can never fail and never has to block. */
    HANDLE        thread;    /* started lazily on the first async acquire */
    volatile LONG stop;
};

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Path normalization: ASCII case fold + backslash to forward slash. Deliberately nothing else -
 * no ".." collapse, no symlink or short-name resolution, no locale folding (which would mangle
 * UTF-8 bytes). Two spellings of one file that differ only in case or slash direction are the
 * cases that actually show up in a project's asset strings. */
static char* norm_path(const char* p) {
    const size_t n = strlen(p);
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; ++i) {
        char ch = p[i];
        if (ch == '\\') ch = '/';
        else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        s[i] = ch;
    }
    s[n] = 0;
    return s;
}

static uint32_t hash_key(const char* key, uint32_t flags) {
    uint32_t h = 2166136261u;                    /* FNV-1a over the path, then the flags */
    for (const char* p = key; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= flags; h *= 16777619u;
    if (h == 0) h = 1;
    return h;
}

/* ---- table ---- */

static void by_idx_set(AssetCache* a, uint32_t snd, uint32_t slot_plus1) {
    const uint32_t i = BWA_H_IDX(snd);           /* the sound table is index-addressed */
    if (i < a->sound_cap) a->by_idx[i] = slot_plus1;
}

static AssetEntry* entry_for_handle(AssetCache* a, uint32_t snd) {
    const uint32_t i = BWA_H_IDX(snd);
    if (!snd || i >= a->sound_cap || !a->by_idx[i]) return NULL;
    AssetEntry* en = &a->tab[a->by_idx[i] - 1];
    /* generation-checked: a recycled slot index may now belong to a different sound entirely */
    return (en->key && en->key != TOMBSTONE && en->snd == snd) ? en : NULL;
}

/* Rebuild the table, dropping tombstones. Live entries are bounded by sound_cap = cap/2, so the
 * fresh table always has room and this can never fail to place an entry. */
static void rehash(AssetCache* a) {
    AssetEntry* nt = (AssetEntry*)calloc(a->cap, sizeof(AssetEntry));
    if (!nt) return;                             /* out of memory: keep limping on the old table */
    const uint32_t mask = a->cap - 1;
    for (uint32_t i = 0; i < a->cap; ++i) {
        AssetEntry* en = &a->tab[i];
        if (!en->key || en->key == TOMBSTONE) continue;
        uint32_t j = en->hash & mask;
        while (nt[j].key) j = (j + 1) & mask;
        nt[j] = *en;
        by_idx_set(a, en->snd, j + 1);
    }
    free(a->tab);
    a->tab  = nt;
    a->used = a->live;
}

static AssetEntry* table_find(AssetCache* a, const char* key, uint32_t flags, uint32_t h) {
    const uint32_t mask = a->cap - 1;
    for (uint32_t i = 0, j = h & mask; i < a->cap; ++i, j = (j + 1) & mask) {
        AssetEntry* en = &a->tab[j];
        if (!en->key) return NULL;               /* empty slot ends the probe */
        if (en->key != TOMBSTONE && en->hash == h && en->flags == flags && strcmp(en->key, key) == 0)
            return en;
    }
    return NULL;
}

/* Insert `key` (ownership moves to the table on success). Caller has already established the key
 * is absent. NULL only if the table is somehow full, which the rehash bound rules out. */
static AssetEntry* table_insert(AssetCache* a, char* key, uint32_t flags, uint32_t h) {
    if ((a->used + 1) * 4u >= a->cap * 3u) rehash(a);
    const uint32_t mask = a->cap - 1;
    for (uint32_t i = 0, j = h & mask; i < a->cap; ++i, j = (j + 1) & mask) {
        AssetEntry* en = &a->tab[j];
        if (en->key && en->key != TOMBSTONE) continue;
        if (!en->key) ++a->used;                 /* a tombstone was already counted in `used` */
        memset(en, 0, sizeof *en);
        en->key = key; en->flags = flags; en->hash = h;
        ++a->live;
        return en;
    }
    return NULL;
}

static void table_erase(AssetCache* a, AssetEntry* en) {
    by_idx_set(a, en->snd, 0);
    free(en->key);
    free(en->errmsg);
    memset(en, 0, sizeof *en);
    en->key = TOMBSTONE;                          /* `used` unchanged: a tombstone still occupies a slot */
    --a->live;
}

/* ---- rings (SPSC, rt.c's pattern: relaxed own index, acquire the peer's, release on publish) ---- */

static bool job_push(JobRing* q, const AJob* j) {
    const uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
    const uint32_t r = atomic_load_explicit(&q->r, memory_order_acquire);
    if (w - r >= AJOB_CAP) return false;
    q->s[w & (AJOB_CAP - 1)] = *j;
    atomic_store_explicit(&q->w, w + 1, memory_order_release);
    return true;
}
static bool job_pop(JobRing* q, AJob* out) {
    const uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
    const uint32_t w = atomic_load_explicit(&q->w, memory_order_acquire);
    if (r == w) return false;
    *out = q->s[r & (AJOB_CAP - 1)];
    atomic_store_explicit(&q->r, r + 1, memory_order_release);
    return true;
}
static bool res_push(ResRing* q, const ARes* v) {
    const uint32_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
    const uint32_t r = atomic_load_explicit(&q->r, memory_order_acquire);
    if (w - r >= AJOB_CAP) return false;
    q->s[w & (AJOB_CAP - 1)] = *v;
    atomic_store_explicit(&q->w, w + 1, memory_order_release);   /* publishes the decoded PCM too */
    return true;
}
static bool res_pop(ResRing* q, ARes* out) {
    const uint32_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
    const uint32_t w = atomic_load_explicit(&q->w, memory_order_acquire);
    if (r == w) return false;
    *out = q->s[r & (AJOB_CAP - 1)];
    atomic_store_explicit(&q->r, r + 1, memory_order_release);
    return true;
}

/* ---- flags ---- */

static bool flags_ok(uint32_t f, char* err, size_t errcap) {
    if (f & ~(uint32_t)BWA_AF_ALL) {
        set_err(err, errcap, "bwa_sound_acquire: unknown load flags (valid: BWA_LOAD_STREAM, BWA_LOAD_AMBIX, BWA_LOAD_FUMA)");
        return false;
    }
    if ((f & BWA_AF_AMBIX) && (f & BWA_AF_FUMA)) {
        set_err(err, errcap, "bwa_sound_acquire: BWA_LOAD_AMBIX and BWA_LOAD_FUMA are mutually exclusive - one file is in one format");
        return false;
    }
    if ((f & BWA_AF_STREAM) && (f & (BWA_AF_AMBIX | BWA_AF_FUMA))) {
        set_err(err, errcap, "bwa_sound_acquire: BWA_LOAD_STREAM cannot combine with BWA_LOAD_AMBIX or BWA_LOAD_FUMA - streaming is mono only");
        return false;
    }
    return true;
}

/* Decode off the control thread (loader thread; also used for a synchronous fall-back). Streaming
 * never reaches here: it has no decode to do off-thread, and its open touches the sound table. */
static bool decode_by_flags(const char* path, uint32_t flags, uint32_t rate,
                            SoundData* out, char* err, size_t errcap) {
    if (flags & BWA_AF_FUMA)  return sound_load_fuma (path, rate, out, err, errcap);
    if (flags & BWA_AF_AMBIX) return sound_load_ambix(path, rate, out, err, errcap);
    return sound_load(path, rate, out, err, errcap);
}

/* Control thread: the synchronous load, straight through rt's existing loaders so an acquired
 * asset is bit-identical to an explicitly loaded one. */
static uint32_t load_now(AssetCache* a, const char* path, uint32_t flags, char* err, size_t errcap) {
    if (flags & BWA_AF_STREAM) return rt_load_sound_streaming(a->rt, path, err, errcap);
    if (flags & BWA_AF_FUMA)   return rt_load_fuma (a->rt, path, err, errcap);
    if (flags & BWA_AF_AMBIX)  return rt_load_ambix(a->rt, path, err, errcap);
    return rt_load_sound(a->rt, path, err, errcap);
}

/* ---- loader thread ---- */

static DWORD WINAPI loader_thread(LPVOID arg) {
    AssetCache* a = (AssetCache*)arg;
    for (;;) {
        if (InterlockedCompareExchange(&a->stop, 0, 0)) break;   /* leftover jobs are reaped by destroy */
        AJob j;
        if (!job_pop(&a->jobs, &j)) { Sleep(2); continue; }      /* a loader has no latency budget */
        ARes r;
        memset(&r, 0, sizeof r);
        r.snd = j.snd;
        r.ok  = decode_by_flags(j.path, j.flags, a->rate, &r.data, r.err, sizeof r.err) ? 1 : 0;
        free(j.path);
        /* Cannot fail: inflight is capped at AJOB_CAP, which is this ring's size. Freeing on the
         * impossible branch anyway, because leaking a decoded file would be the silent failure. */
        if (!res_push(&a->res, &r) && r.ok) sound_unload(&r.data);
    }
    return 0;
}

/* ---- lifecycle ---- */

AssetCache* assets_create(RtCore* rt, uint32_t sample_rate, uint32_t sound_cap) {
    if (!rt || !sound_cap || !sample_rate) return NULL;
    AssetCache* a = (AssetCache*)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->rt = rt; a->rate = sample_rate; a->sound_cap = sound_cap;
    a->cap    = bwa_pow2_ge(sound_cap * 2u);     /* load factor <= 0.5 at the live-entry bound */
    a->tab    = (AssetEntry*)calloc(a->cap, sizeof(AssetEntry));
    a->by_idx = (uint32_t*)  calloc(sound_cap, sizeof(uint32_t));
    a->park   = (uint32_t*)  calloc(sound_cap, sizeof(uint32_t));
    if (!a->tab || !a->by_idx || !a->park) { free(a->tab); free(a->by_idx); free(a->park); free(a); return NULL; }
    return a;
}

void assets_destroy(AssetCache* a) {
    if (!a) return;
    if (a->thread) {                              /* join FIRST: nothing may touch the rings after */
        InterlockedExchange(&a->stop, 1);
        WaitForSingleObject(a->thread, INFINITE);
        CloseHandle(a->thread);
        a->thread = NULL;
    }
    AJob j;
    while (job_pop(&a->jobs, &j)) free(j.path);   /* never decoded */
    ARes r;
    while (res_pop(&a->res, &r)) if (r.ok) sound_unload(&r.data);   /* decoded, never adopted */
    for (uint32_t i = 0; i < a->cap; ++i) {
        if (a->tab[i].key && a->tab[i].key != TOMBSTONE) { free(a->tab[i].key); free(a->tab[i].errmsg); }
    }
    free(a->tab); free(a->by_idx); free(a->park);
    free(a);
}

/* ---- pump ---- */

void assets_pump(AssetCache* a) {
    if (!a) return;
    ARes r;
    while (res_pop(&a->res, &r)) {
        if (a->inflight) --a->inflight;
        AssetEntry* en = entry_for_handle(a, r.snd);
        if (!en || !en->loading) {                /* released mid-flight: the acquire was cancelled */
            if (r.ok) sound_unload(&r.data);
            continue;
        }
        en->loading = 0;
        if (!r.ok) {                              /* the slot stays reserved so the handle cannot be
                                                   * recycled under the caller; release frees it */
            en->failed = 1;
            en->errmsg = _strdup(r.err);
            continue;
        }
        /* Ownership of r.data moves to the sound table here (control thread). A false return means
         * the slot went stale, and then the buffer is still ours to free. */
        if (!rt_sound_publish(a->rt, en->snd, &r.data)) sound_unload(&r.data);
    }
    for (uint32_t i = 0; i < a->park_n; )         /* retry unloads the command ring refused */
        if (rt_unload_sound(a->rt, a->park[i])) a->park[i] = a->park[--a->park_n];
        else ++i;
}

/* ---- acquire / release ---- */

static uint32_t acquire_common(AssetCache* a, const char* path, uint32_t flags,
                               bool async, char* err, size_t errcap) {
    if (!a || !path || !path[0]) { set_err(err, errcap, "bwa_sound_acquire: null or empty path"); return 0; }
    if (!flags_ok(flags, err, errcap)) return 0;
    assets_pump(a);                                /* adopt anything that landed since the last call */

    char* key = norm_path(path);
    if (!key) { set_err(err, errcap, "bwa_sound_acquire: out of memory"); return 0; }
    const uint32_t h = hash_key(key, flags);

    AssetEntry* en = table_find(a, key, flags, h);
    if (en) {                                      /* cache hit: one more owner, same handle */
        free(key);
        if (en->failed) {
            set_err(err, errcap, en->errmsg ? en->errmsg : "bwa_sound_acquire: this asset failed to load");
            return 0;                              /* a failed entry is never reused: release it, then retry */
        }
        ++en->refs;
        return en->snd;
    }

    /* Async only pays when a worker can take it. Streaming already decodes off-thread, and its
     * open writes the sound table (control-thread state), so it loads synchronously either way. */
    const bool defer = async && !(flags & BWA_AF_STREAM) && a->inflight < AJOB_CAP;

    uint32_t snd = 0;
    if (defer) snd = rt_sound_reserve(a->rt, err, errcap);
    else       snd = load_now(a, path, flags, err, errcap);
    if (!snd) { free(key); return 0; }

    en = table_insert(a, key, flags, h);
    if (!en) {                                     /* cannot happen (the rehash bound); unwind anyway */
        free(key);
        if (defer) rt_sound_abandon(a->rt, snd); else rt_unload_sound(a->rt, snd);
        set_err(err, errcap, "bwa_sound_acquire: cache table full");
        return 0;
    }
    en->snd  = snd;
    en->refs = 1;
    by_idx_set(a, snd, (uint32_t)(en - a->tab) + 1u);

    if (defer) {
        if (!a->thread) {                          /* lazily started: a session that never loads
                                                    * asynchronously never pays for a thread */
            a->stop = 0;
            a->thread = CreateThread(NULL, 0, loader_thread, a, 0, NULL);
        }
        AJob j = { NULL, flags, snd };
        j.path = _strdup(path);                    /* raw path: the loader opens the file, not the key */
        if (a->thread && j.path && job_push(&a->jobs, &j)) {
            en->loading = 1;
            ++a->inflight;
        } else {                                   /* no worker: fall back to loading it right here,
                                                    * so the caller still gets a usable asset */
            free(j.path);
            SoundData d;
            char lerr[160] = { 0 };
            if (!decode_by_flags(path, flags, a->rate, &d, lerr, sizeof lerr)) {
                en->failed = 1;
                en->errmsg = _strdup(lerr[0] ? lerr : "bwa_sound_acquire_async: load failed");
            } else if (!rt_sound_publish(a->rt, snd, &d)) {
                sound_unload(&d);                  /* publish refused: the buffer is still ours */
                en->failed = 1;
                en->errmsg = _strdup("bwa_sound_acquire_async: the reserved sound slot went stale");
            }
        }
    }
    return snd;
}

uint32_t assets_acquire(AssetCache* a, const char* path, uint32_t flags, char* err, size_t errcap) {
    return acquire_common(a, path, flags, false, err, errcap);
}

uint32_t assets_acquire_async(AssetCache* a, const char* path, uint32_t flags, char* err, size_t errcap) {
    return acquire_common(a, path, flags, true, err, errcap);
}

bool assets_is_ready(AssetCache* a, uint32_t snd, char* err, size_t errcap) {
    if (!a) return false;
    assets_pump(a);
    AssetEntry* en = entry_for_handle(a, snd);
    if (!en) { set_err(err, errcap, "bwa_sound_is_ready: handle is not owned by the asset cache"); return false; }
    if (en->failed) {
        set_err(err, errcap, en->errmsg ? en->errmsg : "bwa_sound_is_ready: the load failed");
        return false;
    }
    if (en->loading) return false;                 /* still decoding: no error, just not yet */
    return true;
}

bool assets_owns(AssetCache* a, uint32_t snd) {
    return a && entry_for_handle(a, snd) != NULL;
}

int assets_kind(AssetCache* a, uint32_t snd) {
    if (!a) return -1;
    AssetEntry* en = entry_for_handle(a, snd);
    if (!en) return -1;
    /* The flags ARE the kind, decoded or not: sound_load downmixes anything to mono and
     * sound_load_ambix / sound_load_fuma refuse a file that is not 4, 9 or 16 channels. */
    return (en->flags & (BWA_AF_AMBIX | BWA_AF_FUMA)) ? 1 : 0;
}

uint32_t assets_find(AssetCache* a, const char* path, uint32_t flags) {
    if (!a || !path || !path[0] || (flags & ~BWA_AF_ALL)) return 0;
    assets_pump(a);                                /* an async load that just landed IS resident */

    char* key = norm_path(path);
    if (!key) return 0;
    AssetEntry* en = table_find(a, key, flags, hash_key(key, flags));
    free(key);
    /* A still-loading entry answers with its handle: it is resident and playable (rt holds the
     * play), and the caller asks readiness with bwa_sound_is_ready. A FAILED one answers 0 - it
     * holds no data, so every by-path question about it is unanswerable. */
    return (en && !en->failed) ? en->snd : 0;
}

bool assets_release(AssetCache* a, uint32_t snd, char* err, size_t errcap) {
    if (!a) return false;
    assets_pump(a);
    AssetEntry* en = entry_for_handle(a, snd);
    if (!en) {
        set_err(err, errcap, "bwa_sound_release: handle was not acquired from the cache (use bwa_unload_sound)");
        return false;
    }
    if (en->refs) --en->refs;
    if (en->refs) return true;                     /* other owners remain */

    if (en->loading || en->failed) {
        /* Still reserved: the audio thread has never seen the slot, so it is dropped outright. An
         * in-flight decode is NOT waited for - its result finds no entry in assets_pump and is
         * freed there, which is what "cancel" means here. */
        rt_sound_abandon(a->rt, en->snd);
    } else if (!rt_unload_sound(a->rt, en->snd)) {
        /* Command ring full: park the handle and retry from assets_pump. Nobody else holds it now,
         * so if we dropped it the buffer would stay resident for the engine's life. */
        if (a->park_n < a->sound_cap) a->park[a->park_n++] = en->snd;
    }
    table_erase(a, en);
    return true;
}
