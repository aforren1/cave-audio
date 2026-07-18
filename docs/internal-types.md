# Internal types and helper contracts

These types are **internal** to `src/`—they are *not* part of the public ABI
and must **not** go in [`include/bw_audio.h`](../include/bw_audio.h).

This file pins the protocol fields that [`concurrency.md`](./concurrency.md)
reasons about and summarizes the per-subsystem field groups around them. It
tracks the *shape* of the structs, not every field; the full definitions live in
[`src/rt.c`](../src/rt.c). When a detail matters, read the struct.

The thread-ownership rules from [`concurrency.md`](./concurrency.md) apply: the
**audio thread** owns the DSP state (`Voice`, the bus, `Listener.*_active`);
the **control thread** owns handle allocation, the free-lists, generation
tables, and asset (`SoundSlot`) memory.

## Handles

`bwa_source` / `bwa_sound` are `uint32_t = (index | generation<<16)`: the
`BWA_H_IDX` / `BWA_H_GEN` / `BWA_MK_H` macros in [`src/rt.h`](../src/rt.h). Index
and generation are each 16-bit, so the voice and sound tables are each ≤ 65536
slots and generations wrap at 2¹⁶. The wrap is safe: a stale handle only has to
differ from the *current* occupant of the slot.

`rt_create` adds `BWA_FADE_RESERVE` (8) physical voice slots beyond the requested
pool, kept free so a stolen voice can fade out on its own slot (see
concurrency.md).

## DSP state (audio thread owns)

`Voice` (rt.c) is ~50 fields. The protocol core, the fields the concurrency
doc's snapshot logic manipulates:

```c
typedef struct {
    /* identity / lifecycle */
    uint16_t gen;                  /* generation of the handle currently in this slot */
    bool     active;               /* slot in use (created, not destroyed) */
    bool     playing;              /* currently advancing a sound */
    bool     loop;
    bool     dirty;                /* gains need recompute (pos/gain/listener changed) */

    /* playback */
    const SoundData* sound;        /* NULL when idle; set by CMD_PLAY, cleared on retire/stop/destroy */
    uint32_t cursor;               /* sample cursor into sound->pcm (in-memory sounds) */

    /* spatialization */
    float    pos_pending[3];       /* room space; written by CMD_SET_POS */
    float    pos_active[3];        /* promoted on CMD_COMMIT; the mixer reads only this */
    float    gain_user;            /* linear, from CMD_SET_GAIN (default 1.0) */
    float    gtarget[BWA_CHANNELS]; /* panner solve output (per-speaker target gains) */
    float    gcur[BWA_CHANNELS];    /* ramp state; mix_voice interpolates gcur -> gtarget */

    /* ... ~35 more fields; groups below ... */
} Voice;
```

`BWA_CHANNELS` (26) is defined in [`src/sink.h`](../src/sink.h) and is the
**capacity**, not the count. Every `[BWA_CHANNELS]` array in these structs—gain
vectors, the bed decode matrix, the meters—is sized to that capacity, but only
the first `RtCore.channels` entries are used. `channels` is the loaded layout's
speaker count (`Layout.count`, 4..26; 26 for the default grid), resolved in
`bwa_create` *before* `rt_create` and fixed for the engine's lifetime. It is what
`bwa_get_channel_count` reports. Loops over the bus must use `channels`, never
`BWA_CHANNELS`—the tail entries belong to no speaker.

The rest of the struct is per-subsystem DSP state, one group per feature. All of
it is audio-thread-only (ramp state, filter histories), which is why it lives
*inside* `Voice`:

