# Authoring a speaker layout

This page covers `bwa_layout_tool`, the editor for the layout file. The file format itself is
the reference in [layout-schema.md](./layout-schema.md).

- **How-to** (first half): author a layout, declare the listening point, prepare a rig file,
  set placement constraints, run the optimizer, and pin an allocation.
- **Explanation** (second half): how the tool scores a layout, the observer model, conditions
  and stages, the guard, beds in the cost, the multi-consumer pipeline, and the leash.

## Authoring with `bwa_layout_tool`

You can author the layout file interactively with **`bwa_layout_tool`**
(`examples/layout_tool.cpp`, built with `-DBWA_BUILD_PLAYGROUND=ON`).

![bwa_layout_tool](./img/layout_tool.png)

Place each speaker in 3D and **identify it by ear**. The **count** control sets the array
size (4 to 64), and the file's count is the engine's channel count. A speaker's `index` is
its output channel, so the tool drives that channel with the test signal
(`bwa_set_test_signal`) out the cave profile. You hear which physical speaker you're
positioning.

Press **P** for a **DBAP preview**: a pink-noise source pans through your in-progress layout, so
you can hear gaps/smoothness and walk the room to judge off-center coverage. The tool
rebuilds the engine with the edited positions, since the layout is load-time.

The tool exports the [layout schema](./layout-schema.md) with `delay_ms` auto-derived from
the positions (max-distance alignment). A `directivity` block and any per-speaker `aim` in the loaded
file survive the export verbatim; the tool never edits them.

