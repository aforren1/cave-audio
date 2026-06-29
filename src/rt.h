/*
 * rt.h — the real-time core: the two SPSC rings, the voice table, the commit
 * snapshot, and generation-counted handles (docs/concurrency.md, docs/internal-types.md).
 * NOT part of the public ABI. engine.c owns one RtCore and forwards the bw_* API to it.
 *
 * Thread split (docs/CLAUDE.md): the CONTROL thread calls the rt_source_* / rt_set_listener
 * / rt_commit functions (enqueue only, plus handle allocation); the AUDIO thread calls
 * rt_render (drain + mix), which must not allocate/lock/block. They share state only
 * through the two rings and the staging->active snapshot.
 *
 * M2 scope: the rings/voice-table/commit/generation machinery is real and final. The
 * mixing is a placeholder — a generated test tone routed to a single position-derived
 * channel with a per-block gain ramp. M3 replaces the tone with wav playback (mix_voice
 * reading sound->pcm); M4 replaces the single-channel routing with the DBAP 26-gain solve.
 */
#ifndef BW_RT_H
#define BW_RT_H

#include "sink.h"          /* BwTimestamp, BW_CHANNELS */
#include "layout.h"        /* Layout (for rt_set_layout) */
#include "pose.h"          /* PoseSlot (for rt_set_tracker) */

#include <stdbool.h>
#include <stdint.h>

/* Handles are (index | generation<<16); 0 is invalid (see include/bwaudio.h). */
#define BW_H_IDX(h)   ((uint16_t)((h) & 0xFFFFu))
#define BW_H_GEN(h)   ((uint16_t)((h) >> 16))
#define BW_MK_H(i, g) ((uint32_t)(i) | ((uint32_t)(g) << 16))

/* Command ring payload (control -> audio). Fixed-size POD, no framing. */
enum {
    CMD_SRC_CREATE = 0, CMD_SRC_DESTROY, CMD_SET_POS, CMD_SET_GAIN,
    CMD_PLAY, CMD_STOP, CMD_SET_LISTENER, CMD_COMMIT, CMD_SOUND_RETIRE,
    CMD_SET_REFLECTIONS, CMD_TEST_SIGNAL, CMD_SET_DOPPLER, CMD_SET_AIR, CMD_SET_SPREAD
};
typedef struct {
    uint8_t  type;
    uint32_t handle;                 /* source or sound handle */
    union {
        struct { float x, y, z; }                      pos;
        struct { float g; }                            gain;
        struct { uint32_t sound; uint8_t loop, oneshot; } play;
        struct { float px, py, pz, qx, qy, qz, qw; }   lis;
        struct { uint8_t on; }                         refl;
        struct { uint8_t on; }                         dop;   /* per-voice Doppler enable */
        struct { uint8_t on; }                         air;   /* per-voice air absorption enable */
        struct { float amount; }                       spread;/* per-voice source angular width 0..1 */
        struct { uint32_t channel; uint8_t kind; float gain; } test;  /* debug channel injection */
    } u;
} Cmd;

/* Event ring payload (audio -> control). */
enum { EVT_VOICE_ENDED = 0, EVT_SOUND_RETIRED };
typedef struct { uint8_t type; uint32_t handle; } Evt;

typedef struct RtCore RtCore;   /* opaque */

/* ---- lifecycle (control thread; allocates) ---- */
RtCore* rt_create(uint32_t voice_cap, uint32_t sound_cap, uint32_t sample_rate, uint32_t channels);
void    rt_destroy(RtCore* c);
void    rt_set_layout(RtCore* c, const Layout* L);   /* call before bw_start / while stopped */
void    rt_set_panner(RtCore* c, int panner);        /* 0 = DBAP, 1 = SPCAP; call before bw_start */
void    rt_set_bed_decoder(RtCore* c, int decoder);  /* 0 = sampling (SAD), 1 = AllRAD; before bw_start */

/* Attach the internal tracker's pose slot (track_internal). When set, rt_render samples the
 * freshest head pose from it at block time, overriding the committed listener. NULL detaches.
 * Set before the audio thread starts / after it stops (the audio thread reads the pointer). */
void    rt_set_tracker(RtCore* c, const PoseSlot* slot);

