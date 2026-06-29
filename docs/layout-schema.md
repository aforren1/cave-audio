# Speaker layout file schema (`cave_layout.json`)

The layout file is the surveyed description of the physical array. It is passed as
`BwConfig.layout_path` (see [`api.md`](./api.md)), loaded once by `layout.c` at
`bw_create`/`bw_start` time (control thread, file I/O — never on the audio thread), and
consumed by:

- **`dbap.c`** — speaker **positions** drive the listener-relative DBAP gain solve, and the
  global `dbap` block supplies the two tuning knobs (`rolloff_r` blur and the distance
  attenuation curve). See [`spatialization.md`](./spatialization.md).
- **`align_speakers`** — per-speaker `gain_db` trim and `delay_ms` align the unequal
  speaker distances to a common reference (final output stage, after the mix).
- **the binaural monitor** — the same positions become the virtual-speaker directions for
  the 26→ambisonics→binaural decode.

A complete, valid example lives at [`../examples/cave_layout.json`](../examples/cave_layout.json)
(a 3×3×3 boundary grid minus the center = exactly 26 speakers).

You can author this file interactively with **`bw_layout_tool`** (`examples/layout_tool.c`, built with
`-DBWAUDIO_BUILD_PLAYGROUND=ON`): place each speaker in 3D and **identify it by ear** — since a
speaker's `index` is its output channel, the tool drives that channel with the test signal
(`bw_test_signal`) out the cave profile, so you hear which physical speaker you're positioning. Press
**P** for a **DBAP preview** — a source pans through your in-progress layout (the tool rebuilds the
engine with the edited positions, since the layout is load-time) so you can hear gaps/smoothness and
walk the room to judge off-center coverage. It exports this schema with `delay_ms` auto-derived from
the positions (max-distance alignment). A headless `bw_layout_tool --export <file>` writes/normalizes
a layout without the GUI. To audition a saved layout in the full binaural playground:
`bw_playground cave_layout.json`.

**Scoring, constraints, and auto-optimization.** The tool can also *evaluate* and *improve* a layout
for a chosen panner. Press **X** (or run `--score <file>`) to print each panner's rE-localization error
(mean + worst over a direction shell × the working-volume listener grid, via `bw_panner_gains_batch` —
the same solve that ships, so the score reflects reality). Drop a **`constraints.json`** next to the
layout to declare where speakers may go — an allowed `bounds` box plus a `nogo` list of boxes (screens,
structure, doorways); the tool draws them (green bounds / red no-go), flags any speaker that violates,
and **K** snaps all speakers to the nearest allowed point. Press **B** to pick the target panner, then
**O** to **auto-optimize**: a constrained hill-climb that nudges positions to minimise that panner's rE
error while staying feasible — it runs live (watch the layout converge, O again to stop, S to save).
Headless: `--optimize <file> [dbap|spcap|vbap]` runs it to convergence in place. The constraint file:

```jsonc
{
  "bounds": { "min": [-2.5, -1.6, -2.5], "max": [2.5, 1.6, 2.5] },   // allowed placement box (room metres)
  "nogo": [                                                          // boxes speakers must stay out of
    { "min": [-1.6, -1.6, -2.6], "max": [1.6, 1.6, -2.25] }          // e.g. the front projection screen
  ]
}
```

## Top-level structure

```jsonc
{
  "schema_version": 1,
  "units":            { "position": "meters", "gain": "decibels", "delay": "milliseconds" },
  "coordinate_space": "room, right-handed (matches OptiTrack/Motive); origin at working-area center",
  "reference": {
    "alignment":          "max-distance",   // how delay_ms was derived (documentation only)
    "speed_of_sound_mps": 343.0,
    "note":               "..."
  },
  "dbap": {
    "rolloff_r": 0.5,                        // DBAP spatial-blur 'r' (how many speakers share energy)
    "distance_attenuation": {
      "model":               "inverse",      // source->listener attenuation curve
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
| `schema_version` | int | bump on breaking format changes; loader rejects unknown majors. |
| `units` | object | declares the units used; the loader converts to engine-internal (meters, linear gain, samples) at load. |
| `coordinate_space` | string | documentation of the frame. Positions MUST be in **room space, right-handed**, the same frame the engine works in (the Unity/Unreal binding converts at its boundary — see [`integration.md`](./integration.md)). |
| `reference` | object | provenance for the alignment values; `speed_of_sound_mps` is used if delays are derived rather than measured. Informational — `delay_ms` is authoritative. |
| `dbap.rolloff_r` | float | the **blur** knob `r` from [`spatialization.md`](./spatialization.md): larger spreads energy over more speakers. |
| `dbap.distance_attenuation` | object | the source→listener distance-attenuation curve (the second tuning knob). `model` + its parameters; `min_gain_db` floors the attenuation. |
| `speakers[]` | array | **exactly 26** speaker records. Order is not significant for DBAP, but `index` is the channel the speaker maps to on the 26-ch bus / ASIO output. |

### Per-speaker record

| field | type | meaning |
|-------|------|---------|
| `index` | int `0..25` | bus/output channel for this speaker. Must be unique and cover `0..25` with no gaps. |
| `position` | `[x, y, z]` float | surveyed position in room space (meters, RH). |
| `gain_db` | float | measured per-speaker level trim, applied in `align_speakers`. `0.0` = no trim. |
| `delay_ms` | float | per-speaker delay to time-align arrival to the reference; converted to whole samples at `sample_rate` on load. `0.0` = the reference (farthest) speaker. |

## Validation (loader contract)

`bw_create`/`bw_start` fails (NULL / nonzero, see [`api.md`](./api.md) error codes) if any of:
- `speakers.length != 26`;
- `index` values are not a permutation of `0..25`;
- a `position` is missing or not three finite numbers;
- `schema_version` major is newer than the loader supports.

On success, the loader holds the parsed geometry in the internal `Layout` struct (see
[`internal-types.md`](./internal-types.md)); the audio thread reads it but never reloads it.

> The `26` is currently fixed by the array. If the array size ever becomes configurable,
> it moves from a hard constant to a value derived from `speakers.length` — but that is out
> of scope for the current build (see the channel-count decision in
> [`architecture.md`](./architecture.md)).
