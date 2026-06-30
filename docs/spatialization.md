# Spatialization

## Why DBAP, not ambisonics, for point sources

The observer moves across a ~3×3 m area inside the array. Ambisonics is a
single-sweet-spot format — correct only at the decode origin, with no real near-field
behavior. Across a 3×3 m roam the nearest speaker dominates off-center and the image
collapses, and walking up to a source doesn't collapse it to the nearest speaker the
way it should. So localized point sources are panned **object-based**, with gains
recomputed per frame from the **tracked listener position**.

Distance/Direction-Based Amplitude Panning (DBAP) is preferred over textbook VBAP
here because it works directly from speaker and source **positions** and degrades
gracefully when the listener is off-center, rather than assuming a listener fixed at
the array origin.

> **Fixed-observer installs are a supported mode — and some deployments want it immediately.** The
> case above is the *moving*, tracked listener, but a simpler install that seats the audience at one
> fixed spot is equally valid. The engine already serves it with **no extra machinery**: don't enable
> tracking, set the listener once to the sweet spot (or leave it at the origin for a centred spot), and
> DBAP — being position-based — pans correctly for that fixed point. This is already better than an
> origin-only ambisonic decode for an off-centre seat, because DBAP uses the *actual* listening
> position. The layout tool's coverage overlay scores this case directly (its `V` key picks
> fixed-centre vs moving-volume), so a layout can be optimized for the observer model the install uses.
>
> For best-in-class fixed-observer quality, **SPCAP** (speaker-placement correction amplitude panning)
> is **implemented as a selectable panner** — `bw_set_panner(e, BW_PAN_SPCAP)` at load time (`spcap.c`,
> next to `dbap.c` behind the bus seam, touching neither the seam nor its consumers). It weights every
> speaker by a smooth lobe `((1+cos)/2)^focus` toward the source's bearing from the listener, scaled by
> a per-speaker **placement correction** `1/local-density` so a *cluster* of speakers doesn't pull the
> image toward it, then normalises for constant power (with the same distance attenuation as DBAP). The
> correction is cached and rebuilt only when the listener or layout changes, so the per-voice solve
> stays alloc/lock-free. **Pick DBAP for a moving observer, SPCAP for a fixed one**; the layout tool's
> `V` key scores a layout for either model, and its preview `B` key A/Bs the panners live.
>
> **VBAP** (`BW_PAN_VBAP`, `vbap.c`) is also selectable: the 2-3 nearest speakers of the hull triangle
> containing the source carry it — the **sharpest** of the three, also fixed-observer. It needs a clean
> triangulation, so it suits a *regular* array; it has VBAP's direction-dependent source-width artifact
> and falls back to DBAP for a non-triangulable array. SPCAP remains the recommended fixed-observer
> default (smoother, robust on the irregular surveyed array); VBAP is there when pinpoint localization at
> the sweet spot is the priority. It shares the convex-hull + VBAP solve (`hull.c`) with AllRAD's decode.

Ambisonics is still the right tool for the **diffuse layer** (ambient beds,
reflections/reverb), where energy isn't sweet-spot-sensitive and a fixed decode is
fine. If/when that layer is added, decode it with a static matrix (see below). The
material-driven build-out of that layer is specified in [materials.md](.\materials.md).

## The gain solve (`dbap_gains`)

Per voice, per frame *if dirty*: produce a 26-element gain vector `gtarget` from the
source position, the listener position, and the speaker layout.

Inputs:
- `src` — source position (room space).
- `lis` — listener position (room space). Orientation is **not** used by the array
  render.
- `layout` — the 26 surveyed speaker positions (room space), loaded from
  `layout_path`.

Sketch (listener-relative DBAP):
1. For each speaker `k`, compute distance from the source as heard from the listener.
   The listener-aware form weights by the source→speaker geometry referenced to the
   listener position, so off-center listeners get a correct distribution rather than
   one baked for the array center.
2. Convert distances to gains with the DBAP rolloff (a spatial-blur parameter `r`
   controls how many speakers share energy), then normalize for constant perceived
   power.
3. Apply per-source user gain and distance attenuation (source→listener).
4. Write `gtarget[0..25]`.

`r` (blur) and the distance-attenuation curve are the two tuning knobs; expose them
in the layout/config so they can be dialed against the real array.

### Implemented formulation (M4 first cut, `dbap.c`)

For each speaker `k` (positions in room space):

1. **Proximity** — blurred source→speaker distance `d_k = sqrt(|src − spk_k|² + r²)`,
   weight `p_k = 1 / d_k^a` (rolloff exponent `a = 2`). `r` controls how many speakers
   share energy and removes the singularity at `src == spk_k`.
