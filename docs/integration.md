# Engine integration

The engine difference is entirely in how transforms and tracking are read and
converted; the audio code is identical. No audio crosses the boundary — only control
calls on the main thread.

## Coordinate seam (the part that silently ruins spatial audio)

The core works in **room space: right-handed, +Y up, +Z forward, metres** — exactly
OptiTrack/Motive's default streamed frame, so tracked rigid-body poses pass through
unchanged (an identity head quaternion faces +Z; the listener's right ear is at −X).
Engines do not share it. Convert every position and quaternion at the boundary:

- **Unity** is left-handed, +Y up, +Z forward. Up and forward already agree, so the
  baseline conversion is a single axis flip (negate X — Unity identity rotation maps
  to room identity), then apply the CAVE registration matrix that maps Unity world →
  room/Motive origin (Motive's ground plane is calibrated to deck center, so the real
  mapping lives in that matrix — the bare X-negation is only the handedness part).
- **Unreal** is left-handed, Z-up, centimeters. Convert to room meters and apply the
  registration matrix similarly.

Get this wrong and sources end up mirrored or rotated 90° — budget time to verify it
with a known-position test source before trusting anything else.

## Unity

**Implemented as a UPM package — [`bindings/unity/`](../bindings/unity/) (`com.cave.bwaudio`).**
See its [README](../bindings/unity/README.md) for install + plugin staging. Four pieces:
`Bw` (the P/Invoke layer, verified 1:1 against the ABI), `Room` (the coordinate seam), `BwAudio`
(the singleton manager + centralized per-frame push), and `BwEmitter` (the per-source component).
The snippets below are the design rationale; the package is the source of truth.

### Binding

```csharp
using System;
using System.Runtime.InteropServices;

internal static class Bw {
    const string DLL = "bwaudio";
    const CallingConvention CC = CallingConvention.Cdecl;

    // Mirrors BwProfile in bwaudio.h. MUST be a 4-byte int enum, NOT a string — the
    // C ABI's BwConfig.profile is an enum, so marshalling it as a string would push a
    // pointer where the core expects 0/1/2 (undefined behaviour / crash).
    public enum BwProfile : int { Cave = 0, Binaural = 1, Both = 2 }

    [StructLayout(LayoutKind.Sequential)]
    public struct Config {
        public BwProfile profile;                                      // matches BwProfile (int) in the header
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string layoutPath; // const char* — string marshalling is correct here
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string hrtfPath;   // const char* or null
        public uint sampleRate, blockSize;
        [MarshalAs(UnmanagedType.I1)] public bool trackInternal;
    }

    [DllImport(DLL, CallingConvention=CC)] public static extern IntPtr bw_create(in Config c);
    [DllImport(DLL, CallingConvention=CC)] public static extern int  bw_start(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern int  bw_stop(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_destroy(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern IntPtr bw_last_error(IntPtr e); // Marshal.PtrToStringUTF8(...) to read; null => no error
    [DllImport(DLL, CallingConvention=CC)] public static extern uint bw_load_sound(IntPtr e,[MarshalAs(UnmanagedType.LPUTF8Str)] string p);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_unload_sound(IntPtr e, uint snd);
    [DllImport(DLL, CallingConvention=CC)] public static extern uint bw_source_create(IntPtr e);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_source_destroy(IntPtr e, uint s);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_source_set_pos(IntPtr e, uint s, float x, float y, float z);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_source_set_gain(IntPtr e, uint s, float g);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_source_play(IntPtr e, uint s, uint snd,[MarshalAs(UnmanagedType.I1)] bool loop);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_source_stop(IntPtr e, uint s);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_set_listener_pose(IntPtr e, float px,float py,float pz, float qx,float qy,float qz,float qw);
    [DllImport(DLL, CallingConvention=CC)] public static extern void bw_commit(IntPtr e);
}
```

### Coordinate helper

```csharp
using UnityEngine;
public static class Room {
    public static Matrix4x4 UnityToRoom = Matrix4x4.identity;  // set once from CAVE registration
    public static Vector3 Pos(Vector3 v) {
        v = UnityToRoom.MultiplyPoint3x4(v);
        return new Vector3(-v.x, v.y, v.z);                    // baseline LH->RH; real map in UnityToRoom
    }
    public static Quaternion Rot(Quaternion q) {
        q = UnityToRoom.rotation * q;
        return new Quaternion(q.x, -q.y, -q.z, q.w);           // both frames face +Z: identity -> identity
    }
}
```

### Bootstrap (centralized per-frame push)

Centralize the push in the manager rather than letting each emitter push in its own
`LateUpdate` — Unity does not guarantee `LateUpdate` order across components, so
per-emitter pushes risk committing a frame where the listener moved but some sources
hadn't. The manager pushes all sources, then the listener, then commits, so every
block the audio thread sees is internally consistent. That is what makes the
moving-observer case correct.

```csharp
[DefaultExecutionOrder(-100)]
public sealed class BwAudio : MonoBehaviour {
    public static BwAudio Instance { get; private set; }
    public Bw.BwProfile profile = Bw.BwProfile.Binaural;   // inspector dropdown; maps 1:1 to the C enum
    public Transform listener;          // OptiTrack head rigid body, or XR camera at desk
    public bool feedListener = true;    // false => core reads NatNet (cave/both)

    IntPtr _eng; public IntPtr Handle => _eng;
    readonly System.Collections.Generic.List<BwEmitter> _emitters = new();
    readonly System.Collections.Generic.Dictionary<string,uint> _sounds = new();

    void Awake() {
        if (Instance != null) { Destroy(gameObject); return; }
        Instance = this; DontDestroyOnLoad(gameObject);
        var cfg = new Bw.Config {
            profile = profile,
            layoutPath = System.IO.Path.Combine(Application.streamingAssetsPath, "cave_layout.json"),
            hrtfPath = null, sampleRate = 48000, blockSize = 256,
            trackInternal = !feedListener,
        };
        _eng = Bw.bw_create(in cfg);
        if (_eng == IntPtr.Zero || Bw.bw_start(_eng) != 0) Debug.LogError("bw init failed");
    }
    public uint Load(string p) {
        if (_sounds.TryGetValue(p, out var s)) return s;
        s = Bw.bw_load_sound(_eng, System.IO.Path.Combine(Application.streamingAssetsPath, p));
        _sounds[p] = s; return s;
    }
    public void Register(BwEmitter e)   => _emitters.Add(e);
    public void Unregister(BwEmitter e) => _emitters.Remove(e);

    void LateUpdate() {
        if (_eng == IntPtr.Zero) return;
        foreach (var e in _emitters) e.Push();            // all sources first
        if (feedListener && listener) {
            var p = Room.Pos(listener.position); var q = Room.Rot(listener.rotation);
            Bw.bw_set_listener_pose(_eng, p.x,p.y,p.z, q.x,q.y,q.z,q.w);
        }
        Bw.bw_commit(_eng);                               // one atomic snapshot
    }
    void OnDestroy() {
        if (_eng == IntPtr.Zero) return;
        Bw.bw_stop(_eng); Bw.bw_destroy(_eng); _eng = IntPtr.Zero;
        if (Instance == this) Instance = null;
    }
}
```

### Emitter

```csharp
using System; using UnityEngine;
public sealed class BwEmitter : MonoBehaviour {
    public string clip = "sfx/footsteps.wav";   // under StreamingAssets
    public bool loop = true, playOnEnable = true;
    [Range(0,1)] public float gain = 1f;
    uint _src;
    IntPtr Eng => BwAudio.Instance ? BwAudio.Instance.Handle : IntPtr.Zero;

    void OnEnable() {
        if (Eng == IntPtr.Zero) { enabled = false; return; }
        _src = Bw.bw_source_create(Eng);
        Bw.bw_source_set_gain(Eng, _src, gain);
        Push();
        if (playOnEnable) Bw.bw_source_play(Eng, _src, BwAudio.Instance.Load(clip), loop);
        BwAudio.Instance.Register(this);
    }
    public void Push() {
        var p = Room.Pos(transform.position);
        Bw.bw_source_set_pos(Eng, _src, p.x, p.y, p.z);
    }
    void OnDisable() {
        if (Eng == IntPtr.Zero) return;
        BwAudio.Instance.Unregister(this);
        Bw.bw_source_destroy(Eng, _src);
    }
}
```

### Unity-specific traps

- **Native plugins don't unload between Editor play sessions.** The DLL and its
  globals persist. `bw_create` must not assume zeroed global state; `bw_destroy` must
  fully tidy up — otherwise "fine on first Play, crashes on second." If Domain Reload
  is disabled for fast enter-play, C# statics persist too; guard `Instance` re-init.
- **Bypass `AudioClip`.** The core loads wav itself; hand it `StreamingAssets` paths
  (real files on desktop builds) and keep audio assets out of Unity's import pipeline.

## Unreal (notes — not yet implemented)

Same library, same C ABI, linked as a module. The per-engine work mirrors Unity:
- A subsystem (e.g. a `UGameInstanceSubsystem`) owns `bw_create`/`start`/`stop`/
  `destroy` and runs the centralized per-frame push from a single tick, for the same
  ordering reason as Unity's manager.
- A scene component reads `GetActorLocation` (and head pose via a NatNet bridge or
  LiveLink) each tick, converts UE LH/Z-up/cm → room RH/m via the registration
  matrix, and pushes through the same calls.
- No FMOD/Unreal-audio involvement; the engine is purely a control client.

## Profile = desk-vs-CAVE, from the engine's view

At the desk: `binaural` + `feedListener = true`, `listener` pointed at the XR camera,
driving the same emitters and calls. In the CAVE: `cave` (or `both`) +
`feedListener = false`, the core taking head pose straight from OptiTrack. Same build,
same components, same control path — only the profile changes.
