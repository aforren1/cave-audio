# bw_audio for Godot

A GDExtension binding for the CAVE spatial audio engine. Godot is a **thin control
client**: no rendered audio crosses the boundary, and Godot's own `AudioServer` is
unused. The engine opens the audio device itself (ASIO for the array).

<!-- dev -->
<!-- Everything between dev markers is SOURCE-TREE material: it describes bindings/godot/
     in the engine repo, not the shipped addon. tools/godot/pack.ps1 strips these regions
     from the README it stages into the addon (and so from the `godot` branch and the
     release zip), where a demo project, CMake trees and ctest do not exist and a reader
     told to "press play" would rightly ask where the play button went. -->
This directory is both things at once:

- `addons/bw_audio/` - the drop-in addon, which contains the playground (the shipped
  by-ear demo). Copy the folder into your project to install.
- `project.godot` + `demo/` - a project that consumes it: the self-checking test scenes
  the ctest suite runs. Open this folder in Godot and run
  `addons/bw_audio/playground/playground.tscn` to hear it; the default main scene is the
  smoke test, which quits as soon as it has asserted.

The shortest thing to run first is `demo/minimal.tscn`: a click orbits the listener's head for
six seconds and the scene quits. Every other binding of this engine ships the same demo with the
same stimulus, so you can compare them by ear.

## Build

The extension is a CMake target in the main build, opt-in because fetching and
generating godot-cpp's bindings is a multi-minute first build:

```
cmake -S . -B build-godot -A x64 -DBWA_BUILD_GODOT=ON -DGODOTCPP_TARGET=editor
cmake --build build-godot --config RelWithDebInfo --target bwa_gdextension
```

The build writes `bw_audio_gd.windows.editor.x86_64.dll` into
`bindings/godot/addons/bw_audio/bin/` and copies `bw_audio.dll` beside it. That is the only
library to copy: Steam Audio links statically into the engine DLL. Then open
`bindings/godot/` in Godot.

`GODOTCPP_TARGET` is a **build-wide** choice, so one configure tree produces one library
flavor. It defaults to `editor` here. godot-cpp's own default is `template_debug`, which
builds fine and then does not load in the editor, so the addon would look broken. A shipping
build wants a second tree with `-DGODOTCPP_TARGET=template_release`.

