# Glossary

The domain vocabulary this repo's docs use, in one place. Every entry is short and says what the
term means **for a decision**, then links to the doc that owns the explanation. Nothing here
replaces a deep doc: [api.md](./api.md) is the manual, and the specs below it own their subjects.

Formulas and constants carry a `file:line` citation. Check them against the code rather than against
this page.

**Not covered here.** The threading and lifetime vocabulary (SPSC ring, command ring, commit
snapshot, generation handle, retire-ack, seqlock, dirty flag) belongs to
[concurrency.md](./concurrency.md), which defines all of it in one model. Go there.

[api.md](./api.md#coordinates-and-units) owns the naming rules for units for the same reason. The
short version, because it changes how you read every call name below: a unit appears in a NAME only
when the quantity has two live units. Time is the only one, so time-valued names say which
(`_frames` on a getter, `seconds` or `_s` on a parameter). Distances, frequencies, angles and gains
have no competitor. They carry the unit on the value instead (`radius_m`, `xover_hz`, `yaw_rad`),
never on the call. The one hard rule is that a decibel value must say `_db`, because linear is the
unmarked default everywhere.

## By topic

Use this when you do not yet know the term's name. Entries are grouped so the trade-offs sit
together: rE beside rV, the three panners in one place, the three reverb paths in another.

- [Panning and the array render](#panning-and-the-array-render) - the three panners, their knobs,
  and what "listener-relative" buys.
- [Energy-vector metrics and layout scoring](#energy-vector-metrics-and-layout-scoring) - how a
  layout gets a number, and why two of those numbers move in opposite directions.
- [Ambisonics, beds and decoders](#ambisonics-beds-and-decoders) - the diffuse layer, its
  conventions, and the decoders that put it on speakers.
- [Source width: spread, extent, decorrelation](#source-width-spread-extent-decorrelation) - the
  three render modes for a source that is not a point.
- [Acoustics: reflections, reverb and materials](#acoustics-reflections-reverb-and-materials) -
  early versus late, and which of the three paths to run.
- [Binaural and headphone renders](#binaural-and-headphone-renders) - the two headphone profiles
  and the decode they share.
- [Per-source propagation effects](#per-source-propagation-effects) - distance-driven per-voice DSP.
- [Calibration and the acoustic survey](#calibration-and-the-acoustic-survey) - what
  `bwa_calibrate` measures and writes.
- [Validation vocabulary](#validation-vocabulary) - what `bwa_validate` measures, and the engine's
  own coinages for reading it.

## Index

**A** [Absorption](#absorption), [ACN](#acn), [Active-intensity DOA](#active-intensity-doa),
[Air absorption](#air-absorption), [Air temperature](#air-temperature), [AllRAD](#allrad),
[AmbiX and FuMa](#ambix-and-fuma), [Ambisonic order](#ambisonic-order),
[Angular miss](#angular-miss), [Anisotropic decay](#anisotropic-decay),
[Array-sim monitor](#array-sim-monitor), [Azimuth reference](#azimuth-reference)

**B** [Badness map](#badness-map), [Baked reflections](#baked-reflections), [Bed](#bed),
[Bed metric](#bed-metric), [Bending loss](#bending-loss), [Blur](#blur),
[Bootstrap interval](#bootstrap-interval)

**C** [CAP (compensated amplitude panning)](#cap-compensated-amplitude-panning), [Capsule integrity](#capsule-integrity), [Capsule survey](#capsule-survey), [Cell](#cell),
[Channel order](#channel-order), [Comb depth](#comb-depth), [Comb quality](#comb-quality),
[Condition](#condition), [Constant power](#constant-power), [Correction EQ](#correction-eq),
[Coverage shell](#coverage-shell)

**D** [DBAP](#dbap), [Deconvolution](#deconvolution), [Decorrelation](#decorrelation),
[Delay trim](#delay-trim), [Density](#density), [Diffuseness](#diffuseness), [DirAC](#dirac),
[Direct binaural field](#direct-binaural-field), [Direct channel route](#direct-channel-route),
[Directivity](#directivity),
[Distance attenuation](#distance-attenuation), [Doppler](#doppler), [Drift check](#drift-check),
[Dual-band panning](#dual-band-panning)

**E** [Early reflections](#early-reflections), [EPAD](#epad), [Exponential sweep](#exponential-sweep),
[Extent](#extent), [Extra listeners](#extra-listeners)

**F** [FDN](#fdn), [First-order ceiling](#first-order-ceiling), [Focus](#focus),
[Frank spread](#frank-spread)

**G** [Gain trim](#gain-trim), [GDOP](#gdop), [Guard](#guard)

**H** [Headphone correction EQ](#headphone-correction-eq),
[Hole-aware spread floor](#hole-aware-spread-floor), [Hot capsule](#hot-capsule),
[HRTF](#hrtf), [Hybrid reverb](#hybrid-reverb)

**I** [ILD](#ild), [Image-source method](#image-source-method),
[Imaginary loudspeaker](#imaginary-loudspeaker)

**L** [Laterality check](#laterality-check), [Leash](#leash),
[Listener-centric versus listener-relative](#listener-centric-versus-listener-relative),
[Listener-relative panning](#listener-relative-panning),
[Lobe mode](#lobe-mode), [Loudness compensation](#loudness-compensation)

**M** [Matched-cell contrast](#matched-cell-contrast), [max-rE](#max-re), [MDAP](#mdap),
[Mode strength](#mode-strength)

**N** [Near-field proximity boost](#near-field-proximity-boost),
[Near-listener widening](#near-listener-widening), [NFC-HOA](#nfc-hoa)

**O** [Observer model](#observer-model), [Occlusion](#occlusion)

**P** [Parametric bed rendering](#parametric-bed-rendering), [Pathing](#pathing),
[Perceptual weighting](#perceptual-weighting), [Phantom](#phantom),
[Phantom collapse](#phantom-collapse), [Physical floor](#physical-floor),
[Physical reference arm](#physical-reference-arm), [Pin](#pin),
[Placement correction](#placement-correction), [Precisely wrong](#precisely-wrong)

**R** [rE](#re), [rE error](#re-error), [rE magnitude](#re-magnitude),
[Reflection bed](#reflection-bed), [Rendering term](#rendering-term), [Residual](#residual),
[Reverb send](#reverb-send), [Room EQ](#room-eq), [RT60](#rt60), [rV](#rv)

**S** [Sampling decode](#sampling-decode), [Scattering](#scattering),
[Schroeder frequency](#schroeder-frequency), [Simulate](#simulate), [Size](#size),
[SN3D and N3D](#sn3d-and-n3d), [SOFA](#sofa), [Solve position](#solve-position),
[SPCAP](#spcap), [Spectral widening](#spectral-widening), [Speed of sound](#speed-of-sound),
[Spread](#spread), [SRP-PHAT](#srp-phat), [Stimulus](#stimulus), [Survey spread](#survey-spread),
[Sweet spot](#sweet-spot)

**T** [Tracked listener alignment](#tracked-listener-alignment),
[Tracked room EQ](#tracked-room-eq), [Tracked versus fixed solve](#tracked-versus-fixed-solve),
[Transmission](#transmission), [Transported frame](#transported-frame),
[Trilateration](#trilateration)

**V** [VBAP](#vbap), [Virtual-speaker encode](#virtual-speaker-encode)

---

## Panning and the array render

The three point-source panners and the knobs that shape them. Full treatment:
[spatialization.md](./spatialization.md#why-dbap-not-ambisonics-for-point-sources).

### Blur

DBAP's `rolloff_r`, the `r` in the blurred source-to-speaker distance
`d_k = sqrt(|src - spk_k|^2 + r^2)` (`src/dbap.c:21`, `src/dbap.c:34`). Larger `r` spreads a source
over more speakers. It also removes the singularity when a source lands exactly on a speaker, so it
is not optional. Omit `dbap.rolloff_r` from the layout file and the loader derives it as `0.25 x`
the mean centroid-to-speaker distance (`src/layout.c:326`), which is a starting point to dial
against the real array, not a finished tuning. Schema:
[layout-schema.md](./layout-schema.md#fields).

### CAP (compensated amplitude panning)

`bwa_set_dual_band_cap` (Menzies and Fazi), a projection on top of the selected panner that
corrects the [dual-band](#dual-band-panning) low band's **interaural time difference** for the
tracked head **orientation**. The constraint is one scalar, `rV . e == u_s . e` (`src/cap.c:57`),
so the ITD comes out exact and stays exact as the head turns, which plain dual-band does not do.
A speaker the panner left silent stays silent. With the head facing the source, it reduces to the
selected panner. It saturates at the most lateral speaker lit rather than going negative. See
[spatialization.md](./spatialization.md).

### Constant power

Normalizing a gain vector so `sum g_k^2 = gain^2`. Every panner here ends this way
(`src/dbap.c:47`, `src/spcap.c:69`), because at high frequencies the ear localizes by energy. The
consequence to hold onto: constant power is the **rE-optimal** normalization. It is the wrong one
below about 700 Hz, which is what [dual-band panning](#dual-band-panning) exists to fix.

### DBAP

Distance-based amplitude panning, the engine's production panner and the only one built for a
**moving** listener. Per speaker it combines a blurred proximity weight `1/d_k^2` with a
listener-relative directional weight `0.5 + 0.5*cos` and then normalizes for constant power
(`src/dbap.c:22`, `src/dbap.c:34-47`). It never touches a convex hull, so the classic Lossius
hull-projection failures outside the array cannot happen. Pick it whenever the listener is tracked.
See [spatialization.md](./spatialization.md#implemented-formulation-dbapc).

### Density

SPCAP's placement-correction exponent. It sets how sharply `sum cos^density` over the front
hemisphere counts a speaker's local neighbors (`src/spcap.c:34`). The default is a flat 2.0 with no
geometry link at all (`src/layout.h:95`), unlike [focus](#focus). It is a live knob
(`bwa_set_spcap_focus`). It invalidates SPCAP's cached correction when it moves, so dragging it is
more expensive than dragging focus. Inert under DBAP and VBAP.

### Distance attenuation

The source-to-listener level curve, `clamp((ref/max(d,ref))^rolloff, min, 1)`
(`src/layout.h:98`, `src/layout.h:101`). One implementation serves every panner, the per-source
override, and loudness compensation's tracker, so there is exactly one curve in the system. It is
separate from [occlusion](#occlusion) and from the reflection sim's own distance handling, which
stay off precisely so nothing counts distance twice.

### Dual-band panning

Splitting each source at a 700 Hz complementary first-order crossover (`src/rt.c:65`) and
normalizing the low band for **amplitude** (`sum g = gain`, which maximizes [rV](#rv)) while the
high band keeps [constant power](#constant-power). Off by default, a live A/B. It is sweet-spot
dependent like VBAP, so whether it helps a roaming listener is a rig call, not a settled one. See
[spatialization.md](./spatialization.md#dual-band-panning-bwa_set_dual_band-off-by-default).

### Extra listeners

Up to three additional listening positions. The solve runs once per listener and takes the
per-speaker **energy mean** `g[k] = sqrt(mean_i g_i[k]^2)`. Every occupant then gets an image biased
toward their own solve, instead of one person being exact and the rest wrong. Constant power
survives. Spread direction, Doppler, air absorption and the headphone renders stay
primary-relative. See
[spatialization.md](./spatialization.md#multi-listener-compromise-bwa_set_extra_listeners) and
[api.md](./api.md#extra-listeners-multi-occupant-compromise).

### Focus

SPCAP's lobe exponent: the per-speaker weight is `((1+cos)/2)^focus` toward the source's bearing
from the listener (`src/spcap.c:60`). The default comes from the array's mean
nearest-neighbor angle, clamped to 1..64 (`src/layout.c:58-61`). The 26-speaker cube grid lands
on 12.7. It is the knob that trades image tightness against [comb depth](#comb-depth), and
[validation.md](./validation.md#focus-and-where-the-sweep-has-power) puts numbers on the trade.
Trap: the moment the default stopped being the integer 12, `powf` of a slightly negative lobe
with a fractional exponent returned NaN and silenced whole gain vectors. The code clamps the lobe
at 0 now (`src/spcap.c:64`). Any future non-integer exponent reopens that class of bug.

### Listener-relative panning

Solving the gain vector from the **tracked listener's** position every block, rather than from the
array origin. It is the engine's central thesis: an order-3 decode reconstructs the field only
within roughly 16 cm of its center at 1 kHz, and the CAVE listener roams about 3x3 m. The
[image-source method](#image-source-method) applies the same idea to reflections. See
[spatialization.md](./spatialization.md#why-dbap-not-ambisonics-for-point-sources).

### Placement correction

SPCAP's `1/local-density` per-speaker weight (`src/spcap.c:30-36`), the thing that stops a
**cluster** of speakers from pulling an image toward it. The engine caches it and rebuilds it only
when the listener or the layout changes, which is what keeps the per-voice solve allocation-free. It
is the point-source counterpart of what [AllRAD](#allrad) does for the diffuse layer.

### Solve position

The listener position a panner computed its gains *for*, as distinct from where a listener actually
stands. They coincide only for a tracked renderer. Keeping them separate is the whole reason
`bwa_validate` can measure something a sweet-spot measurement cannot, and the reason the layout
tool's scoring changed. See
[validation.md](./validation.md#the-seam-solve-position-versus-microphone-position) and
[layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### SPCAP

Speaker-placement correction amplitude panning, the recommended **fixed-observer** panner
(`src/spcap.c`). It weights every speaker by a smooth [focus](#focus) lobe, scales by the
[placement correction](#placement-correction), then normalizes for constant power. The lobe is
never identically zero across the sphere, so a source over a layout hole fades rather than cutting
out. Pick SPCAP for a seated install, DBAP for a roaming one.

### Sweet spot

The one listening position a fixed decode or a fixed panner solve is correct at. Measuring only
there is the failure this repo keeps guarding against: a tracked renderer and a fixed one are
identical at the sweet spot, so a sweet-spot measurement is silent about the difference that
matters. See [validation.md](./validation.md#what-it-answers).

### Tracked versus fixed solve

`tracked = 1` solves at the listener's real position (what DBAP does every block); `tracked = 0`
solves once at the sweet spot and never corrects (what an SPCAP or VBAP install does at load).
Both the layout tool's scoring and `bwa_validate` carry this as an explicit axis. Measured:
tracking fixes horizontal displacement and does not fix height. See
[validation.md](./validation.md#what-it-has-found-so-far).

### VBAP

Vector-base amplitude panning: the 2 or 3 speakers of the hull triangle containing the source carry
it, and every other speaker is silent (`src/vbap.c:34`). It is the sharpest of the three panners and
also fixed-observer. It needs a clean triangulation, and it falls back to DBAP on a non-triangulable
array (`src/vbap.c:30`, `src/vbap.c:32`). It shares `hull.c` with AllRAD's decode. Judge a VBAP
layout by [Frank spread](#frank-spread), not by [rE error](#re-error).

## Energy-vector metrics and layout scoring

The cheap geometric proxies `bwa_layout_tool` climbs, and the vocabulary of its optimizer. How much
to trust them is measured in [validation.md](./validation.md#what-it-has-found-so-far).

### Badness map

The layout tool's `M` view: a grid of **listener** positions through the working volume, each scored
over a spread of directions and drawn as semitransparent voxels. It answers "where can somebody
stand", where the [coverage shell](#coverage-shell) answers "which directions work from here".
Toggle the [observer model](#observer-model) while watching it and a fixed solve draws an island
around the sweet spot. See [layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### Bed metric

The `AMBI (AllRAD)` row on the layout tool's scoreboard: plane waves at infinity through the
engine's real bed decode build (`bwa_bed_gains_batch`), scored over the same shell as the panners.
A bed wants a good **quadrature** of the sphere, which the point-source scores cannot see. Grade it
with the decoder and the max-rE setting your install actually ships, or you are scoring a sibling of
your render. See [layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### Bootstrap interval

A percentile bootstrap confidence interval on a **median**, fixed seed so a reported interval is
reproducible (`src/valid.h:256-258`). Medians rather than means throughout, because localization
error is heavy-tailed and a mean follows the few directions that fail badly. "The interval excludes
zero" is the claim worth making. See [validation.md](./validation.md#reading-the-output).

### Condition

Two different things wear this name, so check which doc you are in.

- **Layout tool**: a named bundle of optimizer objective knobs (worst weight, focus weight,
  elevation weight), a scoring-shell elevation band, an azimuth wedge, and a
  [leash](#leash). Three ship: `3d`, `horizontal`, `visual`. Conditions chain as **stages**, each
  seeding the next. See [layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).
- **`bwa_validate`**: one panner at one SPCAP [focus](#focus). A `--focus` list gives SPCAP one
  condition per value and leaves DBAP and VBAP at one each, so a sweep costs only what it measures.
  See [validation.md](./validation.md#the-session-shape).

### Coverage shell

The layout tool's `C` overlay: a shell of source **directions** around the observer, shaded to show
where the array is weak. `G` switches its metric between the geometric nearest-speaker gap and the
selected panner's per-direction [rE error](#re-error). It is direction-indexed, so it cannot tell
you where in the room to stand; that is the [badness map](#badness-map).

### Frank spread

The modeled perceived source **width** in degrees, `186.4 * (1 - |rE|) + 10.7`
(`examples/layout_tool.cpp:554`, `src/valid.c:365`), a function of the energy vector's **length**
alone. [rE error](#re-error) reads the same vector's *direction*, so raising SPCAP
[focus](#focus) can improve spread and worsen direction at once. Against acoustic measurement,
rE direction predicts DBAP cells and Frank spread predicts VBAP cells; the layout tool defaults
its focus weight per panner on exactly that basis. Numbers:
[validation.md](./validation.md#what-it-has-found-so-far).

### Guard

An optimizer constraint: while climbing the target panner, reject any move that lets a second
panner's cost slip more than `tol` (default 0.5, roughly degrees) above its stage-start value. You
cannot climb two objectives at once, but you can climb one **inside the other's feasible set**. The
guard baseline is wherever the stage starts, so guarding from an unoptimized layout protects almost
nothing. See [layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### Leash

A cap on how far any speaker may drift from where the optimizer stage started. The room constraints
do not make it redundant: the feasible shell between the CAVE screens and the room walls is thin
radially but long tangentially. Without a leash, a speaker slides meters around the perimeter away
from its rigged position for no score gain. Measured, a 0.75 m leash scored the same as a free
run. See [layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### Observer model

Whether a layout is scored at the sweet spot alone (`fixed`) or averaged over a 27-point grid
across the working volume (`moving`, the default). The two do not transfer symmetrically: a
sweet-spot-optimized layout scored under the roam came out **worse than the unoptimized dome**,
while the reverse is benign. Optimize `fixed` only for a permanently seated install. See
[layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### Perceptual weighting

The layout tool's rE error is **not** a plain great-circle angle by default: `loc_err_deg`
(`examples/layout_tool.cpp:439`) scales the elevation component by `elev_wt` (default 0.3), with
near-vertical targets falling back to the raw angle. `src/valid.c:361` computes the plain `acos`
instead, so a layout-tool degree and a `valid_re_proxy` degree are **not the same unit** unless
you turn the weight off. Do not compare them casually.

### Pin

Per-speaker authoring state confining a speaker to a slab about the ear plane (`pin_slab_m`,
default plus or minus 0.3 m). When an allocation is a *requirement*, pin it rather than weighting
it: measured, unpinned staged runs erode the in-slab population by half or more. Pins ride the
layout file; the engine ignores them. See
[layout-schema.md](./layout-schema.md#authoring-with-bwa_layout_tool).

### rE

The Gerzon **energy vector**: the gain-squared-weighted sum of unit speaker directions seen from the
listener, `rE = sum_k g_k^2 * dir_k / sum_k g_k^2` (`examples/layout_tool.cpp:537-552`,
`src/valid.c:341-351`). Its direction models where a listener localizes the phantom above roughly
700 Hz and its length models how tight the image is.

The limitation worth stating up front: `rE` is a **static statistic over a gain vector**. It knows
nothing about arrival times or frequency, which is exactly why you cannot derive
[comb depth](#comb-depth) from it, and why it needed a microphone.

### rE error

The angle between the direction a source should subtend from the listener and the direction
[rE](#re) points. It is what the layout tool's Score board, `--score`, the coverage overlay and the
optimizer cost all climb. Read [perceptual weighting](#perceptual-weighting) before comparing two
rE-error numbers, and read [Frank spread](#frank-spread) before trusting it on VBAP.

### rE magnitude

`|rE|` in 0..1, computed as `rl / esum` and clamped (`examples/layout_tool.cpp:552`). 1 means a
single speaker carries the source; small values mean energy is smeared over many. It is the sole
input to [Frank spread](#frank-spread).

### rV

The **velocity vector**, the amplitude-weighted (not energy-weighted) direction sum. It models
low-frequency localization, where the ear reads summed pressure. The engine never computes `rV`
explicitly. It appears as the reason [dual-band panning](#dual-band-panning) normalizes the sub-700
Hz band for amplitude, and as the reason [max-rE](#max-re)'s band-split variant leaves the low band
untapered (`src/rt.c:2244`).

## Ambisonics, beds and decoders

The diffuse layer. Point sources are panned; beds, reflections and reverb are ambisonic. Full
treatment: [spatialization.md](./spatialization.md#diffuse-bed-decode-allrad-versus-epad).

### ACN

Ambisonic Channel Number, the channel ordering the engine uses: channel `k` carries degree
`l = floor(sqrt(k))`, running W, Y, Z, X, then the five second-order channels, then the seven
third-order ones (`src/ambisonics.c:9-31`). Every SH loop in the codebase recovers `l` that way, so
`floor(sqrtf(k))` is the idiom to look for.

### AllRAD

All-Round Ambisonic Decoding (Zotter and Frank 2012), the **default** bed decoder (`src/allrad.c`):
sampling-decode to a dense uniform virtual layout (240 Fibonacci-sphere directions), VBAP each
virtual loudspeaker onto the real array's hull, then energy-normalize. The uniform virtual layer
keeps the decode well-conditioned; VBAP absorbs the real array's irregularity. Cost is a heavier
load-time build only. Measured results:
[spatialization.md](./spatialization.md#diffuse-bed-decode-allrad-versus-epad).

### AmbiX and FuMa

The two ambisonic file conventions `bwa_load_ambix` accepts. AmbiX is [ACN](#acn) ordering with
[SN3D](#sn3d-and-n3d) normalization and is the engine's internal convention. FuMa is the older
ordering and normalization. The engine converts it on load. See
[api.md](./api.md#ambisonic-beds-control-thread).

### Ambisonic order

`N`, where the channel count is `(N+1)^2`. The engine defaults to **3rd order, 16 channels**
(`BWA_AMBI_CH`), the sweet spot for a 26-speaker array (4th order's 25 channels nearly match the
26 HRTF convolutions it exists to avoid; 1st to 2nd visibly blurs). The reflection bed runs
**lower** order (default 1) on purpose: diffuse energy carries spaciousness, not primary
localization. See
[spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim).

### Bed

An ambisonic asset played as a whole soundfield rather than as a positioned source: ambience, music,
a recorded scene. Beds decode through a fixed SH-to-speaker matrix built at load, or through the
[parametric bed renderer](#parametric-bed-rendering). In the direct binaural profile a bed passes
SH to SH into the [direct binaural field](#direct-binaural-field) with one diagonal per channel
(`src/ambisonics.c:178`) instead of decoding to speakers and re-encoding. See
[api.md](./api.md#ambisonic-beds-control-thread).

### Diffuseness

Two related meanings, both "how plane-wave-like is this field", both 0..1 with 0 a clean plane
wave. The first is the DirAC bed analysis `psi = 1 - |I|/E` (`src/rt.c:2319`), which splits a band
into direct `sqrt(1-psi)` and diffuse `sqrt(psi)` streams. The second is the trust number
`zylia_intensity_doa` returns beside a direction. A high value there means you measured the
reverberant tail, so the direction is not believable. Diffuseness reads the **opposite** way to
[comb quality](#comb-quality), where 1 is good.

### DirAC

Directional audio coding (Pulkki). The analysis model behind the
[parametric bed renderer](#parametric-bed-rendering): per band, take a direction plus a
[diffuseness](#diffuseness) from the first-order channels and render two streams. The engine runs it
first-order in 4 coarse time-domain bands with one-pole crossovers at 200, 800 and 3200 Hz
(`src/rt.c:82-84`) rather than through an STFT, which keeps it block-rate and latency-free.

### EPAD

Energy-Preserving Ambisonic Decoding (Zotter, Pomberger and Noisternig 2012), the opt-in bed
decoder (`src/epad.c`): the **polar factor** of the transposed encode matrix,
`D = c * Y^T (YY^T)^(-1/2)`, built by a 16x16 Jacobi eigensolve at load. The trade against
[AllRAD](#allrad): EPAD makes a panned plane wave's decoded energy constant over direction **by
construction**, while AllRAD tends to localize a touch sharper. Which wins on the real 26 is a
by-ear A/B. Measured numbers:
[spatialization.md](./spatialization.md#diffuse-bed-decode-allrad-versus-epad).

### Imaginary loudspeaker

An AllRAD device for holes in the array. If no real speaker lies within 60 degrees of a pole
(`src/allrad.c:24`), the decoder adds a fictitious speaker there to close the triangulation. It then
**discards** that speaker's decode share (`src/allrad.c:77`). Without it, a floor-less array's hull
spans the nadir with triangles of bottom-ring speakers, and downward diffuse energy smears onto
them. The cube grid's
roughly 55 degree nadir gap stays under the threshold, so genuinely covered poles are untouched.

### max-rE

A per-degree taper applied to the SH signal before the bed decode matrix, `w_l = P_l(r)` with `r`
the largest zero of `P_{order+1}`, renormalized so diffuse energy matches the untapered decode
(`src/ambisonics.c:61-72`). Trades a little directional sharpness for a smoother, more robust
image away from the center. **Band-split max-rE** (`bwa_set_max_re_split`) tapers only above the
700 Hz crossover, leaving the [rV](#rv)-optimal plain decode below (`src/rt.c:2244`), the Gerzon
split. Score a layout's [bed metric](#bed-metric) with the same setting the install ships.

### NFC-HOA

Near-field-compensated higher-order ambisonics: per-order distance-coding filters (Daniel 2003)
that correct an ambisonic decode for the array's finite radius. **The engine deliberately does
not implement it**; the error was quantified rather than assumed, and the room's Schroeder
frequency sits above the whole effect band anyway. Rationale, numbers, and the revisit condition:
[spatialization.md](./spatialization.md#near-field-compensation-deliberately-omitted).

### Parametric bed rendering

`BWA_BED_PARAMETRIC`, the [DirAC](#dirac) alternative to a matrix decode. Per band, the direct
stream re-pans the W signal through the engine's own **listener-relative panner** at a virtual source
on the array shell, and the diffuse stream decodes through the matrix into the
[decorrelators](#decorrelation). The payoff is that a recorded soundfield becomes **walkable**: it
gets correct directions and parallax off-center, which a center-locked matrix decode structurally
cannot give. The engine loudness-matches both streams to the matrix decode, so the toggle is a clean
A/B.
See [spatialization.md](./spatialization.md#parametric-bed-rendering-bwa_set_bed_renderer-live-ab).

### Sampling decode

Also called the projection decode or SAD: `decode[s][k] = (2l+1) * Y_k(dir_s) / N`
(`src/ambisonics.c:54`). Exact on a perfectly uniform array, and it over-energizes dense regions on
an irregular one because every speaker radiates a fixed diffuse energy regardless of where it sits.
It is **not** a selectable decoder here. It survives only as the automatic fallback when a degenerate
layout defeats AllRAD or EPAD, and as the energy reference both of those normalize to
(`src/allrad.c:93`, `src/epad.c:109`).

### SN3D and N3D

Two SH normalization conventions. The engine encodes **SN3D** (the AmbiX convention,
`src/ambisonics.c:9`). N3D is the fully orthonormal one, related per degree by `sqrt(2l+1)`
(`src/epad.c:62`). EPAD builds in N3D because its energy property holds in the orthonormal
basis. phonon wants orthonormal real SH, so the monitor encode rescales per degree by
`sqrt(2l+1)/sqrt(4*pi)` (`src/ambisonics.c:153-160`). If you are chasing a level or direction bug in
the binaural path, the normalization is where to look first. See
[spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim).

## Source width: spread, extent, decorrelation

Giving a source angular size without moving its perceived direction or its loudness. Full treatment:
[api.md](./api.md#source-spread--size-control-thread-per-frame).

### Decorrelation

`bwa_set_decorrelation`, off by default: a spread source's energy splits into a coherent share on
the normal path and an incoherent share through **per-speaker sparse velvet-noise filters**
(30 taps over 30 ms, `src/rt.c:77-78`; Valimaki and Schlecht). Split amplitude `sqrt(spread)`,
ramped, power-conserving; time-domain, no onset latency. The same bank renders the parametric
bed's diffuse stream. It and [spectral widening](#spectral-widening) are two answers to
[phantom collapse](#phantom-collapse); A/B them and keep the winner.

### Extent

`bwa_source_set_extent`, the anisotropic form of [spread](#spread): separate width and height, each
0..1, so a shoreline can be wide but not tall. Equal values are exactly the isotropic spread on the
same code path. Width and height are **room-referenced**, so anisotropic sources use the up-anchored
frame instead of the [transported frame](#transported-frame) and inherit its pole ambiguity: "width"
straight overhead is undefined, as it is in BS.2127.

### Hole-aware spread floor

`bwa_set_hole_spread`, an engine-wide policy for arrays with **holes**: it floors a source's
[spread](#spread) by the angular gap from the source bearing to the nearest speaker bearing,
`floor = strength * clamp((gap - knee)/(pi/2 - knee), 0, 1)` (`src/hole.c:53`), where `knee` is
the array's own mean speaker spacing. It exists because the hull closes a hole with a big
triangle of distant speakers, a split image rather than a phantom. A source with no speaker near
it is not a point, so it renders honestly wide instead. An array that surrounds the listener
derives no floor at any bearing. See [spatialization.md](./spatialization.md).

### Lobe mode

The default spread render (`BWA_SPREAD_LOBE`): blend the panner's point gains toward a
width-controlled lobe `(0.5*(1+cos))^q` centered on the source direction, with
`q = 1.5 + (1-spread)*6` (`src/rt.c:1168`), then renormalize to the panner's own power. One solve,
smooth and cheap. The catch is that the extent is a **reshaping of gains solved for a point**, so it
does not inherit the selected panner's character the way [MDAP](#mdap) does.

### MDAP

Multiple-direction amplitude panning (Pulkki 1999), spread mode 1. The engine pans a ring of virtual
sources around the source direction (8 at the full cone angle `spread * 90` degrees plus 4 at half)
with the **selected panner**, sums them, then renormalizes to the point solve's power. The extent
comes from real panner solves, so VBAP stays sparse per direction and SPCAP stays
placement-corrected. About 13x the gain-solve cost, block-rate and dirty-gated. At spread going to 0
the ring collapses onto the point solve, so the modes meet continuously.

### Near-listener widening

`bwa_set_near_spread`, an engine-wide policy. With a radius `R` set, every source's spread is floored
at `1 - dist/R`: untouched beyond `R`, fully wide at the head. It exists because a point panner
collapses an approaching source into the nearest speaker and then snaps it across the head as it
passes, which is the common CAVE case. A source with a metric [size](#size) subsumes this policy.

### Phantom collapse

What happens when several speakers radiate **coherent copies** of one signal: the copies fuse into a
phantom image between the speakers and comb-filter position-dependently as the listener walks. It is
the reason [decorrelation](#decorrelation) and [spectral widening](#spectral-widening) exist, and the
mechanism [comb depth](#comb-depth) measures. See
[api.md](./api.md#source-spread--size-control-thread-per-frame).

### Size

`bwa_source_set_size`, the **metric** parametrization of spread: a radius in meters. The spread is
floored at the angle the radius subtends from the tracked listener, `asin(r/d)/(pi/2)`
(`src/rt.c:1412`), capped at 1 once the listener is inside the source. Prefer it when the content has
a physical size, because a 2 m waterfall then *stays* 2 m wide as you walk, where an angular spread
would change physical size with distance. The larger of spread and the size-derived floor wins.

### Spectral widening

`BWA_SPREAD_SPECTRAL`, spread mode 2 (Zotter and Frank's phantom-source widening). The source splits
into 6 complementary one-pole bands with crossovers at 250, 700, 1800, 4500 and 10000 Hz
(`src/rt.c:71-73`). The engine pans each band to its **own direction** inside the spread cone, every
one a real panner solve. The ear integrates the scattered spectrum into width. Because different
frequencies come from different speakers, there are **no coherent copies to collapse or comb**:
extent without decorrelation noise. Costs about 6 band filters plus 6 gain sets per wide voice, and
point sources pay nothing.

### Spread

`bwa_source_set_spread`, 0 for a point and 1 for wide. It runs in the per-block gain solve, not the
sample loop. The engine renormalizes it to **the panner's own power**, so widening redistributes
energy without re-leveling and keeps the centroid on the source direction. It is panner-agnostic,
and the new gains ramp like any gain change. Three render modes sit behind it:
[lobe](#lobe-mode), [MDAP](#mdap) and [spectral](#spectral-widening).

### Transported frame

The orthonormal frame around a source direction that MDAP's ring and the spectral mode's band
directions hang off, **parallel-transported** per voice instead of derived from a fixed
up-vector. The fixed-up branch flips the frame ~180 degrees in one solve when a moving source
leaves the pole zone, which teleports every ring or band direction (Pulkki's reference `vbap`
external carries the same state for the same reason). [Anisotropic extent](#extent) gives this up
deliberately.

## Acoustics: reflections, reverb and materials

Three implementations overlap here and they are complementary. Read
[materials.md](./materials.md#choosing-an-acoustics-path) first.

### Absorption

A material's per-band fraction of energy absorbed on reflection, 0..1 (1 is dead, 0 is a perfect
mirror), in Steam Audio's 3-band low/mid/high model. The [image-source](#image-source-method) path
converts it to a pressure reflection coefficient as `sqrt(1 - absorption)` (`src/ism.c:41`). See
[materials.md](./materials.md#the-acoustic-material-model).

### Anisotropic decay

The FDN's per-line decay time scaled by direction (`bwa_fdn_desc.decay_dir` and `decay_factor`), the
diagonal direction-domain case of the Directional FDN (Alary, Politis and Schlecht, JAES 2019). It
lets a tail die faster toward one side than another. Use it as a **design** parameter. Never set it
from the room's measured [RT60](#rt60).

### Baked reflections

Precomputing the listener-centric reverb at a grid of probes covering the listening zone, so the sim
thread looks reverb up instead of ray-tracing it. The ray trace runs once at `bwa_start` and can
afford more rays and bounces than real time. The one thing runtime geometry does **not** move is a
baked result: the bake froze the geometry, so use real-time reflections if the scene animates. See
[materials.md](./materials.md#baked-reflections-bwa_reflections_descbake).

### Bending loss

The frequency-dependent loss a [pathed](#pathing) sound picks up bending around an occluder
(phonon's `eqCoeffs`, beside `shCoeffs`' direction and level). The engine normalizes it to a pure
tilt (loudest band 1, floored at 0.0625, `src/steam_path.c:167`), so it adds color without
disturbing the level. It applies to the un-occluded signal *before* the SH encode. See
[materials.md](./materials.md#sound-pathing-bwa_descenable_pathing).

### Directivity

A source radiating unevenly, modeled as a weighted dipole: weight 0 is omni, 0.5 is cardioid, 1 is
figure-8, and `power >= 1` sharpens the lobe. It needs no geometry and is independent of occlusion,
so a source can be directional without being occluded. It costs nothing at pan time. A source
facing away from you is a strong cue and this is the cheap way to get it. See
[materials.md](./materials.md#source-directivity-cheap-per-source).

### Early reflections

The first specular bounces, which carry room size and source distance. Two implementations render
them and **you must not run both**: the Steam reflection bed already contains them, so the pair
double-counts. The recommendation is the [image-source method](#image-source-method), because it is
the only one with parallax. See [materials.md](./materials.md#choosing-an-acoustics-path).

### FDN

A 16-line feedback delay network (`src/fdn.c`), the phonon-free **late tail**. Householder feedback
(orthogonal, so the decay filters are the only loss), line delays spread 23 to 90 ms and kept
co-prime-ish so modes do not stack (`src/fdn.c:29-32`), two-band decay filters per line derived as
`g = 10^(-3 * len / (rt60_eff * fs))` (`src/fdn.c:67`). Each line is assigned a Fibonacci-sphere
direction and rendered as a plane wave through the bed decode, which is what makes
[anisotropic decay](#anisotropic-decay) possible. Deterministic CPU, infinite tail, designable
decay. Defaults: 1.2 s low, 0.7 s high, 2000 Hz crossover (`src/fdn.c:127`).

### Hybrid reverb

Steam Audio's split of the reflection bed into a short ray-traced early-reflection IR convolved
per block plus a parametric FDN late tail: the feature's decisive cost control. The bed runs
hybrid unconditionally. See
[materials.md](./materials.md#reflection-bed-hybrid-reverb-directional-early-reflections--parametric-tail).

### Image-source method

`src/ism.c`, first-order specular reflections for a shoebox: the engine renders each of the six
mirror images as **a point source through its own listener-relative panner**. A reflection therefore
gets the right direction, distance attenuation, and **parallax**, which a listener-centric bed
structurally cannot give. Cost is O(N) in sources against the bed's O(1), so opt in on the few
that matter. Do not model the CAVE room itself with it. See
[spatialization.md](./spatialization.md#early-reflections-image-sources-panned-like-point-sources-ismc).

### Listener-centric versus listener-relative

The distinction that decides which reflection path to use. **Listener-centric** means one ambisonic
field decoded around one point (Steam's reflection bed, the FDN tail): O(1) in sources, and
structurally incapable of parallax, so walking toward a wall does not change its reflection's
direction. **Listener-relative** means re-solving per listener position every block (the panner, the
ISM): parallax by construction, at O(N). See [materials.md](./materials.md#choosing-an-acoustics-path).

### Occlusion

How much of the direct path a surface blocks, a scalar 0..1 from a ray query, applied to the
**mono voice upstream of panning** with a 3-band [transmission](#transmission) EQ (so a wall
*muffles*, not merely attenuates). Volumetric, so partial cover attenuates smoothly. It does not
enter the gain solve and does not dirty the voice. The fold is
`raw[b] = occlusion + (1 - occlusion) * transmission[b]` (`src/steam_scene.c:394`). See
[materials.md](./materials.md#direct-sound-per-source-occlusion-not-distance).

### Pathing

Routing sound around occluders and through portals over a baked probe-to-probe visibility graph
(`src/steam_path.c`). A pathing voice SH-encodes its **un-occluded** signal into a shared ambisonic
accumulator, because the indirect path goes around the occluder and the direct-path occlusion must
not apply to it. It only does anything where the scene has real occluders. See
[materials.md](./materials.md#sound-pathing-bwa_descenable_pathing).

### Reflection bed

Steam Audio's listener-centric reflections plus reverb, rendered as a low-order ambisonic field and
decoded onto the same speaker bus as everything else. One shared bed source means **one convolution
regardless of source count**, which is the cost-control choice that makes it viable. IR length,
ambisonic order and the ray budget are baked at `bwa_start`; the wet gain is the one live control.
See [materials.md](./materials.md#reflections--reverb-a-diffuse-ambisonic-bed).

### RT60

The time for a decaying field to drop 60 dB. `bwa_calibrate --room` measures it by Schroeder backward
integration, reading T20 from the -5 dB to -25 dB span and tripling it (`src/measure.c:123-136`).

**Do not copy a measured RT60 into the engine's reverb settings.** The room's own decay is a
**floor**: you cannot render a space deader than the room you are standing in, and setting the
synthetic tail to the measured value renders the room twice. The FDN's decay is a design parameter,
and the same trap applies to modeling the CAVE with the ISM. See
[calibration.md](./calibration.md#modes).

### Scattering

A material's 0..1 fraction of energy reflected diffusely rather than specularly, that is, surface
roughness. It feeds the ray-traced reflection bed only; the ISM is purely specular. See
[materials.md](./materials.md#the-acoustic-material-model).

### Schroeder frequency

The frequency above which a room's modes overlap densely enough to behave statistically rather than
modally, roughly 200-300 Hz for a 3x3 m space. It is the dividing line for two decisions. Below it,
modes are approximately minimum-phase, which is why [room EQ](#room-eq) cuts work there and only
there. It also sits *above* the entire [NFC-HOA](#nfc-hoa) error band, which is part of why skipping
near-field compensation is defensible. See [calibration.md](./calibration.md#modes).

### Transmission

A material's per-band 0..1 fraction passing **through** a surface, the **spectral tilt** of occluded
sound. The material supplies transmission; the ray query supplies [occlusion](#occlusion). Keeping
them separate is what lets a concrete wall and a curtain occlude by the same amount and sound
completely different. See [materials.md](./materials.md#the-acoustic-material-model).

## Binaural and headphone renders

Two headphone profiles share one decode and answer different questions. Full treatment:
[spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim).

### Array-sim monitor

`BWA_PROFILE_CAVE_SIM` (and `CAVE_BOTH`'s tap): a **bus to stereo** transform that treats each
speaker-bus channel as a virtual speaker at its surveyed room direction and HRTFs the lot to stereo.
It auditions the **actual array render**, panner spread and alignment and gain staging included,
which is exactly what it is for. Head orientation rotates the virtual array. The ambisonic order
fixes its cost, and the source count does not change it.

### Direct binaural field

`BWA_PROFILE_BINAURAL`'s 16-channel SH accumulator. Point sources bypass the speaker panner and
SH-encode at their **true** listener-relative direction, so none of the array's phantom spread
reaches the ears. Beds pass SH to SH into it, and [pathing](#pathing) sums in raw. With the SDK
each point voice additionally gets its own `IPLBinauralEffect`, and spread power-splits between
point tap and field. Without the SDK the fallback is two opposed cardioids
(`src/binaural.c:63-75`): fine for routing and gross laterality, useless for timbre. See
[spatialization.md](./spatialization.md#headphone-renders-direct-binaural-and-the-array-sim).

### Headphone correction EQ

`bwa_load_headphone_eq`: an AutoEq `ParametricEQ.txt` parsed into an RBJ biquad cascade applied to
the **final stereo** of every headphone profile (`src/hpeq.c`, preamp at `src/hpeq.c:33`, section
build at `src/hpeq.c:61`). It is the headphone-side counterpart of the array's per-speaker align
stage. Personalized [SOFA](#sofa) HRTFs correct the *ears*, this corrects the *transducer*, and they
compose. A `Filter` line that does not parse fails loudly rather than shipping a silently partial
correction. See [api.md](./api.md#headphone-correction-eq-control-thread).

### HRTF

Head-related transfer function: the per-ear filtering a head, torso and pinnae apply to a sound from
a given direction, and the thing that makes headphone audio externalize. The production decode uses
Steam Audio's; without the SDK there is no HRTF at all, only a lateral pan. **The production array
render never uses HRTF**, so the SDK is a developer-workstation dependency, not a rig one.

### ILD

Interaural level difference, the loudness difference between the ears that carries lateralization
above roughly 1.5 kHz. Named here because of the trap in the next entry.

### Laterality check

A test asserting that a right-hand source lands in the right ear. **Drive it with a tone, never DC.**
A DC-driven `steam_decode` assertion had inverted polarity, because the default HRTF's per-ear DC
gains oppose its audible [ILD](#ild), so the assertion passed exactly when the field was mirrored. It
mis-diagnosed a correct encode and shipped a left/right mirror that only a by-ear report caught. The
current test drives a 660 Hz tone. `test_ambi` only checks `m >= 0` and will not catch a mirror.

### SOFA

Spatially Oriented Format for Acoustics, the standard container for measured HRTF sets. Pass one as
`hrtf_path` to render with a personalized HRTF instead of Steam Audio's default. See
[api.md](./api.md#feature-overview).

### Virtual-speaker encode

The efficiency trick behind both headphone profiles: treat each bus channel as a virtual speaker
at its surveyed direction relative to the head, encode into ambisonics with a fixed matrix, sum
the [direct binaural field](#direct-binaural-field) in, and run **one** ambisonics-to-binaural
decode instead of 26 HRTF convolutions. One function (`ambi_encode_phonon`,
`src/ambisonics.c:166`) does the encode for both consumers, so they cannot drift in convention.

## Per-source propagation effects

Physically motivated per-voice DSP, all opt-in and all phonon-free. See
[spatialization.md](./spatialization.md#propagation-effects-opt-in-per-source) and
[api.md](./api.md#propagation-effects-control-thread-per-frame).

### Air absorption

A distance-driven one-pole low-pass on the direct path: cutoff about 18 kHz at zero distance, falling
650 Hz per meter, floored at 1200 Hz (`src/rt.c:51-53`). Subtle in-room, pronounced for far virtual
sources. Steam Audio's own `airAbsorption` flag stays **off** so the engine does not apply the
effect twice.

### Doppler

Rendering a voice through its acoustic propagation delay, `distance / c`, on a per-voice fractional
delay ring. The engine does not compute the pitch shift: it falls out of the changing delay, because
the read tap's glide rate *is* the resampling ratio `1 - v_radial/c`. So you never supply a
velocity. The
delay saturates past 8 m to bound the ring (`src/rt.c:49`). This is a **per-source** delay and
composes with the per-speaker align delay, which does a different job.

### Loudness compensation

`bwa_source_set_loudness_comp`, the perceptual counterpart of distance attenuation. At lower levels
the ear loses LF sensitivity (the ISO 226 contours), so an attenuated source reads *thin* as well as
far. A one-pole LF shelf boosts by **0.4 dB per dB** of attenuation the panner applied, capped at
+8 dB (`src/rt.c:1766-1768`). A stylization, not physics: strict realism leaves it off.

### Near-field proximity boost

The near-distance mirror of loudness compensation: an LF shelf rising linearly from 0 dB at 1 m to
+6 dB at distance 0, corner 300 Hz (`src/rt.c:57-60`). It renders the spherical-wavefront proximity
effect, so "at arm's length" reads as bass rather than only as level. Load-bearing in a walkable
volume where sources really do reach the head.

### Reverb send

The per-voice wet feed into the reflection tap, ramped from 0.25 at 1 m to 1.0 at 6 m
(`src/rt.c:61-64`), so distant sources sit wetter. The engine taps the send **before** Doppler and
air absorption, which is why those two do not color the reverb feed.

## Calibration and the acoustic survey

How the array is measured, trimmed and characterized at install. Full treatment:
[calibration.md](./calibration.md).

### Air temperature

The dominant systematic in the survey: `c` moves about 0.6 m/s per degree C and every surveyed
range is `c * delay`. Tell the tools with `--temp` and they record it into the layout. **This
does not change what you hear**; set it so the survey is honest as a measurement. See
[calibration.md](./calibration.md#air-temperature).

### Azimuth reference

The unknown constant yaw between a ZM-1's capsule table and the room, because nothing published says
which capsule faces the device's front. It rotates every DOA by a constant and survives every
structural self-check the geometry table has. Pin it by clapping from a known direction, or skip the
question entirely by running a [capsule survey](#capsule-survey), which hands you the orientation as
a side effect. See [calibration.md](./calibration.md#the-capsule-geometry).

### Capsule survey

`zylia_survey`: recover the ZM-1's capsule positions **in room axes, indexed by the ASIO channel
that fed them**, from claps at known positions. The result therefore *is* the
[channel order](#channel-order) and *is* the mounted orientation, so you have nothing left to pin.
Read
[survey spread](#survey-spread) and [residual](#residual) before believing a result. See
[calibration.md](./calibration.md#the-capsule-self-survey-zylia_survey).

### Channel order

Which ASIO input carries which physical capsule. Node `i` in the published table is not necessarily
input `i`, and a permutation still yields a **confident** direction, just the wrong one. Resolve it
with `bwa_zylia_probe` or, better, measure it with a [capsule survey](#capsule-survey). See
[calibration.md](./calibration.md#the-capsule-geometry).

### Correction EQ

`bwa_calibrate --eq`: a per-speaker minimum-phase inverse FIR (up to 512 taps) designed from the
IR gated to the **direct sound**, so it flattens the **speaker's own** response and not the room
(room response is position-dependent across the listening area, so a single-point room EQ
over-fits one spot and worsens the others). Applied in the align stage before gain and delay.
See [calibration.md](./calibration.md#modes).

### Deconvolution

Recovering a speaker's impulse response from a recorded [exponential sweep](#exponential-sweep) by
regularized inverse filtering (`measure_response` in `src/measure.c`). Two numbers come out of the
result: the direct-path **delay** (system latency plus time of flight) and the broadband **level**.
One sample at 48 kHz is 7 mm, and sub-sample peak interpolation gets well below that. The accuracy
limit is your mic position and your assumed [speed of sound](#speed-of-sound), not the acoustics.

### Delay trim

Per-speaker `delay_ms` aligning every speaker's arrival to the farthest one. Applied in the align
stage as a whole-sample delay line (`align_process`). It is a **per-speaker** delay that equalizes
arrival across the array, distinct from [Doppler](#doppler)'s per-source propagation delay; they
compose. Note that it is computed for one reference ear height. That is part of why height
displacement is a calibration problem rather than a tracking one. See
[spatialization.md](./spatialization.md#per-speaker-alignment).

### Drift check

`bwa_calibrate --check`: one fast pass from a mic position comparing each speaker's measured distance
to its stored position, with the common latency removed as the **median** residual so a few moved
speakers cannot define the baseline. Flags anything beyond about 20 mm and exits 3 if it does, so it
is scriptable. It is **radial only**, so a purely tangential move does not change the distance; re-run
`--localize` for a full re-survey. See [calibration.md](./calibration.md#modes).

### Exponential sweep

A Farina exponential sine sweep, the excitation every calibration measurement uses. Its virtue here is
that harmonic distortion products separate out in the deconvolved response instead of contaminating
it. Contrast the validation harness, which uses a **steady-state** [stimulus](#stimulus) specifically
so that round-trip latency never enters. See
[calibration.md](./calibration.md#how-it-measures-exponential-sweep--deconvolution).

### Gain trim

Per-speaker `gain_db` equalizing measured sensitivity, with the speaker-to-mic distance divided out
so it corrects the **speaker** and not the distance, and cut-only so nothing clips. Applied in the
align stage. See [calibration.md](./calibration.md#modes).

### GDOP

Geometric dilution of precision: how much the *arrangement* of your measurement points amplifies
error in the solve. Clustered or coplanar mic positions amplify it badly, so spread the `--localize`
positions out and make them non-coplanar. The same failure in the capsule survey has its own guard,
[survey spread](#survey-spread). See [calibration.md](./calibration.md#modes).

### Residual

What a recovered geometry **fails** to explain, in microseconds, and the number that says whether to
believe a [capsule survey](#capsule-survey). Sub-microsecond is clean. Tens of microseconds means bad
claps, a wrong array center, or a clap that was not where you said it was. It is the survey's
counterpart to [diffuseness](#diffuseness) on the DOA estimator: a trust number, not a result. See
[calibration.md](./calibration.md#the-capsule-self-survey-zylia_survey).

### Room EQ

`bwa_calibrate --room-eq`, room correction **at the mic position, for a static listener only**:
above 200 Hz a frequency-dependent-window FIR (boosts capped +3 dB), below it modal **cuts** as
peaking biquads. Cut-only by design and by schema: peaks are modal energy you can remove, dips
are position-dependent cancellations you cannot fill. `bwa_start` **refuses** a `room_eq` layout
in a moving-listener session. What no EQ fixes is **decay**. See
[calibration.md](./calibration.md#modes).

### Speed of sound

Two different quantities wear this name and confusing them is a real error: **measurement `c`**
(`src/sos.h`), a property of the room being surveyed (`331.3 + 0.606 * t_c`, guarded to
306-380 m/s, riding the layout as `reference.speed_of_sound_mps`), and **engine `c`**
(`BWA_SPEED_OF_SOUND`, `src/rt.c:46`), a live medium and creative control for
[Doppler](#doppler). `src/sos.h:12` states the separation explicitly. See
[calibration.md](./calibration.md#air-temperature).

### Survey spread

The [capsule survey](#capsule-survey)'s isotropy guard, 1 for well-distributed clap directions and 0
for coplanar ones. Claps in a horizontal ring around the array leave the normal matrix singular in the
vertical and the capsule **heights** unrecoverable. Below 0.05 the solver refuses, rather than
handing back a flattened array and a confident wrong answer. **Clap high and low, not just around.**
Unrelated to source [spread](#spread), which is a width. See
[calibration.md](./calibration.md#the-capsule-self-survey-zylia_survey).

### Tracked listener alignment

`bwa_set_tracked_align`, off by default: the per-speaker delay and gain trims re-referenced from
the layout's reference point (`Layout.ref`, the array centroid by default) onto the **tracked** listener, so time coherence follows the head
(`src/rt.c:2692`, `listener_align_track`). Opt-in because every delay change is a resampling event: a
walking listener Doppler-shifts the entire array at once. A dead zone and a slew rate limit bound
that; a faster listener gets a lagging alignment rather than a warbling one. The
tracked-position sibling of [tracked room EQ](#tracked-room-eq). See
[spatialization.md](./spatialization.md#re-aligning-to-the-tracked-listener-bwa_set_tracked_align-off-by-default).

### Tracked room EQ

`--room-eq-grid`, the moving-listener answer to [room EQ](#room-eq)'s modal half: measure a grid
of up to 16 positions, interpolate the cut depths at the live listener position every block, and
glide the biquads at 24 dB/s. It works because mode *frequencies* are fixed properties of the
room; only their strength varies with position, smoothly at LF wavelengths. **Only** the
30-200 Hz band is tracked (mid/HF room response decorrelates over centimeters). See
[calibration.md](./calibration.md#modes) and [api.md](./api.md#tracked-room-eq-control-thread-live).

### Trilateration

`calib_trilaterate`: solving each speaker's 3D position **and** the unknown constant system
latency jointly by least squares, from ranges captured at 5 or more known mic positions. It sees
speakers optical trackers cannot (the sweep passes through the screens). The solved latency's
residual against the driver's reported figures must be a small **positive** number; negative is
physically impossible. See [calibration.md](./calibration.md#modes).

## Validation vocabulary

`bwa_validate` renders a phantom and measures where the array actually put it. Several of these terms
are this engine's own coinages. Full treatment: [validation.md](./validation.md).

### Active-intensity DOA

`zylia_intensity_doa`, the primary direction estimator: project the 19 capsule pressures onto
first-order real SH, divide out the rigid-sphere [mode strengths](#mode-strength), accumulate the
active intensity `I = sum Re{conj(W) * (X,Y,Z)}` over the band; `I` points at the source. It
reads direction out of **continuous** content, the whole reason it exists: a phantom has no
arrival time of its own. See
[validation.md](./validation.md#the-estimator-active-intensity-zylia_intensity_doa).

### Angular miss

The great-circle angle between where a source was supposed to come from and where the estimator says
it came from, `ValidCell.miss_deg` (`src/valid.h:68`). It is the number nobody has, and the number
every proxy in this repo is a proxy for. Read it as an **excess** over the [physical floor](#physical-floor),
never as an absolute.

### Capsule integrity

`zylia_check_capsules`, run **before** believing any direction. It flags `DEAD`, `HOT`, `CLIPPED` and
`INCOHERENT` capsules (`src/zylia.h:120-123`) against the array's own **robust median**, so a fault
cannot define the baseline it is judged by. The flags array drops straight in as the `exclude`
argument to either estimator. **Report every exclusion**: a direction from 17 capsules is fine, a
direction from 17 capsules you believed came from 19 is not. See
[validation.md](./validation.md#signal-integrity-zylia_check_capsules).

### Cell

One measured or simulated data point: a panner, a solve mode, a listening position, a target
direction, and what came back (`ValidCell`, `src/valid.h:56-76`). A cell carries **both** an
[angular miss](#angular-miss) and a [comb depth](#comb-depth), and the two estimators refuse
independently, so `ok` and `comb_ok` are separate flags.

### Comb depth

The spectral ripple a phantom's coherent copies impose, in dB: the **interdecile range** of the
detrended sub-band spectrum (`src/zylia.c:818`), averaged over the included capsules. 0 dB is a
flat response, what a single coherent arrival gives. Three traps: the axis is **frequency, not
the capsules** (all 19 capsules see nearly the same comb; they average its noise down, they do
not sample it independently); it **cannot come from [rE](#re)**, which knows nothing about
arrival times or frequency; and **two equal copies null completely**, so tightening SPCAP
[focus](#focus) does *not* monotonically reduce it on a general direction. See
[validation.md](./validation.md#comb-depth-zylia_comb_depth).

### Comb quality

The believability number beside a [comb depth](#comb-depth): the **standard error** of the capsule
mean, not the raw spread, because the spread is what the averaging already handled
(`src/zylia.c:846`, with 1.0 dB of standard error taking it to 0). It reads the opposite way to
[diffuseness](#diffuseness): **1 is good**. Below about 0.5 the capsules are not seeing one comb, so
suspect a capsule fault or a capture that drifted.

### Direct channel route

`bwa_source_set_channel`: send one source's content to exactly ONE output channel, with every
spatial and propagation stage bypassed (`src/rt.c`, `direct_channel_gains`). It is the live,
in-engine form of the [physical reference arm](#physical-reference-arm) that `bwa_validate` builds
offline, so the A/B a listener hears matches the one the measurement scores. It is installed as a
one-hot gain vector at the panner's output, NOT as a post-align injection like
`bwa_set_test_signal`, which is what keeps the reference on the same output stage, the same gain
ramps and the same level scale as the phantom it is compared against. See
[api.md](./api.md#direct-output-channel-routing-the-reference-condition).

### First-order ceiling

`ZYLIA_FOA_FMAX`, 1200 Hz (`src/zylia.h:89`): a 49 mm sphere hits `kr` about 1 there, above which
the first-order [mode strength](#mode-strength) inversion stops being trustworthy. A band lying
entirely above it makes the call **refuse**, deliberately: a confident wrong direction is worse
than no direction. [SRP-PHAT](#srp-phat) reaches 3300 Hz and is the only path that sees above the
ceiling; [comb depth](#comb-depth) has no ceiling, since nothing there inverts `b_n(ka)`.

### Hot capsule

The dangerous integrity fault. A dead capsule is obvious; one that goes **hot** (self-noise, a
failing preamp) keeps total array power looking healthy while corrupting every SH channel. **Two
estimators agreeing does not clear it**, since both are poisoned identically. You have to catch it
on the raw signals. See
[validation.md](./validation.md#signal-integrity-zylia_check_capsules) for the measured effect.

### Matched-cell contrast

A median of **paired** differences over the same cells measured two ways, with a
[bootstrap interval](#bootstrap-interval) (`valid_contrast`, `src/valid.h:260-266`). A median of
differences and a difference of medians are not the same number; only the first is a statement
about the same cells. Two traps: drop cells where either side failed first, and **never pool the
physical-versus-phantom contrast across placements** (its distribution is bimodal, so a pooled
median lands in the empty middle and reads as "no effect"; it happened during development). See
[validation.md](./validation.md#reading-the-output).

### Mode strength

`b_n(ka)`, the rigid-sphere scattering factor divided out before reading direction from a ZM-1's
spherical-harmonic projection; skipping the inversion biases the direction. The invisible sign
trap: get the FFT convention backwards and `|b_n|` is identical, so everything looks fine and the
DOA comes back exactly 180 degrees out. Synthesizing test input from the model being inverted
**cannot** catch it. See
[validation.md](./validation.md#a-sign-trap-recorded-so-nobody-repeats-it).

### Phantom

A source that exists nowhere physically: several speakers radiating coherent copies of one signal,
which fuse into an image between them. Everything in this section is about the fact that a phantom
lands somewhere other than where you aimed it, and costs something in timbre on the way. Contrast a
**physical source**, which is one speaker driven alone; see
[physical reference arm](#physical-reference-arm).

### Physical floor

What the whole measurement chain costs **before any panning happens**: drive one speaker alone
and estimate its direction (about 0.1 degrees in anechoic simulate; larger on hardware, and the
increase *is* the room plus your survey error). **If it is not small, stop**: nothing measured
afterwards is interpretable. There is a comb-depth floor too. See
[validation.md](./validation.md#the-physical-reference-arm).

### Physical reference arm

**The array's own speakers are physical sources at known positions**, so driving speaker `i`
alone gives a real-source measurement through the same chain in the same room, with nothing moved
(`src/valid.h:189-216`). It buys a [physical floor](#physical-floor), a
[matched-cell contrast](#matched-cell-contrast), and the negative control for content dependence.
It is about 0 at the array center by symmetry (the center row is a null control), and VBAP shows
about 0 even off-center because it collapses onto the coincident speaker: a genuine
characterization, not an artifact. See
[validation.md](./validation.md#the-physical-reference-arm).

### Precisely wrong

The property of a tone measurement: **sub-degree repeatable and tens of degrees biased**
(`src/valid.h:117-118`). Repeats will agree beautifully with each other and with nothing else. Never
read repeatability as accuracy. It is the single most quotable reason to state the
[stimulus](#stimulus) beside any number.

### Rendering term

The part of a measured phantom error that comes from the render, as opposed to the **room term**;
[simulate](#simulate) isolates it. It is the term placement and panner choice control. Do not
read a simulated miss as a predicted in-room miss; read it as the floor the room then adds to.
See [validation.md](./validation.md#what-simulate-does-and-does-not-include).

### Simulate

`bwa_validate --simulate`: an analytic **anechoic** field substituted for a real capture. It has
the real phantom physics (real engine feeds, trims, delays, inter-speaker interference) and **no
room**, and it runs the same session loop the rig will, so it is not a shortcut around the
hardware path. Modeling the room here would repeat the [RT60](#rt60) trap. See
[validation.md](./validation.md#what-simulate-does-and-does-not-include).

### SRP-PHAT

`zylia_srp_doa`, the independent cross-check: steer a beam over a direction grid and take the most
powerful one, PHAT-whitened so a loud bin cannot outvote the rest. It shares as little as possible
with the intensity estimator, so **agreement means the answer is probably real and divergence means
something is wrong with the capture**. It goes higher (order 3, `kr` about 3) but is coarser (a
finite grid, roughly 2 degrees). Excluding capsules steps its order down automatically, rather than
returning a confidently over-fitted answer. Use it to check a number, not to be one. See
[validation.md](./validation.md#the-cross-check-zylia_srp_doa).

### Stimulus

What the harness plays. Broadband (a 24-tone sum across 420-1150 Hz) is the default and the
**optimistic** end of the range; `--tone <hz>` is the pathological end (`src/valid.h:127-130`).
The analysis band follows the stimulus, and a tone whose band lies entirely above the
[first-order ceiling](#first-order-ceiling) is refused with its frequency named. Content
dependence has **two** mechanisms (the room's standing waves, and the phantom's own free-field
interference), so **a number quoted without its stimulus is incomplete**. See
[validation.md](./validation.md#stimulus-and-where-content-dependence-actually-comes-from).
