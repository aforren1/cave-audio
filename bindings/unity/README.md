# BwAudio — Unity package (`com.brainworks.bwaudio`)

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

**Windows x64 only** (ASIO is Windows-only). A released package **ships the native engine** —
`bwaudio.dll` + `phonon.dll` are inside it, with import settings already configured. Nothing to build.

### From OpenUPM (recommended)

```
openupm add com.brainworks.bwaudio
```

…or, without the CLI, add the registry by hand in `Packages/manifest.json`. The package then appears
under **Package Manager → My Registries**, and upgrades are a click.

```json
{
  "scopedRegistries": [
    {
      "name": "package.openupm.com",
      "url": "https://package.openupm.com",
      "scopes": ["com.brainworks"]
    }
  ],
  "dependencies": {
    "com.brainworks.bwaudio": "0.2.0"
  }
}
```

(Unity's "scope" is a package-name *prefix*, not an npm `@scope` — which is why this works on OpenUPM
or npmjs but *cannot* work on GitHub Packages, whose npm registry only accepts `@owner/name`.)

### From a release tarball (no registry)

Grab `com.brainworks.bwaudio-<version>.tgz` from the
[Releases](https://github.com/aforren1/cave-audio/releases) page, then **Package Manager → `+` →
Install package from tarball…**. It is byte-for-byte the *same* artifact OpenUPM serves — just pinned
by hand instead of resolved, so it won't offer upgrades.

### From source (developing the engine itself)

Point the manifest at your working tree, and the CMake build's POST_BUILD copy keeps the plugins fresh:

```json
"com.brainworks.bwaudio": "file:../../cave-audio/bindings/unity"
```

The DLLs are gitignored build output, so a **git-URL install of this repo will not work** — you'd get
the C# with no engine behind it (`DllNotFoundException` on the first call). Use the registry or a
tarball, both of which carry the binaries.

> **License:** the engine is **GPLv3** (`bwaudio.dll` links the ASIO SDK under its GPLv3 option).
> Internal use never triggers copyleft — it's a *distribution* condition. But shipping a Unity app
> containing this DLL to third parties would place that app under GPLv3; see
> [`docs/build.md`](../../docs/build.md) for the proprietary-ASIO alternative.

## Releasing (maintainers)

**The GitHub Release *is* the publish.** OpenUPM runs in `githubRelease` tracking mode: it discovers
the git tag, finds the Release whose tag matches, and serves the attached `.tgz` unchanged. So there is
no registry push, **no token, and no secret to rotate** — and the same file is the manual-tarball
download.

To cut a release: bump `version` in `package.json` (+ the CHANGELOG), then push a matching tag
(`v0.2.0`). CI packs on *every* run — so a broken package fails the build rather than the release — and
on a tag it creates the Release. **The pack fails if the tag and the manifest disagree**, so a tarball
can't claim a version it isn't.

A release carries **two** assets: this package (`com.brainworks.bwaudio-<ver>.tgz`) and the engine on
its own (`bwaudio-win64-<tag>.zip` — dll/lib/header/tools, for C/C++ consumers and the CAVE machine).
The engine one **must stay a `.zip`**: OpenUPM takes the single publishable `.tgz` on the release
unconditionally, so a second tarball would make it ambiguous which is the package.

Locally:

```
cmake --build build --config RelWithDebInfo         # produces the DLLs
powershell -File tools/upm/pack.ps1                 # -> dist/com.brainworks.bwaudio-<version>.tgz
```

Two things that will bite if forgotten:

- **New file in the package? Run `tools/upm/gen-meta.ps1`.** Every asset must ship a committed `.meta`,
  or its GUID is regenerated per-project and scenes lose their script references. `pack.ps1` refuses to
  build a tarball with one missing.
- **Attaching a second `.tgz` to a Release breaks OpenUPM** — it expects exactly one publishable
  tarball. If that ever changes, set `githubReleaseAssetName` in the OpenUPM package config to the
  stable prefix `com.brainworks.bwaudio-`.

One-time setup (already done, recorded here for the next person): the package was submitted to
[OpenUPM](https://openupm.com/packages/add/) with `trackingMode: githubRelease`, which is what makes it
serve our pre-built tarball instead of trying to `npm pack` a git clone — the default mode would ship
the C# with **no DLLs**, since those are gitignored build output.

## Assets under StreamingAssets

The engine loads files itself — **do not** use Unity `AudioClip`. Everything it reads lives in
`Assets/StreamingAssets/`, and every file field on a component is a path **relative to that folder**
(the `[BwClip]` picker only lists what's actually there, so you don't type paths by hand):

```
Assets/
  StreamingAssets/          <- create this folder; Unity does not by default
    cave_layout.json        <- surveyed speaker geometry
    sfx/footsteps.wav       <- your audio (.wav / .flac / .mp3)
```

- `cave_layout.json` — the surveyed speaker geometry. **Watch this one:** if it's missing or fails to
  parse, the engine does *not* stop — it falls back to its built-in 26-speaker grid and only records why
  in `bw_last_error`. On a rig that isn't 26 speakers that silently changes the channel count too, so
  every source gets panned over geometry that isn't the one in your room. `BwAudio` logs an error at
  startup if this happens, and the inspector warns (showing the exact absolute path it will look in)
  before you ever hit Play.
- `constraints.json` *(optional)* — the surveyed **room**: the speaker truss, the CAVE screen cube, the
  projectors. Add a **`BwRoomConstraints`** component and it draws them in the scene view (green truss,
  red keep-out, orange obstacles) so you can place content against the real room instead of guessing.
  It's the same file `bw_layout_tool` and `bw_playground` read — one source of truth; don't re-author
  those boxes as Unity geometry. It affects nothing audible.

### Seeing the room

`BwAudio` draws the **speaker array** in the scene view, each speaker labelled with its channel index —
so, with `BwRoomConstraints`, the whole CAVE (truss, screens, projectors, speakers) is visible while you
work, in real metres.

Two details worth knowing. Stopped, the speaker positions come from the layout **file**; in Play mode
they come from the **engine**, and each speaker lights up with that channel's live output level (a dead
or mis-wired speaker is then obvious, and it's the same trick `bw_playground` uses). And because Play
mode draws what the engine is *actually* panning over, **a failed layout load looks like the wrong
array** — the built-in 26-grid, sitting where your room isn't.

Everything is drawn through `Room.RoomToUnityMatrix()`, the inverse of the coordinate seam. So if the
array and the room land somewhere unexpected in the scene, your `Room.UnityToRoom` registration is
wrong — which is the cheapest possible check on the one setting that silently ruins spatial audio.

### Watching the array from inside the CAVE

Gizmos are an editor feature — they don't render in a build. For live speaker activity **while you're
standing in the room**, add **`BwSpeakerView`**: it spawns one unlit marker per channel at the real
speaker's position, and each one brightens and grows with that channel's output. Look toward a speaker
and you see it light up.

It's a viewer, not a control: it only reads `bw_get_speakers` / `bw_get_bus_levels`, never writes audio
state, and deleting it changes nothing you can hear. The markers are **unlit** deliberately (a CAVE is
dark, so the colour computed is the colour seen — no lighting rig to set up), and the meter has an
instant attack with a slow release, because the engine reports a per-*block* peak that strobes too fast
to read raw.

Pair it with `BwAudio.TestSignal(channel, kind, gain)` for wiring checks: drive one raw output channel
and confirm the speaker that lights up is the one that makes noise.
- Your audio — decoded and resampled to the engine rate at load.

> **While the Unity Editor is open it holds `bwaudio.dll` loaded**, so the file is locked. A CMake
> rebuild will fail its POST_BUILD copy into `Runtime/Plugins/x86_64/`, and so will `pack.ps1`. Close
> the editor before rebuilding the engine. (Packing an *unchanged* DLL is fine — the script hashes it
> and skips the copy.)

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
