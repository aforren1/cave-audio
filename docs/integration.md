# Engine integration

The game engines (Unity, Godot, Unreal) differ only in how the binding reads and
converts transforms and tracking at the boundary. The audio code is identical.
No *rendered* audio crosses the boundary: the mix never routes through the game
engine, only control calls on the main thread. The one inbound exception is the opt-in
push-source feed (`bwa_source_push`): caller-generated PCM *into* the engine,
on the same control thread. It is a source feed, not a render path.

[Python](#python), [MATLAB and Octave](#matlab-and-octave) and [Web](#web) are the three bindings
here that are not game engines. None of them has a transform to convert, so the sections below on
coordinates do not apply to them. Python and MATLAB also have no frame to ride; the web binding
batches its frame itself. Everything else applies to all of them.

If a term in the binding surface is unfamiliar, [glossary.md](./glossary.md) defines it in one
line and points at the doc that owns it.

## The minimal example

Every binding ships the same demo, and it is the fastest way to hear that a binding works. A click
orbits the listener's head at ear height, 2 m out, one lap in about 6 s, in the binaural profile.
Run it with headphones on and the click should travel: in front of you, past your left ear, behind
your head, past your right ear.

The stimulus is a broadband click rather than a tone because the cues that place a source in
elevation and front to back live in the spectrum. A tone carries almost none of them, so an orbit
made with one reads as a level pan instead of as a trajectory.

Each copy synthesizes the same signal, so you can compare the bindings by ear: one 250 ms period
holding a 2 ms Hann-windowed noise burst at -12 dBFS peak, then silence, which comes to four clicks
a second. The noise is a written-out 32-bit LCG, not the language's own generator, because no two
languages' generators agree. [`examples/minimal.c`](../examples/minimal.c) is the reference copy.

| binding | the demo | run it |
|---|---|---|
| C | [`examples/minimal.c`](../examples/minimal.c) | `bwa_minimal` |
| Unity | [`bindings/unity/Samples~/MinimalOrbit/`](../bindings/unity/Samples~/MinimalOrbit/) | import the sample from the Package Manager |
| Godot | [`bindings/godot/demo/minimal.gd`](../bindings/godot/demo/minimal.gd) | `godot --path bindings/godot res://demo/minimal.tscn` |
| Python | [`bindings/python/examples/minimal.py`](../bindings/python/examples/minimal.py) | `uv run examples/minimal.py` |
| MATLAB or Octave | [`bindings/matlab/examples/minimal.m`](../bindings/matlab/examples/minimal.m) | `minimal` |
| Web | [`bindings/web/example/index.html`](../bindings/web/example/index.html) | `node bindings/web/example/serve.mjs`, then open the page and press Start |

The C, Godot, Python and MATLAB/Octave copies also take `--tests`, which forces the offline sink
and self-checks, so ctest runs them with no audio device attached. The Unity sample and the web page
are checked by hand.

## Coordinate seam (the part that silently ruins spatial audio)

The engine works in **room space: right-handed, +Y up, +Z forward, meters, origin on
the floor**. This is exactly OptiTrack/Motive's default streamed frame (the
ground-plane square defines the floor), so tracked rigid-body poses pass through
unchanged: no rotation, no translation. An identity head quaternion faces +Z, the
listener's right ear is at −X, and y is height above the floor.

The engine's nominal listening point is the layout's `listening_point_m`, or the **array
centroid** when the file declares none, so the origin's exact spot is not audio-load-bearing.

Game engines do not share this frame. Convert every position and quaternion at the
boundary:

- **Unity** is left-handed, +Y up, +Z forward. Up and forward already agree, so the
  baseline conversion is a single axis flip: negate X (Unity identity rotation maps
  to room identity). Then apply the CAVE registration matrix that maps Unity world →
  room/Motive origin. Motive's ground plane is calibrated to deck center, so the real
  mapping lives in that matrix. The bare X-negation is only the handedness part.
- **Unreal** is left-handed, Z-up, centimeters. Convert to room meters and apply the
  registration matrix the same way.

Get this wrong and sources end up mirrored or rotated 90°. Budget time to verify the
conversion with a known-position test source before trusting anything else.

## Channel count

The engine's channel count is **the layout's speaker count** (4..`BWA_MAX_CHANNELS`, which is
64), not a constant. The CAVE array starts with 24 and has room to grow; a smaller or larger rig loads its own file. Read
it back with `bwa_get_channel_count` (or `bwa_get_speakers`, which returns the same number) and size any
meter / speaker-gizmo / channel-test array from it. Never hard-code a speaker count in a binding.

The trap: **a failed layout load does not fail `bwa_create`**. `bwa_create` falls back to
the default grid (`BWA_DEFAULT_GRID`, 26 speakers) and records the reason in `bwa_last_error`. The failure
surfaces later, at `bwa_start`, which refuses the fallback with `BWA_ERR_LAYOUT`. Only
`layout_path = NULL` runs the default grid for real. In the window between the two calls
`bwa_get_channel_count` reports the default grid's 26, so on a smaller install the count looks wrong too.
Check `bwa_last_error` right after `bwa_create` and fail loudly if the surveyed layout
didn't load. Then the error names the layout file instead of the start call.

## There are two speeds of sound

The engine exposes one (`bwa_set_speed_of_sound`: `speed_of_sound` in Godot, `speedOfSound` in
Unity), and the layout file carries another (`reference.speed_of_sound_mps`). They are both in meters per second and
they are not the same quantity. Setting either does nothing to the other.

- **The binding property** is `bwa_set_speed_of_sound`, the **propagation medium**. Doppler and
  reflection delays derive from it and glide to a change. 343 is air, 1480 is underwater, and
  small values exaggerate Doppler for slow motion. It is live, and it is yours to drive as an
  effect.
- **The layout field** is `reference.speed_of_sound_mps`, the **speed of sound the acoustic survey
  assumed**. It is a speed, not a temperature, but you set it from one: `bwa_calibrate --temp`
  derives and records it, and it scales the ranges the tool solves positions from. See
  [`calibration.md`](./calibration.md) -> "Air temperature". The engine never reads it; only the
  calibration and authoring tools do. A binding has nothing to do with it.

If you are writing an underwater scene, set the binding property. If your room is not at 20 C,
pass `--temp` to `bwa_calibrate` once and the layout remembers it. Do not copy one into the other.

## Unity

**Implemented as a UPM package: [`bindings/unity/`](../bindings/unity/) (`com.brainworks.bw_audio`).**
See its [README](../bindings/unity/README.md) for install + plugin staging. Start with the
**Minimal orbit** sample ([`Samples~/MinimalOrbit/`](../bindings/unity/Samples~/MinimalOrbit/)),
which the Package Manager imports into your project: it is the orbiting click described above, on
an `Engine` plus a `PushEmitter`, and it needs no audio asset.

Quick start: **GameObject > BwAudio > CAVE Rig**, then move **Room Origin** to the CAVE floor
center. The item creates the `Engine`, pre-fills its Layout File from `Assets/StreamingAssets`
(`trims.json`, else `cave_layout.json`, else empty with a warning), binds the listener to the main
camera, adds the Room Origin as a child, and checks Unity's built-in audio. The README lists what
it creates. The sections below explain each part.

Four core pieces:

- **`Bwa`**: the P/Invoke layer, verified against the ABI (the Binding section below names the two calls it skips).
- **`Room`**: the coordinate seam.
- **`Engine`**: the singleton manager + centralized per-frame push.
- **`Emitter`**: the per-source component.

The rest of the Runtime folder:

- `SourceBase`: the abstract base of `Emitter` and `PushEmitter`. It holds the source lifecycle,
  the per-frame transform push and every `bwa_source_*` setting that applies to any voice.
- `AmbisonicBed`: a world-locked AmbiX soundfield component over `bwa_bed_*`.
- `PushEmitter`: a positional source you feed PCM (see "Procedural (push) sources" below).
- `AcousticGeometry` and `DynamicAcousticGeometry`: mark a mesh as occluding and reflecting
  geometry with a material. `Engine` bakes the static ones into one engine mesh at load, and again
  after any of them is enabled or disabled.
- `MaterialAsset`: an acoustic material as a Project asset, an engine preset or custom 3-band
  coefficients.
- `RoomConstraints`: draws the surveyed room (truss, screen cube, projectors) in the scene view,
  from the same `constraints.json` the C++ tools read. Scene view only, no audio.
- `RoomView`: draws the `RoomConstraints` boxes as wireframes in a build, for use inside the CAVE.
  It still draws when the engine fails to start.
- `SpeakerView`: one unlit marker per speaker, lit by its live output, for use inside the CAVE.
- `UnlitMaterial`: an internal helper that makes one unlit material for the active render
  pipeline. `RoomView` and `SpeakerView` use it.
- `ClipAttribute`: marks a string field as a StreamingAssets path.

Editor side:

- `CaveRig`: the **GameObject > BwAudio > CAVE Rig** menu item. `CaveRig.Create()` is the same
  setup from a script. It is a menu item and not a prefab: a prefab in a package is read-only, so
  a project would live in overrides of it.
- `ProjectCheck`: warns when Unity's built-in audio is still enabled, with one-click disable.
- `ClipDrawer`: the `[Clip]` file picker.
- Custom inspectors for `Engine`, `Emitter` and `MaterialAsset`. They hide settings that do not
  apply and surface the engine's *survivable* mistakes: a layout file that is not where the
  engine will look, both reverb beds contending for the one tap, a tracked listener with nothing
  to track. While playing they show the live backend, meters and voice count.
- A `RoomConstraints` inspector that shows what the constraints file actually loaded, because a
  missing or empty file otherwise looks like an empty room.

**The settings that fail quietly are the ones to watch.** An unknown material name is
not an error to the engine (`bwa_material_preset` returns the generic default and
notes it in `bwa_last_error`). A failed layout load is louder than it looks: `bwa_create`
falls back to the default grid, but `bwa_start` then refuses with `BWA_ERR_LAYOUT`. The binding therefore makes them
unrepresentable where it can (materials are a `BwaMaterialPreset` enum, file paths go
through a picker that lists what actually exists) and loud where it cannot.

