# bw_audio for Godot

A GDExtension binding for the CAVE spatial audio engine. Godot is a **thin control
client**: no rendered audio crosses the boundary, and Godot's own `AudioServer` is
unused. The engine drives ASIO itself.

This directory is both things at once:

- `addons/bw_audio/` — the drop-in addon. Copy it into your project to install.
- `project.godot` + `demo/` — a project that consumes it, so you can open this folder
  in Godot and press play.

## Build

The extension is a CMake target in the main build, opt-in because fetching and
generating godot-cpp's bindings is a multi-minute first build:

```
cmake -S . -B build-godot -A x64 -DBWA_BUILD_GODOT=ON -DGODOTCPP_TARGET=editor
cmake --build build-godot --config RelWithDebInfo --target bwa_gdextension
```

The build writes `bw_audio_gd.windows.editor.x86_64.dll` into
`bindings/godot/addons/bw_audio/bin/` and copies `bw_audio.dll` (plus `phonon.dll` when
the Steam Audio build is on) beside it. Then open `bindings/godot/` in Godot.

`GODOTCPP_TARGET` is a **build-wide** choice, so one configure tree produces one library
flavour. It defaults to `editor` here — godot-cpp's own default is `template_debug`, which
builds fine and then does not load in the editor, so the addon would look broken. A shipping
build wants a second tree with `-DGODOTCPP_TARGET=template_release`.

## Install

**From the Asset Store** (in the editor: AssetLib tab, search "bw_audio"), or by hand:
download `bw_audio-godot-<version>.zip` from a GitHub Release and unzip it into your project
so you end up with `addons/bw_audio/`. Either way, restart the editor afterwards. A
GDExtension loads on project open — there is no plugin to enable.

Godot has **no equivalent of Unity's "add package from git URL"**: the documented routes are
the store's own listing and a manual zip extract, and there is no install-from-URL or
install-from-file entry point in the editor. So unlike the Unity binding, the `godot`
distribution branch is not something you can install *from* directly — it exists to satisfy
a store listing that pulls a repo archive.

Windows x64 only, because the engine's device path is ASIO.

## Distribution

`tools/godot/pack.ps1` builds **both** library flavours, writes the release zip, and leaves a
staged addon tree at `build/godot/addon`:

```
powershell -File tools/godot/pack.ps1
```

That staged tree is then pushed to the **`godot` branch** by
`tools/dist/publish-branch.ps1`, which CI runs on a `v*` tag. The branch's root is
`addons/bw_audio/`, and that layout is not cosmetic: **the Asset Library downloads a repo
archive at a specific commit**, not a release asset, so the binaries have to be committed
somewhere. Keeping them on their own branch leaves `main` source-only and its existing
"no binaries in the tree" rule intact.

Publishes are ordinary commits, never a force-push — an Asset Library entry pins a commit
hash, and rewriting history would eventually orphan a published one. The standing cost is
that the branch grows by roughly the addon's size per release; that is inherent to in-repo
distribution.

A release also attaches the zip beside the Unity `.tgz`, the engine bundle and the ASIO SDK
source, for anyone who would rather not use the AssetLib.

### Submitting a store listing

