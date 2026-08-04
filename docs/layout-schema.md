# Speaker layout file schema (`cave_layout.json`)

The layout file is the surveyed description of the physical array. You pass it as
`bwa_desc.layout_path` (see [`api.md`](./api.md)). `layout.c` loads it once at
`bwa_create`/`bwa_start` time, on the control thread; file I/O never touches the audio
thread. Three consumers read the result:

- **`dbap.c`**: speaker **positions** drive the listener-relative DBAP gain solve.
  The global `dbap` block supplies the two tuning knobs: `rolloff_r` blur and the
  distance-attenuation curve. See [`spatialization.md`](./spatialization.md).
- **the align stage** (`align_process`): per-speaker `gain_db` trim and `delay_ms`
  align the unequal speaker distances to a common reference (output stage, after the mix).
- **the binaural monitor**: the same positions become the virtual-speaker directions
  for the bus→ambisonics→binaural decode.

**The speaker count in this file IS the engine's channel count.** Any N in **4..26** works
(26 = `BWA_CHANNELS`, the compile-time capacity), fixed for the engine's lifetime and readable
back with `bwa_get_channel_count` / `bwa_get_speakers`. The CAVE array is 26; a smaller collaborator
rig loads its own N-speaker file into the same binary.

A complete, valid example lives at [`../examples/cave_layout.json`](../examples/cave_layout.json):
a 3×3×3 boundary grid minus the center = exactly 26 speakers, floor-origin, y layers at
0 / 1.5 / 3 m, ears nominally at 1.5.

## Authoring with `bwa_layout_tool`

You can author this file interactively with **`bwa_layout_tool`**
(`examples/layout_tool.cpp`, built with `-DBWA_BUILD_PLAYGROUND=ON`). Place each
speaker in 3D and **identify it by ear**: a speaker's `index` is its output channel,
so the tool drives that channel with the test signal (`bwa_set_test_signal`) out the cave
profile: you hear which physical speaker you're positioning.

Press **P** for a **DBAP preview**: a source pans through your in-progress layout, so
you can hear gaps/smoothness and walk the room to judge off-center coverage. The tool
rebuilds the engine with the edited positions, since the layout is load-time.

The tool exports this schema with `delay_ms` auto-derived from the positions
(max-distance alignment). A headless `bwa_layout_tool --export <file>`
writes/normalizes a layout without the GUI. To audition a saved layout in the full
binaural playground: `bwa_playground cave_layout.json`.

**Scoring, constraints, and auto-optimization.** The tool can also *evaluate* and
*improve* a layout for a chosen panner. Press **X** (or run `--score <file>`) to print
each panner's rE-localization error: mean + worst over a direction shell × the
working-volume listener grid, via `bwa_panner_gains_batch` (the same solve that ships).

**Where the panner solves is not where you listen.** A fixed install solves once at the
sweet spot and never corrects for you walking away; a tracked one re-solves at your
position every block. Scoring only at the sweet spot cannot see that difference at all,
so every panner is now evaluated across the whole listener grid, and the **solve at**
control picks which position it solves for: *auto* gives each panner its real behavior
(DBAP tracked, SPCAP and VBAP fixed), and forcing a mode A/Bs the contrast. Sources sit
at fixed world positions rather than following the listener, which is what makes an
off-center score comparable to a centered one.

Expect worst-case numbers to be higher than they used to be. Those are the off-center and
off-height cells that were previously never evaluated. **A layout optimized for SPCAP or
VBAP before this change was tuned against an objective that could not move, and is worth
re-running.**

Press **M** for the **badness map**: a grid of *listener* positions through the working
volume, each scored over a spread of directions, drawn as semitransparent voxels. The
coverage shell answers "which directions work from here"; this answers "where can somebody
stand". Good regions fade out, bad ones light up. Toggle **solve at** while watching it:
a fixed solve draws an island around the sweet spot, a tracked one stays flat.

The map's metric follows the panner, and so does the optimizer's **focus wt** default.
That is measured, not taste: against the acoustic measurement in
[validation.md](validation.md), rE direction error ranks DBAP cells well (Spearman 0.82)
and VBAP cells barely at all (0.19), where the Frank spread is the strong predictor
(0.77). One global default would be wrong for two of the three panners. Both are
overridable.

Drop a **`constraints.json`** next to the layout (see `examples/constraints.json`) to
declare where speakers may go:

