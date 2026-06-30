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

/* Decoded sound: interleaved float PCM. A point source is mono (channels == 1, downmixed at load);
 * an ambisonic BED keeps its AmbiX channels (channels == 4/9/16, order 1/2/3). Owned by the control
 * thread; the audio thread reads pcm only via a voice binding and only until the retire-ack frees it. */
struct Stream;   /* forward decl (stream.h); set for a streamed sound, owned by rt.c's StreamSet */

typedef struct {
    float*   pcm;            /* `frames` * `channels` interleaved samples; NULL when empty or streaming */
    uint32_t frames;
    uint32_t sample_rate;
    uint16_t channels;       /* 1 = mono point source; 4/9/16 = ambisonic bed */
    uint16_t order;          /* ambisonic order (0 for mono; 1/2/3 for a bed) */
    struct Stream* stream;   /* non-NULL = streamed from disk (mono); the audio thread reads its ring */
} SoundData;

/* Decode wav/flac/mp3 to MONO float (downmixing multi-channel), resampling to want_rate if the file
 * rate differs. Fails (false + message in `err`) on a missing/unreadable file, decode error, or empty
 * file. On success `out` owns pcm; free it with sound_unload. */
bool sound_load(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap);

/* Decode an AmbiX (ACN/SN3D) file KEEPING its channels (4/9/16 -> order 1/2/3), resampling per
 * channel to want_rate if needed. Rejects other channel counts. Same ownership as sound_load. */
bool sound_load_ambix(const char* path, uint32_t want_rate, SoundData* out, char* err, size_t errcap);

void sound_unload(SoundData* s);   /* frees pcm; safe on a zeroed/empty SoundData */

#endif /* BW_SOUND_H */