The Asset Library has moved to the **[Asset Store](https://store.godotengine.org/)** (beta).
Its guidelines matter to us in three specific ways:

- **Precompiled binaries in a GDExtension are explicitly allowed** — with the condition that
  *every shared library named in the `.gdextension` must be present*. `pack.ps1` parses the
  manifest's `[libraries]` section and refuses to pack if any path is missing, so that rule is
  enforced at build time rather than discovered at review.
- **`windows.template_debug` points at the release binary on purpose.** godot-cpp's
  template_debug differs only in its own internal checks, so building a third flavour would
  triple the godot-cpp cost to debug code that is not the code under debug — but simply
  *listing* a flavour we do not build would be an automatic rejection.
- **The thumbnail must be 16:9**, where the old library wanted a square icon. Both are
  committed: `examples/icons/bw_audio.png` (256×256) and `bw_audio_thumb.png` (1280×720).

Whether the store hosts an uploaded zip or pulls a repo archive is not documented on its
public pages, and it is in beta — so both are covered: the release zip is the upload, and the
`godot` branch is the archive. Whichever it turns out to want, nothing needs rebuilding.

Each release needs the listing edited by hand; there is no API. If it pulls a commit, use the
hash the publish step printed (`published <sha> -> godot`). Icon URLs must be
`raw.githubusercontent.com`, e.g.
`https://raw.githubusercontent.com/aforren1/cave-audio/main/examples/icons/bw_audio.png`.
License is GPL-3.0, and the LICENSE inside the addon carries the copyright holder and year
the guidelines require. Submissions are human-reviewed.

Both flavours are packed because either alone fails in a way that only shows up late —
without `editor` the addon does nothing when you open the project, and without
`template_release` it works right up until someone exports. The script refuses to pack if
either is missing, rather than shipping a manifest that promises libraries it does not carry.

Both flavours are packed because either alone fails in a way that only shows up late —
without `editor` the addon does nothing when you open the project, and without
`template_release` it works right up until someone exports. The script refuses to pack if
either is missing, rather than shipping a manifest that promises libraries it does not carry.

Two things it stages deliberately: it copies into a **clean** directory first, because
`addons/bw_audio/bin/` is gitignored build output and anything packing the working tree
naively yields an addon with no binaries; and `phonon.dll` is listed in the manifest's
`[dependencies]` so Godot's **exporter** carries it. Nothing in Godot references phonon —
it is an import of `bw_audio.dll` — so without that entry an exported game ships the
extension alone and fails to load it, having worked perfectly in the editor.

A no-SDK build is supported (ISM + FDN + manual occlusion); packing one warns rather than
fails, and says what is lost.

### godot-cpp version

Pinned to a commit (`BWA_GODOT_CPP_COMMIT` in `CMakeLists.txt`), not a branch: godot-cpp's
release branches lag the engine — 4.5 is the newest cut — while master already ships
`extension_api.json` for 4.7, which is what the installed editor is. When the editor
moves, bump the commit and check that api file's header still matches.

## Coordinate seam

The core works in **room space: right-handed, +Y up, +Z forward, metres, origin on the
floor** — OptiTrack/Motive's default streamed frame.

Godot is **also right-handed and Y-up**, so unlike Unity there is **no mirror**: positions
and directions pass through the CAVE registration transform and nothing else. The one
difference is a facing convention — Godot's `Vector3.FORWARD` is `-Z`, room's identity
quaternion faces `+Z` — so orientations pick up a 180° yaw:

```gdscript
room_pos  = registration * node.global_position
room_dir  = registration.basis * dir
room_quat = Quaternion(registration.basis * node.global_basis) * Quaternion(Vector3.UP, PI)
```

Two consequences worth knowing, both places Unity's advice does *not* carry over:

- `bwa_bed_set_rotation` takes a room-frame yaw, and because there is no mirror the
  **sense of rotation is preserved**. Unity's binding has to reverse it; this one does not.
- Positions need no axis flip at all. If a source ends up mirrored, the registration
  transform is wrong — not the handedness conversion.

Budget time to verify with a known-position test source before trusting anything else.

## Channel count

The engine's channel count is **the layout's speaker count** (4..26), not a constant. Read
it with `BwaEngine.get_channel_count()` and size any meter or speaker-gizmo array from it.
Never hard-code 26.

The trap: a failed layout load is **not** fatal at create — the core falls back to the
26-speaker default grid and only records the reason. `BwaEngine` surfaces that as a
warning, and `bwa_start` refuses the fallback when a path was given, so a bad layout fails
loudly instead of quietly changing the channel count.

## Asset paths

The core loads audio and layout files itself, by OS path — it never goes through Godot's
virtual filesystem. `res://` paths are globalized for you, but in an **exported** build
`res://` lives inside the `.pck` and that path will not exist. Ship those files beside the
executable, or stage them into `user://` at startup. (Same trap as Unity's
`StreamingAssets`, one layer down.)

## The frame

**One node pushes everything.** Each `_process`, `BwaEngine` pulls every registered
emitter's transform, pushes the listener pose, then calls `bwa_commit` once. Emitters
deliberately do *not* push themselves: the commit is what defines frame coherence, so
everything it covers has to be sampled together, or you can commit a frame where the
listener moved but some sources hadn't.

Godot has no `LateUpdate`, so freshness comes from `process_priority` instead —
`BwaEngine` defaults to `1000` so it runs after ordinary gameplay nodes. Coherence does
not depend on that: if a source moves after we sampled it, every source *and* the listener
are one frame old together, which is inaudible. Only freshness is at stake.

## Nodes

There is no separate 1:1 P/Invoke-style layer. GDExtension needs no such shim, so every
call lives as a method on whichever class owns the handle it operates on.

**`BwaEngine : Node`** — everything engine-global: lifecycle, the registration transform,
listener and tracker wiring, the asset cache, master gain/pause and mix groups, the reverb
configuration (Steam bed *or* FDN), materials and scene geometry, the clock, the
diagnostics readbacks, and the live A/B knobs (panner, dual-band, spread mode,
decorrelation, near-spread, max-rE and its band split, bed renderer, tracked room EQ,
limiter). Those are exported properties that also apply live, so the inspector *is* the
A/B tool.

**`BwaSource : Node3D`** (abstract) — everything a spatial voice can do regardless of where
its audio comes from: gain, priority, group, fades, pause, spread/extent/size, Doppler, air
absorption, loudness compensation, attenuation override, occlusion (ray-traced or manual),
directivity, reverb sends, early reflections, pathing.

**`BwaEmitter : BwaSource`** — plays a file. Adds `play`/`play_at`/`play_loop`/`stop_at`,
gapless `queue`, `seek`, `pitch`, and a `finished` signal.

**`BwaPushSource : BwaSource`** — you feed it PCM. Adds `push`/`push_space`/`push_end`.

**`BwaBed : Node`** — a world-locked ambisonic soundfield. A `Node`, not a `Node3D`: a bed
has no position, only an orientation.

**`BwaMaterial : Resource`** — an acoustic material as a `.tres`. Either a built-in preset
or custom 3-band coefficients. The preset is an **enum, not a string**: the core answers an
unknown material name with the generic default and a note in `bwa_last_error`, which is not
an error, just a wrong sound. Tokens are minted lazily and cached against the engine's
generation, because the core's material table is fixed-capacity and meant to be filled once.

**`BwaAcousticGeometry : Node3D`** — static occluding/reflecting geometry, from a `Mesh` or
an existing `MeshInstance3D`. **`BwaDynamicGeometry : Node3D`** — a movable occluder (a
cheap BVH refit, not a rebuild; needs the Steam Audio build). **`BwaRoomBox : Node3D`** —
the shoebox, which also captures the room for the image-source early reflections and so
matters even in a phonon-free build.

**`BwaSpeakerView : Node3D`** — one gizmo per speaker, lit by that channel's live output
level. It reads the geometry back from the engine, so it draws the array the engine is
*actually* panning with: a layout that failed to load and fell back to the default grid
looks wrong immediately instead of sounding wrong later.

### Geometry is collected, not pushed

`bwa_scene_set_mesh_mat` **replaces** the whole static mesh, and `bwa_scene_set_box` is a
convenience that calls it. Per-node calls would therefore clobber each other — a scene with
a room box and a pillar would lose one of them, silently, with the loser simply inaudible.

So every static piece registers in `_enter_tree` (which Godot runs top-down, before any
`_ready`) and `BwaEngine` merges them into **one** call before `bwa_start`. The room box
goes first and alone, because `scene_set_box` is the only way to capture the shoebox for
the image-source reflections, and then contributes its own 12 inward-facing wall triangles
to that merged mesh — otherwise the walls would vanish the instant any other occluder
existed.

### Warnings for the settings that fail quietly

Godot's `_get_configuration_warnings()` gives the scene tree a warning marker, which is
exactly where the engine's *survivable* mistakes belong — none of these are errors to the
core. It starts, it renders, it just sounds wrong:

- both reverb beds enabled, contending for the one tap;
- source early reflections alongside the Steam reflection bed, which already contains
  early reflections, so they render twice;
- `feed_listener` on with no listener node, so the listener never leaves the origin;
- a `layout_path` that does not exist, or a `res://` one that will not survive export;
- ray-traced occlusion or pathing switched on without the SDK or without geometry.

The split into three source classes is deliberate. The core genuinely *refuses*
play/seek/pitch on a push voice, so a single node with a mode flag would leave those
visible in the inspector and silently inert — the exact class of quiet failure this binding
tries to make unrepresentable.

### Two things that bite anything polling a voice

`bwa_source_play` only **enqueues**, and `bwa_source_is_playing` is a per-block republish,
so for a frame or two after a play the raw readback honestly says "not playing".
`BwaEmitter` absorbs that window — a just-issued play counts as playing, and only a voice
actually *observed* playing can fire `finished`. Without that, a naive edge detector fires
`finished` on frame one of every sound.

`finished` means the sound **ran out** — a non-loop end, a drained queue. An explicit
`stop()` or `fade_out()` never fires it; `stop_at()` deliberately does, because a scheduled
stop is an arranged ending and the caller wants to know when it landed.

`bwa_set_output_capture` is **not bound, on purpose.** Its callback runs on the audio
thread, where calling into GDScript would allocate and take the interpreter lock — exactly
what invariant 1 forbids. Use the MANUAL sink and `render_block()` for capture instead.

## Tests

```
ctest --test-dir build-godot -C RelWithDebInfo -R godot_
```

- **`godot_room`** — the coordinate seam's property test. It asserts the *property* (a
  node's Godot forward equals the converted quaternion's room facing) rather than
  restating the conversion, over cardinal and random orientations. Pure float math, no
  godot-cpp, so it always builds.
