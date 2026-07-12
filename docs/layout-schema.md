# Speaker layout file schema (`cave_layout.json`)

The layout file is the surveyed description of the physical array. You pass it as
`BwConfig.layout_path` (see [`api.md`](./api.md)). `layout.c` loads it once at
`bw_create`/`bw_start` time, on the control thread — file I/O never touches the audio
thread. Three consumers read the result:

- **`dbap.c`** — speaker **positions** drive the listener-relative DBAP gain solve.
  The global `dbap` block supplies the two tuning knobs: `rolloff_r` blur and the
  distance-attenuation curve. See [`spatialization.md`](./spatialization.md).
- **the align stage** (`align_process`) — per-speaker `gain_db` trim and `delay_ms`
  align the unequal speaker distances to a common reference (output stage, after the mix).
- **the binaural monitor** — the same positions become the virtual-speaker directions
  for the 26→ambisonics→binaural decode.

A complete, valid example lives at [`../examples/cave_layout.json`](../examples/cave_layout.json):
a 3×3×3 boundary grid minus the center = exactly 26 speakers, floor-origin, y layers at
0 / 1.5 / 3 m, ears nominally at 1.5.

## Authoring with `bw_layout_tool`

You can author this file interactively with **`bw_layout_tool`**
(`examples/layout_tool.cpp`, built with `-DBWAUDIO_BUILD_PLAYGROUND=ON`). Place each
speaker in 3D and **identify it by ear**: a speaker's `index` is its output channel,
so the tool drives that channel with the test signal (`bw_test_signal`) out the cave
profile — you hear which physical speaker you're positioning.

Press **P** for a **DBAP preview**: a source pans through your in-progress layout, so
you can hear gaps/smoothness and walk the room to judge off-center coverage. The tool
rebuilds the engine with the edited positions, since the layout is load-time.

The tool exports this schema with `delay_ms` auto-derived from the positions
(max-distance alignment). A headless `bw_layout_tool --export <file>`
writes/normalizes a layout without the GUI. To audition a saved layout in the full
binaural playground: `bw_playground cave_layout.json`.

**Scoring, constraints, and auto-optimization.** The tool can also *evaluate* and
*improve* a layout for a chosen panner. Press **X** (or run `--score <file>`) to print
each panner's rE-localization error — mean + worst over a direction shell × the
working-volume listener grid, via `bw_panner_gains_batch`. That is the same solve that
ships, so the score reflects reality.

Drop a **`constraints.json`** next to the layout (see `examples/constraints.json`) to
declare where speakers may go:

- **`bounds`** — the allowed box; speakers must be inside.
- **`nogo`** — keep-out boxes (screens, structure, doorways, the CAVE interior);
  speakers must be outside. Drawn red-wire; snappable with **K**; the optimizer stays
  out of them.
- **`obstacles`** — SOLID occluders (projectors, beams). A speaker can't be inside one
  *nor in its acoustic shadow*: a box on the segment from the speaker to the ears
  blocks its sound (line-of-sight to the observer at ear height). Drawn filled-orange.
  Shadowed speakers are ringed orange, and the optimizer *penalises* them — it can't
  push them out geometrically, so nudge them clear. A box only crudely bounds a
  projector's throw frustum; size it to the body plus the near shadow you care about.

The tool flags violations, and **K** snaps speakers off bounds/no-go/obstacle bodies.
Press **B** to pick the target panner, then **O** to **auto-optimize**: a constrained
hill-climb that nudges positions to minimise that panner's rE error while staying
feasible. A **leash** slider caps how far a speaker may drift from where it started,
so the optimizer refines rather than relocates. It runs live (O again to stop, S to
save). Headless: `--optimize <file> [dbap|spcap|vbap]`.

Scoring/coverage target the observer at **ear height** (the `obs y` slider, default
1.4 m — a real head, not the floor). The **C** coverage overlay shades a direction
shell to show where the array is weak. **G** switches its metric between the geometric
*nearest-speaker gap* and the selected panner's *per-direction rE error* (green =
accurate, red = mislocalised; hover a cube for its value). **V** toggles the observer
model (fixed centre vs the moving working volume). The file:

```jsonc
{
  "bounds": { "min": [-4, 0, -4], "max": [4, 4.5, 4] },        // allowed box (room metres, floor y=0)
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
  "coordinate_space": "room, right-handed, +y up, +z forward (matches OptiTrack/Motive default); origin ON THE FLOOR at the working-area centre (x/z); y = height above the floor",
  "reference": {
    "alignment":          "max-distance",   // how delay_ms was derived (documentation only)
    "speed_of_sound_mps": 343.0,
    "note":               "..."
  },
  "dbap": {
    "rolloff_r": 0.5,                        // DBAP spatial-blur 'r' (how many speakers share energy)
    "distance_attenuation": {
      "model":               "inverse",      // documentation only — the loader ignores this field
      "reference_distance_m": 1.0,
      "rolloff":              1.0,
      "min_gain_db":         -40.0
    }
  },
  "speakers": [ /* exactly 26 entries, see below */ ]
}
```

