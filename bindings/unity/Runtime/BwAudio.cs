// BwAudio.cs — the manager. ONE per scene (singleton). Owns the engine handle, loads assets, and
// runs the CENTRALIZED per-frame push: all sources, then the listener, then one commit — so every
// block the audio thread sees is internally consistent (Unity does not order LateUpdate across
// components, so per-emitter pushes could commit a half-moved frame). See docs/integration.md.
using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace CaveAudio
{
    [DefaultExecutionOrder(-100)]
    public sealed class BwAudio : MonoBehaviour
    {
        public static BwAudio Instance { get; private set; }

        [Header("Engine")]
        public BwProfile profile = BwProfile.Binaural;   // inspector dropdown; maps 1:1 to the C enum
        public string layoutFile = "cave_layout.json";   // under StreamingAssets (cave/both)
        public uint sampleRate = 48000;
        public uint blockSize = 256;

        [Header("Listener")]
        public Transform listener;          // OptiTrack head rigid body, or the XR camera at a desk
        public bool feedListener = true;    // false => the core reads NatNet itself (cave/both)

        [Header("Reflections (load-time)")]
        public bool enableReflections = false;
        [Range(0.1f, 3f)] public float reverbSeconds = 1.0f;
        [Range(1, 2)]     public int   reflectionOrder = 1;

        [Header("Room box (load-time; optional acoustic geometry)")]
        public bool enableRoomBox = false;
        public Vector3 roomSizeMetres = new Vector3(3f, 3f, 3f);
        public string roomMaterial = "concrete";   // a bw_material_preset name

        IntPtr _eng;
        public IntPtr Handle => _eng;
        public bool Ready => _eng != IntPtr.Zero;

        readonly List<BwEmitter> _emitters = new();
        readonly Dictionary<string, uint> _sounds = new();

        void Awake()
        {
            if (Instance != null) { Destroy(gameObject); return; }   // native globals persist across play sessions
            Instance = this; DontDestroyOnLoad(gameObject);

            var cfg = new BwConfig {
                profile = profile,
                layoutPath = Path.Combine(Application.streamingAssetsPath, layoutFile),
                hrtfPath = null, sampleRate = sampleRate, blockSize = blockSize,
                trackInternal = !feedListener,
            };
            _eng = Bw.bw_create(in cfg);
            if (_eng == IntPtr.Zero) { Debug.LogError("[BwAudio] bw_create failed"); return; }

            // load-time configuration MUST precede bw_start
            if (enableReflections)
            {
                var rc = new BwReflectionConfig {
                    irSeconds = reverbSeconds, order = (uint)reflectionOrder,
                    numRays = 0, numBounces = 0, enabled = 1,
                };
                Bw.bw_reflections_config(_eng, in rc);
            }
            if (enableRoomBox)
            {
                uint m = Bw.bw_material_preset(_eng, roomMaterial);   // 0 (default) on an unknown name
                var faces = new[] { m, m, m, m, m, m };
                Bw.bw_scene_set_box(_eng, roomSizeMetres.x, roomSizeMetres.y, roomSizeMetres.z, faces);
            }

            if (Bw.bw_start(_eng) != 0)
            {
                Debug.LogError("[BwAudio] bw_start failed: " + Bw.LastError(_eng));
                Bw.bw_destroy(_eng); _eng = IntPtr.Zero; return;
            }
            Debug.Log("[BwAudio] started, backend=" + Bw.Backend(_eng));
        }

        /// <summary>Load a mono point-source asset (cached by path). Returns 0 on failure.</summary>
        public uint Load(string clip)
        {
            if (!Ready) return 0;
            if (_sounds.TryGetValue(clip, out var s)) return s;
            s = Bw.bw_load_sound(_eng, Path.Combine(Application.streamingAssetsPath, clip));
            if (s == 0) Debug.LogWarning("[BwAudio] load failed: " + clip + " (" + Bw.LastError(_eng) + ")");
            _sounds[clip] = s; return s;
        }

        /// <summary>Load a pre-encoded AmbiX soundfield for a world-locked bed (cached). Returns 0 on failure.</summary>
        public uint LoadAmbix(string clip)
        {
            if (!Ready) return 0;
            var key = "ambix:" + clip;
            if (_sounds.TryGetValue(key, out var s)) return s;
            s = Bw.bw_load_ambix(_eng, Path.Combine(Application.streamingAssetsPath, clip));
            if (s == 0) Debug.LogWarning("[BwAudio] ambix load failed: " + clip + " (" + Bw.LastError(_eng) + ")");
            _sounds[key] = s; return s;
        }

        /// <summary>Mint a material from a named preset (load-time). 0 = the built-in default.</summary>
        public uint MaterialPreset(string name) => Ready ? Bw.bw_material_preset(_eng, name) : 0;

        public void Register(BwEmitter e)   { if (!_emitters.Contains(e)) _emitters.Add(e); }
        public void Unregister(BwEmitter e) => _emitters.Remove(e);

        void LateUpdate()
        {
            if (!Ready) return;
            foreach (var e in _emitters) e.Push();                 // all sources first...
            if (feedListener && listener)
            {
                var p = Room.Pos(listener.position);
                var q = Room.Rot(listener.rotation);
                Bw.bw_set_listener_pose(_eng, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
            }
            Bw.bw_commit(_eng);                                    // ...then one atomic snapshot
        }

        void OnDestroy()
        {
            if (!Ready) return;
            Bw.bw_stop(_eng); Bw.bw_destroy(_eng); _eng = IntPtr.Zero;
            if (Instance == this) Instance = null;
        }
    }
}