- **scheduling / streaming**: `oneshot` (self-recycling voice),
  `stream_pos` (absolute position into a stream's ring),
  `start_sample` (dsp-sample for a scheduled `bwa_source_play_at`).
- **dual-band panning**: `gtarget_lo` / `gcur_lo` (amplitude-normalised LF
  gain set), `xover_lp` (crossover one-pole state), `dual_mix` (0↔1 A/B
  crossfade factor).
- **occlusion / directivity**: `occ_cur`, `dir_cur` ramp state plus the
  3-band transmission-EQ biquad state (`eqg_cur`, `eq_co`, DF-I histories,
  `eq_engaged`). The published *targets* live in `RtCore`'s atomic arrays, not
  here (see below).
- **reflection send**: `refl_send`, `refl_dist`, `refl_gain`, `refl_g_cur`
  (ramped effective send gain).
- **pathing**: `path_on`, `path_sh_cur` (ramped SH coefficients), and a
  second, structurally identical bending-loss EQ state (`path_eq*`).
- **propagation**: `air_on` / `air_a_cur` / `air_y1` (air-absorption
  low-pass), `dop_on` / `dop_init` / `dop_delay` / `dop_dtgt` / `dop_w`
  (Doppler fractional-delay line), `spread` (source angular width).
- **pause / seek**: `paused`, `pause_g` (the ramped gate), `stopping`
  (fade-out for stop/steal), `seek_pending` / `seek_pos`.

## Listener (audio thread owns active; control writes via ring)

```c
typedef struct {
    float p_pending[3], q_pending[4];   /* written by CMD_SET_LISTENER */
    float p_active[3],  q_active[4];    /* promoted on CMD_COMMIT */
} Listener;
```

The position drives the panners; the quaternion drives the binaural monitor and
is also handed to the reflection/pathing taps. With a tracker connected,
`rt_render` overwrites the active fields from the tracker's seqlock each block.

## Sound (control thread owns; audio thread only reads via const*)

A sound is two structs: the **payload** ([`src/sound.h`](../src/sound.h)) and
the **lifecycle wrapper** (rt.c; the sound table is a `SoundSlot[]`):

```c
/* sound.h - the payload */
typedef struct {
    float*   pcm;            /* frames * channels interleaved; NULL when empty or streaming */
    uint32_t frames;
    uint32_t sample_rate;
    uint16_t channels;       /* 1 = mono point source; 4/9/16 = ambisonic bed */
    uint16_t order;          /* ambisonic order (0 for mono; 1/2/3 for a bed) */
    struct Stream* stream;   /* non-NULL = streamed from disk (mono; see stream.h) */
} SoundData;

/* rt.c - the lifecycle wrapper */
typedef struct {
    SoundData data;
    uint16_t  gen;           /* generation for the sound handle */
    uint8_t   inuse;
    uint8_t   retiring;      /* unload requested; awaiting EVT_SOUND_RETIRED before free */
} SoundSlot;
```

Notes:

- `channels` does double duty: 1 is a mono point source, 4/9/16 is an AmbiX
  ambisonic bed (played by `mix_bed`, not the panner).
- Loads **resample to the engine rate at load time** (windowed-sinc in
  sound.c). Streams do not: a stream whose rate differs from the engine rate is
  rejected at open.
- The audio thread resolves a handle to `const SoundData*` via `sound_for`; the
  control thread resolves to the `SoundSlot` via `sound_slot_ctrl`.

## Layout (loaded once from `cave_layout.json`; read-only on the audio thread)

See [`layout-schema.md`](./layout-schema.md) for the file format. Replaceable
while the audio thread is stopped (`rt_set_layout`). Its `count` is the engine's
channel count: the loader accepts 4..`BWA_CHANNELS` speakers whose indices form a
complete `0..count-1` permutation, and a layout with fewer than `BWA_CHANNELS`
leaves the tail `speakers[]` entries at the default grid's values (harmless:
`count` gates every consumer). From [`src/layout.h`](../src/layout.h):

```c
typedef struct { float fc, gain_db, q; } RoomEqSection;   /* cut-only by schema */

typedef struct {
    float    pos[3];               /* room space, right-handed, meters */
    float    gain_lin;             /* per-speaker level trim (linear) */
    uint32_t delay_samples;        /* per-speaker arrival-time alignment */
    uint16_t eq_len;               /* correction-FIR length (0 = none) */
    float    eq[BWA_EQ_TAPS];       /* 512 taps max; minimum-phase speaker correction */
    uint8_t  room_eq_count;        /* LF modal cuts (static-listener installs only) */
    RoomEqSection room_eq[BWA_ROOM_EQ_MAX];   /* 8 sections max */
} Speaker;

typedef struct {
    Speaker  speakers[BWA_CHANNELS];   /* capacity; only `count` are real */
    uint32_t count;                /* 4..BWA_CHANNELS - and it IS the engine's channel count */
    float    ref[3];               /* nominal listening point = the array centroid */
    float    rolloff_r;            /* DBAP spatial blur (meters) */
    /* distance attenuation: atten = clamp((ref/max(d,ref))^rolloff, min_lin, 1) */
    float    atten_ref_m, atten_rolloff, atten_min_lin;
    uint32_t max_delay_samples;    /* max over speakers; sizes the delay lines */
} Layout;
```

## Engine (the opaque `bwa_engine`)

The engine state sits at two levels.

**`RtCore`** (rt.c, opaque behind [`src/rt.h`](../src/rt.h)) is the real-time
core: the two rings, the voice table + `Listener`, the `Layout` + `Aligner`, and
the control-side allocation state (`gen` / `inuse` / `priority` / `stealing` /
free-lists, plus the `SoundSlot` table). The whole `bwa_*` API forwards to it.

**`bwa_engine`** (engine.c) is the ABI-facing shell around it:

```c
struct bwa_engine {                /* abridged; see engine.c */
    bwa_desc    cfg;
    bwa_profile   profile;
    RtCore*     rt;              /* rings + voice/sound tables + mixer */
    Monitor*    monitor;         /* binaural/both: speaker bus -> stereo decode */
    Layout      layout;          /* effective geometry; layout.count IS the channel count */
    bwa_sink*     sink;            /* primary device (cave: layout.count ch / binaural: 2 ch) */
    bwa_sink*     sink_mon;        /* both: the monitor (2-ch) device */
    float*      scratch26;       /* binaural: array render before the monitor decode */
    float*      mon_buf[2];      /* both: stereo double-buffer, array -> monitor thread */
    NatNet*     tracker;         /* internal tracking (bwa_tracker_connect); NULL otherwise */
    SteamMonitor* steam;         /* production HRTF decode; NULL = simple-pan fallback */
    SteamScene* scene;  SteamReflect* reflect;  SteamPath* path;
    /* + material table, per-source position mirror, error buffer */
};
```

There is no bus field in either struct. The bus is a `float* bus` **argument**
to `rt_render`, supplied per block by whichever sink render callback is running:
the device's own buffer for `cave`, `scratch26` for `binaural`. (`scratch26` is
allocated at the `BWA_CHANNELS` capacity; `rt_render` fills only `channels` of it.)

### Also lives in RtCore

Subsystem state parked in `RtCore` (the struct definition in rt.c is the
reference):

- **panner caches**: `SpcapState` / `VbapState`, self-invalidated via
  `layout_gen`; the `panner` / `dual_band` atomics.
- **channel count**: `channels` (the layout's speaker count; set at `rt_create`),
  the width every bus loop, decode matrix, and meter array actually runs to.
- **bed decode**: the `bed_decode[BWA_CHANNELS][BWA_AMBI_CH]` matrix (built for the
  first `channels` rows) + `bed_decoder` selector (SAD / AllRAD).
- **limiter**: `lim_on` / `lim_ceiling` atomics, the `lim_gain` envelope,
  rate-derived attack/release coefficients.
- **pathing publish**: the `PathPub` double buffer + `path_idx` flip atomics,
  the `path_accum` ambisonic scratch, the path tap pointer.
- **pose**: the `tracker` (`const PoseSlot*`, [`src/pose.h`](../src/pose.h))
  and the `readback` `PoseSlot` the audio thread publishes each block.
- **streaming**: the `StreamSet` (background thread + ring pool) and the
  per-block `stream_scratch`.
- **occlusion publish**: the `occ_handle` / `occ_val` / `occ_eq` / `occ_dir`
  atomic arrays (parallel to `voices`, outside `Voice` so a voice-create memset
  can't race a publish).
- **readback / meters**: `play_pub` (per-slot playing state), `chan_peak`
  (per-channel output peaks), `dsp_now` (the published dsp clock).
- **misc DSP**: the reflection `aux` scratch, the `dop_ring` Doppler pool,
  the test-signal state, the `eq_proto` biquad prototypes.

## Helper signatures (implemented in `src/`)

Everything marked *(audio)* MUST obey invariant 1 (no alloc/lock/syscall/I/O);
see [`CLAUDE.md`](../CLAUDE.md).

```c
/* rings (rt.c; static) */
bool cmd_push (CmdRing* r, const Cmd* c);      /* control thread */
bool evt_push (EvtRing* r, const Evt* e);      /* (audio) */
void drain_commands(RtCore* c);                /* (audio) */
void drain_events  (RtCore* c);                /* control; called from rt_commit */

/* lookups (rt.c; static) */
Voice*           voice_for(RtCore* c, uint32_t handle);   /* (audio) */
const SoundData* sound_for(RtCore* c, uint32_t handle);   /* (audio) */

/* dsp (all audio thread, all real-time safe) */
void dbap_gains(const float src[3], const float lis[3], const Layout* L,
                float user_gain, float* out);              /* dbap.h; writes L->count gains */
void mix_voice(RtCore* c, Voice* v, uint16_t idx, float* bus,
               uint32_t n, uint32_t start, float* aux);    /* rt.c: mono point sources */
void mix_bed  (RtCore* c, Voice* v, uint16_t idx, float* bus,
               uint32_t n, uint32_t start);                /* rt.c: ambisonic beds */
void align_process(Aligner* a, float* bus, uint32_t nframes);  /* align.h; in place, planar */
```

In `mix_voice`, `idx` is the voice's slot index (for the handle-gated
publishes), `start` the scheduled-start offset within the block, and `aux` the
reflection aux-send buffer. `align_process` takes the `Aligner`, not the engine.

Two things that are *not* helpers here:

- **The binaural decode** is a sink render callback in engine.c
  (`render_binaural` → `monitor_process` / `steam_monitor_process`), not a bus
  tap. Steam Audio's phonon objects are created at `bwa_start`, and the decode
  runs inside the sink callback.
- **Device output** goes through the `bwa_sink` abstraction
  ([`src/sink.h`](../src/sink.h)), whose render callback fills the device's
  planar buffers directly.
