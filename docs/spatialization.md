# Spatialization

This doc defines most of the engine's spatial vocabulary. For a one-line lookup of any term here,
or of one this doc uses without defining, see [glossary.md](./glossary.md).

## Why DBAP, not ambisonics, for point sources

The observer moves across a ~3×3 m area inside the array. Ambisonics is a
single-sweet-spot format: correct only at the decode origin, with no real near-field
behavior. Off-center, the nearest speaker dominates and the image collapses. So the
engine pans localized point sources **object-based**. It recomputes the gains per
frame from the **tracked listener position**.

DBAP (Distance/Direction-Based Amplitude Panning) beats textbook VBAP here because it
works directly from speaker and source **positions**. It degrades gracefully when the
listener is off-center, instead of assuming a listener fixed at the array origin.

The formulation below is a house design. Its two load-bearing departures from
the original Lossius DBAP have published support. Sundstrom (I3DA 2021,
arXiv:2109.08704) documents how the original's convex-hull projection fails for
sources *outside* the array: projections landing on a hull vertex make distinct
positions produce identical gains, and total power "undulates wildly" across the
boundary. Sundstrom lands on the same shape as the fix, hull-free with a
reference-distance rolloff. The engine's solve never touches a hull, so both
failures are impossible by construction. The `dsp` test sweeps a source from the
center out through a corner speaker to 10 m and pins gain continuity, monotone
level, and exterior injectivity.