Each of those trees compiles the whole engine as well, so a shipping build compiles it twice. Add
`-DBWA_ENGINE_SDK=/path/to/sdk` to link a prebuilt engine instead (see
[the engine SDK](../../docs/build.md#the-engine-sdk)); the extension then links that library and
its post-build step stages that exact file into the addon. `tools/godot/pack.ps1` takes the same
path through `-EngineSdk <dir>`, and CI always passes it, so the engine inside a released addon is
the one that platform's ctest run tested.
<!-- /dev -->

## Install

The addon has no store listing yet. Install it by hand: download `bw_audio-godot-<version>.zip`
from a GitHub Release and unzip it into your project, so you end up with `addons/bw_audio/`. You
can also copy `addons/bw_audio/` out of an archive of the `godot` branch. Then restart the editor.
A GDExtension loads on project open, so there is no plugin to enable.

Godot has **no equivalent of Unity's "add package from git URL"**. The documented routes are
a store listing and a manual zip extract. The editor has no install-from-URL or
install-from-file entry point. So unlike the Unity binding, you cannot install *from* the
`godot` distribution branch directly. The branch exists for a future store listing that pulls a
repo archive.

Four platforms: **Windows x64** for the desktop, where the engine's device path is ASIO,
**Linux x86_64** (JACK or ALSA), **macOS universal** (CoreAudio) and **Android arm64-v8a** for a standalone
headset. Each carries an editor library and an export library, except Android, which runs no
editor.

### Android

The addon carries `libbw_audio_gd.android.template_release.arm64.so` and the engine library
`libbw_audio.so` beside the Windows pair, and the manifest lists both under
`android.template_release.arm64` and `android.template_debug.arm64`. An Android export picks them
up with no extra step: add the Android export template, export, and the exporter copies both into
the APK.

What differs on a headset:

- Output is **stereo through AAudio**. There is no multichannel array transport on Android, so use the
  binaural profile. A wider request falls to the silent offline sink.
- The head pose comes from the **XR rig**, through the same listener push `BwaEngine` already does.
- Steam Audio is **inside the library**, statically linked, so binaural is the real HRTF decode and
  automatic occlusion, pathing and the reflection bed all work. It costs size: about 7 MB rather
  than 0.4 MB.
- `res://` paths are worse here than on the desktop: an exported APK holds them inside the `.pck`,
  where the engine's file loaders have no OS path to open. Stage layouts and audio into `user://`.

### Linux and macOS

Nothing to do: unzip the addon and open the project, the same as on Windows. The manifest names an
editor and a template_release library for each, so the extension loads in the editor too.

- **Linux** outputs through JACK (a PipeWire desktop answers the same ABI) or ALSA. Its engine
  library sits in `bin/linux/`, one level down from the others, because Android's has the same file
  name and one addon carries both. The extension finds it through an `$ORIGIN` run path, in the
  editor and in an exported game alike.
- **macOS** outputs through CoreAudio, on the system output device by default. The device runs
  at the engine's sample rate or the start fails, so the open sets the device's rate when the two
  differ. Its libraries are universal (x86_64 and arm64).
- The macOS libraries are **not signed or notarized**. Gatekeeper blocks a downloaded unsigned
  library, so clear the quarantine flag once after unzipping:
  `xattr -dr com.apple.quarantine addons/bw_audio`.

## Quickstart

### On the CAVE: the rig scene

1. Drag `addons/bw_audio/bwa_rig.tscn` into your scene, at the top of the tree. Instantiate
   Child Scene does the same.
2. Copy the rig's `trims.json` or `cave_layout.json` into the project and set the rig's
   **Layout Path** to it.
3. Move the `RoomOrigin` child to the CAVE floor center, on the floor. To reach it, right-click
   the rig and turn on Editable Children.
4. Rotate `RoomOrigin` 180 degrees about Y if the layout's front is the CAVE's back wall.
5. Set the rig's **Listener** to your camera, or turn **Feed Listener** off to let the engine
   read the tracker.

The rig is a `BwaEngine` on the Cave profile with its **Room Origin** set to that child. It is
an ordinary scene in your project, so edit it freely. The warning marker on the rig lists what
is still missing.

`RoomOrigin` is the room frame. Its blue arrow (+Z) is the layout's front, +z. Its red arrow
(+X) is the layout's +x, which is the room's left when you face the front. Godot calls a node's
-Z its forward. Ignore that here: the origin is a frame, not a facing.

The origin must match the tracker's frame, not the wall you call the front. Motive's +z often
points at the CAVE's back wall, and the measured layout inherits it. The symptom: sources come
from behind when they should be ahead. Rotate `RoomOrigin` 180 degrees. Do not move the camera.

Static `BwaAcousticGeometry` follows the tree after start (see "Geometry is collected, not
pushed"), so a child scene loaded later brings its geometry with it. The `BwaRoomBox` does not:
the engine reads it once, when it starts. Put the rig in each scene that has a room box, or put
the box in the rig.

An Autoload rig (Project Settings > Globals) works for any content with no room box of its own.
An Autoload starts before the scene exists, so a `BwaRoomBox` that arrives later logs an error
and is ignored. If you use one:

- Autoload a configured copy saved outside `addons/`, so an addon update cannot overwrite it.
- Keep `BwaEngine` out of the scenes.
- Set the listener from the scene, with the Autoload's name. A NodePath cannot reach into a
  scene that is not loaded yet.

```gdscript
BwaRig.listener = $Camera3D.get_path()
```

Only one `BwaEngine` runs. A second one that enters the tree is refused: it logs an error and
stays inert, and the first keeps running with its own layout and room origin. Nothing is handed
over.

`bwa_rig.tscn` carries a fixed `uid`, so a scene that instances it keeps its reference across
addon updates.

### At a desk: the smallest scene

The smallest scene that makes sound. Build this tree:

```
Main (Node3D)
  BwaEngine    profile = Binaural, listener = ../Listener
  Listener     a Node3D at (0, 1.5, 0), or your Camera3D
  BwaEmitter   clip = "res://click.wav", loop = on, at (0, 1.5, -2)
```

Press play. `BwaEmitter.autoplay` is on by default, so the clip starts when the emitter is ready,
2 m in front of the listener (a Godot node faces -Z). Move the emitter and the sound follows:
`BwaEngine` pushes every source and the listener each frame.

Keep `BwaEngine` ABOVE every source in the tree. Godot runs siblings' `_ready` top to bottom, the
engine starts in its own `_ready`, and a source that is ready first finds no running engine and
stays silent (it warns). To start a sound from a script instead, turn `autoplay` off and call:

```gdscript
$BwaEmitter.play_clip("res://click.wav")
```

`res://` paths work in the editor only; see "Asset paths" below before you export.
`demo/minimal.tscn` in the repository is this scene with a synthesized click orbiting the head.

<!-- dev -->
## Distribution

`tools/godot/pack.ps1` builds **both** library flavors, writes the release zip, and leaves a
staged addon tree at `build/godot/addon`:

```
powershell -File tools/godot/pack.ps1
```

`tools/dist/publish-branch.ps1` then pushes that staged tree to the **`godot` branch**, and
CI runs it on a `v*` tag. The branch's root is `addons/bw_audio/`, and that layout is not
cosmetic. **The Asset Library downloads a repo archive at a specific commit**, not a release
asset, so the binaries have to be committed somewhere. Keeping them on their own branch
leaves `main` source-only and its existing "no binaries in the tree" rule intact.

Publishes are ordinary commits, never a force-push. An Asset Library entry pins a commit
hash, and rewriting history would eventually orphan a published one. The standing cost is
that the branch grows by roughly the addon's size per release; that is inherent to in-repo
distribution.

A release also attaches the zip beside its other assets, which
[docs/build.md](../../docs/build.md#continuous-integration) lists. Until a store listing exists,
that zip is the install route.

### Submitting a store listing

The Asset Library has moved to the **[Asset Store](https://store.godotengine.org/)** (beta).
Its guidelines matter to us in three specific ways:

- **Precompiled binaries in a GDExtension are explicitly allowed** - with the condition that
  *every shared library named in the `.gdextension` must be present*. `pack.ps1` parses the
  manifest's `[libraries]` section and refuses to pack if any path is missing, so the build
  enforces that rule instead of a reviewer discovering it.
- **`windows.template_debug` points at the release binary on purpose.** godot-cpp's
  template_debug differs only in its own internal checks, so building a third flavor would
  triple the godot-cpp cost to debug code that is not the code under debug. But simply
  *listing* a flavor we do not build would be an automatic rejection.
- **The thumbnail must be 16:9**, where the old library wanted a square icon. Both are
  committed: `examples/icons/bw_audio.png` (256×256) and `bw_audio_thumb.png` (1280×720).

Whether the store hosts an uploaded zip or pulls a repo archive is not documented on its
public pages, and it is in beta. So both are covered: the release zip is the upload, and the
`godot` branch is the archive. Whichever it turns out to want, nothing needs rebuilding.

Edit the listing by hand for each release; there is no API. If it pulls a commit, use the
hash the publish step printed (`published <sha> -> godot`). Icon URLs must be
`raw.githubusercontent.com`, for example
`https://raw.githubusercontent.com/aforren1/cave-audio/main/examples/icons/bw_audio.png`.
License is GPL-3.0, and the LICENSE inside the addon carries the copyright holder and year
the guidelines require. Submissions are human-reviewed.

Both flavors are packed because either alone fails in a way that only shows up late. Without
`editor` the addon does nothing when you open the project, and without `template_release` it
works right up until someone exports. The script refuses to pack if either is missing, rather
than shipping a manifest that promises libraries it does not carry.

One thing it stages deliberately: it copies into a **clean** directory, because
`addons/bw_audio/bin/` is gitignored build output, and anything that packs the working tree
naively yields an addon with no binaries. The manifest's `[dependencies]` names
`bw_audio.dll` so Godot's **exporter** carries it. Nothing in Godot references the engine
library directly, because it is an import of the extension, and without that entry an
exported game ships the extension alone and fails to load it, after working perfectly in
the editor.

A with-SDK build and a no-SDK build stage exactly the same files, because Steam Audio links
statically into `bw_audio.dll`. A no-SDK build is supported (ISM + FDN + manual occlusion);
it just loses the HRTF monitor and the ray-traced acoustics.

### godot-cpp version

Pinned to a commit (`BWA_GODOT_CPP_COMMIT` in `CMakeLists.txt`), not a branch. godot-cpp's
release branches lag the engine (4.5 is the newest cut), while master already ships
`extension_api.json` for 4.7, which is what the installed editor is. When the editor
moves, bump the commit and check that api file's header still matches.
<!-- /dev -->

## Coordinate seam

The engine works in **room space: right-handed, +Y up, +Z forward, meters, origin on the
floor** - OptiTrack/Motive's default streamed frame.

Godot is **also right-handed and Y-up**, so unlike Unity there is **no mirror**: positions
and directions pass through the CAVE registration transform and nothing else. The one
difference is a facing convention. Godot's `Vector3.FORWARD` is `-Z` and room's identity
quaternion faces `+Z`, so orientations pick up a 180° yaw:

```gdscript
room_pos  = registration * node.global_position
room_dir  = registration.basis * dir
room_quat = Quaternion(registration.basis * node.global_basis) * Quaternion(Vector3.UP, PI)
```

Two consequences worth knowing, both places Unity's advice does *not* carry over:

- `bwa_bed_set_orientation` takes a room-frame yaw, and because there is no mirror the
  **sense of rotation is preserved**. Unity's binding has to reverse it; this one does not.
  `BwaBed.orientation` is in **scene** terms: yaw 0 points the field's front along Godot world
  +Z, which is **behind** a default Godot node (its forward is -Z), and the binding adds the
  registration. So `orientation.x = cam.rotation.y` points the front at the camera's back. To
  face the field where a node faces, call `set_yaw_from_basis(node.global_basis)`. The bed is
  world-locked, so a moved `room_origin` turns it with the scene. Pitch and roll are about the
  field's own axes and pass through unchanged, which is exact under a level origin.
- **Changed:** with no origin, `orientation` is the room-frame value it always was. Under a
  turned origin, earlier releases pushed it raw, in room terms. Under a 180 degree Motive
  origin, set the yaw to `PI` to keep the old front.
- Positions need no axis flip at all. If a source ends up mirrored, the registration
  transform is wrong - not the handedness conversion.

Budget time to verify with a known-position test source before trusting anything else.

### Room origin

`registration` is the transform from Godot world to room space. Set it from code, or point
`BwaEngine.room_origin` at a Node3D and the engine derives it:

```gdscript
registration = origin.global_transform.affine_inverse()   # scale removed first
```

The origin's local axes become the room axes. A worked case, with the origin at `(2, 0, 1)` and
a source at `(5, 0.5, 3)`:

| Origin yaw about Y | Room position of the source |
| --- | --- |
| 0 degrees | `(3, 0.5, 2)` |
| +90 degrees | `(-2, 0.5, 3)` |
| 180 degrees | `(-3, 0.5, -2)` |

What to know:

- The engine compares the origin's global transform once per frame, before it pushes the
  sources and the listener, and recomputes only when it changed. A moved origin takes effect
  on the next frame. `BwaSpeakerView` follows it, in world space, under any parent.
- The static mesh (`BwaAcousticGeometry` and the `BwaRoomBox` walls), the ground plane and
  `BwaDynamicGeometry` follow a change: the engine re-bakes the static mesh in the same frame.
  That is a BVH rebuild, so do not animate the origin in a scene with static geometry.
- Three things do not follow: `set_extra_listeners` positions and the FDN decay direction,
  which are converted when you call them, and the room box's image-source reflections, which
  the engine anchors at the room origin by size alone. With any of them present, the first
  registration change after start warns once, and `is_registration_stale()` reads true from
  then on. This covers a moved origin, a `room_origin` set or re-pointed after start, and a
  write to `registration`. Call `set_extra_listeners` again after a change.
- Scale is ignored, the origin's own and any parent's. Room meters are Godot meters. The engine
  warns once.
- A mirrored origin is refused. A negative scale on the origin or on a parent makes its
  transform a reflection, which would mirror every position. The registration keeps its last
  value and the engine warns once, with the node's path. A flattened axis and a non-finite
  transform get the same treatment.
- Keep the origin level. The engine warns once when it is tilted more than 1 degree.
- While `room_origin` points at a node, that node owns the registration. A write to
  `registration` is refused with a warning.
- With `room_origin` empty, nothing changes: `registration` is identity until your code sets it.
  Clear `room_origin` at run time and the registration keeps its last value. The same happens
  when the origin node is freed, and the engine warns once.

## Channel count

The engine's channel count is **the layout's speaker count** (4 to 64, `BWA_MAX_CHANNELS`),
not a constant. Read it with `BwaEngine.get_channel_count()` and size any meter or
speaker-gizmo array from it. Never hard-code a speaker count.

The trap: a failed layout load is **not** fatal at create. The core falls back to the
default grid (26 speakers, `BWA_DEFAULT_GRID`) and only records the reason. `BwaEngine` surfaces that as a
warning, and `bwa_start` refuses the fallback when a path was given, so a bad layout fails
loudly instead of quietly changing the channel count.

Binaural is the exception. It drives no speakers, so the layout only places the virtual speakers
the diffuse bed decodes through. When `profile` is Binaural and the file at `layout_path` does
not exist, `BwaEngine` prints one line and runs the default grid. A file that exists still has
to load. Cave, CaveSim and CaveBoth refuse to start without the file.

## Asset paths

The core loads audio and layout files itself, by OS path. It never goes through Godot's
virtual filesystem. `res://` and `user://` paths are globalized for you, and an absolute path
is used as it is, so a clip your code generates can be played by its full path. In an
**exported** build
`res://` lives inside the `.pck` and that path will not exist. Ship those files beside the
executable, or stage them into `user://` at startup. (Same trap as Unity's
`StreamingAssets`, one layer down.)

## The frame

**One node pushes everything.** Each `_process`, `BwaEngine` pulls every registered
emitter's transform, pushes the listener pose, then calls `bwa_commit` once. Emitters
deliberately do *not* push themselves. The commit is what defines frame coherence, so
everything it covers has to be sampled together. Otherwise you can commit a frame where the
listener moved but some sources had not.

Godot has no `LateUpdate`, so freshness comes from `process_priority` instead.
`BwaEngine` defaults to `1000`, so it runs after ordinary gameplay nodes. Coherence does
not depend on that: if a source moves after we sampled it, every source *and* the listener
are one frame old together, which is inaudible. Only freshness is at stake.

## Nodes

There is no separate 1:1 P/Invoke-style layer. GDExtension needs no such shim, so every
call lives as a method on whichever class owns the handle it operates on.

**`BwaEngine : Node`** - everything engine-global: lifecycle, the registration transform,
listener and tracker wiring, the asset cache, master gain/pause and mix groups, the reverb
configuration (Steam bed *or* FDN), materials and scene geometry, the clock, the
diagnostics readbacks, and the live A/B knobs (panner, dual-band and its CAP, SPCAP focus
and density, spread mode, decorrelation, near-spread, hole spread, speed of sound, max-rE
and its band split, bed renderer, tracked room EQ, tracked directivity, tracked alignment,
master gain, limiter). Those are exported properties that also apply live, so the inspector
*is* the A/B tool. `apply_setup(SETUP_SEATED)` or `apply_setup(SETUP_ROAMING)` sets them all
from one engine preset; `get_setup_tuning()` returns a preset as a Dictionary without applying
it, and `get_live_tuning()` reads back what the engine runs.

**`BwaSource : Node3D`** (abstract) - everything a spatial voice can do regardless of where
its audio comes from: gain, priority, group, fades, pause, spread/extent/size, Doppler, air
absorption, loudness compensation, attenuation override, occlusion (ray-traced or manual),
directivity, reverb sends, early reflections, pathing, and `set_channel()`, the direct
output-channel route. The settings among those are also
readable and writable as one Dictionary: `get_desc()`, `apply_desc()`, `reset_to_preset()`,
and the static `BwaSource.get_preset()`. What a source is DOING stays out of that Dictionary:
fades, pause, and the per-frame manual occlusion level are calls, not configuration.

**`BwaEmitter : BwaSource`** - plays a file. Adds `play`/`play_at`/`play_loop`/`stop_at`,
gapless `queue`, `seek_frames`/`seek_seconds`, `set_region_frames`/`set_region_seconds`,
`pitch`, `streaming` (play a long file from disk instead of RAM), `async_load` with
`is_loading()`, and the `finished` and `looped` signals.

**`BwaPushSource : BwaSource`** - you feed it PCM. Adds `push`/`push_space`/`push_end`.

**`BwaBed : Node`** - a world-locked ambisonic soundfield. A `Node`, not a `Node3D`: a bed
has no position, only an orientation. It takes the same `async_load` opt-in as `BwaEmitter`,
which is where a soundfield usually wants it: 4 to 16 channels of long recording. A bed is a
voice, so it carries the emitter's playback surface too: `play`/`play_at`/`play_loop`/
`stop_at`, `seek_frames`/`seek_seconds`, `set_region_frames`/`set_region_seconds`, and the
`finished` and `looped` signals. It does not carry the spatial calls, because a bed is
world-locked and has no position.

**`BwaMaterial : Resource`** - an acoustic material as a `.tres`. Either a built-in preset
or custom 3-band coefficients. The preset is an **enum, not a string**: the core answers an
unknown material name with the generic default and a note in `bwa_last_error`, which is not
an error, just a wrong sound. The binding mints tokens lazily and caches them against the
engine's generation, because the core's material table is fixed-capacity and meant to be
filled once.

**`BwaAcousticGeometry : Node3D`** - static occluding/reflecting geometry, from a `Mesh` or
an existing `MeshInstance3D`. **`BwaDynamicGeometry : Node3D`** - a movable occluder (a
cheap BVH refit, not a rebuild; needs the Steam Audio build). **`BwaRoomBox : Node3D`** -
the shoebox, which also captures the room for the image-source early reflections and so
matters even in a phonon-free build. Its outdoor degenerate lives on `BwaEngine` as the
`ground_*` properties: one horizontal mirror plane, the ground bounce. `ground_pressure_release`
turns that plane into a water surface seen from below (the Lloyd's-mirror comb). A
`BwaRoomBox` wins when both exist - one room at a time.

**`BwaSpeakerView : Node3D`** - one gizmo per speaker, lit by that channel's live output
level. It reads the geometry back from the engine, so it draws the array the engine is
*actually* panning with: a layout that failed to load and fell back to the default grid
looks wrong immediately instead of sounding wrong later.

### Geometry is collected, not pushed

`bwa_scene_set_mesh_mat` **replaces** the whole static mesh, and `bwa_scene_set_box` is a
convenience that calls it. Per-node calls would therefore clobber each other. A scene with
a room box and a pillar would lose one of them, silently, and the loser would simply be
inaudible.

So every static piece registers in `_enter_tree` (which Godot runs top-down, before any
`_ready`) and `BwaEngine` merges them into **one** call before `bwa_start`. The room box
takes two steps. `bwa_scene_set_ism_room` records the shoebox for the image-source
reflections without touching the static mesh, then the box's own 12 inward-facing wall
triangles join the merge like any other occluder. Otherwise the walls would vanish the
instant any other occluder existed. (`bwa_scene_set_box` still does both at once, for the
case where the box **is** the whole scene.)

After start the merge keeps following the tree:

- A `BwaAcousticGeometry` that enters joins, and one that leaves or is hidden drops out.
  Hidden means not occluding, the way a disabled object is in Unity, and hiding a parent takes
  its subtree out.
- `BwaEngine` re-merges at most once per frame, in its `_process`, and only when the visible
  set differs from the one it last baked. A hide and show in one frame cost nothing. When the
  last piece goes, it clears the static mesh.
- Each re-merge copies every static mesh and rebuilds the BVH on the engine's sim thread.
  That suits a door or a wall that appears. Give a prop that toggles often a
  `BwaDynamicGeometry`. `get_static_rebuilds()` counts rebuilds, the start bake included.
- Moving a static piece, or changing its `mesh`, `source` or `material`, does not re-merge
  until the next rebuild, when the piece jumps. Use `BwaDynamicGeometry` for anything that
  moves.
- The room box is read once, at start. The image-source room can be set but never removed, so
  a box that arrived late could not leave again. A late `BwaRoomBox` logs an error and is
  ignored.
- A merge replaces a mesh you set with `scene_set_mesh`. Use one or the other.

### Warnings for the settings that fail quietly

Godot's `_get_configuration_warnings()` gives the scene tree a warning marker, which is
exactly where the engine's *survivable* mistakes belong. None of these are errors to the
core. It starts, it renders, it just sounds wrong:

- both reverb beds enabled, contending for the one tap;
- source early reflections alongside the Steam reflection bed, which already contains
  early reflections, so they render twice;
- `feed_listener` on with no listener node, so the listener never leaves the origin;
- a `layout_path` that does not exist (under Binaural the marker says the default grid runs), or
  a `res://` one that will not survive export;
- an array profile (Cave, CaveSim, CaveBoth) with no `layout_path`, which renders the built-in
  26-speaker grid and not your room;
- a `room_origin` that is not a Node3D, is mirrored, is scaled, or is tilted more than
  1 degree. The marker follows the origin as you move it in the editor;
- ray-traced occlusion or pathing switched on without the SDK or without geometry;
- `profile` set to Cave on a machine with no ASIO driver, which renders the speaker array
  into nothing and is silent by design;
- a `fallback_profile` on a machine that does not list the primary device: a run from the
  editor here takes the fallback (this one replaces the line above);
- a `fallback_profile` on an array profile with an empty `device`, where the check cannot tell
  a desk from the rig.

The split into three source classes is deliberate. The core genuinely *refuses*
play/seek/pitch on a push voice, so a single node with a mode flag would leave those
visible in the inspector and silently inert. That is the exact class of quiet failure this
binding tries to make unrepresentable.

### One speaker, no panning

`set_channel(n)` sends a source out of exactly one output channel with no spatial processing,
and `BwaSource.CHANNEL_AUTO` puts it back on the panner. That is the psychophysics
ground-truth condition: a real speaker to A/B a phantom against, played with whatever content
you like. It is not `BwaEngine.set_test_signal()`, which injects a built-in tone after the
per-speaker align stage and is therefore not level-comparable with a rendered source.

The routed voice keeps the whole output stage a panned voice gets - align trims and delays,
room EQ, master gain, limiter - and the route ramps in and out instead of clicking. Everything
distance- or direction-derived is suppressed while it is on (attenuation, spread, occlusion,
the reverb and reflection sends, Doppler, air absorption) and takes effect again the moment you
go back to `CHANNEL_AUTO`. Pitch and pause still apply. Mono point sources only.

It is a method, not an exported property: a route is a run-time experimental condition, not
scene configuration, so it is not serialized. It IS replayed if the source is re-created.

A channel outside `0 .. BwaEngine.get_channel_count() - 1` is refused with a warning and the
source keeps the route it had, so `get_channel()` never reports a speaker the voice is not on.
`CHANNEL_AUTO` is the only negative that means anything, so every other negative is refused too,
and refused with no engine at all: a negative needs no channel count to judge.

The same rule runs through the transport calls. Every time-valued argument (`play_at`,
`play_loop`, `stop_at`, `seek_frames`, `set_region_frames` and their seconds twins) reaches the
engine unsigned, so a negative does not fail. It becomes an enormous positive: -1 frames is
1.8e19, about twelve million years of dsp clock, which schedules a start for never and leaves a
voice that reads as playing and stays silent. All of them refuse a negative with a warning.

### Ends and loop boundaries are events

`finished` and `looped` come from the engine's own event rings, not from watching a playing
flag. Watching the flag cannot work: `bwa_source_is_playing` is a per-block republish, so a
clip shorter than a frame may never once read as playing, and a looping voice never ends at
all. `BwaEngine` drains `bwa_poll_ended` and `bwa_poll_looped` once per `_process`, right
after the commit that fills them, and routes each handle to the node that owns it.

`BwaEngine` is the ONLY caller of either drain. Both are engine-wide and destructive, so a
second caller would eat events belonging to other nodes and their signals would never fire.
`BwaEngine.get_ended_this_frame()` and `get_looped_this_frame()` hand you **this frame's batch** rather than
draining again, so reading them costs nothing and misses nothing.
`get_ended_events_dropped()` and `get_loop_events_dropped()` carry the running totals of what
the engine dropped because nothing read it in time. The ended total should stay 0. The loop
total can rise for a harmless reason: a loop region shorter than a frame wraps more often than
any frame rate reads it, so pace trials off the wraps you receive.

`looped` fires once per WRAP, not once per frame.

`finished` means the sound **ran out** - a non-loop end, a drained queue, a play region's end.
An explicit `stop()` or `fade_out()` never fires it; `stop_at()` deliberately does, because a
scheduled stop is an arranged ending and the caller wants to know when it landed. The engine
posts no event for any halt, so `stop_at` rides a narrow is-playing edge instead, read after
the drain. A one-shot latch keeps a halt and a completion that describe the same end from both
being reported.

`bwa_source_play` only **enqueues**, so for a frame or two after a play the raw readback
honestly says "not playing". `BwaEmitter.is_playing()` absorbs that window by counting a
just-issued play as playing. If a play is dropped or its voice stolen at onset the engine
posts nothing at all, and the node drops that claim after a few frames without inventing a
`finished` for a sound that never played.

`bwa_set_output_capture` is **not bound, on purpose.** Its callback runs on the audio
thread, where calling into GDScript would allocate and take the interpreter lock - exactly
what invariant 1 forbids. Use the MANUAL sink and `render_block()` for capture instead.

### Assets: the engine owns the cache

The core holds a by-path, reference-counted asset cache, keyed on the **path plus the load
flags**. A file kept in RAM and the same file streamed are two assets, which is why the key
carries the flags. Nodes acquire through the engine node, so the same clip on twenty emitters
loads once.

`preload_sound(path, flags)` warms it before the first play. `flags` is a `BwaEngine`
bitfield: `LOAD_MEMORY` (0, the default), `LOAD_STREAM`, `LOAD_AMBIX`, `LOAD_FUMA`. The core
refuses a combination no loader can express, such as AmbiX with FuMa.

`unload_sound_path(path)` drops this node's reference to **every** form of the path. Other
holders keep theirs, so nothing is pulled out from under a playing voice.

`sound_get_frames(path)` and `sound_get_channels(path)` are **cached-only** on purpose. They
ask the core through `bwa_sound_find`, a pure lookup that never loads and never takes a
reference, so a path the engine has not loaded reports 0 instead of decoding as a side effect.

`BwaEngine` still records the `(path, flags)` keys it acquired, but only to know which
references are **its own**: `unload_sound_path` releases exactly those, and `sound_is_ready`
answers only for them. That record is the last asset state left in the binding. The
deduplication, the reference counting, and the lifetime are the engine's.

### Loading late, without stalling the frame

`async_load` on `BwaEmitter` and `BwaBed` is an **opt-in**, and off by default: the CAVE's
normal path is load-time and synchronous. Turn it on for content that appears mid-session.

A play against a still-decoding clip is held on the control thread. The voice binds, stays
silent, and starts from the top of the clip on the block the data lands, so nothing clicks and
no frames are skipped. `is_loading()` covers that window. `finished` is not fired in it: a
sound that has not started cannot have ended.

`BwaEngine.preload_sound_async(path, flags)` starts a decode without a player, and
`sound_is_ready(path, flags)` reports the landing. False covers three cases, so do not poll it
without knowing which: still decoding, the decode failed, or this engine never acquired that
`(path, flags)` pair at all. Only the failure reports itself, through `get_last_error()`.

Two calls refuse a not-ready handle rather than hold it, so the binding always loads them
synchronously: `play_oneshot`, which owns no handle to start later, and `BwaEmitter.queue`,
whose entry resolves at bind time.

### Configuring a source in one value

Twenty-plus setters describe a source. The per-property getters report what the simulation is
currently doing, not what you asked for. `get_desc()` returns the whole configuration as a
Dictionary, so you can print it, diff two sources, and
find the one that sounds wrong:

```gdscript
print(emitter.get_desc())
emitter.apply_desc({"gain": 0.5, "occlusion": true})   # only the keys you pass change
emitter.reset_to_preset(BwaSource.KIND_PROP)           # back to a clean configuration
print(BwaSource.get_preset(BwaSource.KIND_AMBIENCE))   # static: no engine needed
```

The kinds are `KIND_DEFAULT`, `KIND_PROP`, `KIND_VOICE`, `KIND_AMBIENCE`, and `KIND_UI`. They
name what a source **is**. Nothing in the table is measured: a kind differs from the default
only where [docs/api.md](../../docs/api.md) argues the case, and every other field sits at the
engine default.

Position, orientation, and playback state are deliberately **out**. Position and orientation
are per-frame and commit-gated, so they belong to the frame loop. Playback is what a source is
doing, not what it is, and an apply must never restart a sound. The manual occlusion level is
out for the same reason: it is a measurement the game publishes each frame.

Applying a desc updates the node's own properties too, so the inspector never disagrees with
what the engine is rendering.

### Picking the profile, and the device

`profile` is the highest-stakes property on the node, so the inspector spells out what each
value does rather than just naming it. The short version: **Binaural** is the direct
headphone render (the default, and what you want at a desk), **CaveSim** auditions the
speaker array over those same headphones, **Cave** drives the rig and nothing else. On a
machine with no rig, Cave is correctly, deliberately silent. [docs/api.md](../../docs/api.md)
has the full "pick by question, not habit" table.

Whatever you pick, `get_audio_backend()` reports what actually happened, decode included:

```gdscript
print($BwaEngine.get_audio_backend())   # asio:RME Digiface Dante (steam HRTF direct)
```

To fill a device picker, ask a backend for its devices in one call. `get_devices(backend)` is
static, needs no engine, and returns the exact strings the `device` property accepts (empty picks
the first usable device):

```gdscript
for name in BwaEngine.get_devices(BwaEngine.SINK_WASAPI):
    print(name)
```

Prefer the id (`get_device_id(backend, i)`) when you persist a choice: friendly names collide
between a headset and its dock. The ASIO-only spelling (`get_asio_drivers()`,
`get_asio_driver_count()`, `get_asio_driver_name(i)`) stays as a wrapper.

### Test a rig scene at your desk

A scene set up for the CAVE names the rig's device. At a desk that device is missing, and the
scene is silent (`sink` Auto) or fails to start (`sink` ASIO). Do not edit the rig settings. Set
the fallback instead, in the inspector's **Device Fallback** group:

1. Keep `profile`, `sink` and `device` as the rig needs them. Put the rig's device name in
   `device` (a string from `BwaEngine.get_devices(BwaEngine.SINK_ASIO)`, read on the rig).
2. Set `fallback_profile` to CaveSim or Binaural. Leave `fallback_sink` on Auto and
   `fallback_device` empty to get the machine's default stereo output.
3. When the device is not listed on this machine, the node's warning marker says that a run
   from the editor here takes the fallback.
4. Run the scene from the editor. The engine pushes one `DESK FALLBACK` warning and
   `is_using_fallback()` is true. `get_active_profile()` is the profile that runs.

On the rig the device is listed and nothing changes.

- **CaveSim** plays the array as virtual speakers on headphones. It uses the same layout file and
  keeps the room modeling (speaker distance, delay and directivity), so it needs the layout.
- **Binaural** is the direct headphone render. It skips the per-speaker stage (trims, delays,
  correction filters, room EQ) by design, and it starts without the layout file.
- The primary's `sink_flags` do not apply to the fallback. It has `fallback_sink_flags`.
- The decision is made once, in `_ready`. There is no second try after a failed start.

`BwaEngine.is_device_present(profile, sink, device)` is the check. It is the engine's
`bwa_device_present`, so it uses the same rule Auto uses at start. What the check sees, and what it does not:

- It reads the device lists, before the engine exists. It does not open anything.
- ASIO, WASAPI, CoreAudio: the device is present when the backend lists that exact name or id.
  With no name, the backend must list at least one device.
- Auto: the same, for each backend Auto offers that profile. Auto never gives the array to
  WASAPI, so a WASAPI endpoint does not count for Cave.
- JACK, ALSA or AAudio named as the sink, with a device: present on a platform that has that
  backend. Those backends open strings their list never held (a JACK port pattern, `hw:1,0`), so
  the list cannot call one missing. On a platform without the backend (an ALSA scene on Windows)
  the device is missing.
- CaveBoth: only the array's device is checked, not the monitor's.
- Null and Manual: always present.
- It cannot see whether a listed device will open. An ASIO driver is listed when it is
  installed, so a laptop with the RME driver and no Digiface reads as present. ALSA and CoreAudio
  list a default device on every machine. Too few channels, a wrong rate, or a busy device read
  as present too.
- A stereo profile under Auto with an empty `device` needs a Windows output device. An installed
  ASIO driver alone does not count there.
- Windows lists output devices for the session a process runs in. An editor or player started
  outside the interactive desktop (from a remote shell, a service or a scheduled task) can list
  none, likely for that reason, and then reads as missing on a machine that has outputs. The
  start line in the log names the device that opened.
- A listed device that does not open is not a fallback. Under a named sink the start fails.
  Under Auto the engine runs the silent offline sink, and the binding logs an error that says
  the fallback did not trigger and why. Set the profile by hand on that machine.
- An array profile with an empty `device` only asks "is any driver installed". That cannot tell
  a desk from the rig, and the warning marker says so. Name the rig's device.
- The fallback can end silent too. If its sink is Auto and nothing opens (no output on this
  machine, or a fallback device name that nothing lists), the engine pushes a second error,
  `DESK FALLBACK has NO AUDIO either`, with the reason. Connect headphones or another output, or
  pick a fallback device this machine has. A fallback on the Null sink is silent on purpose and
  logs no error.

Where the fallback may run:

- `fallback_only_in_editor` is on by default: a run from the editor only. An export behaves as
  if no fallback were set. That includes a debug export. "Export With Debug" is on by default in
  Godot, so a rig often runs a debug build, and the gate does not open for it.
- Where it may run, a missing device is no longer a failed start. A typo in `device`, or a
  driver that is not installed, becomes a headphone fallback with one warning.
- Turning the toggle off allows the fallback in every build, with the same trade. Do not ship
  that to the rig.

For the headphone profiles, `load_headphone_eq(path)` loads an AutoEq `ParametricEQ.txt` as a
correction EQ on the stereo output. Call it after the engine starts. `""` clears it.
`set_headphone_eq(on)` switches the EQ without a click. It is on by default.

### Is the device being starved?

`get_health()` returns a Dictionary - `measured`, `blocks`, `xruns`, `dropped_frames`,
`driver_resyncs`, `late_blocks`, `stream_starves`, `peak_load` - and `get_xruns()` is the one-line
form for a HUD. `xruns` is the device running on without us; `late_blocks` is our own render
overrunning the block period, which is what causes them; `stream_starves` is a streamed voice whose
ring ran dry. Read any of them against `blocks`.

```gdscript
var h := $BwaEngine.get_health()
if not h["measured"]:
    print("this driver cannot report dropouts - a zero xrun count proves nothing")
elif h["xruns"] > 0:
    print("%d dropouts over %d blocks, peak load %.2f" % [h["xruns"], h["blocks"], h["peak_load"]])
```

`measured` is the field that matters. It is false whenever nothing here can see a dropout: before
start, on the manual sink (no deadline, so it cannot miss one), or on a driver that never stamps a
valid sample position. In all of those, `xruns` reads 0. A zero that means "none" and a zero that
means "never looked" are different answers.

### Two returns worth checking

`play_oneshot()` returns **whether the one-shot was accepted**. A one-shot holds no handle,
so that boolean is the only signal it will ever give you. False means the clip failed to
load, or that the voice pool or command ring was momentarily full and the engine dropped the
transient. One-shots never steal, so spam cannot evict your named sources. `get_last_error()`
says which. The load failure also pushes an error; the drop deliberately does not, because
the thing that causes it is one-shot spam and a per-drop message would bury the console.

`get_output_latency_frames()` and `get_output_latency_seconds()` are named for their unit on
purpose. Godot's own `AudioServer.get_output_latency()` returns **seconds**, so a bare
`get_output_latency()` here returning frames reads as seconds to anyone who knows that call.
A device-less sink reporting 0 then hides the mistake indefinitely, because 0 is 0 in either
unit. Neither name here collides.

`seek_frames()` / `seek_seconds()` on `BwaEmitter` and `BwaBed` follow the same rule, for the
same reason. `AudioStreamPlayer3D.seek()` takes seconds, so a bare `seek()` taking frames is a
trap: passing `1.5` lands on frame 1 and the clip just restarts. The engine's own unit is
frames everywhere (`play_at`, `stop_at`, `play_loop`, `get_playhead_frames`) because that is what
the dsp clock counts; the `_seconds` twins are conveniences over the resolved sample rate.

`get_dsp_time_frames()` / `get_dsp_time_seconds()` complete the set. The clock is frames, and every
host's dsp-time call is seconds: Unity's `AudioSettings.dspTime` is a seconds `double`, and Godot's
own `AudioServer` times are seconds too. Schedule with the frame value. It is the exact one, and it
is what `play_at` and `stop_at` take.

`BwaEngine.get_host_time_ns()` is static and follows the same rule. It reads the engine's own host
clock, the one `get_clock()`'s `host_time_ns` is stamped with, so you can measure the offset from
any clock of yours: read yours, call this, read yours again, and the offset is the middle value
minus the mean of the two, with the error bounded by half the span. It needs no engine, so it
answers before `BwaEngine` enters the tree.

The rule behind all four, if you are adding a call: **a unit belongs in the name when the quantity
has two live units in this engine.** Time does - frames and seconds are both real here - so every
time-valued name says which. Nothing else does: distances are meters, frequencies Hz, angles
radians, gains linear (a decibel value would have to say `_db`), and suffixing those would add
noise without removing a decision.

<!-- dev -->
## Tests

```
ctest --test-dir build-godot -C RelWithDebInfo -R godot_
```

- **`godot_room`** - the coordinate seam's property test. It asserts the *property* (a
  node's Godot forward equals the converted quaternion's room facing) rather than
  restating the conversion, over cardinal and random orientations. Pure float math, no
  godot-cpp, so it always builds.
- **`godot_smoke`**, **`godot_frame`**, **`godot_rig`**, **`godot_follow`**, **`godot_fallback`**, **`godot_minimal`**, **`godot_api_manual`**,
  **`godot_api_null`**, **`godot_scene`**, **`godot_scene_no_room`**, **`godot_playground`** - the demo scenes and
  the playground self-test, self-checking and headless. These need an editor binary, which is
  not a build dependency, so point CMake at one:
  `-DBWA_GODOT_EXE=".../Godot_v4.7.1-stable_mono_win64_console.exe"`. Without it they are
  simply not registered.

All of these **do** run in CI, unlike the three imgui tools: Godot's `--headless` needs no
display or GL. CI passes `--no-tests=error` so an empty selection fails rather than passing.
Otherwise a bad filter, a missing `BWA_GODOT_EXE` and a stale build cache all look identical
to success.

Every scene test rides a **`godot_import` fixture** (`test/godot_import.cmake`), and it is
load-bearing. A runtime scene launch loads only the GDExtensions named in
`.godot/extension_list.cfg`, and only the editor's **import scan** writes that file. `.godot/`
is gitignored, so on a fresh checkout - every CI run, every new clone - there is no list, no
`Bwa*` classes, and each scene wedges on parse errors for its full timeout. The fixture runs
`--headless --import` first and then checks the *postcondition* (the list exists and names
`bw_audio.gdextension`) rather than Godot's exit code, which is nonzero even on a healthy
project. ctest schedules it automatically for any selected dependent, so `-R` filters keep
working.

`api.tscn` drives the whole bound surface, and it runs **twice, on both sinks**, because
either alone leaves a hole:

- **MANUAL** has no device and no audio thread, so blocks advance only when the scene pumps
  `render_block()`. Playheads become functions of how many blocks were requested rather
  than of machine speed, which is what lets `seek_frames(24000)` landing and a paused playhead
  freezing be assertions instead of coin flips. But nothing there is concurrent: one thread
  on the command ring, one thread on both ends of the push ring, no voice retiring under a
  live mixer. That is not what ships.
- **NULL** puts the audio thread back - the production topology minus the device - and the
  scene loosens its timing assertions to what a wall clock can honestly promise.

Both check what a binding can actually get wrong: that every call reaches the core with its
arguments intact, that handles survive, and that readbacks come back shaped right. Whether
the DSP is *correct* is the engine's own suite's job.

`rig.tscn` instances the shipped `addons/bw_audio/bwa_rig.tscn` and pins `room_origin` against
hand-worked literals: the +90 degree and 180 degree rows of the table under "Room origin". The
90 degree row is the one that catches a missing inverse. At 180 degrees a rotation is its own
inverse, so that row alone cannot. A pitched origin covers the facing seam, because every yaw
commutes with the seam's 180 degree yaw, and the scene reads the listener pose back out of the
engine to check the push itself. It also checks:

- the registration follows a moved origin, and a `BwaSpeakerView` under a turned, offset parent
  follows in world space;
- scale is ignored, down to 0.0005, and a mirrored origin is refused;
- an origin under a parent that cancels its local transform gives identity;
- a cleared `room_origin` keeps the last registration;
- a `room_origin` re-pointed after start reads `is_registration_stale()` only once
  `set_extra_listeners` has been called, because the ground plane follows;
- a second `BwaEngine` stays inert, and the first is still the engine after it leaves.

`follow.tscn` builds its whole scene in code, properties before `add_child`. With the Steam
Audio build it reads a source's occlusion through a 4 m wall: the wall added after start
occludes, stops when hidden or freed, and is re-baked when the origin turns. Without the SDK
those checks print SKIPPED. Its rebuild counter must read one rebuild for two adds in a frame,
none for a hide and show in a frame or for idle frames, and one for each hide, of the wall and
of its parent. A still `BwaDynamicGeometry` door must follow a turned origin. A first-order bed
from room front pins the yaw: front against back under a 0 and a 180 degree origin, left
against right for a +90 yaw under an identity origin and for yaw 0 under a +90 origin, and
`set_yaw_from_basis` under that +90 origin. `rig.tscn` raises its origin 2 m and reads the
ground plane's occlusion, because a yaw on the floor changes no room height.

`scene.tscn` covers the geometry path, where almost nothing is observable: there is no
readback for the static mesh. The one crack of light is that enabling image-source early
reflections without a captured room makes the core record *"no room - call
bwa_scene_set_box first"*. Its **absence** is therefore evidence the box arrived.
`godot_scene_no_room` drops the box so the same assertion has to fail, which keeps the
positive run from passing vacuously. Both runs then move the registration: with the box it
must read stale, without it it must not.

<!-- /dev -->

## The playground

`addons/bw_audio/playground/` is the by-ear harness, and it ships **inside the addon**.
However you installed, open `addons/bw_audio/playground/playground.tscn` and press play.
The panel's render picker selects CaveSim (the default: the array auditioned over headphones),
Binaural, or Cave (the array itself). A new pick rebuilds the engine. If the pick has no device,
for example Cave with no ASIO driver, the playground runs silent and visual-only and says so in
the HUD.

The array is `dome_24.json` from the same folder: a generated 24-speaker dome, 2 m around a
listening point 1.5 m up, spread evenly over the sphere above the floor. To audition a
surveyed array, pass its path as a user argument (`godot ... -- my_layout.json`) or put a
`cave_layout.json` beside the executable or in the project root. The order is: the argument,
`cave_layout.json`, the dome, then the engine's built-in 26-speaker grid. The playground copies
the dome to `user://` before it loads it, because the engine opens files by OS path and an
exported `.pck` has none.

<!-- dev -->
It is the Godot port of `examples/playground.cpp`. From the source tree:

```
godot --path bindings/godot res://addons/bw_audio/playground/playground.tscn
```

Same eight scenes, same keys, same synthesized signals. The C++ one stays: it needs no
Godot, and it keeps its own imgui test suite. What running both buys you is a check on this
binding - one engine build, two clients, and any audible difference is the binding's fault.

The signal synthesis is a deliberately literal port (same LCG seeds, same Paul Kellet pink
coefficients, same periods), so an A/B between the two playgrounds compares engine paths
rather than two different noises.
<!-- /dev -->

Scenes, TAB to cycle: localization, occlusion and materials, directivity, channel walk,
blind A/B/X, ambisonic bed, reverb bed, underwater. WASD/RF move the source, Q/E turn the
head, 1-4 pick the signal. Every scene also declares its own controls (dropdowns, toggles,
sliders) and the panel builds them. The keyboard shortcuts drive the *same* setters, so the
two input paths cannot drift apart, and pressing a key visibly moves the matching widget.

**Movement follows the room basis, read from the ABI** (`BwaEngine.room_right()` and
friends), not from written-out signs. Room right is **-X**, which is the opposite of the
reflex. A hardcoded guess yields a scene that looks entirely plausible with left and
right swapped. The floor grid marks the two axes for the same reason: green ahead, red to
the listener's right, matching the red right ear on the head gizmo.

Two things worth knowing:

- **The reverb and underwater scenes rebuild the engine** on entry and exit, because the
  Steam bed, the room geometry, and the FDN are load-time. Here the playground tears down the
  whole rig subtree and stands a new one up, which is a brief audio gap by design.
- **`switch_scene()` resets every engine-wide knob.** The knobs are global, so a scene that
  left SPCAP selected or a spread mode engaged would silently change what the *next* scene
  appears to demonstrate. That reset list is ported verbatim for exactly that reason.

With no usable device the engine falls back to the null sink and everything still runs, just
silent. Visual-only is a supported state, not a failure, and the HUD says so.

<!-- dev -->
`godot_playground` checks that it runs on the dome (24 speakers, none below the floor, all
2 m from the center), then walks all eight scenes headless, crosses the reverb and underwater
boundaries both ways, and renders each HUD. It cannot judge how anything *sounds* - that is the tool's job, not the
test's - but it does catch the scene machinery and the rebuild falling over.

## Status

Phases 0-4 done: lifecycle, the coordinate seam, the frame loop, the full ABI surface,
scene-authored acoustics, and the playground port.

Not yet verified by ear on hardware, and `BwaDynamicGeometry` needs the Steam Audio build to
attach - without it the node is inert and says so.
<!-- /dev -->
