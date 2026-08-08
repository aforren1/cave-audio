# Engine integration

The game engines (Unity, Godot, Unreal) differ only in how transforms and
tracking are read and converted at the boundary. The audio code is identical.
No *rendered* audio crosses the boundary: the mix never routes through the game
engine, only control calls on the main thread. The one inbound exception is the opt-in
push-source feed (`bwa_source_push`): caller-generated PCM *into* the engine,
on the same control thread: a source feed, not a render path.

If a term in the binding surface is unfamiliar, [glossary.md](./glossary.md) defines it in one
line and points at the doc that owns it.

## Coordinate seam (the part that silently ruins spatial audio)

The core works in **room space: right-handed, +Y up, +Z forward, meters, origin on
the floor**. This is exactly OptiTrack/Motive's default streamed frame (the
ground-plane square defines the floor), so tracked rigid-body poses pass through
unchanged: no rotation, no translation. An identity head quaternion faces +Z, the
listener's right ear is at −X, and y is height above the floor.

The engine references the **array centroid** as its nominal listening point, so the
origin's exact spot is not audio-load-bearing.

Game engines do not share this frame. Convert every position and quaternion at the
boundary:

- **Unity** is left-handed, +Y up, +Z forward. Up and forward already agree, so the
  baseline conversion is a single axis flip: negate X (Unity identity rotation maps
  to room identity). Then apply the CAVE registration matrix that maps Unity world →
  room/Motive origin. Motive's ground plane is calibrated to deck center, so the real
  mapping lives in that matrix; the bare X-negation is only the handedness part.
- **Unreal** is left-handed, Z-up, centimeters. Convert to room meters and apply the
  registration matrix the same way.

Get this wrong and sources end up mirrored or rotated 90°. Budget time to verify the
conversion with a known-position test source before trusting anything else.

## Channel count

The engine's channel count is **the layout's speaker count** (4..26), not a constant.
The CAVE array is 26; a smaller rig loads its own file. Read it back with
`bwa_get_channel_count` (or `bwa_get_speakers`, which returns the same number) and size any
meter / speaker-gizmo / channel-test array from it. Never hard-code 26 in a binding.

The trap: **a failed layout load is not fatal**: `bwa_create` falls back to the
26-speaker default grid and only records the reason in `bwa_last_error`. On a smaller
install that silently changes the channel count too. Check `bwa_last_error` right after
`bwa_create` and fail loudly if the surveyed layout didn't load.

## There are two speeds of sound

Both bindings expose one (`speed_of_sound` in Godot, `speedOfSound` in Unity), and the layout
file carries another (`reference.speed_of_sound_mps`). They are both in meters per second and
they are not the same quantity. Setting either does nothing to the other.

- **The binding property** is `bwa_set_speed_of_sound`, the **propagation medium**. Doppler and
  reflection delays derive from it and glide to a change. 343 is air, 1480 is underwater, and
  small values exaggerate Doppler for slow motion. It is live, and it is yours to drive as an
  effect.
- **The layout field** is the **room air temperature the acoustic survey was measured at**, which
  scales the ranges `bwa_calibrate` solves positions from. See
  [`calibration.md`](./calibration.md) -> "Air temperature". The engine never reads it; only the
  calibration and authoring tools do. A binding has nothing to do with it.

If you are writing an underwater scene, set the binding property. If your room is not at 20 C,
pass `--temp` to `bwa_calibrate` once and the layout remembers it. Do not copy one into the other.

## Unity

**Implemented as a UPM package: [`bindings/unity/`](../bindings/unity/) (`com.brainworks.bw_audio`).**
See its [README](../bindings/unity/README.md) for install + plugin staging.

Four core pieces:

- **`Bwa`**: the P/Invoke layer, verified against the ABI (the Binding section below names the two calls it skips).
- **`Room`**: the coordinate seam.
- **`Engine`**: the singleton manager + centralized per-frame push.
- **`Emitter`**: the per-source component.

