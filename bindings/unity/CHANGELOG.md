# Changelog

All notable changes to `com.brainworks.bw_audio`.

## [Unreleased]

### Added: compensated amplitude panning (`bwa_set_dual_band_cap`)

Dual-band's low band aims the velocity vector at the source and takes whatever `|rV| < 1` the speaker
geometry gives. The shortfall is direction-dependent, so the rendered interaural time difference falls
short of a real source's by a varying amount and the image **shifts when you turn your head**. CAP
(Menzies and Fazi) constrains the one quantity the ear reads below the crossover instead: the
interaural component of the summed field, `rV . e == u_s . e`. Matching one scalar is satisfiable
where matching a 3-vector is not, so the ITD comes out exact and stays exact as the head turns.

- **`bwa_set_dual_band_cap(e, on)`** is off by default, live-toggleable, and it **requires `bwa_set_dual_band`**
  since the low band is the only thing it touches. Applied as a projection on top of the selected
  panner rather than as a fourth panner, so it inherits DBAP's or VBAP's image and only corrects ITD:
  facing the source it is a no-op and reduces to the seed panner, and it fades out with
  `bwa_source_set_spread` (an engulfing source has no single bearing to fix).
  Renamed from `bwa_set_cap` before release: `cap` already means CAPACITY in three calls in this same
  header, and carrying the parent toggle's name matches `bwa_set_max_re` -> `bwa_set_max_re_split`.
  Unity: a `dualBandCap` inspector toggle plus `SetDualBandCap(bool)`. Godot: the `dual_band_cap`
  property and `BwaEngine.set_dual_band_cap`.
- **This is the first engine feature that reads head orientation into the speaker path.** Everywhere
  else orientation enters only at the binaural decode, so `CMD_COMMIT` now dirties voices on a
  quaternion change, gated on CAP being on so a tracked head does not re-solve every voice every
  block for a rotation nothing downstream reads. Wants a real tracked pose; aimed at the seated case,
  where it rebuilds no panner cache at all (everything is computed in room space, so a head that only
  turns invalidates neither the SPCAP nor the VBAP direction cache).
- Measured over a full head-yaw sweep on the default grid, worst `|rV.e - u_s.e|` is **0.017 with CAP
  against 0.404 without**. CAP's entire residual is the 2 of 24 yaw angles where the target is not
  achievable: `rV` is a convex combination of speaker directions, so no non-negative gain vector can
  render an ITD **more lateral than the most lateral speaker the panner lit**. CAP saturates at that
  bound rather than diverging. It is an array-density limit, not a defect.
- Not implemented: the published method's near-field ILD arm (one first-order filter per image). It
  needs per-speaker frequency-dependent gain, which would make this a render mode rather than a
  gain-vector modifier. The near-field proximity shelf and near-listener widening cover adjacent
  ground. See docs/spatialization.md for how this differs from VISR's own CAP, which minimizes energy
  and permits negative gains where this minimizes change from the seed and does not.

### Added: re-align the array to the tracked listener (`bwa_set_tracked_align`)

The per-speaker delay and gain trims align arrival times at ONE fixed point, `Layout.ref`, so the
array is time-coherent there and progressively less so as the listener walks away. This re-references
that alignment onto the tracked head, in the output stage, so coherence follows the listener. Same
idea as VISR's `librcl/listener_compensation`.

- **`bwa_set_tracked_align(e, on)`** plus **`bwa_set_tracked_align_guards(e, dead_zone_m,
  slew_frames_per_s)`**, off by default. The enable and its tuning are separate calls, matching
  `bwa_set_limiter` / `bwa_set_limiter_ceiling`, so A/B-ing the toggle never resets a dialed guard.
  Per speaker the correction is pure geometry against `Layout.ref`: `extra delay = (dref - dlis) * rate / c` and
  `extra gain = dlis / dref`, so walking toward a speaker delays it further and turns it down. The
  delay set is shifted to a minimum of zero, which keeps it purely relative and makes a listener
  standing at `ref` **bit-exact** identity rather than approximately so. Both `<= 0` arguments revert
  that one knob to its default, the same sentinel `bwa_set_spcap_focus` uses. Unity: `trackedAlign` /
  `trackedAlignDeadZone` / `trackedAlignSlewFramesPerSecond` plus `SetTrackedAlign(bool)` and
  `SetTrackedAlignGuards(float, float)`. Godot: the matching
  properties and `BwaEngine.set_tracked_align`.
- **Off by default because every delay change is a resampling event.** A walking listener means 26
  delay lines gliding at once. Two guards, both tunable: a **dead zone** (default 0.05 m, since
  tracker jitter would otherwise keep the array permanently gliding) and a **rate limit** (default
  about 63 frames/s at 48 kHz, stated internally as a 0.45 m/s closing speed so it means the same
  thing at any sample rate or speed of sound). That rate over the sample rate IS the resampling
  ratio, so the default bounds the pitch shift at 0.13%, about 2.3 cents. A brisk walk outruns it on
  purpose: stale alignment is the cheap failure, warble is not.
- Nothing here touches the gain solve, so it composes with every panner and re-solves nothing, and
  it needs no `pan_gen` bump. It reads the active listener position, so it fires on **both** listener
  paths, the committed pose and the internal tracker that writes that field directly.
- **Two limits worth knowing before rig day.** The level trim is clamped to +/-6 dB, and that clamp
  binds inside the working area: on the shipped grid a listener 1 m off center already asks for
  -9.5 dB on the nearest speaker, and at 1.5 m is standing on one. Treat the level half as partial
  (the delay half stays exact). And the min-subtraction adds position-dependent latency, about 280
  frames (5.8 ms) at 1 m off center, which `bwa_get_output_latency_frames` does **not** report, so a
  client syncing video against the DSP clock will drift as the listener walks.
- Untested on hardware. Whether the coherence gain beats the warble the rate limit still lets through
  is a by-ear rig call.

### Added: hole-aware spread floor (`bwa_set_hole_spread`)

The real array is a barrel, open at both poles, so a source aimed into a pole has no speaker anywhere
near it. The panner's hull closes the hole with big triangles of distant speakers: the rendered
direction stays about right, but the energy is carried by speakers up to 113 degrees apart, which is
a split image rather than a phantom. Imaginary pole speakers were tried and rejected (see below).
This is the other fix. A source with no speaker near it is genuinely not a point, so stop asking the
array to pretend, and widen it into an honest diffuse source instead.

- **`bwa_set_hole_spread(e, strength)`** is off by default (`strength` 0), live, and clamped at 2.
  Per voice the engine measures one angle, the **gap** from the source bearing to the nearest speaker
  bearing seen from the tracked listener, and floors the voice's effective spread at
  `strength * clamp((gap - knee) / (90 deg - knee), 0, 1)`. Unity: a `holeSpread` inspector slider
  plus `SetHoleSpread(float)`. Godot: the `hole_spread` property and `BwaEngine.set_hole_spread`.
- **`knee` is the array's own mean nearest-neighbor speaker angle**, the same geometry SPCAP derives
  its lobe width from. That measurement moved into a shared `layout_mean_speaker_spacing()` and the
  SPCAP focus derivation now sits on top of it, so the two features read one measurement of the
  geometry instead of mirroring it. The cube grid still derives focus 12.70, unchanged.
- **Inert on a covering array by construction, not by tuning.** No direction on an array that
  surrounds the listener is ever a full speaker spacing from a speaker, so the floor is exactly 0
  there. Measured: the default grid's worst gap over a 4096-direction sweep is 27.3 degrees against a
  37.5 degree knee, and the test pins the worst per-bearing energy change at **exactly 0.00e+00** with
  the knob at full strength. On the barrel the same sweep reaches a 57.8 degree gap and a 0.428 floor.
- Composes with the metric-size and near-listener floors as a **max**, feeds the ordinary spread
  machinery (spread mode, decorrelation, gain ramps), and does nothing under `BWA_PROFILE_BINAURAL`,
  which has no speakers and so no holes.
- **It weakens CAP on the sources it widens**, because CAP's strength is `1 - spread`. That is correct
  (a deliberately wide source has no single bearing whose ITD is worth fixing) but worth knowing
  before A/B-ing the two knobs together, since raising one quietly lowers the other.
