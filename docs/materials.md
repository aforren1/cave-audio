# Materials & reflections

How surface **materials** shape simulated **occlusion**, **reflections/reverb**, and **sound
pathing**, and how that audio reaches the speaker array (26 in the CAVE) and the binaural monitor.
Two implementations now exist behind these features — Steam Audio's ray tracer
(`src/steam_scene.c`, `src/steam_reflect.c`, `src/steam_path.c`, gated on the SDK build) and a
phonon-free geometric path (`src/ism.c` early reflections + `src/fdn.c` late reverb + manual
occlusion). **Read "Choosing an acoustics path" first** — they are complementary, not rivals, and
the recommended configuration mixes them. "Implementation status" below says what is tested where.

## Choosing an acoustics path

The array render no longer needs Steam Audio at all. What the SDK still buys is *automatic*
occlusion, pathing, and a real HRTF monitor. Feature by feature:

| | Steam Audio | Homegrown | Take |
|---|---|---|---|
| **Occlusion / transmission** | ray-traced against any mesh, automatic | `bwa_source_set_occlusion_manual` — the game supplies the answer | **Steam**, clearly |
| **Sound pathing** (around occluders) | yes (probe-baked visibility) | none | **Steam** |
| **Early reflections** | ray-traced, any geometry, but a **listener-centric bed** (one field around one point, 30 Hz) | image-source, shoebox only, **panned as point sources** (direction + parallax, per block) | **Homegrown** — see below |
| **Late reverb** | convolved IR (finite length) | FDN: infinite tail, designable, anisotropic decay | **Homegrown** |
| **Binaural HRTF monitor** | real HRTF (SOFA) | lateral pan, no HRTF | **Steam** |
| **Arbitrary scene geometry** | any mesh | shoebox only (ISM) | **Steam** |
| **Cost in sources** | O(1) — one shared bed, N sends | ISM is **O(N)** — six solves + six taps per wet voice | **Steam** at scale |
| **Determinism** | ray count drives CPU; sim lags at 30 Hz | fixed cost, updates every block (~5 ms) | **Homegrown** |
| **Dependency** | submodule + built artifacts + an alignment patch + a shipped DLL | ~200 lines each, no deps | **Homegrown** |

**Reflections are where the homegrown path wins the argument that matters here.** Steam's reflection
bed is listener-*centric*: one ambisonic field decoded around one point. It cannot give parallax —
walk toward a wall and the reflection does not change direction, structurally. The ISM renders each
bounce as a point source through the listener-relative panner, updated every block. For a tracked
listener roaming a 3×3 m area — the engine's whole premise — that is the correct model. The price is
that it costs per *source*, where Steam's bed costs per *scene*.

**Recommended configuration** (and the code allows it — the ISM is independent of the reverb bus tap,
which the FDN takes):

- **Steam scene** for occlusion + directivity + pathing (the ray tracer earns its keep here),
- **ISM** for early reflections (`bwa_source_set_early_reflections`, on the handful of sources that
  matter),
- **FDN** for the late tail (`bwa_fdn_config` — deterministic, infinite, designable).

That config never calls `steam_reflect` at all. **Do not run the Steam reflection bed and the ISM at
the same time**: Steam's bed already contains early reflections, so you would hear them twice
(`bwa_source_set_early_reflections` warns once via `bwa_last_error` if the bed owns the tap).

**No-SDK builds are now fully viable** for the array: ISM + FDN + manual occlusion. What you lose is
automatic occlusion, pathing, and — the practical one for a developer working off-site — the real
HRTF monitor (the fallback is a lateral pan: fine for routing checks, useless for timbre or
front/back).

**One doctrine reminder.** The ISM's shoebox is the **virtual** environment (a hall, a corridor), not
the CAVE room you are standing in — the physical room supplies its own reflections, and modelling it
double-counts. Same trap as matching the measured RT60 ([calibration.md](./calibration.md)).

> The one rule that governs everything here: **materials never become a third consumer of the bus.**
> They parameterize Steam Audio's scene; the *output* (occluded direct sound, an ambisonic
> reflection bed decoded to the speakers) lands on the same in-memory **speaker master bus** as
> everything else, so the ASIO array and the binaural monitor both get reflections for free.
> Protect that property — see [architecture.md](./architecture.md) "the bus seam."

The bus is as wide as the loaded layout has speakers (4..26; 26 in the CAVE — see
[layout-schema.md](./layout-schema.md)). Everything below decodes to that count, not to a
hard-wired 26.

## Where this fits the existing signal flow

The direct path is DBAP (M4). Materials leave it unchanged except for occlusion. The **diffuse
layer** (ambient beds, reflections, reverb) is ambisonic: that energy is not
sweet-spot-sensitive, so a fixed decode is robust across the moving observer
([spatialization.md](./spatialization.md) "Why DBAP, not ambisonics"). Materials build out that
layer:

