# Changelog

All notable changes to `com.brainworks.bwaudio`.

## [0.2.0]

First **published** release: the package now ships as an installable UPM tarball with the native engine
inside it (`bwaudio.dll` + `phonon.dll`, import settings pre-configured), so a Unity project consumes it
without building any C++.

- **Renamed to `com.brainworks.bwaudio`** (was `com.cave.bwaudio`). Done before anything shipped, so
  there is nothing to migrate: the scope you add to `manifest.json` is now `com.brainworks`. The C#
  namespace stays `CaveAudio` — this is the package identity, not the code.
- **Distribution** — `tools/upm/pack.ps1` packs `com.brainworks.bwaudio-<version>.tgz` (uses `tar`, no
  Node anywhere; CI packs on *every* run, so a broken package fails the build rather than the release).
  A `v*` tag cuts a GitHub Release with that tarball attached, and **the Release is the publish**:
  OpenUPM's `githubRelease` tracking mode serves that exact file — no registry push, no token. Install
  with `openupm add com.brainworks.bwaudio`, or download the `.tgz` and use *Install package from
  tarball…* if you'd rather not add a registry at all. See the README.
- **`.meta` files are now committed** (`tools/upm/gen-meta.ps1`). A package installed from a registry is
  immutable, so assets arriving without a `.meta` get a fresh random GUID in every project: a scene
  referencing `BwEmitter` on one machine would deserialize as *"Missing (Mono Script)"* on another. The
  native plugins' import settings (Windows x64, Editor enabled) ship the same way — in an immutable
  package the user cannot fix them in the Inspector.
- **Version/tag guard** — the pack fails if the git tag and `package.json` disagree, so a tarball can't
  claim a version it isn't.

### Usability pass

The theme: the binding had several settings that failed *silently* — the engine survives them, logs
something, and carries on sounding subtly wrong. Those are now impossible to express, or caught in the
inspector where you can still see them.

- **Materials are a dropdown, not a string.** `BwMaterialPreset` (mirrors the engine's table) replaces
  the free-text preset name on `BwAudio.roomMaterial` and `BwMaterialAsset.preset`. An unrecognised name
  was never an error — `bw_material_preset` quietly returns material 0 (generic) and leaves the reason in
  `bw_last_error` — so a typo'd `"concreet"` wall just *sounded* wrong. **Breaking:** `BwMaterialAsset`
  assets whose preset wasn't `concrete` will reset to Concrete; re-pick it from the dropdown.
- **The layout file says where it goes.** `layoutFile` uses the `[BwClip(".json")]` picker (the same one
  audio clips use), so it lists the JSON files actually present under `StreamingAssets`, flags a missing
  one in red, and tooltips that the path is *relative to `Assets/StreamingAssets/`*. The inspector also
  shows the **absolute path the engine will look in** when the file isn't there — paired with the
  runtime error, both ends of that trap are now closed.
- **Inspector sliders work in Play mode.** `BwEmitter` gained the `OnValidate` re-push that `BwAudio` and
  `BwAmbisonicBed` already had. Dragging Gain/Pitch/Spread during Play used to change the field and
  nothing else (the value is only read at source creation), which reads as a broken slider.
- **Custom inspectors** for `BwAudio`, `BwEmitter` and `BwMaterialAsset`: settings that don't apply are
  hidden (FDN decay with the FDN off, pose prediction when Unity feeds the pose, custom coefficients
  under a preset material…), and the mistakes the engine merely *warns* about are surfaced as inspector
  warnings — a missing layout, both reverb beds fighting over the one tap, reflections with no geometry
  to reflect off. In Play mode `BwAudio` shows the live backend (flagging a **silent** fallback to the
  null sink), channel count, active voices and per-channel output meters; `BwEmitter` shows its
  ray-traced occlusion, which is otherwise invisible.
- **The room box is visible.** It draws as a wireframe gizmo (`Room.RoomToUnityMatrix` — the inverse of
  the coordinate seam, so a wrong `Room.UnityToRoom` makes the box land visibly in the wrong place).
- **The speaker array is visible.** `BwAudio` draws each speaker as a gizmo, labelled with its channel
  index. Stopped, the positions come from the layout **file**; in Play mode they come from the **engine**
  (`bw_get_speakers`) and each one lights up with that channel's live output level, the same way the
  playground's gizmos do — so a dead or mis-wired speaker is visible at a glance. The Play-mode source
  matters: it's the geometry the engine is *actually* panning over, which means a failed layout load
  shows up as the built-in 26-grid sitting where your room isn't.
- **`BwSpeakerView` — live speaker activity you can see from inside the CAVE.** The gizmos above are an
  *editor* feature and don't render in a build, so this is the runtime counterpart: one unlit marker per
  channel, placed at the real speaker's position, brightening (and growing) with that channel's output.
  Unlit on purpose — a CAVE is dark, so the colour computed is the colour seen, with no lights to set up.
  Instant attack + slow release, because the engine reports a per-*block* peak that strobes too fast to
  read raw. Uses the same `bw_get_speakers` + `bw_get_bus_levels` readbacks as everything else, writes no
  audio state, and picks its shader across URP / HDRP / built-in. Optional: delete it and nothing changes
  audibly.
- **`BwRoomConstraints` — the physical room, in the scene view.** Reads the surveyed `constraints.json`
  (from StreamingAssets) and draws it: green = the speaker truss, red = the CAVE screen cube / observer
  keep-out, orange = the projectors. It's the **same file** `bw_layout_tool` and `bw_playground` read,
  so the room has one source of truth and doesn't get re-authored as Unity geometry that can drift from
  the survey. Purely a scene-view aid — no engine needed (it parses the JSON directly, so it works with
  the editor stopped) and nothing audible depends on it.
- **Minimum Unity is now 6000.0 (Unity 6)**, up from 2021.3. Nothing had ever been tested below it, and
  the old floor was already forcing compatibility shims.
- **No deprecation warnings.** The scene bake used `FindObjectsOfType`, deprecated in favour of
  `FindObjectsByType`, which forces you to say whether you need the results sorted. We don't — every
  geometry is baked into one mesh — so it passes `FindObjectsSortMode.None` and skips a pointless
  InstanceID sort.
- **Packing no longer dies on a locked DLL.** An open Unity Editor holds `bwaudio.dll` loaded out of
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
  floor), and the room box (`AddBox` / `bw_scene_set_box`) is floor-based: x/z centred, y from 0
  up to the box height. The engine references the array centroid, not the origin, for its
  world-locked decodes, so surveys with other origins keep working.

