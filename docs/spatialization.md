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

The formulation below is a house design, but its two load-bearing departures from
the original Lossius DBAP have published support: Sundstrom (I3DA 2021,
arXiv:2109.08704) documents how the original's convex-hull projection fails for
sources *outside* the array (projections landing on a hull vertex make distinct
positions produce identical gains, and total power "undulates wildly" across the
boundary), and lands on the same shape as the fix (hull-free, with a
reference-distance rolloff). The engine's solve never touches a hull, so both
failures are impossible by construction; the `dsp` test sweeps a source from the
center out through a corner speaker to 10 m and pins gain continuity, monotone
level, and exterior injectivity.

The sweet-spot claim is measured, not just argued. The layout tool's bed metric
(`bwa_bed_gains_batch`, the engine's real AllRAD/EPAD builds; see
[layout-schema.md](./layout-schema.md)) scores one co-optimized 26-speaker layout
at 2.4°/8.1° mean/worst rE error for a sweet-spot listener and 14.4°/52.5° over
the 3×3 m roam, against 5.0°/25.5° for tracked DBAP on the same speakers: the
static decode is the best render on the array at the center and loses 3× to the
tracked panner everywhere else. The physics sets that scale. An order-N decode
reconstructs the field only within roughly N·c/(2πf) of the center, about 16 cm
at 1 kHz for 3rd order, and no order 26 speakers can drive reaches a 3 m working
volume.

> **Fixed-observer installs are a supported mode.** The case above is the *moving*,
> tracked listener. An install that seats the audience at one fixed spot needs no
> extra machinery:
>
> - **Don't enable tracking.** Set the listener once to the sweet spot, or leave the
>   pose unset; the engine defaults it to the array centroid, the nominal listening
>   point (the origin sits on the floor).
> - **DBAP is position-based**, so it pans correctly for that fixed point, using the
>   *actual* listening position, unlike an origin-only ambisonic decode.
> - **The layout tool's coverage overlay scores this case directly** (its `V` key
>   picks fixed-center versus moving-volume), so you can optimize a layout for the
>   observer model your install uses.
>
> For best-in-class fixed-observer quality, **SPCAP** (speaker-placement correction
> amplitude panning) is implemented as a selectable panner:
> `bwa_set_panner(e, BWA_PAN_SPCAP)` at load time (`spcap.c`, next to `dbap.c` behind
> the bus seam; it touches neither the seam nor its consumers). It weights every
> speaker by a smooth lobe `((1+cos)/2)^focus` toward the source's bearing from the
> listener, scales by a per-speaker **placement correction** `1/local-density` so a
> *cluster* of speakers doesn't pull the image toward it, then normalizes for
> constant power (with the same distance attenuation as DBAP). The correction is
> cached and rebuilt only when the listener or layout changes, so the per-voice
> solve stays alloc/lock-free.
>
> **Pick DBAP for a moving observer, SPCAP for a fixed one.** The layout tool's `V`
> key scores a layout for either model; its preview `B` key A/Bs the panners live.
>
> **VBAP** (`BWA_PAN_VBAP`, `vbap.c`) is also selectable: the 2-3 nearest speakers of
> the hull triangle containing the source carry it. It is the **sharpest** of the
> three, also fixed-observer. It needs a clean triangulation, so it suits a
> *regular* array; it has VBAP's direction-dependent source-width artifact and falls
> back to DBAP for a non-triangulable array. SPCAP stays the recommended
> fixed-observer default (smoother, robust on the irregular surveyed array); pick
> VBAP when pinpoint localization at the sweet spot is the priority. It shares the
> convex-hull + VBAP solve (`hull.c`) with AllRAD's decode. VBAP is the optimal
> *sparse* panner: with non-negative gains, the ℓ1-optimal speaker-gain solution is
> exactly VBAP over a Delaunay triangulation (Franck, Wang & Fazi 2017, IEEE TASLP).

Ambisonics is still the right tool for the **diffuse layer** (ambient beds,
reflections/reverb). Diffuse energy isn't sweet-spot-sensitive, so a fixed decode is
fine: decode it with a static matrix (see below). The material-driven build-out of
that layer is specified in [materials.md](./materials.md).

