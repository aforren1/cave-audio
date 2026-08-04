# BwAudio — Unity package (`com.brainworks.bw_audio`)

Unity **control client** for the bw_audio spatial-audio engine. Unity is a *thin* client: it sends
control (source positions, triggers, listener pose) over the engine's C ABI — **no audio crosses the
boundary**. The engine renders the 26-speaker CAVE array over ASIO/Dante and a binaural debug monitor.

This is a UPM package: a verified P/Invoke layer (`Bwa`) plus two MonoBehaviours — a scene manager
(`Engine`) and a positional emitter (`Emitter`) — and the coordinate seam (`Room`).

## Replacing Unity audio (mental model)

You **turn Unity's built-in audio off** (see below) and use these instead. It maps closely:

| Unity built-in audio | This package |
|---|---|
| `AudioListener` (on the camera) | `Engine.listener` (the tracked head transform) |
| `AudioSource` | `Emitter` |
| `AudioClip` (imported asset) | a **StreamingAssets file path** — pick it with the `[Clip]` field; the engine decodes `.wav`/`.flac`/`.mp3` itself and resamples at load |
| `AudioSource.Play()` / `Stop()` | `Emitter.Play()` / `Stop()` |
| `AudioSource.PlayOneShot()` | `Emitter.PlayOneShot()` |
| `AudioSource.volume` | `Emitter.Gain` (and `FadeTo` / `FadeOut` — the engine runs the fade, no coroutine) |
| `AudioSource.pitch` | `Emitter.Pitch` (glides; in-memory clips only) |
| `AudioSource.priority` | `Emitter.Priority` (a full voice pool steals the LOWEST-priority source) |
| `AudioSource.isPlaying` | `Emitter.IsPlaying` (+ an `onFinished` UnityEvent) |
| `AudioListener.volume` | `Engine.MasterGain` |
| `AudioListener.pause` | `Engine.Paused` (every voice freezes; resume continues exactly) |
| `AudioMixerGroup` (ducking) | mix groups: `Emitter.group` + `Engine.SetGroupGain` / `SetGroupPaused` |
| `spatialBlend = 1` (3D) | always 3D — listener-relative DBAP across the speakers |
| `AudioSource.spread` | `Emitter.spread` (angular) or `sizeMetres` (a physical radius — holds its real size as the listener walks) |
| ambient / 2D music | `AmbisonicBed` — a world-locked AmbiX soundfield decoded to every speaker |
| procedural audio (`OnAudioFilterRead`) | `PushEmitter` — a positional source you feed mono PCM you generate |
| Audio Reverb Zone | the shared reverb bed: **Steam Audio reflections**, or the **FDN reverb** (no SDK needed) |
| occlusion (3rd-party) | `Emitter.occlusion`, ray-traced against the acoustic geometry — or `SetOcclusionManual` from game logic (no SDK needed) |
| Doppler / rolloff curves | `Emitter.doppler`, `airAbsorption`, `loudnessComp` (all physically derived from distance) |
| output device / AudioMixer | the engine (ASIO/Dante 26-ch + binaural monitor); Unity's audio output is disabled |

