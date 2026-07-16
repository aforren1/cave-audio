/*
 * stream.h — background file streaming for long sounds (music / ambience), so they don't decode the
 * whole file into RAM. A dedicated streaming thread decodes chunks from disk into a per-stream SPSC
 * ring; the audio thread pulls samples from the ring with no file I/O, decode, or locks (invariant 1).
 *
 * Ownership: the CONTROL thread opens/closes/starts streams. The STREAMING thread is the sole producer
 * of each ring (decode + write). The AUDIO thread is the sole consumer (stream_pull). The two ring
 * positions are atomics; everything else a stream needs is set before it goes active.
 *
 * v1 scope: mono (multi-channel files are downmixed) at the engine sample rate (a rate mismatch is
 * rejected at open — pre-convert, or load in memory which resamples). One stream plays one voice at a
 * time (restarting re-seeks). WAV / FLAC / MP3 via dr_libs, decoded incrementally.
 */
#ifndef BWA_STREAM_H
#define BWA_STREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct StreamSet StreamSet;   /* the streaming thread + a pool of stream slots */
typedef struct Stream    Stream;      /* one streaming playback (decoder + ring) */

/* Create the set + start the streaming thread. `engine_rate` is the rate streams must match. */
StreamSet* stream_set_create(uint32_t engine_rate);
void       stream_set_destroy(StreamSet* set);

/* Open a streaming sound: probe the file, require the engine rate, downmix to mono, allocate the ring.
 * Returns a registered (inactive) Stream, or NULL with a message in `err`. Control thread (does I/O). */
Stream*    stream_open(StreamSet* set, const char* path, char* err, size_t errcap);

/* Stop + free a stream (control thread). Safe after the voice using it has stopped. NON-BLOCKING:
 * the streaming thread performs the whole teardown (decoder, slot, memory) on its next pass — this
 * runs on the per-frame commit path and never waits on it. The pointer is dead after this call;
 * the slot stays occupied until the reap (~ms). */
void       stream_close(StreamSet* set, Stream* s);

/* (Re)start playback from the beginning; `loop` != 0 wraps at EOF. The streaming thread re-seeks and
 * pre-fills. Control thread. */
void       stream_start(Stream* s, int loop);

/* Control thread: has the ring buffered enough to begin playback without an immediate underrun? */
int        stream_prebuffered(const Stream* s);

/* Audio thread: copy up to `n` samples from absolute stream position `pos` into `dst`. Returns the
 * count actually available (< n on underrun or EOF; the untouched tail of `dst` is the caller's to
 * zero). Pure ring reads + atomics — no I/O, alloc, or locks. */
uint32_t   stream_pull(Stream* s, uint64_t pos, float* dst, uint32_t n);

/* Audio thread: for a non-looping stream, has playback reached the end of the file at `pos`?
 * (Distinguishes a real EOF from a transient underrun, so the voice ends only at true EOF.) */
int        stream_ended(const Stream* s, uint64_t pos);

/* File length in frames (engine rate — streams require it). Fixed at stream_open, so any thread
 * may read it. 0 for a push stream (open-ended: the producer decides when it ends). */
uint64_t   stream_total_frames(const Stream* s);

/* ---- push streams (procedural audio: the CALLER is the producer) ----
 * A push stream has no file or decoder: the control thread writes PCM into the ring (stream_push)
 * and the audio thread consumes it with the same stream_pull/stream_ended calls a file stream uses.
 * The streaming thread never touches a push stream (there is nothing to fill), so the ring stays
 * strictly SPSC: control produces, audio consumes. Born active — an empty ring is an underrun
 * (renders silence), never an end. stream_start does not apply (a push stream can't restart). */
Stream*    stream_open_push(StreamSet* set, char* err, size_t errcap);

/* Append up to `n` mono engine-rate samples; returns the count accepted (short/0 when the ring is
 * full or the stream was ended). Non-finite samples are written as 0 — nothing may hand NaN to the
 * audio thread. Control thread (the single producer). */
uint32_t   stream_push(Stream* s, const float* src, uint32_t n);

/* Samples stream_push would accept right now (ring free space; 0 after stream_push_end). */
uint32_t   stream_push_space(Stream* s);

/* Mark end-of-data: stream_ended() turns true once everything pushed has been consumed, and
 * further pushes are refused. Idempotent. Control thread. */
void       stream_push_end(Stream* s);

#endif /* BWA_STREAM_H */