- **`godot_smoke`**, **`godot_frame`**, **`godot_api_manual`**, **`godot_api_null`**,
  **`godot_scene`**, **`godot_scene_no_room`**, **`godot_playground`** — the demo scenes and
  the playground self-test, self-checking and headless. These need an editor binary, which is
  not a build dependency, so point CMake at one:
  `-DBWA_GODOT_EXE=".../Godot_v4.7.1-stable_mono_win64_console.exe"`. Without it they are
  simply not registered.

All of these **do** run in CI, unlike the three imgui tools: Godot's `--headless` needs no
display or GL. CI passes `--no-tests=error` so an empty selection fails rather than passing —
a bad filter, a missing `BWA_GODOT_EXE` and a stale build cache otherwise all look identical
to success.

`api.tscn` drives the whole bound surface, and it runs **twice, on both sinks**, because
either alone leaves a hole:

- **MANUAL** has no device and no audio thread, so blocks advance only when the scene pumps
  `render_block()`. Playheads become functions of how many blocks were requested rather
  than of machine speed, which is what lets `seek(24000)` landing and a paused playhead
  freezing be assertions instead of coin flips. But nothing there is concurrent: one thread
  on the command ring, one thread on both ends of the push ring, no voice retiring under a
  live mixer. That is not what ships.