The snippets below explain the design. Read the package for the current code.

### Binding

```csharp
using System;
using System.Runtime.InteropServices;

internal static class Bwa {
    const string DLL = "bw_audio";
    const CallingConvention CC = CallingConvention.Cdecl;

    // Mirrors bwa_profile in bw_audio.h. MUST be a 4-byte int enum, NOT a string - the
    // C ABI's bwa_desc.profile is an enum, so marshalling it as a string would push a
    // pointer where the engine expects an int (undefined behavior / crash). Mirror ALL four
    // values and their numbering: a short enum here silently renumbers the rest.
    public enum BwaProfile : int { Cave = 0, Binaural = 1, CaveSim = 2, CaveBoth = 3 }

    // TRUNCATED for the page: the shipped BwaDesc mirrors EVERY bwa_desc field, through
    // reserved[3]. Marshal a short struct by `in` and the engine reads past its end.
    [StructLayout(LayoutKind.Sequential)]
    public struct BwaDesc {
        public BwaProfile profile;                                       // matches bwa_profile (int) in the header
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string layoutPath; // const char* - string marshalling is correct here
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string hrtfPath;   // const char* or null
        public uint sampleRate, blockSize;
    }

    [DllImport(DLL, CallingConvention=CC)] public static extern IntPtr bwa_create(in BwaDesc c);
    [DllImport(DLL, CallingConvention=CC)] public static extern BwaResult bwa_start(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern BwaResult bwa_stop(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_destroy(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern IntPtr bwa_last_error(IntPtr e); // Marshal.PtrToStringUTF8(...) to read; null => no error
    [DllImport(DLL, CallingConvention=CC)] public static extern uint bwa_load_sound(IntPtr e,[MarshalAs(UnmanagedType.LPUTF8Str)] string p);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_unload_sound(IntPtr e, uint snd);
    [DllImport(DLL, CallingConvention=CC)] public static extern uint bwa_source_create(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_source_destroy(IntPtr e, uint s);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_source_set_pos(IntPtr e, uint s, float x, float y, float z);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_source_set_gain(IntPtr e, uint s, float g);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_source_play(IntPtr e, uint s, uint snd,[MarshalAs(UnmanagedType.I1)] bool loop);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_source_stop(IntPtr e, uint s);
    [DllImport(DLL, CallingConvention=CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_set_listener_pose(IntPtr e, float px,float py,float pz, float qx,float qy,float qz,float qw);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bwa_commit(IntPtr e);
}
```

The snippet shows the essential calls. The shipped `Bwa.cs` binds every `BWA_API` function in
`bw_audio.h` except two Unity never uses: `bwa_set_output_capture` (an audio-thread callback) and
`bwa_render_block` (the manual-sink golden-render path). Beyond those:

- **Voice management + scheduling**: `bwa_source_set_priority`;
  `bwa_source_play_at` + `bwa_get_dsp_time_frames` (sample-accurate start),
  `bwa_source_play_loop` (`Emitter.PlayLoop`: intro→loop region),
  `bwa_source_stop_at` (`Emitter.StopAt`: click-free scheduled stop on the dsp clock),
  `bwa_source_queue` (`Emitter.Queue`: gapless chaining into the next sound);
  `bwa_get_active_voices`.
- **Transport**: `bwa_source_set_paused`, `bwa_set_paused` (global),
  `bwa_source_seek`, `bwa_source_is_playing`, and `bwa_source_set_region`
  (`Emitter.SetRegionFrames` / `SetRegionSeconds`: bound a clip to a region, which both
  truncates a one-shot and sets a loop). See "Completion and loop events" below for how
  `Emitter` learns that a sound ended.
- **Completion and loop events**: `bwa_poll_ended` and `bwa_poll_looped` drive
  `Emitter.onFinished` / `onLoop` and `AmbisonicBed.onFinished` / `onLoop`. `Engine` is the
  only caller of either, and
  `Engine.EndedEventsDropped` / `LoopEventsDropped` report what the engine dropped. See
  "Completion and loop events" below.
- **Direct output-channel route**: `bwa_source_set_channel` (`SourceBase.Channel`,
  `Bwa.CHANNEL_AUTO` to go back to the panner). Sends one source out of one speaker with no
  spatial processing: the psychophysics ground-truth condition, and a wiring check you can run
  with real content. It is not `bwa_set_test_signal`, which injects a built-in tone after the
  per-speaker align stage and is therefore not level-comparable with a rendered source.
  Both bindings refuse an index outside `0 .. bwa_get_channel_count() - 1` and keep the channel
  the source already had. That matters because the property is readable: cache a refused value and
  the getter reports a speaker the voice is not on, which is how a reference source gets read as
  ground truth while it is still panned. `CHANNEL_AUTO` is the only negative that means anything.
  Every other negative is refused too, and refused before the source is live, because a negative
  needs no channel count to judge.
- **Mixing**: `bwa_source_fade_to` / `bwa_source_fade_out` (engine-side timed
  fades, no coroutines), `bwa_source_set_pitch`, `bwa_set_master_gain`, and mix
  groups (`bwa_source_set_group` + `bwa_group_set_gain` / `bwa_group_set_paused`).
- **Propagation effects**: `bwa_source_set_doppler`, `bwa_source_set_air_absorption`,
  `bwa_source_set_loudness_comp`, `bwa_source_set_spread`, `bwa_source_set_size`
  (metric radius), `bwa_source_set_extent` (`Emitter.Extent`: anisotropic
  width/height), and `bwa_source_set_attenuation_override`
  (`Emitter.SetAttenuationOverride`: per-source distance curve; `ref <= 0` clears).
- **Occlusion + directivity**: `bwa_source_set_occlusion` /
  `bwa_source_get_occlusion`, `bwa_source_set_occlusion_manual` (game-driven
  occlusion; **works without the Steam Audio build**),
  `bwa_source_set_directivity` (+ preset), `bwa_source_set_orientation`.
- **Reflections + pathing**: `bwa_reflections_config`, `bwa_set_reverb_gain`,
  `bwa_source_set_reverb` / `_reverb_send` / `_reverb_distance`,
  `bwa_source_set_pathing` (engine-level enable rides `bwa_desc.enable_pathing`);
  the phonon-free **FDN reverb** (`bwa_fdn_config`).
- **Ambisonic beds**: the full `bwa_bed_*` facade (`create` / `play` / `play_at` /
  `play_loop` / `set_gain` / `set_orientation` (yaw/pitch/roll) / `stop` / `destroy`),
  plus the bed-named forms of the per-voice calls (`stop_at` / `fade_to` / `fade_out` /
  `set_paused` / `seek` / `set_region` / `set_priority` / `set_group` / `is_playing`).
  A bed is a voice, so it also carries the two event feeds an emitter does: Unity's
  `AmbisonicBed.onFinished` / `onLoop`, Godot's `BwaBed` `finished` / `looped`.
- **Materials / scene geometry**: `bwa_material_preset`, `bwa_material_define`,
  `bwa_scene_set_mesh_mat`, `bwa_scene_set_box`.
- **Situation tuning**: `bwa_tuning_preset` fills a complete tuning for `BWA_SETUP_SEATED` or
  `BWA_SETUP_ROAMING`. `bwa_apply_tuning` pushes every knob below in one call. Unity marshals
  the struct; Godot has `get_setup_tuning` (a Dictionary, so you can print it) and `apply_setup`.
  Start from the preset: this struct's zero is not its default, and the engine refuses a
  zero-filled one. See docs/api.md for what each field rests on.
- **Offline evaluation (pure, no engine handle)**: `bwa_panner_gains_batch`,
  `bwa_bed_gains_batch`, `bwa_bed_gains_layout` (a layout FILE's bed decode, imported
  `bed_decode` included), `bwa_bed_decode_matrix` (that decode as an N x 16 matrix), and
  `bwa_spcap_focus_default`, for layout scoring from
  C# tooling. They never touch a running engine.
- **Rendering A/B (live)**: `bwa_set_panner`, `bwa_set_dual_band`, `bwa_set_dual_band_cap`,
  `bwa_set_spcap_focus`, `bwa_set_spread_mode`, `bwa_set_max_re` + `bwa_set_max_re_split`,
  `bwa_set_decorrelation`, `bwa_set_near_spread`, `bwa_set_hole_spread`,
  `bwa_set_bed_renderer`, `bwa_set_tracked_align` + `bwa_set_tracked_align_guards`,
  `bwa_set_tracked_room_eq`, `bwa_set_tracked_directivity` (the bed *decoder* is
  create-time: `bwa_desc.bed_decoder`).
  Semantics: [api.md](./api.md). `Engine` re-pushes these from `OnValidate`, so the
  inspector A/Bs them by ear in Play mode, which is what the engine makes them atomic for.
- **Listener**: `bwa_set_pose_prediction` (internal tracking only) and
  `bwa_set_extra_listeners`, the other occupants, pushed by `Engine` in the same
  frame block as the primary pose (they are commit-gated the same way).