2. **Listener-relative direction** — multiply by `dir_k = 0.5 + 0.5·cos∠(src−lis, spk_k−lis)`
   ∈ [0,1], emphasizing speakers in the source's *bearing from the listener* so an
   off-centre listener still gets the right distribution. (If the source sits on the
   listener, the bearing is undefined and `dir_k = 1` for all — omnidirectional.)
3. **Constant power** — normalize `g_k ← g_k / ‖g‖₂` so `Σ g_k² = 1`.
4. **Level** — scale all gains by `user_gain · atten(|src − lis|)`, where the inverse
   distance-attenuation `atten = clamp((ref/max(d,ref))^rolloff, min, 1)`.

So a source *at* a speaker localizes to that speaker (it has the smallest `d_k` and a
`dir_k ≈ 1`), two speakers split a source between them, the total power is
`user_gain·atten` regardless of position, and the distribution shifts as the listener
moves. The exponents/`r`/curve are tuning knobs to dial against the real array; this is a
first cut, not a final psychoacoustic model.

> Implementation note: keep the math in `dbap.c` pure and listener-position-driven.
> A listener move dirties every voice (concurrency.md), so this runs for all active
> voices on move frames — keep it allocation-free and tight.

## Dual-band panning (`bw_set_dual_band`, off by default)

