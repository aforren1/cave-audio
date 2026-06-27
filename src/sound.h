/*
 * sound.h — wav decoding (stateless; control thread / load time only). The Sound table,
 * handle lifetime, and the retire-ack handshake live in rt.c — this file just turns a
 * file into mono float PCM. Not part of the public ABI.
 */
#ifndef BW_SOUND_H
#define BW_SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoded sound: mono float PCM. Point sources are mono; multi-channel files are
 * downmixed at load. Owned by the control thread; the audio thread reads pcm only via a
 * voice binding and only until the retire-ack frees it (docs/concurrency.md). */
typedef struct {
    float*   pcm;            /* `frames` mono samples; NULL when empty */
    uint32_t frames;
    uint32_t sample_rate;
} SoundData;

/* Decode a wav file to mono float. Fails (false + message in `err`) on a missing/
 * unreadable file, a decode error, an empty file, or a sample-rate != want_rate
 * (M3 does no resampling). On success `out` owns pcm; free it with sound_unload. */
bool sound_load(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap);

void sound_unload(SoundData* s);   /* frees pcm; safe on a zeroed/empty SoundData */

#endif /* BW_SOUND_H */
