# Materials & reflections (design)

How surface **materials** shape simulated **occlusion** and **reflections/reverb**, and how that
audio reaches the 26-speaker array and the binaural monitor. This is a forward-looking design — it
specifies the model so the rest of the engine can be built toward it; implementation lands in the
"Later" bucket (see [roadmap.md](.\roadmap.md)) and depends on the Steam Audio path that the
production binaural decode (M5 upgrade) brings in first.

> The one rule that governs everything here: **materials never become a third consumer of the bus.**
> They parameterize Steam Audio's scene; the *output* (occluded direct sound, an ambisonic
> reflection bed decoded to the 26 channels) lands on the same in-memory **26-channel master bus**
> as everything else, so the ASIO array and the binaural monitor both get reflections for free.
> Protect that property — see [architecture.md](.\architecture.md) "the bus seam."

## Where this fits the existing signal flow

The direct path is built (DBAP, M4) and is *unchanged* by materials except for occlusion. The
architecture already reserved the **diffuse layer** (ambient beds, reflections, reverb) for
ambisonics, because that energy is not sweet-spot-sensitive and a fixed decode is robust across the
moving observer ([spatialization.md](.\spatialization.md) "Why DBAP, not ambisonics"). Materials are
the build-out of that reserved layer:

```
                   ┌─ direct:  occlusion (scalar gain + 3-band EQ) on the mono voice → per-source DBAP ─┐
   source ─────────┤                                                                                     ├─► 26-ch bus ─► { ASIO array;  binaural monitor (26→ambi→stereo) }
                   └─ reflections: IPLSimulator (off-thread ray trace) → ambisonic IR →                  ─┘
                      IPLReflectionEffect convolution → IPLAmbisonicsDecodeEffect (panning, custom 26-dir layout)
```