- Pause/seek + the output protection limiter (engine `9d60c6e`): `BwEmitter.Pause()/UnPause()/Paused`
  (AudioSource.Pause/UnPause equivalents; click-free, the playhead freezes, paused still reads as
  IsPlaying) and `BwEmitter.Seek(samples)` (a timeSamples-set equivalent; in-memory clips only —
  streamed clips ignore it); `BwAudio.SetLimiter(bool)` / `SetLimiterCeiling(dB)` over the
  engine-default ON at -1 dBFS.

- `Bw` — P/Invoke layer, 1:1 with the bwaudio C ABI (`include/bwaudio.h`): lifecycle, assets,
  sources, ambisonic beds, materials/occlusion, directivity, reflection bed, listener, commit.
  Verified against the real `bwaudio.dll` (struct layout, calling convention, string/array/bool
  marshalling).
- `Room` — the room-space (RH) ↔ Unity (LH) coordinate seam.
- `BwAudio` — scene manager singleton: owns the engine handle, loads assets, configures reflections +
  an optional room box at load time, and runs the centralized per-frame push (sources → listener →
  one commit).
- `BwEmitter` — positional source: transform-driven position/orientation, with `occlusion`,
  `reflections`, and `directivity` toggles, plus a one-shot helper.
- Editor guardrail (`BwAudioProjectCheck`): warns if Unity's built-in audio is still enabled (the
  engine owns the device) and offers one-click **Tools → BwAudio → Disable Unity Audio**.
- Acoustic-scene authoring: `BwMaterialAsset` (Create → BwAudio → Acoustic Material; preset or custom
  3-band) and `BwAcousticGeometry` (mark a mesh as occluding/reflecting, assign a material, scene-view
  gizmo). `BwAudio` bakes all geometry (+ the optional room box) world→room into one mesh at load.
- Audio-file authoring: `[BwClip]` attribute + `BwClipDrawer` — an editor picker that lists the
  `.wav`/`.flac`/`.mp3` files under StreamingAssets (with a browse button and a missing-file flag)
  instead of a hand-typed path. `BwEmitter` gains AudioSource-style `Play()`/`Stop()`/`Gain`. README
  has a "Replacing Unity audio" mapping table.
- `BwAmbisonicBed` — world-locked AmbiX soundfield component (wraps `bw_bed_*`): play/stop/gain for
  diffuse ambience/music, decoded straight to all 26 speakers.
- Reverb wet level: `BwAudio.reverbGain` (inspector) + a live `ReverbGain` property, backed by the
  new engine config field `wet_gain` + `bw_reflections_set_gain`.
- `BwEmitter.IsPlaying` + an `onFinished` UnityEvent, backed by a new engine ABI call
  `bw_source_is_playing` (latest-wins per-source playback readback).