The panners normalise gains to constant **power** (`Σg² = gain²`, an energy-vector / `rE` pan) across
the whole band — right for high frequencies, where the ear localises by energy. Below ~700 Hz the ear
localises by summed **pressure** (the velocity vector `rV`), where **amplitude** normalisation
(`Σg = gain`) gives a sharper image (it maximises `|rV|`). Dual-band panning (SPAT's "VBP Dual-Band")
splits each source at a 700 Hz complementary 1st-order crossover (`hi = s − lo`, sums flat) and pans the
low band amplitude-normalised, the high band power-normalised. `compute_gains` derives the low-band
gains (`gtarget_lo`) every solve as the same directions rescaled to `Σg = gain`, so the *direction* is
unchanged — only the low band's level/coherence. The mixer reads the second gain set only when the
`dual_band` atomic is on, so it A/Bs live. It is sweet-spot dependent (like VBAP); the dense array +
small working area is favourable, but whether it helps a *roaming* listener is a by-ear/rig call.

## Gain ramping

`dbap_gains` writes `gtarget`. The mixer holds `gcur` and interpolates
`gcur → gtarget` across the block (per-sample, or a short per-block fade). Never
apply a new 26-gain vector discontinuously — a position jump otherwise produces
audible zipper noise. This is a hard invariant.

## Per-speaker alignment

CAVE speakers sit at unequal distances from the working area, so the final output
stage applies, per channel: a **gain trim** and a short **delay line** to align
arrival times to a reference. This lives in `align_speakers`, after the mix and
before the device write, and is driven by per-speaker values in the layout file.
This is not optional for a real array, but it is trivial DSP.

## Propagation effects (opt-in, per source)

Two physically-motivated, per-voice effects a control client toggles per emitter (default off,
phonon-free — see `docs/api.md`). Both derive from the source↔listener distance, recomputed each block.

**Doppler** renders the voice through its acoustic propagation delay, `distance / c`. Each voice owns
a fractional delay ring; the read tap glides toward the target delay across the block, and *the glide
rate is the resampling ratio* `1 − v_radial/c` — i.e. the pitch shift falls out of the changing delay,
so no velocity needs to be supplied. Note this is a **per-source** delay (propagation from the source),
distinct from the **per-speaker** align delay above (which equalises arrival across the array); they
compose. The delay saturates past ~8 m (bounding the ring); ring indices stay integer (the fractional
part is a separate float) so a voice running for hours never loses sample precision.

**Air absorption** is a distance-driven one-pole low-pass on the direct path: high frequencies roll off
with distance (cutoff ≈ 18 kHz near, −650 Hz/m, ≥1.2 kHz). Subtle in-room, pronounced for far virtual
sources. Both ramp per sample (no zipper) and tap the reflection send *before* themselves.

**Source spread/size** gives a source angular width (a waterfall/crowd shouldn't collapse to a point).
It runs in the per-block gain solve, not the sample loop: the panner's point gains are blended toward a
width-controlled lobe `(½(1+cosθ))^q` centred on the source direction (q shrinks as spread→1, widening
the lobe), then renormalised to *the panner's own power* — so widening redistributes energy without
re-levelling and keeps the centroid on the source direction. Panner-agnostic; the new gains ramp like
any other gain change. (A future refinement is true multi-direction panning — MDAP — if the lobe blend
proves too coarse on the real array.)

## Binaural debug path

The binaural monitor is a **bus→stereo** transform that consumes the same 26-ch bus
the array render does, so it auditions the actual render.

Efficient decode (do **not** run 26 separate HRTF convolutions):
1. Treat each of the 26 bus channels as a virtual speaker at its surveyed room
   **direction** relative to the listener (this is where head **orientation**
   enters — rotate the speaker directions with the head).
2. Encode those 26 feeds into ambisonics — a fixed gain matrix from the speaker
   directions. Cheap.
3. Do a single **ambisonics → binaural** decode (Steam Audio's ambisonics-binaural
   effect with the configured HRTF).

**Ambisonic order:** default to **3rd order (16 channels, 3D)** for the encode/decode. This is the
sweet spot for a 26-speaker array: order `N` uses `(N+1)²` channels, so 3rd order is 16 and 4th is
25 — by 4th order the ambisonic bus is nearly as wide as doing the 26 HRTF convolutions directly,
which defeats the purpose, while 1st–2nd order (4–9 ch) noticeably blurs the directionality the
array is there to reproduce. Expose the order as a config/build knob (alongside `r` and the
distance curve) so it can be traded against CPU on the monitor path, but 3rd order is the baseline.
The decode cost is fixed by the order, **independent of the source count** — that is the whole point
of going through ambisonics rather than per-source HRTF.

A second optional mode binauralizes the **sources directly**, bypassing the panner —
useful for isolating whether a problem is in tracking/positioning or in the decode.
The virtual-speaker tap is the default; the direct mode is a diagnostic.

## Diffuse-bed decode (sampling vs AllRAD)

The diffuse layer (ambisonic beds, the reflection bed) is rendered by a fixed SH→26 **decode matrix**
applied per block (`build_bed_decode` / `mix_bed` in `rt.c`), built from the speaker geometry at load
time. Two decoders are selectable with **`bw_set_panner`'s sibling `bw_set_bed_decoder`** (load-time):

- **Sampling (`BW_DECODE_SAMPLING`, default)** — the projection decode `decode[s][k] = (2l+1)·Y_k^SN3D(dir_s)/N`
  (`build_bed_decode_sad`). Cheap and exact for a *uniform* array, but it over-energises dense regions of
  an irregular one (every speaker radiates a fixed diffuse energy regardless of position, so a cluster
  of speakers makes the diffuse field loud from that direction).
- **AllRAD (`BW_DECODE_ALLRAD`, `allrad.c`)** — All-Round Ambisonic Decoding (Zotter & Frank 2012):
  sampling-decode to a dense **uniform virtual layout** (a Fibonacci sphere), then **VBAP** each virtual
  loudspeaker onto the real array (its convex-hull triangulation), then energy-normalise to the sampling
  decode. The virtual layer is uniform so the decode is well-conditioned there; VBAP absorbs the real
  array's irregularity. Robust on a lopsided survey, at the cost of a heavier load-time build (a brute-
  force hull + VBAP over ~240 virtual directions — the audio thread still just applies the matrix).

Validated against the cube grid + a deliberately clustered array (per-direction energy CV / rE error):
on the near-uniform cube AllRAD matches sampling (≈7% CV, a few degrees); on the **clustered** array it
cuts the loudness-vs-direction variance from **91%→29%** and the localization error from **34°→18°**.
AllRAD doesn't touch the point-source panner (DBAP/SPCAP/VBAP) — it's the diffuse-layer counterpart to
the placement correction those make for localized sources. Its convex-hull + VBAP solve is factored
into `hull.c`, shared with the `BW_PAN_VBAP` point panner.

## Steam Audio usage

Via the **C API** (not the Unity/FMOD integration). Relevant pieces:
- `IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM` — the array as unit-length
  speaker directions. (The integrations don't expose custom layouts; the C API does.)
- Ambisonics encode + ambisonics→binaural decode for the monitor path.
- Later, optionally: the Direct Effect (occlusion, distance attenuation, air
  absorption) feeding the per-source path, and reflections/reverb as a diffuse
  ambisonic bed decoded to the 26 array. These reuse the already-linked dependency.

Note Steam Audio's own custom-layout **panning** is a simpler projection law than
VBAP/DBAP and is angular/center-listener; it is fine for the binaural virtual-speaker
encode but is *not* the array panner. The array panner is our own listener-relative
DBAP, for the moving-observer reasons above.
