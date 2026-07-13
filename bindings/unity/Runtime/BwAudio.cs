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
        [Tooltip("The surveyed speaker geometry, as a path RELATIVE TO Assets/StreamingAssets/ " +
                 "(create that folder and put cave_layout.json in it). Used by the cave/both profiles. " +
                 "If it fails to load the engine falls back to its default 26-speaker grid and logs an " +
                 "error — it does NOT stop, so a smaller rig would silently pan over the wrong geometry.")]
        [BwClip(".json")] public string layoutFile = "cave_layout.json";
        public uint sampleRate = 48000;
        public uint blockSize = 256;

        [Header("Listener")]
        [Tooltip("The tracked head: your OptiTrack rigid body, or the XR camera at a desk. Required " +
                 "when Feed Listener is on — without it the listener never moves from the array centroid.")]
        public Transform listener;          // OptiTrack head rigid body, or the XR camera at a desk
        public bool feedListener = true;    // false => the core reads NatNet itself (cave/both)
        [Tooltip("Other occupants (up to 3). Panning becomes the per-speaker energy MEAN of everyone's " +
                 "solve — each person hears the image biased toward their own spot, instead of one exact " +
                 "listener and N wrong ones. Pushed every frame, like the primary listener.")]
        public Transform[] extraListeners;
        [Tooltip("Internal tracking only (Feed Listener off): lead the tracked position by your MEASURED " +
                 "motion-to-ears latency (typically 20-40 ms). Too much lead overshoots on direction " +
                 "changes. 0 = off.")]
        [Range(0f, 200f)] public float posePredictionMs = 0f;

        [Header("Output")]
        [Tooltip("One ramped scalar over the whole mix — applied before the per-speaker trims (which stay " +
                 "calibrated) and before the limiter. The volume knob / scene fade.")]
        [Range(0f, 2f)] public float masterGain = 1f;
        [Tooltip("Output protection limiter (engine default: ON). Linked across channels, so engaging " +
                 "never shifts the spatial image. Protection against digital overs, NOT mastering — if it " +
                 "engages in normal use, turn the content down.")]
        public bool limiter = true;
        [Range(-60f, 0f)] public float limiterCeilingDb = -1f;

        [Header("Panning")]
        [Tooltip("DBAP: listener-relative, for a MOVING observer (the CAVE case). SPCAP/VBAP assume a " +
                 "FIXED listener — sharper, but only at the sweet spot.")]
        public BwPanner panner = BwPanner.Dbap;
        [Tooltip("Split at ~700 Hz and pan the low band with amplitude normalisation: sharper LF " +
                 "localisation for a near-centred listener. Sweet-spot dependent.")]
        public bool dualBand = false;
        [Tooltip("How a source's spread renders. LOBE: one reshaped solve (cheap, smooth). MDAP: a ring of " +
                 "virtual sources panned with the selected panner (panner-true, ~13x the solve cost).")]
        public BwSpreadMode spreadMode = BwSpreadMode.Lobe;
        [Tooltip("Feed a wide source's speakers mutually INCOHERENT signals (velvet-noise filters), so it " +
                 "doesn't collapse to phantom images or comb-filter as the listener walks. Point sources " +
                 "are untouched.")]
        public bool decorrelation = false;
        [Tooltip("Widen sources that come close to the head, instead of letting them snap across the " +
                 "nearest speaker. ~1 m is a good start; 0 = off.")]
        public float nearSpreadRadius = 0f;
        [Tooltip("For layouts carrying a room_eq_grid (bw_calibrate --room-eq-grid): re-interpolate the LF " +
                 "modal cuts at the live listener position. No-op without a grid; this is the kill switch.")]
        public bool trackedRoomEq = true;

        [Header("Diffuse beds (BwAmbisonicBed / reverb)")]
        [Tooltip("Load-time. AllRAD is more robust on an irregular/lopsided array, at a heavier load-time build.")]
        public BwBedDecoder bedDecoder = BwBedDecoder.Sampling;
        [Tooltip("MATRIX: the static SH->speaker decode. PARAMETRIC: DirAC analysis re-pans the directional " +
                 "part through the listener-relative panner — a recorded soundfield becomes WALKABLE " +
                 "(correct directions + parallax off-centre). Live: beds crossfade, so it A/Bs.")]
        public BwBedRenderer bedRenderer = BwBedRenderer.Matrix;

        [Header("Early reflections (image-source; no SDK needed)")]
        [Tooltip("Level of the per-source wall bounces (opt a source in with BwEmitter.earlyReflections). " +
                 "Needs the Room Box below — that's the geometry they mirror off. These carry room size " +
                 "and source distance with real parallax; the reverb beds below carry the late tail.")]
        [Range(0f, 2f)] public float earlyReflectionGain = 1f;

        [Header("Reflections — Steam Audio bed (load-time; needs the SDK)")]
        public bool enableReflections = false;
        [Range(0.1f, 3f)] public float reverbSeconds = 1.0f;
        [Range(1, 2)]     public int   reflectionOrder = 1;
        [Range(0f, 2f)]   public float reverbGain = 1.0f;   // wet level; adjustable live via ReverbGain

        [Header("Reflections — FDN reverb (load-time; NO SDK needed)")]
        [Tooltip("A directional feedback-delay-network reverb. Takes the reverb tap INSTEAD of the Steam " +
                 "bed (one bed at a time) and is fed by the same per-emitter reflection sends — so reverb " +
                 "works in a build without Steam Audio. Decay is a DESIGN parameter: set what the content " +
                 "wants, do NOT copy the room's measured RT60 (the real room adds its own on top).")]
        public bool enableFdnReverb = false;
        [Range(0.1f, 8f)] public float fdnRt60LowSeconds  = 1.2f;
        [Range(0.1f, 8f)] public float fdnRt60HighSeconds = 0.7f;
        public float fdnCrossoverHz = 2000f;
        [Tooltip("Anisotropic decay: scale the decay time toward this room-space direction (leave zero for " +
                 "a uniform field). Factor < 1 = the field dies faster that way — an open or treated side.")]
        public Vector3 fdnDecayDirection = Vector3.zero;
        [Range(0.1f, 2f)] public float fdnDecayFactor = 1f;

        [Header("Scene view")]
        [Tooltip("Draw the speaker array. Stopped, these come from the layout FILE. In Play mode they " +
                 "come from the ENGINE (bw_get_speakers) and light up with each channel's live output " +
                 "level — so you see the geometry actually being panned over, which is how a failed " +
                 "layout load looks: the default 26-grid, not your room.")]
        public bool showSpeakers = true;
        [Range(0.02f, 0.5f)] public float speakerGizmoRadius = 0.12f;
        public bool showSpeakerIndices = true;

        [Header("Room box (load-time; optional acoustic geometry)")]
        [Tooltip("A shoebox enclosure for occlusion/reflections, drawn as a yellow wireframe in the " +
                 "scene view. FLOOR-BASED: centred on the origin in x/z, running from y=0 up to its " +
                 "height. For anything more detailed, use BwAcousticGeometry instead.")]
        public bool enableRoomBox = false;
        public Vector3 roomSizeMetres = new Vector3(3f, 3f, 3f);
        [Tooltip("Material for all six faces. For per-face or custom materials, build the room out of " +
                 "BwAcousticGeometry objects instead.")]
        public BwMaterialPreset roomMaterial = BwMaterialPreset.Concrete;

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

            // The layout resolves inside bw_create, so the channel count is known before start — and it is
            // the LAYOUT's speaker count, not a constant. A FAILED load is NOT fatal: the engine falls back
            // to the built-in 26-speaker grid and only records why in bw_last_error (which bw_create sets
            // for nothing else). On a smaller rig that silently changes the count too, so every source would
            // be panned over geometry that isn't the one in the room. Say so, loudly.
            _channels = Bw.bw_channel_count(_eng);
            var loadErr = Bw.LastError(_eng);
            if (loadErr != null)
                Debug.LogError($"[BwAudio] layout '{cfg.layoutPath}' did not load — the engine fell back to " +
                               $"its default {_channels}-speaker grid: {loadErr}");

            ApplyLoadTimeSettings();
            SetupScene();   // acoustic geometry + optional room box -> the engine's scene (load-time)

            if (Bw.bw_start(_eng) != 0)
            {
                Debug.LogError("[BwAudio] bw_start failed: " + Bw.LastError(_eng));
                Bw.bw_destroy(_eng); _eng = IntPtr.Zero; return;     // Instance NOT claimed -> another manager can try
            }

            // Only claim the singleton once we have a live engine, so a failed init leaves Instance free.
            Instance = this; DontDestroyOnLoad(gameObject);
            Debug.Log($"[BwAudio] started, backend={Bw.Backend(_eng)}, channels={_channels}");

            // Feeding the pose from Unity with nothing to feed it FROM is a silent no-op: the listener
            // just stays at the array centroid and every source is panned for a head that never moves.
            if (feedListener && !listener)
                Debug.LogWarning("[BwAudio] 'Feed Listener' is on but no listener Transform is assigned — " +
                                 "the listener will never move from the array centroid. Assign the tracked " +
                                 "head (or turn Feed Listener off to let the engine read NatNet itself).");
        }

        // Everything the engine wants set BEFORE bw_start (the reverb bed, the bed decoder), plus the live
        // knobs — pushed here too so the scene STARTS in the state the inspector shows, not at the engine's
        // defaults. ApplyLiveSettings is the same set minus the load-time-only ones.
        void ApplyLoadTimeSettings()
        {
            if (enableReflections && enableFdnReverb)
                Debug.LogWarning("[BwAudio] both reverb beds enabled — they share ONE reverb tap. Using the " +
                                 "FDN (it takes the tap) and ignoring the Steam bed; tick only one.");

            if (enableFdnReverb)
            {
                Bw.bw_reverb_fdn(_eng, true);
                Bw.bw_fdn_set_decay(_eng, fdnRt60LowSeconds, fdnRt60HighSeconds, fdnCrossoverHz);
                if (fdnDecayDirection != Vector3.zero)
                {
                    var d = Room.Dir(fdnDecayDirection.normalized);   // a direction: no registration offset
                    Bw.bw_fdn_set_decay_direction(_eng, new[] { d.x, d.y, d.z }, fdnDecayFactor);
                }
            }
            else if (enableReflections)
            {
                var rc = new BwReflectionConfig {
                    irSeconds = reverbSeconds, order = (uint)reflectionOrder,
                    numRays = 0, numBounces = 0, enabled = 1, wetGain = reverbGain,
                };
                Bw.bw_reflections_config(_eng, in rc);
            }

            Bw.bw_set_bed_decoder(_eng, bedDecoder);   // load-time only
            ApplyLiveSettings();
        }

        // The live A/B surface: safe to re-push at any time (each call is atomic / ramped engine-side).
        // Called once before start, and again from OnValidate so inspector tweaks are audible in Play mode.
        void ApplyLiveSettings()
        {
            Bw.bw_set_panner(_eng, panner);
            Bw.bw_set_dual_band(_eng, dualBand);
            Bw.bw_set_spread_mode(_eng, spreadMode);
            Bw.bw_set_decorrelation(_eng, decorrelation);
            Bw.bw_set_near_spread(_eng, nearSpreadRadius);
            Bw.bw_set_bed_renderer(_eng, bedRenderer);
            Bw.bw_set_tracked_room_eq(_eng, trackedRoomEq);
            Bw.bw_set_master_gain(_eng, masterGain);
            Bw.bw_early_reflections_set_gain(_eng, earlyReflectionGain);
            Bw.bw_set_limiter(_eng, limiter);
            Bw.bw_set_limiter_ceiling(_eng, limiterCeilingDb);
            Bw.bw_set_pose_prediction(_eng, feedListener ? 0f : posePredictionMs);   // internal tracking only
        }

        // Inspector edits take effect live in Play mode — that IS the workflow for these (the engine makes
        // the panner/dual-band/spread-mode/decorrelation/bed-renderer switches atomic so they can be A/B'd
        // by ear). Load-time fields (the reverb beds, the bed decoder, the room box) need a scene restart.
        void OnValidate()
        {
            _layoutXyzFrom = null;                                       // re-read the layout for the gizmos
            if (Application.isPlaying && Ready) ApplyLiveSettings();
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

        /// <summary>Mint one of the engine's built-in materials (load-time). Typed, because the raw call
        /// treats an unknown name as "use the generic default" rather than as an error — for a name the
        /// enum doesn't cover, drop to Bw.bw_material_preset and check Bw.LastError yourself.</summary>
        public uint MaterialPreset(BwMaterialPreset preset) => Ready ? Bw.MaterialPreset(_eng, preset) : 0;

        /// <summary>Reverb wet level (linear), adjustable live — the reverb-send equivalent.</summary>
        public float ReverbGain
        {
            get => reverbGain;
            set { reverbGain = value; if (Ready) Bw.bw_reflections_set_gain(_eng, value); }
        }

        /// <summary>Level of the image-source EARLY reflections (the per-source wall bounces, opted into
        /// with BwEmitter.earlyReflections). Independent of the late reverb bed's wet level above: early
        /// reflections carry room size and distance, the bed carries the tail. Live.</summary>
        public float EarlyReflectionGain
        {
            get => earlyReflectionGain;
            set { earlyReflectionGain = value; if (Ready) Bw.bw_early_reflections_set_gain(_eng, value); }
        }

        /// <summary>Output protection limiter (engine default: ON at -1 dBFS). Linked across the channels —
        /// engaging never shifts the spatial image. Protection against digital overs, not mastering: if it
        /// engages in normal use, turn the content down.</summary>
        public void SetLimiter(bool on) { limiter = on; if (Ready) Bw.bw_set_limiter(_eng, on); }
        public void SetLimiterCeiling(float ceilingDb) { limiterCeilingDb = ceilingDb; if (Ready) Bw.bw_set_limiter_ceiling(_eng, ceilingDb); }

        /// <summary>Master gain over the whole mix (ramped — slider drags never zipper). The volume knob.</summary>
        public float MasterGain
        {
            get => masterGain;
            set { masterGain = value; if (Ready) Bw.bw_set_master_gain(_eng, value); }
        }

        /// <summary>Global pause (focus loss, menu): EVERY voice — sources, streams, beds — ramps out and
        /// freezes; resume continues exactly where it stopped. Paused voices still read as IsPlaying.</summary>
        public bool Paused
        {
            get => _paused;
            set { _paused = value; if (Ready) Bw.bw_set_paused(_eng, value); }
        }
        bool _paused;

        /// <summary>Mix-group gain (group 0..7): ducks every emitter in the group — "quiet the SFX, keep the
        /// dialog" without touching each source. Ramped.</summary>
        public void SetGroupGain(uint group, float linear) { if (Ready) Bw.bw_group_set_gain(_eng, group, linear); }

        /// <summary>Pause a whole mix group (0..7) — same click-free freeze as per-source pause.</summary>
        public void SetGroupPaused(uint group, bool paused) { if (Ready) Bw.bw_group_set_paused(_eng, group, paused); }

        // ---- live rendering A/B (each of these is atomic / crossfaded engine-side) --------------------
        public void SetPanner(BwPanner p)        { panner = p;        if (Ready) Bw.bw_set_panner(_eng, p); }
        public void SetDualBand(bool on)         { dualBand = on;     if (Ready) Bw.bw_set_dual_band(_eng, on); }
        public void SetSpreadMode(BwSpreadMode m){ spreadMode = m;    if (Ready) Bw.bw_set_spread_mode(_eng, m); }
        public void SetDecorrelation(bool on)    { decorrelation = on; if (Ready) Bw.bw_set_decorrelation(_eng, on); }
        public void SetNearSpread(float radiusM) { nearSpreadRadius = radiusM; if (Ready) Bw.bw_set_near_spread(_eng, radiusM); }
        public void SetBedRenderer(BwBedRenderer r) { bedRenderer = r; if (Ready) Bw.bw_set_bed_renderer(_eng, r); }
        public void SetTrackedRoomEq(bool on)    { trackedRoomEq = on; if (Ready) Bw.bw_set_tracked_room_eq(_eng, on); }
        /// <summary>Lead the TRACKED pose by `ms` to hide motion-to-ears latency. Internal tracking only
        /// (Feed Listener off) — when Unity feeds the pose, predict on the Unity side instead.</summary>
        public void SetPosePrediction(float ms)  { posePredictionMs = ms; if (Ready && !feedListener) Bw.bw_set_pose_prediction(_eng, ms); }

        // ---- readback (per-frame-safe: no locks, no allocation in the engine) -------------------------
        /// <summary>The engine's ACTIVE channel count — the layout's speaker count (4..26), NOT a constant.
        /// Size any meter / speaker-gizmo / channel-test array with this; never hard-code 26.</summary>
        public uint ChannelCount => _channels;
        uint _channels;

        /// <summary>Last block's peak |sample| per output channel (linear), as the device received it —
        /// after the trims, the test signal, and the limiter. Drives channel meters / a speaker-activity
        /// display. The array is reused; it is ChannelCount long.</summary>
        public float[] BusLevels()
        {
            if (!Ready) return Array.Empty<float>();
            if (_levels == null || _levels.Length != _channels) _levels = new float[_channels];
            Bw.bw_get_bus_levels(_eng, _levels, (uint)_levels.Length);
            return _levels;
        }
        float[] _levels;

        /// <summary>Speaker positions in ROOM space (3 floats each, in channel order) — the geometry the
        /// engine is actually panning with (the loaded layout, or the default grid it fell back to). The
        /// layout is fixed for the engine's lifetime, so this is read once and cached; the array is
        /// reused, so don't mutate it.</summary>
        public float[] SpeakerPositions()
        {
            if (!Ready) return Array.Empty<float>();
            if (_speakers == null || _speakers.Length != _channels * 3)
            {
                _speakers = new float[_channels * 3];
                Bw.bw_get_speakers(_eng, _speakers, _channels);
            }
            return _speakers;
        }
        float[] _speakers;

        /// <summary>Voices playing in the last block — a voice-pool gauge for a HUD.</summary>
        public uint ActiveVoices => Ready ? Bw.bw_get_active_voices(_eng) : 0;

        /// <summary>The engine's dsp-sample clock (device-anchored, monotonic). Add to it to schedule a
        /// sample-accurate start: <c>DspTime + sampleRate/2</c> plays half a second out.</summary>
        public ulong DspTime => Ready ? Bw.bw_dsp_time(_eng) : 0;

        /// <summary>Drive ONE raw output channel with a test tone — a speaker-check / wiring tool, injected
        /// after the per-speaker trims. NOT a spatial path (it bypasses the panner). gain 0 or Off silences.</summary>
        public void TestSignal(uint channel, BwTestKind kind, float gain) { if (Ready) Bw.bw_test_signal(_eng, channel, kind, gain); }

        // ---- acoustic scene baking (load-time) ----------------------------------------------------
        // Collect every BwAcousticGeometry (+ the optional room box) into ONE mesh and hand it to the
        // engine. The engine's scene is a single static mesh, so this is a one-time bake before start.
        void SetupScene()
        {
            // Sort mode None: we don't care about order (every geometry is baked into one mesh), and the
            // default InstanceID sort is pure overhead. FindObjectsOfType, which sorted unconditionally,
            // is deprecated.
            var geos = FindObjectsByType<BwAcousticGeometry>(FindObjectsSortMode.None);
            bool haveGeo = geos != null && geos.Length > 0;
            if (!haveGeo && !enableRoomBox) return;

            // simple path: just a box, nothing else -> the engine's own box helper (inward normals)
            if (!haveGeo && enableRoomBox)
            {
                uint mb = Bw.MaterialPreset(_eng, roomMaterial);
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
                AddBox(verts, tris, triMat, roomSizeMetres, Bw.MaterialPreset(_eng, roomMaterial));
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
        // X-flip in Room.Pos, UnityToRoom, AND the object's own scale — a negative/mirrored scale flips
        // winding by itself), not just the X-flip. Room.ReversesWinding folds all three together.
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

        // Append a FLOOR-based box (room metres: x/z centred, y from 0 up to size.y — matching
        // bw_scene_set_box), inward-facing normals (the listener is inside).
        static void AddBox(List<float> verts, List<int> tris, List<uint> triMat, Vector3 size, uint mat)
        {
            float hw = size.x * 0.5f, hd = size.z * 0.5f, h = size.y;
            var v = new[] {
                new Vector3(-hw, 0,-hd), new Vector3(hw, 0,-hd), new Vector3(hw, h,-hd), new Vector3(-hw, h,-hd),
                new Vector3(-hw, 0, hd), new Vector3(hw, 0, hd), new Vector3(hw, h, hd), new Vector3(-hw, h, hd) };
            int baseIdx = verts.Count / 3;
            foreach (var p in v) { verts.Add(p.x); verts.Add(p.y); verts.Add(p.z); }   // already room space
            int[][] quad = { new[]{0,4,7,3}, new[]{1,2,6,5}, new[]{0,1,5,4}, new[]{3,7,6,2}, new[]{0,3,2,1}, new[]{4,5,6,7} };
            var ctr = new Vector3(0, h * 0.5f, 0);
            foreach (var qd in quad)
            {
                EmitInward(v, ctr, tris, baseIdx, qd[0], qd[1], qd[2]); triMat.Add(mat);
                EmitInward(v, ctr, tris, baseIdx, qd[0], qd[2], qd[3]); triMat.Add(mat);
            }
        }

        // Emit a box triangle, flipping the last two indices so its normal points toward the box
        // centre (toward the ORIGIN would degenerate: a floor-based box's bottom face contains it).
        static void EmitInward(Vector3[] v, Vector3 ctr, List<int> tris, int baseIdx, int i0, int i1, int i2)
        {
            Vector3 n = Vector3.Cross(v[i1] - v[i0], v[i2] - v[i0]);
            Vector3 c = (v[i0] + v[i1] + v[i2]) / 3f;
            if (Vector3.Dot(n, ctr - c) < 0f) { var t = i1; i1 = i2; i2 = t; }   // normal points outward -> flip
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
            PushExtraListeners();                                  // ...the other occupants (commit-gated too)...
            Bw.bw_commit(_eng);                                    // ...then one atomic snapshot
        }

        // The other occupants, for compromise panning. Commit-gated like the primary pose, so it belongs in
        // the same frame block. The engine takes at most BW_EXTRA_LIS (3); the buffer is reused (no per-frame
        // allocation), and count 0 restores single-listener panning — so clearing the array turns it off.
        static readonly float[] _extraBuf = new float[3 * 3];
        int _extraPushed;   // remember the last count, so dropping the last occupant still sends the 0
        void PushExtraListeners()
        {
            int n = 0;
            if (extraListeners != null)
                for (int i = 0; i < extraListeners.Length && n < 3; i++)
                {
                    var t = extraListeners[i];
                    if (!t) continue;
                    var p = Room.Pos(t.position);
                    _extraBuf[n * 3] = p.x; _extraBuf[n * 3 + 1] = p.y; _extraBuf[n * 3 + 2] = p.z;
                    n++;
                }
            if (n == 0 && _extraPushed == 0) return;               // the common case: nobody else in the room
            Bw.bw_set_extra_listeners(_eng, _extraBuf, (uint)n);
            _extraPushed = n;
        }

        // ---- scene view ------------------------------------------------------------------------------
        // Everything here is drawn in ROOM metres through the inverse of the coordinate seam, so a wrong
        // Room.UnityToRoom puts the array and the box visibly in the wrong place — which is the cheapest
        // possible check on the one setting that silently ruins spatial audio.
        void OnDrawGizmos()
        {
            var prev = Gizmos.matrix;
            Gizmos.matrix = Room.RoomToUnityMatrix();

            if (enableRoomBox)
            {
                Gizmos.color = new Color(1f, 0.9f, 0.3f, 0.9f);
                var size = new Vector3(roomSizeMetres.x, roomSizeMetres.y, roomSizeMetres.z);
                Gizmos.DrawWireCube(new Vector3(0f, size.y * 0.5f, 0f), size);   // floor-based: y from 0 up
            }
            if (showSpeakers) DrawSpeakers();

            Gizmos.matrix = prev;
        }

        // Stopped: the layout FILE (the geometry you authored). Running: the ENGINE's own readback, which
        // is the geometry it is actually panning over — including the default 26-grid it silently falls
        // back to when the layout fails to load. Seeing the wrong array is a better bug report than
        // reading about it.
        void DrawSpeakers()
        {
            float[] xyz; float[] peaks = null;
            if (Application.isPlaying && Ready) { xyz = SpeakerPositions(); peaks = BusLevels(); }
            else                                { xyz = LayoutFileSpeakers(); }
            if (xyz == null) return;

            int n = xyz.Length / 3;
            for (int i = 0; i < n; i++)
            {
                var p = new Vector3(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);

                // Live level lights the speaker up, exactly like the playground's gizmos: a silent
                // channel stays dim, so a dead or mis-wired speaker is visible at a glance.
                float lvl = (peaks != null && i < peaks.Length) ? Mathf.Clamp01(peaks[i]) : 0f;
                Gizmos.color = Application.isPlaying
                    ? new Color(0.25f + 0.75f * lvl, 0.35f + 0.35f * lvl, 0.45f - 0.25f * lvl, 1f)
                    : new Color(0.35f, 0.75f, 0.95f, 0.9f);
                Gizmos.DrawSphere(p, speakerGizmoRadius);
                Gizmos.color = new Color(0.1f, 0.15f, 0.2f, 0.5f);
                Gizmos.DrawWireSphere(p, speakerGizmoRadius);

#if UNITY_EDITOR
                if (showSpeakerIndices)
                {
                    var above = new Vector3(p.x, p.y + speakerGizmoRadius * 1.6f, p.z);
                    UnityEditor.Handles.Label(Room.FromRoom(above), i.ToString());   // = the channel index
                }
#endif
            }
        }

        // The layout as authored, for the stopped editor (the engine isn't up to ask). Only the speaker
        // POSITIONS — the trims/DBAP knobs are the engine's business.
        [Serializable] class LayoutSpeaker { public int index; public float[] position; }
        [Serializable] class LayoutFile    { public LayoutSpeaker[] speakers; }
        float[] _layoutXyz;
        string _layoutXyzFrom;

        float[] LayoutFileSpeakers()
        {
            if (_layoutXyzFrom == layoutFile) return _layoutXyz;
            _layoutXyzFrom = layoutFile;
            _layoutXyz = null;
            if (string.IsNullOrEmpty(layoutFile)) return null;
            try
            {
                string path = Path.Combine(Application.streamingAssetsPath, layoutFile);
                if (!File.Exists(path)) return null;
                var f = JsonUtility.FromJson<LayoutFile>(File.ReadAllText(path));
                if (f?.speakers == null) return null;

                // Speakers are stored BY INDEX (the channel), not by array order — the loader requires a
                // complete 0..N-1 permutation, so index is what the engine pans onto and what to label.
                var xyz = new float[f.speakers.Length * 3];
                foreach (var s in f.speakers)
                {
                    if (s?.position == null || s.position.Length < 3) continue;
                    if (s.index < 0 || s.index >= f.speakers.Length) continue;
                    xyz[s.index * 3]     = s.position[0];
                    xyz[s.index * 3 + 1] = s.position[1];
                    xyz[s.index * 3 + 2] = s.position[2];
                }
                _layoutXyz = xyz;
            }
            catch (Exception) { _layoutXyz = null; }   // a malformed layout just draws nothing
            return _layoutXyz;
        }

        void OnDestroy()
        {
            if (!Ready) return;
            Bw.bw_stop(_eng); Bw.bw_destroy(_eng); _eng = IntPtr.Zero;
            if (Instance == this) Instance = null;
        }
    }
}
