// Bwa.cs — P/Invoke binding for the bw_audio C ABI (include/bw_audio.h).
//
// Pure marshalling layer, no Unity dependency (so it compiles + can be unit-tested standalone; the
// MonoBehaviour wrappers live in Engine.cs / Emitter.cs). Drop bw_audio.dll + phonon.dll in
// Assets/Plugins/. THREADING: every call must come from ONE thread (Unity's main thread); the
// per-frame calls are non-blocking. See docs/api.md + docs/concurrency.md.
//
// Marshalling rules that matter here:
//   * C `bool` is 1 byte  -> [MarshalAs(UnmanagedType.I1)].
//   * C `const char*`     -> [MarshalAs(UnmanagedType.LPUTF8Str)] for IN strings;
//                            IntPtr + Marshal.PtrToStringUTF8 for RETURNED strings (do not let the
//                            marshaller free a pointer the engine still owns).
//   * BwaDesc.profile is a 4-byte enum, NOT a string — marshalling it as a string corrupts the call.
//   * Handles (BwaSound/BwaSource/BwaBed/BwaMaterial) are uint = (index | generation<<16); 0 = invalid.
using System;
using System.Runtime.InteropServices;

namespace BwAudio
{
    // Cave drives the array; Binaural is the direct per-source headphone render (full pose);
    // CaveSim auditions the array render on headphones (virtual speakers, DBAP artifacts included);
    // CaveBoth is the rig plus the CaveSim tap. Maps 1:1 to bwa_profile in bw_audio.h.
    public enum BwaProfile : int { Cave = 0, Binaural = 1, CaveSim = 2, CaveBoth = 3 }
    public enum BwaDirectivity : int { Omni = 0, Cardioid = 1, Figure8 = 2 }
    public enum BwaTestKind : int { Off = 0, Sine = 1, Noise = 2 }
    public enum BwaPanner : int { Dbap = 0, Spcap = 1, Vbap = 2 }
    // Value 0 is RESERVED for default-init and mirrors the C enum: it means "the engine's current
    // default", not a named algorithm, so the default can move without an ABI break.
    public enum BwaBedDecoder : int { Default = 0, Allrad = 1, Epad = 2 }
    public enum BwaSpreadMode : int { Lobe = 0, Mdap = 1, Spectral = 2 }
    public enum BwaBedRenderer : int { Matrix = 0, Parametric = 1 }

    /// <summary>Mirrors bwa_load_flags: which loader the shared asset cache uses for a path. The cache
    /// key is (path, flags), so the SAME file held in RAM and streamed are two different entries — which
    /// is exactly the multi-key case a binding-side dictionary had to special-case. None = the in-memory
    /// mono loader (bwa_load_sound). Combinations no loader can express (Ambix|Fuma, Stream with either)
    /// are REFUSED with a message in bwa_last_error, never narrowed to one of them.</summary>
    [Flags]
    public enum BwaLoadFlags : uint { None = 0, Stream = 1 << 0, Ambix = 1 << 1, Fuma = 1 << 2 }

    /// <summary>Mirrors bwa_source_kind: what a source IS, which is what bwa_source_preset fills a
    /// complete BwaSourceDesc for. Nothing in the preset table is measured — a kind differs from
    /// Default only where a doc already argues the case. See docs/api.md, "What each preset rests on".</summary>
    public enum BwaSourceKind : int { Default = 0, Prop = 1, Voice = 2, Ambience = 3, Ui = 4 }

    /// <summary>Mirrors bwa_material_type: the engine's built-in acoustic materials, in ABI order
    /// (the value indexes the engine's coefficient table).</summary>
    public enum BwaMaterialPreset : int
    {
        Generic = 0, Brick, Concrete, Ceramic, Gravel, Carpet, Glass, Plaster, Wood, Metal, Rock
    }

    /// <summary>Mirrors bwa_result: 0 = success; nonzero = the failure class (bwa_last_error has detail).</summary>
    public enum BwaResult : int
    {
        Ok = 0, ErrConfig, ErrDevice, ErrLayout, ErrHrtf, ErrState, ErrInternal, ErrTracker
    }

    /// <summary>Mirrors bwa_sink_type: the output-device policy. Auto tries ASIO and falls back to
    /// the silent offline sink (the engine keeps rendering — visual-only); Asio demands a real
    /// device (an open failure fails bwa_start loudly); Null forces the offline sink; Manual is
    /// the deterministic caller-pumped sink (bwa_render_block — offline/golden tests, not Unity).</summary>
    public enum BwaSinkType : int { Auto = 0, Asio = 1, Null = 2, Manual = 3 }

    /// <summary>Mirrors bwa_tracker_state: liveness of a connected tracker's stream (Engine.TrackerStatus).
    /// Disconnected = no tracker on this engine; NoData = connected but no frames arriving (check the
    /// stream/network/port); NoBody = frames arriving but the followed rigid body has no valid pose
    /// (check the id/name, or it's occluded now); Live = the pose is arriving and current.</summary>
    public enum BwaTrackerState : int { Disconnected = 0, NoData = 1, NoBody = 2, Live = 3 }

    [StructLayout(LayoutKind.Sequential)]
    public struct BwaDesc
    {
        public BwaProfile profile;                                       // 4-byte int enum
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string layoutPath;  // const char* (or null)
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string hrtfPath;    // const char* (or null)
        public uint sampleRate;                                         // 48000
        public uint blockSize;                                          // e.g. 256
        public BwaSinkType sink;                                        // device policy; 0 = Auto
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string asioDriver;  // ASIO driver name; null = auto-pick
        [MarshalAs(UnmanagedType.I1)] public bool embree;               // Embree ray tracing (falls back if absent)
        [MarshalAs(UnmanagedType.I1)] public bool enablePathing;        // sound-pathing sim at bwa_start (needs SDK + scene)
        public BwaBedDecoder bedDecoder;                                 // diffuse-bed decoder; 0 = the engine default
        public uint reserved0, reserved1, reserved2, reserved3;         // matches reserved[4]; keep zero
    }