- The mapping is a defensible first cut, not a measured optimum. `strength` is the rig-day A/B knob.
  Two known gaps: the offline scoring path (`bwa_panner_gains_batch`) does not run the spread solve,
  so this cannot be swept the way `--focus` sweeps SPCAP, and ISM reflection images bypass it.

### Investigated and rejected: imaginary speakers in the point-source panner

The real array does not surround the listener (speakers mount between the CAVE screen cube and the
truss, so it is a barrel open at both poles). Closing those holes with an imaginary pole speaker, as
`allrad.c` already does for the diffuse bed and as VISR's `<virtualspeaker>` does with explicit
routes, **was tried and made point-source localization worse**: rE direction error against the
intended bearing rose from 14 degrees to 26 (routed to the rim) or 30 (share discarded) at 60 degrees
below the horizon, and only the exact pole improved. An imaginary speaker is a triangulation vertex,
so it claims a share of every direction in the hole and drags it poleward. No code change; the
measurements and the reasoning are recorded in docs/spatialization.md so nobody repeats it.

### Added: SPCAP focus/density tuning, and an ABI break to score it (`BWA_VERSION` → 0.11.0)

SPCAP's lobe sharpness and placement-correction exponent were compile-time `#define`s (12.0 / 2.0).
The 12 was only ever right for the 26-speaker cave, so `focus` now **derives from the array**: the
mean nearest-neighbor angle between speaker directions, then the exponent that puts the lobe 6 dB
down in energy there. The cube grid lands on 12.70; a 6-speaker cross derives 2.0, a 12-speaker ring
20.0. `density` keeps a plain 2.0 (nothing measurable maps onto it).

- **`bwa_set_spcap_focus(e, focus, density)`** retunes SPCAP live, per-frame-safe, and the change
  reaches **every** source on the next block including sources that never move. Pass `0` or less for
  either argument to revert *that one* to its default. Inert under DBAP and VBAP. Neither value
  lives in the layout file: like `bwa_set_near_spread` and `bwa_set_dual_band`, persisting a dialed
  value is your application's business. Unity: `spcapFocus` / `spcapDensity` inspector fields plus
  `SetSpcapFocus(focus, density)`. Godot: `BwaEngine.set_spcap_focus` and
  `BwaEngine.get_spcap_focus_default`.
- **`bwa_spcap_focus_default(positions, n)`** is the pure companion (no engine handle, same contract
  as `bwa_panner_gains_batch`), so a tool can print what an in-progress array implies before you
  override it. Returns 0 on bad arguments.
- **ABI break: `bwa_panner_gains_batch` gained `float focus, float density`** immediately before
  `out`, honoring the same `<= 0` sentinel: either one at or below zero means "the default for THIS
  array", so `focus` falls back to the value derived from the `positions` you passed, `density` to
  2.0. Both are inert under `BWA_PAN_DBAP` and `BWA_PAN_VBAP`. Update call sites: the old
  `(panner, positions, n, lis, srcs, nsrc, out)` becomes
  `(panner, positions, n, lis, srcs, nsrc, 0f, 0f, out)` for identical behavior. This is what lets a
  layout be **scored** at the tuning it ships with. `bwa_layout_tool` now runs its Score board, its
  rE coverage overlay, its badness map and its optimizer cost through the dialed value, with focus
  and density sliders on the analyze panel beside the preview panel's live pair, and a headless
  `--score <file> focus=<n> density=<n>` for sweeping the knob offline.
- Both GUI tools drive the knobs by ear too: the playground's localization scene gained a panner
  combo, focus and density sliders and a "default" button; the layout tool's preview panel the same
  beside its `B` panner A/B. Each prints what the loaded array derives.

### Added: layout-tool conditions, stages, and pinned speaker allocation

What a layout is optimized FOR is now a first-class artifact in `bwa_layout_tool`,
because collaborators disagree about it. A named **condition** bundles the objective
knobs with a scoring-shell restriction: `3d` (the full sphere, the old behavior),
`horizontal` (only source directions within 15 deg of the ear plane, for planar
localization), and `visual` (a front wedge, azimuth and elevation within 30 deg of
+z). The GUI gets a condition combo plus band/azi sliders; the Score board follows
the active condition. Headless, conditions chain as **stages**
(`--optimize file dbap horizontal,3d`), each stage re-anchoring the leash at the
previous result and reporting before/after scores plus ear-plane occupancy.

- Staging alone cannot protect an allocation: objectives compete, and a `3d` stage
  pulls plane speakers back toward elevation (measured on the default dome: 10
  in-slab speakers eroded to 5). Allocation is therefore a CONSTRAINT:
  **`"pin": "plane"`** per speaker (plus top-level `pin_slab_m`) holds a speaker to
  the ear-plane slab through optimizer trials, snap, and optimizer start. Both are
  authoring-only fields; the engine's loader ignores them. With 12 of 26 pinned the
  plane score lands at its ceiling for about 0.2 deg of full-sphere mean.
- `--score [file] [condition] [fixed|moving] [ears=<m>]` scores under a named
  condition, for a seated install (sweet spot only) instead of the roaming grid,
  and at the install's actual ear height. `--optimize` takes the same tokens.
  `ears=` moves everything plane-shaped together: the band, the pin slab, and the
  delay-alignment point written on save.
- `--optimize ... radial` moves speakers only along the ear ray: directions frozen,
  radii free. The cross-panner pass; optimize for VBAP (direction structure), then
  a radial DBAP pass refits distances without disturbing the triangulation.
- `--optimize ... guard=<panner>[:tol]` is the constrained form: while climbing the
  target panner, moves that let the guard panner's cost slip more than tol above
  its stage-start value are rejected. "The best VBAP layout that never lets DBAP
  slip" is DBAP-optimize first, then a guarded VBAP climb.
- The search itself grew past greedy: best-so-far is always tracked and shipped
  (stopping, preview, and headless all restore it), `anneal` adds Metropolis
  acceptance with per-trial cooling, and `restarts=<n>` basin-hops from the best
  layout plus a kick. Motivated by measured multimodality: identical inputs landed
  26.8 vs 31.2 deg worst on different random paths.
- The GUI climb now stops itself at the same step floor the headless runs use
  (it previously ran until stopped by hand), restores the best layout, and reports
  convergence in the HUD.
- `--optimize ... leash=<m>` sets the per-stage displacement cap from the CLI
  (previously GUI-only), and every optimize start projects the incoming layout into
  the room constraints and pin slabs, so an infeasible generated file starts
  feasible instead of leaking through a run.