The rest of the Runtime folder: `AmbisonicBed` (a world-locked AmbiX soundfield
component over `bwa_bed_*`), `AcousticGeometry` (marks a mesh as
occluding/reflecting geometry with a material; `Engine` bakes them all into one
engine mesh at load), `MaterialAsset` (an acoustic material as a Project asset:
an engine preset or custom 3-band coefficients), `RoomConstraints` (draws the
surveyed room (truss, screen cube, projectors) in the scene view, from the same
`constraints.json` the C++ tools read; scene-view only, no audio), and
`ClipAttribute` (marks a string field as a StreamingAssets path). Editor-side:
`ProjectCheck` (warns when Unity's built-in audio is still enabled, with
one-click disable), `ClipDrawer` (the `[Clip]` file picker), and custom
inspectors for `Engine` / `Emitter` / `MaterialAsset` that hide settings which
don't apply and surface the engine's *survivable* mistakes (a layout file that
isn't where the engine will look, both reverb beds contending for the one tap, a
tracked listener with nothing to track), plus live backend / meters / voice count
while playing.

**The settings that fail quietly are the ones to watch.** An unknown material name is
not an error to the engine (`bwa_material_preset` returns the generic default and
notes it in `bwa_last_error`), and a failed layout load is not fatal (it falls back to
the 26-speaker grid). Both merely *sound wrong*. The binding therefore makes them
unrepresentable where it can (materials are a `BwaMaterialPreset` enum, file paths go
through a picker that lists what actually exists) and loud where it cannot.

The snippets below explain the design; read the package for the current code.

### Binding