The big difference: **audio files are raw files in `StreamingAssets`, not imported `AudioClip`s** — the
engine owns decoding (and avoids Unity's 8-channel output cap entirely).

## Install

**Windows x64 only** (ASIO is Windows-only). The released package **ships the native engine** —
`bw_audio.dll` + `phonon.dll` are inside it, with import settings already configured. Nothing to build.

### From a git URL

**Package Manager → `+` → Add package from git URL…**, then:

```
https://github.com/aforren1/cave-audio.git#unity
```

`unity` is a distribution branch whose **root** is the package — `package.json` at the top level,
with the two DLLs already in it — which is the only layout UPM's git installer accepts. CI republishes
it on every `v*` tag. Pin a release instead of tracking the branch by using a tag ref
(`#v0.4.0`) once one exists.

### From a release tarball

Grab `com.brainworks.bw_audio-<version>.tgz` from the
[Releases](https://github.com/aforren1/cave-audio/releases) page, then **Package Manager → `+` →
Install package from tarball…**. That's the whole install.

Either route pins by hand rather than resolving, so neither notifies you of upgrades — take the newer
one and install it again. (This package isn't on a registry: it exists to drive one specific
26-speaker CAVE, and the audience is people who already have the repo.)

### From source (developing the engine itself)

Point the manifest at your working tree, and the CMake build's POST_BUILD copy keeps the plugins fresh:

```json
"com.brainworks.bw_audio": "file:../../cave-audio/bindings/unity"
```

The DLLs are gitignored build output, so a git-URL install pointed at **`main`** will not work — you'd
get the C# with no engine behind it (`DllNotFoundException` on the first call), and `package.json` is
not at the repo root there either. That is what the `unity` branch above exists to fix: CI publishes a
built, root-level copy of the package to it. Against a working tree, use a local path.

> **License:** the engine is **GPLv3** (`bw_audio.dll` links the ASIO SDK under its GPLv3 option).
> Internal use never triggers copyleft — it's a *distribution* condition. But shipping a Unity app
> containing this DLL to third parties would place that app under GPLv3; see
> [`docs/build.md`](../../docs/build.md) for the proprietary-ASIO alternative.

## Releasing (maintainers)

The canonical whole-repo release process (version model, steps, dev versions) lives in
[docs/build.md → Releasing](../../docs/build.md#releasing). This is the Unity-package view of it.

**The GitHub Release is the distribution** — there's no registry, no token, and nothing to keep in sync.

To cut a release: fill in the CHANGELOG's `[Unreleased]` section, then run the helper — it rolls that
heading to the version, commits, and creates the matching annotated tag:

```
powershell -File tools/release.ps1 0.3.0            # roll CHANGELOG, commit, tag v0.3.0 (no push)
powershell -File tools/release.ps1 0.3.0 -DryRun    # preview the roll; change nothing
powershell -File tools/release.ps1 0.3.0 -Push      # ...and push, which triggers the CI release
```

**The tag is the version** — CI stamps it into the packaged `package.json`, so there's no manifest
field to bump and nothing to keep in sync (the committed `version` stays `0.0.0-dev`, the honest value
for an unreleased checkout). Prefer `git tag v0.3.0` by hand? That works too; the helper just also does
the CHANGELOG roll. CI packs on *every* run — so a broken package fails the build rather than the
release — and on a tag it creates the Release with the stamped version.

A release carries **two** assets: this package (`com.brainworks.bw_audio-<ver>.tgz`) and the engine on
its own (`bw_audio-win64-<tag>.zip` — dll/lib/header/tools, for C/C++ consumers and the CAVE machine).

Locally:

```
cmake --build build --config RelWithDebInfo         # produces the DLLs
powershell -File tools/upm/pack.ps1                 # -> dist/com.brainworks.bw_audio-<version>.tgz
```

Without a `-Version`, the local pack derives its version from `git describe` (e.g.
`0.2.0-dev.32.gcf83c8e` — 32 commits past `v0.2.0`), so a dev tarball is traceable to a commit instead
of a flat placeholder. Pass `-Version` (or let CI pass the tag) for a release build.

Two things that will bite if forgotten:

- **New file in the package? Run `tools/upm/gen-meta.ps1`.** Every asset must ship a committed `.meta`,
  or its GUID is regenerated per-project and scenes lose their script references. `pack.ps1` refuses to
  build a tarball with one missing.
- **Keep the engine bundle a `.zip`, and the package the only `.tgz` on a release.** Every UPM registry
  that could ever serve this (they all speak the npm protocol) keys on a single publishable tarball, so
  one `.tgz` per release keeps that door open at zero cost.

Not on a registry, and deliberately so: this drives one specific 26-speaker CAVE, so the audience is
people who already have the repo, and a tarball costs them one click. The tarball *is* the artifact a
registry would serve, so listing it later is a config change, not a rebuild.

## Assets under StreamingAssets

The engine loads files itself — **do not** use Unity `AudioClip`. Everything it reads lives in
`Assets/StreamingAssets/`, and every file field on a component is a path **relative to that folder**
(the `[Clip]` picker only lists what's actually there, so you don't type paths by hand):

```
Assets/
  StreamingAssets/          <- create this folder; Unity does not by default
    cave_layout.json        <- surveyed speaker geometry
    sfx/footsteps.wav       <- your audio (.wav / .flac / .mp3)
```

- `cave_layout.json` — the surveyed speaker geometry. **Watch this one:** if it's missing or fails to
  parse, the engine does *not* stop — it falls back to its built-in 26-speaker grid and only records why
  in `bwa_last_error`. On a rig that isn't 26 speakers that silently changes the channel count too, so
  every source gets panned over geometry that isn't the one in your room. `Engine` logs an error at
  startup if this happens, and the inspector warns (showing the exact absolute path it will look in)
  before you ever hit Play.
- `constraints.json` *(optional)* — the surveyed **room**: the speaker truss, the CAVE screen cube, the
  projectors. Add a **`RoomConstraints`** component and it draws them in the scene view (green truss,
  red keep-out, orange obstacles) so you can place content against the real room instead of guessing.
  It's the same file `bwa_layout_tool` and `bwa_playground` read — one source of truth; don't re-author
  those boxes as Unity geometry. It affects nothing audible.

### Seeing the room

`Engine` draws the **speaker array** in the scene view, each speaker labeled with its channel index —
so, with `RoomConstraints`, the whole CAVE (truss, screens, projectors, speakers) is visible while you
work, in real meters.

Two details worth knowing. Stopped, the speaker positions come from the layout **file**; in Play mode
they come from the **engine**, and each speaker lights up with that channel's live output level (a dead
or mis-wired speaker is then obvious, and it's the same trick `bwa_playground` uses). And because Play
mode draws what the engine is *actually* panning over, **a failed layout load looks like the wrong
array** — the built-in 26-grid, sitting where your room isn't.

Everything is drawn through `Room.RoomToUnityMatrix()`, the inverse of the coordinate seam. So if the
array and the room land somewhere unexpected in the scene, your `Room.UnityToRoom` registration is
wrong — which is the cheapest possible check on the one setting that silently ruins spatial audio.

### Watching the array from inside the CAVE

Gizmos are an editor feature — they don't render in a build. For live speaker activity **while you're
standing in the room**, add **`SpeakerView`**: it spawns one unlit marker per channel at the real
speaker's position, and each one brightens and grows with that channel's output. Look toward a speaker
and you see it light up.

It's a viewer, not a control: it only reads `bwa_get_speakers` / `bwa_get_bus_levels`, never writes audio
state, and deleting it changes nothing you can hear. The markers are **unlit** deliberately (a CAVE is
dark, so the color computed is the color seen — no lighting rig to set up), and the meter has an
instant attack with a slow release, because the engine reports a per-*block* peak that strobes too fast
to read raw.

Pair it with `Engine.TestSignal(channel, kind, gain)` for wiring checks: drive one raw output channel
and confirm the speaker that lights up is the one that makes noise.
- Your audio — decoded and resampled to the engine rate at load.

> **While the Unity Editor is open it holds `bw_audio.dll` loaded**, so the file is locked. A CMake
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

1. Add **`Engine`** to one GameObject (it's a singleton, `DontDestroyOnLoad`). Set the profile,
   the `listener` transform (your OptiTrack head rigid body or XR camera), and — optionally —
   reflections and a room box (both load-time).
2. Add **`Emitter`** to each sound source. Set the `clip`, `loop`, `gain`, and the spatial toggles
   (`occlusion`, `reflections`, `directivity`). Its transform drives the source position every frame.
3. `Engine` runs the **centralized per-frame push** in `LateUpdate`: all emitters, then the
   listener, then one `bwa_commit` — so the audio thread never sees a half-moved frame (this is what
   makes the moving-observer case correct; do not push from individual emitters).

### Procedural audio — `PushEmitter`

When the audio isn't a file but something you **generate** — a software synth, an engine model, a
network voice stream — add a **`PushEmitter`** instead of an `Emitter` and feed it mono float PCM at
`Engine.SampleRate`. It's a full positional source (position, gain, spread, occlusion, reflections,
Doppler, groups, fades all work and ride the same centralized push), so the only difference from
`Emitter` is *where the samples come from*: there's no clip, and the engine **refuses**
play / seek / pitch / queue on a push voice — so those members simply aren't here (the same reason
Godot splits `BwaPushSource` off). Its transform still drives the source position every frame.

The voice consumes from create (silence until the first push; an underrun renders silence without
losing your place). Push a frame or so ahead and **pace against `PushSpace`**; `PushEnd()` ends the
voice once the ring drains (one-way — a push voice is not restartable, so re-enable the component for
a fresh one). The ring holds 65536 frames (~1.37 s at 48 kHz).

```csharp
using UnityEngine;
using BwAudio;

[RequireComponent(typeof(PushEmitter))]
public sealed class SineFeeder : MonoBehaviour
{
    PushEmitter _pe;
    double _phase;
    float[] _buf = new float[4096];

    void Awake() => _pe = GetComponent<PushEmitter>();

    void Update()
    {
        int n = Mathf.Min(_pe.PushSpace, _buf.Length);   // only push what the ring will take
        double step = 2.0 * Mathf.PI * 220.0 / Engine.Instance.sampleRate;
        for (int i = 0; i < n; i++) { _buf[i] = 0.2f * (float)System.Math.Sin(_phase); _phase += step; }
        _pe.Push(_buf, n);                                // returns the count accepted
    }

    // ...call _pe.PushEnd() when the stream is done.
}
```

Feed from Unity's **main thread**, like every other `bw_audio` call — generate on a worker thread if
you must, but hand the finished buffers to `Push` from `Update`/`LateUpdate`.

## Acoustic geometry & materials (authoring)

Occlusion and reflections need the room's geometry. Two ways to author it, both **load-time**:

- **Simple box** — tick `Enable Room Box` on `Engine`, set the size (meters) and a material preset.
- **Per-object meshes** — add **`AcousticGeometry`** to any object with a mesh (or set a low-poly
  `Mesh Override`) and assign a **material**. `Engine` bakes every one of them (plus the box, if on)
  into the engine's scene at startup — transforming each mesh from Unity world into room space.
  Selected/`alwaysDrawGizmo` geometry is drawn as a cyan wireframe in the scene view.

Materials are project assets: **Create → BwAudio → Acoustic Material** (`MaterialAsset`) — either a
named engine preset (`concrete`, `glass`, …) or custom 3-band absorption / scattering / transmission.
Reference one asset from many objects; it's minted once. No material = the engine default.

> **Keep acoustic meshes SIMPLE** — tens to hundreds of triangles. The engine ray-traces them every
> frame; render meshes (thousands of tris) are far too heavy. Use a low-poly proxy via `Mesh Override`
> or a dedicated renderer-less object. Geometry is static (baked once before `bwa_start`).

### Coordinate seam

The engine is **room space: right-handed, +Y up, +Z forward** (Motive's default streamed frame);
Unity is left-handed, +Y up, +Z forward. Up and forward agree, so the baseline conversion is a
single X mirror (`Room` does it in one place — Unity identity rotation maps to room identity).
Set `Room.UnityToRoom` once from your CAVE registration (the rigid transform mapping Unity world →
the physical room origin/axes). **Getting this wrong silently swaps front/back or left/right.**

## Threading & lifecycle notes

- All calls come from Unity's **main thread**; per-frame calls are non-blocking (they enqueue onto an
  SPSC ring). `bwa_create`/`bwa_start`/`bwa_load_*` allocate/do I/O — they run once at startup.
- **Materials, the room box, and the reflection config are load-time** — `Engine` sets them between
  `bwa_create` and `bwa_start`. Calling them after start is rejected (`bwa_last_error`).
- **Native plugins persist across Editor play sessions.** `Engine` guards its singleton and tears the
  engine down in `OnDestroy`; the engine's own globals are re-init-safe.

## API surface

`Bwa` mirrors `include/bw_audio.h` — every `BWA_API` function has an entry point except two Unity
never uses: `bwa_set_output_capture` (an audio-thread callback) and `bwa_render_block` (the
manual-sink golden-render path). The MonoBehaviours cover the common path; for anything else, call
`Bwa.*` directly with `Engine.Instance.Handle`.

**Tune it by ear in Play mode.** The engine makes its rendering choices switchable *live* (atomic or
crossfaded), so `Engine` re-pushes them whenever you touch the inspector: the panner (DBAP /
SPCAP / VBAP), dual-band panning, the spread mode (LOBE / MDAP / SPECTRAL), max-rE (and its band
split), decorrelation, near-listener widening, the bed renderer (MATRIX / **PARAMETRIC** — a recorded
soundfield you can walk through), tracked room EQ, master gain, and the limiter. That is an A/B you
can *hear*, not a restart.

Load-time settings (a scene restart to change): the profile, the reverb bed (Steam **or** FDN — they
share one reverb tap, so pick one), the bed *decoder* (AllRAD / EPAD), the room box, and all
acoustic geometry.

Everything the CAVE needs but a desktop engine doesn't is on `Engine`: `ChannelCount` (the layout's
speaker count — **size meter arrays with it, never hard-code 26**), `BusLevels()` (per-channel output
peaks), `SpeakerPositions()`, `ActiveVoices`, `TestSignal()` (a raw tone on one speaker, for wiring
checks), `DspTime` (schedule a sample-accurate start), and `extraListeners` — the *other* occupants,
so panning becomes a compromise across everyone in the room instead of exact for one head and wrong
for the rest.
