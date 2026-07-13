# Changelog

All notable changes to `com.cave.bwaudio`.

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