- **`bounds`**: the allowed box; speakers must be inside.
- **`nogo`**: keep-out boxes (screens, structure, doorways, the CAVE interior);
  speakers must be outside. Drawn red-wire; snappable with **K**; the optimizer stays
  out of them.
- **`obstacles`**: SOLID occluders (projectors, beams). A speaker can't be inside one
  *nor in its acoustic shadow*: a box on the segment from the speaker to the ears
  blocks its sound (line-of-sight to the observer at ear height). Drawn filled-orange.
  Shadowed speakers are ringed orange, and the optimizer *penalises* them (it can't
  push them out geometrically, so nudge them clear). A box only crudely bounds a
  projector's throw frustum; size it to the body plus the near shadow you care about.

The tool flags violations, and **K** snaps speakers off bounds/no-go/obstacle bodies.
Press **B** to pick the target panner, then **O** to **auto-optimize**: a constrained
hill-climb that nudges positions to minimize that panner's rE error while staying
feasible. A **leash** slider caps how far a speaker may drift from where it started,
so the optimizer refines rather than relocates. It runs live (O again to stop, S to
save). Headless: `--optimize <file> [dbap|spcap|vbap] [stages]`.

What to optimize *for* is a named **condition**, not just slider positions. A
condition bundles the objective knobs (worst wt, focus wt, elev wt), a scoring-shell
elevation **band**, an **azi band** (an azimuth wedge about +z, the room's forward),
and a leash. Three ship: `3d` (the full sphere, the historical default), `horizontal`
(only source directions within 15° of the ear plane count, for a collaborator who
values planar localization), and `visual` (azimuth and elevation within 30° of
straight ahead: spend the accuracy where the listener looks). The band is the honest
form of "2D": with the full sphere, zeroing the elevation weight still spends effort
on the azimuth of overhead sources, where azimuth is nearly meaningless. The `visual`
wedge is anchored to one canonical facing, which fits an install with a dominant
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
4.3°/18.3° inside the wedge, and a plain `3d` run scored the same wedge at
4.3°/15.6° while staying usable everywhere else (26.8° vs 84.1° full-sphere worst).
The wedge's floor is set by the roaming listener, not by how many speakers face
front, so aiming everything at it bought nothing. VBAP is the exception that proves
the A/B rule: its fixed solve does benefit from tighter frontal triangulation, so
`visual,3d` staged beat the direct run inside the wedge (3.4°/14.3° vs 3.9°/20.0°)
for about 3° of full-sphere worst; the wedge-only run still collapsed everywhere
else (74.5° worst). When a collaborator proposes a narrow objective, run both and
read the two `--score` columns before committing.

Both headless commands also take an observer token, `fixed` or `moving` (the
default). This CAVE's listener roams, so scores average a 27-point grid across the
working volume; a *seated* install listens from the sweet spot only, and scoring it
over the roam punishes cells it will never occupy. `--score layout.json fixed`
evaluates (and `--optimize ... fixed` optimizes) at the sweet spot alone.

The observer models do not transfer symmetrically, and the measured gap is large.
A sweet-spot-optimized layout scored under the roam came out WORSE than the
unoptimized dome (12.6°/82.3° vs 10.2°/75.2° for DBAP): at the sweet spot radius
and clustering cost nothing, so the fixed objective builds geometry that parallax
then punishes. The reverse is benign; a roam-optimized layout at the sweet spot
(2.6°/15.6° SPCAP) sits near the dedicated seated optimum (0.9°/3.9°) because the
sweet spot is one of the roam's own cells. Optimize `fixed` only for an install
that is genuinely and permanently seated; never ship its result to a roaming one.

`--optimize` also takes `radial`: trials move speakers only along the ray from the
ears, so directions stay put and radii refit. This is the cross-panner pass. VBAP's
asset is its direction structure (triangulation) and DBAP's sensitivity is distance,
so `--optimize L.json vbap 3d` followed by `--optimize L.json dbap 3d radial` tunes
the array for both: the second command loads the first's result (the file carries
the state between invocations) and cannot disturb what the first one built.

For a hard two-panner requirement, `guard=<panner>[:tol]` is the stronger tool:
while climbing the target panner, any move that lets the guard panner's cost slip
more than `tol` (default 0.5, cost units are roughly degrees) above its stage-start
value is rejected. Two objectives cannot both be climbed, but one can be climbed
inside the other's feasible set. The recipe for "the best VBAP layout that never
lets DBAP slip": optimize DBAP first, then `--optimize L.json vbap 3d guard=dbap`;
the guard baseline is wherever the stage starts, so guarding from an unoptimized
layout protects very little. The stage report prints the guard's before/after
scores next to the target's. Measured on the default dome, that recipe was the best
two-panner result of every mechanism tried: it matched the pure-VBAP mean (4.5°),
beat the pure-VBAP worst by 7° (28.9° vs 35.6°, the warm start plus the constraint
acting as a regularizer), and held DBAP within 0.3° of its own optimum.

