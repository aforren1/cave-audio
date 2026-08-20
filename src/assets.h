/*
 * assets.h — the by-path asset cache behind bwa_sound_acquire / bwa_sound_release, plus the
 * loader thread behind bwa_sound_acquire_async. Control thread only. Not part of the public ABI.
 *
 * Why this exists: bwa_load_* has no dedup and no refcount, so EVERY client rebuilt the same
 * Dictionary<path, handle> (Unity's Engine.cs, the Godot node's load_sound + unload_sound_path
 * fan-out). The fan-out is the tell: one path can be resident under several keys (in RAM /
 * streamed / ambisonic), which is the engine's own multi-key problem leaked outward. The key here
 * is (normalized path, flags), so those are simply different entries.
 *
 * Ownership: this layer OWNS the handles it hands out. The explicit tier (bwa_load_* /
 * bwa_unload_sound) stays exactly as it was and owns its own; engine.c refuses a cross-tier
 * bwa_unload_sound rather than corrupting the refcount.
 *
 * Threading: every assets_* call is control thread. NOTHING here reaches the audio thread - the
 * cache allocates, does the file I/O, and hands rt.c the same SoundData the synchronous loaders
 * always handed it. The async path adds ONE worker thread that decodes; see the handoff comment
 * on assets_pump in assets.c for exactly where the buffer changes owners.
 */
#ifndef BWA_ASSETS_H
#define BWA_ASSETS_H

#include "rt.h"          /* RtCore (opaque) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AssetCache AssetCache;

/* Load-flag bits. Must match bwa_load_flags in include/bw_audio.h (that header is the contract;
 * this is the internal mirror so assets.c does not include the public header). */
#define BWA_AF_STREAM (1u << 0)
#define BWA_AF_AMBIX  (1u << 1)
#define BWA_AF_FUMA   (1u << 2)
#define BWA_AF_ALL    (BWA_AF_STREAM | BWA_AF_AMBIX | BWA_AF_FUMA)

/* `sound_cap` is the rt sound-table capacity: it bounds the number of live entries, so the hash
 * table and the unload-retry park are both sized from it once and never grow. `sample_rate` is the
 * engine rate the loader thread decodes to (rt's own rate is not exposed). NULL on OOM. */
AssetCache* assets_create(RtCore* rt, uint32_t sample_rate, uint32_t sound_cap);
/* Joins the loader thread (if it ever started), discards anything still in flight, and frees the
 * table. Does NOT unload the cached sounds - rt_destroy frees whatever is still resident, and
 * this is called from bwa_destroy right before it. */
void        assets_destroy(AssetCache* a);

/* Acquire (path, flags). Same key = same handle, refcount + 1. 0 + `err` on a rejected flag
 * combination, a decode failure, or a full sound table. */
uint32_t assets_acquire(AssetCache* a, const char* path, uint32_t flags, char* err, size_t errcap);
/* Same, but the decode runs on the loader thread. The handle is valid on return and playable at
 * once (rt holds the play until the data lands). Acquiring a path that is already resident is the
 * ordinary cache hit, async or not. */
uint32_t assets_acquire_async(AssetCache* a, const char* path, uint32_t flags, char* err, size_t errcap);
/* Has this handle's data landed? True for anything acquired synchronously, false for a handle the
 * cache does not own. A load that FAILED never becomes ready and reports its reason in `err`. */
bool assets_is_ready(AssetCache* a, uint32_t snd, char* err, size_t errcap);
/* Drop one reference; at zero the sound unloads through the ordinary retire-ack path (safe while
 * playing). Releasing an in-flight async load cancels it: the handle is abandoned immediately and
 * the worker's result is discarded when it arrives. Not-owned = false + `err`. */
bool assets_release(AssetCache* a, uint32_t snd, char* err, size_t errcap);
/* Is this handle cache-owned? engine.c asks before letting bwa_unload_sound through. */
bool assets_owns(AssetCache* a, uint32_t snd);
/* The asset KIND the entry's load flags fix: 1 = ambisonic bed (BWA_LOAD_AMBIX / BWA_LOAD_FUMA),
 * 0 = mono point source (every other flag combination), -1 = not cache-owned. This is the answer a
 * still-decoding handle cannot give as a channel count: an unpublished slot reports 0 channels, so
 * both of the ABI's kind guards pass it, but the flags were fixed at acquire time. engine.c uses
 * this to refuse a bed played as a point source (or the reverse) AT the play call. */
int  assets_kind(AssetCache* a, uint32_t snd);
/* The handle for (path, flags) if it is ALREADY resident, else 0. Pure lookup: no load, no I/O,
 * and no refcount change, which is the whole point. A binding whose public API is by path (Godot's
 * unload_sound_path / sound_get_frames / sound_get_channels) cannot probe with assets_acquire,
 * because a miss would LOAD - reinstating the hidden-decode bug those getters exist to avoid, and
 * decoding mono so a bed would report 1 channel. A failed entry reports 0 too: it holds no data to
 * answer about. */
uint32_t assets_find(AssetCache* a, const char* path, uint32_t flags);
/* Adopt finished loads and retry parked unloads. Cheap and allocation-free when nothing is in
 * flight; bwa_commit calls it every frame, and the acquire/release/is_ready calls call it too so a
 * client that never commits still makes progress. */
void assets_pump(AssetCache* a);

#endif /* BWA_ASSETS_H */