- **Output stage + diagnostics**: `bwa_set_limiter` / `bwa_set_limiter_ceiling`,
  `bwa_get_bus_levels`, `bwa_set_test_signal`, `bwa_get_speakers` / `bwa_get_channel_count`
  (the layout's speaker count; see "Channel count" above; size meter/speaker
  arrays with it, never a hard-coded count); and the engine-free device enumeration
  `bwa_get_device_count` / `bwa_get_device_name` / `bwa_get_device_id` / `bwa_device_present`
  (`Engine.DeviceCount` / `DeviceName` / `DeviceId` / `Devices`, and Godot's
  `BwaEngine.get_devices(backend)`; static, for a device picker before `bwa_create`).
  Pass a concrete backend: `BwaSinkType.Asio` for ASIO drivers, `BwaSinkType.Wasapi`
  for Windows endpoints. The ASIO-only spelling
  (`bwa_get_asio_driver_count` / `_name`, `Engine.AsioDriverCount` / `AsioDriverName`)
  stays as a wrapper. The chosen string goes in `bwa_desc.device`, the field both
  bindings also still expose under its old name `asioDriver` / `asio_driver`, since it
  is one field with two spellings. Prefer the **id** when you persist a choice:
  friendly names collide between a headset and its dock. `sink_flags` carries four bits, all off
  by default. Three are exposed in both bindings (Unity's `[Flags] BwaSinkFlags` on
  `Engine.sinkFlags`, Godot's `BwaEngine.SINK_FLAG_*` on the inspector's `sink_flags`):
  `Exclusive` takes a WASAPI endpoint from every other application, including the VR runtime, so
  leave it off in a headset build; `ExactRate` fails the open rather than letting the OS resample;
  `TightBuffer` takes the smallest device buffer the backend can. The fourth, `DEEP_BUFFER`, is for
  the browser's AudioWorklet sink only, and neither game binding exposes it. See api.md's "Latency
  classes", which maps PsychPortAudio's classes 0 to 4 onto them.
- **Assets**: `bwa_sound_acquire` / `bwa_sound_release` (the shared, refcounted
  by-path tier) drive `Engine.Acquire`, and `Load` / `LoadStreaming` / `LoadAmbix` /
  `LoadFuma` are one call each over it with the matching `BwaLoadFlags`. The binding
  keeps no path-to-handle dictionary of its own: the cache key is `(path, flags)`
  inside the engine, so the same clip returns the same handle. Assets live for the
  engine's lifetime and `bwa_destroy` frees them, so nothing is released at teardown.
  `bwa_sound_acquire_async` + `bwa_sound_is_ready` are `Engine.AcquireAsync` /
  `IsSoundReady`, opted into per component with `Emitter.loadAsync` and
  `AmbisonicBed.loadAsync`: `Play` returns at once and the source stays silent until
  the data lands. Leave it off for the CAVE's
  normal path, which is load-time and synchronous. `Queue` and `PlayOneShot` refuse a
  still-decoding clip, because neither can be held. `bwa_sound_find` is `Engine.Find`:
  a pure by-path probe that never loads, never takes a reference, and answers 0 for a clip
  nobody asked for yet. Use it to ask whether something is resident without the side effect of
  making it so, and treat what it returns as borrowed (never release against it). Plus the
  metadata readbacks `bwa_sound_get_frames` / `bwa_sound_get_channels` (`Engine.SoundFrames` /
  `SoundChannels`, which probe `Find` first and load on a miss) and the explicit-tier
  `bwa_load_sound_streaming` / `bwa_load_ambix`.
- **Source configuration**: `bwa_source_preset` fills a complete `BwaSourceDesc` for a
  `BwaSourceKind`, and `bwa_source_apply` pushes the lot as ONE ring command.
  `SourceBase.BuildDesc` / `ApplyDesc` / `TryGetDesc` / `ApplyPreset` wrap it, the
  source inspector gets a preset picker, and the create-time push is a single apply
  instead of seventeen setters. The serialized inspector fields stay the source of
  truth: `ApplyPreset` writes them, `BuildDesc` reads them, and the per-property
  setters still push live on their own. Start from a preset, because this struct's
  zero is not its default and the engine refuses a zero-filled one. See
  [api.md](./api.md) for what each preset rests on.
- **Scene transitions**: `bwa_group_stop` and `bwa_stop_all` (`Engine.StopGroup` /
  `StopAll`): click-free, beds included, and neither resets the mixer. Both also
  drop the matching plays that are still waiting on an async decode, so nothing
  you stopped can start on you a moment later.
- **Procedural (push) sources**: `bwa_source_create_push`, `bwa_source_push`,
  `bwa_source_push_space`, `bwa_source_push_end`, surfaced as the **`PushEmitter`**
  component: a positional source you feed mono engine-rate floats instead of a
  clip (a synth, an engine model, a voice stream). It rides `Engine`'s centralized
  push like `Emitter`, but the engine refuses play/seek/pitch/queue on a push
  voice, so those aren't exposed. Feed from the main thread (the binding's control
  thread); see api.md.

Two seams a binding must not get wrong, both handled in `Room` (see below):
`bwa_bed_set_orientation` takes a **room-frame** yaw, and the X mirror **reverses the
sense of rotation**. Pass a Unity euler angle straight in and the soundfield
spins the wrong way (`Room.YawRad` converts, registration included, and `AmbisonicBed`
pushes it again whenever the registration moves). The FDN's decay direction is a
**direction**, so it goes through `Room.Dir` (no registration translation), not
`Room.Pos`.

### Coordinate helper

```csharp
using UnityEngine;
public static class Room {
    public static Matrix4x4 UnityToRoom = Matrix4x4.identity;  // the CAVE registration; Engine.roomOrigin sets it
    public static Vector3 Pos(Vector3 v) {
        v = UnityToRoom.MultiplyPoint3x4(v);
        return new Vector3(-v.x, v.y, v.z);                    // baseline LH->RH; real map in UnityToRoom
    }
    public static Vector3 Dir(Vector3 v) {                     // Pos() without the translation: axes/normals
        v = UnityToRoom.MultiplyVector(v);
        return new Vector3(-v.x, v.y, v.z);
    }
    public static Quaternion Rot(Quaternion q) {
        q = UnityToRoom.rotation * q;
        return new Quaternion(q.x, -q.y, -q.z, q.w);           // both frames face +Z: identity -> identity
    }
    public static float YawRad(float unityYawDegrees) {        // for bwa_bed_set_orientation
        Vector3 ahead = Rot(Quaternion.Euler(0f, unityYawDegrees, 0f)) * Vector3.forward;
        return Mathf.Atan2(ahead.x, ahead.z);                  // RH yaw about +Y - the mirror flips the sense
    }
}
```

### Register the room

The layout was measured in the tracker's frame, so the scene has to say where that frame is.
`Engine` has a **Room Origin** field for it. No code.

**GameObject > BwAudio > CAVE Rig** creates the origin as a child of the `Engine` object and
assigns it. You still place it (step 1) and turn it (step 2). By hand:

1. Create an empty GameObject. Put it at the room-frame origin: the CAVE floor center, on the floor.
2. Turn it so its forward (the blue arrow) points along the layout's +z, the room's front.
3. Assign it to `Engine` > **Room Origin**.

The listener, every source, the acoustic geometry and every gizmo are now relative to that object.
`Engine` draws a marker at the origin: a blue arrow for room front (+z) and a red line for room
right (-x). Face along the blue arrow. Red must be on your right.

The origin must match the tracker's frame, not the wall you call the front. Motive's +z often
points at the CAVE's back wall, and the measured layout inherits it. The symptom: the demo only
makes sense with the camera on the opposite side. Rotate the origin 180 degrees about Y. Do not
move the camera.

What to know:

- `Engine` reads the origin every frame before it pushes the listener and the sources, so a moved
  origin takes effect on the next frame. `SpeakerView` and `RoomView` follow it.
- A moved origin also re-bakes the static `AcousticGeometry` and pushes the `SceneSetGround` plane
  again. The re-bake is a BVH rebuild, so do not animate the origin in a scene with static
  geometry. The FDN decay direction is load-time and keeps the direction it had at start.
- Keep the origin level. `SceneSetGround` uses the origin's height only, and `Engine` warns once
  when the origin is tilted more than 1 degree.
- Scale is ignored, the origin's own and any parent's. Room meters are Unity meters. `Engine`
  warns once.
- With no origin assigned, nothing changes. `Room.UnityToRoom` stays identity unless your code
  writes it.
- The registration belongs to the `Engine` object. Destroying the `Engine` returns
  `Room.UnityToRoom` to identity, and so does entering Play mode, so one play session's
  registration cannot leak into the next.
- During play the `Engine` survives scene loads, and its registration survives with it. A new
  scene that brings its own `Engine` with a Room Origin hands that origin to the surviving
  `Engine`. A new scene without one keeps the previous registration: if the old origin was
  destroyed with its scene, `Engine` warns once. Parent the origin under the `Engine` to keep it.
- `AmbisonicBed` yaw is in scene terms, like every other angle you set in Unity: yaw 0 points the
  field's front along Unity world +z, not along the room's +z. Under an origin turned 180 degrees,
  yaw 0 faces the layout's -z, which is the screen you author against. The bed is world-locked: a
  moved origin turns it with the scene. To face the room's front instead, set the yaw to the
  origin's Y angle.
- `Engine` warns when the listener sits more than 1 m outside the speakers' bounding box (the
  array profiles) or the room box (when it is on). That is the symptom of a wrong Listener or Room
  Origin. Binaural without a room box never warns: at a desk the head can be anywhere. The check
  waits until the listener first moves or 5 s pass, so a head parked at the floor until tracking
  connects does not warn.

The math: `Room.UnityToRoom` becomes the inverse of the origin's position and rotation, which takes
a Unity world point into the origin's local frame. `Room.Pos` then applies the X mirror, once. The
origin's forward becomes room +z and its right becomes room -x.

### Clusters

A cluster runs the same scene on several render nodes, and exactly one of them may open the audio
device. Keep the CAVE Rig in the scene on every node and let each node decide in `Engine.Start`:

- **`Engine.OwnsAudioPredicate`**: a static `Func<bool>`. Set it before the scene loads, from a
  `[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]` method that asks
  your cluster toolkit whether this node is the server. Null means own audio.
- **`Engine.ownsAudio`**: the per-scene switch, on by default. An `Engine` owns audio only when
  both say yes. `Engine.OwnsAudio` reports the decision.

