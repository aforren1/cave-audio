# Internal types & helper contracts

These are **internal** to `src/` — they are *not* part of the public ABI and must **not** go in
[`include/bwaudio.h`](../include/bwaudio.h). The field sets here are the ones the pseudocode in
[`concurrency.md`](./concurrency.md) and [`spatialization.md`](./spatialization.md) already assumes;
this file pins them down so `engine.c` can be laid out without guessing. Treat it as a sketch to
refine during M2–M5, not a frozen contract.

The thread-ownership rules from [`concurrency.md`](./concurrency.md) apply: the **audio thread**
owns the DSP state (`Voice`, the bus, `Listener.*_active`); the **control thread** owns handle
allocation, the free-list, generation tables, and asset (`Sound`) memory.

## Handles

`BwSource` / `BwSound` are `uint32_t = (index | generation<<16)` (see `H_IDX`/`H_GEN`/`MK_H` in
[`concurrency.md`](./concurrency.md)). Index and generation are each 16-bit, so `voice_cap` and the
sound table are each ≤ 65536 slots and generations wrap at 2¹⁶ (acceptable: a stale handle only has
to differ from the *current* occupant, and 65536 reuses between two references to the same slot in
one frame is not reachable).

## DSP state (audio thread owns)

```c
typedef struct {
    // identity / lifecycle
    uint16_t gen;            // generation of the handle currently in this slot
    bool     active;         // slot in use (created, not destroyed)
    bool     playing;        // currently advancing a sound
    bool     loop;
    bool     dirty;          // gains need recompute (pos/gain/listener changed)

    // playback
    const Sound* sound;      // NULL when idle; set by CMD_PLAY, cleared on retire/stop/destroy
    uint32_t cursor;         // current sample frame into sound->frames

    // spatialization
    float    pos_pending[3]; // room space; written by CMD_SET_POS
    float    pos_active[3];  // promoted on CMD_COMMIT; the mixer reads only this
    float    gain_user;      // linear, from CMD_SET_GAIN (default 1.0)
    float    gtarget[26];    // DBAP solve output (per-speaker target gains)
    float    gcur[26];       // ramp state; mix_voice interpolates gcur -> gtarget per block
} Voice;
```

## Listener (audio thread owns active; control writes via ring)

```c
typedef struct {
    float p_pending[3], q_pending[4];   // written by CMD_SET_LISTENER
    float p_active[3],  q_active[4];    // promoted on CMD_COMMIT
} Listener;                             // position used by both consumers; quaternion by binaural only
```

## Sound (control thread owns; audio thread only reads via const*)

```c
typedef struct {
    uint16_t gen;            // generation for the sound handle
    bool     retiring;       // unload requested; awaiting EVT_SOUND_RETIRED before free
    float*   pcm;            // deinterleaved or interleaved per build choice; control-thread owned
    uint32_t frames;         // length in sample frames
    uint16_t channels;       // typically 1 (point sources); >1 downmixed at load
    uint32_t sample_rate;    // must equal BwConfig.sample_rate (no runtime resampling)
} Sound;
```

## Layout (loaded once from `cave_layout.json`; read-only on the audio thread)

See [`layout-schema.md`](./layout-schema.md) for the file format.

```c
typedef struct { float pos[3]; float gain_lin; uint32_t delay_samples; } Speaker;

typedef struct {
    Speaker  speakers[26];
    float    rolloff_r;      // DBAP blur knob 'r'
    // distance-attenuation curve (resolved from the JSON model):
    float    atten_ref_m, atten_rolloff, atten_min_gain_lin;
    uint32_t max_delay_samples;   // delay-line sizing for align_speakers
} Layout;
```

## Engine (the opaque `BwEngine`)

```c
struct BwEngine {
    // rings
    CmdRing  cmds;           // control -> audio
    EvtRing  events;         // audio -> control   (see concurrency.md)

    // audio-thread DSP state
    Voice*   voices;         // voice_cap slots
    uint32_t voice_cap;
    Listener lis;
    Layout   layout;
    float*   bus26;          // nframes * 26, zeroed each block
    Monitor* monitor;        // NULL unless profile is binaural/both

    // control-thread ownership: slot free-list + generation tables, the Sound table,
    // last_error buffer. (Layout out here for brevity.)
    char     last_error[256];
};
```

## Helper signatures (implemented in `src/`)

Referenced by the pseudocode but not declared there. Everything marked *(audio)* MUST obey
invariant 1 (no alloc/lock/syscall/I/O) — see [`CLAUDE.md`](../CLAUDE.md).

```c
// rings
bool         ring_push     (CmdRing* r, const Cmd* c);             // control (audio)
bool         ring_push_evt (EvtRing* r, const Evt* e);            // audio
void         drain_commands(BwEngine* e);                          // audio
void         drain_events  (BwEngine* e);                          // control (called from bw_commit)

// lookups
Voice*       voice_for     (BwEngine* e, uint32_t handle);         // audio
const Sound* sound_for     (BwEngine* e, uint32_t handle);         // audio

// dsp (all audio thread, all real-time safe)
void dbap_gains    (const float src[3], const float lis[3], const Layout* L,
                    float user_gain, float gtarget[26]);
void mix_voice     (Voice* v, float* bus, uint32_t nframes);       // advance cursor; ramp gcur->gtarget
void align_speakers(BwEngine* e, float* bus, uint32_t nframes);    // per-ch gain trim + delay line
void binaural_tap  (Monitor* m, const float* bus, uint32_t nframes, const Listener* lis);
void asio_convert_write(BwEngine* e, long buf_index, const float* bus, uint32_t nframes);
```

> `binaural_tap` wraps Steam Audio. Its real-time safety is **not** assumed — verify that the Steam
> Audio calls on the block path do not allocate or lock (pre-create all effect/context objects at
> `bw_start`). If they cannot be made RT-safe, move the decode behind its own SPSC ring and a
> dedicated monitor thread. This is the one external-dependency risk to invariant 1; track it at M5.
