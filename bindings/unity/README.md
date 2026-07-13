# BwAudio — Unity package (`com.cave.bwaudio`)

Unity **control client** for the bwaudio spatial-audio engine. Unity is a *thin* client: it sends
control (source positions, triggers, listener pose) over the engine's C ABI — **no audio crosses the
boundary**. The engine renders the 26-speaker CAVE array over ASIO/Dante and a binaural debug monitor.

This is a UPM package: a verified P/Invoke layer (`Bw`) plus two MonoBehaviours — a scene manager
(`BwAudio`) and a positional emitter (`BwEmitter`) — and the coordinate seam (`Room`).

## Replacing Unity audio (mental model)

You **turn Unity's built-in audio off** (see below) and use these instead. It maps closely:

| Unity built-in audio | This package |
|---|---|
| `AudioListener` (on the camera) | `BwAudio.listener` (the tracked head transform) |
| `AudioSource` | `BwEmitter` |
| `AudioClip` (imported asset) | a **StreamingAssets file path** — pick it with the `[BwClip]` field; the engine decodes `.wav`/`.flac`/`.mp3` itself and resamples at load |
| `AudioSource.Play()` / `Stop()` | `BwEmitter.Play()` / `Stop()` |
| `AudioSource.PlayOneShot()` | `BwEmitter.PlayOneShot()` |
| `AudioSource.volume` | `BwEmitter.Gain` (and `FadeTo` / `FadeOut` — the engine runs the fade, no coroutine) |
| `AudioSource.pitch` | `BwEmitter.Pitch` (glides; in-memory clips only) |
| `AudioSource.priority` | `BwEmitter.Priority` (a full voice pool steals the LOWEST-priority source) |
| `AudioSource.isPlaying` | `BwEmitter.IsPlaying` (+ an `onFinished` UnityEvent) |
| `AudioListener.volume` | `BwAudio.MasterGain` |
| `AudioListener.pause` | `BwAudio.Paused` (every voice freezes; resume continues exactly) |
| `AudioMixerGroup` (ducking) | mix groups: `BwEmitter.group` + `BwAudio.SetGroupGain` / `SetGroupPaused` |
| `spatialBlend = 1` (3D) | always 3D — listener-relative DBAP across the speakers |
| `AudioSource.spread` | `BwEmitter.spread` (angular) or `sizeMetres` (a physical radius — holds its real size as the listener walks) |
| ambient / 2D music | `BwAmbisonicBed` — a world-locked AmbiX soundfield decoded to every speaker |
| Audio Reverb Zone | the shared reverb bed: **Steam Audio reflections**, or the **FDN reverb** (no SDK needed) |
| occlusion (3rd-party) | `BwEmitter.occlusion`, ray-traced against the acoustic geometry — or `SetOcclusionManual` from game logic (no SDK needed) |
| Doppler / rolloff curves | `BwEmitter.doppler`, `airAbsorption`, `loudnessComp` (all physically derived from distance) |
| output device / AudioMixer | the engine (ASIO/Dante 26-ch + binaural monitor); Unity's audio output is disabled |