```
                   ┌─ direct:  occlusion (scalar gain + 3-band EQ) on the mono voice → per-source DBAP ─┐
   source ─────────┤                                                                                     ├─► speaker bus ─► { ASIO array;  binaural monitor (bus→ambi→stereo) }
                   └─ reflections: IPLSimulator (off-thread ray trace) → ambisonic IR →                  ─┘
                      IPLReflectionEffect convolution → IPLAmbisonicsDecodeEffect (panning, custom speaker layout)
```

Two paths, one bus. Materials feed both. (Sound pathing — the indirect route around occluders —
is a third feed onto the same bus; see "Pathing" below.) The reflection ambisonic field is decoded
to the **same bus channels**. The binaural monitor then re-encodes those bus channels back to
ambisonics for its stereo decode, like any other bus content — see "The monitor and reflections."

## The acoustic material model

A **material** is `IPLMaterial`: frequency-banded coefficients in Steam Audio's 3-band model
(low / mid / high):

| Field            | Type      | Meaning                                                              |
|------------------|-----------|---------------------------------------------------------------------|
| `absorption[3]`  | 0..1/band | fraction absorbed on reflection (1 = dead, 0 = perfect mirror)      |
| `scattering`     | 0..1      | fraction reflected diffusely vs specularly (surface roughness)      |
| `transmission[3]`| 0..1/band | fraction passing *through* the surface — the **spectral tilt** of occluded sound |

The engine ships a **named palette of presets** — `generic` (the built-in default, token 0),
`brick`, `concrete`, `ceramic`, `gravel`, `carpet`, `glass`, `plaster`, `wood`, `metal`, `rock`
(Steam Audio's published example coefficients) — plus custom materials. Materials are **static**.
A surface's material never changes at audio rate.

### Implemented C ABI (control thread, load-time)

A `bwa_material` is an **opaque engine-scoped token** (a small integer; `0` is always the built-in
`generic` default). Mint tokens, then attach them to geometry per triangle. Mint materials at
load time — the table is fixed-capacity. Minting works with or without the Steam Audio build; the
geometry setters are no-ops without it.

```c
bwa_material m_wall = bwa_material_preset(e, BWA_MAT_CONCRETE);    // built-in preset (bwa_material_type)
float abs3[3] = {0.1f,0.1f,0.1f}, trn3[3] = {0.6f,0.6f,0.6f};
bwa_material m_open = bwa_material_define(e, abs3, 0.5f, trn3);    // custom 3-band absorption/scattering/transmission

// per-triangle: one bwa_material token per triangle (out-of-range clamps to the default)
bwa_material tri_mat[N] = { m_wall, m_wall, m_open, /* … */ };
bwa_scene_set_mesh_mat(e, verts, nverts, tris, ntris, tri_mat);

// or a shoebox room: w×h×d metres, floor-based (x/z centred, y 0..h), one material per face
bwa_material faces[6] = { m_wall,m_wall,m_wall,m_open,m_wall,m_wall };  // -x,+x,-y,+y,-z,+z
bwa_scene_set_box(e, 6.f, 3.f, 6.f, faces);                      // triangle normals face inward
```

- **`bwa_material_preset` returns `0` for `BWA_MAT_GENERIC`** — it *is* the default token. No slot
  is minted, so tagging many surfaces with it never exhausts the table.
- **`0` is also what you get on a full table** (or an out-of-range enum value). That is the default
  material, **not** an error sentinel — check `bwa_last_error` to distinguish. Presets and custom
  materials return the same kind of token; the enum only names the preset.
- **Custom coefficients are clamped to `[0,1]`** and NaN/Inf-sanitized before they reach phonon.
- **A single-material mesh** is just `bwa_scene_set_mesh_mat` with every triangle on one token.

**Scene lifecycle.** The geometry setters are safe to call at runtime, reflection bed or not. The
occlusion sim owns the `IPLScene` and is the sole thread that commits it; the borrowing reflection
and pathing sims take that scene's lock **shared** around their ray traces (`steam_scene_ray_lock`,
an `SRWLOCK`), so an `iplSceneCommit` can no longer race a `RunReflections`/`RunPathing`. What moves
each frame is a **dynamic mesh** (`bwa_scene_add_dynamic_mesh` → an `IPLInstancedMesh` you reposition
with `bwa_scene_set_dynamic_transform`) — a cheap top-level BVH refit, not a geometry rebuild — the
acoustic analogue of a physics collider with a transform. Replacing the whole *static* mesh at runtime
also works but rebuilds the entire BVH, so it's for occasional swaps, not per-frame motion. The one
thing runtime geometry does **not** move is a **baked** reverb/pathing result: the bake froze the
geometry at `bwa_start`, so a mover changes nothing there — use real-time reflections (the default) if
the scene animates. `scene_locked` in `src/engine.c` now only means "no scene" (no SDK / failed
create).

