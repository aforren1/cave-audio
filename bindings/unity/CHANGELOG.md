# Changelog

All notable changes to `com.cave.bwaudio`.

## [0.1.0] — unreleased

Initial Unity binding (M7).

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
- `BwEmitter.IsPlaying` + an `onFinished` UnityEvent, backed by a new engine ABI call
  `bw_source_is_playing` (latest-wins per-source playback readback).

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
