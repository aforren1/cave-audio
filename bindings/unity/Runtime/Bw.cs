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
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_unload_sound(IntPtr e, uint snd);
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_load_ambix(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

        // ---- sources (per-frame; non-blocking) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_source_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_destroy(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_pos(IntPtr e, uint s, float x, float y, float z);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_gain(IntPtr e, uint s, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_play(IntPtr e, uint s, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_stop(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] [return: MarshalAs(UnmanagedType.I1)] public static extern bool bw_source_is_playing(IntPtr e, uint s);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_play_oneshot(IntPtr e, uint snd, float x, float y, float z, float gain);

        // ---- ambisonic beds (world-locked diffuse soundfields) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_bed_create(IntPtr e);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_play(IntPtr e, uint b, uint snd, [MarshalAs(UnmanagedType.I1)] bool loop);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_bed_set_gain(IntPtr e, uint b, float linear);
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
        [DllImport(DLL, CallingConvention = CC)] public static extern float bw_source_get_occlusion(IntPtr e, uint s);

        // ---- reflection bed ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_reflections_config(IntPtr e, in BwReflectionConfig cfg);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_reflections_set_gain(IntPtr e, float linear);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_source_set_reflections(IntPtr e, uint s, [MarshalAs(UnmanagedType.I1)] bool on);

        // ---- directivity ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_orientation(IntPtr e, uint s, float qx, float qy, float qz, float qw);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_directivity(IntPtr e, uint s, float weight, float power);
        [DllImport(DLL, CallingConvention = CC)] public static extern void  bw_source_set_directivity_preset(IntPtr e, uint s, BwDirectivity pattern);
        [DllImport(DLL, CallingConvention = CC)] public static extern float bw_source_get_directivity(IntPtr e, uint s);

        // ---- channel test / diagnostics (drives a raw output channel; speaker-check tool) ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_test_signal(IntPtr e, uint channel, BwTestKind kind, float gain);
        // Read back the effective speaker layout (xyz = cap*3 floats, x,y,z per speaker); returns the count (26).
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_get_speakers(IntPtr e, [Out] float[] xyz, uint cap);
        // Panner selection (load-time): DBAP (moving observer, default) or SPCAP (fixed-observer sweet spot).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_panner(IntPtr e, BwPanner panner);
        // Diffuse-bed decoder (load-time): sampling (default) or AllRAD (robust on irregular arrays).
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_bed_decoder(IntPtr e, BwBedDecoder decoder);
        // Offline panner evaluation (no engine handle): out = nsrc*n gains for a layout/panner; for layout scoring.
        [DllImport(DLL, CallingConvention = CC)] public static extern uint bw_panner_gains_batch(BwPanner panner, float[] positions, uint n, float[] lis, float[] srcs, uint nsrc, [Out] float[] outGains);

        // ---- listener + frame boundary ----
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_set_listener_pose(IntPtr e, float px, float py, float pz, float qx, float qy, float qz, float qw);
        // The engine writes p[0..2] and q[0..3] UNCONDITIONALLY — p must be length>=3, q length>=4, both
        // non-null, or the native write corrupts memory. Prefer the GetListenerPose helper below.
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_get_listener_pose(IntPtr e, [Out] float[] p, [Out] float[] q);
        [DllImport(DLL, CallingConvention = CC)] public static extern void bw_commit(IntPtr e);

        // ---- convenience ----
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
