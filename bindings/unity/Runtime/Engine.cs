// Engine.cs — the manager. ONE per scene (singleton). Owns the engine handle, loads assets, and
// runs the CENTRALIZED per-frame push: all sources, then the listener, then one commit — so every
// block the audio thread sees is internally consistent (Unity does not order LateUpdate across
// components, so per-emitter pushes could commit a half-moved frame). See docs/integration.md.
using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace BwAudio
{
    [DefaultExecutionOrder(-100)]
    public sealed class Engine : MonoBehaviour
    {
        public static Engine Instance { get; private set; }

        [Header("Engine")]
        public BwaProfile profile = BwaProfile.Binaural;   // inspector dropdown; maps 1:1 to the C enum
        [Tooltip("The surveyed speaker geometry, as a path RELATIVE TO Assets/StreamingAssets/ " +
                 "(create that folder and put cave_layout.json in it). Used by the cave/both profiles. " +
                 "If it fails to load the engine falls back to its default 26-speaker grid and logs an " +
                 "error — it does NOT stop, so a smaller rig would silently pan over the wrong geometry.")]
        [Clip(".json")] public string layoutFile = "cave_layout.json";
        public uint sampleRate = 48000;
        public uint blockSize = 256;
        [Tooltip("Output-device policy. Auto: try ASIO, fall back to the SILENT offline sink (the " +
                 "engine keeps rendering — the inspector flags the silent fallback). Asio: demand a " +
                 "real device, so a missing driver fails startup loudly. Null: force the offline sink.")]
        public BwaSinkType sink = BwaSinkType.Auto;
        [Tooltip("ASIO driver name to open. Empty = auto-pick the first registered driver with " +
                 "enough output channels for the profile.")]
        public string asioDriver = "";

        [Header("Listener")]
        [Tooltip("The tracked head: your OptiTrack rigid body, or the XR camera at a desk. Required " +
                 "when Feed Listener is on — without it the listener never moves from the array centroid.")]
        public Transform listener;          // OptiTrack head rigid body, or the XR camera at a desk
        public bool feedListener = true;    // false => the core reads NatNet itself (cave/both)
        [Tooltip("Internal tracking only (Feed Listener off): the Motive host IP, for the command " +
                 "handshake and rigid-body names. Empty = multicast-only (no handshake).")]
        public string natnetServer = "";
        [Tooltip("Internal tracking only: the rigid body to follow — a numeric streaming ID, or a " +
                 "name (names need the server above). Empty = the first rigid body in the frame. " +
                 "Ports/multicast/interface use NatNet defaults; call Bwa.bwa_tracker_connect " +
                 "yourself for a non-default rig.")]
        public string natnetRigidBody = "";
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
        public BwaPanner panner = BwaPanner.Dbap;
        [Tooltip("Split at ~700 Hz and pan the low band with amplitude normalisation: sharper LF " +
                 "localisation for a near-centred listener. Sweet-spot dependent.")]
        public bool dualBand = false;
        [Tooltip("How a source's spread renders. LOBE: one reshaped solve (cheap, smooth). MDAP: a ring of " +
                 "virtual sources panned with the selected panner (panner-true, ~13x the solve cost). " +
                 "SPECTRAL: frequency-dependent panning — 6 bands, each from its own direction in the " +
                 "cone; width with nothing to collapse or comb-filter (the decorrelation alternative).")]
        public BwaSpreadMode spreadMode = BwaSpreadMode.Lobe;
        [Tooltip("Feed a wide source's speakers mutually INCOHERENT signals (velvet-noise filters), so it " +
                 "doesn't collapse to phantom images or comb-filter as the listener walks. Point sources " +
                 "are untouched.")]
        public bool decorrelation = false;
        [Tooltip("Widen sources that come close to the head, instead of letting them snap across the " +
                 "nearest speaker. ~1 m is a good start; 0 = off.")]
        public float nearSpreadRadius = 0f;
        [Tooltip("For layouts carrying a room_eq_grid (bwa_calibrate --room-eq-grid): re-interpolate the LF " +
                 "modal cuts at the live listener position. No-op without a grid; this is the kill switch.")]
        public bool trackedRoomEq = true;

        [Header("Diffuse beds (AmbisonicBed / reverb)")]
        [Tooltip("Load-time. AllRAD (default) localizes a touch sharper; EPAD keeps a panned wave's loudness " +
                 "constant over direction by construction (flattest on an irregular array). A by-ear call.")]
        public BwaBedDecoder bedDecoder = BwaBedDecoder.Allrad;
        [Tooltip("MATRIX: the static SH->speaker decode. PARAMETRIC: DirAC analysis re-pans the directional " +
                 "part through the listener-relative panner — a recorded soundfield becomes WALKABLE " +
                 "(correct directions + parallax off-centre). Live: beds crossfade, so it A/Bs.")]
        public BwaBedRenderer bedRenderer = BwaBedRenderer.Matrix;
        [Tooltip("max-rE weighting on the bed decode (and the FDN's render): tapers the high ambisonic " +
                 "orders — fewer decode sidelobes, better localization AWAY from the sweet spot (the " +
                 "walking-listener case), slightly wider main lobe. Live A/B, level-fair.")]
        public bool maxRe = false;

        [Header("Early reflections (image-source; no SDK needed)")]
        [Tooltip("Level of the per-source wall bounces (opt a source in with Emitter.earlyReflections). " +
                 "Needs the Room Box below — that's the geometry they mirror off. These carry room size " +
                 "and source distance with real parallax; the reverb beds below carry the late tail.")]
        [Range(0f, 2f)] public float earlyReflectionGain = 1f;

        [Header("Reflections — Steam Audio bed (load-time; needs the SDK)")]
        public bool enableReflections = false;
        [Range(0.1f, 3f)] public float reverbSeconds = 1.0f;
        [Range(1, 2)]     public int   reflectionOrder = 1;
        [Tooltip("Precompute (bake) the reverb over a probe grid at start, so the sim thread looks it " +
                 "up instead of ray-tracing live. Static scenes only (which the bed requires anyway).")]
        public bool bakeReflections = false;
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

        [Header("Sound pathing (load-time; needs the SDK + scene geometry)")]
        [Tooltip("Run the sound-pathing sim: a blocked emitter's sound is routed around occluders / " +
                 "through openings and rendered from the indirect arrival directions. Emitters opt in " +
                 "with Emitter.pathing.")]
        public bool enablePathing = false;

        [Header("Scene view")]
        [Tooltip("Draw the speaker array. Stopped, these come from the layout FILE. In Play mode they " +
                 "come from the ENGINE (bwa_get_speakers) and light up with each channel's live output " +
                 "level — so you see the geometry actually being panned over, which is how a failed " +
                 "layout load looks: the default 26-grid, not your room.")]
        public bool showSpeakers = true;
        [Range(0.02f, 0.5f)] public float speakerGizmoRadius = 0.12f;
        public bool showSpeakerIndices = true;

        [Header("Room box (load-time; optional acoustic geometry)")]
        [Tooltip("A shoebox enclosure for occlusion/reflections, drawn as a yellow wireframe in the " +
                 "scene view. FLOOR-BASED: centred on the origin in x/z, running from y=0 up to its " +
                 "height. For anything more detailed, use AcousticGeometry instead.")]
        public bool enableRoomBox = false;
        public Vector3 roomSizeMetres = new Vector3(3f, 3f, 3f);
        [Tooltip("Material for all six faces. For per-face or custom materials, build the room out of " +
                 "AcousticGeometry objects instead.")]
        public BwaMaterialPreset roomMaterial = BwaMaterialPreset.Concrete;

        IntPtr _eng;
        public IntPtr Handle => _eng;
        public bool Ready => _eng != IntPtr.Zero;

        readonly List<Emitter> _emitters = new();
        readonly Dictionary<string, uint> _sounds = new();
        // Material tokens are minted into a FIXED 64-slot engine table and never freed, so mint each
        // MaterialAsset / preset ONCE and reuse it across every scene load — re-minting per load leaks
        // the table (multi-scene games hit this fast). The cache is engine-lifetime (assets are Project
        // objects that survive scene unloads), unlike the old per-SetupScene-call cache.
        readonly Dictionary<MaterialAsset, uint> _matCache = new();
        readonly Dictionary<BwaMaterialPreset, uint> _presetCache = new();
        bool _rebakeStatic;      // a scene loaded/unloaded: re-collect static AcousticGeometry next LateUpdate
        bool _hasStaticMesh;     // whether a non-empty static mesh is currently pushed (so we know to clear it)

        void Awake()
        {
            if (Instance != null) { Destroy(gameObject); return; }   // a working manager already owns the engine

            var cfg = new BwaDesc {
                profile = profile,
                layoutPath = Path.Combine(Application.streamingAssetsPath, layoutFile),
                hrtfPath = null, sampleRate = sampleRate, blockSize = blockSize,
                sink = sink,
                asioDriver = asioDriver.Length > 0 ? asioDriver : null,
                enablePathing = enablePathing,
                bedDecoder = bedDecoder,
            };
            _eng = Bwa.bwa_create(in cfg);
            if (_eng == IntPtr.Zero) { Debug.LogError("[bw_audio] bwa_create failed"); return; }   // Instance NOT claimed

            // The layout resolves inside bwa_create, so the channel count is known before start — and it is
            // the LAYOUT's speaker count, not a constant. A FAILED load is NOT fatal: the engine falls back
            // to the built-in 26-speaker grid and only records why in bwa_last_error (which bwa_create sets
            // for nothing else). On a smaller rig that silently changes the count too, so every source would
            // be panned over geometry that isn't the one in the room. Say so, loudly.
            _channels = Bwa.bwa_get_channel_count(_eng);
            var loadErr = Bwa.LastError(_eng);
            if (loadErr != null)
                Debug.LogError($"[bw_audio] layout '{cfg.layoutPath}' did not load — the engine fell back to " +
                               $"its default {_channels}-speaker grid: {loadErr}");

            ApplyLoadTimeSettings();
            SetupScene();   // acoustic geometry + optional room box -> the engine's scene (load-time)

            if (Bwa.bwa_start(_eng) != BwaResult.Ok)
            {
                Debug.LogError("[bw_audio] bwa_start failed: " + Bwa.LastError(_eng));
                Bwa.bwa_destroy(_eng); _eng = IntPtr.Zero; return;     // Instance NOT claimed -> another manager can try
            }

            // Internal tracking: connect the engine to the NatNet stream itself (runtime call — no env
            // vars). Non-fatal: the engine keeps rendering from the committed/default pose, loudly.
            if (!feedListener)
            {
                var td = new BwaTrackerDesc { server = natnetServer.Length > 0 ? natnetServer : null };
                if (natnetRigidBody.Length > 0)
                {
                    if (int.TryParse(natnetRigidBody, out var id)) td.rigidBodyId = id;
                    else td.rigidBodyName = natnetRigidBody;
                }
                if (Bwa.bwa_tracker_connect(_eng, in td) != BwaResult.Ok)
                    Debug.LogError("[bw_audio] tracker connect failed (listener stays at the committed " +
                                   "pose): " + Bwa.LastError(_eng));
            }

            // Only claim the singleton once we have a live engine, so a failed init leaves Instance free.
            Instance = this; DontDestroyOnLoad(gameObject);
            // Follow Unity's loaded scenes: re-bake the static acoustic geometry when one loads/unloads, so
            // the persistent engine's scene tracks the (possibly additive) game scenes on top of it.
            SceneManager.sceneLoaded   += OnSceneChanged;
            SceneManager.sceneUnloaded += OnSceneChanged;
            Debug.Log($"[bw_audio] started, backend={Bwa.Backend(_eng)}, channels={_channels}");

            // Feeding the pose from Unity with nothing to feed it FROM is a silent no-op: the listener
            // just stays at the array centroid and every source is panned for a head that never moves.
            if (feedListener && !listener)
                Debug.LogWarning("[bw_audio] 'Feed Listener' is on but no listener Transform is assigned — " +
                                 "the listener will never move from the array centroid. Assign the tracked " +
                                 "head (or turn Feed Listener off to let the engine read NatNet itself).");
        }

        // Everything the engine wants set BEFORE bwa_start (the reverb beds — the bed decoder and pathing
        // ride BwaDesc into bwa_create), plus the live knobs — pushed here too so the scene STARTS in the
        // state the inspector shows, not at the engine's defaults. ApplyLiveSettings is the same set minus
        // the load-time-only ones.
        void ApplyLoadTimeSettings()
        {
            if (enableReflections && enableFdnReverb)
                Debug.LogWarning("[bw_audio] both reverb beds enabled — they share ONE reverb tap. Using the " +
                                 "FDN (it takes the tap) and ignoring the Steam bed; tick only one.");

            if (enableFdnReverb)
            {
                var d = fdnDecayDirection == Vector3.zero
                    ? Vector3.zero                                    // all-zero => uniform decay
                    : Room.Dir(fdnDecayDirection.normalized);         // a direction: no registration offset
                var fc = new BwaFdnDesc {
                    enabled = 1,
                    rt60LowSeconds = fdnRt60LowSeconds, rt60HighSeconds = fdnRt60HighSeconds,
                    crossoverHz = fdnCrossoverHz,
                    decayDirX = d.x, decayDirY = d.y, decayDirZ = d.z,
                    decayFactor = fdnDecayFactor,
                };
                Bwa.bwa_fdn_config(_eng, in fc);
            }
            else if (enableReflections)
            {
                var rc = new BwaReflectionsDesc {
                    irSeconds = reverbSeconds, order = (uint)reflectionOrder,
                    numRays = 0, numBounces = 0, enabled = 1, bake = bakeReflections ? 1 : 0,
                };
                Bwa.bwa_reflections_config(_eng, in rc);
            }

            ApplyLiveSettings();
        }

        // The live A/B surface: safe to re-push at any time (each call is atomic / ramped engine-side).
        // Called once before start, and again from OnValidate so inspector tweaks are audible in Play mode.
        void ApplyLiveSettings()
        {
            Bwa.bwa_set_panner(_eng, panner);
            Bwa.bwa_set_dual_band(_eng, dualBand);
            Bwa.bwa_set_spread_mode(_eng, spreadMode);
            Bwa.bwa_set_decorrelation(_eng, decorrelation);
            Bwa.bwa_set_near_spread(_eng, nearSpreadRadius);
            Bwa.bwa_set_bed_renderer(_eng, bedRenderer);
            Bwa.bwa_set_max_re(_eng, maxRe);
            Bwa.bwa_set_tracked_room_eq(_eng, trackedRoomEq);
            Bwa.bwa_set_master_gain(_eng, masterGain);
            Bwa.bwa_reverb_set_gain(_eng, reverbGain);   // valid pre-start: seeds whichever reverb bed bwa_start creates
            Bwa.bwa_early_reflections_set_gain(_eng, earlyReflectionGain);
            Bwa.bwa_set_limiter(_eng, limiter);
            Bwa.bwa_set_limiter_ceiling(_eng, limiterCeilingDb);
            Bwa.bwa_set_pose_prediction(_eng, feedListener ? 0f : posePredictionMs);   // internal tracking only
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
            s = Bwa.bwa_load_sound(_eng, Path.Combine(Application.streamingAssetsPath, clip));
            if (s == 0) Debug.LogWarning("[bw_audio] load failed: " + clip + " (" + Bwa.LastError(_eng) + ")");
            _sounds[clip] = s; return s;
        }

        /// <summary>Load a pre-encoded AmbiX soundfield for a world-locked bed (cached). Returns 0 on failure.</summary>
        public uint LoadAmbix(string clip)
        {
            if (!Ready) return 0;
            var key = "ambix:" + clip;
            if (_sounds.TryGetValue(key, out var s)) return s;
            s = Bwa.bwa_load_ambix(_eng, Path.Combine(Application.streamingAssetsPath, clip));
            if (s == 0) Debug.LogWarning("[bw_audio] ambix load failed: " + clip + " (" + Bwa.LastError(_eng) + ")");
            _sounds[key] = s; return s;
        }

        /// <summary>Load a legacy FuMa B-format soundfield (.amb-style: WXYZ order, MaxN, W -3 dB) for a
        /// world-locked bed (cached). Converted to AmbiX at load — past this call it IS an AmbiX asset.
        /// Full 3D sets only (4/9/16 channels). Returns 0 on failure.</summary>
        public uint LoadFuma(string clip)
        {
            if (!Ready) return 0;
            var key = "fuma:" + clip;
            if (_sounds.TryGetValue(key, out var s)) return s;
            s = Bwa.bwa_load_fuma(_eng, Path.Combine(Application.streamingAssetsPath, clip));
            if (s == 0) Debug.LogWarning("[bw_audio] fuma load failed: " + clip + " (" + Bwa.LastError(_eng) + ")");
            _sounds[key] = s; return s;
        }

        /// <summary>Mint one of the engine's built-in materials. Cached (minted once per preset), so
        /// calling it per scene load is safe — null-engine-safe too.</summary>
        public uint MaterialPreset(BwaMaterialPreset preset) => Ready ? ResolvePreset(preset) : 0;

        /// <summary>Reverb wet level (linear), adjustable live — the reverb-send equivalent.</summary>
        public float ReverbGain
        {
            get => reverbGain;
            set { reverbGain = value; if (Ready) Bwa.bwa_reverb_set_gain(_eng, value); }
        }

        /// <summary>Level of the image-source EARLY reflections (the per-source wall bounces, opted into
        /// with Emitter.earlyReflections). Independent of the late reverb bed's wet level above: early
        /// reflections carry room size and distance, the bed carries the tail. Live.</summary>
        public float EarlyReflectionGain
        {
            get => earlyReflectionGain;
            set { earlyReflectionGain = value; if (Ready) Bwa.bwa_early_reflections_set_gain(_eng, value); }
        }

        /// <summary>Output protection limiter (engine default: ON at -1 dBFS). Linked across the channels —
        /// engaging never shifts the spatial image. Protection against digital overs, not mastering: if it
        /// engages in normal use, turn the content down.</summary>
        public void SetLimiter(bool on) { limiter = on; if (Ready) Bwa.bwa_set_limiter(_eng, on); }
        public void SetLimiterCeiling(float ceilingDb) { limiterCeilingDb = ceilingDb; if (Ready) Bwa.bwa_set_limiter_ceiling(_eng, ceilingDb); }

        /// <summary>Master gain over the whole mix (ramped — slider drags never zipper). The volume knob.</summary>
        public float MasterGain
        {
            get => masterGain;
            set { masterGain = value; if (Ready) Bwa.bwa_set_master_gain(_eng, value); }
        }

        /// <summary>Global pause (focus loss, menu): EVERY voice — sources, streams, beds — ramps out and
        /// freezes; resume continues exactly where it stopped. Paused voices still read as IsPlaying.</summary>
        public bool Paused
        {
            get => _paused;
            set { _paused = value; if (Ready) Bwa.bwa_set_paused(_eng, value); }
        }
        bool _paused;

        /// <summary>Mix-group gain (group 0..7): ducks every emitter in the group — "quiet the SFX, keep the
        /// dialog" without touching each source. Ramped.</summary>
        public void SetGroupGain(uint group, float linear) { if (Ready) Bwa.bwa_group_set_gain(_eng, group, linear); }

        /// <summary>Pause a whole mix group (0..7) — same click-free freeze as per-source pause.</summary>
        public void SetGroupPaused(uint group, bool paused) { if (Ready) Bwa.bwa_group_set_paused(_eng, group, paused); }

        // ---- live rendering A/B (each of these is atomic / crossfaded engine-side) --------------------
        public void SetPanner(BwaPanner p)        { panner = p;        if (Ready) Bwa.bwa_set_panner(_eng, p); }
        public void SetDualBand(bool on)         { dualBand = on;     if (Ready) Bwa.bwa_set_dual_band(_eng, on); }
        public void SetSpreadMode(BwaSpreadMode m){ spreadMode = m;    if (Ready) Bwa.bwa_set_spread_mode(_eng, m); }
        public void SetDecorrelation(bool on)    { decorrelation = on; if (Ready) Bwa.bwa_set_decorrelation(_eng, on); }
        public void SetNearSpread(float radiusM) { nearSpreadRadius = radiusM; if (Ready) Bwa.bwa_set_near_spread(_eng, radiusM); }
        public void SetBedRenderer(BwaBedRenderer r) { bedRenderer = r; if (Ready) Bwa.bwa_set_bed_renderer(_eng, r); }
        public void SetMaxRe(bool on)            { maxRe = on;        if (Ready) Bwa.bwa_set_max_re(_eng, on); }
        public void SetTrackedRoomEq(bool on)    { trackedRoomEq = on; if (Ready) Bwa.bwa_set_tracked_room_eq(_eng, on); }
        /// <summary>Lead the TRACKED pose by `ms` to hide motion-to-ears latency. Internal tracking only
        /// (Feed Listener off) — when Unity feeds the pose, predict on the Unity side instead.</summary>
        public void SetPosePrediction(float ms)  { posePredictionMs = ms; if (Ready && !feedListener) Bwa.bwa_set_pose_prediction(_eng, ms); }

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
            Bwa.bwa_get_bus_levels(_eng, _levels, (uint)_levels.Length);
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
                Bwa.bwa_get_speakers(_eng, _speakers, _channels);
            }
            return _speakers;
        }
        float[] _speakers;

        /// <summary>Voices playing in the last block — a voice-pool gauge for a HUD.</summary>
        public uint ActiveVoices => Ready ? Bwa.bwa_get_active_voices(_eng) : 0;

        /// <summary>The engine's dsp-sample clock (device-anchored, monotonic). Add to it to schedule a
        /// sample-accurate start: <c>DspTime + sampleRate/2</c> plays half a second out.</summary>
        public ulong DspTime => Ready ? Bwa.bwa_get_dsp_time(_eng) : 0;

        /// <summary>The device's own (dsp sample ↔ host time) correspondence for the last rendered
        /// block — the raw pair behind DspTimeAt/RealtimeAt, for callers who want to run their own
        /// clock model. hostTimeNs is monotonic nanoseconds on a backend-defined epoch. False until
        /// audio is running with a host-stamped backend.</summary>
        public bool GetClock(out ulong dspSample, out ulong hostTimeNs)
        {
            dspSample = 0; hostTimeNs = 0;
            return Ready && Bwa.bwa_get_clock(_eng, out dspSample, out hostTimeNs);
        }

        /// <summary>Device-reported render→DAC output latency, frames at the engine rate (DVS includes
        /// its Dante network buffering). A sound scheduled for dsp time T is HEARD at T + OutputLatency —
        /// fold it into AV alignment together with your measured display delay. 0 = unknown / no
        /// physical output (the silent null-sink fallback).</summary>
        public uint OutputLatency => Ready ? Bwa.bwa_get_output_latency(_eng) : 0;

        /// <summary>Map a Time.realtimeSinceStartupAsDouble moment to the dsp-sample clock — THE way
        /// to land a sound on a visual event: <c>emitter.PlayAt(engine.DspTimeAt(tEvent))</c> (schedule
        /// with margin; a start in the past plays immediately). Built on the device's own block stamps
        /// (GetClock) with a continuously refreshed epoch offset, so it self-corrects device↔OS clock
        /// drift; typically accurate to well under a millisecond, falling back to block-granular
        /// DspTime pairing (~one block) when no device stamp exists. For events that live on the AUDIO
        /// timeline (cues in a scheduled track), skip wall time entirely — use t0 + cue×rate.</summary>
        public ulong DspTimeAt(double realtime)
        {
            RefreshClock();
            double fs = sampleRate;
            if (!_clkValid)
            {
                double f = (double)DspTime + (realtime - Time.realtimeSinceStartupAsDouble) * fs;
                return f > 0 ? (ulong)f : 0;
            }
            double dsp = _clkSampleD + (realtime + _clkOffset - _clkHostSec) * fs;
            return dsp > 0 ? (ulong)dsp : 0;
        }

        /// <summary>Inverse of DspTimeAt: the Time.realtimeSinceStartupAsDouble moment at which a dsp
        /// sample is RENDERED (add OutputLatency/sampleRate for when it is heard) — for firing visuals
        /// off an audio-timeline event.</summary>
        public double RealtimeAt(ulong dspSample)
        {
            RefreshClock();
            double fs = sampleRate;
            if (!_clkValid)
                return Time.realtimeSinceStartupAsDouble + ((double)dspSample - (double)DspTime) / fs;
            return _clkHostSec + ((double)dspSample - _clkSampleD) / fs - _clkOffset;
        }

        // The wall↔dsp correspondence: the driver-stamped pair is exact, so the only thing to
        // estimate is the constant epoch offset between the driver's host clock and Unity's realtime
        // clock. Each refresh observes (offset − pair age), the age being up to a block plus
        // scheduling noise — so a decaying MAX converges on the true offset (refreshes landing just
        // after a bufferSwitch have ~zero age) while the small decay lets it track ppm-scale clock
        // drift. Guards reset the model across an engine restart or an epoch change.
        void RefreshClock()
        {
            if (!Ready || !Bwa.bwa_get_clock(_eng, out ulong cs, out ulong ct)) { _clkValid = false; return; }
            double hostSec = ct * 1e-9;
            double cand = hostSec - Time.realtimeSinceStartupAsDouble;
            if (!_clkValid || cs < _clkSample || Math.Abs(cand - _clkOffset) > 0.5)
                _clkOffset = cand;                                   // first pair / restart / epoch change
            else
                _clkOffset = Math.Max(cand, _clkOffset - 2e-6);      // decaying max (covers clock drift)
            _clkSample = cs; _clkSampleD = cs; _clkHostSec = hostSec; _clkValid = true;
        }
        bool _clkValid; ulong _clkSample; double _clkSampleD, _clkHostSec, _clkOffset;

        /// <summary>Drive ONE raw output channel with a test tone — a speaker-check / wiring tool, injected
        /// after the per-speaker trims. NOT a spatial path (it bypasses the panner). gain 0 or Off silences.</summary>
        public void TestSignal(uint channel, BwaTestKind kind, float gain) { if (Ready) Bwa.bwa_set_test_signal(_eng, channel, kind, gain); }

        // ---- dynamic (movable) acoustic geometry --------------------------------------------------
        // Register a movable occluder/reflector from a LOW-POLY mesh (the acoustic analogue of a physics
        // collider with a transform). The mesh is baked ONCE into room-handed local space — the object's
        // SCALE plus the baseline X-flip, winding reversed to keep front faces; the registration ROTATION
        // rides the per-frame quaternion (Room.Rot), so it is NOT baked here (see Room.cs). Position and
        // rotation are pushed live via SetDynamicTransform, which only moves the instance transform (a
        // cheap BVH refit), so occlusion + real-time reflections track it. Scale is captured here
        // (rigid-body assumption); to rescale, remove and re-add. Returns a handle >= 0, or -1 (no SDK /
        // bad mesh / table full). Call after the engine is Ready. Usually driven by DynamicAcousticGeometry.
        public int AddDynamicMesh(Mesh mesh, Transform t, MaterialAsset material)
        {
            if (!Ready || mesh == null || t == null) return -1;
            var ls = t.lossyScale;
            var mv = mesh.vertices;
            var verts = new float[mv.Length * 3];
            for (int i = 0; i < mv.Length; i++)
            {
                verts[i * 3 + 0] = -(mv[i].x * ls.x);        // X-flip the scaled local vertex -> room handedness
                verts[i * 3 + 1] =   mv[i].y * ls.y;
                verts[i * 3 + 2] =   mv[i].z * ls.z;
            }
            // Winding flips iff the baked linear map (X-flip * scale) has negative determinant. Room's own
            // helper computes exactly that sign (the object's rotation/translation don't change it).
            bool reverse = Room.ReversesWinding(t.localToWorldMatrix);
            var mt = mesh.triangles;
            var tris = new int[mt.Length];
            for (int i = 0; i < mt.Length; i += 3)
            {
                tris[i] = mt[i];
                if (reverse) { tris[i + 1] = mt[i + 2]; tris[i + 2] = mt[i + 1]; }
                else         { tris[i + 1] = mt[i + 1]; tris[i + 2] = mt[i + 2]; }
            }
            uint mat = ResolveMaterial(material);          // cached: re-adding across scene loads won't leak the table
            int h = Bwa.bwa_scene_add_dynamic_mesh(_eng, verts, mv.Length, tris, mt.Length / 3, mat);
            if (h >= 0) SetDynamicTransform(h, t);           // place it at its current pose immediately
            else Debug.LogWarning("[bw_audio] AddDynamicMesh failed: " + Bwa.LastError(_eng));
            return h;
        }

        /// <summary>Push a dynamic mesh's live room-space pose (position + rotation). Per-frame-safe.</summary>
        public void SetDynamicTransform(int handle, Transform t)
        {
            if (!Ready || handle < 0 || t == null) return;
            var p = Room.Pos(t.position);
            var q = Room.Rot(t.rotation);
            Bwa.bwa_scene_set_dynamic_transform(_eng, handle, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
        }

        /// <summary>Remove a dynamic mesh registered with AddDynamicMesh.</summary>
        public void RemoveDynamicMesh(int handle)
        {
            if (!Ready || handle < 0) return;
            Bwa.bwa_scene_remove_dynamic_mesh(_eng, handle);
        }

        // ---- acoustic scene baking (load-time bake + per-scene re-bake) ---------------------------
        // Collect every AcousticGeometry (+ the optional room box) into ONE static mesh and hand it to
        // the engine. Done once at start and again whenever a Unity scene loads/unloads (below), since
        // bwa_scene_set_mesh_mat is now runtime-safe.
        /// <summary>Mint a MaterialAsset once and reuse it for the engine's lifetime (across scene loads).
        /// null -> the default material (token 0). Use this everywhere instead of MaterialAsset.Resolve so
        /// repeated scene loads never exhaust the 64-slot material table.</summary>
        public uint ResolveMaterial(MaterialAsset a)
        {
            if (a == null) return 0;
            if (_matCache.TryGetValue(a, out var t)) return t;
            t = a.Resolve(_eng); _matCache[a] = t; return t;
        }
        uint ResolvePreset(BwaMaterialPreset p)
        {
            if (_presetCache.TryGetValue(p, out var t)) return t;
            t = Bwa.MaterialPreset(_eng, p); _presetCache[p] = t; return t;
        }
        /// <summary>Release a material minted via ResolveMaterial so the engine can reuse its table slot
        /// (evicts the cache; a later ResolveMaterial re-mints). Caller-managed lifetime — only release a
        /// material no live mesh or dynamic occluder still references. The mint-once cache handles the
        /// common case; this is for apps that churn many distinct materials over a long session.</summary>
        public void ReleaseMaterial(MaterialAsset a)
        {
            if (!Ready || a == null) return;
            if (_matCache.TryGetValue(a, out var t)) { Bwa.bwa_material_release(_eng, t); _matCache.Remove(a); }
        }

        // Load-time (Awake) scene bake. The box-ONLY case uses the engine's own box helper, which also
        // seeds the ISM early-reflection room — valid only before start, so it stays on this path. Any
        // acoustic geometry goes through RebuildSceneMesh, which is runtime-safe and shared with the
        // per-scene re-bake below.
        void SetupScene()
        {
            var geos = FindObjectsByType<AcousticGeometry>(FindObjectsSortMode.None);
            bool haveGeo = geos != null && geos.Length > 0;
            if (!haveGeo && enableRoomBox)
            {
                uint mb = ResolvePreset(roomMaterial);
                var faces = new[] { mb, mb, mb, mb, mb, mb };
                Bwa.bwa_scene_set_box(_eng, roomSizeMetres.x, roomSizeMetres.y, roomSizeMetres.z, faces);
                _hasStaticMesh = true;
                return;
            }
            RebuildSceneMesh(geos);
        }

        // Collect all currently-loaded AcousticGeometry (+ the optional box, baked as geometry) into ONE
        // mesh and push it. Uses bwa_scene_set_mesh_mat only — runtime-safe (a BVH rebuild, fine on a
        // scene transition), and it never touches bwa_scene_set_box's ISM path. Called at load time and
        // on every scene load/unload so the acoustic scene follows Unity's loaded scenes (additive
        // included: FindObjectsByType spans them all).
        void RebuildSceneMesh(AcousticGeometry[] geos)
        {
            var verts = new List<float>(); var tris = new List<int>(); var triMat = new List<uint>();
            if (enableRoomBox)
                AddBox(verts, tris, triMat, roomSizeMetres, ResolvePreset(roomMaterial));
            if (geos != null)
                foreach (var g in geos)
                {
                    var mesh = g.ResolveMesh();
                    if (mesh == null) { Debug.LogWarning("[bw_audio] AcousticGeometry with no mesh: " + g.name); continue; }
                    AddMesh(verts, tris, triMat, mesh, g.transform.localToWorldMatrix, ResolveMaterial(g.material));
                }
            if (tris.Count == 0) { ClearStaticMesh(); return; }
            Bwa.bwa_scene_set_mesh_mat(_eng, verts.ToArray(), verts.Count / 3, tris.ToArray(), tris.Count / 3, triMat.ToArray());
            _hasStaticMesh = true;
            Debug.Log($"[bw_audio] acoustic scene: {verts.Count / 3} verts, {tris.Count / 3} tris");
        }

        // Drop the prior scene's static geometry when nothing is loaded. bwa_scene_set_mesh_mat rejects
        // an empty mesh, so replace it with a tiny degenerate triangle far outside the room (no ray
        // reaches it). Only when a mesh is actually up, so an empty install pushes nothing.
        void ClearStaticMesh()
        {
            if (!_hasStaticMesh) return;
            var v = new[] { 1000f, 1000f, 1000f,  1000.02f, 1000f, 1000f,  1000f, 1000.02f, 1000f };
            Bwa.bwa_scene_set_mesh_mat(_eng, v, 3, new[] { 0, 1, 2 }, 1, new uint[] { 0 });
            _hasStaticMesh = false;
        }

        // A scene loaded/unloaded (sceneUnloaded fires AFTER its objects are destroyed, so a re-collect
        // naturally drops them). Defer to LateUpdate so several additive loads in one frame coalesce into
        // a single BVH rebuild.
        void OnSceneChanged(Scene s, LoadSceneMode m) => _rebakeStatic = true;
        void OnSceneChanged(Scene s)                  => _rebakeStatic = true;

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
        // bwa_scene_set_box), inward-facing normals (the listener is inside).
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

        readonly List<Emitter> _pushBuf = new();   // reusable snapshot (Push may unregister mid-loop)

        public void Register(Emitter e)   { if (!_emitters.Contains(e)) _emitters.Add(e); }
        public void Unregister(Emitter e) => _emitters.Remove(e);

        void LateUpdate()
        {
            if (!Ready) return;
            RefreshClock();   // keep the wall↔dsp offset estimator warm even when no helper ran this frame
            if (_rebakeStatic)   // a scene loaded/unloaded since last frame -> re-collect static geometry once
            {
                _rebakeStatic = false;
                RebuildSceneMesh(FindObjectsByType<AcousticGeometry>(FindObjectsSortMode.None));
            }
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
                Bwa.bwa_set_listener_pose(_eng, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
            }
            PushExtraListeners();                                  // ...the other occupants (commit-gated too)...
            Bwa.bwa_commit(_eng);                                    // ...then one atomic snapshot
        }

        // The other occupants, for compromise panning. Commit-gated like the primary pose, so it belongs in
        // the same frame block. The engine takes at most BWA_EXTRA_LIS (3); the buffer is reused (no per-frame
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
            Bwa.bwa_set_extra_listeners(_eng, _extraBuf, (uint)n);
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
            if (Instance == this)   // only the live engine subscribed (after claiming Instance)
            {
                SceneManager.sceneLoaded   -= OnSceneChanged;
                SceneManager.sceneUnloaded -= OnSceneChanged;
            }
            if (!Ready) return;
            Bwa.bwa_stop(_eng); Bwa.bwa_destroy(_eng); _eng = IntPtr.Zero;
            if (Instance == this) Instance = null;
        }
    }
}
