# Spatialization

## Why DBAP, not ambisonics, for point sources

The observer moves across a ~3×3 m area inside the array. Ambisonics is a
single-sweet-spot format: correct only at the decode origin, with no real near-field
behavior. Off-center, the nearest speaker dominates and the image collapses. Walking
up to a source doesn't collapse it to the nearest speaker the way it should. So
localized point sources are panned **object-based**, with gains recomputed per frame
from the **tracked listener position**.

DBAP (Distance/Direction-Based Amplitude Panning) beats textbook VBAP here for one
reason: it works directly from speaker and source **positions**. It degrades
gracefully when the listener is off-center instead of assuming a listener fixed at
the array origin.

> **Fixed-observer installs are a supported mode — and some deployments want it
> immediately.** The case above is the *moving*, tracked listener. A simpler install
> that seats the audience at one fixed spot is equally valid, and the engine already
> serves it with no extra machinery:
>
> - **Don't enable tracking.** Set the listener once to the sweet spot, or leave the
>   pose unset — the engine defaults it to the array centroid, the nominal listening
>   point (the origin sits on the floor).
> - **DBAP is position-based**, so it pans correctly for that fixed point. This
>   already beats an origin-only ambisonic decode for an off-centre seat, because
>   DBAP uses the *actual* listening position.
> - **The layout tool's coverage overlay scores this case directly** (its `V` key
>   picks fixed-centre vs moving-volume), so you can optimize a layout for the
>   observer model your install uses.
>
> For best-in-class fixed-observer quality, **SPCAP** (speaker-placement correction
> amplitude panning) is implemented as a selectable panner:
> `bw_set_panner(e, BW_PAN_SPCAP)` at load time (`spcap.c`, next to `dbap.c` behind
> the bus seam — it touches neither the seam nor its consumers). It weights every
> speaker by a smooth lobe `((1+cos)/2)^focus` toward the source's bearing from the
> listener, scales by a per-speaker **placement correction** `1/local-density` so a
> *cluster* of speakers doesn't pull the image toward it, then normalises for
> constant power (with the same distance attenuation as DBAP). The correction is
> cached and rebuilt only when the listener or layout changes, so the per-voice
> solve stays alloc/lock-free.
>
> **Pick DBAP for a moving observer, SPCAP for a fixed one.** The layout tool's `V`
> key scores a layout for either model; its preview `B` key A/Bs the panners live.
>
> **VBAP** (`BW_PAN_VBAP`, `vbap.c`) is also selectable: the 2-3 nearest speakers of
> the hull triangle containing the source carry it. It is the **sharpest** of the
> three, also fixed-observer. It needs a clean triangulation, so it suits a
> *regular* array; it has VBAP's direction-dependent source-width artifact and falls
> back to DBAP for a non-triangulable array. SPCAP stays the recommended
> fixed-observer default (smoother, robust on the irregular surveyed array); pick
> VBAP when pinpoint localization at the sweet spot is the priority. It shares the
> convex-hull + VBAP solve (`hull.c`) with AllRAD's decode. VBAP is also the
> theoretically-optimal *sparse* panner: with non-negative gains, the ℓ1-optimal
> speaker-gain solution is exactly VBAP over a Delaunay triangulation (Franck,
> Wang & Fazi 2017, IEEE TASLP) — there is no sparser panner to upgrade to, only
> different trade-offs.

Ambisonics is still the right tool for the **diffuse layer** (ambient beds,
reflections/reverb). Diffuse energy isn't sweet-spot-sensitive, so a fixed decode is
fine — decode it with a static matrix (see below). The material-driven build-out of
that layer is specified in [materials.md](./materials.md).

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
1. For each speaker `k`, compute the distance from the source as heard from the
   listener. The listener-aware form weights the source→speaker geometry referenced
   to the listener position, so off-center listeners get a correct distribution
   rather than one baked for the array center.
2. Convert distances to gains with the DBAP rolloff (a spatial-blur parameter `r`
   controls how many speakers share energy), then normalize for constant perceived
   power.
3. Apply per-source user gain and distance attenuation (source→listener).
4. Write `gtarget[0..25]`.

`r` (blur) and the distance-attenuation curve are the two tuning knobs. Expose them
in the layout/config so they can be dialed against the real array.

### Implemented formulation (M4 first cut, `dbap.c`)

For each speaker `k` (positions in room space):

1. **Proximity** — blurred source→speaker distance `d_k = sqrt(|src − spk_k|² + r²)`,
   weight `p_k = 1 / d_k^a` (rolloff exponent `a = 2`). `r` controls how many speakers
   share energy and removes the singularity at `src == spk_k`.
