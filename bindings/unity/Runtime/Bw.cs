// Bw.cs — P/Invoke binding for the bwaudio C ABI (include/bwaudio.h).
//
// Pure marshalling layer, no Unity dependency (so it compiles + can be unit-tested standalone; the
// MonoBehaviour wrappers live in BwAudio.cs / BwEmitter.cs). Drop bwaudio.dll + phonon.dll in
// Assets/Plugins/. THREADING: every call must come from ONE thread (Unity's main thread); the
// per-frame calls are non-blocking. See docs/api.md + docs/concurrency.md.
//
// Marshalling rules that matter here:
//   * C `bool` is 1 byte  -> [MarshalAs(UnmanagedType.I1)].
//   * C `const char*`     -> [MarshalAs(UnmanagedType.LPUTF8Str)] for IN strings;
//                            IntPtr + Marshal.PtrToStringUTF8 for RETURNED strings (do not let the
//                            marshaller free a pointer the engine still owns).
//   * BwConfig.profile is a 4-byte enum, NOT a string — marshalling it as a string corrupts the call.
//   * Handles (BwSound/BwSource/BwBed/BwMaterial) are uint = (index | generation<<16); 0 = invalid.
using System;
using System.Runtime.InteropServices;

namespace CaveAudio
{
    public enum BwProfile : int { Cave = 0, Binaural = 1, Both = 2 }
    public enum BwDirectivity : int { Omni = 0, Cardioid = 1, Figure8 = 2 }
    public enum BwTestKind : int { Off = 0, Sine = 1, Noise = 2 }
    public enum BwPanner : int { Dbap = 0, Spcap = 1, Vbap = 2 }
    public enum BwBedDecoder : int { Sampling = 0, Allrad = 1 }
    public enum BwSpreadMode : int { Lobe = 0, Mdap = 1 }
    public enum BwBedRenderer : int { Matrix = 0, Parametric = 1 }

