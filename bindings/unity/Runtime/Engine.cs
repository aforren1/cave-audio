// Engine.cs — the manager. ONE per scene (singleton). Owns the engine handle, loads assets, and
// runs the CENTRALIZED per-frame push: all sources, then the listener, then one commit — so every
// block the audio thread sees is internally consistent (Unity does not order LateUpdate across
// components, so per-emitter pushes could commit a half-moved frame). See docs/integration.md.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace BwAudio
{
    /// <summary>What <see cref="Engine.fallbackProfile"/> may name: a headphone profile to run when
    /// the primary device is not on this machine. The values match <see cref="BwaProfile"/>, and
    /// None takes the slot of Cave, which is never a desk fallback.</summary>
    public enum BwaFallbackProfile : int { None = 0, Binaural = 1, CaveSim = 2 }

    [DefaultExecutionOrder(-100)]
    [DisallowMultipleComponent]   // one native engine per object; the singleton check covers the rest
    public sealed class Engine : MonoBehaviour
    {
        public static Engine Instance { get; private set; }

        [Header("Cluster")]
        [Tooltip("Whether this process opens the audio device. Off: the Engine applies Room Origin and " +
                 "does nothing else. It creates no native engine, opens no device, and every BwAudio " +
                 "component is a silent no-op. In a cluster, set Engine.OwnsAudioPredicate from your " +
                 "cluster toolkit instead and leave this on. Read once, in Start.")]
        public bool ownsAudio = true;

        /// <summary>A process-wide answer to "does this process own the audio device", for a cluster
        /// where every node runs the same scene and exactly one may open the device. Set it before the
        /// scene loads, for example from a
        /// <c>[RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]</c> method
        /// that asks the cluster toolkit whether this node is the server. Null means own audio.
        /// <para>Engine.Start calls it ONCE. Start runs after every Awake and OnEnable of the scene,
        /// whatever the script execution orders are, so a toolkit that learns the node's role in its
        /// own Awake has it by then. The answer cannot change for that Engine's lifetime. A toolkit
        /// that learns the role later than that: keep the Engine component disabled and enable it
        /// when the role is known (Start waits for the enable). <see cref="StartNow"/> asks it
        /// earlier, at the caller's risk. An Engine owns audio only when both
        /// this and <see cref="ownsAudio"/> say so. A predicate that throws is logged and read as
        /// false. It is cleared on every entry into Play mode.</para></summary>
        public static Func<bool> OwnsAudioPredicate;

        /// <summary>A process-wide source for the listener Transform, for a head that does not exist
        /// when the scene is saved: a VR toolkit that creates its head node at run time. While
        /// <see cref="listener"/> is unassigned (or was destroyed), the Engine calls this a few times
        /// a second until it returns a live Transform, assigns that to <see cref="listener"/>, and
        /// stops asking. <see cref="listenerName"/> is tried after it. Return null for "not yet".
        /// <para>Static for the reason <see cref="OwnsAudioPredicate"/> is: the code that knows the
        /// toolkit runs before the scene loads and has no Engine to hold, and the Engine is a
        /// singleton that outlives scenes, so a per-instance hook would have nothing to add. Set it
        /// from the same bootstrap method. Asked only on the node that owns audio, and only while
        /// Unity feeds the pose (<see cref="feedListener"/>, or the desk fallback). A provider that
        /// throws is logged once and read as null. Cleared on every entry into Play mode.</para></summary>
        public static Func<Transform> ListenerProvider;

        // With Domain Reload off, statics survive into the next Play session; a predicate captured
        // from the last one must not decide this one. Runs before BeforeSceneLoad, where it is set.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void ResetOnPlay()
        {
            OwnsAudioPredicate = null; ListenerProvider = null; ComponentWaitFrames = 0;
            EditorCheck = DebugBuildCheck = PresentCheck = null;
            OutputLatencyCheck = null; ListenerSearchWarnS = ListenerSearchWarnDefaultS;
        }

        /// <summary>Frames that BwAudio components have spent waiting for an Engine, summed over all of
        /// them. It rises while components exist and no Engine has finished Start. It stays flat on a
        /// node that does not own audio, because nothing waits there. A diagnostic.</summary>
        public static long ComponentWaitFrames { get; private set; }

        /// <summary>The ownership decision Start made. False on a node that leaves the device to
        /// another node: <see cref="Ready"/> stays false, and Room Origin still applies. Also false
        /// until <see cref="Decided"/>.</summary>
        public bool OwnsAudio { get; private set; }

        /// <summary>Whether Start (or <see cref="StartNow"/>) has run: the ownership decision is made
        /// and the native engine is up, declined, or failed. False between Awake and Start. Until
        /// it is true, a false <see cref="Ready"/> means "not yet", and BwAudio components keep
        /// waiting.</summary>
        public bool Decided { get; private set; }

        [Header("Engine")]
        public BwaProfile profile = BwaProfile.Binaural;   // inspector dropdown; maps 1:1 to the C enum
        [Tooltip("The surveyed speaker geometry, as a path RELATIVE TO Assets/StreamingAssets/ " +
                 "(create that folder and put cave_layout.json in it). EMPTY runs the engine's built-in " +
                 "default 26-speaker grid — the only way to opt into it. A non-empty path MUST load: on " +
                 "a failed load the engine logs the reason and REFUSES to start (BWA_ERR_LAYOUT), so a " +
                 "session can never silently pan over geometry that isn't the one in the room. " +
                 "Binaural is the exception: it drives no speakers, so a MISSING file there runs the " +
                 "default grid with a log line instead.")]
        [Clip(".json")] public string layoutFile = "cave_layout.json";
        public uint sampleRate = 48000;
        public uint blockSize = 256;
        [Tooltip("Output-device policy. Auto: for a headphone profile try WASAPI then ASIO, for the " +
                 "array try ASIO (Linux: JACK then ALSA; macOS: CoreAudio; Android: AAudio), and fall " +
                 "back to the SILENT offline sink either way (the engine " +
                 "keeps rendering — the inspector flags the silent fallback). Naming a backend is a " +
                 "demand, so a missing device fails startup loudly. Null: force the offline sink.")]
        public BwaSinkType sink = BwaSinkType.Auto;
        [Tooltip("Device to open, for whichever backend opens it: the exact friendly name or stable " +
                 "id from Engine.Devices. Empty = that backend's default (ASIO: the first driver with " +
                 "enough output channels; WASAPI: the Windows default render endpoint; CoreAudio: " +
                 "the macOS default output device).")]
        // FormerlySerializedAs: this field was `asioDriver` through 0.13, so a scene saved then
        // keeps its value instead of silently reverting to the backend default on load.
        [UnityEngine.Serialization.FormerlySerializedAs("asioDriver")]
        public string device = "";
        [Tooltip("Backend options. Exclusive takes a WASAPI endpoint from every other application on " +
                 "the machine; leave it off for a monitor that shares the device with a VR runtime. " +
                 "ExactRate fails the open rather than letting the OS resample. TightBuffer takes " +
                 "the smallest device buffer the backend can, trading dropout margin for latency. " +
                 "DeepBuffer affects only the browser sink, so it does nothing here.")]
        public BwaSinkFlags sinkFlags = BwaSinkFlags.None;

        [Header("Desk fallback")]
        [Tooltip("What to run when the primary device above is NOT on this machine: a rig scene " +
                 "opened at a desk. CaveSim auditions the array as virtual speakers on headphones " +
                 "(same layout file, room modeling kept). Binaural is the direct headphone render " +
                 "(no speaker trims or correction filters). None = off: a missing device behaves " +
                 "as the Sink field says. Decided once, in Start, before the engine is created, " +
                 "from the device LISTS: a listed device that fails to open still fails the start " +
                 "(or runs silent under Auto), and an installed driver with its hardware " +
                 "unplugged reads as present.")]
        public BwaFallbackProfile fallbackProfile = BwaFallbackProfile.None;
        [Tooltip("Sink for the fallback. Auto with an empty Fallback Device opens the machine's " +
                 "default stereo output.")]
        public BwaSinkType fallbackSink = BwaSinkType.Auto;
        [Tooltip("Device for the fallback, as in Device above. Empty = the backend's default.")]
        public string fallbackDevice = "";
        [Tooltip("Backend options for the fallback. The primary's Sink Flags do not carry over.")]
        public BwaSinkFlags fallbackSinkFlags = BwaSinkFlags.None;
        [Tooltip("On: the fallback is taken only in the Editor and in development builds. A " +
                 "release build then behaves as if Fallback Profile were None, so a rig that " +
                 "loses its device fails or goes silent as before and never plays to headphones. " +
                 "Where the fallback is allowed, a typo in Device or an uninstalled driver is a " +
                 "headphone fallback with one log line, not a failed start: do not run the rig " +
                 "from a development build with a fallback set.")]
        public bool fallbackOnlyInDevBuilds = true;

        /// <summary>True while the engine runs on <see cref="fallbackProfile"/> because Start did
        /// not find the primary device. False until the engine is <see cref="Ready"/>.</summary>
        public bool UsingFallback => _fallbackTaken && Ready;
        bool _fallbackTaken;

        /// <summary>The profile the native engine was created with: <see cref="profile"/>, or the
        /// fallback while the engine runs on it. It reads as profile before Start, after a start
        /// that failed (the fallback's included), and after the Engine is destroyed.</summary>
        public BwaProfile ActiveProfile => _fallbackTaken ? _activeProfile : profile;
        BwaProfile _activeProfile;
        // On the fallback with Feed Listener off: the Transform that stands in for the tracker.
        Transform _fallbackListener;
        // Unity feeds the pose: the scene asked for it, or the fallback replaced the tracker.
        bool FeedsListener => feedListener || _fallbackTaken;

        // Test seams, all null in use. EditorCheck and DebugBuildCheck stand in for
        // Application.isEditor and Debug.isDebugBuild. PresentCheck replaces the device-list
        // answer in Start only, for the one case no machine can stage: a primary that is listed
        // and does not open.
        internal static Func<bool> EditorCheck, DebugBuildCheck, PresentCheck;
        static bool InEditor() => EditorCheck != null ? EditorCheck() : Application.isEditor;
        static bool IsDevBuild()
            => InEditor() || (DebugBuildCheck != null ? DebugBuildCheck() : Debug.isDebugBuild);

        /// <summary>Whether a fallback set on this Engine may run in this build: the Editor or a
        /// development build, or any build with <see cref="fallbackOnlyInDevBuilds"/> off.</summary>
        public bool FallbackAllowed
            => fallbackProfile != BwaFallbackProfile.None && (!fallbackOnlyInDevBuilds || IsDevBuild());

        /// <summary>Whether the device a (profile, sink, device) triple asks for is LISTED on this
        /// machine: <c>bwa_device_present</c>. Engine-free, and it opens nothing. It is the test
        /// the desk fallback runs in Start.
        /// <para>ASIO, WASAPI or CoreAudio with a device: that backend lists the exact name or id.
        /// Any named backend with no device: it lists at least one. Auto: the same two questions,
        /// asked of the backends the engine's Auto order tries on this platform for that
        /// profile's width (Cave and CaveBoth are the array, the other two are stereo). Null and
        /// Manual open no device and are always present.</para>
        /// <para>JACK, ALSA, AAudio and Worklet named as the sink, with a device: present on a
        /// platform whose engine library carries that backend. Those backends do not look a
        /// device up in their own list (JACK takes a port pattern, ALSA any PCM string, AAudio a
        /// numeric id), so the list cannot call one missing. Without the backend it is missing:
        /// an ALSA scene on a Windows desk takes the fallback. CaveBoth: only the array's device
        /// is checked, not the monitor's.</para>
        /// <para>What it cannot see: whether a listed device will OPEN. An ASIO driver is listed
        /// when it is installed, with or without its hardware. ALSA and CoreAudio list a default
        /// device on any machine. A listed device with too few channels, the wrong rate, or
        /// another owner reads as present too. Present and unopenable is a failed start under a
        /// named sink, and the silent offline sink under Auto.</para></summary>
        public static bool DevicePresent(BwaProfile profile, BwaSinkType sink, string device)
            => Bwa.bwa_device_present(profile, sink, string.IsNullOrEmpty(device) ? null : device);

        /// <summary>The old spelling of <see cref="device"/>, kept so existing scripts compile.</summary>
        public string asioDriver { get { return device; } set { device = value; } }

        [Header("Room registration")]
        [Tooltip("Where the physical room sits in this scene. Put an empty GameObject at the room-frame " +
                 "origin (the CAVE floor center) with its forward (blue, +Z) along the layout's +Z, and " +
                 "assign it. The listener, every source, the geometry and the gizmos are then relative " +
                 "to it. It must match the tracker's frame, because the layout was measured there: if " +
                 "the layout's +Z points at the back wall, yaw the origin 180 degrees. Followed every " +
                 "frame. Its scale is ignored. Empty = Room.UnityToRoom is left alone (identity unless " +
                 "your code sets it).")]
        public Transform roomOrigin;

        [Header("Listener")]
        [Tooltip("The tracked head: your OptiTrack rigid body, or the XR camera at a desk. Required " +
                 "when Feed Listener is on — without it the listener never moves from the array centroid.")]
        public Transform listener;          // OptiTrack head rigid body, or the XR camera at a desk
        [Tooltip("Find the listener by GameObject name while Listener is empty: for a head node that a " +
                 "VR toolkit creates at run time, after the scene starts. Looked up a few times a " +
                 "second until it exists, then assigned to Listener. Looked up again if that object is " +
                 "destroyed. A '/' makes it a hierarchy path (Rig/Head). Empty = off. " +
                 "Engine.ListenerProvider is the code route and is tried first.")]
        public string listenerName = "";
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
                 "motion-to-ears latency, in SECONDS (typically 0.02-0.04 s). Too much lead overshoots on " +
                 "direction changes. 0 = off.")]
        [Range(0f, 0.2f)] public float posePredictionS = 0f;

        [Header("AV sync")]
        [Tooltip("The MEASURED time from a frame's buffer swap to its light leaving the display or " +
                 "projector, in seconds. PlayAtHostTime adds it, so a sound scheduled for a frame is " +
                 "heard when that frame is SEEN, not when it is swapped. Measure it once per display " +
                 "(a photodiode against a loopback of the audio). 0 = unknown, treated as none. " +
                 "Negative counts as 0 and anything over 10 s as 10 s.")]
        [Min(0f)] public float displayLatencySeconds = 0f;

        [Header("Output")]
        [Tooltip("One ramped scalar over the whole mix — applied before the per-speaker trims (which stay " +
                 "calibrated) and before the limiter. The volume knob / scene fade.")]
        [Range(0f, 2f)] public float masterGain = 1f;
        [Tooltip("Output protection limiter (engine default: ON). Linked across channels, so engaging " +
                 "never shifts the spatial image. Protection against digital overs, NOT mastering — if it " +
                 "engages in normal use, turn the content down.")]
        public bool limiter = true;
        [Tooltip("Limiter ceiling as a LINEAR peak amplitude in (0..1] (like every other gain). " +
                 "0.891251 = -1 dBFS, the default.")]
        [Range(0.001f, 1f)] public float limiterCeiling = 0.891251f;   // floor > 0: the engine IGNORES <= 0, so a 0 here would silently diverge from it

        [Header("Panning")]
        [Tooltip("DBAP: listener-relative, for a MOVING observer (the CAVE case). SPCAP/VBAP assume a " +
                 "FIXED listener — sharper, but only at the sweet spot.")]
        public BwaPanner panner = BwaPanner.Dbap;
        [Tooltip("Split at ~700 Hz and pan the low band with amplitude normalization: sharper LF " +
                 "localization for a near-centered listener. Sweet-spot dependent.")]
        public bool dualBand = false;
        [Tooltip("Needs Dual Band. Corrects the low band's interaural time difference for the tracked " +
                 "head ORIENTATION, so a source holds still as the listener turns their head. Wants a " +
                 "real tracked pose; aimed at a seated listener.")]
        public bool dualBandCap = false;
        [Tooltip("SPCAP only: lobe sharpness. Higher concentrates a source on fewer speakers (tighter " +
                 "image), lower spreads it (smoother). 0 or less = the default derived from your array's " +
                 "speaker spacing (about 12.7 on the 26-speaker grid).")]
        public float spcapFocus = 0f;
        [Tooltip("SPCAP only: exponent of the placement-correction kernel that de-biases a clustered " +
                 "array. 0 or less = the 2.0 default, which is rarely worth moving.")]
        public float spcapDensity = 0f;
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
        [Tooltip("Widen sources aimed where the array has NO speaker (the CAVE barrel is open at both " +
                 "poles), instead of rendering them as a split image across two distant speakers. Inert " +
                 "on an array that surrounds the listener. 1 = the honest width; 0 = off.")]
        [Range(0f, 2f)] public float holeSpread = 0f;
        [Tooltip("Speed of sound (m/s; live). Doppler and reflection delays derive from it and GLIDE to " +
                 "a change. 343 = air, 1480 = underwater; small values exaggerate Doppler (slow motion).")]
        public float speedOfSound = 343f;
        [Tooltip("For layouts carrying a room_eq_grid (bwa_calibrate --room-eq-grid): re-interpolate the LF " +
                 "modal cuts at the live listener position. No-op without a grid; this is the kill switch.")]
        public bool trackedRoomEq = true;
        [Tooltip("For layouts carrying a directivity model (tools/directivity/clf_to_json.py from the speaker " +
                 "vendor's CLF file): follow each speaker's off-axis loss onto the tracked head as a gain + HF " +
                 "shelf per speaker. No-op without a model; this is the kill switch.")]
        public bool trackedDirectivity = true;
        [Tooltip("Re-align the array's arrival times on the TRACKED listener instead of the fixed array " +
                 "centroid, so time coherence follows the head. OFF by default: every delay change resamples, " +
                 "so a walking listener Doppler-shifts the whole array. Try it, listen for warble.")]
        public bool trackedAlign = false;
        [Tooltip("Tracked align: how far the head must move (METERS) before the alignment is recomputed. " +
                 "Tracker jitter would otherwise keep the array permanently gliding. 0 = the 5 cm default.")]
        [Range(0f, 0.5f)] public float trackedAlignDeadZone = 0f;
        [Tooltip("Tracked align: cap on how fast a speaker's delay may change, in AUDIO frames per second " +
                 "(not video FPS). This over the sample rate is the resampling ratio, so it is what bounds " +
                 "the pitch shift. 0 = the default (~63 at 48 kHz, following a 0.45 m/s walk); higher tracks " +
                 "a faster listener and is more audible.")]
        public float trackedAlignSlewFramesPerSecond = 0f;

        [Header("Diffuse beds (AmbisonicBed / reverb)")]
        [Tooltip("Load-time. AllRAD (default) localizes a touch sharper; EPAD keeps a panned wave's loudness " +
                 "constant over direction by construction (flattest on an irregular array). A by-ear call.")]
        public BwaBedDecoder bedDecoder = BwaBedDecoder.Default;
        [Tooltip("MATRIX: the static SH->speaker decode. PARAMETRIC: DirAC analysis re-pans the directional " +
                 "part through the listener-relative panner — a recorded soundfield becomes WALKABLE " +
                 "(correct directions + parallax off-center). Live: beds crossfade, so it A/Bs.")]
        public BwaBedRenderer bedRenderer = BwaBedRenderer.Matrix;
        [Tooltip("max-rE weighting on the bed decode (and the FDN's render): tapers the high ambisonic " +
                 "orders — fewer decode sidelobes, better localization AWAY from the sweet spot (the " +
                 "walking-listener case), slightly wider main lobe. Live A/B, level-fair.")]
        public bool maxRe = true;
        [Tooltip("Band-split max-rE (needs Max Re on): apply the taper only ABOVE ~700 Hz and keep the " +
                 "unweighted decode below — the ear localizes LF by pressure (plain decode) and HF by " +
                 "energy (max-rE). The Gerzon basic-LF/max-rE-HF split. Bed matrix decodes only. Live A/B.")]
        public bool maxReSplit = false;

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
                 "level — so you see the geometry actually being panned over. An empty Layout File shows " +
                 "as the default 26-grid, not your room.")]
        public bool showSpeakers = true;
        [Range(0.02f, 0.5f)] public float speakerGizmoRadius = 0.12f;
        public bool showSpeakerIndices = true;

        [Header("Room box (load-time; optional acoustic geometry)")]
        [Tooltip("A shoebox enclosure for occlusion/reflections, drawn as a yellow wireframe in the " +
                 "scene view. FLOOR-BASED: centered on the origin in x/z, running from y=0 up to its " +
                 "height. For anything more detailed, use AcousticGeometry instead.")]
        public bool enableRoomBox = false;
        public Vector3 roomSizeMeters = new Vector3(3f, 3f, 3f);
        [Tooltip("Material for all six faces. For per-face or custom materials, build the room out of " +
                 "AcousticGeometry objects instead.")]
        public BwaMaterialPreset roomMaterial = BwaMaterialPreset.Concrete;

        IntPtr _eng;
        public IntPtr Handle => _eng;
        public bool Ready => _eng != IntPtr.Zero;

        readonly List<SourceBase> _sources = new();   // every source component (Emitter + PushEmitter), one registry
        readonly List<AmbisonicBed> _beds = new();    // every bed component; the event route, see RegisterBed
        // The path->handle dictionary this used to keep is GONE: bwa_sound_acquire is a by-path cache
        // keyed on (path, flags) inside the engine, so the same clip returns the same handle and the
        // "ambix:"/"fuma:" key prefixes this binding invented (one path, several loaders) are the
        // engine's own key now. What stays is a NEGATIVE cache, and only because the ABI has no
        // negative one: a SYNCHRONOUS acquire that fails inserts no entry, so without this a missing
        // clip would re-open the file and re-log the warning on every Play. Keys are "<flags>|<clip>".
        readonly HashSet<string> _loadFailed = new();
        // Material tokens are minted into a FIXED 64-slot engine table and never freed, so mint each
        // MaterialAsset / preset ONCE and reuse it across every scene load — re-minting per load leaks
        // the table (multi-scene games hit this fast). The cache is engine-lifetime (assets are Project
        // objects that survive scene unloads), unlike the old per-SetupScene-call cache.
        readonly Dictionary<MaterialAsset, uint> _matCache = new();
        readonly Dictionary<BwaMaterialPreset, uint> _presetCache = new();
        bool _rebakeStatic;      // static AcousticGeometry changed: re-collect it in the next LateUpdate
        bool _staticHasGeometry; // the static mesh holds AcousticGeometry, which bakes the registration (the box does not)
        bool _hasStaticMesh;     // whether a non-empty static mesh is currently pushed (so we know to clear it)

        // Awake does only what must not wait and depends on nobody else: the singleton and the room.
        // The ownership decision and the native engine are Start's. Unity does not order two scripts
        // with the same execution order, so a cluster toolkit that learns the node's role in its own
        // Awake may run after this one. Start runs after every Awake of the scene.
        void Awake()
        {
            if (Instance != null)                                    // a manager already holds the scene
            {
                // This scene brought its own Engine, and its origin says where the room is in THIS
                // scene. The survivor came from another scene and would keep that scene's registration.
                if (roomOrigin) Instance.AdoptRoomOrigin(roomOrigin, transform);
                Destroy(gameObject); return;
            }

            // Claimed here, before Start decides: a second scene's Engine must find the survivor, and
            // SpeakerView and Room users find the Engine. Components read Decided to know whether a
            // not-Ready Engine is still coming up (ForComponent).
            Instance = this;
            DontDestroyOnLoad(gameObject);

            // Every node of a cluster draws with Room.FromRoom from the first frame, so the
            // registration must not depend on who owns audio, or wait for the answer.
            ApplyRoomOrigin();
        }

        // Start, not Awake: see Awake. At -100 it still runs before every default-order Start, so a
        // scene's Emitters create their voices in their own Start against a running engine.
        void Start() => StartNow();

        /// <summary>Make the ownership decision and bring the native engine up NOW, instead of
        /// waiting for Unity to call Start. For a caller that needs <see cref="Ready"/> earlier than
        /// the scene's Start: its own Awake or OnEnable, or the frame it added this Engine from a
        /// script. Runs once. Every later call does nothing, Unity's own Start included. It also does
        /// nothing before this Engine's Awake, and on a duplicate Engine that is about to destroy
        /// itself. Check <see cref="Ready"/> afterwards: the node may not own audio, or the start
        /// may have failed.
        /// <para>In a cluster, do NOT call it from an Awake. It asks
        /// <see cref="OwnsAudioPredicate"/> on the spot, and a toolkit that learns the node's role
        /// in its own Awake may not have run yet. That is the ordering bug the wait for Start
        /// exists to remove.</para></summary>
        public void StartNow()
        {
            // A duplicate that Awake marked for destruction is still alive this frame. _bringUpBegun
            // and not Decided: a predicate or a held call may call back in here mid-way.
            if (Instance != this || _bringUpBegun) return;
            _bringUpBegun = true;
            // A throw in here (a missing native library, a scene bake that fails) must still end the
            // components' wait, and must not leave a created engine that never started reading as
            // Ready. So it ends as a failed start does: logged, no native engine, nothing held.
            try { BringUp(); }
            catch (Exception ex)
            {
                Debug.LogException(ex, this);
                if (_eng != IntPtr.Zero)
                {
                    var dead = _eng; _eng = IntPtr.Zero;
                    try { if (_started) Bwa.bwa_stop(dead); Bwa.bwa_destroy(dead); }
                    catch (Exception ex2) { Debug.LogException(ex2, this); }
                }
                _started = false;
                _deferred.Clear();
            }
            finally { Decided = true; }
            // A fallback that did not start is not in use: ActiveProfile reads as profile again.
            if (!Ready) { _fallbackTaken = false; _fallbackListener = null; }
            // On every node, the ones without audio included: they all draw with this registration.
            LogRegistration();
            RunDeferred();
        }
        bool _started;           // bwa_start succeeded: what a failed bring-up has to stop before it destroys
        bool _bringUpBegun;      // StartNow ran or is running: the bring-up happens once

        void BringUp()
        {
            OwnsAudio = ownsAudio && AskOwnsAudio();
            if (!OwnsAudio)
            {
                Debug.Log("[bw_audio] this process does not own audio: no device opened, no native " +
                          "engine. Room Origin still applies.");
                return;
            }

            // ABI guard, before any struct crosses the boundary: a stale staged bw_audio.dll after an
            // ABI break runs with mismatched enums/struct layouts — silent corruption, not a crash. The
            // header guarantees compatibility only within a major.minor (bw_audio.h, BWA_VERSION), so
            // refuse on that; a patch difference is compatible and passes.
            uint dllVersion = Bwa.bwa_get_version();
            if ((dllVersion >> 8) != (Bwa.BoundVersion >> 8))
            {
                Debug.LogError("[bw_audio] ABI version mismatch: bw_audio.dll is " +
                               Bwa.VersionString(dllVersion) + ", this binding was built against " +
                               Bwa.VersionString(Bwa.BoundVersion) + ". Refusing to start. Restage " +
                               "the DLL or update the package so they match.");
                return;
            }

            // Struct-layout guard, same class of failure as the version check above and the one struct
            // the binding can check for free: bwa_source_preset is PURE and fills struct_size with the
            // engine's own sizeof, so comparing it against the marshalled size proves BwaSourceDesc's
            // field list and padding match the C struct. A mismatch is not a crash — the marshaller would
            // hand the engine a short buffer it reads past — so refuse rather than corrupt memory.
            int descSize = System.Runtime.InteropServices.Marshal.SizeOf<BwaSourceDesc>();
            if (Bwa.SourcePreset(BwaSourceKind.Default).structSize != (uint)descSize)
            {
                Debug.LogError("[bw_audio] BwaSourceDesc layout mismatch: the DLL's bwa_source_desc is " +
                               Bwa.SourcePreset(BwaSourceKind.Default).structSize + " bytes, the binding " +
                               "marshals " + descSize + ". Refusing to start. Restage the DLL or update " +
                               "the package so they match.");
                return;
            }

            // The desk fallback, decided here: after the ownership decision (a node that does not
            // own audio never reaches this line) and before bwa_create, from the device lists.
            // Never after a failed bwa_start: a device that is present and will not open is a rig
            // fault, and the rig must say so.
            var useProfile = profile; var useSink = sink; var useDevice = device; var useFlags = sinkFlags;
            _fallbackTaken = false; _fallbackListener = null;
            bool allowed = FallbackAllowed;
            if (allowed && !(PresentCheck != null ? PresentCheck() : DevicePresent(profile, sink, device)))
            {
                useProfile = (BwaProfile)fallbackProfile; useSink = fallbackSink;
                useDevice = fallbackDevice ?? ""; useFlags = fallbackSinkFlags;
                _fallbackTaken = true; _activeProfile = useProfile;
                string msg = "[bw_audio] DESK FALLBACK: the primary device is not on this machine (" +
                             "profile " + profile + ", sink " + sink + ", device " +
                             (device.Length > 0 ? "'" + device + "'" : "(default)") +
                             "). Running profile " + useProfile + " on sink " + useSink + ", device " +
                             (useDevice.Length > 0 ? "'" + useDevice + "'" : "(default)") +
                             " instead. Engine.UsingFallback is true.";
                // In a player this is a development build that may be standing on the rig, where
                // the same line means a typo in Device or a driver that is not installed.
                if (InEditor()) Debug.LogWarning(msg, this);
                else Debug.LogError(msg + " This is a development build: if this machine is the rig, " +
                                    "check Device and the driver.", this);
            }

            // Empty maps to layout_path = NULL, the ABI's only way to run the default grid, exactly
            // like asioDriver below. Path.Combine of an empty string would yield the StreamingAssets
            // DIRECTORY: still an explicit path, which fails to load and then fails bwa_start
            // (BWA_ERR_LAYOUT), leaving the default grid unreachable.
            string layoutFull = layoutFile.Length > 0 ? AssetPath(layoutFile) : null;
            // Binaural drives no speakers: the layout only places the virtual speakers the diffuse
            // bed decodes through, and the default grid serves that fine. The refusal exists so the
            // ARRAY never pans over the wrong room, so a desk scene on the default profile must not
            // go silent for want of a file it does not need. A file that exists still has to load.
            // (On Android StreamingAssets sits inside the APK, which the engine cannot fopen either.)
            if (layoutFull != null && useProfile == BwaProfile.Binaural && !File.Exists(layoutFull))
            {
                Debug.Log($"[bw_audio] Binaural: no layout at '{layoutFull}', so the diffuse bed decodes " +
                          "through the built-in default grid. The array profiles need the file.");
                layoutFull = null;
            }

            var cfg = new BwaDesc {
                profile = useProfile,
                layoutPath = layoutFull,
                hrtfPath = null, sampleRate = sampleRate, blockSize = blockSize,
                sink = useSink,
                device = useDevice.Length > 0 ? useDevice : null,
                sinkFlags = useFlags,
                enablePathing = enablePathing,
                bedDecoder = bedDecoder,
            };
            _eng = Bwa.bwa_create(in cfg);
            if (_eng == IntPtr.Zero) { Debug.LogError("[bw_audio] bwa_create failed"); return; }

            // The layout resolves inside bwa_create, so the channel count is known before start — and it is
            // the LAYOUT's speaker count, not a constant. A failed EXPLICIT load leaves bwa_create usable
            // on the default 26-grid, but bwa_start then REFUSES the session with BWA_ERR_LAYOUT — only an
            // empty Layout File (layout_path = NULL) runs the default grid. bwa_last_error carries the load
            // failure (bwa_create sets it for nothing else), so report it HERE, where the message can name
            // the layout file and the fix, instead of letting the start failure below say only "layout".
            _channels = Bwa.bwa_get_channel_count(_eng);
            var loadErr = Bwa.LastError(_eng);
            if (loadErr != null)
                Debug.LogError($"[bw_audio] layout '{cfg.layoutPath}' did not load, so bwa_start will REFUSE " +
                               $"to run this session (BWA_ERR_LAYOUT). Fix the file, or clear 'Layout File' " +
                               $"to run the built-in default {_channels}-speaker grid deliberately. " +
                               $"Reason: {loadErr}");

            ApplyLoadTimeSettings();
            SetupScene();   // acoustic geometry + optional room box -> the engine's scene (load-time)

            if (Bwa.bwa_start(_eng) != BwaResult.Ok)
            {
                Debug.LogError("[bw_audio] bwa_start failed: " + Bwa.LastError(_eng));
                Bwa.bwa_destroy(_eng); _eng = IntPtr.Zero; return;
            }
            _started = true;
            // Paused is the one piece of standing state with no inspector field, so no settings push
            // carries it: a pause set before Start would be stored and never reach the engine.
            if (_paused) Bwa.bwa_set_paused(_eng, true);

            // The fallback was set and allowed, the device lists said the primary is here, and
            // Auto still ended on the offline sink: the device is listed and did not open. The
            // check cannot see that, so the scene is silent, and that must not pass quietly.
            if (allowed && !_fallbackTaken && sink != BwaSinkType.Null && sink != BwaSinkType.Manual &&
                Bwa.bwa_get_sink_type(_eng) == BwaSinkType.Null)
                Debug.LogError("[bw_audio] NO AUDIO, and the desk fallback did NOT trigger: the primary " +
                               "device (profile " + profile + ", sink " + sink + ", device " +
                               (device.Length > 0 ? "'" + device + "'" : "(default)") + ") is listed on " +
                               "this machine but did not open, so the engine runs the silent offline " +
                               "sink. Reason: " + (Bwa.LastError(_eng) ?? "none given") + ". The check " +
                               "reads device lists: an installed driver with its hardware unplugged, " +
                               "too few channels, a busy device or a wrong rate all read as present. " +
                               "On the rig: fix the device. At a desk: set Profile to " + fallbackProfile +
                               " by hand for this machine.", this);

            // The other silent ending: the fallback was taken and IT did not open (Auto with no
            // output on this machine, or a Fallback Device nobody has). The start line would say
            // backend=null and nothing else, after a warning that promised the fallback.
            if (_fallbackTaken && useSink != BwaSinkType.Null && useSink != BwaSinkType.Manual &&
                Bwa.bwa_get_sink_type(_eng) == BwaSinkType.Null)
                Debug.LogError("[bw_audio] DESK FALLBACK has NO AUDIO either: the fallback (profile " +
                               useProfile + ", sink " + useSink + ", device " +
                               (useDevice.Length > 0 ? "'" + useDevice + "'" : "(default)") +
                               ") did not open, so the engine runs the silent offline sink. Reason: " +
                               (Bwa.LastError(_eng) ?? "none given") + ". Connect headphones or " +
                               "another output, or set Fallback Device to a device this machine has " +
                               "(Engine.Devices lists them; empty is the default output).", this);

            // A late-bound head that exists already: bind it now, ahead of the fallback's choice of a
            // stand-in and ahead of the missing-listener warning.
            if (FeedsListener) ResolveListener(force: true);

            // The fallback with internal tracking: the tracker is the rig's Motive, which a desk
            // does not have, and a connect toward it would open a socket and report success. Feed
            // the pose from Unity instead.
            if (!feedListener && _fallbackTaken)
            {
                var cam = listener ? null : Camera.main;
                _fallbackListener = listener ? listener : (cam ? cam.transform : null);
                Debug.LogWarning("[bw_audio] DESK FALLBACK: Feed Listener is off, but the tracker is not " +
                                 "connected on the fallback. The listener follows " +
                                 (listener ? "the Listener Transform '" + listener.name + "'"
                                  : _fallbackListener ? "the main camera '" + _fallbackListener.name + "'"
                                  : "NOTHING (no Listener Transform and no main camera), so it stays " +
                                    "at the array centroid") + ".", this);
            }
            // Internal tracking: connect the engine to the NatNet stream itself (runtime call — no env
            // vars). Non-fatal: the engine keeps rendering from the committed/default pose, loudly.
            else if (!feedListener)
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

            // Static geometry follows scene loads by itself: AcousticGeometry reports its own enable
            // and disable (StaticGeometryChanged), and a scene load or unload is exactly that.
            Debug.Log($"[bw_audio] started, profile={useProfile}, backend={Bwa.Backend(_eng)}, channels={_channels}");

            // Feeding the pose from Unity with nothing to feed it FROM is a silent no-op: the listener
            // just stays at the array centroid and every source is panned for a head that never moves.
            // A late-bound listener (Listener Name, ListenerProvider) is expected to be missing here:
            // the search in LateUpdate has its own warning, once, when nothing turns up.
            if (feedListener && !listener && !ListenerExpected)
                Debug.LogWarning("[bw_audio] 'Feed Listener' is on but no listener Transform is assigned: " +
                                 "the listener will never move from the array centroid. Assign the tracked " +
                                 "head (or turn Feed Listener off to let the engine read NatNet itself).");
        }

        // ---- the late-bound listener ------------------------------------------------------------------
        //
        // A VR toolkit can create its head node after the scene starts, so `listener` cannot always be
        // wired in the inspector. While it is empty the Engine asks ListenerProvider, then looks
        // listenerName up, on a timer: GameObject.Find walks every root object, which is too slow for
        // every frame and fast enough for four times a second. Once the warning has fired the name is
        // probably wrong, and a wrong name would otherwise cost a scene walk four times a second for
        // the whole session, on the node whose frames a cluster is locked to: it drops to once a
        // second. A destroyed listener reads as empty
        // again (Unity's null), so the same search re-binds it after a toolkit rebuilds its rig.
        // Only where the pose is used: the owner, while Unity feeds it. A node that does not own
        // audio has nothing to feed, and under internal tracking the engine reads the tracker itself.
        const float ListenerSearchEveryS = 0.25f;
        const float ListenerSearchSlowEveryS = 1f;
        const float ListenerSearchWarnDefaultS = 10f;
        internal static float ListenerSearchWarnS = ListenerSearchWarnDefaultS;   // a test seam: the wait, shortened
        float _listenerSearchNext, _listenerSearchSince = -1f;
        bool _listenerSearchWarned, _listenerProviderThrew;

        bool ListenerExpected => ListenerProvider != null || !string.IsNullOrEmpty(listenerName);

        /// <summary>How many times the late-bound listener search has run. It stops rising once
        /// <see cref="listener"/> is bound. A diagnostic.</summary>
        public int ListenerSearches { get; private set; }

        void ResolveListener(bool force = false)
        {
            if (listener) { _listenerSearchSince = -1f; return; }
            if (!ListenerExpected) return;
            float now = Time.realtimeSinceStartup;
            if (_listenerSearchSince < 0f) { _listenerSearchSince = now; _listenerSearchWarned = false; force = true; }
            if (!force && now < _listenerSearchNext) return;
            _listenerSearchNext = now + (_listenerSearchWarned ? ListenerSearchSlowEveryS : ListenerSearchEveryS);
            ListenerSearches++;

            Transform found = null;
            if (ListenerProvider != null)
            {
                try { found = ListenerProvider(); }
                catch (Exception ex)
                {
                    if (!_listenerProviderThrew) { _listenerProviderThrew = true; Debug.LogException(ex, this); }
                }
            }
            if (!found && !string.IsNullOrEmpty(listenerName))
            {
                // Active objects only: an inactive head has no pose to follow. A '/' in the name is
                // a hierarchy path to Find, which is how two heads with one name are told apart.
                var go = GameObject.Find(listenerName);
                if (go) found = go.transform;
            }
            if (found)
            {
                listener = found;
                if (_fallbackTaken && !feedListener) _fallbackListener = found;   // it replaces the camera stand-in
                _listenerSearchSince = -1f;
                _listenerSeen = _listenerArmed = false;      // a new head gets its own placement grace
                // Not during BringUp: its own registration line follows and already names the head.
                if (Decided) LogRegistration();
                return;
            }
            if (!_listenerSearchWarned && now - _listenerSearchSince > ListenerSearchWarnS)
            {
                _listenerSearchWarned = true;
                Debug.LogWarning("[bw_audio] no listener after " + ListenerSearchWarnS.ToString("F0", System.Globalization.CultureInfo.InvariantCulture) + " s: " +
                                 (ListenerProvider != null ? "Engine.ListenerProvider returns null" : "") +
                                 (ListenerProvider != null && !string.IsNullOrEmpty(listenerName) ? " and " : "") +
                                 (!string.IsNullOrEmpty(listenerName)
                                      ? "no active GameObject is named '" + listenerName + "'" : "") +
                                 ". The listener stays at the array centroid. Still looking, once a " +
                                 "second.", this);
            }
        }

        // One line that makes a wrong Room Origin visible in the log: where the room frame sits in the
        // scene, and where that puts the head. Compare the second against where the head really is.
        // Printed once Start has decided, on every node, and each time a late-bound listener binds.
        void LogRegistration()
        {
            // Invariant: a log line somebody compares against a layout file must not print "1,448".
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            string V(Vector3 v) => "(" + v.x.ToString("F3", inv) + ", " + v.y.ToString("F3", inv) + ", " +
                                   v.z.ToString("F3", inv) + ")";
            var o = roomOrigin;
            var head = feedListener ? listener : _fallbackListener;
            string origin = o ? "Room Origin '" + o.name + "' at world " + V(o.position) + ", yaw " +
                                o.eulerAngles.y.ToString("F1", inv) + " deg"
                              : "no Room Origin (the room frame is Unity world, x mirrored)";
            string where = !Ready ? "no listener is fed on this node"
                         : head ? "listener '" + head.name + "' at room " + V(Room.Pos(head.position))
                         : FeedsListener ? "no listener Transform yet" : "listener from the tracker";
            Debug.Log("[bw_audio] registration: " + origin + "; " + where + ".", this);
        }

        // Calls a component made between Awake and Start that need the native engine (Emitter.Play
        // from a script's Awake or OnEnable). They used to land, when Awake brought the engine up, so
        // they are held and run once Start has it. Dropped on a node that does not own audio or whose
        // start failed, where the same call is a no-op at any time.
        readonly List<(MonoBehaviour owner, Action call)> _deferred = new();

        /// <summary>Hold `call` until the Engine's Start has brought the native engine up. False when
        /// there is nothing to hold it for: no Engine, one that has already decided, or one whose
        /// Start is not about to run. An Engine left disabled can wait minutes for its enable, and
        /// calls held that long would all replay in one frame, so there they stay the no-ops they are
        /// with no Engine at all.</summary>
        internal static bool DeferToStart(MonoBehaviour owner, Action call)
        {
            var e = Instance;
            // Not isActiveAndEnabled, which is still false while the scene's Awakes are running.
            if (!e || e.Decided || !e.enabled || !e.gameObject.activeInHierarchy) return false;
            e._deferred.Add((owner, call));
            return true;
        }

        void RunDeferred()
        {
            try
            {
                for (int i = 0; i < _deferred.Count && Ready; i++)   // a call may destroy the Engine
                {
                    var (owner, call) = _deferred[i];
                    if (!owner || !owner.isActiveAndEnabled) continue;
                    // One caller's throw must not cost the others their play.
                    try { call(); }
                    catch (Exception ex) { Debug.LogException(ex, owner); }
                }
            }
            finally { _deferred.Clear(); }
        }

        // An Engine call that needs the native engine, made between Awake and Start: a script's Awake
        // or OnEnable in the Engine's own scene, where the engine used to be up. The call is lost,
        // so say so, once per Engine. Not after Start: a node that does not own audio, or a failed
        // start, is silent by design.
        bool _earlyCallWarned;
        bool ReadyFor(string call)
        {
            if (Ready) return true;
            if (!Decided && !_earlyCallWarned)
            {
                _earlyCallWarned = true;
                Debug.LogWarning("[bw_audio] Engine." + call + " was called before the Engine's Start, so " +
                                 "it did nothing: the native engine comes up in Engine.Start. Call it from " +
                                 "Start or later, or call Engine.StartNow() first. (Warns once.)", this);
            }
            return false;
        }

        bool AskOwnsAudio()
        {
            if (OwnsAudioPredicate == null) return true;
            try { return OwnsAudioPredicate(); }
            catch (Exception ex)
            {
                Debug.LogError("[bw_audio] Engine.OwnsAudioPredicate threw, so this process does not own " +
                               "audio: " + ex.GetType().Name + ": " + ex.Message, this);
                return false;
            }
        }

        // Everything the engine wants set BEFORE bwa_start (the reverb beds — the bed decoder and pathing
        // ride BwaDesc into bwa_create), plus the live knobs — pushed here too so the scene STARTS in the
        // state the inspector shows, not at the engine's defaults. ApplyLiveSettings is the same set minus
        // the load-time-only ones.
        void ApplyLoadTimeSettings()
        {
            if (enableReflections && enableFdnReverb)
                Debug.LogWarning("[bw_audio] both reverb beds enabled, and they share ONE reverb tap. Using the " +
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
            Bwa.bwa_set_dual_band_cap(_eng, dualBandCap);
            Bwa.bwa_set_spcap_focus(_eng, spcapFocus, spcapDensity);
            Bwa.bwa_set_spread_mode(_eng, spreadMode);
            Bwa.bwa_set_decorrelation(_eng, decorrelation);
            Bwa.bwa_set_near_spread(_eng, nearSpreadRadius);
            Bwa.bwa_set_hole_spread(_eng, holeSpread);
            Bwa.bwa_set_speed_of_sound(_eng, speedOfSound);
            Bwa.bwa_set_bed_renderer(_eng, bedRenderer);
            Bwa.bwa_set_max_re(_eng, maxRe);
            Bwa.bwa_set_max_re_split(_eng, maxReSplit);
            Bwa.bwa_set_tracked_room_eq(_eng, trackedRoomEq);
            Bwa.bwa_set_tracked_directivity(_eng, trackedDirectivity);
            Bwa.bwa_set_tracked_align_guards(_eng, trackedAlignDeadZone, trackedAlignSlewFramesPerSecond);
            Bwa.bwa_set_tracked_align(_eng, trackedAlign);
            Bwa.bwa_set_master_gain(_eng, masterGain);
            Bwa.bwa_set_reverb_gain(_eng, reverbGain);   // valid pre-start: seeds whichever reverb bed bwa_start creates
            Bwa.bwa_set_early_reflections_gain(_eng, earlyReflectionGain);
            Bwa.bwa_set_limiter(_eng, limiter);
            Bwa.bwa_set_limiter_ceiling(_eng, limiterCeiling);
            Bwa.bwa_set_pose_prediction(_eng, FeedsListener ? 0f : posePredictionS);   // internal tracking only
        }

        // ---- situation tuning (seated / roaming) ----------------------------------------------------
        //
        // A preset seeds THE INSPECTOR FIELDS above, then re-pushes them through ApplyLiveSettings. It
        // deliberately does NOT call bwa_apply_tuning directly: the fields are this component's single
        // source of truth, and an engine configured behind their back would show stale values and get
        // silently reverted by the next OnValidate. Seeding them keeps one answer to "what is set".
        //
        // For an A/B, call this from a UI button and listen. Everything a preset touches is a live knob,
        // so switching situations mid-session is audible immediately and needs no restart.

        [Header("Situation tuning")]
        [Tooltip("A starting point for the live knobs above: Seated optimizes one listening position, " +
                 "Roaming optimizes a moving listener. Applying a preset OVERWRITES those fields.")]
        public Bwa.BwaSetup situation = Bwa.BwaSetup.Default;

        /// <summary>
        /// Overwrite the live knobs from `situation`'s preset and push them. Safe before or during play.
        /// Returns false only if the preset could not be read.
        /// </summary>
        public bool ApplySituation() => ApplySituation(situation);

        /// <summary>Overwrite the live knobs from a named preset and push them.</summary>
        public bool ApplySituation(Bwa.BwaSetup setup)
        {
            Bwa.bwa_tuning_preset(setup, out var t);
            if (t.structSize == 0) return false;      // the ABI never returns a zero structSize on success
            situation = setup;
            AdoptTuning(t);
            if (Ready) ApplyLiveSettings();
            return true;
        }

        /// <summary>Copy a tuning struct into the inspector fields. Does not push; ApplySituation does.</summary>
        public void AdoptTuning(Bwa.BwaTuning t)
        {
            panner        = t.panner;
            spcapFocus    = t.spcapFocus;
            spcapDensity  = t.spcapDensity;
            dualBand      = t.dualBand;
            dualBandCap   = t.dualBandCap;
            spreadMode    = t.spreadMode;
            decorrelation = t.decorrelation;
            nearSpreadRadius = t.nearSpread;
            holeSpread    = t.holeSpread;
            maxRe         = t.maxRe;
            maxReSplit    = t.maxReSplit;
            bedRenderer   = t.bedRenderer;
            trackedRoomEq = t.trackedRoomEq;
            trackedDirectivity = t.trackedDirectivity;
            trackedAlign  = t.trackedAlign;
            trackedAlignDeadZone            = t.alignDeadZoneM;
            trackedAlignSlewFramesPerSecond = t.alignSlewFramesPerSecond;
        }

        /// <summary>
        /// What the ENGINE currently has, read back rather than assumed. Use it to record what a rig-day
        /// A/B actually ran with, instead of trusting that the inspector and the engine agree.
        /// </summary>
        public bool TryGetEngineTuning(out Bwa.BwaTuning t)
        {
            t = default;
            return Ready && Bwa.bwa_get_tuning(_eng, out t);
        }

        // Inspector edits take effect live in Play mode — that IS the workflow for these (the engine makes
        // the panner/dual-band/spread-mode/decorrelation/bed-renderer switches atomic so they can be A/B'd
        // by ear). Load-time fields (the reverb beds, the bed decoder, the room box) need a scene restart.
        void OnValidate()
        {
            _layoutXyzFrom = null;                                       // re-read the layout for the gizmos
            ApplyRoomOrigin();                                           // assigned, swapped or cleared in the Inspector
            if (Application.isPlaying && Ready) ApplyLiveSettings();
        }

        /// <summary>Where the engine reads a clip or layout path from. A relative path is relative to
        /// StreamingAssets. An ABSOLUTE path is used as it is: write a generated clip to
        /// <c>Application.temporaryCachePath</c> and pass its full path to any clip field or load
        /// call. That is a contract, not an accident of Path.Combine. For audio you generate
        /// continuously, use <see cref="PushEmitter"/>: there is no load from memory.</summary>
        public static string AssetPath(string clip)
            => Path.IsPathRooted(clip) ? clip : Path.Combine(Application.streamingAssetsPath, clip);

        /// <summary>Load a clip through the engine's shared asset cache. `flags` picks the loader, and the
        /// cache key is (path, flags): the same clip with the same flags always returns the same handle,
        /// and the same clip with DIFFERENT flags is a different asset (a file held in RAM and the same
        /// file streamed are two things). Returns 0 on failure.
        /// <para>Ownership: assets loaded here live for the engine's lifetime, which is what the old
        /// dictionary gave you too. Every call takes one more reference and the binding never releases,
        /// because bwa_destroy frees every cached asset whatever the refcount. Do not call
        /// Bwa.bwa_unload_sound on a handle from here (the engine refuses it): release what you acquired,
        /// unload what you loaded.</para></summary>
        public uint Acquire(string clip, BwaLoadFlags flags = BwaLoadFlags.None)
        {
            if (!ReadyFor("Load") || string.IsNullOrEmpty(clip)) return 0;
            var key = (uint)flags + "|" + clip;
            if (_loadFailed.Contains(key)) return 0;          // already logged; don't re-hit the disk
            uint s = Bwa.bwa_sound_acquire(_eng, AssetPath(clip), flags);
            if (s == 0)
            {
                _loadFailed.Add(key);
                Debug.LogWarning("[bw_audio] load failed: " + clip + " (" + Bwa.LastError(_eng) + ")");
            }
            return s;
        }

        /// <summary>Load a clip WITHOUT blocking: the handle comes back immediately and the decode runs on
        /// the engine's loader thread. Play it at once — the source binds and stays silent until the data
        /// lands, then starts from the top with nothing skipped. Two calls refuse a not-ready handle
        /// instead of holding it, so check <see cref="IsSoundReady"/> before Emitter.Queue and
        /// Emitter.PlayOneShot.
        /// <para>This is for content that appears MID-SESSION. The CAVE's normal path is load-time and
        /// synchronous, so reach for <see cref="Acquire"/> first. Same cache and same key as Acquire: a
        /// clip already resident comes back ready. A decode that FAILS never becomes ready — IsSoundReady
        /// reports the reason.</para></summary>
        public uint AcquireAsync(string clip, BwaLoadFlags flags = BwaLoadFlags.None)
        {
            if (!ReadyFor("AcquireAsync") || string.IsNullOrEmpty(clip)) return 0;
            var key = (uint)flags + "|" + clip;
            if (_loadFailed.Contains(key)) return 0;
            uint s = Bwa.bwa_sound_acquire_async(_eng, AssetPath(clip), flags);
            if (s == 0)
            {
                _loadFailed.Add(key);
                Debug.LogWarning("[bw_audio] async load failed: " + clip + " (" + Bwa.LastError(_eng) + ")");
            }
            return s;
        }

        /// <summary>Has an asynchronously loaded clip's data landed? True at once for anything from
        /// <see cref="Acquire"/>. `error` is null while a load is still in flight and carries the
        /// decoder's reason when it FAILED — a failed load never becomes ready, so that is the only way
        /// to tell the two apart.</summary>
        public bool IsSoundReady(uint snd, out string error)
        {
            error = null;
            if (!Ready || snd == 0) return false;
            if (Bwa.bwa_sound_is_ready(_eng, snd)) return true;
            error = Bwa.LastError(_eng);        // set by that call ONLY on a failed decode
            return false;
        }

        /// <summary>Poll a handle a component is waiting on and report a decode that FAILED, once.
        /// Returns the handle while the load is still in flight (keep watching) and 0 once it has
        /// resolved either way, so the caller assigns the result back over its pending field.
        /// <para><c>landed</c> separates the two ways it resolves: true means the data ARRIVED, which is
        /// also the moment the engine adopted it and bound the play that was waiting (this call is one
        /// of the pump points that does the adopting). False with a 0 return means the decode failed and
        /// the play will never bind. Callers that latch anything on "has this play bound yet" need that
        /// distinction; a bare 0 cannot give it.</para>
        /// <para>Shared by every component that opts into async loading (Emitter, AmbisonicBed), because
        /// the two silences it tells apart — "still decoding" and "the decode failed" — are the same
        /// distinction for all of them, and two copies of it would drift.</para></summary>
        internal static uint WatchPendingLoad(uint pending, MonoBehaviour owner, string subject,
                                              out bool landed)
        {
            landed = false;
            if (pending == 0 || !owner) return 0;
            var engine = Instance;
            if (!engine) return 0;
            if (engine.IsSoundReady(pending, out var error)) { landed = true; return 0; }
            if (error == null) return pending;                 // still decoding
            Debug.LogWarning("[" + owner.GetType().Name + "] async load failed on " + owner.name +
                             ", the " + subject + " stays silent: " + error);
            return 0;
        }

        // ---- the one waiting rule for components that need the native engine ------------------------
        //
        // SourceBase, AmbisonicBed and DynamicAcousticGeometry all wait the same way, so a component
        // that comes up before the Engine (init order, a runtime spawn, a scene loaded first) catches
        // up instead of switching itself off, and one on a node that does not own audio stops asking.

        /// <summary>The Engine a component may create native objects under, or null. `wait` is true
        /// while one may still come (no Engine yet, or one whose Start has not decided), and false
        /// when none ever will: a decided Engine that is not Ready does not own audio or failed to
        /// start.</summary>
        internal static Engine ForComponent(out bool wait)
        {
            var e = Instance;
            wait = !e || !e.Decided;
            return e && e.Ready ? e : null;
        }

        /// <summary>Call `attempt` once a frame until it returns true. The caller stops it in OnDisable
        /// (StopAllCoroutines): Unity stops a coroutine when the GameObject deactivates, not when the
        /// component is disabled.</summary>
        internal static IEnumerator RetryEachFrame(Func<bool> attempt)
        {
            do { yield return null; ComponentWaitFrames++; } while (!attempt());
        }

        /// <summary>Load a mono point-source asset (cached by path). Returns 0 on failure.</summary>
        public uint Load(string clip) => Acquire(clip, BwaLoadFlags.None);

        /// <summary>Load a mono point-source asset STREAMED from disk instead of held in RAM — for long
        /// music or ambience. One voice at a time, no seek, no pitch. Returns 0 on failure.</summary>
        public uint LoadStreaming(string clip) => Acquire(clip, BwaLoadFlags.Stream);

        /// <summary>Load a pre-encoded AmbiX soundfield for a world-locked bed (cached). Returns 0 on failure.</summary>
        public uint LoadAmbix(string clip) => Acquire(clip, BwaLoadFlags.Ambix);

        /// <summary>Load a legacy FuMa B-format soundfield (.amb-style: WXYZ order, MaxN, W -3 dB) for a
        /// world-locked bed (cached). Converted to AmbiX at load — past this call it IS an AmbiX asset.
        /// Full 3D sets (4/9/16 channels) and the .amb mixed-order sets (3/5/6/7/8/11). Returns 0 on failure.</summary>
        public uint LoadFuma(string clip) => Acquire(clip, BwaLoadFlags.Fuma);

        /// <summary>The handle for a clip that is ALREADY resident, or 0. Pure probe: it never loads,
        /// never touches the disk, and never takes a reference, so it cannot answer for a clip nobody
        /// asked for yet. Use it to ask "is this in memory" without the side effect of putting it there.
        /// <para>Because it takes no reference, treat the handle as borrowed: read metadata from it, do
        /// not release against it.</para></summary>
        public uint Find(string clip, BwaLoadFlags flags = BwaLoadFlags.None)
            => Ready && !string.IsNullOrEmpty(clip)
                 ? Bwa.bwa_sound_find(_eng, AssetPath(clip), flags)
                 : 0;

        // Metadata probes the cache FIRST so asking a resident clip its length costs no reference.
        // On a miss it still loads on demand, which is the documented contract.
        uint MetaHandle(string clip) { var s = Find(clip); return s != 0 ? s : Load(clip); }

        /// <summary>Length of a clip in engine-rate frames (seconds = SoundFrames / sampleRate). Loads it on
        /// demand if it is not already resident. 0 for an unknown/failed clip, or a stream of unknown length.</summary>
        public ulong SoundFrames(string clip) { var s = MetaHandle(clip); return s != 0 ? Bwa.bwa_sound_get_frames(_eng, s) : 0; }

        /// <summary>Channel count of a clip: 1 for a mono point-source asset, 4/9/16 for an ambisonic bed
        /// (order 1/2/3). Loads it on demand if not already resident. 0 for an unknown/failed clip.</summary>
        public uint SoundChannels(string clip) { var s = MetaHandle(clip); return s != 0 ? Bwa.bwa_sound_get_channels(_eng, s) : 0; }

        /// <summary>Mint one of the engine's built-in materials. Cached (minted once per preset), so
        /// calling it per scene load is safe — null-engine-safe too.</summary>
        public uint MaterialPreset(BwaMaterialPreset preset) => ReadyFor("MaterialPreset") ? ResolvePreset(preset) : 0;

        /// <summary>Reverb wet level (linear), adjustable live — the reverb-send equivalent.</summary>
        public float ReverbGain
        {
            get => reverbGain;
            set { reverbGain = value; if (Ready) Bwa.bwa_set_reverb_gain(_eng, value); }
        }

        /// <summary>Level of the image-source EARLY reflections (the per-source wall bounces, opted into
        /// with Emitter.earlyReflections). Independent of the late reverb bed's wet level above: early
        /// reflections carry room size and distance, the bed carries the tail. Live.</summary>
        public float EarlyReflectionGain
        {
            get => earlyReflectionGain;
            set { earlyReflectionGain = value; if (Ready) Bwa.bwa_set_early_reflections_gain(_eng, value); }
        }

        /// <summary>Output protection limiter (engine default: ON at -1 dBFS). Linked across the channels —
        /// engaging never shifts the spatial image. Protection against digital overs, not mastering: if it
        /// engages in normal use, turn the content down.</summary>
        public void SetLimiter(bool on) { limiter = on; if (Ready) Bwa.bwa_set_limiter(_eng, on); }
        // Clamped into (0..1] BEFORE caching: the engine silently ignores <= 0, so an unclamped 0 would
        // leave the field claiming a ceiling the engine isn't running (0.001 = the old -60 dB floor).
        public void SetLimiterCeiling(float linear) { limiterCeiling = Mathf.Clamp(linear, 0.001f, 1f); if (Ready) Bwa.bwa_set_limiter_ceiling(_eng, limiterCeiling); }

        /// <summary>Master gain over the whole mix (ramped — slider drags never zipper). The volume knob.</summary>
        public float MasterGain
        {
            get => masterGain;
            set { masterGain = value; if (Ready) Bwa.bwa_set_master_gain(_eng, value); }
        }

        /// <summary>Engine-wide speed of sound (m/s; live). Doppler and reflection delays derive from it
        /// and glide to a change — 343 air, 1480 underwater; small values exaggerate Doppler for slow
        /// motion. Engine-side clamp [30, 20000].</summary>
        public float SpeedOfSound
        {
            get => speedOfSound;
            set { speedOfSound = value; if (Ready) Bwa.bwa_set_speed_of_sound(_eng, value); }
        }

        /// <summary>Retune the FDN reverb's decay LIVE — the room-transition knob: the tail keeps
        /// ringing, only its slope changes (stepping into a cathedral, submerging, walking outside).
        /// Values &lt;= 0 keep the current setting. Needs 'Enable Fdn Reverb'; pair with ReverbGain for
        /// the wet level.</summary>
        public void FdnSetDecay(float rt60LowSeconds, float rt60HighSeconds, float crossoverHz)
        {
            if (rt60LowSeconds  > 0f) fdnRt60LowSeconds  = rt60LowSeconds;
            if (rt60HighSeconds > 0f) fdnRt60HighSeconds = rt60HighSeconds;
            if (crossoverHz     > 0f) fdnCrossoverHz     = crossoverHz;
            if (Ready) Bwa.bwa_fdn_set_decay(_eng, rt60LowSeconds, rt60HighSeconds, crossoverHz);
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
        public void SetGroupGain(uint group, float linear) { if (ReadyFor("SetGroupGain")) Bwa.bwa_group_set_gain(_eng, group, linear); }

        /// <summary>Pause a whole mix group (0..7) — same click-free freeze as per-source pause.</summary>
        public void SetGroupPaused(uint group, bool paused) { if (ReadyFor("SetGroupPaused")) Bwa.bwa_group_set_paused(_eng, group, paused); }

        /// <summary>Stop every voice in one mix group (0..7) — "kill the SFX, keep the dialog". Each voice
        /// takes the click-free stop path (a one-block fade, then end) and drops its pending Queue chain.
        /// Group gains and pause gates are untouched: a stop stops sound, it does not reset the mixer.
        /// Source handles stay valid and re-playable, so an Emitter can Play again straight after.
        /// <para>It also drops this group's plays that are still waiting on an async decode, which would
        /// otherwise start by themselves the moment their data landed. The group a still-decoding play
        /// belongs to is the one its source was in when the stop ran.</para></summary>
        public void StopGroup(uint group) { if (ReadyFor("StopGroup")) Bwa.bwa_group_stop(_eng, group); }

        /// <summary>Stop every voice in the engine, beds included — the one-call scene transition. Same
        /// click-free path and the same "does not reset the mixer" rule as <see cref="StopGroup"/>. It
        /// drops every play still waiting on an async decode, whatever group it was issued in.
        /// A PUSH voice stops like any other, but its feed ring is NOT ended — call PushEmitter.PushEnd
        /// yourself if you are done with it.</summary>
        public void StopAll() { if (ReadyFor("StopAll")) Bwa.bwa_stop_all(_eng); }

        // ---- live rendering A/B (each of these is atomic / crossfaded engine-side) --------------------
        public void SetPanner(BwaPanner p)        { panner = p;        if (Ready) Bwa.bwa_set_panner(_eng, p); }
        public void SetDualBand(bool on)         { dualBand = on;     if (Ready) Bwa.bwa_set_dual_band(_eng, on); }
        /// <summary>Compensated amplitude panning on the dual-band low band. Inert unless
        /// <see cref="SetDualBand"/> is on. Corrects the rendered interaural time difference for the
        /// tracked head orientation, so an image holds still under head rotation.</summary>
        public void SetDualBandCap(bool on)      { dualBandCap = on;  if (Ready) Bwa.bwa_set_dual_band_cap(_eng, on); }
        /// <summary>SPCAP's lobe sharpness and placement-correction density exponent (inert under DBAP/VBAP).
        /// Pass 0 or less for either to revert THAT one to its default: focus to the value derived from the
        /// array geometry, density to 2.0. Every source re-solves next block, static ones included.</summary>
        public void SetSpcapFocus(float focus, float density = 0f)
            { spcapFocus = focus; spcapDensity = density; if (Ready) Bwa.bwa_set_spcap_focus(_eng, focus, density); }
        public void SetSpreadMode(BwaSpreadMode m){ spreadMode = m;    if (Ready) Bwa.bwa_set_spread_mode(_eng, m); }
        public void SetDecorrelation(bool on)    { decorrelation = on; if (Ready) Bwa.bwa_set_decorrelation(_eng, on); }
        public void SetNearSpread(float radiusM) { nearSpreadRadius = radiusM; if (Ready) Bwa.bwa_set_near_spread(_eng, radiusM); }
        /// <summary>Floor a source's spread by how far its bearing sits from the nearest speaker, so a source
        /// aimed into an array HOLE renders as an honest wide source instead of a split image. 0 = off;
        /// 1 = the honest width. Inert on an array that surrounds the listener, and every source re-solves
        /// next block, static ones included.</summary>
        public void SetHoleSpread(float strength) { holeSpread = strength; if (Ready) Bwa.bwa_set_hole_spread(_eng, strength); }
        public void SetBedRenderer(BwaBedRenderer r) { bedRenderer = r; if (Ready) Bwa.bwa_set_bed_renderer(_eng, r); }
        public void SetMaxRe(bool on)            { maxRe = on;        if (Ready) Bwa.bwa_set_max_re(_eng, on); }
        /// <summary>Band-split max-rE (needs max-rE on): taper only above ~700 Hz, plain decode below. Live A/B.</summary>
        public void SetMaxReSplit(bool on)       { maxReSplit = on;   if (Ready) Bwa.bwa_set_max_re_split(_eng, on); }
        public void SetTrackedRoomEq(bool on)    { trackedRoomEq = on; if (Ready) Bwa.bwa_set_tracked_room_eq(_eng, on); }
        /// <summary>Follow each speaker's off-axis loss onto the tracked head (layouts with a directivity
        /// model). On by default; off glides back to the layout's own trims. No-op without a model.</summary>
        public void SetTrackedDirectivity(bool on) { trackedDirectivity = on; if (Ready) Bwa.bwa_set_tracked_directivity(_eng, on); }
        /// <summary>Re-align the array's arrival times on the TRACKED listener instead of the fixed array
        /// centroid. OFF by default: every delay change is a resampling event, so a walking listener glides
        /// all 26 delays at once. `deadZoneM` is how far the head must move before anything is recomputed
        /// (0 = the 5 cm default); `slewFramesPerS` caps how fast a delay may change, in audio frames per
        /// second (0 = the default, about 63 at 48 kHz, which follows a 0.45 m/s walk). A faster listener
        /// LAGS rather than pitch-shifting. Off glides back to the layout's own trims.
        /// The enable and its guards are separate calls, so A/B-ing the toggle never disturbs a dialed
        /// guard. Order does not matter and the guards are live.</summary>
        public void SetTrackedAlign(bool on)
            { trackedAlign = on; if (Ready) Bwa.bwa_set_tracked_align(_eng, on); }
        /// <summary>The two guards, live. 0 on either takes that one's default.</summary>
        public void SetTrackedAlignGuards(float deadZoneM, float slewFramesPerSecond)
            { trackedAlignDeadZone = deadZoneM; trackedAlignSlewFramesPerSecond = slewFramesPerSecond;
              if (Ready) Bwa.bwa_set_tracked_align_guards(_eng, deadZoneM, slewFramesPerSecond); }
        /// <summary>Lead the TRACKED pose by `seconds` to hide motion-to-ears latency. Internal tracking only
        /// (Feed Listener off) — when Unity feeds the pose, predict on the Unity side instead.</summary>
        public void SetPosePrediction(float seconds)  { posePredictionS = seconds; if (Ready && !FeedsListener) Bwa.bwa_set_pose_prediction(_eng, seconds); }

        // ---- readback (per-frame-safe: no locks, no allocation in the engine) -------------------------
        /// <summary>The engine's ACTIVE channel count — the layout's speaker count (4..64), NOT a constant.
        /// Size any meter / speaker-gizmo / channel-test array with this; never hard-code 26.</summary>
        public uint ChannelCount => _channels;
        uint _channels;

        /// <summary>Last block's peak |sample| per channel of the SPEAKER BUS (linear), after the trims,
        /// the test signal, and the limiter. Drives channel meters / a speaker-activity display. The
        /// array is reused; it is ChannelCount long.
        /// <para>What that is depends on the profile. Cave and CaveBoth: exactly what each device
        /// channel received. CaveSim: the virtual speakers, before the HRTF decode that makes the
        /// stereo the headphones receive. Binaural: point sources and beds bypass the bus (they are
        /// rendered straight to the HRTF), so it carries only the reverb and diffuse taps and reads
        /// ZERO while a dry source or a bed is plainly audible. Nothing here meters the headphone
        /// stereo. For "is anything playing" use ActiveVoices or IsPlaying, not these levels.</para></summary>
        public float[] BusLevels()
        {
            if (!Ready) return Array.Empty<float>();
            if (_levels == null || _levels.Length != _channels) _levels = new float[_channels];
            Bwa.bwa_get_bus_levels(_eng, _levels, (uint)_levels.Length);
            return _levels;
        }
        float[] _levels;

        /// <summary>Speaker positions in ROOM space (3 floats each, in channel order) — the geometry the
        /// engine is actually panning with (the loaded layout, or the default grid when Layout File is
        /// empty). The
        /// layout is fixed for the engine's lifetime, so this is read once and cached; the array is
        /// reused, so don't mutate it.</summary>
        public float[] SpeakerPositions()
        {
            // No engine (a node that does not own audio, or a failed start): the layout FILE, as the
            // stopped editor's gizmos draw it, so every render node can still show the array. Empty
            // for the default grid, which has no file.
            if (!Ready) return LayoutFileSpeakers() ?? Array.Empty<float>();
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

        /// <summary>Liveness of the internal tracker (OptiTrack/NatNet) — for a HUD indicator or a dropout
        /// alarm. A successful connect only means the socket opened; this says whether frames are actually
        /// arriving and whether the followed body has a current pose. NoData ⇒ check the stream/network/
        /// port; NoBody ⇒ check the rigid body id/name, or the head is occluded. Disconnected when no
        /// tracker is connected (or the engine isn't running). Cheap; safe to poll every frame.</summary>
        public BwaTrackerState TrackerStatus => Ready ? Bwa.bwa_tracker_status(_eng) : BwaTrackerState.Disconnected;

        /// <summary>The engine's dsp-sample clock in FRAMES (device-anchored, monotonic). Add to it to
        /// schedule a sample-accurate start: <c>DspTimeFrames + sampleRate/2</c> plays half a second out.
        /// <para>Named for its unit, and this is the sharpest name collision in the binding: Unity's own
        /// <c>AudioSettings.dspTime</c> is a <c>double</c> of SECONDS. A bare <c>DspTime</c> here returning
        /// a frame count reads as that call to every Unity developer, and the two are wrong by a factor of
        /// the sample rate — which looks like a plausible-but-early cue rather than an obvious bug.</para>
        /// Schedule with this one. It is exact, and it is what PlayAt and StopAt take.</summary>
        public ulong DspTimeFrames => Ready ? Bwa.bwa_get_dsp_time_frames(_eng) : 0;

        /// <summary>The seconds twin of <see cref="DspTimeFrames"/> (converted at the engine's sample
        /// rate) — the like-for-like comparison against <c>AudioSettings.dspTime</c>. Both count from the
        /// engine starting, not from the app, so only DIFFERENCES are comparable across the two clocks.
        /// A convenience for display and logging: schedule with DspTimeFrames.</summary>
        public double DspTimeSeconds
        {
            get { double fs = sampleRate; return fs > 0 ? DspTimeFrames / fs : 0.0; }
        }

        /// <summary>The device's own (dsp sample ↔ host time) correspondence for the last rendered
        /// block — the raw pair behind DspTimeFramesAt/RealtimeAt, for callers who want to run their own
        /// clock model. hostTimeNs is monotonic nanoseconds on a backend-defined epoch. False until
        /// audio is running with a host-stamped backend.</summary>
        public bool GetClock(out ulong dspSample, out ulong hostTimeNs)
        {
            dspSample = 0; hostTimeNs = 0;
            return Ready && Bwa.bwa_get_clock(_eng, out dspSample, out hostTimeNs);
        }

        /// <summary>Device-reported render→DAC output latency, FRAMES at the engine rate (the Digiface
        /// includes its Dante network buffering). A sound scheduled for dsp time T is HEARD at
        /// T + OutputLatencyFrames — fold it into AV alignment together with your measured display delay.
        /// 0 = unknown / no physical output (the silent null-sink fallback), which is also why the unit
        /// belongs in the name: a device-less sink reports 0, and 0 is 0 in either unit.</summary>
        public uint OutputLatencyFrames => Ready ? Bwa.bwa_get_output_latency_frames(_eng) : 0;

        /// <summary>The seconds twin of <see cref="OutputLatencyFrames"/> (converted at the engine's
        /// sample rate) — the unit AV-alignment arithmetic usually wants, since a measured display delay
        /// is in seconds too.</summary>
        public double OutputLatencySeconds
        {
            get { double fs = sampleRate; return fs > 0 ? OutputLatencyFrames / fs : 0.0; }
        }

        /// <summary>How fast the device clock runs against the host clock, fitted over the per-block
        /// stamps (~2 min window). DspTimeFramesAt re-anchors every frame and needs none of this; reach for
        /// it when something ELSE owns the timeline (video, timecode, another node), when you want a
        /// minutes-long extrapolation to hold, or to log the rig's drift. False until the fit has ~1 s
        /// of stamps, and again for ~1 s after a restart re-bases the device sample position.</summary>
        public bool GetClockModel(out BwaClockModel model)
        {
            model = default;
            return Ready && Bwa.bwa_get_clock_model(_eng, out model);
        }

        /// <summary>Device health: was the callback starved, and by whom. Returns whether the numbers
        /// MEAN anything — false when this configuration cannot observe a dropout at all (not started,
        /// or a driver that never flags a valid sample position), in which case a 0 xrun count is
        /// indistinguishable from "never measured". Check it once at startup on an unfamiliar
        /// driver, then poll Xruns for a HUD.</summary>
        public bool GetHealth(out BwaHealth health)
        {
            health = default;
            return Ready && Bwa.bwa_get_health(_eng, out health);
        }

        /// <summary>Device dropouts since start — the one-line form, for a HUD or a log line. 0 both
        /// when nothing dropped and when nothing could be measured; GetHealth tells them apart.</summary>
        public ulong Xruns => Ready ? Bwa.bwa_get_xruns(_eng) : 0;

        /// <summary>Map a Time.realtimeSinceStartupAsDouble moment (SECONDS) to the dsp-sample clock
        /// (FRAMES) — THE way to land a sound on a visual event:
        /// <c>emitter.PlayAt(engine.DspTimeFramesAt(tEvent))</c> (schedule with margin; a start in the
        /// past plays immediately). Built on the device's own block stamps (GetClock) with a
        /// continuously refreshed epoch offset, so it self-corrects device↔OS clock drift; typically
        /// accurate to well under a millisecond, falling back to block-granular DspTimeFrames pairing
        /// (~one block) when no device stamp exists. For events that live on the AUDIO timeline (cues in
        /// a scheduled track), skip wall time entirely — use t0 + cue×rate.
        /// <para>The name says the RETURN unit, like DspTimeFrames: this call crosses the two units, so
        /// leaving it unmarked would be the one place the collision actually bites.</para></summary>
        public ulong DspTimeFramesAt(double realtime)
        {
            RefreshClock();
            double fs = sampleRate;
            if (!_clkValid)
                return Advance(DspTimeFrames, (realtime - Time.realtimeSinceStartupAsDouble) * fs);
            return Advance(_clkSample, (realtime + _clkOffset - _clkHostSec) * fs);
        }

        // The one place a (pair sample, frames ahead of it) becomes a dsp sample, for DspTimeFramesAt
        // and the host-time helpers alike: round to nearest, and clamp at sample 0. Two helpers that
        // rounded differently would name different samples for the same instant.
        static ulong Advance(ulong pairDspSample, double aheadFrames)
        {
            if (!(Math.Abs(aheadFrames) < 9e18)) return pairDspSample;        // NaN or absurd: no move
            if (aheadFrames >= 0.0) return pairDspSample + (ulong)(aheadFrames + 0.5);
            ulong back = (ulong)(-aheadFrames + 0.5);
            return back < pairDspSample ? pairDspSample - back : 0;
        }

        /// <summary>The longest display latency the host-time helpers accept, in seconds. A larger
        /// value is a unit error (milliseconds typed as seconds), and is clamped to this.</summary>
        public const double MaxDisplayLatencySeconds = 10.0;

        /// <summary>The dsp sample to hand to PlayAt so that a sound is HEARD when a video frame is
        /// SEEN, given the HOST time of that frame's buffer swap. Pure arithmetic, so the rule lives
        /// in one place:
        /// <c>pairDspSample + (swapHostTimeNs + displayLatency - pairHostTimeNs) * rate - outputLatency</c>.
        /// (<paramref name="pairDspSample"/>, <paramref name="pairHostTimeNs"/>) is a
        /// <see cref="GetClock"/> pair. The display latency moves the target LATER (light leaves the
        /// display after the swap); the output latency moves the start EARLIER (a sample scheduled
        /// for T is heard at T + latency). Rounded to the nearest sample; a result before sample 0
        /// clamps to 0.
        /// <para>The arithmetic is exact. The answer is as good as the pair and the rate you pass:
        /// a block stamp carries the driver's stamp jitter (<see cref="GetClockModel"/>'s
        /// <c>jitter_ns</c>), and a nominal rate drifts from the device's by its ppm.</para>
        /// <para>A display latency that is negative or not a number counts as 0, and one above
        /// <see cref="MaxDisplayLatencySeconds"/> as that. A rate that is not a positive finite
        /// number returns 0: there is nothing to schedule against.</para></summary>
        public static ulong DspTimeFramesForHostTime(ulong pairDspSample, ulong pairHostTimeNs, double rateHz,
                                                     ulong swapHostTimeNs, double displayLatencySeconds,
                                                     uint outputLatencyFrames)
        {
            // Written as negations so NaN fails each test: Math.Max(0, NaN) is NaN, and a NaN cast
            // to ulong is an unspecified sample number that PlayAt would take.
            if (!(rateHz > 0.0) || double.IsInfinity(rateHz)) return 0;
            if (!(displayLatencySeconds >= 0.0)) displayLatencySeconds = 0.0;
            if (displayLatencySeconds > MaxDisplayLatencySeconds) displayLatencySeconds = MaxDisplayLatencySeconds;
            // The stamps are subtracted as integers, so the span is exact at any uptime, and the
            // sign is taken by hand because the target may precede the pair.
            double aheadNs = swapHostTimeNs >= pairHostTimeNs ? (double)(swapHostTimeNs - pairHostTimeNs)
                                                            : -(double)(pairHostTimeNs - swapHostTimeNs);
            return Advance(pairDspSample, (aheadNs * 1e-9 + displayLatencySeconds) * rateHz - outputLatencyFrames);
        }

        /// <summary>The cluster onset helper: the dsp sample at which to start a sound so that it is
        /// heard when the frame swapped at <paramref name="swapHostTimeNs"/> is seen. Host time is
        /// <see cref="HostTimeNs"/>'s clock (QPC on Windows), in nanoseconds. It adds
        /// <see cref="displayLatencySeconds"/> and subtracts <see cref="OutputLatencyFrames"/>.
        /// <para>It is the same mapping as <see cref="DspTimeFramesAt"/>: the same block pair, read
        /// by the same refresh, and the same rounding, so the two name the same sample for the same
        /// instant. What DspTimeFramesAt adds is its estimate of the offset between Unity's clock
        /// and the host clock, which a host time does not need. Neither smooths the block stamps:
        /// the result is as good as the engine's clock stamps, and
        /// <see cref="GetClockModel"/>'s <c>jitter_ns</c> says how good that is on this driver.
        /// The rate is the nominal one. For a target minutes away, use GetClockModel's fitted rate
        /// with <see cref="DspTimeFramesForHostTime"/>.</para>
        /// <para>False, with nothing to schedule, until the engine has a host-stamped block
        /// (<see cref="GetClock"/>): before Start, for the first blocks after it, and always on a
        /// node that does not own audio. The manual sink's stamps are SYNTHESIZED from the sample
        /// position, so once it is pumped this returns true and the mapping means nothing against
        /// a real host time: it is for arithmetic tests only.</para>
        /// <para>It does not refuse a time already past. PlayAt starts such a sound at once, which
        /// is late: compare the result with <see cref="DspTimeFrames"/> when the margin matters.</para></summary>
        public bool TryDspTimeFramesForHostTime(ulong swapHostTimeNs, out ulong startSample)
        {
            startSample = 0;
            RefreshClock();
            double fs = sampleRate;
            if (!_clkValid || !(fs > 0.0)) return false;
            uint lat = OutputLatencyCheck != null ? OutputLatencyCheck() : OutputLatencyFrames;
            startSample = DspTimeFramesForHostTime(_clkSample, _clkHostNs, fs, swapHostTimeNs,
                                                   displayLatencySeconds, lat);
            return true;
        }
        // A test seam, null in use: the offline sink has no device and reports a latency of 0, and 0
        // would leave the subtraction above unexercised.
        internal static Func<uint> OutputLatencyCheck;

        /// <summary>Inverse of DspTimeFramesAt: the Time.realtimeSinceStartupAsDouble moment at which a
        /// dsp sample is RENDERED (add OutputLatencySeconds for when it is heard) — for firing visuals
        /// off an audio-timeline event. No unit in the name: `Realtime` is Unity's own word for that
        /// clock and it is seconds there too, and the parameter carries its unit (`dspSample`).</summary>
        public double RealtimeAt(ulong dspSample)
        {
            RefreshClock();
            double fs = sampleRate;
            if (!_clkValid)
                return Time.realtimeSinceStartupAsDouble + ((double)dspSample - (double)DspTimeFrames) / fs;
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
            _clkSample = cs; _clkSampleD = cs; _clkHostNs = ct; _clkHostSec = hostSec; _clkValid = true;
        }
        bool _clkValid; ulong _clkSample, _clkHostNs; double _clkSampleD, _clkHostSec, _clkOffset;

        /// <summary>Drive ONE raw output channel with a test tone — a speaker-check / wiring tool, injected
        /// after the per-speaker trims. NOT a spatial path (it bypasses the panner). gain 0 or Off silences.</summary>
        public void TestSignal(uint channel, BwaTestKind kind, float gain) { if (ReadyFor("TestSignal")) Bwa.bwa_set_test_signal(_eng, channel, kind, gain); }

        // ---- device enumeration (engine-free: no handle, safe before the Engine exists) --------------
        /// <summary>Number of devices `backend` can offer — engine-free, so it works before an Engine
        /// exists (populate a picker for `device`). Reads the OS list fresh each call. Auto, Null and
        /// Manual report 0, and so does a backend this build does not carry.</summary>
        public static uint DeviceCount(BwaSinkType backend) => Bwa.bwa_get_device_count(backend);

        /// <summary>The engine's host clock right now, NANOSECONDS on the same backend-defined epoch
        /// GetClock's hostTimeNs uses (QPC on Windows, CLOCK_MONOTONIC on Linux and Android,
        /// mach_absolute_time on macOS). Engine-free, so it answers before an Engine exists.
        /// <para>Sandwich it between two reads of your own clock to MEASURE the epoch offset rather
        /// than estimate it: <c>a = Time.realtimeSinceStartupAsDouble; h = Engine.HostTimeNs;
        /// b = Time.realtimeSinceStartupAsDouble;</c> gives <c>offset = h*1e-9 - (a+b)/2</c> with the
        /// error bounded by <c>(b-a)/2</c> and no convergence period. DspTimeFramesAt's decaying-max
        /// estimator over block stamps stays the default and needs no such read; this is for a caller
        /// that wants a bounded, one-shot anchor, or that has its own timeline to reconcile.</para></summary>
        public static ulong HostTimeNs => Bwa.bwa_host_time_ns();

        /// <summary>Device `index`'s friendly name on `backend`, or null when out of range.</summary>
        public static string DeviceName(BwaSinkType backend, uint index) => Bwa.DeviceName(backend, index);

        /// <summary>Device `index`'s STABLE id on `backend` — persist this rather than the friendly
        /// name, which collides between a headset and its dock and changes when a driver updates.</summary>
        public static string DeviceId(BwaSinkType backend, uint index) => Bwa.DeviceId(backend, index);

        /// <summary>Every device `backend` offers, in enumeration order — the one call a picker actually
        /// wants. The strings are exactly what `device` accepts (empty picks the backend's default).</summary>
        public static string[] Devices(BwaSinkType backend)
        {
            uint n = DeviceCount(backend);
            var names = new string[n];
            for (uint i = 0; i < n; i++) names[i] = DeviceName(backend, i);
            return names;
        }

        /// <summary>Number of ASIO drivers registered on this machine — DeviceCount(BwaSinkType.Asio).</summary>
        public static uint AsioDriverCount => Bwa.bwa_get_asio_driver_count();

        /// <summary>Registered ASIO driver `index`'s name (the exact string `asioDriver` expects), or null if
        /// out of range. Engine-free — pair with AsioDriverCount to list the installed drivers.</summary>
        public static string AsioDriverName(uint index) => Bwa.AsioDriverName(index);

        /// <summary>Every installed ASIO driver's name, in enumeration order — the one call a device picker
        /// actually wants, instead of an index loop over AsioDriverCount/AsioDriverName. Engine-free; the
        /// strings are exactly what `asioDriver` accepts (empty picks the first usable device).</summary>
        public static string[] AsioDrivers
        {
            get
            {
                uint n = AsioDriverCount;
                var names = new string[n];
                for (uint i = 0; i < n; i++) names[i] = AsioDriverName(i);
                return names;
            }
        }

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
            if (!ReadyFor("AddDynamicMesh") || mesh == null || t == null) return -1;
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
            t = Bwa.bwa_material_preset(_eng, p); _presetCache[p] = t; return t;
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

        /// <summary>The outdoor degenerate of the room box: ONE horizontal mirror plane at the given
        /// Unity-world height — the ground bounce, the dominant early reflection when there is no room.
        /// REPLACES the room box (one room at a time), so use it with 'Enable Room Box' off. Live-safe:
        /// the room publishes to the audio thread lock-free and reflections re-solve next block, so
        /// call it from Start or mid-game (a scene transition). Set pressureRelease
        /// when the "ground" is a water surface and the listener is under it — the reflection inverts
        /// (the Lloyd's-mirror comb that makes near-surface sources sound thin).</summary>
        public void SceneSetGround(float worldY, MaterialAsset material = null, bool pressureRelease = false)
        {
            if (!ReadyFor("SceneSetGround")) return;
            // Kept so a moved Room Origin can push the plane again (OnRegistrationChanged).
            _groundSet = true; _groundWorldY = worldY; _groundMaterial = material; _groundPressureRelease = pressureRelease;
            // The origin's HEIGHT only: a level plane has no single room height under a tilted registration.
            Bwa.bwa_scene_set_ground(_eng, Room.Height(worldY), ResolveMaterial(material), pressureRelease);
        }
        bool _groundSet, _groundPressureRelease; float _groundWorldY; MaterialAsset _groundMaterial;

        /// <summary>Flag room-box faces whose image-source reflection inverts (pressure-release: the
        /// physics of reflecting off a much softer medium). Bit f = face f in the box order
        /// -x,+x,-y,+y,-z,+z; the flagship case is an underwater room whose ceiling is the surface:
        /// 1u &lt;&lt; 3. Image-source only (occlusion and reverb keep the face's material); call after
        /// the box/ground exists; live-safe like the room calls, and a later box/ground call resets
        /// every face.</summary>
        public void SceneSetPressureRelease(uint faceMask)
        {
            if (ReadyFor("SceneSetPressureRelease")) Bwa.bwa_scene_set_pressure_release(_eng, faceMask);
        }

        // Load-time (Start) scene bake. The box-ONLY case uses the engine's own box helper, which also
        // seeds the ISM early-reflection room. The ABI takes it live too, but Engine only calls it here,
        // so it stays on this path. Any acoustic geometry goes through RebuildSceneMesh, which is
        // runtime-safe and shared with the live re-bake below.
        void SetupScene()
        {
            var geos = CollectStaticGeometry();
            if (geos.Count == 0 && enableRoomBox)
            {
                uint mb = ResolvePreset(roomMaterial);
                var faces = new[] { mb, mb, mb, mb, mb, mb };
                Bwa.bwa_scene_set_box(_eng, roomSizeMeters.x, roomSizeMeters.y, roomSizeMeters.z, faces);
                _hasStaticMesh = true;
                return;
            }
            RebuildSceneMesh(geos);
        }

        // Every AcousticGeometry that should occlude right now: on an active object, component enabled.
        // Not isActiveAndEnabled, which stays false until a component's OnEnable has run. Also records
        // what this bake holds, which is what StaticGeometryChanged compares against, and takes any
        // rebuild already asked for: geometry that enabled before Start's bake marked one.
        List<AcousticGeometry> CollectStaticGeometry()
        {
            _rebakeStatic = false;
            _bakedGeometry.Clear();
            var live = new List<AcousticGeometry>();
            foreach (var g in FindObjectsByType<AcousticGeometry>(FindObjectsSortMode.None))
                if (g.enabled && g.gameObject.activeInHierarchy) { live.Add(g); _bakedGeometry.Add(g); }
            return live;
        }
        readonly HashSet<AcousticGeometry> _bakedGeometry = new();   // what the engine's static mesh holds

        /// <summary>An AcousticGeometry was enabled or disabled (spawned, toggled, or its scene loaded or
        /// unloaded). Marks the static mesh for one rebuild in this frame's LateUpdate, however many
        /// objects change. An enable the last bake already holds is skipped; a disable and re-enable
        /// in one frame still rebuilds. Before Start's bake it only marks, and that bake collects
        /// everything enabled by then.</summary>
        internal void StaticGeometryChanged(AcousticGeometry g, bool enabled)
        {
            if (_bakedGeometry.Contains(g) != enabled) _rebakeStatic = true;
        }

        // Collect the static geometry (+ the optional box, baked as geometry) into ONE mesh and push it.
        // Uses bwa_scene_set_mesh_mat only: runtime-safe, and it never touches bwa_scene_set_box's ISM
        // path. The control-thread cost is this managed copy (each mesh's vertices and triangles, and
        // the three arrays handed over) plus the engine's own copy; the BVH rebuild runs on the
        // engine's sim thread. Rare by design: at load, on a geometry enable or disable, and on a
        // moved Room Origin.
        void RebuildSceneMesh(List<AcousticGeometry> geos)
        {
            var verts = new List<float>(); var tris = new List<int>(); var triMat = new List<uint>();
            if (enableRoomBox)
                AddBox(verts, tris, triMat, roomSizeMeters, ResolvePreset(roomMaterial));
            foreach (var g in geos)
            {
                var mesh = g.ResolveMesh();
                if (mesh == null) { Debug.LogWarning("[bw_audio] AcousticGeometry with no mesh: " + g.name); continue; }
                AddMesh(verts, tris, triMat, mesh, g.transform.localToWorldMatrix, ResolveMaterial(g.material));
            }
            _staticHasGeometry = geos.Count > 0;
            StaticRebuilds++;
            if (tris.Count == 0) { ClearStaticMesh(); return; }
            Bwa.bwa_scene_set_mesh_mat(_eng, verts.ToArray(), verts.Count / 3, tris.ToArray(), tris.Count / 3, triMat.ToArray());
            _hasStaticMesh = true;
            // Once, at the load-time bake: a per-rebuild line (with its stack trace) is noise under churn.
            if (StaticRebuilds == 1) Debug.Log($"[bw_audio] acoustic scene: {verts.Count / 3} verts, {tris.Count / 3} tris");
        }

        /// <summary>How many times the static acoustic mesh has been rebuilt (the load-time bake
        /// included). A diagnostic for geometry churn: it should move on a door, not every frame.</summary>
        public int StaticRebuilds { get; private set; }

        // Drop the static geometry when none is left. Only when a mesh is actually up, so an empty
        // install pushes nothing. NULL arrays with zero counts are the ABI's documented clear (an
        // engine older than this binding refused it, so pair the binding with its own engine build).
        void ClearStaticMesh()
        {
            if (!_hasStaticMesh) return;
            Bwa.bwa_scene_set_mesh_mat(_eng, null, 0, null, 0, null);
            _hasStaticMesh = false;
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

        // Append a FLOOR-based box (room meters: x/z centered, y from 0 up to size.y — matching
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
        // center (toward the ORIGIN would degenerate: a floor-based box's bottom face contains it).
        static void EmitInward(Vector3[] v, Vector3 ctr, List<int> tris, int baseIdx, int i0, int i1, int i2)
        {
            Vector3 n = Vector3.Cross(v[i1] - v[i0], v[i2] - v[i0]);
            Vector3 c = (v[i0] + v[i1] + v[i2]) / 3f;
            if (Vector3.Dot(n, ctr - c) < 0f) { var t = i1; i1 = i2; i2 = t; }   // normal points outward -> flip
            tris.Add(baseIdx + i0); tris.Add(baseIdx + i1); tris.Add(baseIdx + i2);
        }

        readonly List<SourceBase> _pushBuf = new();   // reusable snapshot (a FrameSync callback may unregister mid-loop)

        // One registry for every source kind (Emitter's clip voices and PushEmitter's push voices ride
        // the same centralized per-frame push, before the listener + the one commit), all flowing
        // through the same snapshot — so a callback that mutates the registry mid-loop is safe no
        // matter which source kind fires it.
        public void Register(SourceBase s)
        {
            if (ReferenceEquals(s, null)) return;
            if (!_sources.Contains(s)) _sources.Add(s);
            if (s.NativeHandle != 0) _byHandle[s.NativeHandle] = s;   // the ended-event route (see DrainEnded)
        }

        public void Unregister(SourceBase s)
        {
            // ReferenceEquals, not `!s`: a component already torn down compares equal to null under
            // Unity's overloaded operator, and its route still has to go or the map keeps the corpse.
            if (ReferenceEquals(s, null)) return;
            _sources.Remove(s);
            // Removing here is what keeps the map from leaking and from misrouting. It cannot go stale
            // in the meantime: rt bumps a slot's generation before reissuing it, so a source created
            // after this one is destroyed never mints this key, and the engine drops any undrained
            // ended event whose generation no longer matches.
            if (_byHandle.TryGetValue(s.NativeHandle, out var cur) && ReferenceEquals(cur, s))
                _byHandle.Remove(s.NativeHandle);
        }

        // Beds register HERE, in a second registry. A bed is a voice — the core reports its handle
        // through bwa_poll_ended and bwa_poll_looped like any other — but AmbisonicBed is not a
        // SourceBase, because a bed has no position and the source registry exists to PUSH one every
        // frame. So the event drains and the post-commit pass consult this list too. Bed and source
        // handles come out of ONE pool (bwa_bed_create IS bwa_source_create), so a handle is either a
        // source's or a bed's and never both, which is what makes consulting the two maps in turn
        // correct.
        public void RegisterBed(AmbisonicBed b)
        {
            if (ReferenceEquals(b, null)) return;
            if (!_beds.Contains(b)) _beds.Add(b);
            if (b.NativeHandle != 0) _bedsByHandle[b.NativeHandle] = b;
        }

        public void UnregisterBed(AmbisonicBed b)
        {
            // ReferenceEquals, not `!b`: a component already torn down compares equal to null under
            // Unity's overloaded operator, and its route still has to go or the map keeps the corpse.
            if (ReferenceEquals(b, null)) return;
            _beds.Remove(b);
            if (_bedsByHandle.TryGetValue(b.NativeHandle, out var cur) && ReferenceEquals(cur, b))
                _bedsByHandle.Remove(b.NativeHandle);
        }

        // ---- room registration ----------------------------------------------------------------------
        //
        // Room.UnityToRoom is a static, and this Engine is its owner while roomOrigin is set. Lifetime:
        //   - applied in Awake, every LateUpdate before the push, and from OnValidate / OnDrawGizmos in
        //     the stopped editor;
        //   - held while this object lives, a disabled Engine included (it still owns the native engine
        //     and its sources);
        //   - released to identity in OnDestroy. Room clears it by itself when the owner is gone without
        //     one (a closed edit-mode scene) and on every entry into Play mode.
        // An origin that goes away DURING play keeps the last registration: the Engine is
        // DontDestroyOnLoad and the origin usually is not, and a scene load must not turn the room
        // around mid-session.
        // Known limit, stopped editor only: two Engines with different origins both write the one
        // static as they draw, so the last one drawn wins. Play mode has one Engine.
        bool _originHeld, _originLostWarned, _originScaleWarned, _originTiltWarned, _fdnDirectionWarned;
        Transform _originSeen;   // the origin the warnings above were armed for

        bool ApplyRoomOrigin()
        {
            // A duplicate Engine that Awake marked for destruction must not take the registration.
            if (Application.isPlaying && Instance != null && Instance != this) return false;
            bool changed;
            if (roomOrigin)
            {
                bool fresh = !ReferenceEquals(roomOrigin, _originSeen);
                if (fresh)                                   // a new origin gets its own warnings
                {
                    _originSeen = roomOrigin;
                    _originLostWarned = _originScaleWarned = _originTiltWarned = false;
                }
                _originHeld = Application.isPlaying;
                changed = Room.SetOrigin(roomOrigin, this);
                // Only when the pose moved, not every frame. A scale edited by itself moves nothing
                // (scale is not in the registration), so it is caught at the next move.
                if (changed || fresh) CheckOriginShape();
            }
            else if (_originHeld)
            {
                if (!_originLostWarned)
                {
                    _originLostWarned = true;
                    Debug.LogWarning("[bw_audio] Room Origin is gone (destroyed with its scene, or " +
                                     "cleared). Keeping the last registration until a new origin is " +
                                     "assigned. Parent the origin under the Engine to keep it across " +
                                     "scene loads, or call Room.ReleaseOrigin(engine) to go back to " +
                                     "identity.", this);
                }
                return false;
            }
            else
            {
                _originSeen = null;
                changed = Room.ReleaseOrigin(this);
            }
            if (changed && Application.isPlaying && Ready) OnRegistrationChanged();
            return changed;
        }

        void CheckOriginShape()
        {
            if (!_originScaleWarned && (roomOrigin.lossyScale - Vector3.one).sqrMagnitude > 1e-6f)
            {
                _originScaleWarned = true;
                Debug.LogWarning("[bw_audio] Room Origin '" + roomOrigin.name + "' has a scale of " +
                                 roomOrigin.lossyScale + " (its own or a parent's). The scale is " +
                                 "ignored: room meters are Unity meters.", this);
            }
            float tilt = Vector3.Angle(roomOrigin.up, Vector3.up);
            if (!_originTiltWarned && tilt > 1f)
            {
                _originTiltWarned = true;
                Debug.LogWarning("[bw_audio] Room Origin '" + roomOrigin.name + "' is tilted " +
                                 tilt.ToString("F1") + " degrees from level. A CAVE floor origin " +
                                 "should be level. Positions and directions follow the tilt, but " +
                                 "SceneSetGround uses the origin's height only.", this);
            }
        }

        // The registration moved while the engine runs. Positions follow by themselves (they are pushed
        // every frame); these are the things that BAKED the old one. Not the room box: it is authored
        // in room meters. Runs only on a change, so a still origin costs nothing.
        void OnRegistrationChanged()
        {
            if (_staticHasGeometry) _rebakeStatic = true;    // static AcousticGeometry: one BVH rebuild, this LateUpdate
            if (_groundSet)
                Bwa.bwa_scene_set_ground(_eng, Room.Height(_groundWorldY), ResolveMaterial(_groundMaterial),
                                         _groundPressureRelease);
            // The FDN's decay direction rides bwa_fdn_config, which is load-time: the ABI has no live call.
            if (enableFdnReverb && fdnDecayDirection != Vector3.zero && !_fdnDirectionWarned)
            {
                _fdnDirectionWarned = true;
                Debug.LogWarning("[bw_audio] Room Origin moved after start. The FDN decay direction is " +
                                 "load-time and keeps the room direction it had at start.", this);
            }
        }

        // A second scene's Engine found this one alive and is about to destroy itself. Its origin is
        // the room's place in the scene that just loaded, so take it.
        void AdoptRoomOrigin(Transform origin, Transform dying)
        {
            // An origin parented under the dying Engine would die with it one frame later.
            if (origin != dying && origin.IsChildOf(dying)) origin.SetParent(transform, worldPositionStays: true);
            roomOrigin = origin;
            ApplyRoomOrigin();
        }

        void LateUpdate()
        {
            ApplyRoomOrigin();   // before the Ready check: a failed engine still draws its gizmos in the right place
            if (!Ready) return;
            RefreshClock();   // keep the wall↔dsp offset estimator warm even when no helper ran this frame
            if (_rebakeStatic)   // a scene loaded/unloaded since last frame -> re-collect static geometry once
            {
                RebuildSceneMesh(CollectStaticGeometry());
            }
            // Iterate a snapshot: an emitter's onFinished handler may disable it, which runs OnDisable ->
            // Unregister -> _sources.Remove mid-loop. Mutating the list under a foreach would throw and
            // skip the commit. _pushBuf is cleared+refilled (no per-frame allocation after warmup).
            _pushBuf.Clear();
            _pushBuf.AddRange(_sources);
            foreach (var s in _pushBuf) if (s) s.FrameSync();      // all sources first...
            if (FeedsListener) ResolveListener();                  // a late-bound head: a timer check, then nothing
            var head = feedListener ? listener : _fallbackListener;   // the second only on the fallback
            if (head)
            {
                var p = Room.Pos(head.position);
                var q = Room.Rot(head.rotation);
                Bwa.bwa_set_listener_pose(_eng, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
                CheckListenerPlacement(p, head);
            }
            PushExtraListeners();                                 // ...the other occupants (commit-gated too)...
            Bwa.bwa_commit(_eng);                                    // ...then one atomic snapshot
            // ...and only THEN the event drains. bwa_commit is what runs the pass that fills both rings,
            // so polling before it reads a frame-old picture and delays every callback by a frame for no
            // reason. Ended before looped is not load-bearing (the two rings are independent), but the
            // post-commit pass must come after BOTH: it is where an emitter's halt fallback reads
            // IsPlaying, and that read must not race the event describing the same end.
            DrainEnded();
            DrainLooped();
            PostCommitPass();
        }

        // A listener far from the speakers is not an error to the engine: it pans for wherever the head
        // is, and a head 12 m away from the array just hears "mostly nothing". That is the symptom of a
        // wrong Listener Transform or Room Origin, so say it once per excursion, at most every 30 s.
        // Two tests, each with a margin for a head above a ring of speakers or a seated listener:
        //   - the array profiles: outside the speakers' bounding box. Binaural drives no speakers, and
        //     at a desk the head can be anywhere, so it never gets this one.
        //   - any profile with the room box on: outside the box, which is what the reflections mirror off.
        // Not armed until the listener first moves or ListenerGraceS passes: a head parked at the floor
        // origin until tracking connects is not a misconfiguration.
        const float ListenerMarginM = 1f;
        const float ListenerWarnEveryS = 30f;
        const float ListenerGraceS = 5f;
        bool _listenerOutside, _speakerBoxValid, _listenerArmed, _listenerSeen;
        float _listenerWarnAfter, _listenerFirstSeenAt;
        Vector3 _speakerLo, _speakerHi, _listenerFirstPos;

        void CheckListenerPlacement(Vector3 p, Transform head)
        {
            if (!_listenerArmed)
            {
                if (!_listenerSeen) { _listenerSeen = true; _listenerFirstPos = p; _listenerFirstSeenAt = Time.realtimeSinceStartup; }
                _listenerArmed = (p - _listenerFirstPos).sqrMagnitude > 1e-4f ||
                                 Time.realtimeSinceStartup - _listenerFirstSeenAt > ListenerGraceS;
                if (!_listenerArmed) return;
            }
            string what = null; float by = 0f;
            if (ActiveProfile != BwaProfile.Binaural)
            {
                if (!_speakerBoxValid)
                {
                    var xyz = SpeakerPositions();   // fixed for the engine's lifetime
                    _speakerLo = Vector3.positiveInfinity; _speakerHi = Vector3.negativeInfinity;
                    for (int i = 0; i + 2 < xyz.Length; i += 3)
                    {
                        var s = new Vector3(xyz[i], xyz[i + 1], xyz[i + 2]);
                        _speakerLo = Vector3.Min(_speakerLo, s); _speakerHi = Vector3.Max(_speakerHi, s);
                    }
                    _speakerBoxValid = xyz.Length >= 3;
                }
                if (_speakerBoxValid) { by = OutsideBy(p, _speakerLo, _speakerHi); if (by > ListenerMarginM) what = "the speaker array's bounding box"; }
            }
            if (what == null && enableRoomBox)
            {
                var half = new Vector3(roomSizeMeters.x * 0.5f, 0f, roomSizeMeters.z * 0.5f);
                by = OutsideBy(p, -half, half + new Vector3(0f, roomSizeMeters.y, 0f));
                if (by > ListenerMarginM) what = "the room box";
            }

            bool outside = what != null;
            if (outside && !_listenerOutside && Time.realtimeSinceStartup >= _listenerWarnAfter)
            {
                _listenerWarnAfter = Time.realtimeSinceStartup + ListenerWarnEveryS;
                Debug.LogWarning($"[bw_audio] Listener '{head.name}' is at room ({p.x:F2}, {p.y:F2}, " +
                                 $"{p.z:F2}), {by:F1} m outside {what}. Every source is panned for a head " +
                                 "that is not in the room. Check Engine > Listener and Room Origin.", this);
            }
            _listenerOutside = outside;
        }

        static float OutsideBy(Vector3 p, Vector3 lo, Vector3 hi)
            => Vector3.Max(Vector3.Max(lo - p, p - hi), Vector3.zero).magnitude;

        // Every handle bwa_poll_ended reports, routed to the source component that owns it. THIS is the
        // one place that call may be made from: the drain is engine-wide and destructive, so a second
        // caller anywhere would consume events belonging to somebody else's sources and they would
        // simply never fire. Emitters take completions from here, never by polling for themselves.
        readonly Dictionary<uint, SourceBase> _byHandle = new();   // native source handle -> owner
        readonly Dictionary<uint, AmbisonicBed> _bedsByHandle = new();  // native bed handle -> owner (see RegisterBed)
        static readonly uint[] _endedBuf = new uint[64];           // reused; bwa_poll_ended writes at most cap
        ulong _endedDropped;                                       // the engine's running dropped total
        bool _endedDropWarned;

        void DrainEnded()
        {
            for (;;)
            {
                // Re-check every pass: an onFinished handler below is arbitrary user code and may have
                // destroyed the Engine, which zeroes _eng.
                if (!Ready) return;
                uint n = Bwa.bwa_poll_ended(_eng, _endedBuf, (uint)_endedBuf.Length, out ulong dropped);
                if (dropped != _endedDropped)
                {
                    _endedDropped = dropped;
                    if (!_endedDropWarned)
                    {
                        _endedDropWarned = true;
                        Debug.LogWarning("[bw_audio] " + dropped + " voice-completion events were dropped " +
                                         "before they could be read, so that many onFinished callbacks " +
                                         "never fired. The engine's ended ring is bounded and drops the " +
                                         "OLDEST: something stalled Engine's LateUpdate. Watch " +
                                         "Engine.EndedEventsDropped for the running total (this warns once).");
                    }
                }
                for (int i = 0; i < (int)n; i++)
                {
                    // A handle with no route is normal, not an error: the source was destroyed after its
                    // voice ended but before this drain, or it belongs to a source kind that has no
                    // completion event.
                    if (_byHandle.TryGetValue(_endedBuf[i], out var s) && s) s.NotifyEnded();
                    else if (_bedsByHandle.TryGetValue(_endedBuf[i], out var b) && b) b.NotifyEnded();
                }
                if (n < (uint)_endedBuf.Length) break;             // drained
            }
        }

        // The loop-boundary drain, alongside the ended one and under the same single-owner rule:
        // bwa_poll_looped is engine-wide and destructive, so a second caller anywhere would eat wraps
        // belonging to somebody else's sources. Deliberately NOT folded into DrainEnded — a wrap is not
        // an end. The voice is still playing, so none of the completion latching applies to it, and
        // entangling the two would put a "did this stop" question in the path of an event that means the
        // opposite. One callback per WRAP: a short loop region can wrap several times in one audio block.
        static readonly uint[] _loopedBuf = new uint[64];           // reused; bwa_poll_looped writes at most cap
        ulong _loopedDropped;
        bool _loopedDropWarned;

        void DrainLooped()
        {
            for (;;)
            {
                // Re-check every pass: an onLoop handler below is arbitrary user code and may have
                // destroyed the Engine, which zeroes _eng.
                if (!Ready) return;
                uint n = Bwa.bwa_poll_looped(_eng, _loopedBuf, (uint)_loopedBuf.Length, out ulong dropped);
                if (dropped != _loopedDropped)
                {
                    _loopedDropped = dropped;
                    if (!_loopedDropWarned)
                    {
                        _loopedDropWarned = true;
                        Debug.LogWarning("[bw_audio] " + dropped + " loop-boundary events were dropped " +
                                         "before they could be read, so that many onLoop callbacks never " +
                                         "fired. The engine's loop ring is bounded and drops the OLDEST: " +
                                         "either Engine's LateUpdate stalled, or a loop region is short " +
                                         "enough to wrap faster than the frame rate reads it. Watch " +
                                         "Engine.LoopEventsDropped for the running total (this warns once).");
                    }
                }
                for (int i = 0; i < (int)n; i++)
                {
                    // No route is normal, not an error: a source destroyed between the wrap and this
                    // drain, or a source kind with no loop event.
                    if (_byHandle.TryGetValue(_loopedBuf[i], out var s) && s) s.NotifyLooped();
                    else if (_bedsByHandle.TryGetValue(_loopedBuf[i], out var b) && b) b.NotifyLooped();
                }
                if (n < (uint)_loopedBuf.Length) break;            // drained
            }
        }

        // The per-source post-commit pass, after BOTH drains, so a natural end is reported by its event
        // and not by an emitter's own IsPlaying edge (Emitter.PostCommit). _pushBuf still holds this
        // frame's snapshot; a handler above may have disabled entries, which the null + Live guards absorb.
        void PostCommitPass()
        {
            if (!Ready) return;
            foreach (var s in _pushBuf) if (s) s.PostCommit();
            // Beds take the same pass, for the same reason: a bed's halt fallback reads
            // bwa_bed_is_playing, and that read must not race the event describing the same end.
            // Snapshotted like the sources are — an onFinished handler may disable a bed, which runs
            // OnDisable -> UnregisterBed -> _beds.Remove mid-loop.
            _bedBuf.Clear();
            _bedBuf.AddRange(_beds);
            foreach (var b in _bedBuf) if (b) b.PostCommit();
        }

        readonly List<AmbisonicBed> _bedBuf = new();   // reusable snapshot, like _pushBuf

        /// <summary>How many voice-completion events the engine has DROPPED because nothing read them in
        /// time (running total for the engine's lifetime). The ended ring is bounded and drops the oldest,
        /// so a rising number means that many <c>Emitter.onFinished</c> callbacks never fired. It should
        /// stay 0: it only moves if Engine's LateUpdate stopped running while sources kept ending.</summary>
        public ulong EndedEventsDropped => _endedDropped;

        /// <summary>How many loop-boundary events the engine has DROPPED because nothing read them in
        /// time (running total for the engine's lifetime). A rising number means that many
        /// <c>Emitter.onLoop</c> callbacks never fired. Unlike the ended total this one can move without
        /// anything being wrong: a loop region shorter than a frame wraps more often than LateUpdate
        /// reads it, so pace trials off the count you receive, not off the count you expected.</summary>
        public ulong LoopEventsDropped => _loopedDropped;

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
        // Everything here is drawn in ROOM meters through the inverse of the coordinate seam, so a wrong
        // Room.UnityToRoom puts the array and the box visibly in the wrong place — which is the cheapest
        // possible check on the one setting that silently ruins spatial audio.
        void OnDrawGizmos()
        {
            // Stopped, nothing else follows a moved origin (OnValidate fires on Inspector edits only).
            if (!Application.isPlaying) ApplyRoomOrigin();
            var prev = Gizmos.matrix;
            Gizmos.matrix = Room.RoomToUnityMatrix();

            if (roomOrigin) DrawRoomAxes();
            if (enableRoomBox)
            {
                Gizmos.color = new Color(1f, 0.9f, 0.3f, 0.9f);
                var size = new Vector3(roomSizeMeters.x, roomSizeMeters.y, roomSizeMeters.z);
                Gizmos.DrawWireCube(new Vector3(0f, size.y * 0.5f, 0f), size);   // floor-based: y from 0 up
            }
            if (showSpeakers) DrawSpeakers();

            Gizmos.matrix = prev;
        }

        // The room frame at its origin, in room meters: blue = the layout's +z, red = room-right, which
        // is -x. Blue must point at whatever wall the LAYOUT calls front. That can be the CAVE's back
        // wall (Motive's +z often is). Red on the left when you face along blue would be a mirror, which
        // this registration cannot produce.
        void DrawRoomAxes()
        {
            const float len = 0.6f, y = 0.02f, head = 0.08f;   // lifted off the floor so a ground plane does not hide it
            var o     = new Vector3(0f, y, 0f);
            var front = new Vector3(0f, y, len);
            var right = new Vector3(-len * 0.6f, y, 0f);
            Gizmos.color = new Color(0.25f, 0.5f, 1f, 1f);
            Gizmos.DrawLine(o, front);
            Gizmos.DrawLine(front, front + new Vector3( head * 0.5f, 0f, -head));
            Gizmos.DrawLine(front, front + new Vector3(-head * 0.5f, 0f, -head));
            Gizmos.color = new Color(1f, 0.3f, 0.3f, 1f);
            Gizmos.DrawLine(o, right);
            Gizmos.DrawSphere(right, 0.02f);
#if UNITY_EDITOR
            UnityEditor.Handles.Label(Room.FromRoom(front), "room front (+z)");
            UnityEditor.Handles.Label(Room.FromRoom(right), "room right (-x)");
#endif
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
                string path = AssetPath(layoutFile);
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
            Room.ReleaseOrigin(this);   // no-op unless this Engine holds the registration
            // Before the Ready check: every Engine claims Instance in Awake, owning audio or not.
            if (Instance == this) Instance = null;
            _fallbackTaken = false; _fallbackListener = null;
            if (!Ready) return;
            Bwa.bwa_stop(_eng); Bwa.bwa_destroy(_eng); _eng = IntPtr.Zero;
        }
    }
}