2. **Listener-relative direction** — multiply by `dir_k = 0.5 + 0.5·cos∠(src−lis, spk_k−lis)`
   ∈ [0,1]. This emphasizes speakers in the source's *bearing from the listener*, so
   an off-centre listener still gets the right distribution. If the source sits on
   the listener, the bearing is undefined and `dir_k = 1` for all — omnidirectional.
3. **Constant power** — normalize `g_k ← g_k / ‖g‖₂` so `Σ g_k² = 1`.
4. **Level** — scale all gains by `user_gain · atten(|src − lis|)`, where the inverse
   distance-attenuation is `atten = clamp((ref/max(d,ref))^rolloff, min, 1)`.

The result: a source *at* a speaker localizes to that speaker (smallest `d_k`,
`dir_k ≈ 1`), two speakers split a source between them, the total power is
`user_gain·atten` regardless of position, and the distribution shifts as the
listener moves. The exponents, `r`, and the curve are tuning knobs to dial against
the real array. This is a first cut, not a final psychoacoustic model.

> Implementation note: keep the math in `dbap.c` pure and listener-position-driven.
> A listener move dirties every voice (concurrency.md), so this runs for all active
> voices on move frames. Keep it allocation-free and tight.

## Dual-band panning (`bw_set_dual_band`, off by default)

The panners normalise gains to constant **power** (`Σg² = gain²`, an energy-vector /
`rE` pan) across the whole band. That is right for high frequencies, where the ear
localises by energy. Below ~700 Hz the ear localises by summed **pressure** (the
velocity vector `rV`), where **amplitude** normalisation (`Σg = gain`) gives a
sharper image — it maximises `|rV|`.

