# Phantom localization validation (`bwa_validate`)

Calibration *fixes* the array: trims, delays, positions, EQ. This *grades* it. Render a source in a
known direction, measure where the array actually put it, report the angular miss.

That number is the one nobody has. You can listen to a layout all day and not produce it, and the
usual substitutes ([rE error](./glossary.md#re-error), coverage maps, "sounds right to me") are
proxies for it, not it. [glossary.md](./glossary.md) is the one-line lookup for every term below.

Build it with `-DBWA_BUILD_CALIBRATE=ON` (same switch as `bwa_calibrate`). Run
`bwa_validate --simulate` to exercise the whole flow with no hardware.

## What it answers

- How far off is a phantom from its target, per direction, per panner, per listening position.
- **Does tracking actually buy anything.** A tracked renderer and a fixed one are identical at the
  sweet spot, which is where measurements are usually taken, which is why this question normally
  goes unanswered.
- Where in the room the layout works, and where it stops working.
- **How much of the error is the renderer at all.** Driving one speaker alone gives a physical source
  through the same chain, so the phantom miss can be read against a floor rather than in a vacuum.
- **How much the content matters here**, by measuring the same cells broadband and on a tone.
- **What the render costs in timbre, not just in direction.** A phantom is many speakers radiating
  coherent copies of one signal, so it combs. Every cell carries a comb depth beside its angular miss,
  and `--focus` sweeps the SPCAP knob that trades one against the other.

## The seam: solve position versus microphone position

A panner solves its gains for *some* listener position. A listener then hears the result from
wherever they actually are. Those are the same point only for a tracked renderer.

- **tracked**: the panner solves at the listener's real position. What DBAP does every block.
- **fixed**: the panner solves once at the sweet spot and never corrects. What an SPCAP or VBAP
  install does at load.

`bwa_validate` keeps those two positions separate, and that is the whole reason it can see anything a
sweet-spot measurement cannot.

One consequence worth stating, because it is easy to get backwards: **sources sit at fixed world
positions**, not at a bearing from the listener. Only then is every listening position judged against
the same physical sources, and only then does a fixed solve mean anything: its gains were computed for
a source that must not move when the listener does.

## Two paths, one scorer

```
valid_speaker_feeds  →  [ play + record ]  →  valid_score  →  statistics
                     ↘  valid_simulate    ↗
```

`valid_score` is the seam. The hardware path plays real feeds into a real room and hands back a
capture; the simulated path substitutes an analytic field. Everything above the seam (scoring,
medians, bootstrap intervals, contrasts, the report) is shared, so the two paths cannot drift.

The `valid` ctest pins that they agree: it builds feeds, propagates them the long way (explicit
per-speaker sum, interpolated fractional delay, 1/r), scores that, and compares against the analytic
path. Worst disagreement 0.64°.

The same unification happens one level up, in the tool. `bwa_validate` runs **one** session loop with
two capture backends, so `--simulate` is not a shortcut around the hardware path: it executes the
same placement loop, the same once-per-placement capsule check, the same exclusion threading, and the
same reporting the rig will run. Only the handful of lines that actually talk to ASIO go untested,
which is the irreducible part.

### What simulate does and does not include

`valid_simulate` builds the field a ZM-1 records in an **anechoic** free field: every speaker's real
solved gain, its layout trim and alignment delay, 1/r spreading, and the exact propagation delay to
each of the 19 capsules, summed coherently. So it has the real phantom-source physics, inter-speaker
interference included, which is where phantom error comes from.

It has **no room**.

Measured phantom error in a real room is roughly a rendering term plus a room term. Simulate isolates
the rendering term, which is the one placement and panner choice control, and the one that varies as
the listener walks. Do not read a simulated miss as a predicted in-room miss. Read it as the floor
the room then adds to.

Modeling the room here would repeat the trap `calibration.md` warns about with RT60: the physical
room supplies its own acoustics, and baking a model of it into the measurement double-counts.

## The estimator: active intensity (`zylia_intensity_doa`)

The rest of `zylia.c` answers "where are my speakers" from a transient, by arrival times. That will
not work here. A phantom has no arrival time of its own: it is the summed output of many speakers, so
the measurement has to read direction out of continuous content.

```c
int zylia_intensity_doa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, double c,
                        double f_lo, double f_hi, const unsigned char* exclude,
                        float dir_out[3], float* diffuseness_out);
```

STFT each capsule, least-squares project the 19 pressures onto first-order real spherical harmonics
(ACN/SN3D, through the engine's own `ambi_encode_sn3d`), divide out the **rigid-sphere** mode
strengths `b_n(ka)`, then accumulate the active intensity `I = Σ Re{conj(W)·(X,Y,Z)}` over the band.
For a plane wave in SN3D the first-order components are the direction cosines, so `I` points at the
source.

The ZM-1 is a scattering sphere, not an open array. Skipping the `b_n(ka)` inversion biases the
direction.

**The band is the real limit and it is not negotiable.** A 49 mm sphere hits `kr ≈ 1` at about
1.1 kHz, and above that the first-order inversion stops being trustworthy. Broadband content gets
400–1200 Hz; a tone gets ±1/6 octave around itself. `f_hi` clamps to `ZYLIA_FOA_FMAX`, so a band
lying entirely above it collapses and the call **refuses**. That is deliberate. A confident wrong
direction is worse than no direction.

`diffuseness_out` is the trust number, the counterpart of `zylia_survey`'s `resid_us`: 0 is a clean
plane wave, 1 is fully diffuse. A measurement taken in the reverberant tail rather than the direct
sound shows up here as a high value.

If a capsule survey is installed, this estimator inherits its measured channel order and orientation
for free: it reads `zylia_geometry` like everything else.

### A sign trap, recorded so nobody repeats it

`fft.h`'s forward direction carries the twiddle `e^{+i2πnk/N}`, which is the `e^{-iωt}` convention,
which pairs with the spherical Hankel function of the **first** kind. Get that backwards and nothing
looks broken: `|b_n|` is identical, so levels, diffuseness, and conditioning are all unchanged, and
the DOA comes back exactly 180° out. Exactly 180, because the residual factor is
`cos(2(θ₀ − θ₁))` and at low `ka` consecutive degrees sit 90° apart.

This is the same class of bug as the `steam_decode` DC-polarity incident. It **cannot** be caught by
synthesizing test input from the model being inverted, because the error cancels on both sides. The
`zylia` test pins it with a pure geometric-delay forward model and a cross-check against the TDOA
estimator instead. Do not "simplify" that test into a spherical-harmonic round trip.

## Signal integrity (`zylia_check_capsules`)

Check the capsules before believing any direction.

The failure this exists for is not a quiet one. A dead capsule goes to zero and is obvious. A capsule
that goes **hot**, self-noise or a failing preamp, keeps the array's total power looking perfectly
healthy while corrupting the spherical-harmonic projection, because every SH channel is a weighted
sum over *all* capsules.

**Two estimators agreeing does not clear this.** A hot capsule poisons every SH-domain estimator
identically, so intensity and steered-power will agree with each other and both be wrong. This has to
be caught on the raw signals or not at all.

```c
int zylia_check_capsules(const float* const x[ZYLIA_MICS], uint32_t n,
                         unsigned char flags_out[ZYLIA_MICS]);
```

Returns the number of faulty capsules. Flags are `ZYLIA_CAP_DEAD`, `_HOT`, `_CLIPPED`,
`_INCOHERENT`. Everything is judged against the array's own **robust median**, so a fault cannot
define the baseline it is measured by. The coherence check correlates each capsule against the
per-sample median signal over a small lag search, which catches a capsule at the right level carrying
the wrong signal, and does not mistake a healthy off-axis capsule for a broken one.

The flags array drops straight in as the `exclude` argument to either estimator. That is the intended
flow: check, report what you dropped, then estimate on what is left. **Report every exclusion.** A
direction computed on 17 capsules is fine. A direction computed on 17 capsules that you believed came
from 19 is not.

Measured in the `zylia` test: a 45 dB fault (the magnitude of a real documented one) pulls the DOA
9.0° off. Excluding the flagged capsule brings it back to 0.35°.

## The cross-check (`zylia_srp_doa`)

A second estimator sharing as little as possible with the first. Intensity reads a phase relationship
between two SH degrees per bin; this steers a beam over a direction grid and takes the most powerful
one, PHAT-whitened so a loud bin cannot outvote the rest.

Different failure modes. When they agree the answer is probably real. When they diverge, something is
wrong with the **capture**, which is what you want to know before writing a number into a table.

It is not a free upgrade:

- It goes **higher**. 19 capsules support order 3, good to `kr ≈ 3`, so this is the only path here
  that sees above the first-order ceiling at all (`ZYLIA_SH3_FMAX`).
- It is **coarser**. The answer is the best direction on a finite grid, roughly 2°, against the
  intensity vector's continuous solve. Use it to check a number, not to be one.
- Excluding capsules **steps the order down** automatically (3 → 2 → 1) rather than returning a
  confidently over-fitted answer.

## Comb depth (`zylia_comb_depth`)

Everything above measures *where* a phantom went. This measures what making one cost.

A phantom is N loudspeakers radiating **coherent copies of one signal**. At any point in the room
those copies arrive at N different times, so the microphone sees the source through a comb filter:
peaks where they add, notches where they cancel. That is the timbral price of amplitude panning. No
direction estimator can see it, because a comb barely moves the intensity vector.

```c
int zylia_comb_depth(const float* const x[ZYLIA_MICS], uint32_t n, double fs,
                     double f_lo, double f_hi, const unsigned char* exclude,
                     float* depth_db_out, float* quality_out);
```

Welch power spectrum per capsule, sub-band energies across the analysis band, in dB, with a straight
line in (log f, dB) fitted out. What is left is the ripple. `depth_db` is its interdecile range, the
90th percentile minus the 10th, averaged over the included capsules. 0 dB is a flat response, which is
what a single coherent arrival gives.

### The axis is frequency, not the capsules

This is the design mistake worth naming, because it is the natural one to make.

The ZM-1's shell is 49 mm, so the widest capsule pair sits 98 mm apart. The 400–1200 Hz band spans
wavelengths from 0.86 m down to 0.29 m, so every pair lies within 0.11 to 0.34 of a wavelength. All 19
capsules see very nearly the **same** comb. They average its noise down. They do not sample it
independently.

So a statistic taken *across* capsules measures nothing. The statistic is roughness **along
frequency**, computed per capsule, and the capsules only average it.

`quality_out` is the one honest use of the capsule axis: it is the standard error of that average, not
the raw spread, because the spread is what the averaging already handled. It reads the opposite way to
`diffuseness`: 1 is good. Below about 0.5 the capsules are not seeing one comb, so suspect a capsule
fault, a band far above the shell's coherence, or a capture that drifted.

### Read it as an excess, never as an absolute

Three other things put ripple in a spectrum: the room, the stimulus's own line structure, and the
analysis itself. All three are present when **one speaker is driven alone**, which is exactly what the
physical reference arm already does. Measure the reference the same way and subtract. The tool reports
that subtraction as a matched-cell contrast, per speaker, in the physical-versus-phantom table.

An absolute comb depth says as little on its own as an absolute angular miss, and for the same reason.

### Limits

- Ripple **slower** than the analysis band is indistinguishable from a spectral tilt, and the detrend
  removes it. Copies within about 1 ms of each other put their first notch above the band and read as
  flat. That is the time-aligned case, and it is the right answer at the point the alignment was
  computed for and the wrong one everywhere else.
- Ripple **faster** than a sub-band is averaged away. On a 400–1200 Hz band the sub-band is 100 Hz, so
  copies more than about 5 ms apart are understated.
- A band too narrow to hold enough sub-bands is **refused**, so `--tone` cells get no comb number.
  That is correct rather than a gap: one frequency cannot show a frequency-dependent effect, and a
  comb evaluated at one point is just a gain. `comb_ok` is a separate flag from `ok` for this reason.
- There is **no first-order ceiling** here. Nothing inverts `b_n(ka)` or projects onto spherical
  harmonics, so `ZYLIA_FOA_FMAX` does not apply. The band is still matched to the DOA's so the two
  numbers can be read side by side, and because the harness stimulus has no energy outside it.
- `n` must be at least `ZYLIA_COMB_NFFT` (8192, which is `VAL_ANALYZE`). The frame is four times the
  DOA's because here the frequency resolution *is* the measurement, where the DOA only integrates a
  vector over bins.

Measured in the `zylia` test on a synthetic delay-and-add: one coherent copy reads 1.6 dB, the
analysis floor. Adding weaker copies takes it to 4.6, 5.6, 8.6, 10.3 dB. One capsule carrying a
different comb pulls the answer 0.4 dB off and drops quality from 1.00 to 0.58; masking it recovers the
clean answer exactly.

## Sweeping SPCAP focus

`--focus 4,32` measures more than one SPCAP focus in **one session**, at the same placements and the
same directions, so the focus contrasts are matched-cell. Focus is inert under DBAP and VBAP, so those
are measured once however long the list is.

This exists because focus was dialed by ear and nothing could put a figure on what dialing it did.
Focus sets how many speakers carry a source. More speakers means more coherent copies interfering, so
the knob trades image tightness against comb depth, and comb depth is now a number.

**Where the sweep has power, and where it does not.** Two target populations behave completely
differently, and mixing them hides the effect:

| target | what a tight lobe does | sweep |
| --- | --- | --- |
| **at a speaker's own position** | collapses onto that one real speaker: one copy, nothing to interfere with | full range, monotone |
| **an arbitrary direction** | falls between speakers, so the tightest render is two near-equal copies | weak, non-monotone |

Two equal copies null completely. They comb harder than twenty do. So tightening focus does **not**
monotonically reduce comb depth on a general direction, and reading focus off the grid rows will
mislead you.

Read the **physical-versus-phantom table** instead. Its targets sit on speaker positions and every row
carries the comb excess over that same speaker driven alone. On the default grid, off center, in
simulate:

| focus | comb depth | over the real source |
| --- | --- | --- |
| 2 | 7.5 dB | +6.7 |
| 8 | 7.4 dB | +6.5 |
| 16 | 7.1 dB | +6.3 |
| 32 | 3.5 dB | +2.7 |
| 64 | 1.1 dB | +0.3 |

Every interval excludes zero. The physical floor is 0.8 dB. Read that as: below about focus 16 this
array is combing as hard as it can, and the knob does not start buying anything back until the lobe is
tight enough to land on a single speaker.

What the sweep cannot tell you is which setting to ship. Comb depth is one side of the trade; the
other is coverage and image stability as the listener walks, which the angular miss and the
tracked-versus-fixed contrast measure. Run both columns and decide against both.

## Running it

```
bwa_validate --simulate                                   the whole flow, no hardware
bwa_validate --driver "ASIO MADIface USB" --mic-in 26     on the rig
bwa_validate --layout cave_layout.json --azimuths 24 --out cells.csv
```

| flag | meaning |
| --- | --- |
| `--simulate` | analytic field instead of a capture; no device needed |
| `--layout <path>` | `cave_layout.json` (default: the built-in grid) |
| `--driver <name>` | ASIO driver (default: first with enough channels) |
| `--mic-in <n>` | first Zylia input channel |
| `--position x,y,z` | one placement; repeatable, replaces the defaults |
| `--positions <file>` | placements from a file, `x y z [label]` per line, `#` comments |
| `--azimuths <n>` | azimuths per elevation row (default 12) |
| `--radius <m>` | source distance from the sweet spot (default 1.4) |
| `--focus <v[,v,…]>` | SPCAP lobe sharpness; a list sweeps it in one session. See above |
| `--density <v>` | SPCAP placement-correction exponent, one value for the run |
| `--out <file.csv>` | every cell, one row each |
| `--no-prompt` | don't wait for ENTER between placements (unattended runs) |
| `--track <id or name>` | follow the ZM-1's stand as a tracked rigid body; needs `--survey` |
| `--survey <file>` | body-frame capsule survey the tracker rotates per placement |
| `--natnet-server <ip>` | Motive host; **required** to `--track` by name, since the streaming id is resolved from Motive's model definitions |
| `--natnet-multicast <g>` | NatNet group (default 239.255.42.99) |
| `--track-sim` | self-check, see below |
| `--no-reference` | skip the physical reference arm (each speaker driven alone) |
| `--tone <hz>` | measure with a sustained tone instead of broadband; see below |
| `--inject-fault <ch>` | self-check, see below |

The built-in placements are a plausible walking envelope, not your room. Pass your own with
`--position` or `--positions` and give them labels; the labels come back in the report and the CSV,
which matters once you have more than about four.

```
# mics.txt - surveyed listening positions, room meters
0.0  1.55  0.0    seated center
0.8  1.55 -0.4    front-right standing
-0.9 1.20  0.6    back-left seated
```

### Tracking the microphone instead of measuring it

`--track <id|name> --survey <body-frame survey>` follows the ZM-1's stand as a tracked rigid body.
The pose then supplies both things a typed placement cannot.

**Position.** A tape-measure number carries whatever error your tape does, and at the 1.4 m source
radius a 5 cm error injects about 2° of direction error, which is the size of the phantom penalties
being measured. Worse, in *tracked* mode the mic position is also the solve position, so a
mis-measured mic is mathematically identical to a tracking error and you cannot separate the two
afterwards.

**Orientation, which is the bigger prize.** A survey pins the array's orientation for *that*
mounting. The session remounts the mic six or seven times, and each remount is a fresh unknown yaw.
Fitting that rotation back out of the data is possible but statistical, and it conflates mount error
with the measurement. With a tracked stand it is measured:

```
capsules_room = R(pose) · capsules_body        center = pose_pos + R(pose) · offset
```

So the workflow becomes **survey once, then track**. Run `zylia_survey` at any convenient mount pose,
rotate the result into the stand's body frame, save it with a `ZyliaMount`, and it is good for every
placement afterwards. The `zylia` test measures the payoff on synthetic data: a remount that leaves
the stale survey **18.1° wrong** is reconstructed to **0.033°** from the live pose.

You do not need markers on the sphere. Mount it to something the cameras already see and probe the
offset from the stand's body origin to the array's acoustic center. For a rigid sphere that center is
unambiguous (it is the geometric center), unlike a general microphone where "where is it,
acoustically" is a real question.

Two things the tool will not do for you:

- **The coupling must be rigid and stay rigid.** No shock mount, and do not loosen the collar after
  surveying. You are propagating an orientation through the mount, so a quarter turn on the thread is
  90° of azimuth error and nothing downstream notices. Mark the collar.
- **The offset must be probed.** `zylia_survey` takes source positions relative to a center, so it
  cannot solve for that center. The rotation falls out of the survey plus one pose sample; the
  translation does not.

With `--track` the placements you pass become the **plan**, not the measurement. The tool prints the
tracked position beside the planned one with the delta, and warns past half a meter: a wrong rigid
body or a frame mix-up shows up immediately rather than as a puzzling result three hours later.

This is also the undemanding use of the tracker: the mic is static during a capture, so one good pose
per placement is enough. No prediction, no velocity, no clock-domain question, none of the parts of
the render path that make `natnet.c` hard.

Two safety behaviors worth knowing:

- **A stale pose is refused, not reused.** `pose_read` hands back the last *published* pose forever,
  and NatNet only publishes tracking-valid frames, so an occluded stand or a wrong streaming id would
  silently return the *previous* placement's pose and have it accepted as this one's measurement.
  Each placement is gated on `natnet_status() == LIVE`; one with no live pose is skipped and reported
  rather than measured against a stale one.
- **`--track-sim` proves the wiring** without a rig: it drives the same path from a synthetic mount
  pose and exits nonzero unless the placement hook fired for every placement. That check exists
  because the hook was once passed into the session loop and never called, leaving `--track` inert
  while it announced the tracker was measuring. Asserting on the *result* would not have caught it:
  in simulate the field is synthesized from the same capsule table the estimator reads, so a wrong
  table cancels out and looks healthy. It is the `validate_track` ctest.

### The physical reference arm

Every miss elsewhere in this doc is an **absolute** number, so it silently folds together what the
estimator costs, what the layout survey costs, what the room costs, and what the renderer costs.
Separating them needs a real source measured through the same chain in the same room, which is why
the published protocol moved a physical loudspeaker to each target across dozens of sessions.

We do not have to move anything: **the array's own speakers are physical sources at known
positions.** Drive speaker *i* alone, no panning, and the estimator's answer is a real-source
measurement. On by default; `--no-reference` skips it.

It buys two things.

**A floor.** The reported `physical floor` is what the chain costs before any panning happens. In
anechoic simulate it lands around 0.1°. On hardware it will be larger, and the increase *is* the
room's contribution plus your survey error. If it is not small, stop: a directly driven speaker that
does not land on its surveyed position means nothing measured afterwards is interpretable, and you
find that out in seconds rather than after a session.

**A matched contrast.** Render a phantom at speaker *i*'s own position and you have the same
direction, the same room and the same placement measured both ways, so the pair differences cleanly.
That is the published physical-versus-phantom comparison, obtained without moving a loudspeaker.

Two things to know before reading that table:

- **It is ~0 at the array center, by symmetry.** A blurred phantom's energy vector still points at
  the speaker when you are at the center of a symmetric array. The off-center rows carry the
  information; the center row is a null control.
- **Never pool it across placements.** The distribution is bimodal (≈0 centered, degrees off-center),
  so a pooled median lands in the empty middle and reads as "no effect", the exact opposite of the
  truth. The tool reports per placement for this reason. This is not hypothetical: pooling one
  centered placement with one off-center one during development produced 0.13°, against 1.8° for the
  off-center placement alone.

Expect a triangle panner (VBAP) to show ~0 here even off-center, because it collapses onto the
coincident speaker, while a distance-blurred panner (DBAP) spreads and pays a real penalty. That
difference is a genuine characterization of the panners, not an artifact.

### Stimulus, and where content dependence actually comes from

`--tone <hz>` swaps the broadband default for a sustained tone. The analysis band follows: broadband
gets 400–1200 Hz, a tone gets ±1/6 octave around itself, and a tone whose band sits entirely above
the array's first-order ceiling is refused **with its frequency named**, rather than failing
mysteriously per cell.

Localization is strongly content-dependent, and it is natural to credit that entirely to the room.
That is not the whole story, and the harness can show why.

| | anechoic (simulate) | in a room |
| --- | --- | --- |
| **physical source** (one speaker) | content-**independent** | content-dependent (standing waves) |
| **phantom** (many speakers) | content-**dependent** | more so |

Two mechanisms, not one:

- **The room.** A steady tone sets up a standing-wave field, and a single-point intensity vector
  reports net energy flux, which near a pressure node need not point at the source. Hardware only.
- **The array itself.** A phantom is a coherent sum of many speakers, so its interference pattern is
  frequency-dependent *in free field*. Broadband averages over it; one tone cannot.

The published study's anechoic control used a **physical loudspeaker**, which has nothing to
interfere with, so it could only see the first mechanism. Measured here in simulate, the anechoic
content spread is about **0.1° for a physical source and tens of degrees for a phantom**. So a
simulate run *does* show content dependence for phantoms, and that is a result rather than a fault.

**The negative control is therefore the reference arm, not simulate.** A single driven speaker must
localize the same whatever the content. If that ever stops holding, the analysis chain is at fault
and every content finding built on it is an artifact. The `valid` ctest asserts exactly that and
merely reports the phantom spread, whose size is a property of the array rather than a contract.

One property to keep in mind when reading tone results: the error is *precisely wrong*. Sub-degree
repeatable, tens of degrees biased. Repeats will agree beautifully with each other and with nothing
else.

### Proving the integrity layer on your own data

`--inject-fault <ch>` corrupts that capsule in **every** capture, then requires
`zylia_check_capsules` to catch it at every placement. Exit code 3 if it does not.

Use it before trusting a session. It exercises the check, the reporting, and the exclusion threading
end to end on the exact signals your rig produces, and it costs one extra run. It works on hardware
captures too, not only in simulate, so you can confirm the chain against your real room and your real
noise floor rather than against a model of them.

The negative control comes free: a clean run reports "all 19 healthy", so you can see the check is
not simply flagging everything.

### The session shape

```
for each microphone placement (you move the ZM-1, the tool waits):
    check the capsules once, report anything faulty, exclude it for the rest
    for each condition × {tracked, fixed} × direction:  render → capture → score
```

A **condition** is a panner at one SPCAP focus. With no `--focus` there is one per panner, which is
the old shape. With a `--focus` list SPCAP gets one condition per value and the other two panners
still get one, so a sweep costs only what it measures.

**Only the microphone moves.** Phantom sources are rendered, not carried, so a whole direction grid
sweeps electronically from one placement. That inverts the usual cost of this measurement: a study
that moves a physical reference speaker needs one session per direction, this needs one per
*listening position* and gets every direction free. Half a dozen placements covers the walking
envelope.

Measure each placement, do not eyeball it. The mic position is an input to the scoring.

### Latency does not enter

The stimulus is **steady-state**, not a sweep. A swept measurement has to know its round-trip latency
to the sample, which is most of the difficulty in `calib_capture.cpp`. Here you play and capture
concurrently and analyze a window well inside the steady state, so device latency, driver buffering,
and the Digiface's own delay never reach the result. `VAL_SKIP` is simply "long enough that everything has
arrived".

`ASIOGetLatencies` is still logged at open, as a routing sanity check only. If it looks absurd, the
routing is wrong, and that is worth knowing before you spend an afternoon collecting cells.

### Getting the ZM-1 onto the same device

Full duplex on **one** device: speaker feeds out, 19 capsules in, one clock domain. Two separate
interfaces would leave the render and the capture free-running against each other. Dante Via puts the
ZM-1 on the network the Digiface already presents, so a single ASIO driver exposes both. Same unlock as the
sweep path, see `calibration.md` → "Getting the ZM-1 onto Dante".

A device exposing fewer than 19 inputs is refused. Unfilled capsules would enter the SH projection as
silent arrivals and point somewhere confidently wrong.

## Reading the output

Per placement you get a median miss for each panner in each solve mode, so a bad placement is visible
before you move the mic again. Then the claim worth making:

```
matched-cell contrast (fixed - tracked), median of paired differences
  SPCAP  (+0.70,+1.50,+0.00)  +10.2  CI [+6.4, +11.2]  *
  (* = interval excludes zero)
```

**Matched-cell** means the same direction and the same listening position under both conditions, so
the difference is paired. That matters more than it sounds: a median of differences and a difference
of medians are not the same number, and only the first is a statement about the same cells. Intervals
are percentile bootstrap on the median, fixed seed, so a reported interval is reproducible.

Medians rather than means throughout. Localization error is heavy-tailed, a handful of directions
fail badly, and a mean would follow them around.

`--out` writes every cell: panner, focus, density, arm, mode, positions, target direction, measured
direction, miss, diffuseness, comb depth, comb quality, and whether each estimator resolved it. The
`reference` column says which arm a row came from: 0 is a grid phantom, 1 is a speaker driven alone,
2 is the phantom matched to it.

## What it has found so far

On the **default grid**, in simulate, so this is the rendering term with no room. Treat it as a
worked example, not as a result about your installation.

**Tracking fixes horizontal displacement. It does not fix height.**

| displacement | contrast (fixed − tracked) |
| --- | --- |
| horizontal, 0.7 m | DBAP +2.7°, VBAP +4.6°, SPCAP +10.2°, all intervals exclude zero |
| height, 0.4 m | all three intervals include zero |

Height still hurts in absolute terms (SPCAP 1.9° → 7.9°). The panner solve re-aims correctly for your
new height; what it cannot undo is that the array's vertical resolving power from there is worse, and
that the alignment delays were computed for one reference height. **Horizontal displacement is a
tracking problem. Height is a placement and calibration problem.** Different failure, different
remedy, and pooling them into one "off-center" number hides both.

**The optimizer's proxy is trustworthy for some panners and not others.** `valid_re_proxy` computes
the energy-vector direction error and Frank spread that `bwa_layout_tool` climbs, on the same cells
the harness measures acoustically. Spearman ρ against the measurement:

| panner | rE direction (within position) | Frank spread |
| --- | --- | --- |
| DBAP | **0.82** | 0.45 |
| SPCAP | 0.44 | 0.23 |
| VBAP | 0.19 | **0.77** |

So the axis worth optimizing is not the same for every panner. VBAP puts all energy on two or three
speakers and gets direction right by construction, leaving image focus to carry the variation; DBAP
spreads energy over many speakers, so its direction error genuinely varies and tracks the acoustic
outcome. `bwa_layout_tool` now defaults its focus weight and its badness-map metric per panner on
exactly this basis, see `layout-schema.md`.

## Limits

Say these out loud before quoting any number from this tool.

- **Comb depth is a spectrum statistic, not a loudness model.** It says how rippled the response is,
  not how audible that is. The ear is far less bothered by a comb it can move its head out of than by
  one that follows it, and this measurement cannot tell those apart from one static point.
- **A microphone is not a listener.** A single-point intensity vector is a conservative observer.
  Listeners get two ears, head movement, and the precedence effect. Expect this to over-report,
  especially for narrowband content, where a standing wave can send the net energy flux at one point
  somewhere the source is not.
- **First-order band ceiling.** 400–1200 Hz for the primary estimator. That is the low end of where
  humans localize broadband content, and the whole HF regime is above it. `zylia_srp_doa` reaches
  ~3.3 kHz as a cross-check, not as the primary outcome.
- **Simulate is anechoic.** Rendering term only.
- **Single point per placement.** The map is as dense as the placements you measure.
- **A number quoted without its stimulus is incomplete.** Localization error is ordered by effective
  bandwidth: broadband best, narrowband tones far worse. The default is broadband, which is the
  *optimistic* end of that range; `--tone` reaches the other end. Always say which one a figure came
  from. The tool prints the stimulus and its analysis band in the header of every run for this reason.
- **Synthetic stimuli only.** Broadband tone-sum or a single tone, both steady-state. Real program
  material (speech, applause, transients) is not covered, and speech in particular sits between the
  two ends measured here. The steady-state property is what makes the measurement
  latency-independent, so supporting file playback would cost that.

## Not built yet: the rotating two-mic ITD rig

`bwa_set_cap` (compensated amplitude panning, [spatialization.md](./spatialization.md)) claims the
rendered ITD stays correct **as the listener turns their head**. Nothing in this tool can see that. A
spherical microphone array measures the field at a point; ITD is a property of two ears on a head, and
rotating the ZM-1 does not create one. So CAP currently has unit-test evidence and no hardware
evidence.

The instrument that would settle it is cheap. Two omnis at ear spacing on a rigid sphere, rotated
through yaw, with the ITD read by cross-correlation. Notes for whoever builds it:

- **Rotation is the measurement, not extra sampling.** A spherical array at a fixed point already
  samples the field completely up to its order, so turning it tells you nothing new about the field.
  What rotation changes is the **ear pair's orientation**, which is the only variable CAP's claim is
  about.
- **The ZM-1 is a half-scale head.** Two roughly antipodal equatorial capsules on its ~10 cm sphere
  give a usable ITD proxy for free, before anyone buys anything. Rigid-sphere diffraction holds below
  ka ~ 1, so a ~5 cm radius stays valid to about 1.1 kHz, which covers CAP's whole band and is
  actually a wider valid range than a real head's ~620 Hz. The cost is that the ITD comes out roughly
  half a head's, so this validates **stability under rotation** and **monotonicity with intended
  azimuth**, never absolute correctness. Stability is the discriminating half.
- **Put markers on the mount.** OptiTrack is already in the room, so the rotation angle can be
  measured rather than assumed.
- **The floor already exists.** The physical reference arm above drives one speaker alone, which is a
  real source. Put a phantom at the same bearing, sweep yaw, and compare ITD against that speaker.
  Plain dual-band should drift with yaw and CAP should not. If both sit under JND at this array
  density, that kills CAP for the cost of one sweep, which is the point of measuring first.
- **It settles the CAP band too.** The 700 Hz crossover CAP inherits is a rV/rE number, not an ITD
  number, and the sources disagree (VISR says ~1000 Hz, Zhao et al. 1500 Hz). Sweeping the analysis
  band on this rig answers it directly.

The same rotation buys two things for free on the estimators already here: same field, different
capsules, which separates capsule error from field structure far more sharply than
`zylia_check_capsules` can from one orientation, and a direct bias check on
`zylia_intensity_doa`/`zylia_srp_doa`, whose estimates should rotate exactly with the array.

## Where the code lives

| file | what |
| --- | --- |
| `src/valid.c` / `valid.h` | render, score, statistics, the simulated field. Hardware-free, unit-tested |
| `src/zylia.c` | the estimators: intensity DOA, integrity, SRP-PHAT cross-check, comb depth |
| `examples/validate.cpp` | `bwa_validate`, the session driver |
| `examples/valid_capture.cpp` | full-duplex ASIO. **Rig-bound, not verified on hardware** |
| `test/valid_test.c` | the `valid` ctest: statistics, the feed/analytic agreement, the sweep |
| - | the `validate_sim` ctest: the whole session loop; `validate_fault`: the integrity chain; `validate_focus`: the multi-condition sweep |
| `test/zylia_test.c` | the `zylia` ctest: estimator, sign cross-check, integrity, order step-down |

`valid_capture.cpp` carries the same caveat as `calib_capture.cpp`: it mirrors a known-good ASIO host
sequence, and it has not been run against hardware. Everything it feeds is tested.
