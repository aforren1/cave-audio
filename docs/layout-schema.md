# Speaker layout file schema (`cave_layout.json`)

The layout file is the surveyed description of the physical array. You pass it as
`bwa_desc.layout_path` (see [`api.md`](./api.md)). `layout.c` loads it once at
`bwa_create`/`bwa_start` time, on the control thread. File I/O never touches the audio
thread. Three consumers read the result:

- **`dbap.c`**: speaker **positions** drive the listener-relative DBAP gain solve.
  The global `dbap` block supplies the two tuning knobs: `rolloff_r` blur and the
  distance-attenuation curve. See [`spatialization.md`](./spatialization.md).
- **the align stage** (`align_process`): per-speaker `gain_db` trim and `delay_ms`
  align the unequal speaker distances to a common reference (output stage, after the mix).
  The `binaural` profile skips it: its bus drives no physical speaker.
- **the binaural monitor**: the same positions become the virtual-speaker directions
  for the bus→ambisonics→binaural decode. In `cave_sim` and on the `cave_both` tap they
  also set each virtual speaker's distance gain and arrival time to the live listener
  (the array sim's room, [`spatialization.md`](./spatialization.md#the-array-sims-room)).

**The speaker count in this file IS the engine's channel count.** Any N in **4..64** works
(64 = `BWA_MAX_CHANNELS`, the compile-time capacity, which is the transport's bound). The count is
fixed for the engine's lifetime and readable back with `bwa_get_channel_count` /
`bwa_get_speakers`. The CAVE array starts with 24 and has room to grow. A collaborator rig of any size in that range loads its
own N-speaker file into the same binary.

A complete, valid example lives at [`../examples/cave_layout.json`](../examples/cave_layout.json):
a 3×3×3 boundary grid minus the center = exactly 26 speakers, floor-origin, y layers at
0 / 1.5 / 3 m, ears nominally at 1.5. That is the same shape as the built-in default grid
(`BWA_DEFAULT_GRID`, 26 speakers) the engine runs with no file.

A second valid file, [`../examples/dome_24.json`](../examples/dome_24.json), is the playgrounds'
default array: 24 speakers on a 2 m sphere around (0, 1.5, 0), spread evenly over the part above
the floor. `tools/layout/gen_dome.py` generates it; regenerate it rather than editing it. Its
`note` field says so, since JSON has no comments. It declares `listening_point_m: [0, 1.5, 0]`,
because the floor cut makes it top-heavy: its centroid sits at about y = 1.75 m.

## Authoring with `bwa_layout_tool`

You can author this file interactively with **`bwa_layout_tool`**
(`examples/layout_tool.cpp`, built with `-DBWA_BUILD_PLAYGROUND=ON`). Place each
speaker in 3D and **identify it by ear**. A speaker's `index` is its output channel,
so the tool drives that channel with the test signal (`bwa_set_test_signal`) out the cave
profile. You hear which physical speaker you're positioning.

Press **P** for a **DBAP preview**: a source pans through your in-progress layout, so
you can hear gaps/smoothness and walk the room to judge off-center coverage. The tool
rebuilds the engine with the edited positions, since the layout is load-time.

The tool exports this schema with `delay_ms` auto-derived from the positions
(max-distance alignment). A `directivity` block and any per-speaker `aim` in the loaded
file survive the export verbatim; the tool never edits them.

The tool is the **plan editor**. Open a file that a survey has already written (one that
carries `plan_position`) and it edits the plan instead, and saves the plan only. See
"Plan versus as-built" below.

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
   writes the as-built positions and keeps the plan beside them (see below).
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

### Scoring and the observer model

The tool can also *evaluate* and
*improve* a layout for a chosen panner. Press **X** (or run `--score <file>`) to print
each panner's [rE-localization error](./glossary.md#re-error): mean + worst over a direction shell ×
the working-volume listener grid, via `bwa_panner_gains_batch` (the same solve that ships). It is
azimuth-weighted, not a plain angle; see [perceptual weighting](./glossary.md#perceptual-weighting).

**Where the panner solves is not where you listen.** A fixed install solves once at the
sweet spot and never corrects for you walking away; a tracked one re-solves at your
position every block. Scoring only at the sweet spot cannot see that difference at all,
so the tool now evaluates every panner across the whole listener grid. The **solve at**
control picks which position it solves for: *auto* gives each panner its real behavior
(DBAP tracked, SPCAP and VBAP fixed), and forcing a mode A/Bs the contrast. Sources sit
at fixed world positions rather than following the listener, which is what makes an
off-center score comparable to a centered one.

Expect worst-case numbers to be higher than they used to be. Those are the off-center and
off-height cells that were previously never evaluated. **A layout optimized for SPCAP or
VBAP before this change was tuned against an objective that could not move, and is worth
re-running.**

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
That is measured, not taste. Against the acoustic measurement in
[validation.md](validation.md), rE direction error ranks DBAP cells well (Spearman 0.82)
and VBAP cells barely at all (0.19), where the Frank spread is the strong predictor
(0.77). One global default would be wrong for two of the three panners. Both are
overridable.

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
    { "min": [-2, 0, -2], "max": [2, 4, 2] }                    // e.g. the CAVE 4x4x4 interior
  ],
  "obstacles": [                                                // solid occluders: also block line-of-sight
    { "min": [-0.35, 3.9, -0.35], "max": [0.35, 4.3, 0.35] }    // e.g. a ceiling projector + its shadow
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

### Conditions and stages

What to optimize *for* is a named **condition**, not just slider positions. A
condition bundles the objective knobs (worst wt, focus wt, elev wt), a scoring-shell
elevation **band**, an **azi band** (an azimuth wedge about +z, the room's forward),
and a leash. Three ship: `3d` (the full sphere, the historical default), `horizontal`
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

A narrow condition is for expressing a requirement, not a shortcut to accuracy.
Measured on the default dome with DBAP: a wedge-only `visual` climb reached
4.3°/18.3° inside the wedge. A plain `3d` run scored the same wedge at
4.3°/15.6° while staying usable everywhere else (26.8° vs 84.1° full-sphere worst).
The roaming listener sets the wedge's floor, not how many speakers face
front, so aiming everything at it bought nothing. VBAP is the exception that proves
the A/B rule: its fixed solve does benefit from tighter frontal triangulation, so
`visual,3d` staged beat the direct run inside the wedge (3.4°/14.3° vs 3.9°/20.0°)
for about 3° of full-sphere worst. The wedge-only run still collapsed everywhere
else (74.5° worst). When a collaborator proposes a narrow objective, run both and
read the two `--score` columns before committing.

### Observer token, radial pass, and the guard

Both headless commands also take an observer token, `fixed` or `moving` (the
default). This CAVE's listener roams, so scores average a 27-point grid across the
working volume. A *seated* install listens from the sweet spot only, and scoring it
over the roam punishes cells it will never occupy. `--score layout.json fixed`
evaluates (and `--optimize ... fixed` optimizes) at the sweet spot alone.

The observer models do not transfer symmetrically, and the measured gap is large.
A sweet-spot-optimized layout scored under the roam came out WORSE than the
unoptimized dome (12.6°/82.3° vs 10.2°/75.2° for DBAP). At the sweet spot radius
and clustering cost nothing, so the fixed objective builds geometry that parallax
then punishes. The reverse is benign. A roam-optimized layout at the sweet spot
(2.6°/15.6° SPCAP) sits near the dedicated seated optimum (0.9°/3.9°) because the
sweet spot is one of the roam's own cells. Optimize `fixed` only for an install
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
scores next to the target's. Measured on the default dome, that recipe was the best
two-panner result of every mechanism tried: it matched the pure-VBAP mean (4.5°),
beat the pure-VBAP worst by 7° (28.9° vs 35.6°, the warm start plus the constraint
acting as a regularizer), and held DBAP within 0.3° of its own optimum.

The climb is greedy by default: it accepts only improving moves, and the step
shrinks when progress stalls. That is basin-limited, measurably (identical inputs
landed 26.8° and 31.2° worst on different random paths; stiff seeds barely improved
at all). Two tokens loosen it, and both always keep and ship the **best layout
seen**. `anneal` switches to Metropolis acceptance (uphill moves accepted with
probability exp(-slip/T), cooling every trial). `restarts=<n>` re-climbs n
times from the best layout plus a 0.25 m kick. Restarts explore near the seed's
basin; seed shape still dominates. Measured: on the default dome the upgrade is a
wash (4.8°/28.8° vs greedy's 26.8-31.2° spread, at 7x the iterations), but it
rescues a stiff seed outright. The clamped r=3.2 sphere that greedy left at 28.3°
worst reached 4.5°/24.1°, the best moving-listener result measured on this array.
VBAP saw no rescue anywhere (eight restarts across two very different seeds landed
within 2% of one cost: a flat-bottomed landscape). Use the upgrade when a seed
underperforms, not as the default. The GUI's `anneal` checkbox is the same switch,
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
cost at that weight. 0 keeps the historical point-source-only objective. The row
grades AllRAD with max-rE by default, which is what the engine now defaults to. An
install that ships EPAD passes `epad` (GUI: the bed decode combo and max-rE
checkbox) so the score matches the render, not a sibling of it. Pass `maxre` to
force the taper on when you have turned it off some other way.

Measured on the dome: point-source optimization already does most of the bed's
work for free (bed 23.9°/126.8° at the seed, 17.0°/54.0° after a plain DBAP
climb). A naive `bed=1` co-optimization trades badly (2° of bed mean for 17°
of DBAP worst). The recipe that works is the guard again: from the DBAP optimum,
`--optimize L.json dbap 3d bed=3 guard=dbap` reached the best bed mean measured
(14.1°) while DBAP actually improved to 4.8°/24.6°. The guard evaluates the
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
Measured, the shared layout costs the seated user about 1.5° mean against a
dedicated seated-only array, while a seated-only array is unusable for everyone
else. Verify any use case's slice of the same file with
`--score L.json [condition] [fixed|moving] [epad] [maxre]`.

### Leash and ear height

`--optimize` also takes `leash=<m>`. It caps each speaker's displacement from the
stage start and overrides the active condition's own leash. The room constraints
do not make it redundant. The feasible shell between the CAVE screens and the room
walls is thin radially but long tangentially. The leash is what stops a speaker
from sliding meters around the perimeter away from its surveyed, rigged position.
Measured with the real constraints file: a 0.75 m leash scored the same as a free
one (6.5°/42.5° vs 6.4°/41.1°) while the free run wandered speakers up to 1.4 m
further along the truss. The tight leash is free insurance for installability.
Every optimize start (GUI and headless) now also projects the incoming layout into
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

### Pins

When an allocation is a *requirement* ("26 speakers, spend 12 on the plane"), pin it,
don't weight it. Objectives compete: a `3d` stage happily pulls plane speakers back
toward elevation. **pin to plane** (per speaker, in the panel) confines that speaker
to a slab about the ear plane (**pin slab**, default ±0.3 m). The optimizer's trials
and **K** snap both project into it, so no later stage can undo the allocation.
Pinned speakers label cyan. Pins ride the layout file (`"pin": "plane"` per speaker,
`pin_slab_m` top-level) so the allocation travels into headless runs. The engine
ignores both fields.

What pinning costs, measured on the default 26-speaker dome (your numbers will
differ; the shape will not):

- **Without pins the allocation erodes.** Across a `horizontal,3d` staged run, DBAP's
  in-slab population went 10 speakers to 5; VBAP's went 15 to 7, and a direct `3d`
  run leaves 3. The plane preference effectively vanishes unless pinned.
- **With 12 pinned, the plane score lands at its ceiling.** Both panners matched or
  beat their plane-only run in-band (DBAP 3.8°/19.4° vs 3.9°/19.6°; VBAP 3.0°/16.3°
  with the best in-band spread, 18.0°), because the 3d stage keeps refining the free
  speakers while the pins hold the structure it would otherwise cannibalize.
- **The full-sphere price is small where it counts.** Mean stays within 0.2° of the
  unpinned result for both panners; the worst case gives up 3 to 5°.
- **More pins trade full-sphere mean for the plane.** Full-sphere mean climbs with
  pin count (DBAP 4.9° unpinned, 5.1° at 12 pins, 5.4° at 16; VBAP 4.6°, 4.6°, 5.1°)
  while the plane gain past 12 is small and panner-dependent (DBAP 3.8° to 3.5°,
  VBAP 3.0° to 3.5°, so VBAP regressed). These are single runs of a stochastic
  climb; read trends, not decimals. On this dome 12 pins buy the plane ceiling.
- **Judge VBAP's plane by spread, not direction error.** Against the acoustic
  measurement the Frank spread is VBAP's strong predictor (Spearman 0.77 vs 0.19 for
  direction error; see [validation.md](validation.md)).
- **Cross-compare with care.** `horizontal` carries a 1 m leash and `3d` a 3 m one,
  so a single-stage run and a staged run are not leash-matched. Comparisons within
  one condition are clean.

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

## Top-level structure

```jsonc
{
  "schema_version": 1,
  "units":            { "position": "meters", "gain": "decibels", "delay": "milliseconds" },
  "coordinate_space": "room, right-handed, +y up, +z forward (matches OptiTrack/Motive default); origin ON THE FLOOR at the working-area center (x/z); y = height above the floor",
  "listening_point_m": [0.0, 1.5, 0.0],     // optional; omit it and the engine uses the array centroid
  "reference": {
    "alignment":          "max-distance",   // how delay_ms was derived (documentation only)
    "ears_m":             1.4,              // the listening-point height it was derived AT
    "speed_of_sound_mps": 343.0,
    "note":               "..."
  },
  "dbap": {
    "rolloff_r": 0.5,                        // DBAP spatial-blur 'r' (how many speakers share energy); omitted -> derived from the geometry
    "distance_attenuation": {
      "model":               "inverse",      // documentation only - the loader ignores this field
      "reference_distance_m": 1.0,
      "rolloff":              1.0,
      "min_gain_db":         -40.0
    }
  },
  "speakers": [ /* 4..64 entries - the count IS the engine's channel count; see below */ ]
}
```

## Fields

| field | type | meaning |
|-------|------|---------|
| `schema_version` | int | reserved for breaking format changes. The loader currently ignores it - it is neither validated nor stored. |
| `units` | object | documentation only. The loader always converts dB → linear gain and ms → samples at the engine rate (positions are read as meters); changing this field has no effect. |
| `coordinate_space` | string | documentation of the frame; positions MUST match the `coordinate_space` value above (**room space**, floor origin). The engine works in it and the Unity/Unreal binding converts at its boundary; full seam: [`integration.md`](./integration.md) → "Coordinate seam". The engine's **nominal listening point** (world-locked bed/monitor decode directions + the default listener position) is `listening_point_m` when the file has it, else the array centroid, so the origin's exact spot is not load-bearing. |
| `listening_point_m` | `[x, y, z]` float (optional) | the nominal listening point, in room meters in the same frame as the speakers. Omit it and the engine uses the array centroid, which is right for a symmetric array. Set it for a top-heavy one: a floor-cut dome or a rig with more speakers overhead puts the centroid above your ears. It sets where a pose-less listener stands, where the world-locked decodes aim from, and the point the derived `rolloff_r` and SPCAP focus measure the array from. Exactly three finite numbers, each within ±1e6 m, or the file is rejected. `bwa_layout_tool` keeps it on save, with `y` following its ear-height slider. |
| `reference` | object | provenance for the alignment values. Informational - the engine applies `delay_ms` as written. |
| `reference.speed_of_sound_mps` | float (optional) | the room-temperature speed of sound the survey and the delay derivation assumed. `bwa_calibrate` records the value it used (`--temp` / `--c`) and reads it back on the next run, so a rig sets its temperature once; `bwa_layout_tool` reads it too, so both tools agree on one file. Range `306..380` m/s. Absent means the 20 C reference, 343.0. See [`calibration.md`](./calibration.md) -> "Air temperature". |
| `reference.ears_m` | float (optional) | authoring only, engine-ignored: the listening-point height (meters above the floor) that `delay_ms` was time-aligned at, and the anchor the optimizer scored against. `bwa_layout_tool` writes it from `ears=<m>` and reads it back on load, so a file reopens at its own anchor. Without it, reopening a 1.2 m layout and saving silently re-aligns every delay to the 1.4 m default. An explicit `ears=<m>` on the command line still wins (it parses after the load). |
| `dbap.rolloff_r` | float | the **blur** knob `r` from [`spatialization.md`](./spatialization.md): larger spreads energy over more speakers. Must be > 0; the loader floors it at 0.001 m (1 mm), below any audible blur. **Omit it and the loader derives it from the geometry**: `0.25 ×` the mean listening-point→speaker distance, the centroid unless `listening_point_m` is set (Sundstrom 2021 recommends 0.2–0.5 of it) - ~0.53 m on the default grid. An explicit value always wins; treat the derived one as the starting point to dial against the real array. |
| `dbap.distance_attenuation` | object | the source→listener distance-attenuation curve (the second tuning knob). The loader reads only `reference_distance_m` (> 0), `rolloff` (> 0), and `min_gain_db` (≤ 0; floors the attenuation). `model` is ignored - the inverse curve is the only one implemented. |
| `pin_slab_m` | float (optional) | authoring only, engine-ignored: half-height of the ear-plane slab that `"pin": "plane"` speakers are confined to. Written by `bwa_layout_tool` when any speaker is pinned. |
| `directivity` | object (optional) | one speaker model's off-axis loss table, written by `tools/directivity/clf_to_json.py` from the vendor's CLF simulation file. Read by `bwa_calibrate` (trims measured off a speaker's axis are re-aimed to the listening point), by the engine's tracked directivity compensation (`bwa_set_tracked_directivity`), and by the array-sim headphone monitor (`cave_sim` and the `cave_both` tap play each speaker's off-axis loss toward the listener, at the table's full resolution). Format below. |
| `speakers[]` | array | **4..64** speaker records (64 = the compile-time `BWA_MAX_CHANNELS` capacity). **The speaker count IS the engine's channel count** - a 24-speaker install loads a 24-entry file into the same binary. Order is not significant for DBAP, but `index` is the channel the speaker maps to on the bus / ASIO output, and the indices must form a complete `0..N-1` permutation. |

### Per-speaker record

| field | type | meaning |
|-------|------|---------|
| `index` | int `0..N-1` | bus/output channel for this speaker (N = the number of `speakers[]` records). Must be unique and cover `0..N-1` with no gaps - a complete permutation. |
| `position` | `[x, y, z]` float | the speaker's **acoustic center** in room space (meters, RH): not its front baffle and not its mounting point. See "Positions are acoustic centers" below. Each component must be finite and within ±1000 m. |
| `gain_db` | float | measured per-speaker level trim, applied in the align stage (`align_process`). `0.0` = no trim. Must be in **[-100, 24]**; anything outside rejects the file. |
| `delay_ms` | float | per-speaker delay to time-align arrival to the reference; converted to whole samples at `sample_rate` on load. `0.0` = the reference (farthest) speaker. A negative value clamps to `0`; anything **over 1000 ms rejects the file**. |
| `eq` | float array (optional) | minimum-phase correction-FIR taps (up to 512), written by `bwa_calibrate --eq` / `--room-eq`; applied per channel in the align stage before gain+delay. |
| `pin` | string (optional) | authoring only, engine-ignored: `"plane"` holds this speaker to the ear-plane slab (`pin_slab_m`) during optimization and snap. The allocation constraint from the authoring section above. |
| `room_eq` | object array (optional) | up to 8 LF modal-cut sections `{fc, gain_db, q}` (RBJ peaking, **cuts only**: `gain_db` in `[-24, 0]`, `fc` in `[10, 1000]`, `q` in `[0.25, 24]`), written by `bwa_calibrate --room-eq`. **Static-listener room correction** - see [`calibration.md`](./calibration.md); rendered as biquads at the engine rate. |
| `aim` | `[x, y, z]` float (optional) | the speaker's acoustic axis as a direction vector in room space (any length; normalized on load). Omit it and the speaker is aimed at the listening point, which is the CAVE's case. A zero or non-finite vector rejects the file. Only the `directivity` model reads it. |
| `plan_position` | `[x, y, z]` float (optional) | tools only, engine-ignored: where the installer planned this speaker's acoustic center. The first survey writes it (see "Plan versus as-built" below). Same checks as `position`: three finite numbers within ±1000 m. |
| `plan_aim` | `[x, y, z]` float (optional) | tools only, engine-ignored: the planned acoustic axis. Same checks as `aim`. Needs `plan_position` in the same record. Omit it and the plan points at the listening point from `plan_position`. |

### Positions are acoustic centers

Every position in this file is a speaker's acoustic center: `position`, and `plan_position` too.
The engine renders a speaker as a point source at `position`, and the point a box radiates from is
its acoustic center, somewhere behind the front baffle. So:

- Plan each box's acoustic center, not its baffle or its bracket.
- `bwa_calibrate --localize` measures acoustic centers, so its output already is one.
- `bwa_speaker_survey` sees markers ON the baffle. `--baffle-offset-m` moves each optical point back
  to the acoustic center along the box's aim, and on a layout `--localize` measured, the tool reads
  that depth itself ([calibration.md: Optical speaker check](./calibration.md#optical-speaker-check-bwa_speaker_survey)).

### Plan versus as-built

A rig layout describes two things that drift apart: where you planned each box, and where it
ended up. `position` and `aim` are the **as-built**: the engine renders from them, and the
measuring tools overwrite them. `plan_position` and `plan_aim` are the **plan**: the engine never
reads them, and only the tools use them.

The rule for a record:

- A record with `plan_position` has a plan: `plan_position`, plus `plan_aim`, or the direction to
  the listening point when `plan_aim` is absent.
- A record without `plan_position` is its own plan: its `position` and `aim`.

**Who writes the plan.** A measuring writer that overwrites `position` or `aim` first copies the
record's current `position` into `plan_position`, and its `aim` (when it has one) into
`plan_aim`, if the record has no `plan_position` yet. It never touches an existing one. So the
first survey records the plan and every later survey leaves it alone. The measuring writers are
`bwa_calibrate --localize`, the `bwa_calibrate --zylia` position survey and
`bwa_speaker_survey --write`. Each one prints how many plans it recorded.

**Who reads it.** Live aiming (`bwa_calibrate --live N --zylia` and `bwa_calib_view`'s Aim tab)
measures each box against its plan and says so in its header line. With no plan in the file, it
measures against `position` and `aim`, and says that instead.

**Editing the plan.** `bwa_layout_tool` decides by the file:

- No `plan_position` anywhere: a plan file. The tool edits `position` and `aim` and writes the
  whole file, as it always has.
- Any `plan_position`: an as-built file. The tool loads each speaker's plan as the thing you edit
  and draws the as-built positions as blue dots. Its save patches only `plan_position`,
  `plan_aim`, `pin`, `pin_slab_m` and `listening_point_m`, and writes `plan_position` on every
  speaker. The measured `position` and `aim`, the trims, `eq` and the grid pass through
  untouched. The speaker count cannot change in this mode. The preview (P) plays the plan.

**Which file you keep.** One. The file you save from the layout tool is the plan. After the first
survey writes into it, it holds both, so you never need a second copy to know what you intended.
### Tracked room EQ: top-level `room_eq_grid` (optional)

The moving-listener form of `room_eq`, written by `bwa_calibrate --room-eq-grid` (one
run per mic placement; see [`calibration.md`](./calibration.md)). The engine
interpolates the cut depths at the live listener position each block and glides the
align biquads toward them (`bwa_set_tracked_room_eq` is the live kill switch).

```jsonc
"room_eq_grid": [
  { "position": [-0.5, 1.2, 0.0],          // mic position, room meters
    "speakers": [                           // one entry per speaker (N), channel order
      [ {"fc": 44.6, "gain_db": -7.9, "q": 6.1} ],   // speaker 0's sections AT THIS POSITION
      [],                                            // speaker 1: no cuts
      // ... one array per remaining speaker
    ] },
  { "position": [0.5, 1.2, 0.0], "speakers": [ /* same ladder, this position's depths */ ] }
]
```

1–16 positions. Section ranges match `room_eq` (cuts only, `fc` `[10, 1000]`, `q`
`[0.25, 24]`; `gain_db 0` = this position doesn't need the cut). **Every position
must carry the same per-speaker `fc`/`q` ladder** (only the depths vary), because
the runtime interpolates depths by ladder index; the loader rejects a mismatch.
`room_eq` and `room_eq_grid` in one file are rejected too (one correction scheme at
a time). The calibration writeback maintains both invariants for you.

### Speaker directivity: top-level `directivity` (optional)

One speaker model's measured off-axis loss, written by `tools/directivity/clf_to_json.py`
from the vendor's CLF file (see [calibration.md](./calibration.md) -> "Speaker directivity
from the vendor's simulation file"). One model per layout, axisymmetric about each
speaker's `aim`.

```jsonc
"directivity": {
  "model":      "Genelec 4410A",
  "source":     "Genelec_Oy-4410A.CF2 (CLF v2 balloon ...)",   // provenance, engine-ignored
  "bands_hz":   [40, 50, 63, /* ... */ 16000],                  // 1..32 band centers, ascending
  "angles_deg": [0, 5, 10, /* ... */ 180],                      // 2..37, ascending from 0 (on axis), <= 180
  "split_hz":   1000,                                           // optional; the runtime's two-band split (default 1000)
  "loss_db":    [[0, -0.02, /* ... */], /* one row per band */],  // dB relative to on-axis, in [-80, 12]
  "on_axis_db": [56.05, 69.62, /* ... */ 77.16],                // optional; one per band, in [-200, 200]
  "planes":     { /* the two principal polars, same shape */ }  // informational, engine-ignored
}
```

At load the engine derives two curves from the table: the power mean of `loss_db` over the
bands below `split_hz` and over the bands at or above it. Those two are what the tracked
compensation applies, as a broadband gain and a high shelf per speaker. `bwa_calibrate`
reads the full table.

`on_axis_db` is the model's absolute on-axis response, one value per band. The Genelec CF2
states it in dB SPL, and the converter exports it when the file carries it. Only the
differences between bands are used: `bwa_calibrate --live N --zylia` and `calib_view`'s Aim
tab read its high-to-mid tilt as the tilt a speaker shows when it points straight at the mic
(see [calibration.md](./calibration.md) -> "Live aiming"). Leave it out and everything else
works; the live angle estimate then needs a reference speaker. The engine never reads it.

## Validation (loader contract)

`layout_load` rejects a malformed file (the reason surfaces through `bwa_last_error`)
if any of:

- `speakers.length` is outside `4..64` (`4..BWA_MAX_CHANNELS`);
- `index` values are not a permutation of `0..N-1`;
- a `position` component is missing, non-numeric, non-finite, or beyond ±1000 m;
- `gain_db` is outside `[-100, 24]`, or `delay_ms` exceeds 1000 ms (a negative
  `delay_ms` is not an error: it clamps to 0);
- a `room_eq` section is out of its documented range, or an `eq` (>512 taps) or
  `room_eq` (>8 sections) array is over its cap, or a tap is non-finite;
- a `room_eq_grid` is malformed: 0 or >16 positions, an entry without
  `position[3]` + one `speakers` array per speaker (N), a section out of range,
  positions disagreeing on a speaker's `fc`/`q` ladder, or the file carrying both
  `room_eq` and `room_eq_grid`;
- a speaker's `aim` is present but not three finite numbers, or is the zero vector;
- a speaker's `plan_position` is not three finite numbers within ±1000 m, its `plan_aim` is not
  three finite numbers or is the zero vector, or it has a `plan_aim` without a `plan_position`;
- a `directivity` block is malformed: not an object, `bands_hz` outside 1..32 entries or
  not ascending, `angles_deg` outside 2..37 entries, not starting at 0, not ascending, or
  past 180, a `loss_db` row count or length that does not match, an entry outside
  `[-80, 12]`, a `split_hz` outside `[20, 20000]`, or an `on_axis_db` that is present
  without one number per band or with an entry outside `[-200, 200]`.

`schema_version` is not checked: the loader never reads it.

**A present-but-invalid `bwa_desc.layout_path` does not fail `bwa_create`, but it does
fail `bwa_start`.** `bwa_create` records the reason in `bwa_last_error` and falls back to
the **default grid** (`BWA_DEFAULT_GRID`, 26 speakers), so a desk session can still inspect the engine, but
`bwa_start` then refuses with `BWA_ERR_LAYOUT`. A session that named a survey never runs
on the wrong geometry. Only `layout_path = NULL` runs the default grid for real. Check
`bwa_last_error` after `bwa_create` if you want the reason before you try to start. A
silently-defaulted layout pans the array with the wrong speaker positions, and (since the
count follows the layout) with a **different channel count**. A 20-speaker install whose
file fails to load comes up as a 26-channel engine. A NULL/empty
`layout_path` intentionally selects the default grid with no error.

On success, the loader holds the parsed geometry in the internal `Layout` struct (see
[`internal-types.md`](./internal-types.md)); the audio thread reads it but never
reloads it.

## Calibration writeback (unknown fields survive)

`bwa_calibrate` writes its results back into this file, and it does so
non-destructively. Every `calib_write_*` function (`src/calib/calib.c`) re-parses the
original JSON, mutates only its target fields (`gain_db`/`delay_ms` for the trims,
`eq`, `room_eq`, `room_eq_grid`, `position` for the survey), and re-serializes the
whole root. The position writer also records the plan once, by the rule above
(`layout_json_keep_plan` in `layout.c`, which `bwa_speaker_survey` calls too). Every writer
finds a speaker's record by its `index`, not its place in the array, so the records can sit in
any order.

Everything else in the file survives: unknown fields, per-speaker annotations, the
`reference` block, `note` strings. You can annotate a layout freely and recalibrate
without losing it. JSON has no comments, so keep annotations as extra string fields
(like `note`); those round-trip.