The tool is the **plan editor**. Open a file that a survey has already written (one that
carries `plan_position`) and it edits the plan instead, and saves the plan only. See
[Plan versus as-built](./layout-schema.md#plan-versus-as-built).

**The selected speaker's beam.** In edit mode the selected speaker shows its acoustic axis and,
with a `directivity` model in the file, two rings at the distance of the ears: the -3 dB
half-angle at 2 kHz (yellow) and at the top band (orange), so the ring's radius is the lateral
extent at the listener before that band drops 3 dB. Only the selected speaker, so the scene
stays readable. Without a model the ring is a 20 degree guide, which on the Genelec 4410A is
about the 2 dB region above 2 kHz. The HUD line under the speaker prints the two angles and the
loss at 20 degrees. The axis is the file's `aim` when the record has one, else the line to the
ears, which is what the engine's loader assumes too. A headless `bwa_layout_tool --export <file>`
writes/normalizes a layout without the GUI. To audition a saved layout in the full
binaural playground: `bwa_playground cave_layout.json`.

### Declaring the listening point

The engine's reference point is `listening_point_m` when the file declares one, else the array
centroid. That point is where a pose-less listener stands, what the world-locked decodes aim
from, and the point every speaker without an explicit `aim` is assumed to face. On an array with
more speakers overhead the centroid sits above your ears, so a rig layout should declare it.

A file that never declared one keeps it absent when the tool saves, on purpose: a symmetric
array wants the centroid. To add one, tick **declare listening point** under the ear-height
slider, or do it headless:

```
bwa_layout_tool --export cave_layout.json ears=1.448 listen
```

`ears=` sets the ear height (1.448 m is 4.75 ft), and `listen` declares
`listening_point_m` at that height, keeping the file's x and z when it had them, else 0. The same
height becomes `reference.ears_m`, which the tool's `delay_ms` derivation aligns to. With a
`directivity` model in the file and no listening point, the tool warns in the HUD and on export,
because that is the combination that aims every speaker at the wrong point.

### Preparing the rig layout

The order that gets a rig file right, each step writing into the same `cave_layout.json`
(every writer keeps the fields it does not own):

1. **Positions and the channel map**: `bwa_layout_tool` (Stage 1 of
   [hardware-validation.md](./hardware-validation.md)). What it saves is the plan. Then
   the acoustic survey, `bwa_calibrate --zylia` ([calibration.md](./calibration.md)),
   writes the as-built positions and keeps the plan beside them
   ([Plan versus as-built](./layout-schema.md#plan-versus-as-built)).
2. **The listening point**: `bwa_layout_tool --export cave_layout.json ears=1.448 listen`.
3. **The speaker model**: `uv run tools/directivity/clf_to_json.py Genelec_Oy-4410A.CF2 --into
   cave_layout.json`.
4. **Aims**: leave `aim` out for a speaker that points at the listening point; the engine
   assumes that. `bwa_calibrate --aim-sheet aims.csv` prints the bearing and down-tilt each
   mount needs and flags any speaker whose layout aim is over 20 degrees off. For the speakers
   the cameras can see, `bwa_speaker_survey --write` measures the real aim
   ([calibration.md](./calibration.md), "Optical speaker check").
5. **Trims**, then **`--verify`** from the same placement, with the ZM-1's center at the
   listening point.

### Constraints (`constraints.json`)

Drop a **`constraints.json`** next to the layout (see `examples/constraints.json`) to
declare where speakers may go:

- **`bounds`**: the allowed box; speakers must be inside.
- **`nogo`**: keep-out boxes (screens, structure, doorways, the CAVE interior);
  speakers must be outside. Drawn red-wire; snappable with **K**; the optimizer stays
  out of them.
- **`obstacles`**: SOLID occluders (projectors, beams). A speaker can't be inside one
  *nor in its acoustic shadow*: a box on the segment from the speaker to the ears
  blocks its sound (line-of-sight to the observer at ear height). Drawn filled-orange.
  Shadowed speakers are ringed orange, and the optimizer *penalizes* them (it can't
  push them out geometrically, so nudge them clear). A box only crudely bounds a
  projector's throw frustum; size it to the body plus the near shadow you care about.

```jsonc
{
  "bounds": { "min": [-4, 0, -4], "max": [4, 4.5, 4] },        // allowed box (room meters, floor y=0)
  "nogo": [                                                     // keep-out: speakers must stay OUTSIDE
    { "min": [-2, 0, -2], "max": [2, 4, 2] }                    // for example, the CAVE 4x4x4 interior
  ],
  "obstacles": [                                                // solid occluders: also block line-of-sight
    { "min": [-0.35, 3.9, -0.35], "max": [0.35, 4.3, 0.35] }    // for example, a ceiling projector + its shadow
  ]
}
```

### The optimizer

The tool flags violations, and **K** snaps speakers off bounds/no-go/obstacle bodies.
Press **B** to pick the target panner, then **O** to **auto-optimize**: a constrained
hill-climb that nudges positions to minimize that panner's rE error while staying
feasible. A **leash** slider caps how far a speaker may drift from where it started,
so the optimizer refines rather than relocates. It runs live (O again to stop, S to
save). Headless: `--optimize <file> [dbap|spcap|vbap] [stages]`.

### Pins

When an allocation is a *requirement* ("26 speakers, spend 12 on the plane"), pin it,
don't weight it. Objectives compete: a `3d` stage happily pulls plane speakers back
toward elevation. **pin to plane** (per speaker, in the panel) confines that speaker
to a slab about the ear plane (**pin slab**, default ±0.3 m). The optimizer's trials
and **K** snap both project into it, so no later stage can undo the allocation.
Pinned speakers label cyan. Pins ride the layout file (`"pin": "plane"` per speaker,
`pin_slab_m` top-level) so the allocation travels into headless runs. The engine
ignores both fields.

What pinning costs (your numbers will differ; the shape will not):

- **Without pins the allocation erodes.** A staged `horizontal,3d` run ends with about
  half the in-slab speakers it started with, and a direct `3d` run leaves fewer. The
  plane preference effectively vanishes unless you pin it.
- **With about half the speakers pinned, the plane score reaches its ceiling.** It
  matches a plane-only run, because the `3d` stage keeps refining the free speakers while
  the pins hold the structure it would otherwise cannibalize.
- **The full-sphere price is small where it counts.** The mean barely moves; the worst
  case gives up a few degrees.
- **Past that, more pins trade full-sphere mean for the plane**, and the extra plane gain
  is small and panner-dependent (VBAP can regress). The evidence is single runs of a
  stochastic climb, so read trends, not decimals.
- **Judge VBAP's plane by spread, not direction error.** Against the acoustic
  measurement, the [Frank spread](./glossary.md#frank-spread) predicts VBAP's
  localization and direction error does not (see [validation.md](validation.md)).
- **Cross-compare with care.** `horizontal` carries a 1 m leash and `3d` a 3 m one,
  so a single-stage run and a staged run are not leash-matched. Comparisons within
  one condition are clean.

## How scoring and the optimizer work

The sections below explain what the scores mean and why the optimizer's options exist.

### Scoring and the observer model

The tool can also *evaluate* and
*improve* a layout for a chosen panner. Press **X** (or run `--score <file>`) to print
each panner's [rE-localization error](./glossary.md#re-error): mean + worst over a direction shell ×
the working-volume listener grid, via `bwa_panner_gains_batch` (the same solve that ships). It is
azimuth-weighted, not a plain angle; see [perceptual weighting](./glossary.md#perceptual-weighting).

**Where the panner solves is not where you listen.** A fixed install solves once at the
sweet spot and never corrects for you walking away; a tracked one re-solves at your
position every block. Scoring only at the sweet spot cannot see that difference at all,
so the tool evaluates every panner across the whole listener grid. The **solve at**
control picks which position it solves for: *auto* gives each panner its real behavior
(DBAP tracked, SPCAP and VBAP fixed), and forcing a mode A/Bs the contrast. Sources sit
at fixed world positions rather than following the listener, which is what makes an
off-center score comparable to a centered one.

Worst-case numbers come mostly from the off-center and off-height cells. **Re-score any
layout that was optimized against the sweet spot alone.**

**Scoring SPCAP scores a tuning, not just a geometry.** SPCAP's lobe sharpness is a
runtime knob (`bwa_set_spcap_focus`), so the score has to know which value you mean. The
tool's **focus** and **density** sliders feed `bwa_panner_gains_batch` directly, and the
Score board, the rE overlay, the badness map, and the optimizer cost all follow them.
0 keeps the value the geometry derives, which is what the panel prints under the sliders.
Headless, `--score <file> focus=<n> density=<n>` sweeps the knob offline, so you can find
the lobe a layout likes before you dial it by ear. The knobs are inert for DBAP and VBAP:
those rows do not move whatever you pass.

Press **M** for the **badness map**: a grid of *listener* positions through the working
volume, each scored over a spread of directions, drawn as semitransparent voxels. The
coverage shell answers "which directions work from here"; this answers "where can somebody
stand". Good regions fade out, bad ones light up. Toggle **solve at** while watching it:
a fixed solve draws an island around the sweet spot, a tracked one stays flat.

The map's metric follows the panner, and so does the optimizer's **focus wt** default.
The acoustic measurement in [validation.md](validation.md) is why. For DBAP, rE direction
error predicts how well a cell localizes. For VBAP it barely does, and the
[Frank spread](./glossary.md#frank-spread) (the predicted image width) predicts it instead.
One global default would be wrong for two of the three panners. Both are overridable.

Scoring/coverage target the observer at **ear height** (the `obs ear y` slider, default
1.4 m; a real head, not the floor). The **C** coverage overlay shades a direction
shell to show where the array is weak. **G** switches its metric between the geometric
*nearest-speaker gap* and the selected panner's *per-direction rE error* (green =
accurate, red = mislocalized; hover a cube for its value). **V** toggles the observer
model (fixed center versus the moving working volume).

The listener grid spans ±0.45 m in **height**, a realistic seated-to-tall range rather
than a token nudge. Off-height is a distinct failure mode from off-center-in-plane. The
speakers sit mostly overhead, and the alignment delays follow one reference
height, so tracking re-aims the solve but fixes neither. A grid that barely varies height
cannot see the problem it most needs to.

### Conditions and stages

What to optimize *for* is a named **condition**, not just slider positions. A
condition bundles the objective knobs (worst wt, focus wt, elev wt), a scoring-shell
elevation **band**, an **azi band** (an azimuth wedge about +z, the room's forward),
and a leash. Three ship: `3d` (the full sphere, the default), `horizontal`
(only source directions within 15° of the ear plane count, for a collaborator who
values planar localization), and `visual` (azimuth and elevation within 30° of
straight ahead: spend the accuracy where the listener looks). The band is the honest
form of "2D". With the full sphere, zeroing the elevation weight still spends effort
on the azimuth of overhead sources, where azimuth is nearly meaningless. The `visual`
wedge anchors to one canonical facing, which fits an install with a dominant
screen direction and not a turn-anywhere CAVE. The Score board follows the active
condition; the coverage overlay stays full-sphere so the view never hides what a
condition ignores.

Conditions chain as **stages**, each seeding the next: `--optimize cave_layout.json
dbap horizontal,3d` climbs the plane objective to convergence, re-anchors the leash
there, then climbs the 3D objective from that result. Per stage it prints before/after
scores and how many speakers sit within 0.5 m of the ear plane. The plane stage pulls
speakers toward the ear plane (elevated coverage costs it nothing), and its leash is
the knob for how much of that migration you allow. In the GUI, stage by hand: optimize
under one condition, stop, switch, optimize again. Each start re-anchors the leash.
A warm start is not guaranteed to beat a direct 3D run (the climb is local), so A/B it
with `--score <file> [condition]`. The optional condition scores under that objective:
`--score out.json horizontal` answers "what did the 3D stage cost the plane".

A narrow condition expresses a requirement. It is not a shortcut to accuracy. For DBAP,
a plain `3d` run scores the `visual` wedge as well as a wedge-only climb does, and stays
usable everywhere else. The roaming listener sets the wedge's floor, not how many speakers
face front, so aiming everything at the wedge buys nothing. VBAP is the exception: its
fixed solve gains from tighter frontal triangulation, so a staged `visual,3d` run beats a
direct `3d` run inside the wedge, for a few degrees of full-sphere worst case. A wedge-only
run collapses outside the wedge for both panners. When a collaborator proposes a narrow
objective, run both and read the two `--score` columns before you commit.

### Observer token, radial pass, and the guard

Both headless commands also take an observer token, `fixed` or `moving` (the
default). This CAVE's listener roams, so scores average a 27-point grid across the
working volume. A *seated* install listens from the sweet spot only, and scoring it
over the roam punishes cells it will never occupy. `--score layout.json fixed`
evaluates (and `--optimize ... fixed` optimizes) at the sweet spot alone.

The observer models do not transfer symmetrically, and the gap is large. A
sweet-spot-optimized layout can score WORSE under the roam than the unoptimized seed did.
At the sweet spot, radius and clustering cost nothing, so the fixed objective builds
geometry that parallax then punishes. The reverse is benign. A roam-optimized layout
scores near the dedicated seated optimum at the sweet spot, because the sweet spot is one
of the roam's own cells. Optimize `fixed` only for an install
that is genuinely and permanently seated; never ship its result to a roaming one.

`--optimize` also takes `radial`: trials move speakers only along the ray from the
ears, so directions stay put and radii refit. This is the cross-panner pass. VBAP's
asset is its direction structure (triangulation) and DBAP's sensitivity is distance,
so `--optimize L.json vbap 3d` followed by `--optimize L.json dbap 3d radial` tunes
the array for both. The second command loads the first's result (the file carries
the state between invocations) and cannot disturb what the first one built.

For a hard two-panner requirement, `guard=<panner>[:tol]` is the stronger tool.
While climbing the target panner, the optimizer rejects any move that lets the guard
panner's cost slip more than `tol` (default 0.5, cost units are roughly degrees) above
its stage-start value. You cannot climb two objectives at once, but you can climb one
inside the other's feasible set. The recipe for "the best VBAP layout that never
lets DBAP slip": optimize DBAP first, then `--optimize L.json vbap 3d guard=dbap`.
The guard baseline is wherever the stage starts, so guarding from an unoptimized
layout protects very little. The stage report prints the guard's before/after
scores next to the target's. This recipe is the best two-panner mechanism the tool has.
On the default dome it matches a pure-VBAP run's mean, beats its worst case (the warm
start plus the constraint act as a regularizer), and holds DBAP close to its own optimum.

The climb is greedy by default: it accepts only improving moves, and the step
shrinks when progress stalls. That makes it local. Identical inputs land in different
places on different random paths, and a stiff seed may barely improve. Two tokens
loosen it, and both always keep and ship the **best layout seen**. `anneal` also
accepts some moves that make the cost worse (simulated annealing: a move that raises
the cost by `slip` passes with probability exp(-slip/T), and T cools every trial).
`restarts=<n>` re-climbs n times from the best layout plus a 0.25 m kick. Restarts
explore near the seed's basin; seed shape still dominates. On a good seed the upgrade
costs several times the iterations for no real gain, but it can rescue a stiff seed
outright. VBAP's cost landscape is flat-bottomed, so it gains nothing. Use the upgrade
when a seed underperforms, not as the default. The GUI's `anneal` checkbox is the same switch,
and stopping the optimizer (or entering preview) always restores the best layout.

Every climb terminates on its own: the step floor (0.02 m) or a 120k-iteration
cap, in the GUI and headless alike. Pressing **O** again re-climbs with a fresh
step schedule.

### Beds in the cost

Point sources are not the only consumer of the array. Ambisonic **beds** decode
SH to the speakers through a layout-fixed AllRAD matrix. What a bed wants is a
good *quadrature* of the sphere (uniformity), which the panner scores cannot see.
The Score board and `--score` carry an **AMBI (AllRAD)** row: plane waves at
infinity through the engine's real decode build (`bwa_bed_gains_batch`), evaluated
over the same shell, condition, and observer model. To *optimize* for it too, pass
`bed=<wt>` (or the GUI's `bed wt` slider): the bed's mean/worst blend joins the
cost at that weight. 0, the default, scores point sources only. The row grades AllRAD
with max-rE by default, which matches the engine's default. An
install that ships EPAD passes `epad` (GUI: the bed decode combo and max-rE
checkbox) so the score matches the render, not a sibling of it. Pass `maxre` to
force the taper on when you have turned it off some other way.

Point-source optimization already does most of the bed's work for free. A naive
`bed=1` co-optimization trades badly: a little bed accuracy for a lot of DBAP worst
case. The recipe that works is the guard again: from the DBAP optimum,
`--optimize L.json dbap 3d bed=3 guard=dbap` gives the best bed score without letting
DBAP slip. The guard evaluates the
panner alone (the bed term is excluded from it by design), so "best bed, don't
let DBAP slip" means exactly that.

### The multi-consumer pipeline

Put the evidence together and the full multi-consumer pipeline is one command per
consumer. Each stage climbs its own objective inside the previous winners'
feasible set (the file carries the state):

```
bwa_layout_tool --optimize L.json dbap 3d                          # 1. the production panner
bwa_layout_tool --optimize L.json vbap 3d guard=dbap               # 2. best VBAP that keeps DBAP
bwa_layout_tool --optimize L.json vbap 3d bed=3 guard=dbap maxre   # 3. the bed, VBAP in the cost
bwa_layout_tool --score L.json maxre                               # verify all four rows
```

Compose with the rest as the install demands: pins plus `horizontal,3d` stages in
step 1 for a planar requirement, `constraints.json` in the working directory for
the real room, `ears=<m>` for the install's height, `epad` if it ships EPAD,
`anneal restarts=<n>` on a stage whose seed underperforms. The guard is single,
so the production panner holds it throughout. The middle consumer stays in the
later stages' objective, which protects it softly.

This pipeline's output is the SHARED-array answer: one surveyed layout, and each
use case picks its renderer on top (tracked DBAP for a roaming session, SPCAP or
VBAP fixed-solve for a seated one, the bed renderer for ambisonic content).
The shared layout costs a seated user little against a dedicated seated-only
array, while a seated-only array is unusable for everyone else. Verify any use case's slice of the same file with
`--score L.json [condition] [fixed|moving] [epad] [maxre]`.

### Leash and ear height

`--optimize` also takes `leash=<m>`. It caps each speaker's displacement from the
stage start and overrides the active condition's own leash. The room constraints
do not make it redundant. The feasible shell between the CAVE screens and the room
walls is thin radially but long tangentially. The leash is what stops a speaker
from sliding meters around the perimeter away from its surveyed, rigged position.
With the real constraints file, a 0.75 m leash scores about the same as a free run,
and the free run wanders speakers more than a meter along the truss. A tight leash is
free insurance for installability. Every optimize start (GUI and headless) also
projects the incoming layout into
the constraints and pin slabs, the same projection every trial gets, so a generated
or hand-edited file cannot smuggle an infeasible position through a run.

Both also take `ears=<m>`, the listener ear height above the floor (default 1.4;
the GUI's `obs ear y` slider). Everything plane-shaped anchors to it: the
horizontal band, the pin slab, the scoring shell, and the delay-alignment point
written on save. A seated install at 1.2 m ears that optimizes without this flag
gets a plane 20 cm too high, silently. The value rides the file as
`reference.ears_m` and is read back on load, so you pass the flag once and the
file remembers. Without that, every reopen would silently re-align the delays to
the 1.4 m default on save. An explicit `ears=<m>` still overrides the file.