The climb is greedy by default: only improving moves are accepted, and the step
shrinks when progress stalls. That is basin-limited, and measurably so (identical
inputs landed 26.8° and 31.2° worst on different random paths; stiff seeds barely
improved at all). Two tokens loosen it, and both always keep and ship the **best
layout seen**, so a run can never end worse than its best moment: `anneal` switches
to Metropolis acceptance (uphill moves accepted with probability exp(-slip/T), the
temperature cooling every trial), and `restarts=<n>` re-climbs n times, each
restart hopping from the best layout plus a 0.25 m kick. Restarts explore near the
seed's basin; they do not invent a new structure, so seed shape still dominates.
Measured: on the default dome the upgrade is a wash (4.8°/28.8° vs greedy's
26.8-31.2° spread, at 7x the iterations; the basin's floor is real), but it rescues
a stiff seed outright: the clamped r=3.2 sphere that greedy left at 28.3° worst
reached 4.5°/24.1°, the best moving-listener result measured on this array. VBAP
saw no rescue anywhere: eight restarts across two very different seeds all landed
within 2% of one cost, so its landscape is flat-bottomed and the extra wandering
only added worst-case noise. Use the upgrade when a seed underperforms, not as the
default.
The GUI's `anneal` checkbox is the same switch, and stopping the optimizer (or
entering preview) always restores the best layout.

Every climb terminates on its own. Headless runs stop at the step floor (0.02 m)
or a 120k-iteration cap, and the GUI climb now stops itself at the same floor,
restores the best layout, and says so in the HUD; pressing **O** again re-climbs
from there with a fresh step schedule.

Point sources are not the only consumer of the array: ambisonic **beds** decode
SH to the speakers through a layout-fixed AllRAD matrix, and what a bed wants is a
good *quadrature* of the sphere (uniformity), which the panner scores cannot see.
The scoreboard and `--score` carry an **AMBI (AllRAD)** row: plane waves at
infinity through the engine's real decode build (`bwa_bed_gains_batch`), evaluated
over the same shell, condition, and observer model. To *optimize* for it too, pass
`bed=<wt>` (or the GUI's `bed wt` slider): the bed's mean/worst blend joins the
cost at that weight. 0 keeps the historical point-source-only objective. The row
grades AllRAD without max-rE by default (the engine defaults); an install that
ships EPAD or `bwa_set_max_re` passes `epad` / `maxre` (GUI: the bed decode combo
and max-rE checkbox) so the score matches the render, not a sibling of it.

Measured on the dome: point-source optimization already does most of the bed's
work for free (bed 23.9°/126.8° at the seed, 17.0°/54.0° after a plain DBAP
climb), and a naive `bed=1` co-optimization trades badly (2° of bed mean for 17°
of DBAP worst). The recipe that works is the guard again: from the DBAP optimum,
`--optimize L.json dbap 3d bed=3 guard=dbap` reached the best bed mean measured
(14.1°) while DBAP actually improved to 4.8°/24.6°. The guard evaluates the
panner alone (the bed term is excluded from it by design), so "best bed, don't
let DBAP slip" means exactly that.

Putting the whole session of evidence together, the full multi-consumer pipeline
is one command per consumer, each stage climbing its own objective inside the
previous winners' feasible set (the file carries the state):

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
so the production panner holds it throughout; the middle consumer is protected
softly by staying in the later stages' objective.

This pipeline's output is the SHARED-array answer: one surveyed layout, and each
use case picks its renderer on top (tracked DBAP for a roaming session, SPCAP or
VBAP fixed-solve for a seated one, the bed renderer for ambisonic content).
Measured, the shared layout costs the seated user about 1.5° mean against a
dedicated seated-only array, while a seated-only array is unusable for everyone
else. Verify any use case's slice of the same file with
`--score L.json [condition] [fixed|moving] [epad] [maxre]`.

`--optimize` also takes `leash=<m>`, capping each speaker's displacement from the
stage start and overriding the active condition's own leash. The room constraints
do not make it redundant: the feasible shell between the CAVE screens and the room
walls is thin radially but long tangentially, and the leash is what stops a speaker
from sliding meters around the perimeter away from its surveyed, rigged position.
Measured with the real constraints file: a 0.75 m leash scored the same as a free
one (6.5°/42.5° vs 6.4°/41.1°) while the free run wandered speakers up to 1.4 m
further along the truss. The tight leash is free insurance for installability.
Every optimize start (GUI and headless) now also projects the incoming layout into
the constraints and pin slabs, the same projection every trial gets, so a generated
or hand-edited file cannot smuggle an infeasible position through a run.

Both also take `ears=<m>`, the listener ear height above the floor (default 1.4).
Everything plane-shaped anchors to it: the horizontal band, the pin slab, the
scoring shell, and the delay-alignment point written on save. A seated install at
1.2 m ears that optimizes without this flag gets a plane 20 cm too high, silently.
The GUI's `obs ear y` slider is the same knob.

When an allocation is a *requirement* ("26 speakers, spend 12 on the plane"), pin it,
don't weight it. Objectives compete: a `3d` stage happily pulls plane speakers back
toward elevation. **pin to plane** (per speaker, in the panel) confines that speaker
to a slab about the ear plane (**pin slab**, default ±0.3 m); the optimizer's trials
and **K** snap both project into it, so no later stage can undo the allocation.
Pinned speakers label cyan. Pins ride the layout file (`"pin": "plane"` per speaker,
`pin_slab_m` top-level) so the allocation travels into headless runs; the engine
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

Scoring/coverage target the observer at **ear height** (the `obs y` slider, default
1.4 m; a real head, not the floor). The **C** coverage overlay shades a direction
shell to show where the array is weak. **G** switches its metric between the geometric
*nearest-speaker gap* and the selected panner's *per-direction rE error* (green =
accurate, red = mislocalised; hover a cube for its value). **V** toggles the observer
model (fixed center versus the moving working volume).