    /// <summary>Mirrors bwa_tracker_desc: the OptiTrack/NatNet connection for internal tracking.
    /// Zero-init and set what you need — every field's zero/null is its default.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BwaTrackerDesc
    {
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string multicast;      // null => "239.255.42.99"
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string server;         // Motive host; null => multicast-only
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string localIface;     // local interface IP; null => any
        public ushort dataPort;                                            // 0 => 1511
        public ushort commandPort;                                         // 0 => 1510
        public int    rigidBodyId;                                         // 0 => first rigid body in the frame
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string rigidBodyName;  // by name (needs server); null => use the ID
        public int    versionMajor, versionMinor;                          // 0 => handshake / default 3.1
        public uint   reserved0, reserved1, reserved2, reserved3;          // matches reserved[4]; keep zero
    }

    /// <summary>Mirrors bwa_clock_model: how fast the device clock runs against the host clock,
    /// fitted over the per-block stamps (see Engine.GetClockModel).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BwaClockModel
    {
        public double ppm;        // device vs host, parts per million (+ = device fast)
        public double ppmSigma;   // 1-sigma standard error of ppm (optimistic: assumes independent stamp noise)
        public double rateHz;     // fitted device rate, samples per host second
        public double spanS;      // host seconds of stamps behind the fit
        public double jitterNs;   // rms residual of the stamps about the fit (driver stamp quality)
        public uint   stamps;     // effective (exponentially weighted) stamp count
    }