The big difference: **audio files are raw files in `StreamingAssets`, not imported `AudioClip`s** — the
engine owns decoding (and avoids Unity's 8-channel output cap entirely).

## Install

Add to your project's `Packages/manifest.json` (local path or git URL):

```json
"com.cave.bwaudio": "file:../../cave-audio/bindings/unity"
```

…or copy this folder into `Packages/com.cave.bwaudio/`.

## Native plugins (required)

The package calls into two native DLLs that the **engine build produces** (they are gitignored, not
shipped in source):

```
Runtime/Plugins/x86_64/
  bwaudio.dll      # the engine
  phonon.dll       # Steam Audio (occlusion/reflections/HRTF) — must include the alignment patch
```

The engine's CMake build stages them here automatically (a POST_BUILD copy). For a manual/release
install, copy them from `build/<config>/` into `Runtime/Plugins/x86_64/`. The `x86_64` folder name
makes Unity auto-import them for **Standalone Windows x64** (ASIO is Windows-only). Confirm in the
plugin importer that the platform is set to *Windows x86_64* and **Editor** is enabled (so it works
in Play mode).

## Assets under StreamingAssets

The engine loads files itself — **do not** use Unity `AudioClip`. Put under `Assets/StreamingAssets/`:

- `cave_layout.json` — surveyed speaker geometry (`cave`/`both` profiles).
- your audio (`.wav` / `.flac` / `.mp3`; resampled to the engine rate at load).

## Disable Unity's built-in audio (do this once)

The engine **owns** the audio device. Unity's built-in audio pipeline must be **off** — left on it
opens its own output device (wasted CPU, possible contention for the binaural monitor's headphones)
and any stray `AudioSource` plays the wrong path instead of the CAVE array.

Tick **Project Settings → Audio → "Disable Unity Audio"** (or run **Tools → BwAudio → Disable Unity
Audio**). The package's editor check warns on load if it's still enabled. Unity reads this flag at
startup, so it can't be flipped from a runtime script — it's a one-time project setting.

## Use

1. Add **`BwAudio`** to one GameObject (it's a singleton, `DontDestroyOnLoad`). Set the profile,
   the `listener` transform (your OptiTrack head rigid body or XR camera), and — optionally —
   reflections and a room box (both load-time).
2. Add **`BwEmitter`** to each sound source. Set the `clip`, `loop`, `gain`, and the spatial toggles
   (`occlusion`, `reflections`, `directivity`). Its transform drives the source position every frame.
3. `BwAudio` runs the **centralized per-frame push** in `LateUpdate`: all emitters, then the
   listener, then one `bw_commit` — so the audio thread never sees a half-moved frame (this is what
   makes the moving-observer case correct; do not push from individual emitters).

## Acoustic geometry & materials (authoring)

Occlusion and reflections need the room's geometry. Two ways to author it, both **load-time**:

- **Simple box** — tick `Enable Room Box` on `BwAudio`, set the size (metres) and a material preset.
- **Per-object meshes** — add **`BwAcousticGeometry`** to any object with a mesh (or set a low-poly
  `Mesh Override`) and assign a **material**. `BwAudio` bakes every one of them (plus the box, if on)
  into the engine's scene at startup — transforming each mesh from Unity world into room space.
  Selected/`alwaysDrawGizmo` geometry is drawn as a cyan wireframe in the scene view.

Materials are project assets: **Create → BwAudio → Acoustic Material** (`BwMaterialAsset`) — either a
named engine preset (`concrete`, `glass`, …) or custom 3-band absorption / scattering / transmission.
Reference one asset from many objects; it's minted once. No material = the engine default.

> **Keep acoustic meshes SIMPLE** — tens to hundreds of triangles. The engine ray-traces them every
> frame; render meshes (thousands of tris) are far too heavy. Use a low-poly proxy via `Mesh Override`
> or a dedicated renderer-less object. Geometry is static (baked once before `bw_start`).

### Coordinate seam

The engine is **room space: right-handed, +Y up, +Z forward** (Motive's default streamed frame);
Unity is left-handed, +Y up, +Z forward. Up and forward agree, so the baseline conversion is a
single X mirror (`Room` does it in one place — Unity identity rotation maps to room identity).
Set `Room.UnityToRoom` once from your CAVE registration (the rigid transform mapping Unity world →
the physical room origin/axes). **Getting this wrong silently swaps front/back or left/right.**

## Threading & lifecycle notes

- All calls come from Unity's **main thread**; per-frame calls are non-blocking (they enqueue onto an
  SPSC ring). `bw_create`/`bw_start`/`bw_load_*` allocate/do I/O — they run once at startup.
- **Materials, the room box, and the reflection config are load-time** — `BwAudio` sets them between
  `bw_create` and `bw_start`. Calling them after start is rejected (`bw_last_error`).
- **Native plugins persist across Editor play sessions.** `BwAudio` guards its singleton and tears the
  engine down in `OnDestroy`; the engine's own globals are re-init-safe.

## API surface

`Bw` mirrors `include/bwaudio.h` **1:1** — every `BW_API` function has an entry point. The
MonoBehaviours cover the common path; for anything else, call `Bw.*` directly with
`BwAudio.Instance.Handle`.

**Tune it by ear in Play mode.** The engine makes its rendering choices switchable *live* (atomic or
crossfaded), so `BwAudio` re-pushes them whenever you touch the inspector: the panner (DBAP /
SPCAP / VBAP), dual-band panning, the spread mode (LOBE / MDAP), decorrelation, near-listener
widening, the bed renderer (MATRIX / **PARAMETRIC** — a recorded soundfield you can walk through),
tracked room EQ, master gain, and the limiter. That is an A/B you can *hear*, not a restart.

Load-time settings (a scene restart to change): the profile, the reverb bed (Steam **or** FDN — they
share one reverb tap, so pick one), the bed *decoder* (sampling / AllRAD), the room box, and all
acoustic geometry.

Everything the CAVE needs but a desktop engine doesn't is on `BwAudio`: `ChannelCount` (the layout's
speaker count — **size meter arrays with it, never hard-code 26**), `BusLevels()` (per-channel output
peaks), `SpeakerPositions()`, `ActiveVoices`, `TestSignal()` (a raw tone on one speaker, for wiring
checks), `DspTime` (schedule a sample-accurate start), and `extraListeners` — the *other* occupants,
so panning becomes a compromise across everyone in the room instead of exact for one head and wrong
for the rest.