Dual-band panning (SPAT's "VBP Dual-Band") splits each source at a 700 Hz
complementary 1st-order crossover (`hi = s − lo`, sums flat) and pans the low band
amplitude-normalised, the high band power-normalised.

`compute_gains` derives the low-band gains (`gtarget_lo`) every solve: the same
directions, rescaled so the amplitude sum `Σg` equals the power-normalised gains'
own magnitude `‖g‖₂` (`rt.c`, `sc = sqrt(gp)/gs`). Not the bare user gain — that
would cancel the distance attenuation and leave a distant source's bass at full
level. The *direction* is unchanged; the LF pressure sum matches the HF energy level
at every distance. The mixer reads the second gain set only when the `dual_band`
atomic is on, so it A/Bs live.

Dual-band is sweet-spot dependent (like VBAP). The dense array + small working area
is favourable, but whether it helps a *roaming* listener is a by-ear/rig call.

## Multi-listener compromise (`bw_set_extra_listeners`)

Single-listener panning is exact for the tracked head and wrong for every other
occupant — and CAVEs usually hold a group. With extra listener positions supplied
(up to 3, commit-gated like the pose), `compute_gains` solves the same source once
per listener — each extra keeps its **own** SPCAP/VBAP cache, since those caches are
listener-keyed — and takes the per-speaker **energy mean**:
`g[k] = sqrt(mean_i g_i[k]²)`, the L2 barycentre of the individual renderings.
Constant power is preserved (the mean of the solves' powers), and every occupant
hears an image biased toward their own solve instead of one exact and the rest
wrong. Spread direction, Doppler, air absorption, the reverb-send distance, and the
binaural monitor stay primary-relative. Panner-agnostic, block-rate, one extra
point solve per listener per dirty voice.

The layout tool's coverage scoring is the design-time complement: it can already
score a layout for a fixed centre vs a moving volume; the compromise mode is the
runtime answer when several tracked-or-known heads share the room.

## Gain ramping

`dbap_gains` writes `gtarget`. The mixer holds `gcur` and interpolates
`gcur → gtarget` across the block (per-sample, or a short per-block fade). Never
apply a new 26-gain vector discontinuously — a position jump otherwise produces
audible zipper noise. This is a hard invariant.

## Per-speaker alignment

CAVE speakers sit at unequal distances from the working area, so the final output
stage corrects each channel after the mix and before the device write. This is the
spec's `align_speakers`, implemented as `align_process` in `align.c`, driven by
per-speaker values in the layout file (schema parsed in `layout.c`). Per channel,
in order:

- **Correction FIR** (`eq`, up to `BW_EQ_TAPS` = 512 taps) — the speaker-flattening
  inverse EQ written by `bw_calibrate --eq`. Applied before the gain+delay.
- **LF room-EQ cuts** (`room_eq`, up to `BW_ROOM_EQ_MAX` = 8 sections) — RBJ peaking
  cuts for room modes, written by `--room-eq`. Static-listener installs only. Also
  before the gain+delay.
- **Gain trim** — per-speaker level equalisation.
- **Delay line** — a short integer-sample delay aligning arrival times to a
  reference.

The gain trim + delay are not optional for a real array, but they are trivial DSP.
The two EQ stages are optional and bypass when absent. See
[calibration.md](./calibration.md) for how `bw_calibrate` measures and writes all
four.

## Propagation effects (opt-in, per source)

Physically-motivated per-voice effects, toggled per emitter by the control client
(default off, phonon-free — see `docs/api.md`). The first two derive from the
source↔listener distance, recomputed each block.

**Doppler** renders the voice through its acoustic propagation delay, `distance / c`.
Each voice owns a fractional delay ring. The read tap glides toward the target delay
across the block, and *the glide rate is the resampling ratio* `1 − v_radial/c` —
the pitch shift falls out of the changing delay, so you never supply a velocity.
This is a **per-source** delay (propagation from the source), distinct from the
**per-speaker** align delay above (which equalises arrival across the array); they
compose. The delay saturates past ~8 m (bounding the ring). Ring indices stay
integer (the fractional part is a separate float), so a voice running for hours
never loses sample precision.

**Air absorption** is a distance-driven one-pole low-pass on the direct path: high
frequencies roll off with distance (cutoff ≈ 18 kHz near, −650 Hz/m, ≥1.2 kHz).
Subtle in-room, pronounced for far virtual sources.

**Loudness compensation** (`bw_source_set_loudness_comp`, opt-in) is the perceptual
counterpart: attenuation lowers level, and at lower levels the ear loses LF
sensitivity (the ISO 226 equal-loudness contours), so an attenuated source reads
*thin* as well as far. A one-pole LF shelf (~250 Hz) boosts by 0.4 dB per dB of
attenuation the panner applied (capped +8 dB) — "far, not tinny". A stylization,
not physics; strict realism leaves it off.

Both ramp per sample (no zipper) and tap the reflection send *before* themselves.

**Source spread/size** gives a source angular width — a waterfall or a crowd
shouldn't collapse to a point. It runs in the per-block gain solve, not the sample
loop, renormalised to *the panner's own power*: widening redistributes energy
without re-levelling and keeps the centroid on the source direction. It is
panner-agnostic, and the new gains ramp like any other gain change. Two render
modes sit behind `bw_set_spread_mode` (an atomic live A/B, like the panner switch):

- **Lobe** (default): the panner's point gains are blended toward a
  width-controlled lobe `(½(1+cosθ))^q` centred on the source direction (`q`
  shrinks as spread→1, widening the lobe). One solve — smooth and cheap, but the
  extent is a reshaping of gains solved for a *point*.
- **MDAP** (Pulkki 1999, multiple-direction amplitude panning): a ring of virtual
  sources around the source direction — 8 at the full cone angle (`spread`·90°)
  plus 4 offset at half, at the source's own distance — each panned with the
  *selected panner*, summed, and renormalised to the point solve's power. The
  extent is built from real panner solves, so it inherits the panner's character
  (VBAP stays sparse per direction, SPCAP stays placement-corrected) and sharpens
  the extent's edge. ~13× the gain-solve cost, block-rate + dirty-gated — still
  cheap. At spread→0 the ring collapses onto the point solve, so the two modes
  meet continuously.

Either mode still feeds every speaker the **same signal** — coherent copies, which
collapse to phantom images and comb-filter *position-dependently* as the tracked
listener walks (the artifact a CAVE occupant actually notices).
**`bw_set_decorrelation`** (off by default, live A/B) splits a spread source's
energy: the coherent share takes the normal path, the rest routes through a
per-speaker **sparse velvet-noise filter bank** (`rt.c`, ~30 signed taps jittered
over 30 ms with an exponential envelope, unit energy — Välimäki et al.'s
velvet-noise decorrelator, DAFx-17/18; time-domain, no FFT, no onset latency). The
split amplitude is `sqrt(spread)` and ramps per sample; power is conserved because
incoherent energy adds. The same filter bank renders the parametric bed's diffuse
stream (below).

**Near-listener widening** (`bw_set_near_spread`, off by default): a source
approaching the head subtends a growing solid angle, but a point panner collapses
it into the nearest speaker and snaps it across the head as it passes through. With
a radius `R` set, every source's spread is floored at `1 − dist/R` in the gain
solve — untouched beyond `R`, fully wide at the head — and the widened part rides
the selected spread mode and the decorrelators. Sources flying through the room are
the common CAVE case this exists for.

## Binaural debug path

The binaural monitor is a **bus→stereo** transform. It consumes the same 26-ch bus
the array render does, so it auditions the actual render.

Two implementations sit behind the same seam:

- **Production (`steam_decode.c`, gated `BW_HAVE_STEAMAUDIO`)** — ambisonic encode →
  Steam Audio HRTF decode, described below.
- **Fallback (`binaural.c`, no SDK)** — a simple lateral-projection pan: each bus
  channel's bearing from the listener is projected onto the head's right axis and
  constant-power panned to L/R (`gL² + gR² = 1`). No HRTF. It verifies routing and
  gross laterality, not timbre or externalization.

The production decode is efficient — do **not** run 26 separate HRTF convolutions:
1. Treat each of the 26 bus channels as a virtual speaker at its surveyed room
   **direction** relative to the listener. This is where head **orientation**
   enters — rotate the speaker directions with the head.
2. Encode those 26 feeds into ambisonics — a fixed gain matrix from the speaker
   directions. Cheap.
3. Do a single **ambisonics → binaural** decode (Steam Audio's ambisonics-binaural
   effect with the configured HRTF).

Two convention details in the implemented encode matrix:

- **Normalization.** The engine encodes SN3D (AmbiX); phonon's
  `iplAmbisonicsDecodeEffect` decodes orthonormal real SH. Each ACN channel is
  rescaled per degree by `sqrt(2l+1)/sqrt(4π)` (`ambi_phonon_scale`,
  `ambisonics.c`).
- **m<0 sign.** phonon's real-SH m<0 channels have the opposite sign to the
  engine's encode, so ACN channels 1, 4, 5, 9, 10, 11 are negated (`SH_M_NEG`,
  `steam_decode.c`). Getting this wrong inverted left/right in the decoded
  stereo. `test_ambi` only checked the m≥0 channels against phonon's constants, so
  it couldn't see it; the `steam_decode` laterality test (right source → right ear,
  180° flips) caught it. If you touch either convention, run that test.