/* Post-mix aux-send tap: rt_render calls it on the AUDIO thread AFTER the voice loop and BEFORE
 * align_process, handing it the 26-ch bus, the block size, the active listener pose, and the summed
 * mono aux send (the post-occlusion/directivity signal of voices opted in via rt_source_set_reflections).
 * A phonon-free seam — the reflection bed registers a tap that convolves `aux` and sums onto `bus`.
 * Set while the audio thread is stopped (the audio thread reads the pointer). NULL detaches. */
typedef void (*RtBusTap)(void* ud, float* bus, uint32_t n, const float* lp, const float* lq, const float* aux);
void    rt_set_bus_tap(RtCore* c, RtBusTap tap, void* ud);
void    rt_source_set_reflections(RtCore* c, uint32_t h, bool on);   /* gate this voice into the aux send */
/* Debug: drive output `channel` with a built-in test signal (kind 0=off/1=sine/2=noise), injected
 * AFTER the per-speaker align stage (raw channel). Control thread; takes effect next block. */
void    rt_test_signal(RtCore* c, uint32_t channel, uint8_t kind, float gain);

/* Publish a voice's occlusion transmittance (1 = clear .. 0 = blocked), applied to its mono signal
 * before the DBAP pan. Called from the off-thread occlusion sim (not the control thread). Stale/
 * recycled handles are dropped; the audio thread ramps to it (no zipper). */
void    rt_set_occlusion(RtCore* c, uint32_t handle, float transmittance);
/* Occlusion with a 3-band transmission tilt: broadband `level` + normalized `band_gains[3]` (the
 * audio thread applies a 3-biquad EQ). Same off-thread publisher + handle gate as rt_set_occlusion. */
void    rt_set_occlusion_eq(RtCore* c, uint32_t handle, float level, const float band_gains[3]);
/* Full direct-effect publish: broadband level + 3-band tilt + directivity gain, in one handle-gated
 * store set. The off-thread sim calls this per source; the audio thread ramps level/dir and EQ. */
void    rt_set_direct(RtCore* c, uint32_t handle, float level, const float bands[3], float dir);
float   rt_get_occlusion(RtCore* c, uint32_t handle);   /* control thread: published factor (1 = clear) */
float   rt_get_directivity(RtCore* c, uint32_t handle); /* control thread: published gain (1 = on-axis) */

/* ---- assets (control thread; file I/O + alloc) ---- */
uint32_t rt_load_sound  (RtCore* c, const char* path, char* err, size_t errcap); /* 0 on failure */
uint32_t rt_load_ambix  (RtCore* c, const char* path, char* err, size_t errcap); /* multichannel bed asset */
uint16_t rt_sound_channels(RtCore* c, uint32_t sound);   /* 1 = mono, 4/9/16 = bed, 0 = invalid */
void     rt_unload_sound(RtCore* c, uint32_t sound);  /* safe any time; retire-acked internally */

/* ---- control thread: handle allocation is synchronous, the rest enqueue ---- */
uint32_t rt_source_create(RtCore* c);                 /* 0 if the table is full */
void rt_source_destroy (RtCore* c, uint32_t h);
void rt_source_set_pos (RtCore* c, uint32_t h, float x, float y, float z);
void rt_source_set_gain(RtCore* c, uint32_t h, float linear);
void rt_source_play    (RtCore* c, uint32_t h, uint32_t sound, bool loop);
void rt_source_stop    (RtCore* c, uint32_t h);
void rt_source_set_doppler(RtCore* c, uint32_t h, bool on);          /* propagation: glided delay -> pitch from radial motion */
void rt_source_set_air_absorption(RtCore* c, uint32_t h, bool on);   /* propagation: distance-driven HF low-pass */
void rt_source_set_spread(RtCore* c, uint32_t h, float amount);      /* source angular width: 0 = point .. 1 = wide */
void rt_play_oneshot   (RtCore* c, uint32_t sound, float x, float y, float z, float gain);
void rt_set_listener   (RtCore* c, const float p[3], const float q[4]);
void rt_commit         (RtCore* c);                   /* enqueue CMD_COMMIT + drain events */

/* ---- audio thread: drain the command ring, then mix one block into `bus` ---- */
void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts);
void rt_get_listener(RtCore* c, float p[3], float q[4]);   /* audio thread: active pose */
void rt_read_pose(RtCore* c, float p[3], float q[4]);      /* control thread: active pose (seqlock readback) */
bool rt_source_is_playing(RtCore* c, uint32_t h);         /* control thread: is the source's voice still playing? */

#endif /* BW_RT_H */