Two paths, one bus. Materials feed both. Critically, the reflection ambisonic field is decoded to
the **same 26 bus channels**, then the binaural monitor re-encodes those 26 channels back to
ambisonics for its stereo decode like any other bus content — see "The monitor and reflections."

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
(Steam Audio's published example coefficients) — plus custom materials. Materials are **static** —
a surface's material never changes at audio rate.

### Implemented C ABI (control thread, load-time)

A `BwMaterial` is an **opaque engine-scoped token** (a small integer; `0` is always the built-in
`generic` default). Mint tokens, then attach them to geometry per-triangle. All of this is
load-time (do it before `bw_start`); minting works with or without the Steam Audio build, but the
geometry setters are no-ops without it.

```c
BwMaterial m_wall = bw_material_preset(e, "concrete");          // named preset (case-insensitive)
float abs3[3] = {0.1f,0.1f,0.1f}, trn3[3] = {0.6f,0.6f,0.6f};
BwMaterial m_open = bw_material_define(e, abs3, 0.5f, trn3);    // custom 3-band absorption/scattering/transmission

// per-triangle: one BwMaterial token per triangle (out-of-range clamps to the default)
BwMaterial tri_mat[N] = { m_wall, m_wall, m_open, /* … */ };
bw_scene_set_mesh_mat(e, verts, nverts, tris, ntris, tri_mat);

// or a shoebox room: w×h×d metres centred at origin, one material per face
BwMaterial faces[6] = { m_wall,m_wall,m_wall,m_open,m_wall,m_wall };  // -x,+x,-y,+y,-z,+z
bw_scene_set_box(e, 6.f, 3.f, 6.f, faces);                      // triangle normals face inward
```

`bw_material_preset` returns `0` for `"generic"` (it *is* the default token — no slot is minted, so
tagging many surfaces with it never exhausts the table) and also `0` (the default, **not** an error
sentinel) on an unknown name or a full table — check `bw_last_error` to distinguish those. Custom
coefficients are clamped to `[0,1]` and NaN/Inf-sanitized before reaching phonon. The geometry
setters are **enforced** load-time: a call after `bw_start` is rejected with a `bw_last_error`
(the occlusion + reflection sims share one `IPLScene`, and a mesh swap can't run concurrently with
the reflection thread's ray tracing). The single-material `bw_scene_set_mesh` remains the `nmat==1`
convenience.

### Reflection bed: hybrid reverb (directional early reflections + parametric tail)

The reflection bed runs Steam Audio's **HYBRID** reverb: an early-reflection **convolution** plus a
**parametric (FDN)** late tail, rendered as a **full ambisonic field** (order `order`, decoded across
the 26 speakers) so the early reflections are **directional** — they arrive from the directions the
geometry actually reflects them. A symmetric scene with a centred listener correctly collapses to a
near-omni field (the directional ambisonic channels cancel); asymmetric geometry lights them up
(verified: an offset source puts ~half the omni energy into a directional channel).

Getting there required fixing a real **Steam Audio 4.8.1 bug** (found via a single-threaded phonon
repro + a debugger trace): the complex `ArrayMath::multiplyAccumulate` reads its accumulator with an
*aligned* SSE load on its *unaligned* code path, which access-violates on the odd ambisonic channels
(the per-channel FFT stride is `8 mod 16` bytes, because `numSpectrumSamples` is always odd). That
crashed any `numChannels > 1` reflection effect at channel 1. The one-line fix (`load`→`loadu`) is a
local patch on the vendored phonon — see [third_party/patches/](../third_party/patches/) and
[third_party/README.md](../third_party/README.md); report it upstream and drop it once it lands.

The same per-triangle materials drive **both** occlusion (per-band
transmission) and the reflection bed (absorption/scattering), because both simulators share one
committed `IPLScene`. A per-triangle smoke test confirms a source behind a `concrete` triangle is
occluded to ~0.015 while one behind a high-transmission triangle passes at ~0.6.

Note the occlusion *amount* is separate from the material: Steam Audio's `IPLDirectEffectParams`
carries a **scalar `occlusion`** (how much of the direct path is blocked, a geometry/ray result)
*and* the **3-band `transmission`** (the material's spectral tilt of what leaks through). The
material supplies `transmission`; the ray query supplies `occlusion`.

## Geometry — what materials attach to

Materials live on the **virtual acoustic scene**: an `IPLStaticMesh` (vertices + triangle indices +
one material index per triangle) plus the material list. This is the geometry of the rendered
*virtual* environment — **not** the speaker layout ([layout-schema.md](.\layout-schema.md) is
physical and stays separate).

**Coordinate frame (load-bearing).** The mesh, the source/listener positions Steam Audio ray-traces
against, and the ambisonic IR it returns are all in **room space** (right-handed, metres, the same
frame DBAP and the layout use — [integration.md](.\integration.md)). A binding pushing geometry from
a Unity/Unreal level **must convert mesh vertices through the same registration matrix** it already
uses for source/listener positions, or reflections arrive rotated/mirrored relative to the array —
exactly the failure integration.md warns about for direct sources. Triangle **winding** must follow
Steam Audio's inside/outside convention. The shoebox mode below builds its mesh directly in room
space.

Two authoring modes:

1. **Shoebox room** — `(w, h, d)` + a material per face → the engine generates the 12-triangle mesh
   in room space. Covers the common room-like space with no asset pipeline.
2. **Arbitrary mesh** — vertices/triangles/material-indices from the content scene or a scene file.

**Scene lifecycle (v1): load-time only.** Building the `IPLScene`/`IPLStaticMesh` and committing it
into the `IPLSimulator` allocates and builds a ray-trace BVH — expensive and not RT-safe. So scene
setters are callable only **between `bw_create` and `bw_start`** (like the speaker layout). Runtime
geometry swap (rebuild on the sim thread, publish via a generation/retire-ack handoff so the old
`IPLScene` is freed only after the sim thread drops it) is **out of scope for v1**, alongside dynamic
geometry.

## How materials drive the audio — the two paths

### Direct sound (per source) — occlusion, not distance

Spatialization stays **DBAP** (M4) and **DBAP keeps owning distance attenuation and air absorption**
(spatialization.md step 4 already scales gains by `user_gain · atten(|src − lis|)`). Materials add
only a per-source occlusion stage via Steam Audio's **Direct Effect**, configured to apply **only
occlusion + transmission** — its `distanceAttenuation` and `airAbsorption` flags are left **off** so
distance is not counted twice and the level is identical the instant occlusion is toggled on.

Occlusion is applied to the **mono voice signal, upstream of panning** — a scalar occlusion gain
plus a 3-band transmission EQ on the pre-DBAP signal. It deliberately does **not** enter `dbap_gains`
and does **not** set the per-voice `dirty` flag: the 26-gain DBAP vector stays position/listener-
driven and constant-power, and the solve stays dirty-gated on geometry only. Occlusion's low-rate
updates are decoupled from DBAP recompute.

This needs its **own** per-voice state, distinct from the DBAP `gcur→gtarget` ramp: add to `Voice`
an occlusion scalar (`occ_gain_cur/target`) and the **biquad state + target coefficients** for the
3-band transmission filter. The coefficients are computed **off-thread** (the sim thread turns
`transmission[3]` into biquad coefficients) and published as a small POD; the audio thread only
**interpolates** between current and target coefficient sets per block — never calls transcendentals
in `bufferSwitch` — and ramps the occlusion gain. Both glides satisfy invariant 4 (no
discontinuity), but they are their *own* mechanism, not the DBAP gain ramp.

### Source directivity (cheap, per source)

A simple radiation model is worth having and nearly free: an **aperture** (cone width, directive ↔
omnidirectional) plus a **face-listener** toggle (keep the source on-axis toward the listener), à la
IRCAM Spat's "aperture" / "relative direction." It maps straight onto Steam Audio's Direct Effect
**`IPLDirectivity`** — a per-source gain from the source's orientation and the source→listener vector —
applied on the same pre-pan mono stage as occlusion, so it reuses the occlusion glide and adds no
panning cost. A source facing away is a strong perceptual cue we currently ignore.

### Reflections + reverb — a diffuse ambisonic bed

Steam Audio's **`IPLSimulator`** (off-thread) ray-traces each reflection-enabled source against the
scene + materials and produces a **single mixed ambisonic IR** (early reflections *and* late reverb
together — the public C API does **not** expose discrete image sources). The audio thread runs the
**`IPLReflectionEffect`** convolution of the source against that IR, yielding an ambisonic reflection
signal, which is then **decoded to the 26 channels with an `IPLAmbisonicsDecodeEffect` in panning
mode** (`binaural = false`) whose `speakerLayout` is the **same `IPL_SPEAKERLAYOUTTYPE_CUSTOM`
26-direction layout** the binaural monitor builds from the surveyed geometry — and summed onto the
bus.

Using Steam Audio's decode effect here is deliberate and important: decoding an ambisonic field onto
the **irregular** 26-speaker grid (a 3×3×3 boundary minus centre, *not* a uniform sphere) is the hard,
non-unique direction — a naïve sampling/transpose or raw pseudo-inverse gives uneven loudness and
direction errors. Steam Audio's decoder handles a custom layout with a sane decode law and the
correct SH channel-order/normalization convention; **re-deriving the matrix ourselves risks getting
that convention wrong** (rotated/scaled bed). A literal precomputed matrix is only an *optimization*
of the same effect (to skip per-block effect overhead), and if taken must reproduce Steam Audio's SH
convention and a proper irregular-layout decode (e.g. AllRAD / energy-preserving max-rE) — it is not
a substitute for thinking the decode through.

Why ambisonic-and-fixed-decode rather than per-image-source DBAP:
- It is Steam Audio's native reflection output — no re-derivation of image sources.
- Reflections carry **spaciousness and distance**, not primary localization; the sweet-spot argument
  that *mandates* DBAP for the direct sound is much weaker for the diffuse field, so a fixed decode
  across the 3×3 m roam is acceptable (the explicit split in [spatialization.md](.\spatialization.md)).
- A **lower ambisonic order suffices** for a diffuse bed (1st–2nd; the direct binaural monitor uses
  3rd), keeping the reflection channel count and convolution cheap.

**Bounding the convolution cost — hybrid reverb (the default).** Convolving a full IR (early + a long
diffuse tail) every block is the feature's CPU cost centre (below). Steam Audio's **hybrid reverb**
splits it the way IRCAM Spat and most production reverbs do: a **short ray-traced early-reflection
IR** — the part that carries spatial cues — convolved as above, plus a **parametric / FDN late tail**
synthesised from the simulator's estimated per-band decay (RT60; `reverb_estimator` /
`hybrid_reverb_estimator` in the SDK). The convolution then runs against a *short* IR and the long
tail is a cheap recursive reverb sharing the same ambisonic→bus decode — far less CPU for the same
perceived space. This is the **default** for the reflection bed; full-length-IR convolution is the
high-fidelity option.

> **Out of scope — sharp early reflections.** Strong, directional first-order reflections localize
> more than diffuse reverb and could be rendered as image-source DBAP "voices." But those image
> sources **cannot be extracted** from Steam Audio's mixed ambisonic IR — it would require a
> *separate* first-party image-source/ray pass running alongside the ambisonic bed, not a re-tap of
> the simulator output. Future refinement, not v1.

## Threading & RT-safety

Reflection/occlusion simulation (ray tracing) is **expensive and never runs on the audio thread**
(invariant 1). This introduces a **third thread** — and it is *not* the `bw_*` control thread.

- **Control thread** (`bw_*`, single-producer of the command ring — invariant 2): owns scene setup
  (load-time), source create/destroy, and registering each reflection/occlusion-enabled `BwSource`
  as an `IPLSource` with the `IPLSimulator`. It must stay the **only** producer of the command ring.
- **Simulation thread** (new, third): runs `iplSimulatorRunReflections` + the occlusion ray queries
  at **10–30 Hz** (far below audio rate). It **never touches the command/event rings or any
  control-owned allocation state** (doing so would make it a second ring producer and break the
  SPSC scheme). Its inputs come through dedicated single-writer publications:
  - **listener pose** via the existing pose seqlock (`bw_get_listener_pose`) — which is correct for
    both `feedListener` and `track_internal` (under `track_internal` the live pose is audio-thread-
    owned; the seqlock is how everyone else reads it). It must **not** read `Listener.*_active`.
  - **source positions** from a control-thread-published positions snapshot/shadow (the control
    thread already has them when it issues `CMD_SET_POS`) — **not** the audio thread's
    `Voice.pos_active`. Each tick it pushes `iplSourceSetInputs` per source + `iplSimulatorSetSharedInputs`
    for the listener.
- **Audio thread**: applies only the per-block effects (below). All effect/context objects, the IR
  buffer pool, the decode-effect, and every FFT/convolution scratch are allocated **at `bw_start`**
  and never in the callback.

**The reflection convolution is the feature's CPU cost centre on the audio thread — not "cheap."**
`iplReflectionEffectApply` is a **partitioned (FFT) convolution** (`IPL_REFLECTIONEFFECTTYPE_CONVOLUTION`
on CPU — TrueAudio Next is GPU and out of scope) of the source against a 1–2 s, `(order+1)²`-channel
ambisonic IR, every block. A 1–2 s IR at 48 kHz over a 512–1024-sample block is ~48k–96k taps ×
channels per reflection stream — it must be budgeted against the ASIO block deadline. The decisive
cost-control choice: **default to a single shared listener-centric reflection bed**, so the
convolution is **one** instance regardless of source count; per-source reflection streams are opt-in
for the few sources that need distinct reflections. This call is the same one
[internal-types.md](.\internal-types.md) already flags ("its real-time safety is not assumed") — so
`iplReflectionEffectApply`/`iplAmbisonicsDecodeEffectApply` get the **M5 allocation-free verification
gate** before they go on the block path.

**Fixed-at-create, not live knobs.** `IPLReflectionEffect` is created with a fixed `irSize` and
`numChannels = (order+1)²`, and the simulator's reflection channel count **must match the effect's**
(a mismatch is a hard crash). So **IR length, ambisonic order, ray/bounce budget, and IR-pool
sizing are `bw_start`/load-time configuration** — `bw_reflections_config` is a stopped/load-time
call, and changing any of them requires `bw_stop`/`bw_start`. Per block, only
`IPLReflectionEffectParams.numChannels` may be *reduced* (≤ created) to shed CPU; nothing is resized.

**The IR handoff is a pointer swap, never a per-block copy.** An ambisonic IR is multi-megabyte
(e.g. 2nd order × 2 s × 48 kHz ≈ 9 × 96000 floats ≈ 3.4 MB) — far too large to memcpy through a
value seqlock in `bufferSwitch`. In phonon the IR lives in simulator/source-owned memory retrieved
via `iplSourceGetOutputs`; the sim thread runs the simulation, gets the `IPLReflectionEffectParams`
(IR pointer + `numChannels` + `irSize`), and **publishes the params (pointer) via an atomic
exchange**. The audio thread atomically loads the current IR pointer (no copy). Backing storage is an
**N-deep pool (≥3) of IR buffers** allocated at `bw_start`; the sim thread may not reuse a buffer
until the audio thread has provably released it — the same retire-ack/lifetime reasoning the sound
buffers use, never a use-after-free of an IR the convolution is still reading.

**Per-source results are generation-keyed.** Occlusion params and IR pointers are keyed by the full
generation-counted `BwSource` handle and **dropped on a generation mismatch** at consume time (just
as `voice_for` drops stale commands) — so a slot destroyed and recycled between simulation and
consumption can't apply a stale occlusion/IR to a different sound. This path is **intentionally
decoupled from `CMD_COMMIT` frame coherence**: occlusion/reflection energy is diffuse and low-rate,
and the crossfade hides the sub-frame skew, so it does not need the listener/source atomicity the
direct DBAP pan requires (where a torn snapshot *is* audible). That decoupling is acceptable *here*
precisely because the diffuse field tolerates it — unlike the direct path.

**No clicks on low-rate updates.** Occlusion gain + filter coefficients ramp per block (above). The
**IR crossfade** is its own mechanism — prefer Steam Audio's in-place IR transition (keep one
persistent `IPLReflectionEffect` per stream and only update `params->ir`, letting the library
cross-fade); if hand-rolled, pre-allocate the dual-IR scratch + a second output buffer at `bw_start`
and budget the doubled convolution on swap blocks.

## The monitor and reflections

To keep the bus seam, reflections go **ambisonic → 26-ch decode → bus**, and the binaural monitor
then re-encodes those 26 channels back to ambisonics for its stereo decode, like any other bus
content. This is a deliberate double ambisonic round-trip (low-order reflection ambi → irregular
26-ch → 3rd-order monitor ambi → stereo); the cost and the directionality loss are **accepted to
protect the "both consumers audition the identical bus" guarantee**. Short-circuiting the monitor by
feeding it the pre-decode reflection ambisonics directly is **prohibited** — it would create exactly
the second, bus-bypassing binaural consumer this design forbids, and desync the array vs monitor
reflection content. (If reflection quality on headphones ever demands it, the only sanctioned
exception must be documented explicitly and reconciled with the single-seam rule.)

## Geometric default, perceptual option

Everything above is the **geometric** path — materials + scene → ray-traced reflections — and it is
the **default**, because the CAVE mostly simulates physical situations. But geometry isn't always
wanted (abstract or musical content, or scenes with no usable mesh), and the *perceptual* school
(IRCAM Spat) is the alternative: drive a parametric reverb from a handful of listener-tested knobs —
**presence** (direct vs reverb), **warmth** / **brillance** (LF/HF balance), **room presence**,
**reverberance** (decay time), **envelopment** — instead of geometry.

It is cheap to tack on *because the hybrid reverb already provides the engine*: the FDN late tail
(above) exposes exactly the parameters a thin perceptual mapping targets (per-band RT60, early/late
balance, band EQ), so a perceptual layer is a control mapping, **no new DSP**, landing on the same
bus. So the design supports both — **geometric by default**, with an optional **perceptual mode**
that bypasses the scene and drives the late-reverb parameters directly (per source, or as one shared
bed). The two are mutually exclusive *per source*: a source is either physically simulated or
perceptually placed.

## Data surface / API (additive; load-time setup, per-frame-safe toggles)

All additive and optional — an engine with no scene behaves exactly as today. Names provisional:

```c
/* materials: presets by name, or define custom 3-band coefficients */
BwMaterial bw_material_preset(const char* name);                 /* "concrete", "carpet", ... */
BwMaterial bw_material_define(const float absorption[3], float scattering, const float transmission[3]);

/* scene geometry + reflection config — LOAD-TIME ONLY (between bw_create and bw_start) */
void bw_scene_set_box (BwEngine*, float w, float h, float d, const BwMaterial faces[6]);
void bw_scene_set_mesh(BwEngine*, const float* verts_room, int nverts,   /* verts in ROOM space */
                                  const int* tris, int ntris, const int* tri_material);
void bw_scene_load    (BwEngine*, const char* path);
void bw_reflections_config(BwEngine*, const BwReflectionConfig*);  /* IR length, order, rays/bounces, update Hz, shared-vs-per-source — FIXES effect sizes */

/* per-source toggles (per-frame-safe: enqueue only) */
void bw_source_set_reflections(BwEngine*, BwSource, bool on);
void bw_source_set_occlusion  (BwEngine*, BwSource, bool on);     /* level identical with occlusion off — distance stays in DBAP */
void bw_source_set_directivity(BwEngine*, BwSource, float aperture, bool face_listener);  /* radiation cone */
```

A **perceptual reverb mode** (above) would add a small `bw_perceptual_config` (presence / warmth /
brillance / reverberance / envelopment) that drives the late-reverb engine when a source has no
geometric reflections — a load-time/while-running parameter set, not per-block.

Two provisioning routes: **Unity/Unreal** push room-space geometry + materials at load time (the game
owns the scene — [integration.md](.\integration.md)); **standalone** uses a scene JSON (a future
`docs/scene-schema.md`, separate from the speaker layout).

## CPU budget & tuning knobs

Reflections are the cost centre; the knobs are **start-time** sizing decisions, not live controls:

| Knob                       | Effect                                          | Default       |
|----------------------------|-------------------------------------------------|---------------|
| hybrid reverb (early IR + FDN tail) | **the** cost control — convolve a short IR, not a long one | on (default) |
| shared bed vs per-source   | one convolution vs N                            | shared bed    |
| reflection ambisonic order | bed directionality vs channel count / IR width  | 1st–2nd       |
| early-IR length            | early-reflection convolution cost (tail is the cheap FDN) | ~50–200 ms |
| rays / bounces             | reflection accuracy vs ray-trace cost (off-thread) | moderate   |
| simulation update rate     | reflection responsiveness vs CPU (off-thread)   | 10–30 Hz      |

Two multiplicative cost controls: **hybrid reverb** keeps each convolution short (early IR only; the
long tail is a cheap recursive reverb), and the **shared listener-centric bed** bounds it to a single
convolution regardless of source count. Per-source reflection streams are opt-in and each add one.

## The real room

Materials simulate the *virtual* scene. The physical CAVE also reflects sound off its real walls, and
those reflections are not under engine control. For the simulated acoustics to read correctly, **the
physical room should be acoustically treated (absorptive)** so the rendered virtual reflections
dominate. A deployment requirement, noted here so it is designed for, not discovered on site.

## Scope & sequencing

- **Prerequisite:** the production Steam Audio binaural decode (M5 upgrade) lands first — it links the
  SDK and establishes the ambisonics/decode-effect plumbing this design reuses (no new dependency,
  [build.md](.\build.md)).
- **Then, cheapest first:** per-source **occlusion** (Direct Effect, occlusion + transmission only) —
  a small per-voice gain+EQ glide, immediate payoff — and **directivity** rides the same pre-pan
  stage for nearly free.
- **Then:** the **reflection bed** — the simulation thread, the IPLSource registration, the pointer-
  swap IR handoff, the `IPLReflectionEffect` convolution, and the `IPLAmbisonicsDecodeEffect` to the
  26-ch bus; **v1 = a single shared listener-centric bed, hybrid reverb (short early IR + FDN tail),
  static (load-time) scene**.
- **Optional, non-default:** the **perceptual reverb mode** (geometry-free) — a thin mapping onto the
  FDN late tail, for abstract content.
- **Out of scope (v1):** dynamic/runtime geometry, per-image-source DBAP early reflections, **Doppler**
  (a per-voice fractional-delay line — relevant for fast sources, noted for later), full-length-IR
  convolution, GPU (TrueAudio Next) convolution, runtime order/IR-length changes.

## Implementation status

- **Occlusion — implemented** (`src/steam_scene.c`, gated on the Steam Audio build; `bw_scene_set_mesh`
  + `bw_source_set_occlusion` + `bw_source_get_occlusion`). The third "simulation thread" is real: it
  owns an `IPLScene` + `IPLStaticMesh` (one material) + an `IPLSimulator`, ray-traces **volumetric**
  occlusion + transmission at 30 Hz, and publishes one transmittance scalar per source. The control
  thread feeds geometry + per-source enable/position through a locked shadow; the audio thread reads
  the published value lock-free and **ramps** it (invariant 4). The sim → audio handoff is a pair of
  per-voice atomics (`occ_handle`/`occ_val`) the audio thread gates on its own generation — the sim
  never touches audio-owned voice state, so there is no data race and a recycled slot can't inherit a
  stale occlusion. The playground wall is a real scene mesh.
  - **v1 is level-only:** the published scalar combines the geometric occlusion with the material's
    *mean* transmittance (so concrete vs glass differ in level). The **per-band transmission EQ**
    (the spectral tilt / biquad described above — the "3-band EQ" half of the direct stage) is **not
    yet built**; that and **directivity** are the next increment on this same pre-pan stage.
- **Reflection bed, perceptual mode, directivity — designed, not yet implemented** (this document).