    /// <summary>The engine's built-in acoustic materials (BW_PRESETS in engine.c, same order). This is
    /// an ENUM rather than the raw string bw_material_preset takes, because an unrecognised name is not
    /// an error there — it quietly returns material 0 (the generic default) and leaves the reason in
    /// bw_last_error, so a typo'd "concreet" wall just sounds wrong instead of failing.</summary>
    public enum BwMaterialPreset : int
    {
        Generic = 0, Brick, Concrete, Ceramic, Gravel, Carpet, Glass, Plaster, Wood, Metal, Rock
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BwConfig
    {
        public BwProfile profile;                                       // 4-byte int enum
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string layoutPath;  // const char* (or null)
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string hrtfPath;    // const char* (or null)
        public uint sampleRate;                                         // 48000
        public uint blockSize;                                          // e.g. 256
        [MarshalAs(UnmanagedType.I1)] public bool trackInternal;        // true => core reads NatNet itself
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BwReflectionConfig
    {
        public float irSeconds;      // reverb tail length; 0 => default 1.0
        public uint  order;          // ambisonic order 1 or 2; 0 => default 1
        public uint  numRays;        // off-thread ray budget; 0 => default 4096
        public uint  numBounces;     // off-thread bounce depth; 0 => default 16
        public int   enabled;        // 0 => no bed created
        public float wetGain;        // reverb level summed onto the bus; 0 => default 1.0
        public uint  reserved0, reserved1, reserved2;   // matches reserved[3]; keep zero
    }

    /// <summary>Raw P/Invoke entry points. 1:1 with include/bwaudio.h.</summary>
    public static class Bw
    {
        const string DLL = "bwaudio";
        const CallingConvention CC = CallingConvention.Cdecl;

        // ---- lifecycle (load time; may block/allocate) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bw_create(in BwConfig cfg);
        [DllImport(DLL, CallingConvention = CC)] public static extern int    bw_start(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern int    bw_stop(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void   bw_destroy(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bw_last_error(IntPtr e);     // PtrToStringUTF8; null = none
        [DllImport(DLL, CallingConvention = CC)] public static extern IntPtr bw_audio_backend(IntPtr e);  // "asio:<drv>" / "null" / "none"

        // ---- assets (load time; file I/O) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_load_sound(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_load_sound_streaming(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_unload_sound(IntPtr e, uint snd);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_load_ambix(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

        // ---- sources (per-frame; non-blocking) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_source_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_priority(IntPtr e, uint s, int priority);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_destroy(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_pos(IntPtr e, uint s, float x, float y, float z);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_gain(IntPtr e, uint s, float linear);
        // Engine-side timed fades (no per-frame scripting). fade_out lands on the click-free stop path.
        // A later set_gain or fade replaces a running one.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_fade_to(IntPtr e, uint s, float gain, float seconds);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_fade_out(IntPtr e, uint s, float seconds);
        // Mix groups (0..7; sources start in group 0): a group gain folds into every member's gain solve
        // and a paused group freezes its members exactly like per-voice pause.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_group(IntPtr e, uint s, uint group);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_group_set_gain(IntPtr e, uint group, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_group_set_paused(IntPtr e, uint group, [MarshalAs(UnmanagedType.I1)] bool paused);
        // Playback rate, clamped [0.25, 4] (glides across a block). In-memory sounds only — streams ignore it.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_pitch(IntPtr e, uint s, float rate);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_play(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_play_at(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop, ulong startSample);
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bw_dsp_time(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_stop(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_paused(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool paused);
        // Global pause: EVERY voice (memory, streamed, bed) ramps out and freezes; resume continues exactly.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_paused(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool paused);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_seek(IntPtr e, uint s, ulong frame);   // engine-rate frames; in-memory sounds
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bw_source_is_playing(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);

        // ---- ambisonic beds (world-locked diffuse soundfields) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_bed_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_play(IntPtr e, uint b, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_set_gain(IntPtr e, uint b, float linear);
        // Yaw the soundfield about the room's vertical axis, in RADIANS, in the ROOM frame (positive turns
        // the field from room +z toward room +x). Unity's yaw is the opposite sense — convert with
        // Room.YawRad, do not pass a Unity euler angle straight in. Glides (~1 turn/s), click-free.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_set_rotation(IntPtr e, uint b, float yawRad);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_stop(IntPtr e, uint b);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_destroy(IntPtr e, uint b);

        // ---- materials / scene geometry (load time; needs the Steam Audio build) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_scene_set_mesh(IntPtr e, float[] verts, int nverts, int[] tris, int ntris, float[] absorption, float scattering, float[] transmission);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_material_preset(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_material_define(IntPtr e, float[] absorption, float scattering, float[] transmission);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_scene_set_mesh_mat(IntPtr e, float[] verts, int nverts, int[] tris, int ntris, uint[] triMaterial);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_scene_set_box(IntPtr e, float w, float h, float d, uint[] faces);

        // ---- occlusion (per-frame setters; readback any time) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_occlusion(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // MANUAL occlusion (no SDK needed): drive the sim's own publish path from game logic. `level` is
        // broadband transmittance (1 = clear .. 0 = blocked); `bands` is an optional low/mid/high tilt in
        // [0,1] rendered as the same 3-biquad transmission EQ (so a wall MUFFLES, not just attenuates) —
        // pass null (marshals to NULL) for broadband only. Do NOT also enable bw_source_set_occlusion on
        // the same source: the sim republishes every tick and wins.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_occlusion_manual(IntPtr e, uint s, float level, float[] bands);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bw_source_get_occlusion(IntPtr e, uint s);

        // ---- reflection bed ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_reflections_config(IntPtr e, in BwReflectionConfig cfg);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_reflections_set_gain(IntPtr e, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_reflections(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_reflection_send(IntPtr e, uint s, float gain);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_reflection_distance(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_pathing(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);

        // ---- directional FDN reverb bed (load-time; works WITHOUT the Steam Audio build) ----
        // Takes the reverb tap INSTEAD of the Steam reflection bed (one bed at a time), fed by the same
        // per-source send (bw_source_set_reflections + send levels apply unchanged). All three are
        // load-time (between bw_create and bw_start). Decay is a DESIGN parameter — do not copy the room's
        // measured RT60; the real room adds its own on top.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_reverb_fdn(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_fdn_set_decay(IntPtr e, float rt60LowS, float rt60HighS, float xoverHz);
        // Anisotropic decay: scale the decay time toward `dir` (3 floats, room space) by `factor`
        // (< 1 = the field dies faster that way — an open or treated side).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_fdn_set_decay_direction(IntPtr e, float[] dir, float factor);

        // ---- image-source EARLY reflections (per source; no SDK needed) ----
        // The other half of the phonon-free acoustics path: the FDN renders the late diffuse tail, this
        // renders the six first-order wall bounces — the ones that carry room size and source distance.
        // Each is a real POINT SOURCE at its mirrored position, panned through the engine's own
        // listener-relative panner, so reflections keep correct direction AND parallax as the listener
        // walks — which no shared reverb bed can do. Needs the room: call bw_scene_set_box first.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_early_reflections(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_early_reflections_set_gain(IntPtr e, float linear);   // default 1; live

        // ---- propagation effects (no SDK needed; opt-in per source, default off) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_doppler(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_air_absorption(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // Equal-loudness distance compensation: an LF shelf tracking the panner's attenuation ("far, not
        // tinny"). A perceptual stylization, not physics — leave off for strict realism.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_loudness_comp(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_spread(IntPtr e, uint s, float amount);
        // Source size in METRES (radius; 0 = point). The physical alternative to the angular spread above:
        // the width is the angle the radius subtends from the listener, so a 2 m source STAYS 2 m wide as
        // the listener walks. Floors spread (the larger of the two wins).
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_size(IntPtr e, uint s, float radiusM);

        // ---- directivity ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_orientation(IntPtr e, uint s, float qx, float qy, float qz, float qw);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_directivity(IntPtr e, uint s, float weight, float power);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_directivity_preset(IntPtr e, uint s, BwDirectivity pattern);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bw_source_get_directivity(IntPtr e, uint s);

        // ---- channel test / diagnostics (drives a raw output channel; speaker-check tool) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_test_signal(IntPtr e, uint channel, BwTestKind kind, float gain);
        // The engine's active channel count = the layout's speaker count (4..26). Size meter/speaker
        // arrays with this; never hard-code 26 (that is only the compile-time capacity).
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_channel_count(IntPtr e);
        // Read back the effective speaker layout (xyz = cap*3 floats, x,y,z per speaker); returns the count.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_get_speakers(IntPtr e, [Out] float[] xyz, uint cap);
        // Per-channel output meter: last-block peak |sample| per channel (linear), as sent to the device; returns the count filled.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_get_bus_levels(IntPtr e, [Out] float[] peaks, uint cap);
        // Last block's ACTIVE voice count (a voice-pool gauge for a HUD next to the meters); 0 until audio runs.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_get_active_voices(IntPtr e);
        // Panner selection (load-time, or live — the switch is atomic): DBAP (moving observer, default),
        // SPCAP or VBAP (both fixed-observer sweet-spot panners).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_panner(IntPtr e, BwPanner panner);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_dual_band(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // How bw_source_set_spread renders width (live A/B): LOBE (default, one reshaped solve) or MDAP
        // (a ring of virtual sources panned with the selected panner — panner-true, ~13x the solve cost).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_spread_mode(IntPtr e, BwSpreadMode mode);
        // Decorrelate the WIDE part of spread sources (live A/B): per-speaker velvet-noise filters make the
        // copies mutually incoherent — real extent, no phantom collapse or comb-filtering as the listener
        // walks. Point sources (spread 0) are untouched.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_decorrelation(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Near-listener widening (0 = off): floor every source's spread at 1 - dist/radius, so a source
        // flying at the head widens instead of snapping across the nearest speaker. ~1.0 m is a good start.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_near_spread(IntPtr e, float radiusM);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_limiter(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_limiter_ceiling(IntPtr e, float ceilingDb);   // default -1 dBFS; clamped [-60, 0]
        // One ramped scalar over the whole mix, applied BEFORE the per-speaker align stage (trims + the raw
        // channel-test signal stay calibrated) and before the limiter. The volume knob / scene fade. Live.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_master_gain(IntPtr e, float linear);
        // Diffuse-bed decoder (load-time): sampling (default) or AllRAD (robust on irregular arrays).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_bed_decoder(IntPtr e, BwBedDecoder decoder);
        // How ambisonic BEDS render (live; each bed crossfades — a click-free A/B): MATRIX (default, the
        // static SH->speaker decode) or PARAMETRIC (DirAC: the directional stream is re-panned through the
        // listener-relative panner, so a recorded soundfield becomes WALKABLE — parallax off-centre).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_bed_renderer(IntPtr e, BwBedRenderer renderer);
        // Tracked room EQ (layouts carrying a room_eq_grid): the LF modal cuts follow the live listener
        // position. ON by default when a grid is present; this is the live kill switch. No-op without a grid.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_tracked_room_eq(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Offline panner evaluation (no engine handle): out = nsrc*n gains for a layout/panner; for layout scoring.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_panner_gains_batch(BwPanner panner, float[] positions, uint n, float[] lis, float[] srcs, uint nsrc, [Out] float[] outGains);

        // ---- listener + frame boundary ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_listener_pose(IntPtr e, float px, float py, float pz, float qx, float qy, float qz, float qw);
        // The engine writes p[0..2] and q[0..3] UNCONDITIONALLY — p must be length>=3, q length>=4, both
        // non-null, or the native write corrupts memory. Prefer the GetListenerPose helper below.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_get_listener_pose(IntPtr e, [Out] float[] p, [Out] float[] q);
        // Pose prediction (track_internal only; 0 = off): lead the TRACKED position by `leadMs` along the
        // tracker's own velocity, hiding the motion-to-ears latency. Set it to your MEASURED latency —
        // too much lead overshoots on direction changes (clamped at 200 ms). No effect when Unity feeds
        // the pose (bw_set_listener_pose): predict on your side instead.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_pose_prediction(IntPtr e, float leadMs);
        // Extra listeners (multi-occupant compromise panning; up to 3, count 0 = off/single-listener). `xyz`
        // is count*3 floats in ROOM space — the OTHER occupants; the primary stays bw_set_listener_pose /
        // tracking. Every source's gains become the per-speaker energy mean of the per-listener solves.
        // Commit-gated like the pose (push it in the same frame block, before bw_commit).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_extra_listeners(IntPtr e, float[] xyz, uint count);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_commit(IntPtr e);

        // ---- convenience ----
        /// <summary>Mint a built-in material by preset. The typed form of bw_material_preset — the enum
        /// name IS the engine's name (lowercased), so a preset can't be misspelled into silently becoming
        /// the generic default.</summary>
        public static uint MaterialPreset(IntPtr e, BwMaterialPreset preset)
            => bw_material_preset(e, PresetName(preset));

        /// <summary>The engine-side name for a preset ("concrete", ...). Case-insensitive engine-side.</summary>
        public static string PresetName(BwMaterialPreset preset) => preset.ToString().ToLowerInvariant();

        /// <summary>Human-readable last error (null if clean). Does not take ownership of the pointer.</summary>
        public static string LastError(IntPtr e)
        {
            var p = bw_last_error(e);
            return p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
        }

        /// <summary>Backend in use after bw_start: "asio:&lt;driver&gt;", "null", or "none".</summary>
        public static string Backend(IntPtr e)
        {
            var p = bw_audio_backend(e);
            return p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
        }

        /// <summary>Read the listener pose the engine is rendering with. Always allocates the correct
        /// sizes (position[3], orientation xyzw[4]) — the raw bw_get_listener_pose writes those slots
        /// unconditionally, so a wrong-sized array would corrupt memory.</summary>
        public static void GetListenerPose(IntPtr e, out float[] position, out float[] orientation)
        {
            position = new float[3];
            orientation = new float[4];
            bw_get_listener_pose(e, position, orientation);
        }
    }
}