## The gain solve (`dbap_gains`)

Per voice, per frame *if dirty*: produce a gain vector `gtarget`, one entry per
speaker, from the source position, the listener position, and the speaker layout.
The vector is as long as the **layout's** speaker count (`bwa_get_channel_count()`, 4..26;
26 on the CAVE array), never a hard-coded 26.

Inputs:
- `src`: source position (room space).
- `lis`: listener position (room space). Orientation is **not** used by the array
  render.
- `layout`: the surveyed speaker positions (room space), loaded from `layout_path`.

The solve (listener-relative DBAP):
1. For each speaker `k`, compute the distance from the source as heard from the
   listener. The listener-aware form weights the source→speaker geometry referenced
   to the listener position, so off-center listeners get a correct distribution
   rather than one baked for the array center.
2. Convert distances to gains with the DBAP rolloff (a spatial-blur parameter `r`
   controls how many speakers share energy), then normalize for constant perceived
   power.
3. Apply per-source user gain and distance attenuation (source→listener).
4. Write `gtarget[0..count-1]`.

`r` (blur) and the distance-attenuation curve are the two tuning knobs. Expose them
in the layout/config so they can be dialed against the real array. If the layout
file omits `rolloff_r`, the loader derives it from the geometry: `0.25 ×` the mean
centroid→speaker distance (Sundstrom 2021's recommended 0.2–0.5 band), a defensible
starting point, not a substitute for dialing it by ear.

### Implemented formulation (`dbap.c`)

For each speaker `k` (positions in room space):

1. **Proximity**: blurred source→speaker distance `d_k = sqrt(|src − spk_k|² + r²)`,
   weight `p_k = 1 / d_k^a` (rolloff exponent `a = 2`). `r` controls how many speakers
   share energy and removes the singularity at `src == spk_k`.
2. **Listener-relative direction**: multiply by `dir_k = 0.5 + 0.5·cos∠(src−lis, spk_k−lis)`
   ∈ [0,1]. This emphasizes speakers in the source's *bearing from the listener*, so
   an off-center listener still gets the right distribution. If the source sits on
   the listener, the bearing is undefined and `dir_k = 1` for all: omnidirectional.
3. **Constant power**: normalize `g_k ← g_k / ‖g‖₂` so `Σ g_k² = 1`.
4. **Level**: scale all gains by `user_gain · atten(|src − lis|)`, where the inverse
   distance-attenuation is `atten = clamp((ref/max(d,ref))^rolloff, min, 1)`.

The result: a source *at* a speaker localizes to that speaker (smallest `d_k`,
`dir_k ≈ 1`), two speakers split a source between them, the total power is
`user_gain·atten` regardless of position, and the distribution shifts as the
listener moves.

> Implementation note: keep the math in `dbap.c` pure and listener-position-driven.
> A listener move dirties every voice (concurrency.md), so this runs for all active
> voices on move frames. Keep it allocation-free and tight.

## Dual-band panning (`bwa_set_dual_band`, off by default)

The panners normalize gains to constant **power** (`Σg² = gain²`, an energy-vector /
`rE` pan) across the whole band. That is right for high frequencies, where the ear
localizes by energy. Below ~700 Hz the ear localizes by summed **pressure** (the
velocity vector `rV`), where **amplitude** normalization (`Σg = gain`) gives a
sharper image: it maximizes `|rV|`.

Dual-band panning (SPAT's "VBP Dual-Band") splits each source at a 700 Hz
complementary 1st-order crossover (`hi = s − lo`, sums flat) and pans the low band
amplitude-normalized, the high band power-normalized.

`compute_gains` derives the low-band gains (`gtarget_lo`) every solve: the same
directions, rescaled so the amplitude sum `Σg` equals the power-normalized gains'
own magnitude `‖g‖₂` (`rt.c`, `sc = sqrt(gp)/gs`). Not the bare user gain: that
would cancel the distance attenuation and leave a distant source's bass at full
level. The *direction* is unchanged; the LF pressure sum matches the HF energy level
at every distance. The mixer reads the second gain set only when the `dual_band`
atomic is on, so it A/Bs live.

Dual-band is sweet-spot dependent (like VBAP). The dense array + small working area
is favorable, but whether it helps a *roaming* listener is a by-ear/rig call.

## Multi-listener compromise (`bwa_set_extra_listeners`)

Single-listener panning is exact for the tracked head and wrong for every other
occupant, and CAVEs usually hold a group. With extra listener positions supplied
(up to 3, commit-gated like the pose), `compute_gains` solves the same source once
per listener (each extra keeps its **own** SPCAP/VBAP cache, since those caches are
listener-keyed) and takes the per-speaker **energy mean**:
`g[k] = sqrt(mean_i g_i[k]²)`, the L2 barycentre of the individual renderings.
Constant power is preserved (the mean of the solves' powers), and every occupant
hears an image biased toward their own solve instead of one exact and the rest
wrong. Spread direction, Doppler, air absorption, the reverb-send distance, and the
headphone renders stay primary-relative (`BWA_PROFILE_BINAURAL` ignores extras
entirely; one head). Panner-agnostic, block-rate, one extra point solve per
listener per dirty voice.

## Gain ramping

`dbap_gains` writes `gtarget`. The mixer holds `gcur` and interpolates
`gcur → gtarget` across the block (per-sample, or a short per-block fade). Never
apply a new gain vector discontinuously: a position jump otherwise produces
audible zipper noise. This is a hard invariant.

## Per-speaker alignment

CAVE speakers sit at unequal distances from the working area, so the final output
stage corrects each channel after the mix and before the device write. This is the
spec's `align_speakers`, implemented as `align_process` in `align.c`, driven by
per-speaker values in the layout file (schema parsed in `layout.c`). Per channel,
in order:

- **Correction FIR** (`eq`, up to `BWA_EQ_TAPS` = 512 taps): the speaker-flattening
  inverse EQ written by `bwa_calibrate --eq`. Applied before the gain+delay.
- **LF room-EQ cuts** (`room_eq`, up to `BWA_ROOM_EQ_MAX` = 8 sections): RBJ peaking
  cuts for room modes, written by `--room-eq`. Static-listener installs only. Also
  before the gain+delay.
- **Gain trim**: per-speaker level equalization.
- **Delay line**: a short integer-sample delay aligning arrival times to a
  reference.

The gain trim + delay are not optional for a real array, but they are trivial DSP.
The two EQ stages are optional and bypass when absent. See
[calibration.md](./calibration.md) for how `bwa_calibrate` measures and writes all
four.

## Propagation effects (opt-in, per source)

Physically-motivated per-voice effects, toggled per emitter by the control client
(default off, phonon-free; see `docs/api.md`). The first two derive from the
source↔listener distance, recomputed each block.

**Doppler** renders the voice through its acoustic propagation delay, `distance / c`.
Each voice owns a fractional delay ring. The read tap glides toward the target delay
across the block, and *the glide rate is the resampling ratio* `1 − v_radial/c`:
the pitch shift falls out of the changing delay, so you never supply a velocity.
This is a **per-source** delay (propagation from the source), distinct from the
**per-speaker** align delay above (which equalizes arrival across the array); they
compose. The delay saturates past ~8 m (bounding the ring). Ring indices stay
integer (the fractional part is a separate float), so a voice running for hours
never loses sample precision.

**Air absorption** is a distance-driven one-pole low-pass on the direct path: high
frequencies roll off with distance (cutoff ≈ 18 kHz near, −650 Hz/m, ≥1.2 kHz).
Subtle in-room, pronounced for far virtual sources.

**Loudness compensation** (`bwa_source_set_loudness_comp`, opt-in) is the perceptual
counterpart: attenuation lowers level, and at lower levels the ear loses LF
sensitivity (the ISO 226 equal-loudness contours), so an attenuated source reads
*thin* as well as far. A one-pole LF shelf (~250 Hz) boosts by 0.4 dB per dB of
attenuation the panner applied (capped +8 dB): "far, not tinny". A stylization,
not physics; strict realism leaves it off.

Both ramp per sample (no zipper) and tap the reflection send *before* themselves.

**Source spread/size** gives a source angular width: a waterfall or a crowd
shouldn't collapse to a point. It runs in the per-block gain solve, not the sample
loop, renormalised to *the panner's own power*: widening redistributes energy
without re-levelling and keeps the centroid on the source direction. It is
panner-agnostic, and the new gains ramp like any other gain change. Two render
modes sit behind `bwa_set_spread_mode` (an atomic live A/B, like the panner switch):

- **Lobe** (default): the panner's point gains are blended toward a
  width-controlled lobe `(½(1+cosθ))^q` centered on the source direction (`q`
  shrinks as spread→1, widening the lobe). One solve: smooth and cheap, but the
  extent is a reshaping of gains solved for a *point*.
- **MDAP** (Pulkki 1999, multiple-direction amplitude panning): a ring of virtual
  sources around the source direction: 8 at the full cone angle (`spread`·90°)
  plus 4 offset at half, at the source's own distance, each panned with the
  *selected panner*, summed, and renormalised to the point solve's power. The
  extent is built from real panner solves, so it inherits the panner's character
  (VBAP stays sparse per direction, SPCAP stays placement-corrected) and sharpens
  the extent's edge. ~13× the gain-solve cost, block-rate + dirty-gated, so still
  cheap. At spread→0 the ring collapses onto the point solve, so the two modes
  meet continuously.

Both MDAP's ring and the spectral mode's band directions hang off an orthonormal
frame around the source direction. That frame is **parallel-transported** per voice
(project the previous frame off the new direction) rather than derived from a fixed
up-vector: the fixed-up branch flips the frame ~180° in one solve when a moving
source leaves the pole zone, teleporting the ring/band directions (Pulkki's
reference `vbap` external carries the same state for the same reason). The `rt`
test sweeps a wide source over the zenith and pins step-to-step continuity.

Either mode still feeds every speaker the **same signal**: coherent copies, which
collapse to phantom images and comb-filter *position-dependently* as the tracked
listener walks. **`bwa_set_decorrelation`** (off by default, live A/B) splits a spread source's
energy: the coherent share takes the normal path, the rest routes through a
per-speaker **sparse velvet-noise filter bank** (`rt.c`, ~30 signed taps jittered
over 30 ms with an exponential envelope, unit energy; Välimäki et al.'s
velvet-noise decorrelator, DAFx-17/18; time-domain, no FFT, no onset latency). The
split amplitude is `sqrt(spread)` and ramps per sample; power is conserved because
incoherent energy adds. The same filter bank renders the parametric bed's diffuse
stream (below).

**Near-listener widening** (`bwa_set_near_spread`, off by default): a source
approaching the head subtends a growing solid angle, but a point panner collapses
it into the nearest speaker and snaps it across the head as it passes through. With
a radius `R` set, every source's spread is floored at `1 − dist/R` in the gain
solve (untouched beyond `R`, fully wide at the head), and the widened part rides
the selected spread mode and the decorrelators. Sources flying through the room are
the common CAVE case this exists for.

**Metric source size** (`bwa_source_set_size`, radius in meters, 0 = point) is the
physical parametrization of the same machinery: the spread is floored at the angle
the radius subtends from the tracked listener, `asin(r/d)/(π/2)`, capped at 1 when
the listener is inside the source. A 2 m waterfall therefore *stays* 2 m wide as
the listener walks (an angular spread would change physical size with distance),
and a sized source subsumes the near-listener policy (engulfment = the `d < r`
case). The larger of spread and the size-derived floor wins; everything
downstream (lobe/MDAP, decorrelation, constant-power) is unchanged.

**Anisotropic extent** (`bwa_source_set_extent`, width + height each 0..1) is the
BS.2127-style refinement: a shoreline is wide but not tall, rain is tall but not
wide. The ring modes squash their virtual-source cap per axis (affine tangent
scaling: a 1×0 extent is a horizontal *arc* of real panner solves, never a
collapse), the lobe stretches its falloff on the extent ellipse. Width/height are
room-referenced, so anisotropic sources use the up-anchored frame (with its pole
ambiguity: "width" straight overhead is undefined, same as BS.2127) instead of the
transported one; equal extents are exactly the isotropic spread, same solve path.
The size/near floors apply to both axes. Pinned in the `rt` test (vertical-spill
contrast + iso-equality).

## Headphone renders: direct binaural and the array sim

Two headphone profiles share one decode; they answer different questions.

**`BWA_PROFILE_CAVE_SIM`** is the array audition: a **bus→stereo** transform. It
consumes the same speaker bus the array render does, so it auditions the actual
render: panner spread, alignment, gain staging, everything. Each bus channel is a
virtual speaker at its surveyed room direction; head orientation rotates the
virtual array. `BWA_PROFILE_CAVE_BOTH` runs this same transform as the rig's
headphone tap.

**`BWA_PROFILE_BINAURAL`** is the first-class headphone render. Point sources (and
their ISM reflection images) bypass the speaker panner: the mixer SH-encodes each
at its **true** listener-relative direction into a 16-channel direct field
(`rt.c`, the same `gcur→gtarget` ramp machinery; the coefficients ramp, so motion
never zippers), with the same user gain and layout distance curve the panner would
apply. None of the array's phantom-source spread reaches the ears. Ambisonic
**beds pass SH→SH** into the same field: one diagonal per channel
(`ambi_canon_to_phonon`: the `(-1)^|m|` axis flip times the orthonormal rescale)
instead of decode-to-speakers plus virtual-speaker re-encode. The **pathing
accumulator sums in raw** (it is already phonon-basis SH), which also puts head
orientation on the indirect arrivals, as it should. Only the synthesized-diffuse
taps (the FDN tail, the Steam reflection bed) still render to the speaker bus and
join the decode as virtual speakers. Dual-band, decorrelation, the speaker spread
modes, max-rE, the parametric bed renderer, and extra listeners are speaker-array
concerns and don't apply; spread maps to a per-degree taper toward omni
(energy-renormalized, `cos^l`). One decode serves every contribution.

With the SDK, the dry render goes one step further: **one `IPLBinauralEffect` per
voice** (mode 2, chosen at `bwa_start` when the phonon monitor and its per-voice
fleet exist). Each point voice's post-DSP mono block and true direction feed a
real per-source HRTF convolution: no ambisonic order ceiling on localization.
Spread **power-splits** the dry between the point tap (`sqrt(1−s)`) and the
tapered SH field (`sqrt(s)`), so both paths always exist and a spread change
crossfades through the gain solve instead of switching render paths. A recycled
voice slot resets its effect (generation-gated), so no overlap tail bleeds across
voices. Without the SDK (or if the fleet fails to build) the render stays on the
shared SH field: mode 1, the same 3rd-order path the tests pin.

Either way, the chain ends at real headphones, which are not acoustically flat:
`bwa_load_headphone_eq` runs an AutoEq correction on the final stereo of every
headphone profile (the headphone-side align stage; docs/api.md "Headphone
correction EQ"). Personalized SOFA HRTFs (`hrtf_path`) correct the ears, the EQ
corrects the transducer; they compose.

Two decode implementations sit behind the same seam:

- **Production (`steam_decode.c`, gated `BWA_HAVE_STEAMAUDIO`)**: ambisonics →
  Steam Audio HRTF decode, described below. The direct field sums straight into
  the virtual-speaker encode's SH scratch, same basis by construction
  (`ambi_encode_phonon`, shared with `rt.c`).
- **Fallback (`binaural.c`, no SDK)**: a simple lateral-projection pan for the bus
  channels (`gL² + gR² = 1`), plus two opposed cardioids at the ear axes on the
  direct field's first-order channels. No HRTF. It verifies routing and gross
  laterality, not timbre or externalization.

The production decode is efficient: do **not** run one HRTF convolution per bus
channel (26 of them on the CAVE array):
1. Treat each bus channel as a virtual speaker at its surveyed room **direction**
   relative to the listener. This is where head **orientation** enters: rotate the
   speaker directions with the head.
2. Encode those feeds into ambisonics: a fixed gain matrix from the speaker
   directions. Cheap.
3. Sum the direct field (`BINAURAL` only; it is already in this basis).
4. Do a single **ambisonics → binaural** decode (Steam Audio's ambisonics-binaural
   effect with the configured HRTF).

Convention details in the implemented encode (one function, `ambi_encode_phonon`
in `ambisonics.c`, used by the virtual-speaker matrix AND the direct-field solve
so they cannot drift apart):

- **Axes.** The phonon net-AmbiX map: front = `-z`room, left = `-x`room, up =
  `+y`room (`steam_decode.c` CONVENTION 1).
- **Normalization.** The engine encodes SN3D (AmbiX); phonon's
  `iplAmbisonicsDecodeEffect` decodes orthonormal real SH. Each ACN channel is
  rescaled per degree by `sqrt(2l+1)/sqrt(4π)` (`ambi_phonon_scale`,
  `ambisonics.c`).
- **m<0 sign: no fix-up.** phonon's real-SH m<0 convention **matches** the
  engine's encode on every channel (the xval golden pins `ambi_encode_sn3d`
  against phonon's own SH table, sin harmonics included). A negation of ACN
  1, 4, 5, 9, 10, 11 lived in `steam_decode.c` briefly, added to satisfy a
  DC-driven laterality test: the default HRTF's per-ear DC gains are laterally
  opposite its audible ILD, so the DC assertion passed exactly when the field was
  mirrored. If you touch any convention, run the `steam_decode` laterality test
  (a 660 Hz **tone**, never DC; right source → right ear, 180° flips);
  `test_ambi` only checks m≥0 and will not catch a mirror.

**Ambisonic order:** default to **3rd order (16 channels, 3D)** for the
encode/decode. This is the sweet spot for a 26-speaker array: order `N` uses
`(N+1)²` channels, so 3rd order is 16 and 4th is 25. By 4th order the ambisonic bus
is nearly as wide as doing the 26 HRTF convolutions directly, which defeats the
purpose; 1st–2nd order (4–9 ch) noticeably blurs the directionality the array is
there to reproduce. Expose the order as a config/build knob (alongside `r` and the
distance curve) so it can be traded against CPU on the monitor path, but 3rd order
is the baseline. The decode cost is fixed by the order, **independent of the source
count**, the right trade for the bus audition. `BINAURAL`'s dry render pays the
per-source cost instead where it buys localization: mode 2 convolves each point
voice through its own `IPLBinauralEffect` (O(voices), block-rate direction
updates, bilinear HRTF interpolation), with the shared field carrying the wide
shares, the beds, and the pathing. The field's 3rd-order ceiling then bounds only
the diffuse-ish content, where it doesn't hurt.

## Diffuse-bed decode (AllRAD versus EPAD)

The diffuse layer (ambisonic beds, the reflection bed) is rendered by a fixed
SH→speaker **decode matrix** applied per block (`build_bed_decode` / `mix_bed` in
`rt.c`), built from the speaker geometry at load time. Two decoders are selectable with
**`bwa_desc.bed_decoder`** (create-time); the plain **sampling (projection) decode**
`decode[s][k] = (2l+1)·Y_k^SN3D(dir_s)/N` (`ambi_sad_decode`) is **not** one of
them: exact on a perfectly uniform array, it over-energises dense regions on an
irregular one (every speaker radiates a fixed diffuse energy regardless of position),
which both selectable decoders dominate. It survives only as the automatic fallback
when a degenerate layout defeats the chosen build, and as the FDN's
non-triangulable fallback.

- **AllRAD (`BWA_DECODE_ALLRAD`, default, `allrad.c`)**: All-Round Ambisonic Decoding
  (Zotter & Frank 2012). Sampling-decode to a dense **uniform virtual layout** (a
  Fibonacci sphere), then **VBAP** each virtual loudspeaker onto the real array
  (its convex-hull triangulation), then energy-normalize to the sampling decode.
  The virtual layer is uniform so the decode is well-conditioned there; VBAP
  absorbs the real array's irregularity. Robust on a lopsided survey, at the cost
  of a heavier load-time build: a brute-force hull + VBAP over ~240 virtual
  directions. The audio thread still only applies the matrix.

  A pole with no real speaker within ~60° gets an **imaginary loudspeaker** (IEM
  AllRADecoder practice): it closes the triangulation at the hole and its decode
  share is *discarded*. Without it, a floor-less array's hull spans the nadir
  with triangles of bottom-ring speakers, and downward diffuse energy smears onto
  them; with it, energy aimed where no speaker exists is dropped. The cube grid's
  ~55° nadir gap stays under the threshold, so genuinely-covered poles are
  untouched.

- **EPAD (`BWA_DECODE_EPAD`, `epad.c`)**: Energy-Preserving Ambisonic Decoding
  (Zotter, Pomberger & Noisternig 2012). The decode is the **polar factor** of the
  transposed encode matrix, `D = c·Yᵀ(YYᵀ)^(-1/2)`: the constant-singular-value
  member of the pseudo-inverse family, which makes a panned plane wave's decoded
  **energy constant over direction**, by construction, not by approximation.
  Envelop-scale HOA venues reach the same goal through AllRAD; EPAD attacks it
  directly. Rank-deficient field components (a degenerate survey) truncate out of
  the inverse square root instead of amplifying. A 16×16 Jacobi eigensolve at load
  time (`xval` pins it against numpy's SVD polar factor); a degenerate array falls
  back to sampling. Loudness-versus-direction is EPAD's win (the `dsp` test measures
  CV ≈ 0.09 versus sampling's 0.95 on a clustered array); AllRAD tends to localize a
  touch sharper. Which sounds better on the real 26 is a by-ear A/B.

Validated against the cube grid + a deliberately clustered array (per-direction
energy CV / rE error): on the near-uniform cube AllRAD matches sampling (≈7% CV, a
few degrees); on the **clustered** array it cuts the loudness-versus-direction variance
from **91%→29%** and the localization error from **34°→18°**.

AllRAD doesn't touch the point-source panner (DBAP/SPCAP/VBAP). It is the
diffuse-layer counterpart to the placement correction those make for localized
sources. Its convex-hull + VBAP solve is factored into `hull.c`, shared with the
`BWA_PAN_VBAP` point panner.

### Near-field compensation: deliberately omitted

The SH→speaker decodes are plane-wave: no NFC-HOA distance-coding filters
(Daniel 2003, AES 23rd Int. Conf.). This is a decision, not an oversight, and the
error is quantified: at 3rd order on a ~2 m-radius array, skipping the compensation
costs **exactly 0 dB at the array center** (only order 0 contributes there at LF)
and **±2–6 dB below ~150–250 Hz off-center**; above ~250 Hz it vanishes. Three
things eat what's left: max-rE weighting tapers the higher orders where the error
lives; the room's Schroeder frequency (~200–300 Hz for a 3×3 m space) sits *above*
the entire effect band, so down there the physical room's modal field dominates
whatever the array synthesizes (the tracked room EQ's territory, and the same
"don't fight the room" logic as [calibration.md](./calibration.md)); and NFC's real
payoff (finite-distance *sources*) is the job the listener-relative point panner
already does. The parametric bed renderer sidesteps the wavefront question
entirely: it re-pans direction at the array shell on purpose. No mainstream
≤3rd-order decoder (IEM AllRADecoder, Resonance, Steam Audio) applies playback NFC
either. Revisit only if hardware calibration ever measures an off-center LF boost;
the fix is three fixed per-order IIR sections on the bed SH channels
(`sfs.td.nfchoa`'s matched-z realization is the reference).

## Parametric bed rendering (`bwa_set_bed_renderer`, live A/B)

Any matrix decode, sampling or AllRAD, has two limits on this rig: the array is
sparse for 3rd-order content (26 speakers on the CAVE, so directional material
blurs), and the decode is locked to the array center (walking off-center skews a
recorded field in exactly the way the engine's listener-relative panning was built
to avoid).

`BWA_BED_PARAMETRIC` renders beds the DirAC way (Pulkki's directional audio coding,
first-order, in 4 coarse time-domain bands instead of an STFT; `mix_bed` in
`rt.c`). Per band, the smoothed **intensity vector** of the bed's FOA channels
gives a direction and a **diffuseness** `ψ = 1 − |I|/E` (0 = a plane wave, 1 =
isotropic). Two streams render per band:

- **direct** (`√(1−ψ)`): the W signal, panned through the engine's own
  **listener-relative panner** at a virtual source on the array shell
  (`ref + R·doa`). The direct stream re-pans *per listener position*, so a recorded
  soundfield becomes **walkable**: correct directions and parallax off-center.
- **diffuse** (`√ψ`): the FOA band decoded through the bed matrix into the
  **velvet-noise decorrelators**: envelopment from incoherent speaker feeds, not a
  correlated copy on every speaker.

Both streams are loudness-matched to the matrix decode (a direction-averaged
plane-wave power reference computed with the decode), parameters ramp per block
(invariant 4), and the renderer crossfades per bed on the toggle, so the matrix
versus parametric comparison is a clean live A/B. Analysis is first-order (HO-DirAC
sectors are the upgrade path if band-level parameters prove too coarse); beds with
fewer than 4 channels stay on the matrix.

## Early reflections: image sources, panned like point sources (`ism.c`)

The FDN below renders the late tail; `ism.c` renders the **first-order specular
reflections**: the six wall bounces that carry room size and source distance.
Together they are the classic early+late hybrid, and neither needs phonon: the
engine has a complete acoustics path with no SDK at all.

The geometry is trivial for a shoebox (the room `bwa_scene_set_box` already
describes): mirroring the source across a face flips the one coordinate normal to
it. Each of the six images is then rendered as **a point source at its mirrored
position, through the engine's own listener-relative panner**, so a reflection has
the right *direction*, the panner's own distance attenuation over its longer path,
and, crucially, **parallax**: walk toward the wall and its reflection changes
direction and level as it physically must. A shared listener-centric bed (Steam's,
or the FDN's) cannot do that: it decodes one field around one point. This is the
engine's central thesis (re-solve per listener position) applied to reflections.

Per image: a gliding fractional delay (path/c; a moving source *bends* its
reflections), a one-pole HF damping derived from the material's high-versus-mid
absorption (walls eat treble, which is why a reflection sounds duller than the
direct sound), and a ramped gain vector from the panner. Order 1 only: higher
orders blend into the diffuse field within tens of ms, which is precisely what the
FDN renders for free. A source outside the room renders dry.

## Directional FDN reverb (`bwa_fdn_config`)

The reflection bed no longer *requires* phonon: a 16-line **feedback delay
network** (`fdn.c`, Householder feedback, two-band decay filters per line) can
take the reverb bus tap instead. Each line is assigned a Fibonacci-sphere
direction and rendered as a plane wave through the same SH→speaker bed decode, and the
per-line decay time scales with direction (`bwa_fdn_desc.decay_dir`/`decay_factor`):
**anisotropic decay**, the diagonal direction-domain case of the Directional FDN
(Alary/Politis/Schlecht, JAES 2019). Deterministic CPU, infinite tail, no rays or
IRs. The decay is a *design* parameter: don't set it from the room's measured
RT60: the real room adds its own reverb on top ([calibration.md](./calibration.md)). The
`fdn` test pins RT60 landing, the two-band split, anisotropy, and stability.

## Steam Audio usage

Via the **C API**, not the Unity/FMOD integration. Relevant pieces:
- `IPLSpeakerLayout` with `IPL_SPEAKERLAYOUTTYPE_CUSTOM`: the array as unit-length
  speaker directions. The integrations don't expose custom layouts; the C API does.
- Ambisonics encode + ambisonics→binaural decode for the monitor path.
- The Direct Effect (occlusion + transmission) on the per-source path, and
  reflections/reverb as a diffuse ambisonic bed decoded to the speakers; see
  [materials.md](./materials.md).

Steam Audio's own custom-layout **panning** is a simpler projection law than
VBAP/DBAP and is angular/center-listener. Use it for the binaural virtual-speaker
encode; it is *not* the array panner. The array panner is the engine's own
listener-relative DBAP.