**Ambisonic order:** default to **3rd order (16 channels, 3D)** for the
encode/decode. This is the sweet spot for a 26-speaker array: order `N` uses
`(N+1)²` channels, so 3rd order is 16 and 4th is 25. By 4th order the ambisonic bus
is nearly as wide as doing the 26 HRTF convolutions directly, which defeats the
purpose; 1st–2nd order (4–9 ch) noticeably blurs the directionality the array is
there to reproduce. Expose the order as a config/build knob (alongside `r` and the
distance curve) so it can be traded against CPU on the monitor path, but 3rd order
is the baseline. The decode cost is fixed by the order, **independent of the source
count** — that is the whole point of going through ambisonics rather than
per-source HRTF.

A second optional mode binauralizes the **sources directly**, bypassing the panner.
Use it to isolate whether a problem is in tracking/positioning or in the decode. The
virtual-speaker tap is the default; the direct mode is a diagnostic.

## Diffuse-bed decode (sampling vs AllRAD)

The diffuse layer (ambisonic beds, the reflection bed) is rendered by a fixed SH→26
**decode matrix** applied per block (`build_bed_decode` / `mix_bed` in `rt.c`),
built from the speaker geometry at load time. Two decoders are selectable with
`bw_set_panner`'s sibling **`bw_set_bed_decoder`** (load-time):

- **Sampling (`BW_DECODE_SAMPLING`, default)** — the projection decode
  `decode[s][k] = (2l+1)·Y_k^SN3D(dir_s)/N` (`build_bed_decode_sad`). Cheap and
  exact for a *uniform* array. On an irregular one it over-energises dense regions:
  every speaker radiates a fixed diffuse energy regardless of position, so a
  cluster of speakers makes the diffuse field loud from that direction.
