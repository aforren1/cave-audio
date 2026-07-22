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
    public enum BwaProfile : int { Cave = 0, Binaural = 1, Both = 2 }
    public enum BwaDirectivity : int { Omni = 0, Cardioid = 1, Figure8 = 2 }
    public enum BwaTestKind : int { Off = 0, Sine = 1, Noise = 2 }
    public enum BwaPanner : int { Dbap = 0, Spcap = 1, Vbap = 2 }
    public enum BwaBedDecoder : int { Allrad = 0, Epad = 1 }   // sampling is no longer selectable (internal fallback)
    public enum BwaSpreadMode : int { Lobe = 0, Mdap = 1, Spectral = 2 }
    public enum BwaBedRenderer : int { Matrix = 0, Parametric = 1 }

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
        public BwaBedDecoder bedDecoder;                                 // diffuse-bed decoder; 0 = AllRAD (default)
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

    /// <summary>Raw P/Invoke entry points. 1:1 with include/bw_audio.h.</summary>
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
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_output_latency(IntPtr e);
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
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_source_get_playhead(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);
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
        // Yaw the soundfield about the room's vertical axis, in RADIANS, in the ROOM frame (positive turns
        // the field from room +z toward room +x). Unity's yaw is the opposite sense — convert with
        // Room.YawRad, do not pass a Unity euler angle straight in. Glides (~1 turn/s), click-free.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_bed_set_rotation(IntPtr e, uint b, float yawRad);
        // Full 3-axis orientation, RADIANS, ROOM frame (level or tilt a capture; supersedes set_rotation,
        // which is the yaw shorthand and resets pitch/roll). Room senses: positive pitch tilts the field's
        // front upward, positive roll tilts its top toward room -x (the room's right). Across the seam:
        // yaw needs Room.YawRad (the X mirror reverses it); pitch and roll pass through with the SAME
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
        [DllImport(DLL, CallingConvention = CC)] public static extern ulong bwa_bed_get_playhead(IntPtr e, uint b);

        // ---- materials / scene geometry (load time; needs the Steam Audio build) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_material_preset(IntPtr e, BwaMaterialPreset preset);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_material_define(IntPtr e, float[] absorption, float scattering, float[] transmission);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_material_release(IntPtr e, uint token);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_mesh_mat(IntPtr e, float[] verts, int nverts, int[] tris, int ntris, uint[] triMaterial);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_scene_set_box(IntPtr e, float w, float h, float d, uint[] faces);
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
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_reverb_set_gain(IntPtr e, float linear);
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

        // ---- image-source EARLY reflections (per source; no SDK needed) ----
        // The other half of the phonon-free acoustics path: the FDN renders the late diffuse tail, this
        // renders the six first-order wall bounces — the ones that carry room size and source distance.
        // Each is a real POINT SOURCE at its mirrored position, panned through the engine's own
        // listener-relative panner, so reflections keep correct direction AND parallax as the listener
        // walks — which no shared reverb bed can do. Needs the room: call bwa_scene_set_box first.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_source_set_early_reflections(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_early_reflections_set_gain(IntPtr e, float linear);   // default 1; live

        // ---- propagation effects (no SDK needed; opt-in per source, default off) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_doppler(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_air_absorption(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        // Equal-loudness distance compensation: an LF shelf tracking the panner's attenuation ("far, not
        // tinny"). A perceptual stylization, not physics — leave off for strict realism.
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_loudness_comp(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_spread(IntPtr e, uint s, float amount);
        // Source size in METRES (radius; 0 = point). The physical alternative to the angular spread above:
        // the width is the angle the radius subtends from the listener, so a 2 m source STAYS 2 m wide as
        // the listener walks. Floors spread (the larger of the two wins).
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_size(IntPtr e, uint s, float radiusM);

        // ---- directivity ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_orientation(IntPtr e, uint s, float qx, float qy, float qz, float qw);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_directivity(IntPtr e, uint s, float weight, float power);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bwa_source_set_directivity_preset(IntPtr e, uint s, BwaDirectivity pattern);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bwa_source_get_directivity(IntPtr e, uint s);

        // ---- channel test / diagnostics (drives a raw output channel; speaker-check tool) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_test_signal(IntPtr e, uint channel, BwaTestKind kind, float gain);
        // The engine's active channel count = the layout's speaker count (4..26). Size meter/speaker
        // arrays with this; never hard-code 26 (that is only the compile-time capacity).
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_channel_count(IntPtr e);
        // The DLL's packed BWA_VERSION (major<<16 | minor<<8 | patch) — verify against the bound header rev.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_get_version();
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
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_dual_band(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // How bwa_source_set_spread renders width (live A/B): LOBE (default, one reshaped solve), MDAP
        // (a ring of virtual sources panned with the selected panner — panner-true, ~13x the solve cost),
        // or SPECTRAL (frequency-dependent panning: 6 bands, each to its own direction in the cone — width
        // with no coherent copies to collapse or comb-filter; the decorrelation alternative).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_spread_mode(IntPtr e, BwaSpreadMode mode);
        // max-rE weighting for the SH->speaker BED decode (live A/B, crossfaded): tapers the high orders —
        // fewer decode sidelobes, better localization away from the sweet spot. Reaches the bed matrix
        // renderer and the FDN's line render; point-source panners and phonon's decodes are untouched.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_max_re(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Decorrelate the WIDE part of spread sources (live A/B): per-speaker velvet-noise filters make the
        // copies mutually incoherent — real extent, no phantom collapse or comb-filtering as the listener
        // walks. Point sources (spread 0) are untouched.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_decorrelation(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Near-listener widening (0 = off): floor every source's spread at 1 - dist/radius, so a source
        // flying at the head widens instead of snapping across the nearest speaker. ~1.0 m is a good start.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_near_spread(IntPtr e, float radiusM);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_limiter(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_limiter_ceiling(IntPtr e, float ceilingDb);   // default -1 dBFS; clamped [-60, 0]
        // One ramped scalar over the whole mix, applied BEFORE the per-speaker align stage (trims + the raw
        // channel-test signal stay calibrated) and before the limiter. The volume knob / scene fade. Live.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_master_gain(IntPtr e, float linear);
        // How ambisonic BEDS render (live; each bed crossfades — a click-free A/B): MATRIX (default, the
        // static SH->speaker decode) or PARAMETRIC (DirAC: the directional stream is re-panned through the
        // listener-relative panner, so a recorded soundfield becomes WALKABLE — parallax off-centre).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_bed_renderer(IntPtr e, BwaBedRenderer renderer);
        // Tracked room EQ (layouts carrying a room_eq_grid): the LF modal cuts follow the live listener
        // position. ON by default when a grid is present; this is the live kill switch. No-op without a grid.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_tracked_room_eq(IntPtr e, [MarshalAs(UnmanagedType.I1)] bool on);
        // Offline panner evaluation (no engine handle): out = nsrc*n gains for a layout/panner; for layout scoring.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bwa_panner_gains_batch(BwaPanner panner, float[] positions, uint n, float[] lis, float[] srcs, uint nsrc, [Out] float[] outGains);

        // ---- listener + frame boundary ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_listener_pose(IntPtr e, float px, float py, float pz, float qx, float qy, float qz, float qw);
        // The engine writes p[0..2] and q[0..3] UNCONDITIONALLY — p must be length>=3, q length>=4, both
        // non-null, or the native write corrupts memory. Prefer the GetListenerPose helper below.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_get_listener_pose(IntPtr e, [Out] float[] p, [Out] float[] q);
        // Pose prediction (internal tracking only; 0 = off): lead the TRACKED position by `leadMs` along the
        // tracker's own velocity, hiding the motion-to-ears latency. Set it to your MEASURED latency —
        // too much lead overshoots on direction changes (clamped at 200 ms). No effect when Unity feeds
        // the pose (bwa_set_listener_pose): predict on your side instead.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_pose_prediction(IntPtr e, float leadMs);
        // Extra listeners (multi-occupant compromise panning; up to 3, count 0 = off/single-listener). `xyz`
        // is count*3 floats in ROOM space — the OTHER occupants; the primary stays bwa_set_listener_pose /
        // tracking. Every source's gains become the per-speaker energy mean of the per-listener solves.
        // Commit-gated like the pose (push it in the same frame block, before bwa_commit).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_set_extra_listeners(IntPtr e, float[] xyz, uint count);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bwa_commit(IntPtr e);

        // ---- convenience ----
        /// <summary>Mint a built-in material by preset (the extern is already typed — bwa_material_preset
        /// takes the enum natively; this alias just reads better at call sites).</summary>
        public static uint MaterialPreset(IntPtr e, BwaMaterialPreset preset)
            => bwa_material_preset(e, preset);

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
