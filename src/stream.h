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
#ifndef BW_STREAM_H
#define BW_STREAM_H

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

/* Stop + free a stream (control thread). Safe after the voice using it has stopped; blocks briefly to
 * ensure the streaming thread has released the decoder before the slot is reused. */
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

#endif /* BW_STREAM_H */