```csharp
using System;
using System.Runtime.InteropServices;

internal static class Bwa {
    const string DLL = "bw_audio";
    const CallingConvention CC = CallingConvention.Cdecl;

    // Mirrors bwa_profile in bw_audio.h. MUST be a 4-byte int enum, NOT a string - the
    // C ABI's bwa_desc.profile is an enum, so marshalling it as a string would push a
    // pointer where the core expects 0/1/2 (undefined behavior / crash).
    public enum bwa_profile : int { Cave = 0, Binaural = 1, Both = 2 }

    [StructLayout(LayoutKind.Sequential)]
    public struct Config {
        public bwa_profile profile;                                      // matches bwa_profile (int) in the header
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string layoutPath; // const char* - string marshalling is correct here
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string hrtfPath;   // const char* or null
        public uint sampleRate, blockSize;
    }

    [DllImport(DLL, CallingConvention=CC)] public static extern IntPtr bwa_create(in Config c);
    [DllImport(DLL, CallingConvention=CC)] public static extern int  bwa_start(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern int  bwa_stop(IntPtr e);
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

The snippet shows the core calls. The shipped `Bwa.cs` binds every `BWA_API` function in
`bw_audio.h` except two Unity never uses: `bwa_set_output_capture` (an audio-thread callback) and
`bwa_render_block` (the manual-sink golden-render path). Beyond the core:

- **Voice management + scheduling**: `bwa_source_set_priority`;
  `bwa_source_play_at` + `bwa_get_dsp_time` (sample-accurate start),
  `bwa_source_play_loop` (`Emitter.PlayLoop`: intro→loop region),
  `bwa_source_stop_at` (`Emitter.StopAt`: click-free scheduled stop on the dsp clock),
  `bwa_source_queue` (`Emitter.Queue`: gapless chaining into the next sound);
  `bwa_get_active_voices`.
- **Transport**: `bwa_source_set_paused`, `bwa_set_paused` (global),
  `bwa_source_seek`, `bwa_source_is_playing`; `Emitter` polls the playing state
  each frame to fire its `onFinished` UnityEvent.
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
- **Ambisonic beds**: the full `bwa_bed_*` facade (`create` / `play` / `set_gain` /
  `set_orientation` (yaw/pitch/roll) / `stop` / `destroy`), plus the bed-named forms
  of the per-voice calls (`fade_to` / `fade_out` / `set_paused` / `seek` /
  `set_priority` / `set_group` / `is_playing`).
- **Materials / scene geometry**: `bwa_material_preset`, `bwa_material_define`,
  `bwa_scene_set_mesh_mat`, `bwa_scene_set_box`.
- **Situation tuning**: `bwa_tuning_preset` fills a complete tuning for `BWA_SETUP_SEATED` or
  `BWA_SETUP_ROAMING` and `bwa_apply_tuning` pushes every knob below in one call. Unity marshals
  the struct; Godot has `get_setup_tuning` (a Dictionary, so you can print it) and `apply_setup`.
  Start from the preset: this struct's zero is not its default and applying a zero-filled one is
  refused. See docs/api.md for what each field rests on.
- **Rendering A/B (live)**: `bwa_set_panner`, `bwa_set_dual_band`,
  `bwa_set_spcap_focus` (SPCAP's lobe sharpness and placement-correction density;
  0 or less on either reverts that one to the default, focus to whatever the array
  geometry implies), `bwa_set_spread_mode`, `bwa_set_max_re` + `bwa_set_max_re_split`,
  `bwa_set_decorrelation`, `bwa_set_near_spread`, `bwa_set_hole_spread`,
  `bwa_set_dual_band_cap` (inert unless `bwa_set_dual_band` is also on: it corrects that low
  band's ITD for the tracked head orientation, so it wants a real pose),
  `bwa_set_bed_renderer`, `bwa_set_tracked_align` + `bwa_set_tracked_align_guards`
  (re-references the per-speaker
  delay and gain trims onto the tracked head; output stage, so it re-solves nothing),
  `bwa_set_tracked_room_eq` (the bed *decoder* is create-time:
  `bwa_desc.bed_decoder`). `Engine` re-pushes these from `OnValidate`, so the
  inspector A/Bs them by ear in Play mode, which is what the engine makes them
  atomic for.
- **Listener**: `bwa_set_pose_prediction` (internal tracking only) and
  `bwa_set_extra_listeners`, the other occupants, pushed by `Engine` in the same
  frame block as the primary pose (they are commit-gated the same way).
- **Output stage + diagnostics**: `bwa_set_limiter` / `bwa_set_limiter_ceiling`,
  `bwa_get_bus_levels`, `bwa_set_test_signal`, `bwa_get_speakers` / `bwa_get_channel_count`
  (the layout's speaker count; see "Channel count" above; size meter/speaker
  arrays with it, never a hard-coded 26); and the engine-free ASIO driver
  enumeration `bwa_get_asio_driver_count` / `bwa_get_asio_driver_name`
  (`Engine.AsioDriverCount` / `AsioDriverName`; static, for a driver picker
  before `bwa_create`).
- **Assets**: `bwa_load_sound_streaming`, `bwa_load_ambix`, and the metadata
  readbacks `bwa_sound_get_frames` / `bwa_sound_get_channels`
  (`Engine.SoundFrames` / `SoundChannels`).
- **Procedural (push) sources**: `bwa_source_create_push`, `bwa_source_push`,
  `bwa_source_push_space`, `bwa_source_push_end`, surfaced as the **`PushEmitter`**
  component: a positional source you feed mono engine-rate floats instead of a
  clip (a synth, an engine model, a voice stream). It rides `Engine`'s centralized
  push like `Emitter`, but the engine refuses play/seek/pitch/queue on a push
  voice, so those aren't exposed. Feed from the main thread (the binding's control
  thread); see api.md.

Two seams a binding must not get wrong, both handled in `Room` (see below):
`bwa_bed_set_orientation` takes a **room-frame** yaw, and the X mirror **reverses the
sense of rotation**: pass a Unity euler angle straight in and the soundfield
spins the wrong way (`Room.YawRad` converts). The FDN's decay direction is a
**direction**, so it goes through `Room.Dir` (no registration translation), not
`Room.Pos`.

### Coordinate helper

```csharp
using UnityEngine;
public static class Room {
    public static Matrix4x4 UnityToRoom = Matrix4x4.identity;  // set once from CAVE registration
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
    readonly System.Collections.Generic.Dictionary<string,uint> _sounds = new();