- New ABI: **`bwa_bed_gains_batch`**, the bed counterpart of `bwa_panner_gains_batch`:
  the per-speaker gains the diffuse-bed decode (AllRAD/EPAD, optional max-rE, the
  engine's real builds) produces for plane-wave directions over a candidate layout.
  The layout tool uses it for an **AMBI** scoreboard/--score row and a
  `bed=<wt>` objective term, so a layout can be optimized for what ambisonic
  content wants (spherical uniformity), not only for the point-source panners.
  `epad` / `allrad` / `maxre` tokens (GUI: bed decode combo + max-rE checkbox)
  grade the decode the install actually ships.
- The measured tradeoffs live in docs/layout-schema.md (pin-count trend, the
  visual-wedge verdict per panner, the leash-matching caveat). Headline: narrow
  conditions express requirements, they are not accuracy shortcuts; DBAP gained
  nothing in the wedge from wedge-only optimization, VBAP's fixed solve did.
- The layout-tool suite grew 15 -> 17 tests: band/wedge scoring on synthetic
  layouts, condition-preset wiring, pin enforcement (including the regression where
  a file-authored pin starting outside its slab was never projected in), and the
  fixed-observer scoring model.

## [0.5.0]

### Added — device health (`bwa_get_health` / `bwa_get_xruns`)

The third dogfooding gap, and the one that cost three probes: every readback described the RENDER —
voices, meters, the clock — and nothing counted blocks the device asked for and didn't get. You could
prove the render path was clean and still not know whether the callback missed its deadline.

- **`bwa_get_xruns(e)`** answers "is the device being starved?" in one line. **`bwa_get_health(e, &h)`**
  fills a `bwa_health` when you need the diagnosis: `blocks` (the denominator), `xruns` and
  `dropped_frames` (the device ran on without us), `driver_resyncs` (the driver reporting a
  discontinuity itself), `late_blocks` and `peak_load` (OUR render overrunning the block period —
  the cause, not the symptom), and `stream_starves` (a streamed voice's ring ran dry; the device kept
  its deadline, we had nothing to give it).
- Most of this was already being computed and thrown away. The ASIO callback predicts the next
  sample position for its fallback clock; comparing the driver's actual position against that
  prediction IS the dropout, in one comparison. `kAsioResyncRequest` was already handled and its
  information discarded.
- **`bwa_get_health` returns a bool, and that is the load-bearing part.** False = this configuration
  cannot observe a dropout at all: not started, the manual sink (no clock, no deadline — an offline
  render cannot miss one by construction), or a driver that never flags a valid sample position. In
  every one of those `xruns` is 0, and reading that as a clean bill of health is how a starved device
  goes unnoticed. Same trap as the latency-0 bug two entries down; this time it is designed out.
- **Tested off-hardware, honestly.** A real missed deadline needs a real device, but the arithmetic
  that turns a position jump into a count is ordinary code: the gap rule is factored into
  `sink_position_gap` and unit-tested (including its refusals — a backward or absurd jump is a driver
  reset, not a dropout), and a null-sink injection hook makes the sink report a position skip it did
  not render, so the whole path from detection to readback runs under ctest. The null sink's
  `late_blocks` are real, not simulated: that thread has a genuine deadline.
- Unity: `Engine.GetHealth(out BwaHealth)` and `Engine.Xruns`. Godot: `get_health()` returning a
  Dictionary (`measured` included) and `get_xruns()`.

### Changed — units in names, where they earn it

The narrow generalization of the `get_output_latency` trap below, applied once and then written
down. The rule: **a unit belongs in a name when the quantity has two live units.** Time is the only
one in this ABI — frames and seconds are both real — so every time-valued name now says which.
Distances (meters), frequencies (Hz), angles (radians) and gains (linear) have no competitor and
stay unmarked, carrying the unit on the value (`radius_m`, `xover_hz`, `yaw_rad`) where it helps.
A decibel value must say `_db`, since linear is the unmarked default. Stated in
docs/api.md → "Coordinates and units".

- **Three C symbols renamed** (the only frames-valued names that did not say so):
  `bwa_get_output_latency` → `bwa_get_output_latency_frames`, `bwa_source_get_playhead` →
  `bwa_source_get_playhead_frames`, `bwa_bed_get_playhead` → `bwa_bed_get_playhead_frames`.
  Signatures and semantics are unchanged. This also closes a layer mismatch: the Godot binding
  says `_frames`, so C now agrees with it.
- **Godot: `get_playhead()` → `get_playhead_frames()`** on `BwaSource` and `BwaBed`, beside the
  existing `get_playhead_seconds()`. That surface is now uniform: `seek_frames`/`seek_seconds`,
  `get_playhead_frames`/`_seconds`, `get_output_latency_frames`/`_seconds`.
- **Unity is unaffected at the C# level** — only the P/Invoke declarations follow the C symbols.
  `Playhead` / `PlayheadSeconds` and `OutputLatency` keep their names: `AudioSource.timeSamples`
  vs `time` is Unity's own spelling of the same pairing, and there is no host collision to fix.
- Deliberately NOT renamed: the `sample`/`frame` synonym (`start_sample`, `dsp_sample`), which
  denotes the same thing for mono voices and has never misled anyone, and `ir_seconds` → `ir_s`
  for consistency with its `_s` siblings. Both are churn against a frozen-soon ABI.

### Fixed — a dogfooding pass on the Godot addon

All six from an outside install of `bw_audio-godot-0.4.0.zip`. The two expensive ones are the same
failure in different clothes: a call that could not be checked, and a number that could not be
interpreted. Both stayed invisible off-hardware, because the null sink returns the benign value
(nothing to drop, zero latency).

- **`bwa_play_oneshot` now returns `bool`** — whether the one-shot was ACCEPTED. It was `void`, so
  a caller could not tell "played" from "clip missing" from "dropped because the voice pool or
  command ring was full", and the only workaround was probing `sound_get_channels` as a proxy for
  load success. False sets `bwa_last_error` to which of the three it was. The drop itself is
  unchanged and still correct: one-shots never steal, so spam cannot evict named sources. There is
  still no handle to poll — use `bwa_source_create` + `bwa_source_play` if you need to track the
  voice. Unity: `Emitter.PlayOneShot()` returns `bool`. Godot: `BwaEngine.play_oneshot()` returns
  `bool`. Adding a return to a `void` C function is caller-compatible, so existing C call sites
  that ignore it keep working.
- **Godot: `get_output_latency()` is now `get_output_latency_frames()`**, with a new
  `get_output_latency_seconds()` beside it. Godot's own `AudioServer.get_output_latency()` returns
  SECONDS, so the identical name returning frames read as seconds to anyone who knew that call — a
  normal 960-frame ASIO latency displayed as `960000.0 ms`. It hid indefinitely because the null
  sink returns 0, and 0 is 0 in any unit.
- **Godot: `seek()` is now `seek_frames()`**, with a new `seek_seconds()`, on both `BwaEmitter` and
  `BwaBed` — the same collision found by sweeping for it (`AudioStreamPlayer3D.seek()` takes
  seconds, so `seek(1.5)` silently truncated to frame 1). The engine's own unit stays frames
  everywhere, because that is what the dsp clock counts.
- **Godot: the `profile` property now spells out what each value does** in the inspector
  (`"Binaural — headphones, direct render"` and friends) instead of naming the enum, and a new
  configuration warning fires when `profile` is Cave on a machine with no ASIO driver — the case
  that renders 26 channels into nothing and is silent by design. It is the highest-stakes property
  on the node and the inspector is where the choice is made.
- **A one-call driver list**: `BwaEngine.get_asio_drivers() -> PackedStringArray` and Unity's
  `Engine.AsioDrivers`. The count/name pair remains, but it was undocumented and had to be found by
  grepping the DLL's strings.
- **The release artifacts no longer ship dangling doc pointers.** The 0.4.0 zip contained
  `playground.gd` and `scenes.gd` citing `api.md` and `THIRD_PARTY-NOTICES.md` citing
  `docs/build.md`, none of which are in the zip. Both packs now run `tools/dist/doc-pointers.ps1`,
  which rewrites every repo-doc reference in the staged tree to a permalink at the packed commit
  and then FAILS the pack if any relative `.md` reference is left that the stage cannot satisfy.
  The Unity tarball had the same bug and is fixed the same way.
- **ASCII in every runtime-printed string** in the Godot binding: an en-dash in
  `"no running BwaEngine in the scene"` came out as mojibake on a Windows console codepage. Comments
  and docs keep their punctuation; only strings that reach a console changed.

## [0.4.0]

### Added — physical emulation batch

- **`SourceBase.proximity`** (inspector toggle + `bwa_source_set_proximity`): near-field LF boost —
  the shelf rises as the source closes inside ~1 m, so "at arm's length" reads as bass, not just
  level. Loudness comp's near mirror; pushed on init and live from OnValidate like its siblings.
- **`Engine.SpeedOfSound`** (inspector field + live property, `bwa_set_speed_of_sound`): Doppler
  and reflection delays derive from it and glide to a change. 343 air, 1480 underwater; small
  values exaggerate Doppler for slow motion.
- **`Engine.FdnSetDecay(low, high, xover)`** (`bwa_fdn_set_decay`): LIVE FDN decay retune — the
  room-transition knob; the tail keeps ringing, only its slope changes. `<= 0` keeps a parameter.
- **`Engine.SceneSetGround(worldY, material, pressureRelease)`** (`bwa_scene_set_ground`): the
  outdoor degenerate of the room box — one mirror plane, the ground bounce. Replaces the box.
- **`Engine.SceneSetPressureRelease(faceMask)`** (`bwa_scene_set_pressure_release`): flag box faces
  whose image-source reflection inverts — an underwater room's ceiling-as-surface (`1u << 3`), the
  Lloyd's-mirror comb.
- **Directivity note**: `directivity`/`directivityPower` now work in every build — without a Steam
  scene the engine evaluates the same weighted dipole on the audio thread (no binding change).
- **Headphone correction EQ** (`bwa_load_headphone_eq` / `bwa_set_headphone_eq`; Unity raw externs,
  Godot `load_headphone_eq(path)` + the `set_headphone_eq` ramped A/B): an AutoEq ParametricEQ.txt
  for your headphone model, applied to the final stereo of every headphone profile after the HRTF
  decode — the headphone-side align stage (corrects the transducer, not the render; inert in
  `cave`). The Preamp line is honored; a bad file fails with `ErrConfig` and keeps the previous EQ;
  loading and toggling both crossfade. Reload after an engine rebuild (the correction dies with the
  engine; the Godot toggle itself replays on restart).

### Added — clock drift

- **`Engine.GetClockModel(out BwaClockModel)`** → new engine ABI `bwa_get_clock_model`: how fast the
  device clock actually runs against the host clock. `bwa_get_clock`'s pair fixes an exact *instant*;
  this fits the *slope* by exponentially weighted least squares over the same per-block stamps
  (~2 min window, on the audio thread), and reports it as `ppm` with its own `ppmSigma`, plus
  `rateHz`, `spanS`, `jitterNs` and the effective stamp count. `DspTimeAt` re-anchors every frame and
  needs none of it — reach for this when something else owns the timeline (a video file, timecode,
  another render node), when a minutes-long extrapolation has to hold (use `rateHz` in place of the
  nominal rate), or to log the rig's drift. False until the fit has ~1 s of stamps, and again for
  ~1 s after a restart re-bases the device sample position. `ppmSigma` assumes independent stamp
  noise, so read it as a lower bound; `jitterNs` grades the *driver's* stamps, and a driver without
  `kSystemTimeValid` reads worse because the QPC fallback adds dispatch noise.

### Changed — ABI breaks (pre-freeze cleanup, `BWA_VERSION` → 0.10.0)

Deliberate, one-time ABI breaks before the pre-hardware freeze. Update call sites:

- **Removed the bed yaw-shorthand binding** (the `Bwa` P/Invoke was bound but never called). It was
  the pure subset of `bwa_bed_set_orientation(yaw, 0, 0)` — call that with `pitch = roll = 0`
  instead (bit-identical path: yaw-only stays on the exact phasor rotation).
- **Renamed the two engine-global reverb setters** to the `bwa_set_<x>` form, matching the other
  engine-global setters: now `bwa_set_reverb_gain` and `bwa_set_early_reflections_gain` (previously
  the noun-first `..._set_gain` spelling). The Godot method names (`reverb_set_gain` /
  `early_reflections_set_gain`) and the Unity properties (`ReverbGain` / `EarlyReflectionGain`) are
  unchanged — only the underlying C symbol moved.
- **`bwa_set_limiter_ceiling` now takes a LINEAR peak amplitude** in `(0..1]`, like every other gain
  in the ABI — no longer decibels. The default is `0.891251f` (still −1 dBFS). The Unity inspector
  field is now `limiterCeiling` (linear; was `limiterCeilingDb`) with `SetLimiterCeiling(float
  linear)`; the Godot `limiter_ceiling` property range is now `0..1`.
- **`bwa_set_pose_prediction` lead is now in SECONDS**, not milliseconds (matching the fade-time
  calls). The Unity field is `posePredictionS` (was `posePredictionMs`, range `0..0.2`); the Godot
  `set_pose_prediction` argument is seconds.
- **The profile enum was renamed and renumbered** around the new first-class binaural render:
  `BWA_PROFILE_BINAURAL` (still 1) is now the DIRECT per-source headphone render (point sources
  SH-encode at their true listener-relative directions and HRTF-decode — no speaker-array
  simulation in the direct path); the old virtual-speaker array audition is
  `BWA_PROFILE_CAVE_SIM` (2), and the old `BWA_PROFILE_BOTH` is `BWA_PROFILE_CAVE_BOTH` (3, rig +
  the sim tap). Unity: `BwaProfile.{Cave, Binaural, CaveSim, CaveBoth}`; Godot:
  `PROFILE_{CAVE, BINAURAL, CAVE_SIM, CAVE_BOTH}` (the Godot playground now uses CAVE_SIM, since
  its speaker meters visualize the array bus). If you were using `Binaural` to audition the
  ARRAY, switch to `CaveSim`; if you wanted the best headphone render, `Binaural` just got better:
  with the Steam Audio build every point source gets its own true HRTF convolution (one
  `IPLBinauralEffect` per voice), ambisonic beds pass SH→SH, and pathing's indirect field joins
  the binaural decode directly — no speaker-array round trip anywhere in the direct render. No
  API surface changed for this; it's all behind the profile.

### Added — ABI parity (the seven calls the binding had missed)

`Bwa.cs` is **1:1 with `include/bw_audio.h`** again: it now binds every `BWA_API` function except
`bwa_set_output_capture` (an audio-thread callback) and `bwa_render_block` (the manual-sink
golden-render path), both deliberately unbound — Unity uses neither. Seven declarations were added,
with the ones a scene actually authors surfaced on the components:

- **`Emitter.Extent` (Vector2)** → `bwa_source_set_extent`: anisotropic angular width/height (a
  shoreline is wide but not tall, rain tall but not wide). Equal values behave as the isotropic
  `Spread`; setting `Spread` resets it to isotropic (last call wins).
- **`Emitter.SetAttenuationOverride(refDist, rolloff, minGain)`** →
  `bwa_source_set_attenuation_override`: a per-source distance-attenuation curve (rolloff 0 = a
  direction-only cue that never fades; `refDist <= 0` clears it back to the layout curve).
- **`Engine.maxReSplit` / `SetMaxReSplit`** → `bwa_set_max_re_split`: band-split max-rE (taper only
  above ~700 Hz, plain decode below; needs `maxRe` on). Live A/B; the inspector hides it while
  max-rE is off.
- **`Engine.SoundFrames(clip)` / `SoundChannels(clip)`** → `bwa_sound_get_frames` /
  `bwa_sound_get_channels`: asset length (engine-rate frames) and channel count, loaded on demand
  through the same cache as `Load`.
- **`Engine.AsioDriverCount` / `AsioDriverName(index)`** (static, engine-free) →
  `bwa_get_asio_driver_count` / `bwa_get_asio_driver_name`: enumerate the registered ASIO drivers
  before an `Engine` exists, to populate a picker for `asioDriver`.

### Added — `PushEmitter` component

- **`PushEmitter`** — a MonoBehaviour wrapper for procedural (push) sources, filling the one gap
  where the raw `bwa_source_*` push calls (`bwa_source_create_push` / `_push` / `_push_space` /
  `_push_end`) had no component like every other source concept. A positional source you FEED mono
  float PCM at `Engine.SampleRate` instead of a clip (a synth, an engine model, a voice stream):
  `Push(float[])` / `Push(float[], count)` (returns the count accepted), `PushSpace`, `PushEnd()`,
  `IsPlaying`, plus the source-generic surface `Emitter` has that applies to a push voice — `Gain` /
  `FadeTo` / `FadeOut`, `Spread` / `Extent` / `SizeMetres`, `Priority`, `Group`, `Pause` / `UnPause`,
  `SetAttenuationOverride`, `SetOcclusionManual`, `Occlusion`, `Playhead` / `PlayheadSeconds`, `Stop`,
  and the inspector spatial toggles (occlusion, early reflections, reverb send/distance, pathing,
  directivity, doppler, air absorption, loudness comp). It registers with `Engine` and its transform
  rides the same centralized per-frame push, through the one source registry and snapshot loop
  (see `SourceBase` under Changed). Deliberately OMITTED — the engine refuses them
  on a push voice: `Play` / `PlayAt` / `PlayLoop`, `Seek`, `Pitch`, `Queue` / `ClearQueue`,
  `PlayOneShot`, and the file `clip` / `loop` / `playOnEnable` machinery. Mirrors Godot's
  `BwaPushSource` (which splits off `BwaEmitter` for the same reason) adapted to `Emitter`'s Unity
  idioms (lazy `TryInit` with the init-order-race coroutine, generation-safe handle, live `OnValidate`
  re-push). One-way: `PushEnd`/`Stop`/`FadeOut` end the voice, so re-enable the component for a fresh one.

### Removed

- **`Emitter.Position` / `Emitter.PositionSeconds` and `AmbisonicBed.Position`** — the `[Obsolete]`
  forwarders left over from the 0.3.0 `Position → Playhead` rename are gone. Use `Playhead` /
  `PlayheadSeconds` (the content playhead, unrelated to the spatial transform).
- **`Bwa.MaterialPreset(engine, preset)`** — the thin static alias over the already-typed
  `bwa_material_preset` extern is gone; call `Bwa.bwa_material_preset` directly (the two internal
  callers, `Engine.ResolvePreset` and `MaterialAsset.Resolve`, were migrated). The engine-level
  `Engine.MaterialPreset(preset)` convenience (the cached mint) is unaffected.

### Changed

- **`SourceBase`** — the source-generic surface `Emitter` and `PushEmitter` had duplicated
  member-for-member (lifecycle, the per-frame transform push, the live `OnValidate` re-push, and the
  whole knob surface) now lives on one abstract `SourceBase` MonoBehaviour, mirroring Godot's
  `BwaSource` base: a subclass overrides only the create call and adds its own feed. `Engine` keeps
  ONE source registry (the `Register`/`Unregister(PushEmitter)` overloads and the second per-frame
  loop are gone — every source kind runs through the same mutation-safe snapshot loop), and the
  custom inspector now registers on `SourceBase` with `editorForChildClasses`, so `PushEmitter` gets
  the conditional hides + live occlusion bar too. Public API and serialized field names are
  unchanged — existing scenes and scripts migrate untouched.
- **`Emitter` directivity** — the cardioid-weight mapping + `set_directivity` call, duplicated in
  `TryInit` and `OnValidate`, moved into one `ApplyDirectivity()` helper (no behavior change).

### Fixed

- **Play-mode inspector edits no longer wipe a script-set `Extent`** — `OnValidate` re-pushes
  `spread`, which the engine defines as resetting extent (last call wins), and now re-asserts the
  extent after it, exactly like `TryInit` always did. Previously ANY inspector nudge on a live
  source collapsed a script-set anisotropic extent to a point while the `Extent` getter kept
  reporting the stale vector.
- **`SetAttenuationOverride` is mirrored + replayed** (Unity and Godot): it is standing per-source
  state, so a call that loses the init-order race now lands at create, and the override survives a
  disable/re-enable instead of silently reverting to the layout curve.
- **A stale source handle can no longer cross an `Engine` destroy+recreate** — a source component
  records its owning `Engine`; if that engine is replaced while the component stays enabled, every
  call (including `OnDisable`'s destroy) no-ops instead of aliasing the successor engine's
  deterministically-recycled first handles, and the next enable re-creates cleanly.
- **Limiter ceiling can't silently diverge from the engine** — the inspector slider floors at 0.001
  and `SetLimiterCeiling` clamps into `(0..1]` before caching (the engine ignores `<= 0`); the
  Godot hint floors the same way, and its setter converts a replayed pre-0.10 dB scene value to
  linear with a warning instead of silently dropping the authored ceiling.
- **ASIO driver names are UTF-8 across the ABI** — the native enumeration converts the registry's
  ANSI bytes to UTF-8 (and converts an explicit `asioDriver` back before the SDK's byte-exact
  match), so a non-ASCII driver name survives the picker round trip; Godot now decodes with
  `String::utf8`.
- **A failed source create logs on `Emitter` too** (previously only `PushEmitter` checked the
  handle) — unified in `SourceBase.TryInit`.
- **`ProjectCheck` warning** pointed at the wrong menu: "Tools → Engine → Disable Unity Audio" now
  reads "Tools → BwAudio → Disable Unity Audio", matching the actual `MenuItem` path.
- **Docs**: the README and `docs/integration.md` "1:1" claims now name the two deliberately-unbound
  calls instead of overclaiming. The README's live-A/B list gains SPECTRAL spread and max-rE, and
  its load-time list names the bed decoder as AllRAD / EPAD (sampling is no longer selectable).

## [0.3.2-rc3]

- CI tests

## [0.3.2-rc2]

- CI tests

## [0.3.2-rc1]

- CI tests

## [0.3.1]

- Updated build docs.

## [0.3.0]

### Changed — release versioning

- **The git `v*` tag is now the single source of truth for the package version.** `pack.ps1` stamps
  the tag into the packaged `package.json` at build time, replacing the 0.2.0 version/tag guard. The
  committed manifest carries a `0.0.0-dev` placeholder that never needs bumping — cutting a release is
  pushing a tag (plus this CHANGELOG), and `tools/release.ps1` does both in one step. A non-tag pack
  derives a SemVer dev version from `git describe` (`0.2.0-dev.<n>.g<hash>`) so dev tarballs stay
  traceable. No consumer-visible change; the released tarball still carries its real version.

### Added — gapless chaining

- **`Emitter.Queue(clip, loopTerminal)` / `Emitter.ClearQueue()`** → new engine ABI
  `bwa_source_queue` / `bwa_source_clear_queue`: queue a clip to play the instant the current one
  ends, with no gap at the seam (the engine swaps mid-block if the boundary falls there). Queue
  several for a sequence; a `loopTerminal: true` entry is the looping tail — `Play(intro)` then
  `Queue(body, loopTerminal: true)` is an intro→loop across two files. Up to 7 pending; queue *after*
  Play (Play restarts and clears the queue). In-memory mono clips only.

### Added — loop regions + scheduled stop

- **`Emitter.PlayLoop(loopBeg, loopEnd)`** → new engine ABI `bwa_source_play_loop`: the intro→loop
  pattern. Playback starts at 0, plays the intro `[0, loopBeg)` once, then loops the body
  `[loopBeg, loopEnd)` forever (wraps at loopEnd back to loopBeg, not the clip end). Frames are
  engine-rate; loopEnd 0 = the clip end. In-memory clips; a stream loops its whole file. The seam is
  a hard wrap, so author the loop points on matched endpoints.
- **`Emitter.StopAt(stopSample)`** → new engine ABI `bwa_source_stop_at`: a click-free stop on the
  dsp clock (same time base as `PlayAt`). When `Engine.DspTime` reaches stopSample the source fades
  out over one block and ends — never a hard cut, so it can't pop. Block-granular; a later
  Play/PlayAt/PlayLoop clears a pending stop.

### Changed — engine ABI clarity renames (native 0.9.0)

The engine renamed seven symbols for clarity; the binding follows. C#-visible changes:

- **`Emitter.Position`/`PositionSeconds` → `Playhead`/`PlayheadSeconds`**, **`AmbisonicBed.Position`
  → `Playhead`** — "Position" collided with the spatial transform; the readback is the CONTENT
  playhead. The old properties remain as `[Obsolete]` forwarders for now.
- Raw `Bwa` layer follows the C renames: `bwa_source_get_playhead` / `bwa_bed_get_playhead`
  (was `_get_position`), `bwa_source_create_push` (was `_create_stream`), and the reverb-send
  family `bwa_set_reverb_gain` / `bwa_source_set_reverb` / `bwa_source_set_reverb_send` /
  `bwa_source_set_reverb_distance` (was `bwa_reflections_set_gain` / `bwa_source_set_reflections`
  / `..._reflection_send` / `..._reflection_distance`) — "reflections" now always means the Steam
  reflection-bed config or the image-source earlies, "reverb" the shared send/tap.
- New imports: `bwa_get_version` (the DLL's packed version — check it against the header rev at
  startup), `bwa_get_sample_rate` / `bwa_get_block_size` (resolved config — divide frames by THIS,
  not by what you put in the desc), `bwa_get_sink_type` (enum-typed backend readback; `BwaSinkType`
  gains `Manual`).
- A failed **explicit** `layoutPath` now fails `bwa_start` with `BwaResult.ErrLayout` instead of
  silently running the 26-grid default at the wrong channel count (`layoutPath = null` still
  means the default grid deliberately).

### Added — AV sync surface (scheduled play + playhead readback)

- **`Emitter.PlayAt(startSample)`** → the already-bound `bwa_source_play_at`: sample-accurate
  scheduled play on the engine's dsp clock (`Engine.DspTime`) — the `AudioSource.PlayScheduled`
  equivalent, previously reachable only through the raw `Bwa` layer. Keep the startSample you
  passed: `DspTime - startSample` is the sync clock for beat-cued visuals.
- **`Emitter.Position` / `PositionSeconds`** and **`AmbisonicBed.Position`** → new engine ABI
  `bwa_source_get_position` / `bwa_bed_get_position` (latest-wins per-voice playhead readback,
  like `is_playing`): the content playhead in engine-rate frames, correct where client-side
  `DspTime` arithmetic breaks — it freezes under pause, lands where a seek lands, follows pitch
  at the actual rate, and for streamed clips counts frames actually consumed (an underrun slips
  it, exactly like the audible clock). ~One audio block of lag; for tighter-than-a-block
  scheduling keep using `DspTime` arithmetic.
- **`Engine.DspTimeAt(realtime)` / `RealtimeAt(dspSample)`** → new engine ABI `bwa_get_clock`:
  the wall↔dsp bridge. The engine now publishes the device's own (sample position, host time)
  stamp from each audio callback — `ASIOTime`'s pair, previously captured at the sink and
  discarded — so mapping a `Time.realtimeSinceStartupAsDouble` moment to a dsp sample no longer
  carries a block of jitter: `emitter.PlayAt(engine.DspTimeAt(tEvent))` lands a sound on a visual
  event to well under a millisecond. The helpers maintain the epoch offset between the driver's
  clock and Unity's (decaying-max estimator, refreshed per frame from `LateUpdate`), self-correct
  ppm clock drift, and fall back to block-granular `DspTime` pairing when the backend has no host
  stamp. `Engine.GetClock` exposes the raw pair.
- **`Engine.OutputLatency`** → new engine ABI `bwa_get_output_latency`: the device's self-reported
  render→DAC latency in frames (`ASIOGetLatencies` — the Digiface includes its Dante buffering;
  0 on the null-sink fallback). A sound scheduled for dsp time T is *heard* at T + OutputLatency:
  the audio half of AV-latency alignment, so only the display delay is left to measure by hand.

### Added — multi-scene support

- The `Engine` (a `DontDestroyOnLoad` singleton = the physical CAVE, not a level) now **follows Unity's
  loaded scenes**: it subscribes to `SceneManager.sceneLoaded`/`sceneUnloaded` and re-bakes the static
  `AcousticGeometry` whenever scenes change (deferred one frame so additive loads coalesce into a single
  BVH rebuild; `sceneUnloaded` fires after teardown, so an unloaded scene's geometry drops naturally).
  Made possible by the runtime-safe geometry work — no engine rebuild, no audio gap. Additive scenes
  compose (a re-bake spans all loaded scenes); emitters were already per-scene via `OnEnable`/`OnDisable`.
- **Persistent material cache** (`Engine.ResolveMaterial`): material tokens are minted into a fixed
  64-slot engine table, so each `MaterialAsset`/preset is now minted **once** and reused across every
  scene load. Re-minting per load (the old per-`SetupScene` cache, and `AddDynamicMesh`) would exhaust
  the table in a multi-scene game — both now route through the shared cache.
- **`Engine.ReleaseMaterial`** → `bwa_material_release`: frees a material's table slot for reuse and
  evicts it from the cache (a later `ResolveMaterial` re-mints). Caller-managed — only release a
  material no live mesh/occluder references. The mint-once cache covers the common case; this is the
  escape hatch for apps that churn many *distinct* materials over a long session.
- Recommended pattern: put the `Engine` in a **persistent bootstrap scene**, load levels on top
  (single or additive). Sources, dynamic occluders, and static geometry all track the loaded scenes;
  the reflection-bed *config* (IR/order, room box) and the speaker layout stay engine-level (rebuild
  the engine only if those must change — rare for a fixed install).

### Added — dynamic (movable) acoustic geometry

- **`DynamicAcousticGeometry`** component + **`Engine.AddDynamicMesh` / `SetDynamicTransform` /
  `RemoveDynamicMesh`** → `bwa_scene_add_dynamic_mesh` & co.: mark a MOVING object (door, lift,
  rotating panel) as an occluder/reflector. It registers a low-poly acoustic mesh as a rigid
  instance (Steam Audio `IPLInstancedMesh`) and pushes its pose each frame (throttled by
  `positionEpsilon`/`angleEpsilon`), so occlusion and REAL-TIME reflections track it — moving it is a
  cheap scene-BVH refit, not a geometry rebuild. Same "keep it simple / use `meshOverride`" rules as
  `AcousticGeometry`; scale is captured at registration (rigid-body). Coordinate seam handled: the
  mesh bakes into room handedness once (X-flip + scale, winding reversed) and the per-frame pose goes
  through `Room.Pos`/`Room.Rot`. Baked reflections/pathing do NOT track movement (real-time does).
  Needs the Steam Audio backend (a no-op otherwise). Static geometry stays on `AcousticGeometry`,
  which is now also safe to re-push at runtime (a full scene rebuild — prefer dynamic meshes for
  movers).

### Added — parity with the engine's A/B round (max-rE · spectral spread · FuMa · bed orientation)

- **`Engine.maxRe` / `SetMaxRe`** → `bwa_set_max_re`: max-rE weighting on the bed decode and the
  FDN's render (live A/B, crossfaded, level-fair) — fewer decode sidelobes, better localization
  away from the sweet spot. Sits under the *Diffuse beds* header; off by default like the engine.
- **`BwaSpreadMode.Spectral`**: the third spread render — frequency-dependent panning (6 bands,
  each from its own direction inside the cone; width with no coherent copies to collapse or
  comb-filter — the decorrelation alternative). The existing `spreadMode` field/`SetSpreadMode`
  pass it through unchanged.
- **`Engine.LoadFuma`** + **`AmbisonicBed.fumaClip`** → `bwa_load_fuma`: legacy FuMa B-format
  clips (WXYZ order, MaxN, the W −3 dB) convert to AmbiX at load — downstream they are AmbiX
  assets, cached under a separate `fuma:` key so the same path can be loaded both ways.
- **`AmbisonicBed.pitchDegrees` / `rollDegrees`** (+ `PitchDegrees`/`RollDegrees` properties) →
  `bwa_bed_set_orientation`: level or tilt a capture, glided and click-free like yaw. Coordinate
  seam: yaw still converts through `Room.YawRad` (the X mirror reverses its sense), while pitch
  and roll pass through with the **same** sense — "front tilts up" never touches the mirrored
  axis, and Unity-right maps to room-right. All orientation paths (inspector, properties,
  enable) now go through one `ApplyOrientation()`.

**Breaking** — the native ABI was reshaped for consistency (nothing had shipped against it, so no
migration window): load-time configuration lives in config structs, live control lives in setters,
one door per knob.

- **The whole API is renamed**, sokol/miniaudio-style: the native prefix is `bwa_` (header
  `bw_audio.h`; `bw` stays free as the family namespace), C types are lowercase snake_case with
  `_desc` config structs (`bwa_desc`, `bwa_reflections_desc`, `bwa_fdn_desc`), constants are
  `BWA_*`, env vars are `BWA_*` (were `BWAUDIO_*`), and the native library is `bw_audio.dll`
  (was `bwaudio.dll`) — one product string everywhere.
- **The C# namespace is `BwAudio`** (was `CaveAudio`) **and the components lost their `Bw`
  prefix** — the namespace is the prefix now: `BwAudio.Engine` (the manager, previously the
  `BwAudio` class), `BwAudio.Emitter`, `AmbisonicBed`, `SpeakerView`, `AcousticGeometry`, `MaterialAsset`,
  `RoomConstraints`, and the `[Clip]` attribute. The raw P/Invoke layer keeps its C shape on
  purpose: `Bwa.bwa_*` with `Bwa*` mirror types (`BwaDesc`, `BwaPanner`, …). Scene/prefab
  references survive (every rename moved the `.meta` with its file, so GUIDs are unchanged);
  the package id is now `com.brainworks.bw_audio`.
- **`BwaDesc`** gained `enablePathing` (replaces the `BWAUDIO_PATHING` env var), `bedDecoder`
  (replaces the removed `bwa_set_bed_decoder`), and reserved fields so future growth won't break
  the ABI again.
- **`BwaReflectionsDesc.wetGain` is gone** — `bwa_reflections_set_gain` is the one wet-level
  control (live; a value pushed before `bwa_start` seeds whichever reverb bed starts). New `bake`
  field (replaces the `BWAUDIO_BAKE` env var).
- **The FDN setters** (`bwa_reverb_fdn`, `bwa_fdn_set_decay`, `bwa_fdn_set_decay_direction`)
  collapsed into one `bwa_fdn_config(in BwaFdnDesc)` — same shape as the reflection config.
- **`bwa_scene_set_mesh` (single-material) removed** — use `bwa_scene_set_mesh_mat` with one
  material token.
- **The `bwa_bed_*` facade is complete**: `bwa_bed_fade_to` / `fade_out` / `set_paused` / `seek` /
  `set_priority` / `set_group` / `is_playing` — a bed is a voice; bed code never needs the
  `bwa_source_*` prefix. Note a bed CAN be voice-stolen at default priority; protect a music bed
  with priority 255.
- **`Engine` inspector**: new `enablePathing` and `bakeReflections` toggles; `reverbGain` now
  rides the live setter (re-applied from `OnValidate` like the other live knobs).
- **Typed results**: the documented error codes are now a real enum — `bwa_result` in C,
  `BwaResult` here — and `bwa_start`/`bwa_stop`/`bwa_tracker_connect` return it.
- **The tracker is a runtime API, not env vars**: `bwa_tracker_connect(in BwaTrackerDesc)` /
  `bwa_tracker_disconnect` replace the `BWA_NATNET_*` environment variables AND the
  `BwaDesc.trackInternal` flag (connect/reconnect/disconnect any time, like every other NatNet
  client; the pose-source swap is glitch-free). `Engine` gained `natnetServer` / `natnetRigidBody`
  inspector fields and connects after start when Feed Listener is off.
- **Material presets are typed end to end**: `bwa_material_preset` takes `bwa_material_type`
  (mirrored by the existing `BwaMaterialPreset`) instead of a name string — the misspelled-name
  footgun is gone, and the C# `PresetName` shim with it. Custom materials are unchanged:
  `bwa_material_define` returns the same kind of `bwa_material` token.
- **Readback naming unified**: `bwa_get_channel_count`, `bwa_get_dsp_time`,
  `bwa_get_audio_backend` (were `bwa_channel_count`/`bwa_dsp_time`/`bwa_audio_backend`), and the
  test tone is a setter like its siblings: `bwa_set_test_signal` (was `bwa_test_signal`).
- **The last env vars moved into `BwaDesc`** — there are now NO environment variables:
  `sink` (`BwaSinkType`: Auto = try ASIO then fall back to the silent null sink; Asio = demand a
  device, fail loudly; Null = force offline — replaces `BWA_SINK`), `asioDriver` (replaces
  `BWA_ASIO_DRIVER`; empty = auto-pick by channel count), and `embree` (replaces `BWA_EMBREE`;
  silently falls back to the default ray tracer when the phonon build lacks Embree). `Engine`
  gained `sink` + `asioDriver` inspector fields.

## [0.2.0]

First **published** release: the package now ships as an installable UPM tarball with the native engine
inside it (`bw_audio.dll` + `phonon.dll`, import settings pre-configured), so a Unity project consumes it
without building any C++.

- **Renamed to `com.brainworks.bw_audio`** (was `com.cave.bw_audio`). Done before anything shipped, so
  there is nothing to migrate: the scope you add to `manifest.json` is now `com.brainworks`. The C#
  namespace is `BwAudio` — this is the package identity, not the code.
- **Distribution** — `tools/upm/pack.ps1` packs `com.brainworks.bw_audio-<version>.tgz` (uses `tar`, no
  Node anywhere; CI packs on *every* run, so a broken package fails the build rather than the release).
  A `v*` tag cuts a GitHub Release with that tarball attached, and **the Release is the distribution** —
  no registry, no token, nothing to keep in sync. Install it with Package Manager → `+` → *Install
  package from tarball…*. See the README.
- **`.meta` files are now committed** (`tools/upm/gen-meta.ps1`). An installed package is
  immutable, so assets arriving without a `.meta` get a fresh random GUID in every project: a scene
  referencing `Emitter` on one machine would deserialize as *"Missing (Mono Script)"* on another. The
  native plugins' import settings (Windows x64, Editor enabled) ship the same way — in an immutable
  package the user cannot fix them in the Inspector.
- **Version/tag guard** — the pack fails if the git tag and `package.json` disagree, so a tarball can't
  claim a version it isn't.

### Usability pass

The theme: the binding had several settings that failed *silently* — the engine survives them, logs
something, and carries on sounding subtly wrong. Those are now impossible to express, or caught in the
inspector where you can still see them.

- **Materials are a dropdown, not a string.** `BwaMaterialPreset` (mirrors the engine's table) replaces
  the free-text preset name on `Engine.roomMaterial` and `MaterialAsset.preset`. An unrecognised name
  was never an error — `bwa_material_preset` quietly returns material 0 (generic) and leaves the reason in
  `bwa_last_error` — so a typo'd `"concreet"` wall just *sounded* wrong. **Breaking:** `MaterialAsset`
  assets whose preset wasn't `concrete` will reset to Concrete; re-pick it from the dropdown.
- **The layout file says where it goes.** `layoutFile` uses the `[Clip(".json")]` picker (the same one
  audio clips use), so it lists the JSON files actually present under `StreamingAssets`, flags a missing
  one in red, and tooltips that the path is *relative to `Assets/StreamingAssets/`*. The inspector also
  shows the **absolute path the engine will look in** when the file isn't there — paired with the
  runtime error, both ends of that trap are now closed.
- **Inspector sliders work in Play mode.** `Emitter` gained the `OnValidate` re-push that `Engine` and
  `AmbisonicBed` already had. Dragging Gain/Pitch/Spread during Play used to change the field and
  nothing else (the value is only read at source creation), which reads as a broken slider.
- **Custom inspectors** for `Engine`, `Emitter` and `MaterialAsset`: settings that don't apply are
  hidden (FDN decay with the FDN off, pose prediction when Unity feeds the pose, custom coefficients
  under a preset material…), and the mistakes the engine merely *warns* about are surfaced as inspector
  warnings — a missing layout, both reverb beds fighting over the one tap, reflections with no geometry
  to reflect off. In Play mode `Engine` shows the live backend (flagging a **silent** fallback to the
  null sink), channel count, active voices and per-channel output meters; `Emitter` shows its
  ray-traced occlusion, which is otherwise invisible.
- **The room box is visible.** It draws as a wireframe gizmo (`Room.RoomToUnityMatrix` — the inverse of
  the coordinate seam, so a wrong `Room.UnityToRoom` makes the box land visibly in the wrong place).
- **The speaker array is visible.** `Engine` draws each speaker as a gizmo, labeled with its channel
  index. Stopped, the positions come from the layout **file**; in Play mode they come from the **engine**
  (`bwa_get_speakers`) and each one lights up with that channel's live output level, the same way the
  playground's gizmos do — so a dead or mis-wired speaker is visible at a glance. The Play-mode source
  matters: it's the geometry the engine is *actually* panning over, which means a failed layout load
  shows up as the built-in 26-grid sitting where your room isn't.
- **`SpeakerView` — live speaker activity you can see from inside the CAVE.** The gizmos above are an
  *editor* feature and don't render in a build, so this is the runtime counterpart: one unlit marker per
  channel, placed at the real speaker's position, brightening (and growing) with that channel's output.
  Unlit on purpose — a CAVE is dark, so the color computed is the color seen, with no lights to set up.
  Instant attack + slow release, because the engine reports a per-*block* peak that strobes too fast to
  read raw. Uses the same `bwa_get_speakers` + `bwa_get_bus_levels` readbacks as everything else, writes no
  audio state, and picks its shader across URP / HDRP / built-in. Optional: delete it and nothing changes
  audibly.
- **`RoomConstraints` — the physical room, in the scene view.** Reads the surveyed `constraints.json`
  (from StreamingAssets) and draws it: green = the speaker truss, red = the CAVE screen cube / observer
  keep-out, orange = the projectors. It's the **same file** `bwa_layout_tool` and `bwa_playground` read,
  so the room has one source of truth and doesn't get re-authored as Unity geometry that can drift from
  the survey. Purely a scene-view aid — no engine needed (it parses the JSON directly, so it works with
  the editor stopped) and nothing audible depends on it.
- **Minimum Unity is now 6000.0 (Unity 6)**, up from 2021.3. Nothing had ever been tested below it, and
  the old floor was already forcing compatibility shims.
- **No deprecation warnings.** The scene bake used `FindObjectsOfType`, deprecated in favor of
  `FindObjectsByType`, which forces you to say whether you need the results sorted. We don't — every
  geometry is baked into one mesh — so it passes `FindObjectsSortMode.None` and skips a pointless
  InstanceID sort.
- **Packing no longer dies on a locked DLL.** An open Unity Editor holds `bw_audio.dll` loaded out of
  `Runtime/Plugins/x86_64/`, so it can't be overwritten — `pack.ps1` now hashes first and skips the copy
  when the binary is already identical, and explains itself instead of throwing a raw IOException when
  it genuinely has to write. (A CMake rebuild hits the same lock; close the editor first.)
- **A tracked listener with nothing to track now warns** — `feedListener` on with no `listener` Transform
  silently left the listener parked at the array centroid, panning every source for a head that never
  moves.

## [0.1.0] — unreleased

Initial Unity binding (M7).

- Room convention change (engine-wide): room space is now **+Z forward** (identity head faces +z,
  matching Motive's default streamed frame). `Room`'s baseline handedness flip moved from the Z
  axis to the X axis; since Unity is also +Z-forward, identity rotations now map to identity.
- Room origin is now canonically **on the floor** (Motive ground plane; y = height above the
  floor), and the room box (`AddBox` / `bwa_scene_set_box`) is floor-based: x/z centered, y from 0
  up to the box height. The engine references the array centroid, not the origin, for its
  world-locked decodes, so surveys with other origins keep working.

- Pause/seek + the output protection limiter (engine `9d60c6e`): `Emitter.Pause()/UnPause()/Paused`
  (AudioSource.Pause/UnPause equivalents; click-free, the playhead freezes, paused still reads as
  IsPlaying) and `Emitter.Seek(samples)` (a timeSamples-set equivalent; in-memory clips only —
  streamed clips ignore it); `Engine.SetLimiter(bool)` / `SetLimiterCeiling(dB)` over the
  engine-default ON at -1 dBFS.

- `Bwa` — P/Invoke layer, 1:1 with the bw_audio C ABI (`include/bw_audio.h`): lifecycle, assets,
  sources, ambisonic beds, materials/occlusion, directivity, reflection bed, listener, commit.
  Verified against the real `bw_audio.dll` (struct layout, calling convention, string/array/bool
  marshalling).
- `Room` — the room-space (RH) ↔ Unity (LH) coordinate seam.
- `Engine` — scene manager singleton: owns the engine handle, loads assets, configures reflections +
  an optional room box at load time, and runs the centralized per-frame push (sources → listener →
  one commit).
- `Emitter` — positional source: transform-driven position/orientation, with `occlusion`,
  `reflections`, and `directivity` toggles, plus a one-shot helper.
- Editor guardrail (`ProjectCheck`): warns if Unity's built-in audio is still enabled (the
  engine owns the device) and offers one-click **Tools → Engine → Disable Unity Audio**.
- Acoustic-scene authoring: `MaterialAsset` (Create → Engine → Acoustic Material; preset or custom
  3-band) and `AcousticGeometry` (mark a mesh as occluding/reflecting, assign a material, scene-view
  gizmo). `Engine` bakes all geometry (+ the optional room box) world→room into one mesh at load.
- Audio-file authoring: `[Clip]` attribute + `ClipDrawer` — an editor picker that lists the
  `.wav`/`.flac`/`.mp3` files under StreamingAssets (with a browse button and a missing-file flag)
  instead of a hand-typed path. `Emitter` gains AudioSource-style `Play()`/`Stop()`/`Gain`. README
  has a "Replacing Unity audio" mapping table.
- `AmbisonicBed` — world-locked AmbiX soundfield component (wraps `bwa_bed_*`): play/stop/gain for
  diffuse ambience/music, decoded straight to all 26 speakers.
- Reverb wet level: `Engine.reverbGain` (inspector) + a live `ReverbGain` property, backed by the
  new engine config field `wet_gain` + `bwa_reflections_set_gain`.
- `Emitter.IsPlaying` + an `onFinished` UnityEvent, backed by a new engine ABI call
  `bwa_source_is_playing` (latest-wins per-source playback readback).

### Caught up to the engine ABI

The engine had grown 23 `BWA_API` calls the binding never got. `Bwa` is **1:1 with `bw_audio.h`** again
(verified by diffing the exported symbols), and the components expose the ones a scene actually
authors:

- **Mixing** — `Emitter.FadeTo()` / `FadeOut()` (the engine runs the fade on the audio thread; no
  coroutine, and `FadeOut` lands on the click-free stop path), `Emitter.Pitch` (glides — a change
  bends the pitch rather than stepping it), `Emitter.Priority` (voice-steal), mix groups
  (`Emitter.group` + `Engine.SetGroupGain` / `SetGroupPaused` — duck the SFX, keep the dialog),
  `Engine.MasterGain`, and `Engine.Paused` (global freeze; resume continues exactly).
- **Width** — `Emitter.spread` was bound but had no inspector field; `sizeMetres` (a physical
  radius: the source keeps its real-world size as the listener walks, where a fixed angular spread
  would not) is new, as are the engine-wide `spreadMode` (LOBE / MDAP), `decorrelation` (wide sources
  stop collapsing to phantom images as you walk), and `nearSpreadRadius`.
- **Propagation** — `Emitter.loudnessComp` (an LF shelf tracking the distance attenuation: far, not
  thin) joins `doppler` and `airAbsorption`.
- **Reverb without the SDK** — `Engine.enableFdnReverb` + the decay controls: a directional FDN bed
  that takes the reverb tap **instead of** the Steam bed (the manager warns if both are ticked) and is
  fed by the same per-emitter sends, so reverb works in a build with no phonon. Likewise
  `Emitter.SetOcclusionManual()` — game-driven occlusion (a door the gameplay knows about,
  underwater) through the sim's own ramped, band-tilted publish path, no SDK required.
- **Beds** — `AmbisonicBed.YawDegrees` (turn a recorded soundfield to line up with the scene) and
  `Engine.bedRenderer`: MATRIX, or **PARAMETRIC**, which re-pans the directional part of the field
  through the listener-relative panner — a recorded soundfield becomes *walkable*.
- **Listener** — `Engine.extraListeners` (up to 3 other occupants; panning becomes the energy mean of
  everyone's solve, instead of exact for one head and wrong for the rest), pushed in the same frame
  block as the primary pose since it is commit-gated the same way; and `posePredictionMs`, which leads
  the *tracked* pose by your measured motion-to-ears latency.
- **Diagnostics** — `Engine.ChannelCount` / `BusLevels()` / `SpeakerPositions()` / `ActiveVoices` /
  `TestSignal()` / `DspTime`.
- **Live A/B by ear** — `Engine.OnValidate` re-pushes every knob the engine makes atomic or
  crossfaded (panner, dual-band, spread mode, decorrelation, near-spread, bed renderer, tracked room
  EQ, master gain, limiter), so inspector tweaks are audible in Play mode instead of needing a restart.

Two coordinate seams the new calls exposed, both now in `Room` so nothing re-derives them:
`Room.YawRad` (the X mirror **reverses the sense of rotation** — a Unity euler angle passed straight
to the bed's yaw (`bwa_bed_set_orientation`) spins the soundfield the wrong way) and `Room.Dir` (a *direction*, e.g. the
FDN's decay axis, must not pick up the registration transform's translation the way `Room.Pos` does).

`Engine` now also **reports a failed layout load** (`bwa_last_error` right after `bwa_create`): it is
non-fatal — the engine falls back to its 26-speaker default grid — which on a smaller rig silently
changes the channel count and pans every source over geometry that isn't the one in the room.

### Hardened (adversarial review)

- `Engine` claims the `Instance` singleton only after a successful `bwa_start` (a failed init no longer
  leaves a dead, un-replaceable manager), and iterates a snapshot of the emitter list during the
  per-frame push (an `onFinished` handler that disables its emitter can no longer mutate the list
  mid-loop and drop the frame's commit).
- `Emitter` lazily creates its source (a coroutine retries until `Engine` is ready) instead of
  permanently disabling on an init-order race, and resets its play-edge state on disable (no spurious
  `onFinished` on re-enable).
- Mesh baking computes the winding flip from the full transform determinant, so a negative/mirrored
  object scale no longer bakes backward-facing reflectors. `Room.UnityToRoom` is documented as
  rigid-only.
- `Bwa.GetListenerPose` allocates correctly-sized arrays (the raw call writes fixed slots); the editor
  clip scan tolerates an inaccessible StreamingAssets subdir.
