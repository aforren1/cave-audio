# BwAudio - Unity package (`com.brainworks.bw_audio`)

Unity **control client** for the bw_audio spatial-audio engine. Unity is a *thin* client: it sends
control (source positions, triggers, listener pose) over the engine's C ABI. **No audio crosses the
boundary.** The engine renders the CAVE's speaker array (24 to start) over ASIO/Dante and a binaural monitor.

This is a UPM package: a verified P/Invoke layer (`Bwa`), the coordinate seam (`Room`), and
MonoBehaviours built around two of them - a scene manager (`Engine`) and a positional emitter
(`Emitter`).

## Replacing Unity audio (mental model)

**Turn Unity's built-in audio off** (see below) and use these instead. The mapping is close:

| Unity built-in audio | This package |
|---|---|
| `AudioListener` (on the camera) | `Engine.listener` (the tracked head transform) |
| `AudioSource` | `Emitter` |
| `AudioClip` (imported asset) | a **StreamingAssets file path** - pick it with the `[Clip]` field; the engine decodes `.wav`/`.flac`/`.mp3` itself and resamples at load |
| `AudioSource.Play()` / `Stop()` | `Emitter.Play()` / `Stop()` |
| `AudioSource.PlayOneShot()` | `Emitter.PlayOneShot()` |
| `AudioSource.volume` | `Emitter.Gain` (and `FadeTo` / `FadeOut` - the engine runs the fade, no coroutine) |
| `AudioSource.pitch` | `Emitter.Pitch` (glides; in-memory clips only) |
| `AudioSource.priority` | `Emitter.Priority` (a full voice pool steals the LOWEST-priority source) |
| `AudioSource.isPlaying` | `Emitter.IsPlaying` for "still going"; the `onFinished` and `onLoop` UnityEvents for "it ended" and "it wrapped" |
| `AudioListener.volume` | `Engine.MasterGain` |
| `AudioListener.pause` | `Engine.Paused` (every voice freezes; resume continues exactly) |
| `AudioMixerGroup` (ducking) | mix groups: `Emitter.group` + `Engine.SetGroupGain` / `SetGroupPaused` |
| `spatialBlend = 1` (3D) | always 3D - listener-relative DBAP across the speakers |
| `AudioSource.spread` | `Emitter.spread` (angular) or `sizeMeters` (a physical radius - holds its real size as the listener walks) |
| ambient / 2D music | `AmbisonicBed` - a world-locked AmbiX soundfield decoded to every speaker |
| procedural audio (`OnAudioFilterRead`) | `PushEmitter` - a positional source you feed mono PCM you generate |
| Audio Reverb Zone | the shared reverb bed: **Steam Audio reflections**, or the **FDN reverb** (no SDK needed) |
| early reflections (3rd-party) | `Emitter.earlyReflections`: image-source wall bounces off the room box, panned as point sources (no SDK needed) |
| occlusion (3rd-party) | `Emitter.occlusion`, ray-traced against the acoustic geometry - or `SetOcclusionManual` from game logic (no SDK needed) |
| Doppler / rolloff curves | `Emitter.doppler`, `airAbsorption`, `loudnessComp` (all physically derived from distance) |
| output device / AudioMixer | the engine (ASIO/Dante speaker array + binaural monitor); Unity's audio output is disabled |

The big difference: **audio files are raw files in `StreamingAssets`, not imported `AudioClip`s**. The
engine owns decoding, which also avoids Unity's 8-channel output cap entirely.

## Install

**Four platforms**: Windows x64, Linux x86_64, macOS (universal) and Android arm64-v8a. The
released package **ships the native engine** for every one of them, with import settings already
configured. Nothing to build, and one file per platform: Steam Audio is linked into the engine
rather than shipped beside it.

The array's production path is Windows, because ASIO is. Linux reaches it over AES67 through
JACK or ALSA; macOS outputs through CoreAudio, on the system output device by default; Android is
the headphone path on a standalone headset. The macOS library is unsigned, so clear its quarantine flag once after
installing a downloaded package: `xattr -dr com.apple.quarantine <the imported package folder>`.

### From a git URL

**Package Manager → `+` → Add package from git URL…**, then:

```
https://github.com/aforren1/cave-audio.git#unity/v<version>
```

That pins one release. Put the release you want in place of `<version>`: the tag is `unity/v`
plus the release number. To track the latest release instead, use the branch:

```
https://github.com/aforren1/cave-audio.git#unity
```

`unity` is a distribution branch whose **root** is the package: `package.json` at the top level, with
every platform's engine library already in it. That is the only layout UPM's git installer accepts.
CI republishes the branch on every `v*` tag, as one commit named `unity <version>`, and tags that
commit `unity/v<version>`. The plain `v*` tags point at `main`, where `package.json` is not at the
root, so `#v<version>` does not work and `#unity/v<version>` does. The tags start with the first
release after 0.23.2. Release 0.23.2 and older have no `unity/` tag: pin those by the commit's
hash (`#<commit>`).

Pin a release on a cluster. Every node must load the same package, and a branch ref resolves to
whatever was newest when each machine last fetched.

### From a release tarball

Grab `com.brainworks.bw_audio-<version>.tgz` from the
[Releases](https://github.com/aforren1/cave-audio/releases) page, then **Package Manager → `+` →
Install package from tarball…**. That's the whole install.

Either route pins by hand rather than resolving, so neither notifies you of upgrades. Take the newer
one and install it again. (This package isn't on a registry: it exists to drive one specific
CAVE, and the audience is people who already have the repo.)

### From source (developing the engine itself)

Point the manifest at your working tree, and the CMake build's POST_BUILD copy keeps the plugins fresh:

```json
"com.brainworks.bw_audio": "file:../../cave-audio/bindings/unity"
```

The DLLs are gitignored build output, so a git-URL install pointed at **`main`** will not work. You get
the C# with no engine behind it (`DllNotFoundException` on the first call), and `package.json` is not
at the repo root there either. That is what the `unity` branch above exists to fix: CI publishes a
built, root-level copy of the package to it. Against a working tree, use a local path.

The package has a headless PlayMode suite in `test~/`, wired into ctest as `unity_playmode`. It runs
the real components against the real DLL on the offline sink. Point CMake at an editor to register
it (`-DBWA_UNITY_EXE="C:/.../Editor/Unity.exe"`); without that the test is not registered, because
Unity is not a build dependency. See
[docs/integration.md → Tests](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md#tests) for what it covers and why it needs
PlayMode. `test~/` never ships: the pack script stages `Runtime`, `Editor` and `Samples~` only, and
Unity itself ignores any folder whose name ends in `~`.

> **License:** the engine is **GPLv3** (`bw_audio.dll` links the ASIO SDK under its GPLv3 option).
> Internal use never triggers copyleft: it is a *distribution* condition. But shipping a Unity app
> containing this DLL to third parties would place that app under GPLv3. See
> [`docs/build.md`](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/build.md) for the proprietary-ASIO alternative.

## Assets under StreamingAssets

The engine loads files itself, so **do not** use Unity `AudioClip`. Everything it reads lives in
`Assets/StreamingAssets/`. Every file field on a component is a path **relative to that folder**, and
the `[Clip]` picker only lists what is actually there, so you never type a path by hand. An
**absolute** path is used as it is, so a clip your code generates can be written to
`Application.temporaryCachePath` and played by its full path:

```
Assets/
  StreamingAssets/          <- create this folder; Unity does not by default
    cave_layout.json        <- surveyed speaker geometry
    sfx/footsteps.wav       <- your audio (.wav / .flac / .mp3)
```

- `cave_layout.json` - the surveyed speaker geometry. **Watch this one:** if it's missing or fails to
  parse, `bwa_create` does *not* fail. It falls back to its built-in 26-speaker grid and records why in
  `bwa_last_error`, and then `bwa_start` refuses that fallback with `BWA_ERR_LAYOUT`. So a bad layout
  does not mis-render, it stops the session. `Engine` logs an error at startup if this happens. The
  inspector also warns before you ever hit Play, and shows the exact absolute path the engine will
  look in. To run the built-in default grid deliberately (no surveyed rig yet), leave the `Engine`
  component's Layout File field **empty**. The **Binaural** profile drives no speakers, so it does
  not need the file: when the file is missing, `Engine` runs the default grid and logs one line. A
  file that exists still has to parse.
- `constraints.json` *(optional)* - the surveyed **room**: the speaker truss, the CAVE screen cube, the
  projectors. Add a **`RoomConstraints`** component and it draws them in the scene view: green truss,
  red keep-out, orange obstacles. Place content against the real room instead of guessing. It's the
  same file `bwa_layout_tool` and `bwa_playground` read, so the room has one source of truth. Don't
  re-author those boxes as Unity geometry. It affects nothing audible.
- **Your audio** - decoded and resampled to the engine rate at load.

### Seeing the room

`Engine` draws the **speaker array** in the scene view, and labels each speaker with its channel index.
With `RoomConstraints`, the whole CAVE (truss, screens, projectors, speakers) is then visible while you
work, in real meters.

Two details worth knowing. Stopped, the speaker positions come from the layout **file**. In Play mode
they come from the **engine**, and each speaker lights up with that channel's live output level, so a
dead or mis-wired speaker is obvious. `bwa_playground` uses the same trick. And because Play mode
draws what the engine is *actually* panning over, the array you see is the one being rendered.
A layout you named but that failed to load never gets that far: `bwa_start` refuses it with
`BWA_ERR_LAYOUT`, so read the console, not the gizmo. The built-in 26-grid shows up only when you
run with no layout path set at all.

Every gizmo goes through `Room.RoomToUnityMatrix()`, the inverse of the coordinate seam. So if the
array and the room land somewhere unexpected in the scene, your registration (`Engine`'s Room
Origin) is wrong. That is the cheapest possible check on the one setting that silently ruins
spatial audio.

### Watching the array from inside the CAVE

Gizmos are an editor feature. They don't render in a build. For live speaker activity **while you're
standing in the room**, add **`SpeakerView`**. It spawns one unlit marker per channel at the real
speaker's position, and each marker brightens and grows with that channel's output. Look toward a
speaker and you see it light up.

It's a viewer, not a control: it only reads `bwa_get_speakers` / `bwa_get_bus_levels`, never writes audio
state, and deleting it changes nothing you can hear. The markers are **unlit** deliberately. A CAVE is
dark, so the color computed is the color seen, and there is no lighting rig to set up. The meter has an
instant attack with a slow release, because the engine reports a per-*block* peak that strobes too fast
to read raw.

It shows the speaker bus. In the Binaural profile point sources and beds do not use that bus, so
the markers stay dark under a sound you can hear, and light only for reverb. That is expected. Use
it with Cave, CaveBoth or CaveSim.

Pair it with `Engine.TestSignal(channel, kind, gain)` for wiring checks: drive one raw output channel
and confirm the speaker that lights up is the one that makes noise.

To see the room itself in a build, add **`RoomView`** beside `RoomConstraints`. It draws the
truss, screens and projectors as unlit wireframes and follows the same show toggles. It does not
need an engine, so it still draws when the engine fails to start. It follows `Engine`'s Room Origin.
Call `RoomView.Rebuild()` after you write `Room.UnityToRoom` yourself, because `RoomView` bakes the
transform into the boxes when it builds them.

> **While the Unity Editor is open it holds `bw_audio.dll` loaded**, so the file is locked. A CMake
> rebuild will fail its POST_BUILD copy into `Runtime/Plugins/x86_64/`, and so will `pack.ps1`. Close
> the editor before rebuilding the engine. (Packing an *unchanged* DLL is fine - the script hashes it
> and skips the copy.)

## Disable Unity's built-in audio (do this once)

The engine **owns** the audio device. Turn Unity's built-in audio pipeline **off**. Left on, it opens
its own output device (wasted CPU, possible contention for the binaural monitor's headphones), and any
stray `AudioSource` plays the wrong path instead of the CAVE array.

Tick **Project Settings → Audio → "Disable Unity Audio"** (or run **Tools → BwAudio → Disable Unity
Audio**). The package's editor check warns on load if it's still enabled. Unity reads this flag at
startup, so you can't flip it from a runtime script. It's a one-time project setting.

## Use

Hear it working first: **Package Manager -> BwAudio -> Samples -> Minimal orbit -> Import**. The
sample is one MonoBehaviour that orbits a click around the listener's head for six seconds, on an
`Engine` plus a `PushEmitter`, and it synthesizes its own stimulus so it needs no audio asset.
Every other binding of this engine ships the same demo, which makes them comparable by ear (see
`https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md`, "The minimal example").

Then set up your scene in one step: **GameObject > BwAudio > CAVE Rig**, then move **Room Origin**
to the CAVE floor center. The same item is in the Hierarchy's right-click menu. It creates:

- **`BwAudio`**, with `Engine`. Layout File is `trims.json` if the top of `Assets/StreamingAssets`
  has one, else `cave_layout.json`. Subfolders are not searched. With neither, it is empty: the
  built-in default grid runs, and the console says so. Listener is the main camera of the scene
  the rig is in. With no camera tagged `MainCamera` there, it is left empty with a warning: Unity
  does not save a reference to a camera in another scene.
- **`Room Origin`**, a child, assigned to `Engine` > Room Origin. It starts at the world origin and
  faces +z. Move it to the floor center. Rotate it 180 degrees about Y if the layout's front is the
  CAVE's back wall. It is a child so that it survives scene loads with the `Engine`.
- **`Speaker View`**, a child with `SpeakerView`. The component is off. Turn it on to see the array
  in a build.
- **`Room`**, a child with `RoomConstraints` and `RoomView`, only when `constraints.json` exists.
  `RoomView` is off for the same reason.

The item then checks Unity's built-in audio and offers to disable it. A scene that already has an
`Engine` gets nothing new: the item selects the existing one and logs why. The item is off in Play
mode and refuses while a prefab is open for editing. One undo removes the rig. From a script, call `BwAudio.EditorTools.CaveRig.Create()`.

The manual steps, which are what the item does:

1. Add **`Engine`** to one GameObject (it's a singleton, `DontDestroyOnLoad`). Set the profile,
   the `listener` transform (your OptiTrack head rigid body or XR camera), and - optionally -
   reflections and a room box (both load-time). `sink`, `device` and `sinkFlags` pick the output
   device; `Engine.Devices(backend)` lists the exact strings `device` accepts. To let the engine
   read NatNet itself instead of following a transform, turn `feedListener` off and set
   `natnetServer` and `natnetRigidBody`. `Engine.TrackerStatus` reports whether poses arrive.
   A head that a VR toolkit creates at run time cannot be dragged in: leave `listener` empty and
   set **Listener Name** (`listenerName`) or `Engine.ListenerProvider`. The `Engine` looks a few
   times a second until the head exists, and again if it is destroyed.
2. Add **`Emitter`** to each sound source. Set the `clip`, `loop`, `gain`, and the spatial toggles
   (`occlusion`, `reflections`, `directivity`). Its transform drives the source position every frame.
   The inspector's **Preset** and **Apply** fill every field from one of the engine's source
   presets (`BwaSourceKind`: Default, Prop, Voice, Ambience, Ui). `loadAsync` decodes the clip on
   the engine's loader thread, for content that appears mid-session.
3. `Engine` runs the **centralized per-frame push** in `LateUpdate`: all emitters, then the
   listener, then one `bwa_commit`. The audio thread therefore never sees a half-moved frame, which
   is what makes the moving-observer case correct. Do not push from individual emitters.

Components added from a script can be configured in the same frame: `AddComponent<Emitter>()`,
then set `clip` and the rest, and the first play happens at `Start` with those values. See
"Creating things at run time" in [docs/integration.md](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md#creating-things-at-run-time).

### On a cluster

A cluster runs the same scene on several render nodes. Keep the rig on every node and set
`Engine.OwnsAudioPredicate` so that exactly one node opens the device. The `Engine` asks it in
`Start`, after every `Awake` in the scene, so your cluster toolkit's own `Awake` has run by then.
The binding names no toolkit: the lines that ask yours live in your project.
[docs/integration.md: Clusters](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md#clusters) has the detail for each point
below.

- **The owner is the node with the audio interface.** That is not always the cluster server. Ask
  the toolkit for the role, or ask for the device:
  `Engine.OwnsAudioPredicate = () => Engine.DevicePresent(profile, sink, device);`. Copy `device`
  from `Engine.Devices(sink)` on the machine that has the interface. That check
  reads device lists, so a node with the driver installed and no hardware answers true (see the
  list under "Test a rig scene at your desk" below). Exactly one node must answer true.
- **Audio state exists on the owner only.** On every other node `IsPlaying` is false, the
  playheads, `DspTimeFrames` and `BusLevels` read zero, and `onFinished` and `onLoop` never fire.
  Never drive visuals or trial flow from them. On the owner, forward the event through your
  toolkit's synchronized channel, and let every node act on the forwarded event.
- **Onsets.** Every node draws the flash on a synchronized frame, and only the owner has the
  sample clock. The owner predicts the host time at which that frame swaps and calls
  `Emitter.PlayAtHostTime(swapHostTimeNs)`. Host time is nanoseconds on `Engine.HostTimeNs`'s
  clock, which is QPC on Windows: a `Stopwatch` timestamp converts as
  `ticks * 1e9 / Stopwatch.Frequency`. The call adds
  `Engine.displayLatencySeconds` (measured: swap to light), subtracts the output latency, and
  schedules the play. It returns false, and plays nothing, on the other nodes and while the
  engine has no clock yet. Schedule three or more frames ahead.
- **Room Origin is the toolkit's tracking origin.** Assign the Transform your tracked devices are
  children of (in MiddleVR, the system center node). A wrong one rotates or offsets every source
  and nothing reports it. The `Engine` logs `registration: ...` at start on every node with the
  origin's pose, and on the owner the listener's room position. Check that position against the layout while you stand on a
  known spot. [Match the tracking origin](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md#match-the-tracking-origin) has
  the full check.
- **The head is usually created at run time.** Use Listener Name or `Engine.ListenerProvider`
  (step 1 above). Only the owner looks for it. A `/` in the name is a hierarchy path
  (`Rig/Head`).
- **Pin the package.** Install from `#unity/v<version>`, not `#unity`, so every node has the same
  release.

### Test a rig scene at your desk

A scene set up for the CAVE names the rig's device. At a desk that device is missing, and the
scene is silent (`sink` Auto) or fails to start (`sink` Asio). Do not edit the rig settings. Set
the fallback instead:

1. Keep `profile`, `sink` and `device` as the rig needs them. Put the rig's device name in
   `device` (a string from `Engine.Devices(BwaSinkType.Asio)`, read on the rig).
2. Set **Fallback Profile** to `CaveSim` or `Binaural`. Leave **Fallback Sink** on Auto and
   **Fallback Device** empty to get the machine's default stereo output.
3. Select the `Engine`. When the device is not listed on this machine, the inspector says that
   Play mode here takes the fallback.
4. Press Play. The console shows one `DESK FALLBACK` warning, the inspector shows a banner, and
   `Engine.UsingFallback` is true. `Engine.ActiveProfile` is the profile that runs.

On the rig the device is listed and nothing changes.

- **CaveSim** plays the array as virtual speakers on headphones. It uses the same layout file and
  keeps the room modeling (speaker distance, delay and directivity), so it needs the layout.
- **Binaural** is the direct headphone render. It skips the per-speaker stage (trims, delays,
  correction filters, room EQ) by design, and it starts without the layout file.
- **The tracker does not carry over.** With `feedListener` off, the rig reads the head from
  Motive. On the fallback the `Engine` does not connect the tracker. The listener follows the
  `listener` Transform when one is assigned, and the main camera when none is. One warning says
  which. Pose prediction is off there.
- The primary's `sinkFlags` do not apply to the fallback. It has `fallbackSinkFlags`.
- A cluster node that does not own audio opens nothing, the fallback included.
- The decision is made once, in `Start`. There is no second try after a failed start.

`Engine.DevicePresent(profile, sink, device)` is the check. It is the engine's
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
  a desk from the rig, and the inspector warns about it. Name the rig's device.
- The fallback can end silent too. If its sink is Auto and nothing opens (no output on this
  machine, or a fallback device name that nothing lists), the console shows a second error,
  `DESK FALLBACK has NO AUDIO either`, with the reason. Connect headphones or another output, or
  pick a fallback device this machine has. A fallback on the Null sink is silent on purpose and
  logs no error.

Where the fallback may run:

- **Fallback Only In Dev Builds** is on by default: the Editor and development builds only. A
  release build behaves as if no fallback were set.
- Where it may run, a missing device is no longer a failed start. A typo in `device`, or a
  driver that is not installed, becomes a headphone fallback with one log line. In the Editor
  that line is a warning. In a development build it is an error, because that build may be on
  the rig. Do not run the rig from a development build with a fallback set.
- Turning the toggle off allows the fallback in a release build, with the same trade.

### Ends and loop boundaries are events

`onFinished` and `onLoop` come from the engine's own event rings, not from watching a playing flag.
Watching the flag cannot work: `IsPlaying` is a per-block readback, so a clip shorter than a frame
may never once read as playing, and a looping voice never ends at all. `Engine` drains
`bwa_poll_ended` and `bwa_poll_looped` once per `LateUpdate`, right after the commit that fills
them, and routes each handle to the component that owns it. `AmbisonicBed` has both events too,
and the same `IsPlaying` read as an `Emitter`: true from a play until the soundfield ends or is
stopped, and still true while paused. `Paused`, `Pause()` and `UnPause()` work as they do on an
`Emitter`.

`Engine` is the **only** caller of either drain. Both are engine-wide and destructive, so a second
caller would eat events belonging to other components and their UnityEvents would never fire. Read
`Engine.EndedEventsDropped` and `Engine.LoopEventsDropped` for what the engine had to throw away
because nothing read it in time. The ended total should stay 0. The loop total can rise for a
harmless reason: a loop region shorter than a frame wraps more often than any frame rate reads it.

`onLoop` fires once per **wrap**, not once per frame. It is how you pace a trial off the content
rather than off `Time.deltaTime`. `Emitter.SetRegionFrames` / `SetRegionSeconds` set what it wraps
at: a play region loops a sub-range of the clip, or cuts a one-shot short. `AmbisonicBed` has the
same pair. Set the region **after** the play, because a play resets it.

### One speaker, no panning

`Emitter.Channel = n` sends a source out of exactly one output channel with no spatial processing,
and `Bwa.CHANNEL_AUTO` puts it back on the panner. That is the psychophysics ground-truth condition:
a real speaker to A/B a phantom against, played with whatever content you like. It is not
`Engine.TestSignal()`, which injects a built-in tone *after* the per-speaker align stage and is
therefore not level-comparable with anything the engine renders.

The routed voice keeps the whole output stage a panned voice gets (align trims and delays, room EQ,
master gain, limiter) and the route ramps in and out instead of clicking. Everything distance- or
direction-derived is suppressed while it is on and comes back the moment you go to `CHANNEL_AUTO`.
Mono point sources only. It is script-only on purpose: a route is a run-time experimental condition,
not scene configuration, so it is not serialized.

### Procedural audio - `PushEmitter`

When the audio isn't a file but something you **generate** - a software synth, an engine model, a
network voice stream - add a **`PushEmitter`** instead of an `Emitter` and feed it mono float PCM at
`Engine.sampleRate`. It's a full positional source: position, gain, spread, occlusion, reflections,
Doppler, groups and fades all work, and ride the same centralized push. The only difference from
`Emitter` is *where the samples come from*. There's no clip, and the engine **refuses**
play / seek / pitch / queue on a push voice, so those members simply aren't here. Godot splits
`BwaPushSource` off for the same reason. Its transform still drives the source position every frame.

The voice consumes from create (silence until the first push; an underrun renders silence without
losing your place). Push a frame or so ahead and **pace against `PushSpace`**. `PushEnd()` ends the
voice once the ring drains. That is one-way: a push voice is not restartable, so re-enable the
component for a fresh one. The ring holds 65536 frames (~1.37 s at 48 kHz).

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

Feed from Unity's **main thread**, like every other `bw_audio` call. Generate on a worker thread if
you must, but hand the finished buffers to `Push` from `Update`/`LateUpdate`.

## Acoustic geometry and materials (authoring)

Occlusion and reflections need the room's geometry. Three ways to author it:

- **Simple box** - tick `Enable Room Box` on `Engine`, set the size (meters) and a material preset.
- **Per-object meshes** - add **`AcousticGeometry`** to any object with a mesh (or set a low-poly
  `Mesh Override`) and assign a **material**. `Engine` bakes every enabled one (plus the box, if on)
  into the engine's scene at startup, and again in the frame after any of them is enabled or
  disabled: spawned, toggled, or its scene loaded or unloaded. It transforms each mesh from Unity
  world into room space. The scene view draws selected or `alwaysDrawGizmo` geometry as a cyan
  wireframe.
- **Moving meshes** - add **`DynamicAcousticGeometry`** instead, for a door, a lift, or a panel.
  It pushes position and rotation live (throttled by `positionEpsilon` and `angleEpsilon`), so
  occlusion and real-time reflections follow the mesh. It reads the scale once. Baked reflections
  do not follow it.

With no room box, `Engine.SceneSetGround(worldY, material, pressureRelease)` adds one ground
plane for the ground bounce. It replaces the room box, so leave `Enable Room Box` off.

Materials are project assets: **Create → BwAudio → Acoustic Material** (`MaterialAsset`) - either a
named engine preset (`concrete`, `glass`, …) or custom 3-band absorption / scattering / transmission.
Reference one asset from many objects; it's minted once. No material = the engine default.

> **Keep acoustic meshes SIMPLE** - tens to hundreds of triangles. The engine ray-traces them every
> frame; render meshes (thousands of tris) are far too heavy. Use a low-poly proxy via `Mesh Override`
> or a dedicated renderer-less object. `AcousticGeometry` is static: moving one does nothing until
> the next rebuild. Use `DynamicAcousticGeometry` for anything that moves.

### Coordinate seam

The engine works in **room space: right-handed, +Y up, +Z forward** (Motive's default streamed frame);
Unity is left-handed, +Y up, +Z forward. Up and forward agree, so the baseline conversion is a
single X mirror (`Room` does it in one place - Unity identity rotation maps to room identity).
**Register the room** with `Engine` > **Room Origin**: put an empty GameObject at the CAVE floor
center, turn its forward (blue arrow) along the layout's +z, and assign it. `Engine` then keeps
`Room.UnityToRoom` set from it every frame and draws the room's front and right at the origin. The
layout was measured in the tracker's frame, so match that frame: if the layout's +z is the back
wall, rotate the origin 180 degrees about Y. With no origin assigned, `Room.UnityToRoom` stays
identity unless your code writes it. **Getting this wrong silently swaps front/back or left/right.**
See [docs/integration.md → Register the room](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md#register-the-room).

## Threading and lifecycle notes

- All calls come from Unity's **main thread**; per-frame calls are non-blocking (they enqueue onto an
  SPSC ring). `bwa_create`/`bwa_start`/`bwa_load_*` allocate/do I/O - they run once at startup.
- **The reflection config and the FDN config are load-time** - `Engine` sets them between
  `bwa_create` and `bwa_start`. The engine rejects them after start (`bwa_last_error`). `Engine`
  bakes the room box and static geometry at start and when an `AcousticGeometry` is enabled or
  disabled, not when you edit one.
  It mints each material asset once and reuses it across scene loads, because the engine's
  material table holds 64 entries.
- **Native plugins persist across Editor play sessions.** `Engine` guards its singleton and tears the
  engine down in `OnDestroy`; the engine's own globals are re-init-safe.

## API surface

`Bwa` mirrors `include/bw_audio.h`: every `BWA_API` function has an entry point except two Unity
never uses. Those two are `bwa_set_output_capture` (an audio-thread callback) and `bwa_render_block`
(the manual-sink golden-render path). The MonoBehaviours cover the common path; for anything else,
call `Bwa.*` directly with `Engine.Instance.Handle`.

**Tune it by ear in Play mode.** The engine makes its rendering choices switchable *live* (atomic or
crossfaded), so `Engine` re-pushes them whenever you touch the inspector: the panner (DBAP /
SPCAP / VBAP), dual-band panning and dual-band CAP, SPCAP focus and density, the spread mode (LOBE /
MDAP / SPECTRAL), max-rE (and its band split), decorrelation, near-listener widening, the hole-aware
spread floor, tracked alignment (with its dead zone and slew guards), the bed renderer (MATRIX /
**PARAMETRIC** - a recorded soundfield you can walk through), tracked room EQ, tracked directivity,
the speed of sound, the reverb and early-reflection levels, master gain, and the limiter. That is an
A/B you can *hear*, not a restart.

To set a whole configuration at once instead of knob by knob, pick `situation` (Seated or Roaming)
and call `Engine.ApplySituation()`. It copies the engine's preset into the inspector fields and
pushes them, so the inspector shows what is set. `Engine.TryGetEngineTuning` reads back what the
engine runs. `Bwa.bwa_tuning_preset` and `Bwa.bwa_apply_tuning` are bound too, but the next
inspector edit overwrites what you set through them.

Load-time settings (a scene restart to change): the profile, the layout, the sample rate and block
size, the output device, the reverb bed (Steam **or** FDN - they share one reverb tap, so pick one),
the bed *decoder* (AllRAD / EPAD / Layout, the layout file's imported `bed_decode`), sound pathing,
the room box, and the static acoustic geometry.

Everything the CAVE needs but a desktop engine doesn't is on `Engine`: `ChannelCount` (the layout's
speaker count - **size meter arrays with it, never hard-code a speaker count**), `BusLevels()` (per-channel
speaker-bus peaks: what the device received in the array profiles, the virtual speakers in CaveSim,
and only the reverb taps in Binaural, where it reads zero under a dry source or a bed),
`SpeakerPositions()`, `ActiveVoices`, `TestSignal()` (a raw tone on one speaker, for wiring
checks), `DspTimeFrames` (schedule a sample-accurate start), and `extraListeners` - the *other* occupants,
so panning becomes a compromise across everyone in the room instead of exact for one head and wrong
for the rest.

For AV sync, `DspTimeFramesAt(realtime)` and `RealtimeAt(dspSample)` map between the dsp clock and
`Time.realtimeSinceStartupAsDouble`. `HostTimeNs` reads the engine's host clock, and
`OutputLatencyFrames` / `OutputLatencySeconds` give the device latency.
`Emitter.PlayAtHostTime` and `AmbisonicBed.PlayAtHostTime` schedule against a host time and fold
in `displayLatencySeconds` and the output latency; `Engine.TryDspTimeFramesForHostTime` is the
conversion alone. It is the same mapping as `DspTimeFramesAt`, and both are as good as the
engine's clock stamps: `GetClockModel` reports their jitter. For dropouts, `GetHealth`
returns the counters and `Xruns` is the short form. `GetHealth` returns false when nothing can
observe a dropout (not started, the manual sink, a driver with no valid sample position). Then a
zero count proves nothing.

## Maintainers

### Releasing (maintainers)

The canonical whole-repo release process (version model, steps, dev versions) lives in
[docs/build.md → Releasing](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/build.md#releasing). This is the Unity-package view of it.

**The GitHub Release is the distribution** - there's no registry, no token, and nothing to keep in sync.

To cut a release, fill in the CHANGELOG's `[Unreleased]` section, then run the helper. The helper
rolls that heading to the version, commits, and creates the matching annotated tag:

```
powershell -File tools/release.ps1 0.3.0            # roll CHANGELOG, commit, tag v0.3.0 (no push)
powershell -File tools/release.ps1 0.3.0 -DryRun    # preview the roll; change nothing
powershell -File tools/release.ps1 0.3.0 -Push      # ...and push, which triggers the CI release
```

**The tag is the version.** CI stamps it into the packaged `package.json`, so there's no manifest
field to bump and nothing to keep in sync. The committed `version` stays `0.0.0-dev`, the honest value
for an unreleased checkout. Prefer `git tag v0.3.0` by hand? That works too; the helper just also does
the CHANGELOG roll. CI packs on *every* run, so a broken package fails the build rather than the
release. On a tag it also creates the Release with the stamped version.

**Two release assets matter here**: this package (`com.brainworks.bw_audio-<ver>.tgz`) and the
Windows engine on its own (`bw_audio-win64-<tag>.zip` - dll/lib/header/tools, for C/C++ consumers
and the CAVE machine). See [docs/build.md](https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/build.md#continuous-integration) for the
rest.

Locally:

```
cmake --build build --config RelWithDebInfo         # produces the DLLs
powershell -File tools/upm/pack.ps1                 # -> dist/com.brainworks.bw_audio-<version>.tgz
```

The pack refuses to run unless all four platforms' engine libraries are in `Runtime/Plugins/`. A
Windows build stages only `bw_audio.dll`, so hand the other three over with `-AndroidFrom`,
`-LinuxFrom` and `-MacFrom` (a directory each), or build for those platforms first (each build
stages its own library there).

Without a `-Version`, the local pack derives its version from `git describe`, for example
`0.2.0-dev.32.gcf83c8e` (32 commits past `v0.2.0`). A dev tarball is therefore traceable to a commit
instead of a flat placeholder. Pass `-Version` (or let CI pass the tag) for a release build.

Two things that will bite if forgotten:

- **New file in the package? Run `tools/upm/gen-meta.ps1`.** Every asset must ship a committed `.meta`,
  or its GUID is regenerated per-project and scenes lose their script references. `pack.ps1` refuses to
  build a tarball with one missing.
- **Keep the engine bundle a `.zip`, and the package the only `.tgz` on a release.** Every UPM registry
  that could ever serve this speaks the npm protocol, and keys on a single publishable tarball. One
  `.tgz` per release keeps that door open at zero cost.

Not on a registry, and deliberately so: this drives one specific CAVE, so the audience is
people who already have the repo. A tarball costs them one click. The tarball *is* the artifact a
registry would serve, so listing it later is a config change, not a rebuild.
