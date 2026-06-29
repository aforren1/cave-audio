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
            if (Instance != null) { Destroy(gameObject); return; }   // a working manager already owns the engine

            var cfg = new BwConfig {
                profile = profile,
                layoutPath = Path.Combine(Application.streamingAssetsPath, layoutFile),
                hrtfPath = null, sampleRate = sampleRate, blockSize = blockSize,
                trackInternal = !feedListener,
            };
            _eng = Bw.bw_create(in cfg);
            if (_eng == IntPtr.Zero) { Debug.LogError("[BwAudio] bw_create failed"); return; }   // Instance NOT claimed

            // load-time configuration MUST precede bw_start
            if (enableReflections)
            {
                var rc = new BwReflectionConfig {
                    irSeconds = reverbSeconds, order = (uint)reflectionOrder,
                    numRays = 0, numBounces = 0, enabled = 1,
                };
                Bw.bw_reflections_config(_eng, in rc);
            }
            SetupScene();   // acoustic geometry + optional room box -> the engine's scene (load-time)

            if (Bw.bw_start(_eng) != 0)
            {
                Debug.LogError("[BwAudio] bw_start failed: " + Bw.LastError(_eng));
                Bw.bw_destroy(_eng); _eng = IntPtr.Zero; return;     // Instance NOT claimed -> another manager can try
            }

            // Only claim the singleton once we have a live engine, so a failed init leaves Instance free.
            Instance = this; DontDestroyOnLoad(gameObject);
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

        // ---- acoustic scene baking (load-time) ----------------------------------------------------
        // Collect every BwAcousticGeometry (+ the optional room box) into ONE mesh and hand it to the
        // engine. The engine's scene is a single static mesh, so this is a one-time bake before start.
        void SetupScene()
        {
            var geos = FindObjectsOfType<BwAcousticGeometry>();
            bool haveGeo = geos != null && geos.Length > 0;
            if (!haveGeo && !enableRoomBox) return;

            // simple path: just a box, nothing else -> the engine's own box helper (inward normals)
            if (!haveGeo && enableRoomBox)
            {
                uint mb = Bw.bw_material_preset(_eng, roomMaterial);
                var faces = new[] { mb, mb, mb, mb, mb, mb };
                Bw.bw_scene_set_box(_eng, roomSizeMetres.x, roomSizeMetres.y, roomSizeMetres.z, faces);
                return;
            }

            // combined path: geometry (+ optional box), all baked into one mesh
            var verts = new List<float>(); var tris = new List<int>(); var triMat = new List<uint>();
            var cache = new Dictionary<BwMaterialAsset, uint>();
            uint Resolve(BwMaterialAsset a)
            {
                if (a == null) return 0;                       // default material
                if (cache.TryGetValue(a, out var t)) return t; // mint each asset once
                t = a.Resolve(_eng); cache[a] = t; return t;
            }

            if (enableRoomBox)
                AddBox(verts, tris, triMat, roomSizeMetres, Bw.bw_material_preset(_eng, roomMaterial));
            foreach (var g in geos)
            {
                var mesh = g.ResolveMesh();
                if (mesh == null) { Debug.LogWarning("[BwAudio] BwAcousticGeometry with no mesh: " + g.name); continue; }
                AddMesh(verts, tris, triMat, mesh, g.transform.localToWorldMatrix, Resolve(g.material));
            }
            if (tris.Count == 0) return;
            Bw.bw_scene_set_mesh_mat(_eng, verts.ToArray(), verts.Count / 3, tris.ToArray(), tris.Count / 3, triMat.ToArray());
            Debug.Log($"[BwAudio] acoustic scene: {verts.Count / 3} verts, {tris.Count / 3} tris, {cache.Count} material(s)");
        }

        // Append a Unity mesh, transformed local -> world -> room space. Whether the winding must be
        // reversed to keep front faces depends on the SIGN of the full linear map's determinant (the
        // Z-flip in Room.Pos, UnityToRoom, AND the object's own scale — a negative/mirrored scale flips
        // winding by itself), not just the Z-flip. Room.ReversesWinding folds all three together.
        static void AddMesh(List<float> verts, List<int> tris, List<uint> triMat, Mesh mesh, Matrix4x4 l2w, uint mat)
        {
            int baseIdx = verts.Count / 3;
            var mv = mesh.vertices;
            foreach (var lv in mv) { var p = Room.Pos(l2w.MultiplyPoint3x4(lv)); verts.Add(p.x); verts.Add(p.y); verts.Add(p.z); }
            bool reverse = Room.ReversesWinding(l2w);
            var mt = mesh.triangles;
            for (int i = 0; i < mt.Length; i += 3)
            {
                int a = baseIdx + mt[i], b = baseIdx + mt[i + 1], c = baseIdx + mt[i + 2];
                if (reverse) { tris.Add(a); tris.Add(c); tris.Add(b); } else { tris.Add(a); tris.Add(b); tris.Add(c); }
                triMat.Add(mat);
            }
        }

        // Append an origin-centred box (room metres), inward-facing normals (the listener is inside).
        static void AddBox(List<float> verts, List<int> tris, List<uint> triMat, Vector3 size, uint mat)
        {
            float hw = size.x * 0.5f, hh = size.y * 0.5f, hd = size.z * 0.5f;
            var v = new[] {
                new Vector3(-hw,-hh,-hd), new Vector3(hw,-hh,-hd), new Vector3(hw,hh,-hd), new Vector3(-hw,hh,-hd),
                new Vector3(-hw,-hh, hd), new Vector3(hw,-hh, hd), new Vector3(hw,hh, hd), new Vector3(-hw,hh, hd) };
            int baseIdx = verts.Count / 3;
            foreach (var p in v) { verts.Add(p.x); verts.Add(p.y); verts.Add(p.z); }   // already room space
            int[][] quad = { new[]{0,4,7,3}, new[]{1,2,6,5}, new[]{0,1,5,4}, new[]{3,7,6,2}, new[]{0,3,2,1}, new[]{4,5,6,7} };
            foreach (var qd in quad)
            {
                EmitInward(v, tris, baseIdx, qd[0], qd[1], qd[2]); triMat.Add(mat);
                EmitInward(v, tris, baseIdx, qd[0], qd[2], qd[3]); triMat.Add(mat);
            }
        }

        // Emit a box triangle, flipping the last two indices so its normal points toward the origin.
        static void EmitInward(Vector3[] v, List<int> tris, int baseIdx, int i0, int i1, int i2)
        {
            Vector3 n = Vector3.Cross(v[i1] - v[i0], v[i2] - v[i0]);
            Vector3 c = (v[i0] + v[i1] + v[i2]) / 3f;
            if (Vector3.Dot(n, -c) < 0f) { var t = i1; i1 = i2; i2 = t; }   // normal points outward -> flip
            tris.Add(baseIdx + i0); tris.Add(baseIdx + i1); tris.Add(baseIdx + i2);
        }

        readonly List<BwEmitter> _pushBuf = new();   // reusable snapshot (Push may unregister mid-loop)

        public void Register(BwEmitter e)   { if (!_emitters.Contains(e)) _emitters.Add(e); }
        public void Unregister(BwEmitter e) => _emitters.Remove(e);

        void LateUpdate()
        {
            if (!Ready) return;
            // Iterate a snapshot: an emitter's onFinished handler may disable it, which runs OnDisable ->
            // Unregister -> _emitters.Remove mid-loop. Mutating the list under a foreach would throw and
            // skip the commit. _pushBuf is cleared+refilled (no per-frame allocation after warmup).
            _pushBuf.Clear();
            _pushBuf.AddRange(_emitters);
            foreach (var e in _pushBuf) if (e) e.Push();           // all sources first...
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
