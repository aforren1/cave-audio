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