- **NULL** puts the audio thread back — the production topology minus the device — and the
  scene loosens its timing assertions to what a wall clock can honestly promise.

Both check what a binding can actually get wrong: that every call reaches the core with its
arguments intact, that handles survive, and that readbacks come back shaped right. Whether
the DSP is *correct* is the engine's own suite's job.

`scene.tscn` covers the geometry path, where almost nothing is observable: there is no
readback for the static mesh. The one crack of light is that enabling image-source early
reflections without a captured room makes the core record *"no room — call
bwa_scene_set_box first"*. Its **absence** is therefore evidence the box arrived — and
`godot_scene_no_room` drops the box so the same assertion has to fail, which is what keeps
the positive run from passing vacuously.

## The playground

`playground/` is the Godot port of `examples/playground.cpp` — the by-ear harness. Open this
folder in Godot and run `playground/playground.tscn`, or:

```
godot --path bindings/godot res://playground/playground.tscn
```

Same seven scenes, same keys, same synthesized signals. The C++ one stays: it needs no
Godot, and it keeps its own imgui test suite. What running both buys you is a check on this
binding — one engine build, two clients, and any audible difference is the binding's fault.

The signal synthesis is a deliberately literal port (same LCG seeds, same Paul Kellet pink
coefficients, same periods), so an A/B between the two playgrounds compares engine paths
rather than two different noises.

Scenes, TAB to cycle: localization, occlusion and materials, directivity, channel walk,
blind A/B/X, ambisonic bed, reverb bed. WASD/RF move the source, Q/E turn the head, 1–4 pick
the signal. Every scene also declares its own controls — dropdowns, toggles, sliders — which
the panel builds; the keyboard shortcuts drive the *same* setters, so the two input paths
cannot drift apart, and pressing a key visibly moves the matching widget.

**Movement follows the room basis, read from the ABI** (`BwaEngine.room_right()` and
friends), not from written-out signs. Room right is **−X**, which is the opposite of the
reflex — and a hardcoded guess yields a scene that looks entirely plausible with left and
right swapped. The floor grid marks the two axes for the same reason: green ahead, red to
the listener's right, matching the red right ear on the head gizmo.

Two things worth knowing:

- **The reverb scene rebuilds the engine** on entry and exit, because the bed and the room
  geometry are load-time. Here that means tearing down the whole rig subtree and standing a
  new one up, which is a brief audio gap by design.
- **`switch_scene()` resets every engine-wide knob.** The knobs are global, so a scene that
  left SPCAP selected or a spread mode engaged would silently change what the *next* scene
  appears to demonstrate. That reset list is ported verbatim for exactly that reason.

With no ASIO device the engine falls back to the null sink and everything still runs, just
silent — visual-only is a supported state, not a failure, and the HUD says so.

`godot_playground` walks all seven scenes headless, crosses the reverb boundary both ways,
and renders each HUD. It cannot judge how anything *sounds* — that is the tool's job, not the
test's — but it does catch the scene machinery and the rebuild falling over.

## Status

Phases 0–4 done: lifecycle, the coordinate seam, the frame loop, the full ABI surface,
scene-authored acoustics, and the playground port.

Not yet verified by ear on hardware, and `BwaDynamicGeometry` needs the Steam Audio build to
attach — without it the node is inert and says so.