The sweet-spot claim is measured, not just argued. The layout tool's bed metric
(`bwa_bed_gains_batch`, the engine's real AllRAD/EPAD builds; see
[layout-schema.md](./layout-schema.md)) scores one co-optimized 26-speaker layout
at 2.4°/8.1° mean/worst [rE error](./glossary.md#re-error) for a sweet-spot listener and 14.4°/52.5° over
the 3×3 m roam, against 5.0°/25.5° for tracked DBAP on the same speakers. The
static decode is the best render on the array at the center, and it loses 3× to the
tracked panner everywhere else. The physics sets that scale. An order-N decode
reconstructs the field only within roughly N·c/(2πf) of the center, about 16 cm
at 1 kHz for 3rd order. No order 26 speakers can drive reaches a 3 m working
volume.

> **Fixed-observer installs are a supported mode.** The case above is the *moving*,
> tracked listener. A seated install needs no extra machinery. Leave tracking off and
> set the listener once to the sweet spot. You can also leave the pose unset: the
> default is the array centroid, the nominal listening point. DBAP is position-based, so it
> pans correctly for that fixed point, unlike an origin-only ambisonic decode. The
> layout tool's `V` key scores either observer model
> ([layout-schema.md](./layout-schema.md)).
>
> For best-in-class fixed-observer quality, **SPCAP** (speaker-placement correction
> amplitude panning) is a selectable panner: `bwa_set_panner(e, BWA_PAN_SPCAP)`
> (`spcap.c`, next to `dbap.c` behind the bus seam). It weights every speaker by a
> smooth lobe `((1+cos)/2)^focus` toward the source's bearing from the listener.
> Then it scales by a per-speaker **placement correction** `1/local-density`, so a
> *cluster* of speakers doesn't pull the image toward it. Finally it normalizes for
> constant power, with the same distance attenuation as DBAP. The engine caches the
> correction and rebuilds it only when the listener or layout changes, so the
> per-voice solve stays alloc/lock-free.
>
> **The lobe width follows the array.** `focus` used to be a compile-time 12: right
> for this 26-speaker grid, arbitrary anywhere else. The default now comes from the
> geometry. Measure the mean angle from each speaker to its nearest neighbor. Then
> pick the exponent that puts the lobe 6 dB down in energy at that angle,
> `n = ln(0.25) / ln((1+cos delta)/2)`. A sparse array gets a broad lobe, a dense one
> a tight lobe. The cube grid sits at 37.5 degrees of mean spacing and lands on 12.7,
> close to the constant it replaced. `bwa_spcap_focus_default` computes the same
> number for any speaker set, so a tool can show what an in-progress array implies.
>
> **Both exponents are live knobs**, not file settings:
> `bwa_set_spcap_focus(e, focus, density)` retunes the render on the next block. Pass
> 0 or less on either argument to revert that one to its default. Like the rest of the live
> A/B group they persist nowhere on disk. Focus rewrites the gain vector of every
> source, so the engine re-solves even sources that never move (rt.c bumps a panner
> generation the mixer compares against, rather than writing voice state from the
> control thread). The same knobs reach the offline scoring path
> (`bwa_panner_gains_batch`, same "0 or less means this array's default" sentinel).
> They are inert under DBAP and VBAP, which have no lobe.
>
> **VBAP** (`BWA_PAN_VBAP`, `vbap.c`) is also selectable: the 2-3 speakers of the
> hull triangle containing the source carry it. Sharpest of the three, also
> fixed-observer. It needs a clean triangulation (it falls back to DBAP for a
> non-triangulable array) and has VBAP's direction-dependent source-width artifact,
> so SPCAP stays the recommended fixed-observer default on an irregular survey. Pick
> VBAP when pinpoint sweet-spot localization is the priority. It shares the
> convex-hull + VBAP solve (`hull.c`) with AllRAD's decode. It is also the optimal
> *sparse* panner: with non-negative gains, the l1-optimal speaker-gain solution is
> exactly VBAP over a Delaunay triangulation (Franck, Wang and Fazi 2017, IEEE TASLP).
>
> **Pick DBAP for a moving observer, SPCAP for a fixed one.** The layout tool's
> preview `B` key A/Bs the panners live, with the focus and density sliders beside
> it.

### Array holes, and why the panner has no imaginary speakers

The real array does not surround the listener. Speakers mount in the band between the
CAVE screen cube and the truss (`constraints.json`): a **barrel**, open at both poles.
From seated ear height the worst case is roughly a 46 degree hole at nadir and 54 at
zenith. The final geometry should beat the zenith figure, since a few speakers can poke
through the top. The nadir hole is the floor and does not improve. The hull closes those
holes with big triangles of distant speakers, so a source aimed into one is carried by
speakers far apart. At exact nadir on a symmetric barrel the containing triangle is an
antipodal pair 113 degrees apart: a split image, not a phantom.

The obvious fix is [allrad.c](../src/allrad.c)'s imaginary pole speaker, which the bed
decode already uses, or VISR's `<virtualspeaker>` with explicit routes. **It was tried and
it makes point-source localization worse.** Measured on a jittered barrel, rE direction
error against the intended bearing:

| Source elevation | Plain hull | Imaginary speaker, routed to the rim | Imaginary speaker, share discarded |
|---|---|---|---|
| -60 deg | 14.0 | 26.4 | 29.8 |
| -70 deg | 13.7 | 31.0 | 39.7 |
| -80 deg | 9.1 | 26.0 | 49.6 |
| -90 deg (nadir) | 4.3 | 1.5 | 90 (silent) |

An imaginary speaker at the pole is a triangulation vertex, so it claims a share of every
direction in the hole and drags it toward the pole. Routing that share to the rim smears
it. Discarding it fades the source out. Only the exact pole improves, and only slightly.
The plain hull already interpolates across the hole about as well as the geometry allows.

The distinction that resolves this: BS.2127 and VISR use virtual loudspeakers for
**channel-format compatibility**, rendering content authored for a nominal channel that is
not physically present. That virtual speaker sits where content actually is. It is not a
general hole-filler for arbitrary 3D directions, and using it as one costs accuracy.
AllRAD's imaginary speaker is a different case again: a diffuse bed aimed into a hole
*should* lose that energy, so discarding is right there and wrong here.

What the plain hull still does badly is image **compactness**, not direction. In the
mid-hole band it spreads a source across speakers up to 113 degrees apart, while keeping
the direction roughly right. The fix is not a virtual speaker. It is flooring the source's
spread as it enters the hole, the way near-listener widening already floors it for close
sources. A source with no speaker anywhere near it is genuinely not a point, and rendering
it as an honest wide source beats pretending.

`bwa_set_hole_spread(e, strength)` turns that on. It is off by default (`strength` 0), so
an array you never asked about renders exactly as before. Per voice the engine measures one
angle: the **gap** from the source bearing to the nearest speaker bearing, both seen from
the tracked listener. The spread floor follows

```
floor = strength * clamp( (gap - knee) / (90 degrees - knee), 0, 1 )
```

`knee` is the array's own mean nearest-neighbor speaker angle, the same geometry SPCAP's
lobe width comes from (`layout_mean_speaker_spacing`). The feature therefore scales itself
to any layout instead of hard-coding an angle. Both ends of the ramp carry meaning:

- Below the knee the floor is exactly 0. On an array that surrounds the listener, no
  direction is ever a full speaker spacing away from a speaker **as seen from the
  reference**, so the floor is 0 there. Watch the frame mismatch though. The knee is an
  array property measured from `Layout.ref`, while the gap follows the live listener.
  Angular gaps stretch as you leave the middle. On the default grid the worst gap is 27.5
  degrees at center, 39.7 at 0.7 m out and 61 at a corner, against a 37.5 degree knee. A
  hole-free array can therefore still derive a floor off-center. That is the feature
  working rather than misfiring: from a corner those bearings really do have no speaker
  near them.
- At a 90 degree gap the floor is fully wide. A source with no speaker in its own
  hemisphere has nothing point-like left to render, so diffuse is the honest answer.
- The slope scales too. A sparse array has a wider knee, so less angular room between
  covered and fully wide, so it widens faster. That is what a sparse array needs.

The floor composes with the metric-size and near-listener floors as a **max**, so the
widest honest claim wins. It then feeds the ordinary spread machinery: it follows the
selected spread mode, it decorrelates when decorrelation is on, and the gains ramp like any
other solve. `strength` scales the result. 1.0 is the honest width, less is a partial
widening, more exaggerates (clamped at 2). It does nothing in `BWA_PROFILE_BINAURAL`, which
has no speakers and so no holes.

**It weakens CAP on the sources it widens, by design.** CAP's correction strength is
`1 - spread`, so raising the floor lowers it: a source deep in a hole ends up with most of
its ITD correction withdrawn. That is the right behavior, because a deliberately wide source
has no single bearing whose ITD is worth fixing. Know about it before you A/B the two knobs
on the rig. Turning hole spread up quietly turns CAP down in exactly the directions where
the array is worst.

The cost is small and the cache self-invalidates. The per-listener part, the unit speaker
directions, rebuilds only when the listener moves or the layout changes, the same idiom
SPCAP and VBAP use. The knee rebuilds only on a layout change. Per voice it is one dot
product per speaker plus one arc cosine.

Measured on a barrel of 8 perimeter positions at 3 heights with the listener at 1.4 m: mean
speaker spacing 33.7 degrees, nadir gap 59.0 degrees, floor 0.45, which is a 40 degree
half-width. The zenith gap is 53.7 degrees and floors at 0.36. The default 26-speaker cube
grid never exceeds a 27.3 degree gap in any direction, well inside its 37.5 degree knee, so
its floor is 0 everywhere. Both numbers come out of the `dsp_test` suite, which prints them.

Still untested by ear on the rig. The mapping is a defensible first cut, not a measured
optimum, and `strength` is the knob to A/B on rig day.

Ambisonics is still the right tool for the **diffuse layer** (ambient beds,
reflections/reverb). Diffuse energy isn't sweet-spot-sensitive, so a fixed decode is
fine: decode it with a static matrix (see below). [materials.md](./materials.md)
specifies the material-driven build-out of that layer.

## The gain solve (`dbap_gains`)

Per voice, per frame *if dirty*: produce a gain vector `gtarget`, one entry per
speaker, from the source position, the listener position (the array render does
**not** use orientation), and the surveyed speaker layout. The vector is as long
as the **layout's** speaker count (`bwa_get_channel_count()`, 4..26), never a
hard-coded 26.

`r` (blur) and the distance-attenuation curve are the two tuning knobs. The layout
file exposes them, so you can dial them against the real array. If the layout omits
`rolloff_r`, the loader derives it from the geometry: `0.25 ×` the mean
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
complementary 1st-order crossover (`hi = s − lo`, sums flat). It pans the low band
amplitude-normalized and the high band power-normalized.

`compute_gains` derives the low-band gains (`gtarget_lo`) every solve: the same
directions, rescaled so the amplitude sum `Σg` equals the power-normalized gains'
own magnitude `‖g‖₂` (`rt.c`, `sc = sqrt(gp)/gs`). Not the bare user gain: that
would cancel the distance attenuation and leave a distant source's bass at full
level. The *direction* is unchanged. The LF pressure sum matches the HF energy level
at every distance. The mixer reads the second gain set only when the `dual_band`
atomic is on, so it A/Bs live.

Dual-band is sweet-spot dependent (like VBAP). The dense array + small working area
is favorable, but whether it helps a *roaming* listener is a by-ear/rig call.

## Compensated amplitude panning (`bwa_set_dual_band_cap`, off by default)

Dual-band's low band aims the velocity vector at the source and takes whatever
`|rV| < 1` the geometry gives. The shortfall is direction-dependent, so the rendered
ITD is short of a real source's by a varying amount. The image also shifts when you
turn your head. CAP (Menzies and Fazi) fixes that by constraining the one quantity
the ear actually reads below the crossover.

Below ~700 Hz the ear localizes by ITD, and ITD depends only on the **interaural
component** of the incident field. The rigid-sphere low-frequency diffraction factor
multiplies every incident wave equally, so it cancels out of the ratio and leaves one
scalar constraint on the gains:

```
rV . e  ==  u_s . e        e = interaural axis, u_s = source direction
```

Matching one scalar is satisfiable where matching a 3-vector is not. Any two speakers
straddling the target hit it exactly, so the rendered ITD equals a real source's at
that bearing. Because `e` is fixed in the head frame while the speakers are not,
re-solving per block against the current `e` **is** the head-rotation compensation.

`cap.c` applies this as a projection on top of the selected panner, not as a fourth
panner. Weighting the correction by the seed gains collapses it to a multiplicative
tilt `g_k = g0_k * (1 - lam * a_k)`, which means a speaker the panner left silent
stays exactly silent. CAP never recruits a distant speaker to buy an ITD, and a VBAP
seed stays on its triangle. Facing the source the constraint is already satisfied and
CAP is a no-op, so it reduces to whatever panner seeded it. It fades out with
`bwa_source_set_spread`, because an engulfing source has no single bearing to fix.

**Two known exclusions.** Spectral spread (`BWA_SPREAD_SPECTRAL`) replaces the
single-path output stage with per-band targets, and those bands never see the
projection. A spread source in that mode gets plain dual-band on its low bands and
no ITD correction at all, rather than the documented fade. Projecting them is not a
one-liner: the scattered bands each sit off the point bearing and need their own ITD
target. Both modes are off by default, so it is a corner, but a real one. Second, the
offline scoring path (`bwa_panner_gains_batch`) never solves a low band, so CAP cannot
be swept there the way `--focus` sweeps SPCAP.

`cap.c` computes everything in room space on purpose. The SPCAP and VBAP direction
caches key on listener **position**. Rotate the interaural axis into room space
instead of rotating the speakers into head space, and a head that only turns
invalidates no panner cache. A seated listener costs one 3-vector rotate plus one dot
product per speaker per block.

**What the array can and cannot do.** `rV` is a convex combination of speaker
directions, so no non-negative gain vector renders an ITD more lateral than the most
lateral speaker the panner lit. CAP clamps the target into that range and saturates
there rather than diverging. On the default grid from the center this binds only when
the head turns to put the source within about 11 degrees of the interaural axis while
the nearest speaker sits 15 degrees off it. Over the `dsp_test` yaw sweep that is 2 of
24 angles, and the worst residual is 0.017 against dual-band's 0.404.

CAP requires dual-band, since the low band is the only thing it touches. It is the one
engine feature that reads head orientation into the speaker path, so it wants a real
tracked pose. With an identity head it still corrects ITD for a listener facing room
ahead, but the head-rotation benefit is exactly what you are not getting.

This implementation deliberately leaves out the near-field ILD arm of the published
method (one first-order filter per image). It needs per-speaker frequency-dependent
gain, which would make this a render mode rather than a gain-vector modifier and
break the bus seam. The
near-field proximity shelf and near-listener widening cover adjacent ground.

### How this differs from the reference implementation

VISR (`librcl/cap_gain_calculator`, `libpanning/CAP`) is the authors' own CAP, and it
uses the **same constraint**: its `a[i] = rL . (r[i] - rI)` is this file's
`a_k = u_k.e - u_s.e`. Two things differ, both deliberate.

**The objective.** VISR minimizes total energy subject to the constraint plus a unit
gain sum, which has the closed form `g_i = (-b*a_i + c) / (c*n - b*b)`. That makes CAP a
standalone panner. This engine instead minimizes the *seed-weighted* change from the
selected panner's gains, which makes CAP a modifier. It inherits DBAP's or VBAP's image
and only corrects ITD, and it reduces exactly to the seed when the constraint is already
satisfied.

**Non-negativity.** The min-energy solution is free to go **negative**, which is how it
reaches ITDs more lateral than any real speaker. This engine's multiplicative tilt on a
non-negative seed cannot, which is where the lateral limit above comes from. That is a
deliberate trade. Negative low-frequency gains are a crosstalk-cancellation-flavored
trick that is sharply position-dependent and can blow up. VISR needs a singularity
guard plus two gain clamps (1.5 pre-compensation, 10 overall) to hold it together. A
CAVE listener who walks and turns is the worst case for that, so this implementation
takes the conservative arm and saturates instead.

### Open question: the CAP band

CAP currently rides the dual-band low band, so its crossover is 700 Hz. That number comes
from the Gerzon rV/rE argument, not from ITD. The ITD literature puts the useful range
higher, and the three sources disagree. VISR's own header says "valid in ITD frequency
range ~(0,1000) Hz", Zhao et al. (2025) low-pass at 1500 Hz, and this engine splits at
700 Hz. Since CAP is justified by ITD and not by rV, there is a real case for decoupling
the two crossovers and running CAP wider than the panner's dual-band split. Untested: it
needs a listening or measurement call on the rig, not a decision from theory.

## Multi-listener compromise (`bwa_set_extra_listeners`)

Single-listener panning is exact for the tracked head and wrong for every other
occupant, and CAVEs usually hold a group. With extra listener positions supplied
(up to 3), `compute_gains` solves the same source once per listener. Each extra
keeps its **own** SPCAP/VBAP cache, since those caches are listener-keyed. The solve
then takes the per-speaker **energy mean** `g[k] = sqrt(mean_i g_i[k]²)`, the L2
barycenter of the individual renderings. The solve preserves constant power, and every
occupant hears an image biased toward their own solve instead of one exact and the
rest wrong. Everything else (spread direction, Doppler, air absorption, reverb-send
distance, the headphone renders) stays primary-relative. Call contract:
[api.md](./api.md#extra-listeners-multi-occupant-compromise).

## Gain ramping

`dbap_gains` writes `gtarget`. The mixer holds `gcur` and interpolates
`gcur → gtarget` across the block (per-sample, or a short per-block fade). Never
apply a new gain vector discontinuously: a position jump otherwise produces
audible zipper noise. This is a hard invariant.

## Per-speaker alignment

CAVE speakers sit at unequal distances from the working area, so the final output
stage corrects each channel after the mix and before the device write. This is the
spec's `align_speakers`. `align.c` implements it as `align_process`, driven by
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

### Re-aligning to the tracked listener (`bwa_set_tracked_align`, off by default)

The reference the trims align to is **fixed**, so the array is time-coherent at one point
and progressively less so as the listener walks away from it. Turn this on and the output
stage re-references that alignment onto the tracked listener, so coherence follows the
head. VISR has the same component (`librcl/listener_compensation`).

The correction is pure geometry. Per speaker, against `Layout.ref`:

```
dref = |speaker - ref|
dlis = |speaker - listener|
extra delay (frames) = (dref - dlis) * rate / c
extra gain  (linear) = dlis / dref
```

Sign: walk **toward** a speaker and its wavefront arrives early and loud, so the fix
delays it further and turns it down. The engine then shifts the delay set so its minimum
over the array is zero. That does three things. It makes the correction purely relative,
since a delay common to every speaker is just latency. It keeps every target non-negative,
so the aligner only ever reads further back in its ring. It also makes a listener standing
exactly at `ref` bit-exact identity rather than approximately so. The cost of that choice
is added latency: at 1 m off-center the whole array runs about 280 frames (5.8 ms) later
than it does at the reference.

Both corrections ride on top of the layout's own trims and are slewed, never stepped.
Nothing here touches the gain solve, so it composes with every panner and re-solves
nothing.

#### Why it is off by default

Every delay change is a resampling event on that speaker. A walking listener means all 26
delay lines gliding at once, which is 26 simultaneous Doppler shifts on everything the
array plays. That is a global, always-audible failure mode, unlike a local one. Two guards
keep it usable, and both are yours to tune:

- **Dead zone** (`dead_zone_m`, default 0.05). The engine recomputes nothing until the head
  moves further than this from where it solved the standing targets. Tracker position
  output is jittery. Without a dead zone the array glides permanently. 5 cm of slack costs
  0.15 ms of residual arrival error, well under the ~1 ms scale where precedence and
  comb-filtering start to read, and about 3% of the correction at a 1.5 m excursion.
- **Rate limit** (`slew_frames_per_s`, default about 63 at 48 kHz). The ceiling on how fast
  a speaker's delay may change. That rate over the sample rate **is** the resampling ratio,
  so it is what bounds the pitch shift. The default is stated as a listener closing speed
  it can follow, 0.45 m/s, which works out to 0.13% of shift (2.3 cents). A brisk walk at
  1.4 m/s therefore outruns it on purpose. The alignment lags, then catches up when the
  listener slows. Stale alignment is the cheap failure. Warble is not.

The mechanics mirror the tracked room EQ ([`calibration.md`](./calibration.md) -> "`--room-eq-grid`"):
`rt.c` re-derives the targets each block
from the live listener position and hands them to `align.c`, which slews toward them.
Because it reads the active listener position it fires on both listener paths, the
committed pose and the internal tracker, which writes that field directly.

Off is exact, not approximate. While no channel is displaced, `align_process` runs its
original integer delay tap and the output is bit-identical to a build without the feature.
The fractional (linear-interpolated) tap engages only once something leaves identity, and
disengages again the moment everything lands back. Toggling either way glides.

Two limits to know. Corrections saturate about 4 m from the reference, which is the delay
headroom the ring reserves. The per-speaker level trim is clamped to +/-6 dB, because a
listener standing on top of a speaker asks for an unbounded cut on it and an unbounded
boost on the far side.

**That level clamp binds inside the working area, so treat the gain half as partial.** On
the shipped grid a listener 1 m off center already asks for -9.5 dB on the nearest speaker
against a -6 dB clamp, and at 1.5 m they are standing on one. The delay half stays exact
across the same excursion. Saturating is the safe failure, but it does mean the level
re-reference is fully applied only near the middle. The two halves ride one toggle. If the
rig says the level half hurts, splitting it off is a small change.

**The added latency is not reported anywhere.** `bwa_get_output_latency_frames` does not
include the tracked-align shift, so a client doing audio-video sync against the DSP clock
will drift by up to those ~280 frames as the listener walks. The drift reverses as they
return. Nothing else in the engine has position-dependent latency. Worth knowing before
you use this in a session that also cares about AV sync.

Untested on hardware. The by-ear question is whether the improvement in coherence is worth
any warble the rate limit still lets through, and that is a rig call, not a theory call.

## Propagation effects (opt-in, per source)

Physically-motivated per-voice effects, toggled per emitter (default off,
phonon-free). Full call contracts and parameter values:
[api.md](./api.md#propagation-effects-control-thread-per-frame). The design points
that belong here:

- **Doppler** renders the voice through its acoustic propagation delay,
  `distance / c`, on a per-voice fractional delay ring. The read tap glides toward
  the target delay across the block, and *the glide rate is the resampling ratio*
  `1 − v_radial/c`. The pitch shift falls out of the changing delay, so you never
  supply a velocity. This is a **per-source** delay, distinct from the
  **per-speaker** align delay above. The two compose. The delay saturates past ~8 m
  (bounding the ring), and ring indices stay integer (the fraction is a separate
  float), so a voice running for hours never loses sample precision.
- **Air absorption** is a distance-driven one-pole low-pass on the direct path
  (cutoff ≈ 18 kHz near, −650 Hz/m, ≥1.2 kHz floor).
- **Loudness compensation** is the perceptual counterpart of distance attenuation:
  at lower levels the ear loses LF sensitivity (ISO 226), so an attenuated source
  reads *thin* as well as far. An LF shelf restores 0.4 dB per dB of attenuation,
  capped +8 dB. A stylization, not physics: strict realism leaves it off.

All ramp per sample (no zipper) and tap the reflection send *before* themselves.

**Source spread/size** gives a source angular width: a waterfall or a crowd
shouldn't collapse to a point. It runs in the per-block gain solve, not the sample
loop, renormalized to *the panner's own power*. Widening redistributes energy
without re-leveling and keeps the centroid on the source direction. It is
panner-agnostic, and the new gains ramp like any other gain change. Three render
modes sit behind `bwa_set_spread_mode` (an atomic live A/B, like the panner switch):

- **Lobe** (default): the engine blends the panner's point gains toward a
  width-controlled lobe `(½(1+cosθ))^q` centered on the source direction (`q`
  shrinks as spread→1, widening the lobe). One solve: smooth and cheap, but the
  extent is a reshaping of gains solved for a *point*.
- **MDAP** (Pulkki 1999, multiple-direction amplitude panning): a ring of virtual
  sources around the source direction: 8 at the full cone angle (`spread`·90°)
  plus 4 offset at half, at the source's own distance, each panned with the
  *selected panner*, summed, and renormalized to the point solve's power. The
  extent is built from real panner solves, so it inherits the panner's character
  (VBAP stays sparse per direction, SPCAP stays placement-corrected) and sharpens
  the extent's edge. ~13× the gain-solve cost, block-rate + dirty-gated, so still
  cheap. At spread→0 the ring collapses onto the point solve, so MDAP and lobe
  meet continuously.
- **Spectral** (Zotter/Frank phantom-source widening): the source splits into 6
  complementary bands (crossovers at 250, 700, 1800, 4500 and 10000 Hz) and each
  band pans to its *own* direction inside the cone, the low band staying put. Every
  band gain is a real panner solve, so the extent is panner-true as MDAP's is. The
  difference is what arrives at the speakers. Different frequencies come from
  different speakers, so there are no coherent copies to collapse into a phantom or
  to comb as the listener walks. That is width with no decorrelation noise, and it
  is the mode to reach for when a wide source has to survive a walking listener.
  About 6 band filters plus 6 gain sets per wide voice. Point sources pay nothing.

Both MDAP's ring and the spectral mode's band directions hang off an orthonormal
frame around the source direction. That frame is **parallel-transported** per voice
(project the previous frame off the new direction) rather than derived from a fixed
up-vector. The fixed-up branch flips the frame ~180° in one solve when a moving
source leaves the pole zone, which teleports the ring/band directions. Pulkki's
reference `vbap` external carries the same state for the same reason. The `rt`
test sweeps a wide source over the zenith and pins step-to-step continuity.

Either mode still feeds every speaker the **same signal**: coherent copies, which
collapse to phantom images and comb-filter *position-dependently* as the tracked
listener walks. **`bwa_set_decorrelation`** (off by default, live A/B) splits a spread source's
energy: the coherent share takes the normal path, the rest routes through a
per-speaker **sparse velvet-noise filter bank** (`rt.c`, ~30 signed taps jittered
over 30 ms with an exponential envelope, unit energy; Välimäki et al.'s
velvet-noise decorrelator, DAFx-17/18; time-domain, no FFT, no onset latency). The
split amplitude is `sqrt(spread)` and ramps per sample. Power is conserved because
incoherent energy adds. The same filter bank renders the parametric bed's diffuse
stream (below).

**Near-listener widening** (`bwa_set_near_spread`, off by default): a source
approaching the head subtends a growing solid angle, but a point panner collapses
it into the nearest speaker and snaps it across the head as it passes through. With
a radius `R` set, every source's spread is floored at `1 − dist/R` in the gain
solve (untouched beyond `R`, fully wide at the head). The widened part rides
the selected spread mode and the decorrelators. Sources flying through the room are
the common CAVE case this exists for.

**Metric source size** (`bwa_source_set_size`, radius in meters, 0 = point) is the
physical parametrization of the same machinery: the spread is floored at the angle
the radius subtends from the tracked listener, `asin(r/d)/(π/2)`, capped at 1 when
the listener is inside the source. A 2 m waterfall therefore *stays* 2 m wide as
the listener walks (an angular spread would change physical size with distance).
A sized source also subsumes the near-listener policy (engulfment = the `d < r`
case). The larger of spread and the size-derived floor wins. Everything
downstream (lobe/MDAP, decorrelation, constant-power) is unchanged.

**Anisotropic extent** (`bwa_source_set_extent`, width + height each 0..1) is the
BS.2127-style refinement: a shoreline is wide but not tall, rain is tall but not
wide. The ring modes squash their virtual-source cap per axis (affine tangent
scaling: a 1×0 extent is a horizontal *arc* of real panner solves, never a
collapse). The lobe stretches its falloff on the extent ellipse. Width/height are
room-referenced, so anisotropic sources use the up-anchored frame (with its pole
ambiguity: "width" straight overhead is undefined, same as BS.2127) instead of the
transported one. Equal extents are exactly the isotropic spread, same solve path.
The size/near floors apply to both axes. Pinned in the `rt` test (vertical-spill
contrast + iso-equality).

## Headphone renders: direct binaural and the array sim

Two headphone profiles share one decode; they answer different questions.

**`BWA_PROFILE_CAVE_SIM`** is the array audition: a **bus→stereo** transform. It
consumes the same speaker bus the array render does, so it auditions the actual
render: panner spread, alignment, gain staging, everything. Each bus channel is a
virtual speaker at its surveyed room direction. Head orientation rotates the
virtual array. `BWA_PROFILE_CAVE_BOTH` runs this same transform as the rig's
headphone tap.

**`BWA_PROFILE_BINAURAL`** is the first-class headphone render. Point sources (and
their ISM reflection images) bypass the speaker panner. The mixer SH-encodes each
at its **true** listener-relative direction into a 16-channel direct field
(`rt.c`, the same `gcur→gtarget` ramp machinery; the coefficients ramp, so motion
never zippers). It applies the same user gain and layout distance curve the panner
would. None of the array's phantom-source spread reaches the ears. Ambisonic
**beds pass SH→SH** into the same field: one diagonal per channel
(`ambi_canon_to_phonon`: the `(-1)^|m|` axis flip times the orthonormal rescale)
instead of decode-to-speakers plus virtual-speaker re-encode. The **pathing
accumulator sums in raw** (it is already phonon-basis SH), which also puts head
orientation on the indirect arrivals, as it should. Only the synthesized-diffuse
taps (the FDN tail, the Steam reflection bed) still render to the speaker bus and
join the decode as virtual speakers. Dual-band, decorrelation, the speaker spread
modes, max-rE, the parametric bed renderer, and extra listeners are speaker-array
concerns and don't apply. Spread maps to a per-degree taper toward omni
(energy-renormalized, `cos^l`). One decode serves every contribution.

With the SDK, the dry render goes one step further: **one `IPLBinauralEffect` per
voice** (mode 2, chosen at `bwa_start` when the phonon monitor and its per-voice
fleet exist). Each point voice's post-DSP mono block and true direction feed a
real per-source HRTF convolution: no ambisonic order ceiling on localization.
Spread **power-splits** the dry between the point tap (`sqrt(1−s)`) and the
tapered SH field (`sqrt(s)`). Both paths therefore always exist, and a spread change
crossfades through the gain solve instead of switching render paths. A recycled
voice slot resets its effect (generation-gated), so no overlap tail bleeds across
voices. Without the SDK (or if the fleet fails to build) the render stays on the
shared SH field: mode 1, the same 3rd-order path the tests pin.

Either way, the chain ends at real headphones, which are not acoustically flat:
`bwa_load_headphone_eq` runs an AutoEq correction on the final stereo of every
headphone profile (the headphone-side align stage; docs/api.md "Headphone
correction EQ"). Personalized SOFA HRTFs (`hrtf_path`) correct the ears. The EQ
corrects the transducer. The two compose.

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
  DC-driven laterality test. The default HRTF's per-ear DC gains are laterally
  opposite its audible ILD, so the DC assertion passed exactly when the field was
  mirrored. If you touch any convention, run the `steam_decode` laterality test
  (a 660 Hz **tone**, never DC; right source → right ear, 180° flips);
  `test_ambi` only checks m≥0 and will not catch a mirror.

**Ambisonic order:** default to **3rd order (16 channels, 3D)** for the
encode/decode. This is the right balance for a 26-speaker array: order `N` uses
`(N+1)²` channels, so 3rd order is 16 and 4th is 25. By 4th order the ambisonic bus
is nearly as wide as doing the 26 HRTF convolutions directly, which defeats the
purpose. 1st–2nd order (4–9 ch) noticeably blurs the directionality the array is
there to reproduce. Expose the order as a config/build knob (alongside `r` and the
distance curve) so you can trade it against CPU on the monitor path, but 3rd order
is the baseline. The decode cost is fixed by the order, **independent of the source
count**, the right trade for the bus audition. `BINAURAL`'s dry render pays the
per-source cost instead where it buys localization: mode 2 convolves each point
voice through its own `IPLBinauralEffect` (O(voices), block-rate direction
updates, bilinear HRTF interpolation), with the shared field carrying the wide
shares, the beds, and the pathing. The field's 3rd-order ceiling then bounds only
the diffuse-ish content, where it doesn't hurt.

## Diffuse-bed decode (AllRAD versus EPAD)

A fixed SH→speaker **decode matrix** renders the diffuse layer (ambisonic beds, the
reflection bed) per block (`build_bed_decode` / `mix_bed` in `rt.c`). The engine
builds that matrix from the speaker geometry at load time. Two decoders are selectable
with **`bwa_desc.bed_decoder`** (create-time). The plain **sampling (projection) decode**
`decode[s][k] = (2l+1)·Y_k^SN3D(dir_s)/N` (`ambi_sad_decode`) is **not** one of
them. It is exact on a perfectly uniform array, but it over-energizes dense regions on
an irregular one, since every speaker radiates a fixed diffuse energy regardless of
position. Both selectable decoders dominate it. It survives only as the automatic fallback
when a degenerate layout defeats the chosen build, and as the FDN's
non-triangulable fallback.

- **AllRAD (`BWA_DECODE_ALLRAD`, default, `allrad.c`)**: All-Round Ambisonic Decoding
  (Zotter and Frank 2012). Sampling-decode to a dense **uniform virtual layout** (a
  Fibonacci sphere), then **VBAP** each virtual loudspeaker onto the real array
  (its convex-hull triangulation), then energy-normalize to the sampling decode.
  The virtual layer is uniform, so the decode is well-conditioned there. VBAP
  absorbs the real array's irregularity. Robust on a lopsided survey, at the cost
  of a heavier load-time build: a brute-force hull + VBAP over ~240 virtual
  directions. The audio thread still only applies the matrix.

  A pole with no real speaker within ~60° gets an **imaginary loudspeaker** (IEM
  AllRADecoder practice): it closes the triangulation at the hole and its decode
  share is *discarded*. Without it, a floor-less array's hull spans the nadir
  with triangles of bottom-ring speakers, and downward diffuse energy smears onto
  them. With it, energy aimed where no speaker exists is dropped. The cube grid's
  ~55° nadir gap stays under the threshold, so genuinely-covered poles are
  untouched.

- **EPAD (`BWA_DECODE_EPAD`, `epad.c`)**: Energy-Preserving Ambisonic Decoding
  (Zotter, Pomberger and Noisternig 2012). The decode is the **polar factor** of the
  transposed encode matrix, `D = c·Yᵀ(YYᵀ)^(-1/2)`: the constant-singular-value
  member of the pseudo-inverse family, which makes a panned plane wave's decoded
  **energy constant over direction**, by construction, not by approximation.
  Envelop-scale HOA venues reach the same goal through AllRAD. EPAD attacks it
  directly. Rank-deficient field components (a degenerate survey) truncate out of
  the inverse square root instead of amplifying. A 16×16 Jacobi eigensolve at load
  time (`xval` pins it against numpy's SVD polar factor). A degenerate array falls
  back to sampling. Loudness-versus-direction is EPAD's win (the `dsp` test measures
  CV ≈ 0.09 versus sampling's 0.95 on a clustered array). AllRAD tends to localize a
  touch sharper. Which sounds better on the real 26 is a by-ear A/B.

Validated against the cube grid + a deliberately clustered array (per-direction
energy CV / rE error): on the near-uniform cube AllRAD matches sampling (≈7% CV, a
few degrees). On the **clustered** array it cuts the loudness-versus-direction variance
from **91%→29%** and the localization error from **34°→18°**.

AllRAD doesn't touch the point-source panner (DBAP/SPCAP/VBAP). It is the
diffuse-layer counterpart to the speaker-placement correction SPCAP makes for
localized sources. Its convex-hull + VBAP solve is factored into `hull.c`, shared with the
`BWA_PAN_VBAP` point panner.

### Near-field compensation: deliberately omitted

The SH→speaker decodes are plane-wave: no NFC-HOA distance-coding filters
(Daniel 2003, AES 23rd Int. Conf.). This is a decision, not an oversight, and the
error is quantified. At 3rd order on a ~2 m-radius array, skipping the compensation
costs **exactly 0 dB at the array center** (only order 0 contributes there at LF)
and **±2–6 dB below ~150–250 Hz off-center**. Above ~250 Hz it vanishes. Three
things eat what's left. First, max-rE weighting tapers the higher orders where the
error lives. Second, the room's Schroeder frequency (~200–300 Hz for a 3×3 m space)
sits *above* the entire effect band, so down there the physical room's modal field
dominates whatever the array synthesizes (the tracked room EQ's territory, and the
same "don't fight the room" logic as [calibration.md](./calibration.md)). Third,
NFC's real payoff (finite-distance *sources*) is the job the listener-relative point
panner already does. The parametric bed renderer sidesteps the wavefront question
entirely: it re-pans direction at the array shell on purpose. No mainstream
≤3rd-order decoder (IEM AllRADecoder, Resonance, Steam Audio) applies playback NFC
either. Revisit only if hardware calibration ever measures an off-center LF boost.
The fix is three fixed per-order IIR sections on the bed SH channels
(`sfs.td.nfchoa`'s matched-z realization is the reference).

## Parametric bed rendering (`bwa_set_bed_renderer`, live A/B)

Any matrix decode, sampling or AllRAD, has two limits on this rig. The array is
sparse for 3rd-order content: 26 speakers on the CAVE, so directional material
blurs. The decode is also locked to the array center, and walking off-center skews a
recorded field in exactly the way the engine's listener-relative panning was built
to avoid.

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
plane-wave power reference computed with the decode). Parameters ramp per block
(invariant 4), and the renderer crossfades per bed on the toggle, so the matrix
versus parametric comparison is a clean live A/B. Analysis is first-order (HO-DirAC
sectors are the upgrade path if band-level parameters prove too coarse). Beds with
fewer than 4 channels stay on the matrix.

## Early reflections: image sources, panned like point sources (`ism.c`)

The FDN below renders the late tail. `ism.c` renders the **first-order specular
reflections**: the six wall bounces that carry room size and source distance.
Together they are the classic early+late hybrid, and neither needs phonon. The
engine has a complete acoustics path with no SDK at all.

The geometry is trivial for a shoebox (the room `bwa_scene_set_box` already
describes): mirroring the source across a face flips the one coordinate normal to
it. The engine then renders each of the six images as **a point source at its mirrored
position, through the engine's own listener-relative panner**. A reflection therefore
has the right *direction*, the panner's own distance attenuation over its longer path,
and, crucially, **parallax**. Walk toward the wall and its reflection changes
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
take the reverb bus tap instead. The engine assigns each line a Fibonacci-sphere
direction and renders it as a plane wave through the same SH→speaker bed decode. The
per-line decay time scales with direction (`bwa_fdn_desc.decay_dir`/`decay_factor`):
**anisotropic decay**, the diagonal direction-domain case of the Directional FDN
(Alary/Politis/Schlecht, JAES 2019). Deterministic CPU, infinite tail, no rays or
IRs. The decay is a *design* parameter. Don't set it from the room's measured
RT60, because the real room adds its own reverb on top
([calibration.md](./calibration.md)). The `fdn` test pins RT60 landing, the two-band
split, anisotropy, and stability.

## Steam Audio usage

Via the **C API**, not the Unity/FMOD integration (the C API supports custom
speaker layouts; the integrations do not; see
[architecture.md](./architecture.md)). One boundary worth stating here: Steam
Audio's own custom-layout **panning** is a simpler projection law than VBAP/DBAP
and is angular/center-listener. It serves the binaural virtual-speaker encode. It
is *not* the array panner. The array panner is the engine's own listener-relative
DBAP. Occlusion, reflections, and pathing: [materials.md](./materials.md).