The decision is made once, in `Engine.Start`, and it cannot change while that `Engine` lives.
`Start` runs after every `Awake` in the scene, whatever the script execution orders are. A toolkit
that learns the node's role in its own `Awake` therefore has it in time, even when that `Awake`
shares the `Engine`'s execution order of -100 (MiddleVR's manager does). A predicate that throws
is logged and read as "not the owner". The predicate is cleared on every entry into Play mode.

`Engine.Awake` does two things only: it claims `Engine.Instance` and applies **Room Origin**. So
every node agrees on the room before the first `Start`, whoever owns audio. `Engine.Decided` turns
true when `Start` has run. Until then `Engine.Ready` is false on every node, and components wait.

If your toolkit learns the role later than `Awake`, save the scene with the `Engine` component
disabled and enable it when the role is known. Unity holds `Start` until the enable, components
keep waiting, and Room Origin is applied from `Awake` but not followed until then. Nothing is
held while the `Engine` is disabled: a `Play` or `Stop` made then does nothing, as with no `Engine`
in the scene. Auto-play still runs when the `Engine` starts.

```csharp
static class AudioOwner {
    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
    static void Install() => BwAudio.Engine.OwnsAudioPredicate = () => MyCluster.IsServer;
}
```

A node that does not own audio:

- creates no native engine and opens no device. `Engine.Ready` is false.
- still claims `Engine.Instance`, survives scene loads, and applies **Room Origin** every frame.
  The registration is the same on every node, so `Room.FromRoom` places visuals identically
  everywhere. Do not skip the `Engine` on client nodes: without it `Room.UnityToRoom` stays
  identity there.
- leaves every component a silent no-op. Emitters, beds, push sources and geometry create nothing,
  load nothing, log nothing, and stop waiting. `Engine.ComponentWaitFrames` stays flat there.
- shows the array in `SpeakerView` from the layout file, unlit, because there are no levels to read.

An owner whose start fails behaves the same way after its error: it stays the scene's `Engine` and
keeps the registration, so the nodes never disagree on the room.

The binding names no cluster toolkit. The one line that asks yours lives in your project.

The ownership switch is Unity-only for now. Godot's `BwaEngine` always owns audio.

### Creating things at run time

Add components to an active object and set their fields in the same frame. The first create waits
for `Start`, which runs before the next `Update`, so the fields you set after `AddComponent` are
the ones that play:

```csharp
var em = go.AddComponent<Emitter>();
em.clip = "sfx/door.wav";
em.loop = false;     // plays once, at Start, with these values
```

- A call that needs the voice before `Start` (`Play`, `Gain`, `Push`) creates it on the spot,
  with the fields as they are at that moment. A `Play` before `Start` replaces the auto-play.
- A playback-state call before `Start` (`Stop`, `FadeOut`, `Paused`, `StopAt`, `SeekFrames`,
  `SetRegionFrames`, `Queue`) runs the auto-play first and then acts on it, the order these calls
  always had. So a `Stop` in a manager's `Start` stays stopped, and `Paused = true` starts paused.
- Every later disable and enable creates and auto-plays in `OnEnable`, as before.
- `Emitter.clip` and `AmbisonicBed.clip` default to empty, which plays nothing and logs nothing.
- A component that comes up before the `Engine` waits for it, beds included.
- The `Engine` brings the native engine up in its `Start`, at execution order -100. A default-order
  `Start` finds it running. An `Awake` or `OnEnable` in the same scene load does not:
  `Engine.Ready` is false there, and `Engine.Load` returns 0. `Play`, `PlayAt`, `PlayLoop` and
  `Stop` on an `Emitter` or `AmbisonicBed` are held and run when the `Engine` starts. A held call
  whose component is inactive by then is dropped. Every other call is a no-op until then, as it is
  with no `Engine` in the scene. Field values are kept and applied at create. `PlayOneShot` returns
  false. An `Engine` call that is lost this way (`Load`, `SceneSetGround`, `SetGroupGain`) warns
  once. `Engine.Paused` and the settings with an inspector field are kept and applied at start.
- An `Engine` you add from a script is the same: `Ready` is false until its `Start`, which runs
  before the next `Update`.
- `Engine.StartNow()` brings the engine up at once, for code that needs `Ready` before the scene's
  `Start`. It runs once: later calls do nothing, Unity's own `Start` included. Check `Ready`
  afterwards. Do not call it from an `Awake` in a cluster scene. It asks `OwnsAudioPredicate` on
  the spot, and your toolkit's `Awake` may not have run yet.
- `AcousticGeometry` added, enabled or disabled after start joins or leaves the static mesh in the
  next `LateUpdate`, one rebuild per frame however many change. A disable and re-enable in the
  same frame still rebuilds. Geometry added after start misses a baked reflection probe grid
  (`bakeReflections`), which is computed at start.
- A rebuild is not free. It runs `FindObjectsByType`, copies every static mesh's vertex and
  triangle arrays on the main thread (about 0.5 ms at 2,000 triangles), and rebuilds the BVH on
  the engine's sim thread. That suits a door or a wall that appears. A prop toggled every frame
  makes on the order of 100 KB of garbage per frame. Give pooled or frequently toggled props
  `DynamicAcousticGeometry`, or keep them out of the acoustic scene. `Engine.StaticRebuilds`
  counts rebuilds, so churn shows.

**Clip paths.** A relative path is relative to `StreamingAssets`. An absolute path is used as it
is (`Engine.AssetPath`), so a clip your code generates can be written to
`Application.temporaryCachePath` and played by its full path. The ABI has no load from memory. For
audio you generate continuously, use `PushEmitter`.

### Bootstrap (centralized per-frame push)

Centralize the push in the manager. Do not let each emitter push in its own
`LateUpdate`: Unity does not guarantee `LateUpdate` order across components, so
per-emitter pushes risk committing a frame where the listener moved but some
sources hadn't. The manager pushes all sources, then the listener, then commits,
so every block the audio thread sees is internally consistent.

```csharp
[DefaultExecutionOrder(-100)]
public sealed class Engine : MonoBehaviour {
    public static Engine Instance { get; private set; }
    public BwaProfile profile = BwaProfile.Binaural;   // inspector dropdown; maps 1:1 to the C enum
    public Transform listener;          // OptiTrack head rigid body, or XR camera at desk
    public bool feedListener = true;    // false => core reads NatNet (cave/both)

    IntPtr _eng; public IntPtr Handle => _eng;
    readonly System.Collections.Generic.List<Emitter> _emitters = new();

    void Awake() {
        if (Instance != null) { Destroy(gameObject); return; }
        Instance = this; DontDestroyOnLoad(gameObject);
    }
    // Start, not Awake: the cluster ownership decision waits for every other Awake (see Clusters).
    void Start() {
        if (Instance != this) return;
        var cfg = new BwaDesc {
            profile = profile,
            layoutPath = System.IO.Path.Combine(Application.streamingAssetsPath, "cave_layout.json"),
            hrtfPath = null, sampleRate = 48000, blockSize = 256,
        };
        _eng = Bwa.bwa_create(in cfg);
        if (_eng == IntPtr.Zero || Bwa.bwa_start(_eng) != BwaResult.Ok) Debug.LogError("bw init failed");
        // internal tracking (Feed Listener off): connect the engine to the NatNet stream itself -
        // a runtime call, like any NatNet client (reconnect/disconnect any time)
        if (!feedListener) {
            var td = new BwaTrackerDesc { server = natnetServer, rigidBodyName = natnetRigidBody };
            if (Bwa.bwa_tracker_connect(_eng, in td) != BwaResult.Ok)
                Debug.LogError("tracker: " + Bwa.LastError(_eng));
        }
    }
    // No path->handle dictionary: bwa_sound_acquire IS one, keyed on (path, flags) inside the
    // engine, so the same clip returns the same handle with one more reference.
    public uint Load(string p) =>
        Bwa.bwa_sound_acquire(_eng, System.IO.Path.Combine(Application.streamingAssetsPath, p),
                              BwaLoadFlags.None);
    public void Register(Emitter e)   => _emitters.Add(e);
    public void Unregister(Emitter e) => _emitters.Remove(e);

    void LateUpdate() {
        if (_eng == IntPtr.Zero) return;
        foreach (var e in _emitters) e.Push();            // all sources first
        if (feedListener && listener) {
            var p = Room.Pos(listener.position); var q = Room.Rot(listener.rotation);
            Bwa.bwa_set_listener_pose(_eng, p.x,p.y,p.z, q.x,q.y,q.z,q.w);
        }
        Bwa.bwa_commit(_eng);                               // one atomic snapshot
    }
    void OnDestroy() {
        if (_eng == IntPtr.Zero) return;
        Bwa.bwa_stop(_eng); Bwa.bwa_destroy(_eng); _eng = IntPtr.Zero;
        if (Instance == this) Instance = null;
    }
}
```

### Emitter

```csharp
using System; using UnityEngine;
public sealed class Emitter : MonoBehaviour {
    public string clip = "sfx/footsteps.wav";   // under StreamingAssets
    public bool loop = true, playOnEnable = true;
    [Range(0,1)] public float gain = 1f;
    uint _src;
    IntPtr Eng => Engine.Instance ? Engine.Instance.Handle : IntPtr.Zero;

    void OnEnable() {
        if (Eng == IntPtr.Zero) { enabled = false; return; }
        _src = Bwa.bwa_source_create(Eng);
        Bwa.bwa_source_set_gain(Eng, _src, gain);
        Push();
        if (playOnEnable) Bwa.bwa_source_play(Eng, _src, Engine.Instance.Load(clip), loop);
        Engine.Instance.Register(this);
    }
    public void Push() {
        var p = Room.Pos(transform.position);
        Bwa.bwa_source_set_pos(Eng, _src, p.x, p.y, p.z);
    }
    void OnDisable() {
        if (Eng == IntPtr.Zero) return;
        Engine.Instance.Unregister(this);
        Bwa.bwa_source_destroy(Eng, _src);
    }
}
```

### Completion and loop events