## Fields

| field | type | meaning |
|-------|------|---------|
| `schema_version` | int | reserved for breaking format changes. The loader currently ignores it — it is neither validated nor stored. |
| `units` | object | documentation only. The loader always converts dB → linear gain and ms → samples at the engine rate (positions are read as meters); changing this field has no effect. |
| `coordinate_space` | string | documentation of the frame. Positions MUST be in **room space: right-handed, +y up, +z forward, origin on the floor** (Motive's ground-plane default; y = height above the floor) — the frame the engine works in (the Unity/Unreal binding converts at its boundary — see [`integration.md`](./integration.md)). The engine derives its **nominal listening point from the array centroid** (world-locked bed/monitor decode directions + the default listener position), so the origin's exact spot is not load-bearing. |
| `reference` | object | provenance for the alignment values; `speed_of_sound_mps` is used if delays are derived rather than measured. Informational — the engine applies `delay_ms` as written. |
| `dbap.rolloff_r` | float | the **blur** knob `r` from [`spatialization.md`](./spatialization.md): larger spreads energy over more speakers. Must be > 0. |
| `dbap.distance_attenuation` | object | the source→listener distance-attenuation curve (the second tuning knob). The loader reads only `reference_distance_m` (> 0), `rolloff` (> 0), and `min_gain_db` (≤ 0; floors the attenuation). `model` is ignored — the inverse curve is the only one implemented. |
| `speakers[]` | array | **exactly 26** speaker records. Order is not significant for DBAP, but `index` is the channel the speaker maps to on the 26-ch bus / ASIO output. |

### Per-speaker record

| field | type | meaning |
|-------|------|---------|
| `index` | int `0..25` | bus/output channel for this speaker. Must be unique and cover `0..25` with no gaps. |
| `position` | `[x, y, z]` float | surveyed position in room space (meters, RH). Each component must be finite and within ±1000 m. |
| `gain_db` | float | measured per-speaker level trim, applied in the align stage (`align_process`). `0.0` = no trim. Must be in **[-100, 24]**; anything outside rejects the file. |
| `delay_ms` | float | per-speaker delay to time-align arrival to the reference; converted to whole samples at `sample_rate` on load. `0.0` = the reference (farthest) speaker. A negative value clamps to `0`; anything **over 1000 ms rejects the file**. |
| `eq` | float array (optional) | minimum-phase correction-FIR taps (up to 512), written by `bw_calibrate --eq` / `--room-eq`; applied per channel in the align stage before gain+delay. |
| `room_eq` | object array (optional) | up to 8 LF modal-cut sections `{fc, gain_db, q}` (RBJ peaking, **cuts only**: `gain_db` in `[-24, 0]`, `fc` in `[10, 1000]`, `q` in `[0.25, 24]`), written by `bw_calibrate --room-eq`. **Static-listener room correction** — see [`calibration.md`](./calibration.md); rendered as biquads at the engine rate. |

## Validation (loader contract)

`layout_load` rejects a malformed file (the reason surfaces through `bw_last_error`)
if any of:

- `speakers.length != 26`;
- `index` values are not a permutation of `0..25`;
- a `position` component is missing, non-numeric, non-finite, or beyond ±1000 m;
- `gain_db` is outside `[-100, 24]`, or `delay_ms` exceeds 1000 ms (a negative
  `delay_ms` is not an error — it clamps to 0);
- a `room_eq` section is out of its documented range, or an `eq` (>512 taps) or
  `room_eq` (>8 sections) array is over its cap, or a tap is non-finite.

`schema_version` is not checked — the loader never reads it.

**A present-but-invalid `BwConfig.layout_path` is NOT fatal.** `bw_create` records the
reason in `bw_last_error` and falls back to the **default grid**, so desk/dev runs
with no survey still work; `bw_start` still returns success. An installation that must
run on the surveyed geometry therefore **must check `bw_last_error` after
`bw_create`** — a silently-defaulted layout pans the array with the wrong speaker
positions. A NULL/empty `layout_path` intentionally selects the default grid with no
error.

On success, the loader holds the parsed geometry in the internal `Layout` struct (see
[`internal-types.md`](./internal-types.md)); the audio thread reads it but never
reloads it.

## Calibration writeback (unknown fields survive)

`bw_calibrate` writes its results back into this file, and it does so
non-destructively. Every `calib_write_*` function (`src/calib.c`) re-parses the
original JSON, mutates only its target fields — `gain_db`/`delay_ms` for the trims,
`eq`, `room_eq`, `position` for the survey — and re-serializes the whole root.

Everything else in the file survives: unknown fields, per-speaker annotations, the
`reference` block, `note` strings. You can annotate a layout freely and recalibrate
without losing it. JSON has no comments, so keep annotations as extra string fields
(like `note`) — those round-trip.

> The `26` is currently fixed by the array. If the array size ever becomes configurable,
> it moves from a hard constant to a value derived from `speakers.length` — but that is out
> of scope for the current build (see the channel-count decision in
> [`architecture.md`](./architecture.md)).
