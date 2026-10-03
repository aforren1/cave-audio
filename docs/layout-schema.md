# Speaker layout file schema (`cave_layout.json`)

The layout file is the surveyed description of the physical array. You pass it as
`bwa_desc.layout_path` (see [`api.md`](./api.md)). `layout.c` loads it once at
`bwa_create` time, on the control thread. File I/O never touches the audio
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

## Authoring

To author or optimize a layout with `bwa_layout_tool`, see
[layout-authoring.md](./layout-authoring.md). This page is the file format reference.

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
| `dbap.rolloff_r` | float (optional) | the **blur** knob `r` from [`spatialization.md`](./spatialization.md): larger spreads energy over more speakers. A number ≤ 0 rejects the file. The loader floors a smaller positive value at 0.001 m (1 mm), below any audible blur. **Omit it and the loader derives it from the geometry**: `0.25 ×` the mean distance from the listening point to the speakers (the centroid unless `listening_point_m` is set; Sundstrom 2021 recommends 0.2 to 0.5 of it), floored at 0.05 m. A non-number counts as omitted. On a file shaped like the default grid that comes to about 0.53 m; the built-in grid with no file uses 0.5. An explicit value always wins. Treat the derived one as the starting point to dial against the real array. |
| `dbap.distance_attenuation` | object (optional) | the source→listener distance-attenuation curve (the second tuning knob), the default for every source without its own attenuation. The loader reads only `reference_distance_m` (default 1.0; must be > 0), `rolloff` (default 1.0; must be > 0), and `min_gain_db` (default -40; must be ≤ 0; floors the attenuation). An out-of-range number rejects the file; a non-number keeps the default. `model` is ignored: the inverse curve is the only one implemented. |
| `pin_slab_m` | float (optional) | authoring only, engine-ignored: half-height of the ear-plane slab that `"pin": "plane"` speakers are confined to. Written by `bwa_layout_tool` when any speaker is pinned. |
| `directivity` | object (optional) | one speaker model's off-axis loss table, written by `tools/directivity/clf_to_json.py` from the vendor's CLF simulation file. Read by `bwa_calibrate` (trims measured off a speaker's axis are re-aimed to the listening point), by the engine's tracked directivity compensation (`bwa_set_tracked_directivity`), and by the array-sim headphone monitor (`cave_sim` and the `cave_both` tap play each speaker's off-axis loss toward the listener, at the table's full resolution). Format below. |
| `speakers[]` | array | **4..64** speaker records (64 = the compile-time `BWA_MAX_CHANNELS` capacity). **The speaker count IS the engine's channel count** - a 24-speaker install loads a 24-entry file into the same binary. Order is not significant for DBAP, but `index` is the channel the speaker maps to on the bus / ASIO output, and the indices must form a complete `0..N-1` permutation. |

### Per-speaker record

| field | type | meaning |
|-------|------|---------|
| `index` | int `0..N-1` (required) | bus/output channel for this speaker (N = the number of `speakers[]` records). Must be unique and cover `0..N-1` with no gaps: a complete permutation. A fractional value truncates to an integer. |
| `position` | `[x, y, z]` float (required) | the speaker's **acoustic center** in room space (meters, RH): not its front baffle and not its mounting point. See "Positions are acoustic centers" below. Each component must be finite and within ±1000 m. |
| `gain_db` | float (optional, default 0) | measured per-speaker level trim, applied in the align stage (`align_process`). `0.0` = no trim. Must be in **[-100, 24]**; anything outside rejects the file. A non-number counts as 0. |
| `delay_ms` | float (optional, default 0) | per-speaker delay to time-align arrival to the reference; rounded to the nearest whole sample at `sample_rate` on load. `0.0` = the reference (farthest) speaker. A negative value clamps to `0`; anything **over 1000 ms rejects the file**. A non-number counts as 0. |
| `eq` | float array (optional) | minimum-phase correction-FIR taps (up to 512), written by `bwa_calibrate --eq` / `--room-eq`; applied per channel in the align stage before gain+delay. Each tap must be a finite number within ±100, or the file is rejected. |
| `pin` | string (optional) | authoring only, engine-ignored: `"plane"` holds this speaker to the ear-plane slab (`pin_slab_m`) during optimization and snap. See [Pins](./layout-authoring.md#pins). |
| `room_eq` | object array (optional) | up to 8 LF modal-cut sections `{fc, gain_db, q}` (RBJ peaking, **cuts only**: `gain_db` in `[-24, 0]`, `fc` in `[10, 1000]`, `q` in `[0.25, 24]`), written by `bwa_calibrate --room-eq`. **Static-listener room correction** - see [`calibration.md`](./calibration.md); rendered as biquads at the engine rate. |
| `aim` | `[x, y, z]` float (optional) | the speaker's acoustic axis as a direction vector in room space (any length; normalized on load). Omit it and the speaker is aimed at the listening point, which is the CAVE's case. A zero or non-finite vector rejects the file. In the engine, only the `directivity` model reads it (the tracked compensation and the array sim). The tools read it too: `bwa_calibrate --aim-sheet` and `--check-aim`, and the layout tool's beam display. |
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

1 to 16 positions, each `position` component finite and within ±1000 m. Section ranges
match `room_eq` (cuts only, `fc` `[10, 1000]`, `q` `[0.25, 24]`; `gain_db 0` = this position
doesn't need the cut). **Every position must carry the same per-speaker `fc`/`q` ladder**
(only the depths vary), because the runtime interpolates depths by ladder index. The loader
rejects a speaker whose section count differs between positions, or an `fc` or `q` more than
0.5% off the first position's.
`room_eq` and `room_eq_grid` in one file are rejected too (one correction scheme at
a time). The calibration writeback maintains both invariants for you.

### Speaker directivity: top-level `directivity` (optional)

One speaker model's measured off-axis loss, written by `tools/directivity/clf_to_json.py`
from the vendor's CLF file (see [calibration.md](./calibration.md) -> "Speaker directivity
from the vendor's simulation file"). One model per layout, axisymmetric about each
speaker's `aim`.

```jsonc
"directivity": {
  "model":      "Genelec 4410A",                                  // provenance, engine-ignored
  "source":     "Genelec_Oy-4410A.CF2 (CLF v2 balloon ...)",   // provenance, engine-ignored
  "bands_hz":   [40, 50, 63, /* ... */ 16000],                  // 1..32 band centers, positive, ascending, <= 1e6
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

- the file cannot be opened or is not valid JSON;
- `speakers` is missing or not an array, or an entry is not an object;
- `speakers.length` is outside `4..64` (`4..BWA_MAX_CHANNELS`);
- an `index` is missing or non-numeric, or the indices are not a permutation of `0..N-1`;
- a `position` is missing or not three entries, or a component is non-numeric, non-finite,
  or beyond ±1000 m;
- `dbap.rolloff_r` is a number ≤ 0, `dbap.distance_attenuation.reference_distance_m` or
  `.rolloff` is a number ≤ 0, or `.min_gain_db` is a number > 0;
- `listening_point_m` is present but not three finite numbers within ±1e6 m;
- `gain_db` is outside `[-100, 24]`, or `delay_ms` exceeds 1000 ms (a negative
  `delay_ms` is not an error: it clamps to 0);
- an `eq` array has more than 512 taps, or a tap is non-numeric, non-finite, or beyond ±100;
- a `room_eq` array has more than 8 sections, or a section lacks a numeric `fc`, `gain_db`
  or `q`, or one is out of its documented range;
- a `room_eq_grid` is malformed: 0 or more than 16 positions, an entry without
  `position[3]` plus one `speakers` array per speaker (N), a position component non-finite
  or beyond ±1000 m, more than 8 sections for a speaker, a section out of range,
  positions disagreeing on a speaker's section count or its `fc`/`q` ladder (0.5%
  tolerance), or the file carrying both `room_eq` and `room_eq_grid`;
- a speaker's `aim` is present but not three finite numbers, or is the zero vector;
- a speaker's `plan_position` is not three finite numbers within ±1000 m, its `plan_aim` is not
  three finite numbers or is the zero vector, or it has a `plan_aim` without a `plan_position`;
- a `directivity` block is malformed: not an object, a missing `bands_hz`, `angles_deg` or
  `loss_db` array, `bands_hz` outside 1..32 entries, not positive, not ascending, or past
  1e6, `angles_deg` outside 2..37 entries, not starting at 0, not ascending, or
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
`eq`, `room_eq`, `room_eq_grid`, `position` for the survey, `reference.speed_of_sound_mps`
for the air temperature), and re-serializes the
whole root. The position writer also records the plan once, by the rule above
(`layout_json_keep_plan` in `layout.c`, which `bwa_speaker_survey` calls too). Every writer
finds a speaker's record by its `index`, not its place in the array, so the records can sit in
any order.

Everything else in the file survives: unknown fields, per-speaker annotations, the
`reference` block, `note` strings. You can annotate a layout freely and recalibrate
without losing it. JSON has no comments, so keep annotations as extra string fields
(like `note`); those round-trip.