- **AllRAD (`BW_DECODE_ALLRAD`, `allrad.c`)** — All-Round Ambisonic Decoding
  (Zotter & Frank 2012). Sampling-decode to a dense **uniform virtual layout** (a
  Fibonacci sphere), then **VBAP** each virtual loudspeaker onto the real array
  (its convex-hull triangulation), then energy-normalise to the sampling decode.
  The virtual layer is uniform so the decode is well-conditioned there; VBAP
  absorbs the real array's irregularity. Robust on a lopsided survey, at the cost
  of a heavier load-time build — a brute-force hull + VBAP over ~240 virtual
  directions. The audio thread still just applies the matrix.

  A pole with no real speaker within ~60° gets an **imaginary loudspeaker** (IEM
  AllRADecoder practice): it closes the triangulation at the hole and its decode
  share is *discarded*. Without it, a floor-less array's hull spans the nadir
  with triangles of bottom-ring speakers, and downward diffuse energy smears onto
  them — with it, energy aimed where no speaker exists is dropped. The cube grid's
  ~55° nadir gap stays under the threshold, so genuinely-covered poles are
  untouched.

Validated against the cube grid + a deliberately clustered array (per-direction
energy CV / rE error): on the near-uniform cube AllRAD matches sampling (≈7% CV, a
few degrees); on the **clustered** array it cuts the loudness-vs-direction variance
from **91%→29%** and the localization error from **34°→18°**.

AllRAD doesn't touch the point-source panner (DBAP/SPCAP/VBAP). It is the
diffuse-layer counterpart to the placement correction those make for localized
sources. Its convex-hull + VBAP solve is factored into `hull.c`, shared with the
`BW_PAN_VBAP` point panner.

## Parametric bed rendering (`bw_set_bed_renderer`, live A/B)

Any matrix decode — sampling or AllRAD — has two limits on this rig: 26 speakers
are sparse for 3rd-order content (directional material blurs), and the decode is
locked to the array centre (walking off-centre skews a recorded field in exactly
the way the engine's listener-relative panning was built to avoid).

`BW_BED_PARAMETRIC` renders beds the DirAC way (Pulkki's directional audio coding,
first-order, in 4 coarse time-domain bands instead of an STFT — `mix_bed` in
`rt.c`). Per band, the smoothed **intensity vector** of the bed's FOA channels
gives a direction and a **diffuseness** `ψ = 1 − |I|/E` (0 = a plane wave, 1 =
isotropic). Two streams render per band:

- **direct** (`√(1−ψ)`): the W signal, panned through the engine's own
  **listener-relative panner** at a virtual source on the array shell
  (`ref + R·doa`). This is the payoff and the twist no stock DirAC gives: the
  direct stream re-pans *per listener position*, so a recorded soundfield becomes
  **walkable** — correct directions and parallax off-centre.
- **diffuse** (`√ψ`): the FOA band decoded through the bed matrix into the
  **velvet-noise decorrelators** — envelopment from incoherent speaker feeds, not
  26 correlated copies.

Both streams are loudness-matched to the matrix decode (a direction-averaged
plane-wave power reference computed with the decode), parameters ramp per block
(invariant 4), and the renderer crossfades per bed on the toggle, so the matrix vs
parametric comparison is a clean live A/B. Analysis is first-order (HO-DirAC
sectors are the upgrade path if band-level parameters prove too coarse); beds with
fewer than 4 channels stay on the matrix.

## Directional FDN reverb (`bw_reverb_fdn`)

The reflection bed no longer *requires* phonon: a 16-line **feedback delay
network** (`fdn.c`, Householder feedback, two-band decay filters per line) can
take the reverb bus tap instead. Each line is assigned a Fibonacci-sphere
direction and rendered as a plane wave through the same SH→26 bed decode, and the
per-line decay time scales with direction (`bw_fdn_set_decay_direction`) —
**anisotropic decay**, the diagonal direction-domain case of the Directional FDN
(Alary/Politis/Schlecht, JAES 2019). Deterministic CPU, infinite tail, no rays or
IRs; the decay is a *design* parameter (never match the measured room — that
double-counts, see calibration.md). The `fdn` test pins RT60 landing, the two-band
split, anisotropy, and stability.

## Steam Audio usage

Via the **C API**, not the Unity/FMOD integration. Relevant pieces:
- `IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM` — the array as unit-length
  speaker directions. The integrations don't expose custom layouts; the C API does.
- Ambisonics encode + ambisonics→binaural decode for the monitor path.
- Later, optionally: the Direct Effect (occlusion, distance attenuation, air
  absorption) feeding the per-source path, and reflections/reverb as a diffuse
  ambisonic bed decoded to the 26 array. These reuse the already-linked dependency.

Steam Audio's own custom-layout **panning** is a simpler projection law than
VBAP/DBAP and is angular/center-listener. It is fine for the binaural
virtual-speaker encode but is *not* the array panner. The array panner is our own
listener-relative DBAP, for the moving-observer reasons above.