The listener grid spans ±0.45 m in **height**, a realistic seated-to-tall range rather
than a token nudge. Off-height is a distinct failure mode from off-center-in-plane: the
speakers sit mostly overhead and the alignment delays are computed for one reference
height, so tracking re-aims the solve but fixes neither. A grid that barely varies height
cannot see the problem it most needs to. The file:

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

## Top-level structure

```jsonc
{
  "schema_version": 1,
  "units":            { "position": "meters", "gain": "decibels", "delay": "milliseconds" },
  "coordinate_space": "room, right-handed, +y up, +z forward (matches OptiTrack/Motive default); origin ON THE FLOOR at the working-area center (x/z); y = height above the floor",
  "reference": {
    "alignment":          "max-distance",   // how delay_ms was derived (documentation only)
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
  "speakers": [ /* 4..26 entries - the count IS the engine's channel count; see below */ ]
}
```

## Fields

| field | type | meaning |
|-------|------|---------|
| `schema_version` | int | reserved for breaking format changes. The loader currently ignores it - it is neither validated nor stored. |
| `units` | object | documentation only. The loader always converts dB → linear gain and ms → samples at the engine rate (positions are read as meters); changing this field has no effect. |
| `coordinate_space` | string | documentation of the frame; positions MUST match the `coordinate_space` value above (**room space**, floor origin). The engine works in it and the Unity/Unreal binding converts at its boundary; full seam: [`integration.md`](./integration.md) → "Coordinate seam". The engine derives its **nominal listening point from the array centroid** (world-locked bed/monitor decode directions + the default listener position), so the origin's exact spot is not load-bearing. |
| `reference` | object | provenance for the alignment values; `speed_of_sound_mps` is used if delays are derived rather than measured. Informational - the engine applies `delay_ms` as written. |
| `dbap.rolloff_r` | float | the **blur** knob `r` from [`spatialization.md`](./spatialization.md): larger spreads energy over more speakers. Must be > 0. **Omit it and the loader derives it from the geometry**: `0.25 ×` the mean centroid→speaker distance (Sundstrom 2021 recommends 0.2–0.5 of it) - ~0.53 m on the default grid. An explicit value always wins; treat the derived one as the starting point to dial against the real array. |
| `dbap.distance_attenuation` | object | the source→listener distance-attenuation curve (the second tuning knob). The loader reads only `reference_distance_m` (> 0), `rolloff` (> 0), and `min_gain_db` (≤ 0; floors the attenuation). `model` is ignored - the inverse curve is the only one implemented. |
| `pin_slab_m` | float (optional) | authoring only, engine-ignored: half-height of the ear-plane slab that `"pin": "plane"` speakers are confined to. Written by `bwa_layout_tool` when any speaker is pinned. |
| `speakers[]` | array | **4..26** speaker records (26 = the compile-time `BWA_CHANNELS` capacity). **The speaker count IS the engine's channel count** - a 24-speaker install loads a 24-entry file into the same binary. Order is not significant for DBAP, but `index` is the channel the speaker maps to on the bus / ASIO output, and the indices must form a complete `0..N-1` permutation. |