    void Awake() {
        if (Instance != null) { Destroy(gameObject); return; }
        Instance = this; DontDestroyOnLoad(gameObject);
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
    public uint Load(string p) {
        if (_sounds.TryGetValue(p, out var s)) return s;
        s = Bwa.bwa_load_sound(_eng, System.IO.Path.Combine(Application.streamingAssetsPath, p));
        _sounds[p] = s; return s;
    }
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

### Unity-specific traps

- **Native plugins don't unload between Editor play sessions.** The DLL and its
  globals persist. `bwa_create` must not assume zeroed global state, and `bwa_destroy`
  must fully tidy up; otherwise you get "fine on first Play, crashes on second."
  If Domain Reload is disabled for fast enter-play, C# statics persist too; guard
  the `Instance` re-init.
- **Bypass `AudioClip`.** The core loads wav itself. Hand it `StreamingAssets`
  paths (real files on desktop builds) and keep audio assets out of Unity's
  import pipeline.

## Godot

**Implemented as a GDExtension: [`bindings/godot/`](../bindings/godot/).** Its
[README](../bindings/godot/README.md) is the manual: install, the node reference, the
traps; this section carries only what belongs in the cross-engine comparison.

No 1:1 binding layer: GDExtension needs no P/Invoke shim, so every call lives as a method
on the class owning its handle (`BwaEngine`, `BwaSource` → `BwaEmitter`/`BwaPushSource`,
`BwaBed`), plus scene-authored acoustics (`BwaMaterial`, `Bwa{Acoustic,Dynamic}Geometry`,
`BwaRoomBox`) and a live `BwaSpeakerView`. The by-ear playground ships inside the addon.

The seam is **simpler than Unity's, and Unity's advice must not be carried over**:

- Godot is right-handed and +Y up like room space, so there is **no mirror**. Positions
  pass through the CAVE registration transform and nothing else: if a source comes out
  mirrored, the registration is wrong, not the handedness conversion.
- The one conversion is the **facing convention**: Godot's forward is −Z, room identity
  faces +Z, so orientations pick up a 180° yaw (a quaternion swizzle, pinned by the
  `godot_room` ctest). This applies to *facings only*: listener pose, directivity axes.
  Mesh/instance transforms take registration alone; routing them through the facing helper
  spins every occluder 180° about Y.
- `bwa_bed_set_orientation`'s **sense of rotation is preserved**: no mirror means no
  reversal, where the Unity binding must flip it.

Godot has no `LateUpdate`, so the centralized per-frame push rides `process_priority`
instead (the `BwaEngine` node defaults high, so it samples after gameplay). Coherence never
depends on that ordering (one node pushes all sources, then the listener, then commits);
only freshness does.

One platform trap with no Unity analogue: the core opens files by OS path, and in an
exported build `res://` lives inside the `.pck` where no OS path exists. Ship layouts and
audio beside the executable or stage them into `user://` (`StreamingAssets`, one layer
down).

## Unreal (notes, not yet implemented)

Same library, same C ABI, linked as a module. The per-engine work mirrors Unity:

- A subsystem (for example, a `UGameInstanceSubsystem`) owns `bwa_create`/`start`/`stop`/
  `destroy` and runs the centralized per-frame push from a single tick, for the same
  ordering reason as Unity's manager.
- A scene component reads `GetActorLocation` (and head pose via a NatNet bridge or
  LiveLink) each tick, converts UE LH/Z-up/cm → room RH/m via the registration
  matrix, and pushes through the same calls.
- No FMOD/Unreal-audio involvement; the engine is purely a control client.

## Profile = desk-vs-CAVE, from the engine's view

At the desk: run `binaural` with `feedListener = true` and `listener` pointed at
the XR camera, driving the same emitters and calls. In the CAVE: run `cave` (or
`both`) with `feedListener = false`; the core takes head pose straight from
OptiTrack. Same build, same components, same control path; only the profile
changes.