The engine reports both as events. Poll them, do not watch a playing flag: a sound shorter
than your frame interval may never once read as playing, so an edge detector misses it, and a
looping voice never ends at all.

`Engine` drains both rings once per frame, right after `bwa_commit`. That order matters:
`bwa_commit` runs the pass that fills the rings, so a drain placed before it reports every
event a frame late. `Engine` is also the ONLY caller of `bwa_poll_ended` and
`bwa_poll_looped`. Both drains are engine-wide and destructive, so a second caller anywhere
eats events belonging to somebody else's sources and those callbacks never fire. `Engine`
routes each handle to the component that owns it and calls `Emitter.onFinished` or
`Emitter.onLoop`.

A bed is a voice, so the core reports a bed handle through the same two rings. `AmbisonicBed`
is not a `SourceBase` (a bed has no position, and the source registry exists to push one every
frame), so `Engine` keeps a second handle map for beds and consults it when a handle does not
belong to a source. Bed and source handles come from one pool, so a handle is a source's or a
bed's and never both. `AmbisonicBed.onFinished` and `onLoop` carry the same contract as the
emitter's, halt fallback included. Godot's `BwaBed` gets the same pair through the same second
registry, and its `finished` follows Godot's rule rather than Unity's: silent for `stop()`,
`fade_out()` and a group stop, and fired for `stop_at()`.

One thing the events cannot tell you: an explicit halt. `Stop`, `StopAt`, `FadeOut`,
`Engine.StopGroup` and `Engine.StopAll` all take the click-free stop path, which posts no
completion, and a stolen voice posts none either. Unity's `onFinished` has always fired for a
halt, so `Emitter` keeps a narrow is-playing edge for exactly that case, read after the drain.
A one-shot latch keeps a halt and a completion that describe the same end from both reporting
it. Godot's `BwaEmitter` is built the same way, with the same latch, except that its
`finished` signal deliberately stays silent for `stop()` and `fade_out()`.

**The latch is voided by the next play that BINDS, not by the next play.** A play held on an
async decode has not bound. The engine bumps a voice's play counter only at bind time, and that
counter is the gate that drops a completion straggling in from the previous play, so until it
moves the straggler is still deliverable and the latch is the only thing standing in its way.
Both bindings therefore keep the latch armed across a held play and clear it on the frame the
data lands. The case to picture is a stop that lands on the block a clip runs out in: the engine
posts a completion for that stopped voice anyway, and without the latch a play issued before the
next drain would let it through.

Both rings are bounded and drop the OLDEST entry when nothing reads them, so a rising
`EndedEventsDropped` means that many `onFinished` callbacks never fired. `LoopEventsDropped`
can rise for a second, harmless reason: a loop region shorter than a frame wraps more often
than any frame rate can read it. Pace trials off the wraps you receive.

### Driving rig-day trials from a Unity scene