### Per-speaker record

| field | type | meaning |
|-------|------|---------|
| `index` | int `0..N-1` | bus/output channel for this speaker (N = the number of `speakers[]` records). Must be unique and cover `0..N-1` with no gaps - a complete permutation. |
| `position` | `[x, y, z]` float | surveyed position in room space (meters, RH). Each component must be finite and within ±1000 m. |
| `gain_db` | float | measured per-speaker level trim, applied in the align stage (`align_process`). `0.0` = no trim. Must be in **[-100, 24]**; anything outside rejects the file. |
| `delay_ms` | float | per-speaker delay to time-align arrival to the reference; converted to whole samples at `sample_rate` on load. `0.0` = the reference (farthest) speaker. A negative value clamps to `0`; anything **over 1000 ms rejects the file**. |
| `eq` | float array (optional) | minimum-phase correction-FIR taps (up to 512), written by `bwa_calibrate --eq` / `--room-eq`; applied per channel in the align stage before gain+delay. |
| `pin` | string (optional) | authoring only, engine-ignored: `"plane"` holds this speaker to the ear-plane slab (`pin_slab_m`) during optimization and snap. The allocation constraint from the authoring section above. |
| `room_eq` | object array (optional) | up to 8 LF modal-cut sections `{fc, gain_db, q}` (RBJ peaking, **cuts only**: `gain_db` in `[-24, 0]`, `fc` in `[10, 1000]`, `q` in `[0.25, 24]`), written by `bwa_calibrate --room-eq`. **Static-listener room correction** - see [`calibration.md`](./calibration.md); rendered as biquads at the engine rate. |

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

## Validation (loader contract)

`layout_load` rejects a malformed file (the reason surfaces through `bwa_last_error`)
if any of:

- `speakers.length` is outside `4..26`;
- `index` values are not a permutation of `0..N-1`;
- a `position` component is missing, non-numeric, non-finite, or beyond ±1000 m;
- `gain_db` is outside `[-100, 24]`, or `delay_ms` exceeds 1000 ms (a negative
  `delay_ms` is not an error: it clamps to 0);
- a `room_eq` section is out of its documented range, or an `eq` (>512 taps) or
  `room_eq` (>8 sections) array is over its cap, or a tap is non-finite;
- a `room_eq_grid` is malformed: 0 or >16 positions, an entry without
  `position[3]` + one `speakers` array per speaker (N), a section out of range,
  positions disagreeing on a speaker's `fc`/`q` ladder, or the file carrying both
  `room_eq` and `room_eq_grid`.

`schema_version` is not checked: the loader never reads it.

**A present-but-invalid `bwa_desc.layout_path` is NOT fatal.** `bwa_create` records the
reason in `bwa_last_error` and falls back to the **26-speaker default grid**, so desk/dev
runs with no survey still work; `bwa_start` still returns success. An installation that
must run on the surveyed geometry therefore **must check `bwa_last_error` after
`bwa_create`**: a silently-defaulted layout pans the array with the wrong speaker
positions, and (since the count follows the layout) a **different channel count**: a
20-speaker install whose file fails to load comes up as a 26-channel engine. A NULL/empty
`layout_path` intentionally selects the default grid with no error.

On success, the loader holds the parsed geometry in the internal `Layout` struct (see
[`internal-types.md`](./internal-types.md)); the audio thread reads it but never
reloads it.

## Calibration writeback (unknown fields survive)

`bwa_calibrate` writes its results back into this file, and it does so
non-destructively. Every `calib_write_*` function (`src/calib.c`) re-parses the
original JSON, mutates only its target fields (`gain_db`/`delay_ms` for the trims,
`eq`, `room_eq`, `room_eq_grid`, `position` for the survey), and re-serializes the
whole root.

Everything else in the file survives: unknown fields, per-speaker annotations, the
`reference` block, `note` strings. You can annotate a layout freely and recalibrate
without losing it. JSON has no comments, so keep annotations as extra string fields
(like `note`); those round-trip.