    /// <summary>Mirrors bwa_health: was the audio callback starved, and by whom. `xruns` is the DEVICE
    /// running on without us (bigger buffer, quieter machine); `lateBlocks` is our render overrunning
    /// the block period, which is what produces those (cheaper scene, watch peakLoad); `streamStarves`
    /// is neither — the disk thread failed to refill a streamed voice and its tail rendered silence.
    /// Every count is monotonic since start and means nothing without `blocks` under it.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BwaHealth
    {
        public ulong blocks;          // blocks rendered — the denominator
        public ulong xruns;           // device dropouts: it ran on without us
        public ulong droppedFrames;   // frames those dropouts swallowed
        public ulong driverResyncs;   // the driver reporting a discontinuity itself
        public ulong lateBlocks;      // our render overran the block period
        public ulong streamStarves;   // a streamed voice's ring ran dry without the asset ending
        public float peakLoad;        // worst block's render time / block period; 1.0 = at budget
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BwaReflectionsDesc
    {
        public float irSeconds;      // reverb tail length; 0 => default 1.0
        public uint  order;          // ambisonic order 1 or 2; 0 => default 1
        public uint  numRays;        // off-thread ray budget; 0 => default 4096
        public uint  numBounces;     // off-thread bounce depth; 0 => default 16
        public int   enabled;        // 0 => no bed created
        public int   bake;           // non-0 => precompute (bake) the reverb at a probe grid at bwa_start
        public uint  reserved0, reserved1, reserved2;   // matches reserved[3]; keep zero
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BwaFdnDesc
    {
        public int   enabled;          // 0 => no FDN created (the default)
        public float rt60LowSeconds;   // low-band decay; 0 => default 1.2
        public float rt60HighSeconds;  // high-band decay; 0 => default 0.7
        public float crossoverHz;      // decay-band crossover; 0 => default 2000
        public float decayDirX, decayDirY, decayDirZ;   // room space; all-zero => uniform decay
        public float decayFactor;      // decay scale toward the direction: <1 dies faster; 0 or 1 => uniform
        public uint  reserved0, reserved1, reserved2;   // matches reserved[3]; keep zero
    }

    /// <summary>Mirrors bwa_source_desc: every per-source CONFIGURATION knob in one struct, the same
    /// fill-then-apply shape as Bwa.BwaTuning (which does it for the ENGINE knobs) and with the same
    /// structSize guard for the same reason: THIS STRUCT'S ZERO IS NOT ITS DEFAULT (a zero-filled one
    /// means gain 0 = silence and pitch 0 = invalid), so a zero-init mistake must fail loudly. NEVER
    /// apply a default(BwaSourceDesc) — always start from Bwa.SourcePreset.
    /// <para>What is deliberately OUT: position and orientation (per-frame, commit-gated, so they belong
    /// to the frame loop), playback state (an apply must never restart a sound), and the manual-occlusion
    /// LEVEL (a live per-frame value; the desc carries only the occlusion on/off).</para></summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BwaSourceDesc
    {
        public uint  structSize;        // set by Bwa.SourcePreset / bwa_source_get_desc; apply refuses a wrong one
        public float gain;              // linear; 1 = unity
        public float pitch;             // playback rate; 1 = native, clamped [0.25, 4]
        public int   priority;          // steal priority 0..255; 128 = default
        public uint  group;             // mix group 0..7
        public float spread;            // angular width 0 = point .. 1 = wide
        public float extentHeight;      // vertical extent 0..1 when >= 0 (spread is then the WIDTH); < 0 = isotropic
        public float sizeMeters;        // metric radius (m); 0 = point
        public float reverbSend;        // wet-send level; 1 = default
        public float attenRefDist;      // distance-curve override: <= 0 = no override (the layout's curve)
        public float attenRolloff;      // ... its exponent; 0 = constant level at any distance
        public float attenMinGain;      // ... its floor, 0..1
        public float directivityWeight; // 0 = omni (off) .. 0.5 = cardioid .. 1 = figure-8
        public float directivityPower;  // ... lobe sharpness, >= 1; 1 = default
        [MarshalAs(UnmanagedType.I1)] public bool doppler;
        [MarshalAs(UnmanagedType.I1)] public bool airAbsorption;
        [MarshalAs(UnmanagedType.I1)] public bool loudnessComp;
        [MarshalAs(UnmanagedType.I1)] public bool proximity;
        [MarshalAs(UnmanagedType.I1)] public bool occlusion;
        [MarshalAs(UnmanagedType.I1)] public bool earlyReflections;
        [MarshalAs(UnmanagedType.I1)] public bool reverb;
        [MarshalAs(UnmanagedType.I1)] public bool reverbDistance;
        [MarshalAs(UnmanagedType.I1)] public bool pathing;
        public uint r0, r1, r2, r3;     // matches reserved[4]; keep zero
    }

    /// <summary>Raw P/Invoke entry points — every BWA_API function in include/bw_audio.h except
    /// bwa_set_output_capture (an audio-thread callback) and bwa_render_block (the manual-sink
    /// golden-render path), which Unity never uses.</summary>
    public static class Bwa
    {
        const string DLL = "bw_audio";
        const CallingConvention CC = CallingConvention.Cdecl;

        // ---- lifecycle (load time; may block/allocate) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bwa_create(in BwaDesc cfg);
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaResult bwa_start(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaResult bwa_stop(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void   bwa_destroy(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bwa_last_error(IntPtr e);     // PtrToStringUTF8; null = none
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bwa_get_audio_backend(IntPtr e);  // "asio:<drv>" / "null" / "none"; binaural/both append "(steam HRTF|simple-pan monitor)"

        // ---- ASIO device query (engine-free; call before bwa_create to populate a driver picker for
        // BwaDesc.asioDriver). Reads the OS's registered-driver list fresh each call; nothing is opened. ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_asio_driver_count();
        // Fills buf with driver `index`'s registered name (NUL-terminated, truncated to cap-1) — the exact
        // string BwaDesc.asioDriver expects. False = index out of range. The AsioDriverName helper below wraps it.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_get_asio_driver_name(uint index, [Out] byte[] buf, uint cap);

        // ---- internal tracking (OptiTrack/NatNet; may block — lifecycle-class, not per-frame) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaResult bwa_tracker_connect(IntPtr e, in BwaTrackerDesc desc);
        [DllImport(DLL, CallingConvention = CC)] public static extern void      bwa_tracker_disconnect(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaTrackerState bwa_tracker_status(IntPtr e);  // never blocks; poll per-frame

        // ---- assets (load time; file I/O) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_load_sound(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_load_sound_streaming(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_unload_sound(IntPtr e, uint snd);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_load_ambix(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        // Legacy FuMa B-format (.amb and friends: WXYZ order, MaxN + the W -3 dB) — converted to AmbiX
        // at load, so the returned asset is indistinguishable from bwa_load_ambix of the same field.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_load_fuma(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        // Asset metadata (any time after a successful load): frames at the ENGINE rate (seconds = frames /
        // bwa_get_sample_rate). get_frames is 0 for an invalid handle or a stream of unknown length (push
        // sources); get_channels is 1 for a mono point-source asset, 4/9/16 for an ambisonic bed (order
        // 1/2/3), 0 for an invalid handle.
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_sound_get_frames(IntPtr e, uint snd);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint  bwa_sound_get_channels(IntPtr e, uint snd);

        // ---- shared-ownership asset cache (load time; file I/O) ----
        // The by-path, refcounted tier over the same four loaders above: the key is (path, flags), the
        // same key returns the SAME handle with one more reference, and the last release unloads through
        // the retire-ack path (safe while playing). Engine.Load/LoadAmbix/LoadFuma ride this instead of a
        // binding-side dictionary. Do NOT mix tiers on one handle: bwa_unload_sound on an acquired handle
        // is refused, and so is bwa_sound_release on a handle the cache does not own.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_sound_acquire(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, BwaLoadFlags flags);
        // Async twin: a usable handle IMMEDIATELY, decoded on the engine's loader thread. Play it at once —
        // the source binds and stays SILENT until the data lands, then starts from the top. bwa_play_oneshot
        // and bwa_source_queue REFUSE a not-ready handle (neither can be held), so check bwa_sound_is_ready
        // before those two. BWA_LOAD_STREAM loads synchronously here.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_sound_acquire_async(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, BwaLoadFlags flags);
        // Has the data landed? True for anything acquired synchronously; false for a handle the cache does
        // not own. A decode that FAILED never becomes ready and puts its reason in bwa_last_error AT THIS
        // CALL — that is the only way to tell "still decoding" (no error) from "failed" (an error). Calling
        // it also adopts finished loads, as does bwa_commit.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_sound_is_ready(IntPtr e, uint snd);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_sound_release(IntPtr e, uint snd);
        // The handle for (path, flags) if the cache ALREADY holds it, else 0. Pure lookup: never loads,
        // never takes a reference. Probe with THIS, not with bwa_sound_acquire, whose miss path loads the
        // file (and loads it mono, so a bed would answer 1 channel). The handle is borrowed: do not
        // release against it.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_sound_find(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, BwaLoadFlags flags);

        // ---- sources (per-frame; non-blocking) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_source_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_priority(IntPtr e, uint s, int priority);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_destroy(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_pos(IntPtr e, uint s, float x, float y, float z);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_gain(IntPtr e, uint s, float linear);
        // Engine-side timed fades (no per-frame scripting). fade_out lands on the click-free stop path.
        // A later set_gain or fade replaces a running one.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_fade_to(IntPtr e, uint s, float gain, float seconds);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_fade_out(IntPtr e, uint s, float seconds);
        // Mix groups (0..7; sources start in group 0): a group gain folds into every member's gain solve
        // and a paused group freezes its members exactly like per-voice pause.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_group(IntPtr e, uint s, uint group);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_group_set_gain(IntPtr e, uint group, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_group_set_paused(IntPtr e, uint group, [MarshalAs(UnmanagedType.I1)] bool paused);
        // Scene transitions. Both take the SAME click-free path as bwa_source_stop (one-block fade, then
        // end), stop BEDS too, and drop each stopped voice's pending queue. Neither touches group gains,
        // group pause, the global pause, or the master gain: a stop stops sound, it does not reset the
        // mixer. Stopped source handles stay valid and re-playable. bwa_stop_all additionally drops the
        // plays still waiting on an async decode; bwa_group_stop cannot (an unbound held play has no
        // voice to read a group from).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_group_stop(IntPtr e, uint group);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_stop_all(IntPtr e);
        // Playback rate, clamped [0.25, 4] (glides across a block). In-memory sounds only — streams ignore it.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_pitch(IntPtr e, uint s, float rate);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_play(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_play_at(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop, ulong startSample);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_play_loop(IntPtr e, uint s, uint snd, ulong loopBeg, ulong loopEnd);
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_get_dsp_time(IntPtr e);
        // The device-stamped (output sample, host time ns) pair for the last rendered block — the
        // jitter-free wall<->dsp bridge (see Engine.DspTimeAt). hostTimeNs is monotonic on a
        // backend-defined epoch. False (outputs zeroed) until a host-stamped block has rendered.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_get_clock(IntPtr e, out ulong dspSample, out ulong hostTimeNs);
        // Device-reported render->DAC latency in frames (ASIOGetLatencies; the Digiface includes its Dante
        // buffering). 0 = unknown / no physical output (null sink).
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_output_latency_frames(IntPtr e);
        // The fitted device-vs-host clock drift (see Engine.GetClockModel). False (out untouched) until
        // the fit has ~1 s of stamps, and again for ~1 s after a restart re-bases the sample position.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_get_clock_model(IntPtr e, out BwaClockModel model);
        // False = this configuration cannot observe a dropout at all (no device deadline, or a driver
        // that never stamps a valid sample position) — so a 0 xrun count is not a clean bill of health.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_get_health(IntPtr e, out BwaHealth health);
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_get_xruns(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_stop(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_stop_at(IntPtr e, uint s, ulong stopSample);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_queue(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_clear_queue(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_paused(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool paused);
        // Global pause: EVERY voice (memory, streamed, bed) ramps out and freezes; resume continues exactly.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_paused(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool paused);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_seek(IntPtr e, uint s, ulong frame);   // engine-rate frames; in-memory sounds
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_source_is_playing(IntPtr e, uint s);
        // Content playhead in engine-rate frames (latest-wins readback, ~one block of lag): freezes under
        // pause, lands where seek lands, follows pitch at the actual rate; streamed sounds report frames
        // actually CONSUMED (an underrun slips it). 0 = idle, stale handle, or a scheduled play not started.
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_source_get_playhead_frames(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);
        // Procedural (push) sources: the voice plays PCM you push — mono float frames at the engine
        // rate, ~1.3 s ring. Underrun renders silence without losing your place; push returns the
        // count accepted (pace with push_space); push_end ends the voice once the ring drains (not
        // restartable; stop/fade_out also end it). Push from the one control thread, like every bwa_* call.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_source_create_push(IntPtr e);
        [DllImport(DLL, EntryPoint = "bwa_source_push", CallingConvention = CC)] private static extern uint bwa_source_push_native(IntPtr e, uint s, float[] frames, uint n);
        // n is clamped to frames.Length: push_space can report up to the full ring (65536), and passing
        // it with a shorter buffer must never let native code read past the pinned array.
        public static uint bwa_source_push(IntPtr e, uint s, float[] frames, uint n) {
            if (frames == null) return 0;
            if (n > (uint)frames.Length) n = (uint)frames.Length;
            return bwa_source_push_native(e, s, frames, n);
        }
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_source_push_space(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_push_end(IntPtr e, uint s);

        // ---- ambisonic beds (world-locked diffuse soundfields) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_bed_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_play(IntPtr e, uint b, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_gain(IntPtr e, uint b, float linear);
        // Full 3-axis orientation of the soundfield, RADIANS, ROOM frame (level or tilt a capture, or spin
        // it slowly for effect). Positive yaw turns the field about the room's vertical axis from room +z
        // toward room +x; positive pitch tilts the field's front upward; positive roll tilts its top toward
        // room -x (the room's right). Yaw-only (pitch=roll=0) stays on the exact phasor path. Across the
        // seam: yaw needs Room.YawRad (the X mirror reverses it); pitch and roll pass through with the SAME
        // sense (front-up doesn't touch the mirrored axis, and Unity-right maps to room-right). Glided.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_orientation(IntPtr e, uint b, float yawRad, float pitchRad, float rollRad);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_stop(IntPtr e, uint b);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_destroy(IntPtr e, uint b);
        // same voice machinery as the bwa_source_* calls of the same name, bed-named (a bed IS a voice)
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_fade_to(IntPtr e, uint b, float gain, float seconds);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_fade_out(IntPtr e, uint b, float seconds);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_paused(IntPtr e, uint b, [MarshalAs(UnmanagedType.I1)] bool paused);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_seek(IntPtr e, uint b, ulong frame);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_priority(IntPtr e, uint b, int priority);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_group(IntPtr e, uint b, uint group);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bwa_bed_is_playing(IntPtr e, uint b);
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_bed_get_playhead_frames(IntPtr e, uint b);

        // ---- materials / scene geometry (load time; needs the Steam Audio build) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_material_preset(IntPtr e, BwaMaterialPreset preset);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_material_define(IntPtr e, float[] absorption, float scattering, float[] transmission);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_material_release(IntPtr e, uint token);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_mesh_mat(IntPtr e, float[] verts, int nverts, int[] tris, int ntris, uint[] triMaterial);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_box(IntPtr e, float w, float h, float d, uint[] faces);
        // The outdoor degenerate of the box: ONE horizontal mirror plane at height y (room meters) — the
        // ground bounce, the dominant early reflection when there is no room. Replaces any prior box
        // (one room at a time); live-safe like the box (reflections re-solve next block); works with and
        // without the Steam build.
        // pressureRelease flips the reflection's polarity — set it when the "ground" is a water surface
        // and the listener is under it (the Lloyd's-mirror comb).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_ground(IntPtr e, float y, uint mat, [MarshalAs(UnmanagedType.I1)] bool pressureRelease);
        // Flag box faces whose image-source reflection NEGATES (bit f = face f, -x,+x,-y,+y,-z,+z order):
        // reflecting off a much softer medium inverts (an underwater room's ceiling = the surface:
        // 1u << 3). ISM only — occlusion/reverb keep the face's material. Call AFTER set_box/set_ground;
        // a later room call resets every face.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_pressure_release(IntPtr e, uint faceMask);
        // Dynamic (movable) occluders/reflectors — an instanced sub-scene placed by a rigid transform, so
        // moving it is a cheap BVH refit (physics-collider-with-a-transform). add returns a handle >= 0 or
        // -1; geometry is in the mover's LOCAL space (room handedness), placed with set_dynamic_transform.
        [DllImport(DLL, CallingConvention = CC)] public static extern int  bwa_scene_add_dynamic_mesh(IntPtr e, float[] verts, int nverts, int[] tris, int ntris, uint material);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_dynamic_transform(IntPtr e, int handle, float x, float y, float z, float qx, float qy, float qz, float qw);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_remove_dynamic_mesh(IntPtr e, int handle);

        // ---- occlusion (per-frame setters; readback any time) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_occlusion(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // MANUAL occlusion (no SDK needed): drive the sim's own publish path from game logic. `level` is
        // broadband transmittance (1 = clear .. 0 = blocked); `bands` is an optional low/mid/high tilt in
        // [0,1] rendered as the same 3-biquad transmission EQ (so a wall MUFFLES, not just attenuates) —
        // pass null (marshals to NULL) for broadband only. Do NOT also enable bwa_source_set_occlusion on
        // the same source: the sim republishes every tick and wins.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_occlusion_manual(IntPtr e, uint s, float level, float[] bands);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bwa_source_get_occlusion(IntPtr e, uint s);

        // ---- reflection bed ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_reflections_config(IntPtr e, in BwaReflectionsDesc cfg);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_reverb_gain(IntPtr e, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_reverb(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_reverb_send(IntPtr e, uint s, float gain);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_reverb_distance(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_pathing(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);

        // ---- directional FDN reverb bed (load-time; works WITHOUT the Steam Audio build) ----
        // Takes the reverb tap INSTEAD of the Steam reflection bed (one bed at a time), fed by the same
        // per-source send (bwa_source_set_reverb + send levels apply unchanged). Load-time (between
        // bwa_create and bwa_start). Decay is a DESIGN parameter — do not copy the room's measured RT60;
        // the real room adds its own on top.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_fdn_config(IntPtr e, in BwaFdnDesc cfg);
        // LIVE decay retune — the room-transition knob: the tail keeps ringing, only its slope changes
        // (the FDN ramps its loss gains over ~5 ms). <= 0 keeps a parameter's current value; before
        // Start it just updates the staged config, so call it unconditionally on a scene change.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_fdn_set_decay(IntPtr e, float rt60LowS, float rt60HighS, float xoverHz);

        // ---- image-source EARLY reflections (per source; no SDK needed) ----
        // The other half of the phonon-free acoustics path: the FDN renders the late diffuse tail, this
        // renders the six first-order wall bounces — the ones that carry room size and source distance.
        // Each is a real POINT SOURCE at its mirrored position, panned through the engine's own
        // listener-relative panner, so reflections keep correct direction AND parallax as the listener
        // walks — which no shared reverb bed can do. Needs the room: call bwa_scene_set_box first.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_early_reflections(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_early_reflections_gain(IntPtr e, float linear);   // default 1; live

        // ---- propagation effects (no SDK needed; opt-in per source, default off) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_doppler(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_air_absorption(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // Equal-loudness distance compensation: an LF shelf tracking the panner's attenuation ("far, not
        // tinny"). A perceptual stylization, not physics — leave off for strict realism.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_loudness_comp(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // Near-field proximity boost: an LF shelf that RISES as the source closes inside ~1 m (up to
        // +6 dB at the head) — the spherical-wavefront proximity effect, loudness comp's near mirror:
        // at arm's length a source reads as BASS, not just level.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_proximity(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // Engine-wide speed of sound (m/s; default 343 = air; live). Everything rendering a propagation
        // DELAY derives from it — Doppler (delay + pitch magnitude) and the image-source reflection
        // delays — and a change GLIDES every delay to its new target (bends, never steps). 1480 =
        // underwater; small values exaggerate Doppler for slow motion. Clamped to [30, 20000].
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_set_speed_of_sound(IntPtr e, float metersPerSec);
        // Override the LAYOUT's distance-attenuation curve for one source: atten = clamp((refDist /
        // max(d, refDist))^rolloff, minGain, 1). rolloff 0 = constant level at any distance (a direction-only
        // cue that never fades); refDist <= 0 CLEARS the override (back to the layout curve). Applied by ratio,
        // so it composes with spread / dual-band / decorrelation / loudness comp. Mono point sources only.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_attenuation_override(IntPtr e, uint s, float refDist, float rolloff, float minGain);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_spread(IntPtr e, uint s, float amount);
        // Anisotropic extent (BS.2127-style width/height, each 0..1): a shoreline is wide but not tall, rain
        // tall but not wide. Equal values behave as the isotropic spread; bwa_source_set_spread resets to
        // isotropic (last call wins). Rides the spread mode, the size/near floors, and decorrelation.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_extent(IntPtr e, uint s, float width, float height);
        // Source size in METERS (radius; 0 = point). The physical alternative to the angular spread above:
        // the width is the angle the radius subtends from the listener, so a 2 m source STAYS 2 m wide as
        // the listener walks. Floors spread (the larger of the two wins).
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_size(IntPtr e, uint s, float radiusM);

        // ---- directivity (works in EVERY build: with a Steam scene the sim evaluates it; without one
        // the audio thread evaluates the same weighted dipole per block — walk-correct either way) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_orientation(IntPtr e, uint s, float qx, float qy, float qz, float qw);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_directivity(IntPtr e, uint s, float weight, float power);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_directivity_preset(IntPtr e, uint s, BwaDirectivity pattern);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bwa_source_get_directivity(IntPtr e, uint s);

        // ---- channel test / diagnostics (drives a raw output channel; speaker-check tool) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_test_signal(IntPtr e, uint channel, BwaTestKind kind, float gain);
        // The engine's active channel count = the layout's speaker count (4..26). Size meter/speaker
        // arrays with this; never hard-code 26 (that is only the compile-time capacity).
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_channel_count(IntPtr e);
        // The DLL's packed BWA_VERSION (major<<16 | minor<<8 | patch). Engine.Awake compares it against
        // BoundVersion below and refuses to start on a major.minor mismatch.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_version();
        // The BWA_VERSION these bindings were written against (bw_audio.h). The header guarantees enum
        // values and struct layouts only WITHIN a major.minor, so a DLL with a different major.minor may
        // marshal every struct in this file wrong — silent corruption, not a crash. Bump this alongside
        // any re-sync with a header whose BWA_VERSION moved.
        public const uint BoundVersion = (0u << 16) | (12u << 8) | 0u;   // 0.12.0

        /// <summary>A packed BWA_VERSION as "major.minor.patch", for logs.</summary>
        public static string VersionString(uint v) => (v >> 16) + "." + ((v >> 8) & 0xFF) + "." + (v & 0xFF);
        // Resolved engine config (zero-defaulted desc fields resolved at create) — derive seconds from these.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_sample_rate(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_block_size(IntPtr e);
        // The sink actually running (Auto resolved to Asio/Null once started) — the enum side of bwa_get_audio_backend.
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaSinkType bwa_get_sink_type(IntPtr e);
        // Read back the effective speaker layout (xyz = cap*3 floats, x,y,z per speaker); returns the count
        // FILLED (min(cap, count) — the bwa_get_bus_levels convention); xyz = null returns the total count.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_speakers(IntPtr e, [Out] float[] xyz, uint cap);
        // Per-channel output meter: last-block peak |sample| per channel (linear), as sent to the device; returns the count filled.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_bus_levels(IntPtr e, [Out] float[] peaks, uint cap);
        // Last block's ACTIVE voice count (a voice-pool gauge for a HUD next to the meters); 0 until audio runs.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_active_voices(IntPtr e);
        // Panner selection (load-time, or live — the switch is atomic): DBAP (moving observer, default),
        // SPCAP or VBAP (both fixed-observer sweet-spot panners).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_panner(IntPtr e, BwaPanner panner);
        // SPCAP's two tuning exponents (inert under DBAP/VBAP; live). focus = lobe sharpness: higher
        // concentrates a source on fewer speakers, lower spreads it. density = the placement-correction
        // kernel exponent (2 is the default and rarely worth moving). Pass <= 0 for either to revert THAT
        // one to its default — focus falls back to a value derived from the array geometry.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_spcap_focus(IntPtr e, float focus, float density);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_dual_band(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Compensated amplitude panning on that low band. REQUIRES dual band (the low band is the only
        // thing it touches). Constrains the interaural component of the summed field to a real source's,
        // using the tracked head ORIENTATION, so the image holds still as the listener turns. A no-op
        // facing the source, and it fades out with source spread.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_dual_band_cap(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // How bwa_source_set_spread renders width (live A/B): LOBE (default, one reshaped solve), MDAP
        // (a ring of virtual sources panned with the selected panner — panner-true, ~13x the solve cost),
        // or SPECTRAL (frequency-dependent panning: 6 bands, each to its own direction in the cone — width
        // with no coherent copies to collapse or comb-filter; the decorrelation alternative).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_spread_mode(IntPtr e, BwaSpreadMode mode);
        // max-rE weighting for the SH->speaker BED decode (live A/B, crossfaded): tapers the high orders —
        // fewer decode sidelobes, better localization away from the sweet spot. Reaches the bed matrix
        // renderer and the FDN's line render; point-source panners and phonon's decodes are untouched.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_max_re(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Band-split max-rE (needs bwa_set_max_re on; live A/B): apply the taper only ABOVE the ~700 Hz
        // crossover and keep the unweighted (rV-optimal) decode below — the ear localizes LF by pressure, HF
        // by energy (the Gerzon basic-LF/max-rE-HF split). Bed matrix decodes only; the FDN stays broadband.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_max_re_split(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Decorrelate the WIDE part of spread sources (live A/B): per-speaker velvet-noise filters make the
        // copies mutually incoherent — real extent, no phantom collapse or comb-filtering as the listener
        // walks. Point sources (spread 0) are untouched.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_decorrelation(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Near-listener widening (0 = off): floor every source's spread at 1 - dist/radius, so a source
        // flying at the head widens instead of snapping across the nearest speaker. ~1.0 m is a good start.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_near_spread(IntPtr e, float radiusM);
        // Hole-aware spread floor (0 = off): floor a source's spread by how far its bearing sits from the
        // nearest speaker, so a source aimed where the array has no speaker (the CAVE barrel's open poles)
        // renders as an honest wide source instead of a split image across two distant speakers. Zero until
        // the gap exceeds the array's own mean speaker spacing, so a surrounding array is inert. 1.0 = the
        // honest width; clamped at 2.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_hole_spread(IntPtr e, float strength);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_limiter(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_limiter_ceiling(IntPtr e, float linear);   // linear peak ceiling in (0..1]; default 0.891251f (-1 dBFS)
        // Headphone correction EQ (the headphone-side align stage): parse an AutoEq ParametricEQ.txt
        // into a biquad cascade on the final stereo of every headphone profile (binaural/cave_sim/
        // cave_both's tap; inert in cave). Load-class (file I/O, may block — not per-frame); a parse
        // failure returns ErrConfig, keeps the previous EQ, and bwa_last_error has the reason.
        // null/"" clears. The bool is the ramped live A/B (default on: loading engages).
        [DllImport(DLL, CallingConvention = CC)] public static extern BwaResult bwa_load_headphone_eq(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_headphone_eq(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // One ramped scalar over the whole mix, applied BEFORE the per-speaker align stage (trims + the raw
        // channel-test signal stay calibrated) and before the limiter. The volume knob / scene fade. Live.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_master_gain(IntPtr e, float linear);
        // How ambisonic BEDS render (live; each bed crossfades — a click-free A/B): MATRIX (default, the
        // static SH->speaker decode) or PARAMETRIC (DirAC: the directional stream is re-panned through the
        // listener-relative panner, so a recorded soundfield becomes WALKABLE — parallax off-center).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_bed_renderer(IntPtr e, BwaBedRenderer renderer);
        // Tracked room EQ (layouts carrying a room_eq_grid): the LF modal cuts follow the live listener
        // position. ON by default when a grid is present; this is the live kill switch. No-op without a grid.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_tracked_room_eq(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Tracked listener alignment (OFF by default). The layout's per-speaker delay/gain trims align the
        // array at ONE point (the array centroid); this re-references them onto the TRACKED listener, adding
        // each speaker's extra propagation delay and 1/r level for |speaker - listener| vs |speaker - centroid|.
        // Opt-in because a moving delay line resamples: a walking listener glides every speaker's delay at
        // once, which is a Doppler shift on the whole array. deadZoneM (default 0.05) is how far the head must
        // move before anything is recomputed; slewFramesPerS (default ~63 at 48 kHz, which follows a 0.45 m/s
        // walk) caps how fast a delay may change, so a faster listener LAGS instead of warbling. Either <= 0
        // takes the default. Live A/B; off glides back to the layout's own trims.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_tracked_align(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_tracked_align_guards(IntPtr e, float deadZoneM, float slewFramesPerS);

        // Situation tuning. Fill a BwaTuning from a preset, edit what you disagree with, apply it.
        // NEVER apply a default(BwaTuning): this struct's zero is not its default (it would force
        // max-rE off), which is exactly what structSize makes fail loudly.
        public enum BwaSetup : int { Default = 0, Seated = 1, Roaming = 2 }

        [StructLayout(LayoutKind.Sequential)]
        public struct BwaTuning
        {
            public uint structSize;
            public BwaPanner panner;
            public float spcapFocus, spcapDensity;
            [MarshalAs(UnmanagedType.I1)] public bool dualBand;
            [MarshalAs(UnmanagedType.I1)] public bool dualBandCap;
            public BwaSpreadMode spreadMode;
            [MarshalAs(UnmanagedType.I1)] public bool decorrelation;
            public float nearSpread, holeSpread;
            [MarshalAs(UnmanagedType.I1)] public bool maxRe;
            [MarshalAs(UnmanagedType.I1)] public bool maxReSplit;
            public BwaBedRenderer bedRenderer;
            [MarshalAs(UnmanagedType.I1)] public bool trackedRoomEq;
            [MarshalAs(UnmanagedType.I1)] public bool trackedAlign;
            public float alignDeadZoneM, alignSlewFramesPerSecond;
            public uint r0, r1, r2, r3;
        }

        // Source configuration. Same fill-then-apply shape as the engine tuning above, for a SOURCE:
        // fill a BwaSourceDesc from a kind's preset, edit what you disagree with, apply it. NEVER apply a
        // default(BwaSourceDesc) — its zero is not its default, which is what structSize makes fail loudly.
        // The preset call is PURE (no engine handle), so it works in edit mode with nothing running.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_preset(BwaSourceKind kind, out BwaSourceDesc outDesc);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_source_create_desc(IntPtr e, in BwaSourceDesc d);
        // ONE ring command for the fifteen knobs the audio thread owns. Out-of-range FINITE values clamp
        // exactly as the individual setters clamp them; NaN/Inf refuses the whole apply (false + an error).
        // A stale handle is the usual silent no-op and still returns TRUE — the desc was valid, the source
        // was not. earlyReflections with no room set leaves them off, says so in bwa_last_error, and still
        // returns true (the rest of the configuration landed).
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool bwa_source_apply(IntPtr e, uint s, in BwaSourceDesc d);
        // What the source is SET to (not what the sim is doing to it — that is bwa_source_get_occlusion /
        // bwa_source_get_directivity). Fills structSize, so a readback hands straight back to apply.
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool bwa_source_get_desc(IntPtr e, uint s, out BwaSourceDesc outDesc);

        /// <summary>A complete BwaSourceDesc for `kind` (with structSize filled). Pure: no engine, so it
        /// works in edit mode. Start every desc here — the struct's zero is not its default.</summary>
        public static BwaSourceDesc SourcePreset(BwaSourceKind kind)
        {
            bwa_source_preset(kind, out var d);
            return d;
        }

        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_tuning_preset(BwaSetup setup, out BwaTuning outTuning);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool bwa_get_tuning(IntPtr e, out BwaTuning outTuning);
        // Completion as an EVENT. Drains handles whose voices ended; prefer it over edge-detecting
        // bwa_source_is_playing, which misses any sound shorter than your frame interval.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_poll_ended(IntPtr e, [Out] uint[] outHandles, uint cap, out ulong dropped);
        // The ISM shoebox WITHOUT replacing the static mesh, and the box's own triangles, so the
        // box can be composed with your geometry instead of replacing it.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_ism_room(IntPtr e, float w, float h, float d, uint[] faces);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool bwa_box_mesh(float w, float h, float d, uint[] faces, [Out] float[] outVerts, [Out] int[] outTris, [Out] uint[] outTriMaterial);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool bwa_apply_tuning(IntPtr e, ref BwaTuning t);
        // Offline panner evaluation (no engine handle): out = nsrc*n gains for a layout/panner; for layout scoring.
        // focus/density are SPCAP's tuning knobs, same <= 0 = default sentinel as bwa_set_spcap_focus, and
        // inert under DBAP/VBAP. Pass 0, 0 to score at the focus this array's geometry derives.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_panner_gains_batch(BwaPanner panner, float[] positions, uint n, float[] lis, float[] srcs, uint nsrc, float focus, float density, [Out] float[] outGains);
        // The pure companion of bwa_set_spcap_focus (no engine handle): the focus its <= 0 sentinel
        // reverts to, derived from n speaker positions (3 floats each) — so a tool can show what an
        // in-progress layout implies before you override it. Returns 0 on bad arguments.
        [DllImport(DLL, CallingConvention = CC)] public static extern float bwa_spcap_focus_default(float[] positions, uint n);
        // The BED counterpart of the panner batch: per-speaker gains of the diffuse-bed decode (the
        // engine's real AllRAD/EPAD builds, optional max-rE) for ndir plane-wave DIRECTIONS (unit
        // vectors — a bed is content at infinity) over a layout of n speaker positions. outGains =
        // ndir*n floats (gains may be negative — SH sidelobes); returns ndir. Pure and reentrant.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_bed_gains_batch(BwaBedDecoder decoder, [MarshalAs(UnmanagedType.I1)] bool maxRe, float[] positions, uint n, float[] dirs, uint ndir, [Out] float[] outGains);

        // ---- listener + frame boundary ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_listener_pose(IntPtr e, float px, float py, float pz, float qx, float qy, float qz, float qw);
        // The engine writes p[0..2] and q[0..3] UNCONDITIONALLY — p must be length>=3, q length>=4, both
        // non-null, or the native write corrupts memory. Prefer the GetListenerPose helper below.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_get_listener_pose(IntPtr e, [Out] float[] p, [Out] float[] q);
        // Pose prediction (internal tracking only; 0 = off): lead the TRACKED position by `leadS` SECONDS along
        // the tracker's own velocity, hiding the motion-to-ears latency. Set it to your MEASURED latency —
        // too much lead overshoots on direction changes (clamped at 0.2 s). No effect when Unity feeds
        // the pose (bwa_set_listener_pose): predict on your side instead.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_pose_prediction(IntPtr e, float leadS);
        // Extra listeners (multi-occupant compromise panning; up to 3, count 0 = off/single-listener). `xyz`
        // is count*3 floats in ROOM space — the OTHER occupants; the primary stays bwa_set_listener_pose /
        // tracking. Every source's gains become the per-speaker energy mean of the per-listener solves.
        // Commit-gated like the pose (push it in the same frame block, before bwa_commit).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_extra_listeners(IntPtr e, float[] xyz, uint count);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_commit(IntPtr e);

        // ---- convenience ----
        /// <summary>Human-readable last error (null if clean). Does not take ownership of the pointer.</summary>
        public static string LastError(IntPtr e)
        {
            var p = bwa_last_error(e);
            return p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
        }

        /// <summary>Backend in use after bwa_start: "asio:&lt;driver&gt;", "null", or "none".</summary>
        public static string Backend(IntPtr e)
        {
            var p = bwa_get_audio_backend(e);
            return p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
        }

        /// <summary>Registered ASIO driver `index`'s name (the exact string BwaDesc.asioDriver expects), or
        /// null if the index is out of range. Engine-free — call it before bwa_create (with
        /// bwa_get_asio_driver_count) to build a driver picker.</summary>
        public static string AsioDriverName(uint index)
        {
            var buf = new byte[256];
            if (!bwa_get_asio_driver_name(index, buf, (uint)buf.Length)) return null;
            int len = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, len < 0 ? buf.Length : len);
        }

        /// <summary>Read the listener pose the engine is rendering with. Always allocates the correct
        /// sizes (position[3], orientation xyzw[4]) — the raw bwa_get_listener_pose writes those slots
        /// unconditionally, so a wrong-sized array would corrupt memory.</summary>
        public static void GetListenerPose(IntPtr e, out float[] position, out float[] orientation)
        {
            position = new float[3];
            orientation = new float[4];
            bwa_get_listener_pose(e, position, orientation);
        }
    }
}