The by-ear trials are the
[Stage 5 list in hardware-validation.md](./hardware-validation.md#stage-5-by-ear-checks). Build the
scene to mirror that list: one row per trial, a blind A/B, a recorded verdict. Every knob is on the
Unity binding; two cost a scene restart. The live ones are `Engine` fields that `OnValidate`
re-pushes in Play mode.

| Trial | Unity control | Cost |
| --- | --- | --- |
| Phantom against a real speaker | `Emitter.Channel` (`Bwa.CHANNEL_AUTO` to go back) | live |
| Preset baseline | `Engine.situation` + `ApplySituation()` | live |
| SPCAP focus | `spcapFocus` / `spcapDensity` | live |
| Hole-aware floor | `holeSpread`, 0 against 1 | live |
| CAP | `dualBandCap` (needs `dualBand`) | live |
| Tracked alignment | `trackedAlign` + dead zone / slew | live |
| Tracked directivity | `trackedDirectivity` | live |
| Tracked room EQ | `trackedRoomEq` | live |
| Panner | `panner` | live |
| max-rE and its band split | `maxRe`, `maxReSplit` | live |
| Air absorption | `Emitter.airAbsorption` | live |
| Spread mode, decorrelation | `spreadMode`, `decorrelation` | live |
| Bed decoder, AllRAD vs EPAD (vs an imported `Layout` one) | `bedDecoder` | restart |
| HRTF monitor quality | profile `Binaural` | restart |

- **Load twice, not four times**: `bedDecoder` is the only load-time half of the decoder and taper
  pair. Load AllRAD, then EPAD, and toggle `maxRe` inside each.
- **Build the reference row first**, then the three trials no instrument settles: SPCAP focus, the
  hole-aware floor, and CAP.
- **Encode the ordering rules**: refuse the tracked alignment trial before
  [Stage 2](./hardware-validation.md#stage-2-calibration-the-session) is accepted, and show its
  listening-point row as a null check.
- **Record the configuration beside every verdict** with `Engine.TryGetEngineTuning`, which reads
  what the engine has rather than what the inspector claims.
- Put spread mode and decorrelation last:
  [Stage 4b](./hardware-validation.md#stage-4b-instrumental-phantom-accuracy-bwa_validate) already
  settled them.

### Tests

The binding has a headless PlayMode suite in [`bindings/unity/test~/`](../bindings/unity/test~/). It
drives the real components against the real `bw_audio.dll`, so the completion and loop feeds, the
region and seek calls, and the bed event route are executed, not only compiled.

The same ctest entry first runs a small EditMode suite (`test~/EditMode`) for the editor tooling:
the CAVE Rig menu item's hierarchy, layout pre-fill, single-`Engine` rule and undo. It is a second
editor launch, because `-testPlatform` takes one value. Its floor is `MIN_EDIT_TESTS` in the driver.

Run it with the rest of the suite:

```
cmake -S . -B build -A x64 -DBWA_UNITY_EXE="C:/path/to/Editor/Unity.exe"
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo -R unity_playmode
```

Without `BWA_UNITY_EXE` the `unity_playmode` test is not registered, the same way the Godot scene
tests gate on `BWA_GODOT_EXE`. Unity is not a build dependency and CI has no editor.

Three choices make it work without hardware:

- **PlayMode, not EditMode, for the components.** `Engine.LateUpdate` is the pump and both event drains run in it.
  EditMode turns no frames, so it can never report a completion.
- **The offline sink.** The suite sets `sink = Null`, which is the shipping topology minus the
  device: a real audio thread, a real command ring, real event rings. `Manual` has no audio thread
  and has to be pumped with `bwa_render_block`, which the binding does not bind.
- **The staged DLL.** `bindings/unity/Runtime/Plugins/x86_64/bw_audio.dll` is build output. The
  `bw_audio` target copies it there after every build, so `cmake --build` followed by `ctest` runs
  against the engine that build produced. There is no copy step to forget.

The suite gets its determinism from ordering, not from timing. Several tests turn the `Engine`
component off for a stretch. `bwa_source_play` reaches the audio thread on its own, because only
`bwa_commit` is frame-gated, so a voice still starts, plays and ends while nothing commits, drains,
or polls is-playing. Turning the pump back on then gives exactly one pump against a known state.
That is what makes "a clip shorter than a frame fires `onFinished` exactly once" decidable instead
of a race the test happens to win.

The suite runs against a copy of the package, made fresh on every run. Unity treats a package
referenced by path as writable and deletes any `.meta` whose asset is missing, which includes the
`.meta` of every native library this machine did not build.

The folder is named `test~` because Unity ignores any folder whose name ends in `~`. Drop the suffix
and the package carries a second copy of the test assembly into every project that references it,
which is a duplicate assembly name and a compile error.

### Unity-specific traps

- **Native plugins don't unload between Editor play sessions.** The DLL and its
  globals persist. `bwa_create` must not assume zeroed global state, and `bwa_destroy`
  must fully tidy up. Otherwise you get "fine on first Play, crashes on second."
  If Domain Reload is disabled for fast enter-play, C# statics persist too. Guard
  the `Instance` re-init. `Room.UnityToRoom` is one of those statics: it resets to identity
  on every entry into Play mode, and `Engine` sets it again from **Room Origin**.
- **Bypass `AudioClip`.** The core loads wav itself. Hand it `StreamingAssets`
  paths (real files on desktop builds) and keep audio assets out of Unity's
  import pipeline.

### Android (standalone headsets)

The package ships one Android library, `Runtime/Plugins/Android/arm64-v8a/libbw_audio.so`, which
is the ABI every current standalone headset runs. Its `.meta` is committed with the rest, and it
enables the plugin for Android with CPU ARM64 and for nothing else, not the Editor. An installed
package is immutable, so those import settings cannot be fixed in the Inspector afterwards.

`DllImport("bw_audio")` needs no change. Mono resolves that name to `libbw_audio.so` on Android
and to `bw_audio.dll` on Windows, so the whole binding compiles and runs unchanged.

Two differences from the desk build, both in what the engine does rather than in what you call:

- **Output is stereo through AAudio.** Android carries no array transport, so a wide request falls
  to the offline sink. Use `BWA_PROFILE_BINAURAL`, which is the headphone render.
- **The pose comes from the headset.** Feed the head transform to `bwa_set_listener_pose` every
  frame from the XR rig, the same call the CAVE tracker drives. Nothing tracks over the network
  here.

The Android library carries **Steam Audio inside it**, statically linked, so binaural is the real
HRTF decode and automatic occlusion, pathing and the reflection bed all work, exactly as on the
desk. The package gains no file for it and the import settings do not change. What it costs is
size: the library is about 7 MB on arm64 rather than 0.4 MB, all of it inside the one `.so`.

### Linux and macOS (the other two desktops)

The package ships an engine library for each, beside the Windows one:

| platform | file | enabled for |
|----------|------|-------------|
| Linux    | `Runtime/Plugins/Linux/x86_64/libbw_audio.so` | the Linux editor and the `LinuxStandalone64` player, x86_64 |
| macOS    | `Runtime/Plugins/macOS/libbw_audio.dylib` | the macOS editor and `StandaloneOSX`, AnyCPU (the file is universal) |

Both `.meta` files are committed with the rest and both enable the **editor**, not only the
player, which the Android one deliberately does not. A Linux or macOS collaborator opens the
project and presses Play, and the plugin loads in their editor. An installed package is immutable,
so those settings ship correct or not at all.

`DllImport("bw_audio")` needs no change here either. Mono resolves that name to `libbw_audio.so`
on Linux and `libbw_audio.dylib` on macOS.

What differs is the device, and only the device:

- **Linux** outputs through JACK (a PipeWire desktop answers the same ABI) or ALSA. The array is
  reachable over AES67 into the Dante network, so this is a real rig path and not only a desk one.
- **macOS** outputs through CoreAudio, on the system output device by default, in the editor and
  in a `StandaloneOSX` player. The device runs at the engine's sample rate or the start fails,
  so the open sets the device's rate when the two differ
  ([api.md: Sink policy](./api.md#sink-policy)). The inspector's Sink popup offers `CoreAudio`
  for a macOS build target.

The macOS library is **universal** (x86_64 and arm64) and **unsigned**. Unity itself does not care,
but Gatekeeper does: a `.dylib` downloaded inside a tarball carries a quarantine flag, and the
editor then refuses to load it. Clear it once after installing, on the package folder Unity
imported into (`Library/PackageCache/...`) or on the tarball's extracted tree:
`xattr -dr com.apple.quarantine <folder>`. Shipping a signed build to anyone outside the lab needs
a Developer ID signature and notarization, which nothing here does yet.

## Godot

**Implemented as a GDExtension: [`bindings/godot/`](../bindings/godot/).** Its
[README](../bindings/godot/README.md) is the manual: install, the node reference, the
traps. This section carries only what belongs in the cross-engine comparison. Start with
[`demo/minimal.tscn`](../bindings/godot/demo/minimal.tscn), the orbiting click on a `BwaEngine`
plus a `BwaEmitter`.

### Quick start on the CAVE

Drag `addons/bw_audio/bwa_rig.tscn` into your scene. Move its `RoomOrigin` child to the CAVE
floor center. Rotate it 180 degrees about Y if the layout's front is the CAVE's back wall.

Then set two properties on the rig: **Layout Path**, to the `trims.json` or `cave_layout.json`
you copied into the project, and **Listener**, to your camera. Turn **Feed Listener** off
instead to let the engine read the tracker. To reach `RoomOrigin` on an instanced rig,
right-click the rig and turn on Editable Children.

What you get:

- The rig is a `BwaEngine` on the Cave profile, with a `RoomOrigin` Node3D child and
  `room_origin` pointing at it. It is the Godot counterpart of Unity's
  `GameObject > BwAudio > CAVE Rig`. A scene under `addons/` is an ordinary project file, so it
  needs no menu item and no editor plugin.
- `room_origin` is the counterpart of Unity's `Engine.roomOrigin`. `BwaEngine` sets
  `registration` to the inverse of the origin's global transform, with scale removed. The
  origin's local axes become the room axes: its blue arrow (+Z) is the layout's front and its
  red arrow (+X) is the layout's +x, the room's left. There is no mirror and no facing flip.
  Godot's -Z forward is a facing convention, and the origin is a frame.
- With the origin at `(2, 0, 1)`, a source at `(5, 0.5, 3)` is at room `(3, 0.5, 2)`. Yaw the
  origin +90 degrees and it is at `(-2, 0.5, 3)`. Yaw it 180 degrees and it is at
  `(-3, 0.5, -2)`. The `godot_rig` ctest pins the last two.
- `BwaEngine` compares the origin once per frame and recomputes only when it moved.
  `BwaSpeakerView` follows. So do the static mesh (`BwaAcousticGeometry` and the room box's
  walls), the ground plane and `BwaDynamicGeometry`: the engine re-bakes the static mesh in the
  same frame, one BVH rebuild, as Unity does. Do not animate the origin in a scene with static
  geometry. Three things do not follow: `set_extra_listeners` and the FDN decay direction,
  which are converted when called, and the room box's image-source reflections, which the
  engine anchors at the room origin by size alone. With any of them present, a change warns
  once and `is_registration_stale()` reads true.
- `BwaBed.orientation` is yaw, pitch and roll in radians, in scene terms: yaw 0 points the
  field's front along Godot world +Z, which is BEHIND a default Godot node (its forward is -Z).
  So `orientation.x = cam.rotation.y` points the front at the camera's back. To face the field
  where a node faces, call `set_yaw_from_basis(node.global_basis)`. The bed is world-locked, so
  a moved origin turns it with the scene. Pitch and roll are about the field's own axes and pass
  through unchanged, which is exact for a level origin. With no origin, this is the room-frame
  value it always was. Under an origin turned 180 degrees, earlier releases pointed yaw 0 at the
  room's front; set the yaw to `PI` to keep that. Unity's `AmbisonicBed` yaw works the same way.
- Scale is ignored, and a tilt past 1 degree is reported. A mirrored origin (a negative scale)
  is refused, and the registration keeps its last value. Each warns once at run time and shows
  on the node's warning marker in the editor. An array profile with no layout path shows there
  too.
- With `room_origin` empty, nothing changes. `registration` stays identity until code sets it.

Static geometry follows the tree after start. A `BwaAcousticGeometry` that enters joins the
static mesh, and one that leaves or is hidden drops out of it. Hidden means not occluding, the
way a disabled object is in Unity. `BwaEngine` rebuilds at most once per frame, and only when
the visible set differs from the one it last baked. When the last piece goes, it clears the
mesh. Each rebuild is a BVH rebuild, so give a prop that toggles often a `BwaDynamicGeometry`.
Moving a static piece, or changing its `mesh`, `source` or `material`, does not rebuild: the
change waits for the next rebuild, and the piece then jumps. `get_static_rebuilds()` counts
rebuilds, so churn shows.

The room box is still read once, at start. The image-source room can be set but never removed,
so a box that arrived late could not leave again. A `BwaRoomBox` added after start logs an
error and is ignored.

So an Autoload rig (Project Settings > Globals) now works for any content with no `BwaRoomBox`:
the engine starts before the scene, and the scene's geometry joins when it loads. Put a room
box in the rig itself, or put the rig in each scene that has one. Only one `BwaEngine` runs: a
second one is refused, logs an error and stays inert, and the first keeps its layout and room
origin. Unity hands a new scene's origin to the surviving `Engine`. Godot does not.

To create a node at run time, set its properties before `add_child`. `_ready` reads them there,
the Godot form of Unity's `AddComponent`-then-set pattern:

```gdscript
var em := BwaEmitter.new()
em.clip = "user://door.wav"
em.loop = false
add_child(em)   # plays once, with these values
```

**Clip paths.** `res://` and `user://` paths are globalized. An absolute path is used as it is,
so a clip your code generates can be written anywhere and played by its full path.

### The binding

No 1:1 binding layer: GDExtension needs no P/Invoke shim, so every call lives as a method
on the class owning its handle (`BwaEngine`, `BwaSource` → `BwaEmitter`/`BwaPushSource`,
`BwaBed`), plus scene-authored acoustics (`BwaMaterial`, `Bwa{Acoustic,Dynamic}Geometry`,
`BwaRoomBox`) and a live `BwaSpeakerView`. The by-ear playground ships inside the addon, with its default
array: `dome_24.json`, the same generated 24-speaker dome the native and web playgrounds load.
Pass a surveyed file as a user argument (`godot ... -- my_layout.json`) or put a
`cave_layout.json` beside the executable or in the project root to use that instead.

The convenience tier is three things. Assets: `BwaEngine` keeps no cache of its own. It acquires
through the core's `(path, flags)` cache, and keeps only a record of
the keys it acquired, so that `unload_sound_path` releases the references **this node** holds
and no others. `sound_get_frames` and `sound_get_channels` do not use that record at all: they
go through `bwa_sound_find`, the ABI's by-path lookup, which never loads on a miss.
Loading: `async_load` on `BwaEmitter` and `BwaBed` is an opt-in, off by default, and a play
against a still-decoding clip is held until the data lands rather than dropped. Sources:
`get_desc` and `apply_desc` carry `bwa_source_desc` as a Dictionary, for the same reason
`get_setup_tuning` carries the engine tuning as one, plus `reset_to_preset` and the static,
engine-free `BwaSource.get_preset`.

`BwaEmitter.finished` is driven by the same `bwa_poll_ended` drain Unity uses, and
`BwaEmitter.looped` by `bwa_poll_looped`. `BwaEngine` owns both drains, for the reason above,
and `BwaEngine.get_ended_this_frame()` / `get_looped_this_frame()` hand back THIS FRAME'S batch rather than
draining again. `get_ended_events_dropped()` and `get_loop_events_dropped()` carry the running
dropped totals. `BwaEmitter` gains `set_region_frames` / `set_region_seconds`, and every source
gains `set_channel` with `BwaSource.CHANNEL_AUTO`. `BwaBed` carries the same region pair,
plus `play_at` / `play_loop` / `stop_at` and the `finished` / `looped` signals. The unit lives in the region call's name for
the reason `seek_frames` carries one: the seconds spelling differs from the frames one by a
factor of the sample rate. `BwaEngine.get_dsp_time_frames()` / `get_dsp_time_seconds()` is the same
pair, and it exists because every host's dsp-time call is seconds: Unity's `AudioSettings.dspTime`
is a seconds `double`, and Godot's own `AudioServer` times are seconds too. Schedule with the frame
value. `channel` takes no suffix, because a channel index is not a quantity with two units.

**A Godot call that refuses an argument says so.** Every time-valued argument on the transport
surface (`play_at`, `play_loop`, `stop_at`, `seek_frames`, `set_region_frames` and their seconds
twins) reaches the C ABI unsigned, so a negative does not fail. It becomes an enormous positive:
-1 frames is 1.8e19, about twelve million years of dsp clock, which schedules a start for never
and leaves a voice that reads as playing and stays silent. All of them refuse a negative with a
warning instead, the same way `set_channel` refuses an index it cannot route.

The seam is **simpler than Unity's, so do not carry Unity's advice over**:

- Godot is right-handed and +Y up like room space, so there is **no mirror**. Positions
  pass through the CAVE registration transform and nothing else: if a source comes out
  mirrored, the registration is wrong, not the handedness conversion.
- The one conversion is the **facing convention**: Godot's forward is −Z, room identity
  faces +Z, so orientations pick up a 180° yaw (a quaternion swizzle, pinned by the
  `godot_room` ctest). This applies to *facings only*: listener pose, directivity axes.
  Mesh/instance transforms take registration alone. Route them through the facing helper
  and every occluder spins 180° about Y.
- `bwa_bed_set_orientation`'s **sense of rotation is preserved**: no mirror means no
  reversal, where the Unity binding must flip it.

Godot has no `LateUpdate`, so the centralized per-frame push rides `process_priority`
instead (the `BwaEngine` node defaults high, so it samples after gameplay). Coherence never
depends on that ordering (one node pushes all sources, then the listener, then commits);
only freshness does.

One platform trap with no Unity analogue: the engine opens files by OS path, and in an
exported build `res://` lives inside the `.pck` where no OS path exists. Ship layouts and
audio beside the executable or stage them into `user://` (`StreamingAssets`, one layer
down).

### Android export (standalone headsets)

The addon ships an Android library beside the Windows pair:
`addons/bw_audio/bin/libbw_audio_gd.android.template_release.arm64.so`, with the engine library
`libbw_audio.so` next to it as a manifest `[dependencies]` entry so the exporter copies it into the
APK too. The manifest names `android.template_release.arm64` and points `android.template_debug.arm64`
at the same file, which is what the Windows entries do and for the same reason.

The `lib` prefix on both files is required, not a convention: an APK extracts only `lib*.so` from
its `lib/<abi>/` directory, so a prefix-less extension is not on disk when Godot tries to load it.

Everything else is the desk build's behavior. Output is stereo through AAudio, because Android
carries no array transport; the headset pose goes to the engine through `bwa_set_listener_pose`,
which is `BwaEngine`'s listener push; and the `res://` trap above bites harder here, since an
exported APK has no OS path inside its `.pck` at all.

The Android library carries **Steam Audio inside it**, statically linked, so an exported APK gets
the real HRTF decode along with occlusion, pathing and the reflection bed. The manifest gains no
entry for it, because there is no second file: the cost is the engine library's own size, about
7 MB on arm64 rather than 0.4 MB.

### Linux and macOS (the other two desktops)

The addon carries an `editor` and a `template_release` library for each, so the extension loads in
a Linux or macOS **editor** as well as in an exported game. The manifest names them:

| platform | extension | engine library |
|----------|-----------|----------------|
| Linux    | `bin/libbw_audio_gd.linux.{editor,template_release}.x86_64.so` | `bin/linux/libbw_audio.so` |
| macOS    | `bin/libbw_audio_gd.macos.{editor,template_release}.universal.dylib` | `bin/libbw_audio.dylib` |

Two things to know about those paths:

- **Linux keeps its engine library one directory down.** Android's has the same file name, and one
  addon carries both. The extension finds it through an `$ORIGIN` run path, which covers the editor
  (`$ORIGIN/linux`) and an exported game (`$ORIGIN`, where the exporter copies every dependency
  flat beside the binary). macOS uses `@loader_path` and needs no subdirectory.
- **The macOS manifest keys carry no architecture.** `macos.editor`, not `macos.editor.universal`:
  Godot matches every dot-separated tag against the running platform's feature tags, and
  `universal` is not one. The file name keeps godot-cpp's own `.universal` spelling, which is the
  one place a key and a file name deliberately differ.

The device story is the engine's, not the binding's: Linux outputs through JACK or ALSA, and macOS
through CoreAudio, on the system output device by default. Everything the binding exposes
behaves the same on all four platforms. The `sink` property's inspector list names every
backend on every platform, because a scene travels between them. A backend the running platform
lacks fails the start with a message that says so, so leave the property on Auto in a project
that ships to more than one.

The macOS libraries are **unsigned and un-notarized**. An addon unzipped from a download carries a
quarantine flag and the editor refuses to load it, so clear it once:
`xattr -dr com.apple.quarantine addons/bw_audio`. Shipping to anyone outside the lab needs a
Developer ID signature and notarization.

## Python

**Implemented as a nanobind extension: [`bindings/python/`](../bindings/python/).** Its
[README](../bindings/python/README.md) is the manual: install, the two layers, the two experiment
shapes, the traps. This section carries only what belongs in the cross-binding comparison. Start
with [`examples/minimal.py`](../bindings/python/examples/minimal.py), the orbiting click; the two
experiment shapes are `offline_render.py` and `live_onset.py` beside it.

It is not a game-engine binding, so most of this page does not apply to it. There is no coordinate
seam, because there is no host engine to convert from: you pass room coordinates. There is no
centralized per-frame push, because there is no frame. What the binding adds instead is the two
shapes a psychophysics experiment takes, and the audience is PsychoPy
([backends.md](./backends.md) covers why).

Two layers, unlike Unity's one and Godot's one:

- `bw_audio._bwa` is the **raw layer**, one function for each C entry point, named by its C name
  without the `bwa_` prefix, so `bwa_source_play` is `_bwa.source_play`. Enums keep their C
  spelling without the type prefix; structs keep their C field names. A test parses the header and
  fails when a new ABI call arrives unbound, so this layer cannot drift.
- `bw_audio` is the **Pythonic layer**, pure Python over the raw one: an `Engine` context manager,
  `Sound`, `Source`, `PushSource`, `Bed` and `Listener` objects, and exceptions instead of return
  codes. It adds one semantic, the commit model: a commit-gated write commits itself, or lands
  with the rest of an `Engine.frame()` block, which is this binding's answer to the per-frame
  commit Unity and Godot make for you (the README's "Commit model" states the three modes).
  Both layers drive the same engine, so anything the Pythonic layer
  does not wrap you reach through `Engine.raw`.

**One ABI call is deliberately not bound.** `bwa_set_output_capture` installs a callback that the
engine runs on the **audio thread**, and an interpreter must never run there: taking the GIL inside
a device callback allocates, blocks on a lock, and runs arbitrary bytecode. The offline path is the
manual sink and `render_block`, which hands back the same post-limiter samples on the caller's own
thread. Every binding excludes this call, because its callback runs on the audio thread; Python
adds one more reason, a global interpreter lock to take.

**The threading contract is stricter to state and easier to break here**, because Python makes
threads cheap. Unity and Godot get the one-control-thread rule for free from their main loop;
Python does not. So `Engine` records the thread that created it and raises from any other, with an
opt-out for a caller who funnels their own calls through one thread. The raw layer has no guard.
The GIL is released around the calls the header marks as blocking or doing I/O, and around
`render_block`, so another Python thread runs while those are in flight.

**Units follow the same rule as the other two bindings**, and for the same reason: `seek_frames` /
`seek_seconds`, `set_region_frames` / `set_region_seconds`, `dsp_time_frames` /
`dsp_time_seconds`, `output_latency_frames` / `output_latency_seconds`. Python has no host engine
whose spelling could collide, but a reader moving between the bindings should not have to relearn
which one a bare name means.

**`Engine.bridge()` is the timing seam**: it sandwiches `bwa_host_time_ns` between two reads of
your clock, so the offset from `time.perf_counter` (or any experiment clock you pass) to the
dsp-sample clock is a measurement with a stated error bound rather than the estimator the game
bindings run per frame.

`render_block` returns a **read-only numpy view of the engine's own buffer**, valid until the next
call. That is the one place this binding hands out memory it does not own, and it is what makes an
offline render cost no copy. Pass `copy=True` when you keep the block.

The wheel carries the engine library inside the package, like the Unity package and the Godot
addon carry theirs. A release ships three wheels per platform, covering Python 3.10 and later: the
`cp312-abi3` one runs on 3.12 and every later version, and 3.10 and 3.11 get a wheel each. The
Linux wheels are built for `manylinux_2_28` and the macOS ones as `universal2`, so each file serves
more than the machine that built it.

## MATLAB and Octave

**Implemented as a MEX gateway: [`bindings/matlab/`](../bindings/matlab/).** Its
[README](../bindings/matlab/README.md) is the manual: install, the two layers, the commit model,
the timing recipe, coexisting with PsychPortAudio. This section carries only what belongs in the
cross-binding comparison. Start with
[`examples/minimal.m`](../bindings/matlab/examples/minimal.m), the orbiting click; the two
experiment shapes are `offline_render.m` and `live_onset.m` beside it.

Like Python it is not a game-engine binding, so there is no coordinate seam and no per-frame push.
The audience is Psychtoolbox, which is MATLAB or Octave, where PsychoPy is Python. The two
experiment shapes are the same two [backends.md](./backends.md) describes.

**One source, two binaries.** The gateway is written against the classic C MEX API, which Octave
implements, and not the MATLAB-only C++ Data API. MATLAB and Octave still need different binaries
(different MEX extensions, different compilers), so the build produces both and
`bwa.setup` picks the one for the interpreter it is running in. The C API pin is what keeps that a
packaging difference rather than a source fork.

**One MEX file with a subcommand string**, `bwa_mex('source_play', h, src, snd, false)`, rather
than one MEX per call. That is PsychPortAudio's own shape and what this audience reads without
being told. Each subcommand is a C entry point minus the `bwa_` prefix, so it maps onto the Python
raw layer one for one.

Two layers, like Python's:

- `bwa_mex` is the **raw layer**. Handles are `uint64` scalars, out-parameters become extra
  outputs, and a `bool` plus out-struct call returns `[]` on false. A test pins the subcommand
  count, so an ABI call added and left unbound goes red.
- The **`+bwa` package** is the class layer: `Engine`, `Source`, `PushSource`, `Bed`, `Sound`,
  `Listener` and `ClockBridge`, plus `Const` for the enums. It adds the same commit model Python
  adds, in the form MATLAB can express: a commit-gated write commits itself, or lands with the
  rest of a frame block. MATLAB has no `with`, so the block is `beginFrame`/`endFrame` and an
  `onCleanup`-based `frame()` that commits when you clear it.

**The same ABI call is not bound**, and for the same reason: `bwa_set_output_capture` runs its
callback on the audio thread. The subcommand exists and refuses with that reason, so a reader finds
the explanation where they looked.

**The threading contract is satisfied by construction here**, unlike Python's. A MEX call runs on
the interpreter's main thread, so the one-control-thread rule needs no guard. Parallel-pool workers
are separate processes and an engine handle does not travel to one. What does need care is
lifetime: the gateway is `mexLock`ed while any engine is live, so `clear mex` cannot unload it
under a running audio thread, and a `mexAtExit` handler stops and destroys whatever is live so
quitting never leaves a device open.

**`Engine.bridge()` is the timing seam**, the same one Python has: it sandwiches `bwa_host_time_ns`
between two reads of your clock, and reports the sandwich width as the error bound. It defaults to
Psychtoolbox's `GetSecs` when that is usable, because that is what the rest of the experiment times
against, and to a monotonic `tic`/`toc` otherwise.

`render_block` returns an `[nframes, channels]` `single` matrix, and it is a **copy** where
Python's is a view. An `mxArray` cannot alias memory the engine owns and overwrites. The copy costs
one `memcpy` and no transpose, because the engine's planar block already has MATLAB's column-major
layout for that shape.

**Octave's classdef support is partial**, and the class layer is written to the intersection: no
property validation syntax, no `arguments` blocks, no access lists on methods, and no `isvalid`.
The one construct that bites rather than fails to parse is reference cycles. Octave refcounts handle
objects and cannot collect a cycle, so `Engine.listener` is a DEPENDENT property that builds a
`Listener` on demand rather than a stored object pointing back at the engine. Stored, the engine
would outlive its variable and hold its device until the interpreter exited.

## Web

**Implemented as an ES module: [`bindings/web/`](../bindings/web/).** Its
[README](../bindings/web/README.md) is the manual: build, the two layers, the two topologies, the
per-frame path, the playground and XR pages. [web.md](./web.md) holds the decisions and the
measurements behind them. Start with [`example/index.html`](../bindings/web/example/index.html),
the orbiting click on a push source.

Two layers, like Python's: `src/raw.js` is the raw ABI, generated from the header, and
`src/engine.js` is the idiomatic layer with Python's auto-commit model. A page reaches both through
`src/client.js`, because the engine lives on a control Worker by default. Only the `"main"`
topology can open the AudioWorklet sink, because `new AudioContext()` needs a `Window`.

There is no host engine, so there is no coordinate seam: you pass room coordinates. The XR page
converts the WebXR reference space itself (a 180 degree yaw, not a mirror). There IS a frame: the
page stages the pose and every moved source in a `SharedArrayBuffer` slab, and `pushFrame()` sends
one message that the control side applies as one commit.

`bwa_set_output_capture` is absent from both layers. Tap the output with `outputNode()` instead.

## Unreal (notes, not yet implemented)

Same library, same C ABI, linked as a module. The per-engine work mirrors Unity:

- A subsystem (for example, a `UGameInstanceSubsystem`) owns `bwa_create`/`start`/`stop`/
  `destroy` and runs the centralized per-frame push from a single tick, for the same
  ordering reason as Unity's manager.
- A scene component reads `GetActorLocation` (and head pose via a NatNet bridge or
  LiveLink) each tick, converts UE LH/Z-up/cm → room RH/m via the registration
  matrix, and pushes through the same calls.
- No FMOD/Unreal-audio involvement; the engine is purely a control client.

## Profile: desk or CAVE, from the engine's view

At the desk: run `binaural` with `feedListener = true` and `listener` pointed at
the XR camera, and drive the same emitters and calls. In the CAVE: run `cave` (or
`cave_both`) with `feedListener = false`. The core takes head pose straight from
OptiTrack. Same build, same components, same control path; only the profile
changes.

### Test a rig scene at your desk

You do not have to change the profile by hand. Both bindings carry an opt-in device fallback: a
second profile, sink and device that the engine node uses when the primary device is not listed
on the machine.

| | Unity `Engine` | Godot `BwaEngine` |
|---|---|---|
| Fallback profile (None = off) | `fallbackProfile` | `fallback_profile` |
| Its sink, device, flags | `fallbackSink`, `fallbackDevice`, `fallbackSinkFlags` | `fallback_sink`, `fallback_device`, `fallback_sink_flags` |
| The gate (default on) | `fallbackOnlyInDevBuilds`: Editor and development builds | `fallback_only_in_editor`: a run from the editor |
| Is it in use | `Engine.UsingFallback` | `is_using_fallback()` |
| The profile that runs | `Engine.ActiveProfile` | `get_active_profile()` |
| The check | `Engine.DevicePresent(profile, sink, device)` | `BwaEngine.is_device_present(profile, sink, device)` |

Set the fallback profile to CaveSim or Binaural, leave its sink on Auto and its device empty, and
keep the rig settings as they are. At a desk the node logs one `DESK FALLBACK` line and opens the
default stereo output. On the rig nothing changes.

The node decides before `bwa_create`, with one engine call: `bwa_device_present(profile, sink,
device)` (docs/api.md, "Is the device here"). It reads the device lists and opens nothing. The
primary device is present when:

- **ASIO, WASAPI or CoreAudio, with a device name:** that backend lists the exact name or id.
- **ASIO, WASAPI or CoreAudio, no device name:** that backend lists at least one device.
- **JACK, ALSA, AAudio or Worklet, with a device name:** always, on a platform that has the
  backend. These backends do not look the string up in their own list. JACK takes a port
  pattern, ALSA any PCM string (`hw:0,0`, `plughw:1`, an asoundrc name), AAudio a numeric id. The
  list cannot call such a device missing, and a rig must never read as a desk. On a platform
  without the backend the device is missing: a Linux rig scene opened on a Windows desk takes the
  fallback.
- **JACK, ALSA, AAudio or Worklet, no device name:** the backend lists at least one device.
- **Auto and a device name:** a backend that Auto offers this profile lists it, as the engine's
  own Auto rule does. For Cave and CaveBoth that is ASIO on Windows, CoreAudio on macOS, JACK or
  ALSA on Linux. A WASAPI endpoint does not count for the array, because Auto never gives the
  array to WASAPI.
- **Auto, no device name:** one of those backends lists at least one device. A stereo profile
  on Windows is narrower: WASAPI must have a default render endpoint, and an installed ASIO
  driver does not count. The consequence: on a machine with ASIO only (no Windows output
  device), a scene whose primary profile is stereo with no device named now takes the fallback,
  although the start would have reached the ASIO driver. Name the driver in `device` to keep
  the primary.
- **Null or Manual:** always. They open no device.

CaveBoth opens two devices. Only the primary one, the array, is checked. The monitor is not.

What the check cannot see:

- **Whether the hardware is attached.** An ASIO driver is listed when it is installed. A laptop
  with the RME driver and no Digiface reads as present. ALSA lists `default` whenever the library
  loads, and CoreAudio always has an output device.
- **Whether the device will open.** Too few channels, a wrong rate, or another owner all read as
  present.
- **Outputs that this process cannot see.** WASAPI lists endpoints for the session the process
  runs in. A process started outside the interactive desktop (from a remote shell, a service or
  a scheduled task) can list none, likely for that reason. A stereo Auto request then reads as
  missing although the machine has outputs. The start line in the log names the device that
  opened (`backend=wasapi:<endpoint name>` in Unity, the same string in Godot), so compare it
  with a run started from the desktop.
- **Anything at all, with an empty device on an array profile.** The question is then "is any
  driver installed". Both inspectors warn about that setup.

A device that is listed and does not open is not a fallback. Under a named sink the start fails,
as before. Under Auto the engine lands on the silent offline sink, as before, and the binding
logs an error: the fallback did not trigger, the reason the open gave, and what to do (fix the
device on the rig, or set the profile by hand at that desk). The node does not try the fallback
after a failed `bwa_start`. On the rig a failed open is a fault you need to see.

The fallback can end silent too. When its own sink is Auto and nothing opens (no output on the
machine, or a fallback device name that nothing lists), the engine runs the offline sink. The
binding then logs a second error, `DESK FALLBACK has NO AUDIO either`, with the reason and what
to do: connect headphones or another output, or pick a fallback device the machine has. A
fallback whose sink is Null is silent on purpose and logs no error.

The gate, and what it costs:

- Unity takes the fallback in the Editor and in development builds. Godot takes it only in a
  run from the editor. Godot's gate is not `OS.is_debug_build()`, because "Export With Debug" is
  on by default and a rig often runs such an export.
- With the gate open, a missing device is no longer a failed start. A typo in `device`, or a
  driver that is not installed, becomes a headphone fallback with one log line. Unity logs that
  line as an error outside the Editor. Do not run the rig from a Unity development build with a
  fallback set.
- Clearing the toggle opens the gate in every build, with the same cost.

What does not carry over:

- Binaural skips the per-speaker align stage (trims, delays, correction filters, room EQ) by
  design, and it runs without the layout file. CaveSim keeps the layout and models the room (each
  speaker's distance, delay and directivity).
- The primary's sink flags. The fallback has its own.
- The tracker, in Unity. With `feedListener` off the rig reads the head from Motive. On the
  fallback the `Engine` skips `bwa_tracker_connect`, and the listener follows the `listener`
  Transform when one is assigned, or the main camera. One warning says which. In Godot a script
  calls `tracker_connect` itself, so guard that call with `is_using_fallback()`.
- A Unity cluster node that does not own audio opens nothing, the fallback included.