### Caught up to the engine ABI

The engine had grown 23 `BW_API` calls the binding never got. `Bw` is **1:1 with `bwaudio.h`** again
(verified by diffing the exported symbols), and the components expose the ones a scene actually
authors:

- **Mixing** — `BwEmitter.FadeTo()` / `FadeOut()` (the engine runs the fade on the audio thread; no
  coroutine, and `FadeOut` lands on the click-free stop path), `BwEmitter.Pitch` (glides — a change
  bends the pitch rather than stepping it), `BwEmitter.Priority` (voice-steal), mix groups
  (`BwEmitter.group` + `BwAudio.SetGroupGain` / `SetGroupPaused` — duck the SFX, keep the dialog),
  `BwAudio.MasterGain`, and `BwAudio.Paused` (global freeze; resume continues exactly).
- **Width** — `BwEmitter.spread` was bound but had no inspector field; `sizeMetres` (a physical
  radius: the source keeps its real-world size as the listener walks, where a fixed angular spread
  would not) is new, as are the engine-wide `spreadMode` (LOBE / MDAP), `decorrelation` (wide sources
  stop collapsing to phantom images as you walk), and `nearSpreadRadius`.
- **Propagation** — `BwEmitter.loudnessComp` (an LF shelf tracking the distance attenuation: far, not
  thin) joins `doppler` and `airAbsorption`.
- **Reverb without the SDK** — `BwAudio.enableFdnReverb` + the decay controls: a directional FDN bed
  that takes the reverb tap **instead of** the Steam bed (the manager warns if both are ticked) and is
  fed by the same per-emitter sends, so reverb works in a build with no phonon. Likewise
  `BwEmitter.SetOcclusionManual()` — game-driven occlusion (a door the gameplay knows about,
  underwater) through the sim's own ramped, band-tilted publish path, no SDK required.
- **Beds** — `BwAmbisonicBed.YawDegrees` (turn a recorded soundfield to line up with the scene) and
  `BwAudio.bedRenderer`: MATRIX, or **PARAMETRIC**, which re-pans the directional part of the field
  through the listener-relative panner — a recorded soundfield becomes *walkable*.
- **Listener** — `BwAudio.extraListeners` (up to 3 other occupants; panning becomes the energy mean of
  everyone's solve, instead of exact for one head and wrong for the rest), pushed in the same frame
  block as the primary pose since it is commit-gated the same way; and `posePredictionMs`, which leads
  the *tracked* pose by your measured motion-to-ears latency.
- **Diagnostics** — `BwAudio.ChannelCount` / `BusLevels()` / `SpeakerPositions()` / `ActiveVoices` /
  `TestSignal()` / `DspTime`.
- **Live A/B by ear** — `BwAudio.OnValidate` re-pushes every knob the engine makes atomic or
  crossfaded (panner, dual-band, spread mode, decorrelation, near-spread, bed renderer, tracked room
  EQ, master gain, limiter), so inspector tweaks are audible in Play mode instead of needing a restart.

Two coordinate seams the new calls exposed, both now in `Room` so nothing re-derives them:
`Room.YawRad` (the X mirror **reverses the sense of rotation** — a Unity euler angle passed straight
to `bw_bed_set_rotation` spins the soundfield the wrong way) and `Room.Dir` (a *direction*, e.g. the
FDN's decay axis, must not pick up the registration transform's translation the way `Room.Pos` does).

`BwAudio` now also **reports a failed layout load** (`bw_last_error` right after `bw_create`): it is
non-fatal — the engine falls back to its 26-speaker default grid — which on a smaller rig silently
changes the channel count and pans every source over geometry that isn't the one in the room.

### Hardened (adversarial review)

- `BwAudio` claims the `Instance` singleton only after a successful `bw_start` (a failed init no longer
  leaves a dead, un-replaceable manager), and iterates a snapshot of the emitter list during the
  per-frame push (an `onFinished` handler that disables its emitter can no longer mutate the list
  mid-loop and drop the frame's commit).
- `BwEmitter` lazily creates its source (a coroutine retries until `BwAudio` is ready) instead of
  permanently disabling on an init-order race, and resets its play-edge state on disable (no spurious
  `onFinished` on re-enable).
- Mesh baking computes the winding flip from the full transform determinant, so a negative/mirrored
  object scale no longer bakes backward-facing reflectors. `Room.UnityToRoom` is documented as
  rigid-only.
- `Bw.GetListenerPose` allocates correctly-sized arrays (the raw call writes fixed slots); the editor
  clip scan tolerates an inaccessible StreamingAssets subdir.