### Reflection bed: hybrid reverb (directional early reflections + parametric tail)

The reflection bed runs Steam Audio's **HYBRID** reverb: an early-reflection **convolution** plus a
**parametric (FDN)** late tail, rendered as a **full ambisonic field** (order `order`, decoded
across the layout's speakers). The early reflections are **directional** — they arrive from the
directions the geometry actually reflects them. A symmetric scene with a centred listener
correctly collapses to a near-omni field (the directional ambisonic channels cancel); asymmetric
geometry lights them up (verified: an offset source puts ~half the omni energy into a directional
channel).

The vendored phonon carries a local patch for a **Steam Audio 4.8.1 bug**: the complex
`ArrayMath::multiplyAccumulate` reads its accumulator with an *aligned* SSE load on its *unaligned*
code path, which access-violates on the odd ambisonic channels (the per-channel FFT stride is
`8 mod 16` bytes, because `numSpectrumSamples` is always odd) — so any `numChannels > 1` reflection
effect crashes at channel 1. The fix is one line (`load`→`loadu`); see
[third_party/patches/](../third_party/patches/) and [third_party/README.md](../third_party/README.md).
Report it upstream and drop the patch once it lands.

The same per-triangle materials drive **both** occlusion (per-band transmission) and the
reflection bed (absorption/scattering), because both simulators share one committed `IPLScene`. A
per-triangle smoke test confirms a source behind a `concrete` triangle is occluded to ~0.015 while
one behind a high-transmission triangle passes at ~0.6.

The occlusion *amount* is separate from the material. Steam Audio's `IPLDirectEffectParams`
carries a **scalar `occlusion`** (how much of the direct path is blocked, a geometry/ray result)
*and* the **3-band `transmission`** (the material's spectral tilt of what leaks through). The
material supplies `transmission`; the ray query supplies `occlusion`.

## Geometry — what materials attach to

Materials live on the **virtual acoustic scene**: an `IPLStaticMesh` (vertices + triangle indices +
one material index per triangle) plus the material list. This is the geometry of the rendered
*virtual* environment — **not** the speaker layout ([layout-schema.md](./layout-schema.md) is
physical and stays separate).

**Coordinate frame.** The mesh, the source/listener positions Steam Audio
ray-traces against, and the ambisonic IR it returns are all in **room space** (right-handed,
metres, the same frame DBAP and the layout use — [integration.md](./integration.md)). A binding
pushing geometry from a Unity/Unreal level **must convert mesh vertices through the same
registration matrix** it already uses for source/listener positions. Skip that and reflections
arrive rotated/mirrored relative to the array — exactly the failure integration.md warns about for
direct sources. Triangle **winding** must follow Steam Audio's inside/outside convention. The
shoebox mode builds its mesh directly in room space.

Two authoring modes:

1. **Shoebox room** — `bwa_scene_set_box(w, h, d)` + a material per face. The engine generates the
   12-triangle mesh in room space. Covers the common room-like space with no asset pipeline.
2. **Arbitrary mesh** — vertices/triangles/material-tokens from the content scene, via
   `bwa_scene_set_mesh_mat`.

Set geometry before `bwa_start` if you use the reflection bed; the runtime-swap rule is under
"Scene lifecycle" above.

## How materials drive the audio — the two paths

### Direct sound (per source) — occlusion, not distance

Spatialization stays **DBAP** (M4), and **DBAP keeps owning distance attenuation and air
absorption** (spatialization.md step 4 scales gains by `user_gain · atten(|src − lis|)`).
Materials add only a per-source occlusion stage. The occlusion sim ray-traces with phonon's direct
simulation configured for **occlusion + transmission only** — the `distanceAttenuation` and
`airAbsorption` flags stay **off**, so distance is not counted twice and the level is identical
the instant you toggle occlusion on. Occlusion is **volumetric** (partial cover attenuates
smoothly, not as a binary shadow).

Occlusion applies to the **mono voice signal, upstream of panning**: a scalar occlusion gain plus
a 3-band transmission EQ on the pre-DBAP signal. It does **not** enter `dbap_gains` and does
**not** set the per-voice `dirty` flag — the per-speaker DBAP gain vector stays
position/listener-driven and constant-power, and the solve stays dirty-gated on geometry only.

What the sim publishes, per source at 30 Hz: it folds occlusion and transmission per band —
`raw[b] = occlusion + (1 − occlusion) · transmission[b]` — then splits that into a broadband
**level** (`max(raw)`) and a normalized 3-band **tilt** (`raw / max`), plus the directivity gain.
The handoff is a set of per-voice atomics in `rt.c` (`occ_handle`/`occ_val`/`occ_eq`/`occ_dir`),
gated by the audio thread's own generation — a publish for a recycled slot is dropped.

On the audio thread this is its **own** per-voice state, distinct from the DBAP `gcur→gtarget`
ramp. The occlusion gain and the three band gains ramp **per sample** (invariant 4). The biquad
coefficient sets are derived once per block from per-band filter prototypes precomputed at create
(`eq_proto`, rate-derived — this runs at 96 kHz too) and interpolated across the block;
`bufferSwitch` never rebuilds filter prototypes. The result: a wall *muffles*, it doesn't just
attenuate.

### Source directivity (cheap, per source)

A source facing away from you is a strong perceptual cue. The shipped model is a **weighted
dipole** (phonon's `IPLDirectivity`):

- **`bwa_source_set_directivity(e, s, weight, power)`** — `weight` 0 = omni (off), 0.5 = cardioid,
  1 = figure-8; `power >= 1` sharpens the lobe.
- **`bwa_source_set_directivity_preset(e, s, pattern)`** — named sugar: `BWA_DIR_OMNI` /
  `BWA_DIR_CARDIOID` / `BWA_DIR_FIGURE8` (OMNI disables it).
- **`bwa_source_set_orientation(e, s, qx, qy, qz, qw)`** — the dipole axis is the source's forward
  (+z rotated by the quaternion), same room frame and handedness as the listener pose.

The sim evaluates the dipole gain from the source's orientation and the source→listener vector and
publishes it alongside the occlusion values; the audio thread ramps it on the same pre-pan mono
stage. Directivity needs no geometry and is independent of occlusion — a source can be directional
without being occluded. No panning cost.

### Reflections + reverb — a diffuse ambisonic bed

A dedicated sim thread (12 Hz) runs Steam Audio's **`IPLSimulator`** reflections against the scene
+ materials for a single listener-centric bed source and produces a **single mixed ambisonic IR**
(early reflections *and* late reverb together — the public C API does **not** expose discrete
image sources). The audio thread runs the **`IPLReflectionEffect`** convolution against that IR,
yielding an ambisonic reflection signal, then **decodes it to the bus channels with an
`IPLAmbisonicsDecodeEffect` in panning mode** (`binaural = false`) whose `speakerLayout` is the
**same `IPL_SPEAKERLAYOUTTYPE_CUSTOM` layout** the binaural monitor builds from the surveyed
geometry — one direction per layout speaker — and sums it onto the bus.

Decoding an ambisonic field onto an **irregular** speaker grid (the CAVE's 3×3×3 boundary minus
centre, *not* a uniform sphere) is the hard, non-unique direction: a naïve sampling/transpose or raw
pseudo-inverse gives uneven loudness and direction errors. Steam Audio's decoder handles the custom
layout with a sane decode law and the right SH channel-order/normalization convention. A literal
precomputed matrix is only an *optimization* of the same effect (skipping per-block effect
overhead); if you take it, it must reproduce phonon's SH convention **and** a proper
irregular-layout decode (e.g. AllRAD / energy-preserving max-rE), or you get a rotated/scaled bed.

Reflections carry **spaciousness and distance**, not primary localization, so the sweet-spot
argument that mandates DBAP for the direct sound is weak for the diffuse field: a fixed decode
across the 3×3 m roam is fine (the split in [spatialization.md](./spatialization.md)). A **lower
ambisonic order suffices** for a diffuse bed (`order` is 1 or 2, default 1; the direct binaural
monitor uses 3rd), keeping the reflection channel count and convolution cheap.

**Bounding the convolution cost — hybrid reverb.** Convolving a full IR (early + a long diffuse
tail) every block is the feature's CPU cost centre (below). Steam Audio's **hybrid reverb** splits
it: a **short ray-traced early-reflection IR** — the part that carries spatial cues — convolved as
above, plus a **parametric / FDN late tail** synthesised from the simulator's estimated per-band
decay (RT60; `reverb_estimator` / `hybrid_reverb_estimator` in the SDK). The convolution then runs
against a *short* IR and the long tail is a cheap recursive reverb sharing the same ambisonic→bus
decode — far less CPU for the same perceived space. The bed runs hybrid unconditionally
(`REFL_TYPE` in `steam_reflect.c` is a compile-time constant); full-length-IR convolution would be
a code change, not a config option.

## Threading & RT-safety

Simulation (ray tracing) is expensive and never runs on the audio thread (invariant 1). It runs on
dedicated **simulation threads** — and they are *not* the `bwa_*` control thread.

- **Control thread** (`bwa_*`, single producer of the command ring — invariant 2): owns material
  minting, scene setup, source create/destroy, and the per-source feature toggles. It stays the
  **only** producer of the command ring.
- **Simulation threads** (below-normal priority, so they never preempt the audio callback):
  **occlusion at 30 Hz** (`SIM_HZ`, `steam_scene.c`), **reflections at 12 Hz** (`REFL_HZ`,
  `steam_reflect.c` — reverb changes slowly, so it's cheaper than occlusion), **pathing at 10 Hz**
  (`PATH_HZ`, `steam_path.c`). None of them touch the command/event rings or any control-owned
  allocation state (that would make them a second ring producer and break the SPSC scheme). Their
  inputs come through dedicated single-writer publications:
  - **listener pose** via the existing pose seqlock — correct for both `feedListener` and
    internal tracking (with a tracker connected the live pose is audio-thread-owned; the seqlock is
    how everyone else reads it). They must **not** read `Listener.*_active`.
  - **source positions/features** from a control-thread-fed locked shadow — **not** the audio
    thread's `Voice.pos_active`. Each tick the sim snapshots the shadow and pushes
    `iplSourceSetInputs` per source + `iplSimulatorSetSharedInputs` for the listener.
- **Audio thread**: applies only the per-block effects (below). All effect/context objects, the
  decode effect, and every FFT/convolution scratch are allocated **at `bwa_start`** and never in
  the callback.
- **`bwa_desc.embree`** runs the ray-tracing sims on Intel Embree — opt-in, with a graceful
  fallback to the default tracer. Details in [api.md](./api.md).

**The reflection convolution is the feature's CPU cost centre on the audio thread — not "cheap."**
`iplReflectionEffectApply` is a **partitioned (FFT) convolution**
(`IPL_REFLECTIONEFFECTTYPE_CONVOLUTION` on CPU for the early part — TrueAudio Next is GPU and out
of scope) of the source against an `(order+1)²`-channel ambisonic IR, every block. A 1–2 s IR at
48 kHz over a 512–1024-sample block is ~48k–96k taps × channels per reflection stream — budget it
against the ASIO block deadline. The decisive cost-control choice: the bed is **a single shared
listener-centric instance** (one immortal bed `IPLSource` co-located with the listener), so the
convolution is **one** instance regardless of source count. Per-source reflection streams are not
built; each would add a convolution. This call is the same one
[internal-types.md](./internal-types.md) already flags ("its real-time safety is not assumed") —
`iplReflectionEffectApply`/`iplAmbisonicsDecodeEffectApply` run per block, so the M5
allocation-free verification gate applies to them; do not add anything to the tap that hasn't
cleared it.

**Fixed-at-create, not live knobs.** `IPLReflectionEffect` is created with a fixed `irSize` and
`numChannels = (order+1)²`, and the simulator's reflection channel count **must match the
effect's** (a mismatch is a hard crash). So **IR length, ambisonic order, and the ray/bounce
budget are load-time configuration** — `bwa_reflections_config` is callable only between
`bwa_create` and `bwa_start`, and changing any of them means `bwa_stop`/`bwa_start`. The one live
control is the wet gain: `bwa_reverb_set_gain` (an atomic the audio-thread tap reads). Per
block, only `IPLReflectionEffectParams.numChannels` may be *reduced* (≤ created) to shed CPU;
nothing is resized.

**The IR handoff is a params publish, never a per-block copy.** An ambisonic IR is multi-megabyte
(e.g. 2nd order × 2 s × 48 kHz ≈ 9 × 96000 floats ≈ 3.4 MB) — far too large to copy in
`bufferSwitch`. The IR lives in simulator/source-owned memory. The sim thread runs the simulation,
retrieves the `IPLReflectionEffectParams` (IR pointer + `numChannels` + `irSize`) via
`iplSourceGetOutputs`, and publishes that small POD through a **seqlock** (`steam_reflect.c`); the
audio-thread tap reads it consistently or keeps the previous block's params. The sim thread is the
sole writer; the tap is the sole reader and the sole consumer of the IR (apply mutates the IR's
read state). Lifetime is solved by ownership, not a pool: `params.ir` aliases interior memory of
the immortal bed `IPLSource`, which is created once and released only at destroy, after the audio
thread has joined — no use-after-free window.

**Per-source results are generation-keyed.** Occlusion/directivity publishes are keyed by the full
generation-counted `bwa_source` handle and **dropped on a generation mismatch** at consume time
(just as `voice_for` drops stale commands) — a slot destroyed and recycled between simulation and
consumption can't apply stale values to a different sound. This path is **decoupled from
`CMD_COMMIT` frame coherence**: occlusion/reflection energy is diffuse and low-rate, and the ramps
hide the sub-frame skew, so it does not need the listener/source atomicity the direct DBAP pan
requires (where a torn snapshot *is* audible).

**No clicks on low-rate updates.** Occlusion gain + filter coefficients ramp per block (above).
The IR transition is phonon's own: one persistent `IPLReflectionEffect`, with only `params->ir`
updated per publish, and the library cross-fades in place.

## The monitor and reflections

To keep the bus seam, reflections go **ambisonic → speaker decode → bus**, and the binaural monitor
then re-encodes those bus channels back to ambisonics for its stereo decode, like any other bus
content. That is a deliberate double ambisonic round-trip (low-order reflection ambi → the
irregular speaker bus → 3rd-order monitor ambi → stereo); its cost and directionality loss buy the "both consumers
audition the identical bus" guarantee. Short-circuiting the monitor by feeding it the pre-decode
reflection ambisonics directly is **prohibited** — it would create a second, bus-bypassing binaural
consumer and desync the array vs monitor reflection content.

## Geometric default, perceptual option

Everything above is the **geometric** path — materials + scene → ray-traced reflections — and it
is the **default**. For content with no usable mesh (abstract or musical), the *perceptual* school
(IRCAM Spat) drives a parametric reverb from listener-tested knobs instead of geometry:
**presence** (direct vs reverb), **warmth** / **brillance** (LF/HF balance), **room presence**,
**reverberance** (decay time), **envelopment**.

The hybrid reverb's FDN late tail already exposes what such a mapping targets (per-band RT60,
early/late balance, band EQ), so a perceptual mode is a control mapping — **no new DSP**, same bus.
It bypasses the scene and drives the late-reverb parameters directly, and is mutually exclusive
with geometry *per source*: a source is either physically simulated or perceptually placed. **Not
implemented** (see "Implementation status").

## Data surface / API (additive; load-time setup, per-frame-safe toggles)

Signatures live in [include/bw_audio.h](../include/bw_audio.h); per-call threading
semantics are in [api.md](./api.md). An engine with no scene renders as if materials didn't exist.
The shipped surface:

```c
/* materials — engine-scoped tokens; 0 = the built-in generic default */
bwa_material bwa_material_preset(bwa_engine* e, const char* name);   /* "concrete", "carpet", ... */
bwa_material bwa_material_define(bwa_engine* e, const float absorption[3], float scattering,
                              const float transmission[3]);

/* scene geometry (room space; runtime-callable until the reflection bed runs — see "Scene lifecycle") */
void bwa_scene_set_mesh_mat(bwa_engine* e, const float* verts, int nverts, const int* tris, int ntris,
                           const bwa_material* tri_material);     /* per-triangle tokens */
void bwa_scene_set_box     (bwa_engine* e, float w, float h, float d, const bwa_material faces[6]);

/* reflection bed — config is load-time only (sizes are baked at bwa_start); wet gain is live */
void bwa_reflections_config (bwa_engine* e, const bwa_reflections_desc* cfg);
void bwa_reverb_set_gain(bwa_engine* e, float linear);        /* LIVE, per-frame-safe */

/* per-source toggles (per-frame-safe: enqueue only) */
void bwa_source_set_occlusion          (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_reverb        (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_reverb_send    (bwa_engine* e, bwa_source s, float gain);
void bwa_source_set_reverb_distance(bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_pathing            (bwa_engine* e, bwa_source s, bool on);
void bwa_source_set_orientation        (bwa_engine* e, bwa_source s, float qx, float qy, float qz, float qw);
void bwa_source_set_directivity        (bwa_engine* e, bwa_source s, float weight, float power);
void bwa_source_set_directivity_preset (bwa_engine* e, bwa_source s, bwa_directivity pattern);

/* readbacks (HUD/diagnostics) */
float bwa_source_get_occlusion  (bwa_engine* e, bwa_source s);       /* 1 = clear .. 0 = fully blocked */
float bwa_source_get_directivity(bwa_engine* e, bwa_source s);       /* 1 = on-axis/omni .. 0 = full null */
```

`bwa_reflections_desc` (bw_audio.h) is: `ir_seconds` (0 → default 1.0, range ~0.5..2.0), `order`
(1 or 2; 0 → default 1), `num_rays` (0 → default 4096), `num_bounces` (0 → default 16), `enabled`
(0 = no bed created), `bake` (non-0 = precompute the reverb at a probe grid at `bwa_start`), and
`reserved[3]` (zero; room to grow without an ABI break). The wet level is not config — it is
`bwa_reverb_set_gain`, live, default 1. There is no "update Hz" field (the sim
rates are compile-time constants) and no shared-vs-per-source field (the bed is always the single
shared instance).

The per-source wet send:

- **`bwa_source_set_reverb_send`** sets the source's wet-send **level** (default 1.0; 0 =
  none). Drive it yourself for a manual dry/wet.
- **`bwa_source_set_reverb_distance`** turns on **distance→wet**: the engine scales the send by
  the source↔listener distance, on top of the level — near = drier, far = wetter (0.25× at ≤1 m up
  to 1× at ≥6 m). Off by default.
- Both are per-frame-safe. The final send gain is computed per block and **ramped** in `rt.c`
  (invariant 4), so motion and on/off never zipper the send. Sends shape how much each source
  feeds the one shared bed — they do not add convolutions.

Not built: a scene-file loader (`bwa_scene_load`) does not exist — engines push geometry through
the setters above; a standalone scene JSON (a future `docs/scene-schema.md`, separate from the
speaker layout) would be its own addition. A **perceptual reverb mode** (above) would add a small
`bwa_perceptual_config` (presence / warmth / brillance / reverberance / envelopment) driving the
late-reverb engine — also future.

Provisioning today: **Unity/Unreal** push room-space geometry + materials at load time (the game
owns the scene — [integration.md](./integration.md)).

## CPU budget & tuning knobs

Reflections are the cost centre. Every sizing knob is baked at `bwa_start` (it fixes effect/IR
allocation sizes — see "Fixed-at-create" above); the wet gain is the one live control.

| Knob                       | Effect                                          | Default       | When set |
|----------------------------|-------------------------------------------------|---------------|----------|
| hybrid reverb (early IR + FDN tail) | **the** cost control — convolve a short early IR, not a long one | always on | compile-time (`REFL_TYPE`) |
| `ir_seconds`               | reverb tail / IR length                         | 1.0 (~0.5..2.0) | baked at `bwa_start` |
| `order`                    | bed directionality vs channel count / IR width  | 1 (max 2)     | baked at `bwa_start` |
| `num_rays`                 | reflection accuracy vs ray-trace cost (off-thread) | 4096       | baked at `bwa_start` |
| `num_bounces`              | bounce depth (off-thread)                       | 16            | baked at `bwa_start` |
| wet level                  | reverb level summed onto the bus                | 1.0           | **live** (`bwa_reverb_set_gain`) |
| sim update rate            | reflection responsiveness vs CPU (off-thread)   | 12 Hz         | compile-time (`REFL_HZ`) |

Two multiplicative cost controls: **hybrid reverb** keeps each convolution short (early IR only;
the long tail is a cheap recursive reverb), and the **single shared listener-centric bed** bounds
it to one convolution regardless of source count.

## The real room

Materials simulate the *virtual* scene. The physical CAVE also reflects sound off its real walls,
and those reflections are not under engine control. For the simulated acoustics to read correctly,
**the physical room should be acoustically treated (absorptive)** so the rendered virtual
reflections dominate. That is a deployment requirement.

## Scope & sequencing

Built: occlusion + transmission EQ + directivity, the reflection bed, and the probe machinery on
top of it (baked reflections `bwa_reflections_desc.bake`, sound pathing `bwa_desc.enable_pathing`) —
all below.

Still not built:

- **Perceptual reverb mode** (geometry-free) — a thin mapping onto the FDN late tail, for abstract
  content.
- **Runtime scene swap under the reflection bed** — needs a scene-swap handshake; today the
  setters are rejected while the bed runs (see "Scene lifecycle").
- **Per-image-source DBAP early reflections** (can't be extracted from the mixed ambisonic IR),
  **full-length-IR convolution**, **GPU (TrueAudio Next) convolution**, and **runtime
  order/IR-length changes** (phonon fixes effect sizes at create).

**Doppler** is phonon-free per-voice DSP in `rt.c` (`bwa_source_set_doppler` — see
[api.md](./api.md)), not part of the materials path.

## Implementation status

- **Occlusion + per-band transmission EQ + directivity — implemented** (`src/steam_scene.c`, gated on
  the Steam Audio build; `bwa_scene_set_mesh_mat` / `bwa_scene_set_box` +
  `bwa_source_set_occlusion` / `bwa_source_set_directivity` / `bwa_source_set_orientation` +
  `bwa_source_get_occlusion` / `bwa_source_get_directivity`). The sim thread owns an `IPLScene` +
  `IPLStaticMesh` (**per-triangle materials**) + an `IPLSimulator` and ray-traces **volumetric**
  occlusion + transmission + directivity at 30 Hz; the audio thread ramps the published values
  (mechanism under "Direct sound" and "Threading & RT-safety" above). The published
  level/EQ/directivity ramp is asserted in `test/rt_test.c`; the playground wall is a real scene
  mesh.
- **Reflection bed — implemented** (`src/steam_reflect.c`; the sections above, plus the per-source
  send controls in "Data surface / API"). The `reflect` test proves it is directional;
  `bwa_reflections_desc.bake` precomputes it and the `bake` test proves the baked path stays directional.
- **Sound pathing — implemented** (`src/steam_path.c`; the section below). The `path` test proves it
  routes around a wall with the right direction; the `rt` test proves the SH-encode lands on `s·shCoeffs`.
- **Perceptual reverb mode — designed, not yet implemented** (this document) — a thin control mapping
  onto the same bus, **no new DSP**.

## Baked reflections — implemented (`bwa_reflections_desc.bake`)

`bwa_reflections_desc.bake` precomputes the listener-centric reverb at a grid of probes covering the listening
zone (`steam_reflect.c` `do_bake`, gated `BWA_HAVE_STEAMAUDIO`); the reflection sim thread then *looks
up* the reverb at the listener each tick instead of ray-tracing it. The ray-trace runs once, offline,
at `bwa_start`, and can afford more rays/bounces than real time. The reverb still renders through the
same hybrid effect + speaker decode, so it stays **directional** — the `bake` test confirms the +X-near
listener gets +X-biased reverb out of baked data (ratio ≈ 1.4, vs ~1.0 for an omni bed).

Two things must be right:

- **Probe placement.** `UNIFORMFLOOR` generation is sensitive to floor-mesh winding (it finds no floor
  in a box whose floor faces down). Place probes manually with one `CENTROID` call per grid point —
  but phonon's `generateCentroidProbe` reads the probe **centre from the transform's translation
  column** and the influence **radius from the basis-column lengths (min/2)** (it treats the matrix
  as an OBB centre+basis, NOT the documented unit-cube→box mapping). So the translation must be the
  grid point itself (not a corner), and a box edge of `2*spacing` gives `radius == spacing`, so the
  probes' influence spheres overlap.
- **Influencing probes.** `SimulationManager::lookupBakedReflections` only fills the reverb for sources
  whose listener has **influencing probes** (`getInfluencingProbes` → `validSimulationData`). Get the
  transform wrong and the probes land outside their own influence radius of the listener: no
  influencing probe → the lookup is silently skipped and the reverb stays zero.

The CPU win for *this* installation is marginal (the real-time ray-trace already runs off the audio
thread with Tracy headroom), but baking enables much higher bake-time quality and is the right
default for a static room.

## Pathing — wired (`bwa_desc.enable_pathing`)

`IPL_SIMULATIONFLAGS_PATHING` + `iplPathBakerBake` + `IPLSimulationInputs.pathingProbes` route sound
around occluders / through portals over the same probe network baking uses. It's wired end to
end (`steam_path.c`, same with-SDK gate, opt in with `bwa_desc.enable_pathing` at `bwa_create`):

- **Bake (Stage 1, `steam_path_create`).** A probe grid spans the layout (+margin) at mean speaker
  height, using the SAME OBB-transform convention learned for reflections (centre in the translation
  column, radius from the basis lengths — see above). `iplPathBakerBake` writes the probe-to-probe
  visibility graph. Set the pathing sim's `IPLSimulationSettings.maxOrder` to the path order: if you
  don't, the path field is silently capped to order-0 (omni, no direction). The `path` test proves a
  route bends around a wall AND that the recovered shCoeffs point the right way (−X toward the
  source, +Z out the opening).
- **Render (Stage 2).** A 10 Hz sim thread runs pathing per opted-in source and publishes each one's
  `IPLPathEffectParams.shCoeffs` (+ the normalized bending-loss `eqCoeffs`, below) to rt.c via
  `rt_set_pathing` (handle-gated, double-buffered). In the mixer, a pathing voice SH-encodes its
  UN-occluded signal (`accum[k] += s·shCoeffs[k]` — the indirect path goes around the occluder, so
  the direct-path occlusion must NOT apply to it) into a shared ambisonic accumulator, ramped per
  sample (invariant 4). After the voice loop the `path` tap decodes that accumulator to the speaker
  bus through phonon's own `iplAmbisonicsDecodeEffect` — so phonon's ACN/N3D convention is
  consistent encode-to-decode, no hand-rolled normalization. The `rt` test verifies the encode
  lands exactly on `s·shCoeffs`; the decode is the same call the (tested) reflection bed uses.

Opt sources in with `bwa_source_set_pathing`. The **bending-loss EQ (`eqCoeffs[3]`) is rendered
too.** phonon splits a path's response two ways (`path_simulator.cpp`): `shCoeffs` carry the
direction *and* level (each path is SH-projected weighted by its distance attenuation), while
`eqCoeffs` carry the frequency-dependent *deviation* (bending) loss. To add the colour without
disturbing the level, the sim normalizes `eqCoeffs` to a pure tilt — loudest band = 1, floored at
phonon's `kMaxEQGain` (0.0625) — exactly phonon's `normalizeEQ` mode, and publishes it beside the
shCoeffs (`rt_set_pathing`, same handle-gated double buffer). The mixer applies that tilt as the
same low-shelf/peak/high-shelf biquad cascade the occlusion EQ uses, to the **un-occluded** `s_raw`
*before* the SH-encode — precisely phonon's own `path_effect.cpp` render order (EQ the mono signal,
then scale each SH channel). It's ramped per sample (invariant 4) and bypassed while flat, so a
path with no occluder to bend around costs nothing and is byte-identical to the pre-EQ render. The
`rt` test asserts a non-flat tilt colours the encoded field (a DC source through `{0.5,1,1}` lands
the accumulator at `0.5·shCoeffs`, the RBJ low-shelf DC gain). Pathing only does anything where the
scene has real occluders to bend sound around; judge it by ear at the rig.
