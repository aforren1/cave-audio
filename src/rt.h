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
    CMD_PLAY, CMD_STOP, CMD_SET_LISTENER, CMD_COMMIT, CMD_SOUND_RETIRE
};
typedef struct {
    uint8_t  type;
    uint32_t handle;                 /* source or sound handle */
    union {
        struct { float x, y, z; }                      pos;
        struct { float g; }                            gain;
        struct { uint32_t sound; uint8_t loop, oneshot; } play;
        struct { float px, py, pz, qx, qy, qz, qw; }   lis;
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

/* Attach the internal tracker's pose slot (track_internal). When set, rt_render samples the
 * freshest head pose from it at block time, overriding the committed listener. NULL detaches.
 * Set before the audio thread starts / after it stops (the audio thread reads the pointer). */
void    rt_set_tracker(RtCore* c, const PoseSlot* slot);

/* Publish a voice's occlusion transmittance (1 = clear .. 0 = blocked), applied to its mono signal
 * before the DBAP pan. Called from the off-thread occlusion sim (not the control thread). Stale/
 * recycled handles are dropped; the audio thread ramps to it (no zipper). */
void    rt_set_occlusion(RtCore* c, uint32_t handle, float transmittance);

/* ---- assets (control thread; file I/O + alloc) ---- */
uint32_t rt_load_sound  (RtCore* c, const char* path, char* err, size_t errcap); /* 0 on failure */
void     rt_unload_sound(RtCore* c, uint32_t sound);  /* safe any time; retire-acked internally */

/* ---- control thread: handle allocation is synchronous, the rest enqueue ---- */
uint32_t rt_source_create(RtCore* c);                 /* 0 if the table is full */
void rt_source_destroy (RtCore* c, uint32_t h);
void rt_source_set_pos (RtCore* c, uint32_t h, float x, float y, float z);
void rt_source_set_gain(RtCore* c, uint32_t h, float linear);
void rt_source_play    (RtCore* c, uint32_t h, uint32_t sound, bool loop);
void rt_source_stop    (RtCore* c, uint32_t h);
void rt_play_oneshot   (RtCore* c, uint32_t sound, float x, float y, float z, float gain);
void rt_set_listener   (RtCore* c, const float p[3], const float q[4]);
void rt_commit         (RtCore* c);                   /* enqueue CMD_COMMIT + drain events */

/* ---- audio thread: drain the command ring, then mix one block into `bus` ---- */
void rt_render(RtCore* c, float* bus, uint32_t nframes, const BwTimestamp* ts);
void rt_get_listener(RtCore* c, float p[3], float q[4]);   /* audio thread: active pose */
void rt_read_pose(RtCore* c, float p[3], float q[4]);      /* control thread: active pose (seqlock readback) */

#endif /* BW_RT_H */
