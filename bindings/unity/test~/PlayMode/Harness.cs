// Harness.cs — the shared scaffolding for the PlayMode suite: wav fixtures written at run time, an
// Engine brought up on the offline sink, and the two waiting primitives every test is built from.
//
// Why the null sink and not the manual one: BWA_SINK_MANUAL has no audio thread, so blocks advance
// only when the caller pumps bwa_render_block — and Bwa.cs deliberately does not bind that call
// (docs/integration.md lists it as one of the two the binding skips). BWA_SINK_NULL is the shipping
// topology minus the device: a real audio thread on a real clock, the real command ring, the real
// event rings. That is what these tests are about, so it is the sink they run on. The cost is that
// playback advances in WALL time, which is why the ordering helpers below exist.
using System;
using System.Collections;
using System.Diagnostics;
using System.IO;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace BwAudio.Tests
{
    /// <summary>Engine bring-up, wav fixtures, and the wall-clock waits the suite shares.</summary>
    public static class Harness
    {
        public const uint SampleRate = 48000;
        public const uint BlockSize  = 256;

        // The frame rate every test runs at. A pump interval this long is what makes "shorter than a
        // frame" a real claim: SubFrameFrames below is 1.33 ms against a ~33 ms frame, a 25x margin
        // that SubFrameClipIsActuallySubFrame asserts rather than assumes.
        public const int TargetFrameRate = 30;

        // ---- fixture clips (engine-rate, so nothing resamples) ---------------------------------------

        public const string SubFrame = "bwa_test_subframe.wav";   // mono, 64 frames  = 1.33 ms
        public const string Short    = "bwa_test_short.wav";      // mono, 12288 frames = 256 ms (48 blocks)
        public const string Long     = "bwa_test_long.wav";       // mono, 96000 frames = 2.0 s
        public const string BedShort = "bwa_test_bed_short.wav";  // 4-ch AmbiX, 64 frames
        public const string BedLong  = "bwa_test_bed_long.wav";   // 4-ch AmbiX, 12288 frames
        public const string BedFront = "bwa_test_bed_front.wav";  // 4-ch AmbiX plane wave from the front, 12288 frames

        public const uint SubFrameFrames = 64;
        public const uint ShortFrames    = 12288;
        public const uint LongFrames     = 96000;

        /// <summary>Write every fixture clip into StreamingAssets. Idempotent, and it rewrites rather
        /// than skips: a fixture left over from an older revision of this file would otherwise be the
        /// thing under test.</summary>
        public static void WriteFixtures()
        {
            string dir = Application.streamingAssetsPath;
            Directory.CreateDirectory(dir);
            WriteTone(Path.Combine(dir, SubFrame), SubFrameFrames, 1);
            WriteTone(Path.Combine(dir, Short),    ShortFrames,    1);
            WriteTone(Path.Combine(dir, Long),     LongFrames,     1);
            WriteTone(Path.Combine(dir, BedShort), SubFrameFrames, 4);
            WriteTone(Path.Combine(dir, BedLong),  ShortFrames,    4);
            WriteTone(Path.Combine(dir, BedFront), ShortFrames,    4, front: true);
        }

        // A 16-bit PCM wav of a 440 Hz tone. channels = 1 for a point source; 4 writes an AmbiX
        // first-order soundfield (W carries the tone, XYZ silent = a diffuse field), which is the
        // channel count bwa_load_ambix demands (4 / 9 / 16) and what the bed tests need. `front` also
        // puts the tone on X (ACN 3): an SN3D plane wave arriving from the field's front.
        public static void WriteTone(string path, uint frames, int channels, bool front = false)
        {
            const double freq = 440.0, amp = 0.5;
            int dataBytes = (int)frames * channels * 2;
            using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
            using (var w = new BinaryWriter(fs))
            {
                w.Write(new[] { 'R', 'I', 'F', 'F' });
                w.Write(36 + dataBytes);
                w.Write(new[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
                w.Write(16);                                    // PCM fmt chunk size
                w.Write((short)1);                              // PCM
                w.Write((short)channels);
                w.Write((int)SampleRate);
                w.Write((int)SampleRate * channels * 2);        // byte rate
                w.Write((short)(channels * 2));                 // block align
                w.Write((short)16);                             // bits per sample
                w.Write(new[] { 'd', 'a', 't', 'a' });
                w.Write(dataBytes);
                for (uint i = 0; i < frames; i++)
                {
                    short s = (short)(Math.Sin(2.0 * Math.PI * freq * i / SampleRate) * amp * 32767.0);
                    for (int ch = 0; ch < channels; ch++)
                        w.Write(ch == 0 || (front && ch == 3) ? s : (short)0);   // W (+ X): the rest silent
                }
            }
        }

        // ---- engine bring-up -------------------------------------------------------------------------

        /// <summary>Stand up an Engine on the offline sink. Fields are set while the GameObject is
        /// INACTIVE so Awake sees them: an active AddComponent runs Awake immediately, against the
        /// inspector defaults. On return Awake has run and Start has NOT: the Engine holds
        /// Instance and the room registration, and the native engine comes up in Start. Yield
        /// <see cref="Started"/> before using it.</summary>
        /// <param name="configure">Runs on the inactive Engine, after the defaults above, so a test
        /// can set what Awake and Start read (profile, roomOrigin, ownsAudio), and add components
        /// that must wake in the same activation as the Engine.</param>
        public static Engine CreateEngine(Action<Engine> configure = null)
        {
            var go = new GameObject("bwa_test_engine");
            go.SetActive(false);
            var eng = go.AddComponent<Engine>();
            eng.sink = BwaSinkType.Null;             // no device, real audio thread
            eng.profile = BwaProfile.Cave;           // straight to the 26-ch bus; no HRTF machinery
            eng.layoutFile = "";                     // the ABI's only way to ask for the default grid
            eng.sampleRate = SampleRate;
            eng.blockSize = BlockSize;
            eng.feedListener = true;
            eng.listener = go.transform;             // assigned so Start does not warn about a dead pose
            configure?.Invoke(eng);
            go.SetActive(true);                      // -> Awake. Start (bwa_create + bwa_start) is Unity's to call
            return eng;
        }

        /// <summary>An Engine that is started on return (Engine.StartNow), for a test that only needs
        /// a running engine. Tests about WHEN the engine starts use <see cref="CreateEngine"/> and
        /// <see cref="Started"/>, which leave the start to Unity's own Start call.</summary>
        public static Engine CreateStartedEngine(Action<Engine> configure = null)
        {
            var eng = CreateEngine(configure);
            eng.StartNow();
            return eng;
        }

        /// <summary>Turn frames until the Engine's Start has run (Engine.Decided). Unity calls Start
        /// before the next Update, so this is one frame. Bounded, so a Start that never comes fails
        /// the caller's next assert and does not hang the run.</summary>
        public static IEnumerator Started(Engine eng)
        {
            var sw = Stopwatch.StartNew();
            while (eng && !eng.Decided && sw.Elapsed.TotalSeconds < 10.0) yield return null;
        }

        /// <summary>Tear an Engine (and anything parented to it) down synchronously, so the next test
        /// starts against a fresh native engine rather than this one's leftovers.</summary>
        public static void DestroyEngine(Engine eng)
        {
            if (eng) UnityEngine.Object.DestroyImmediate(eng.gameObject);
        }

        public static Emitter CreateEmitter(string clip, bool loop)
        {
            var go = new GameObject("bwa_test_emitter");
            go.SetActive(false);
            var em = go.AddComponent<Emitter>();
            em.clip = clip;
            em.loop = loop;
            em.playOnEnable = false;                 // every test drives Play itself, at a known moment
            go.SetActive(true);
            return em;
        }

        public static AmbisonicBed CreateBed(string clip, bool loop)
        {
            var go = new GameObject("bwa_test_bed");
            go.SetActive(false);
            var bed = go.AddComponent<AmbisonicBed>();
            bed.clip = clip;
            bed.loop = loop;
            bed.playOnEnable = false;
            go.SetActive(true);
            return bed;
        }

        // ---- waiting ---------------------------------------------------------------------------------

        /// <summary>Turn `n` Unity frames. With the Engine enabled each one is a pump: push, commit,
        /// both drains, then the post-commit pass.</summary>
        public static IEnumerator Frames(int n)
        {
            for (int i = 0; i < n; i++) yield return null;
        }

        /// <summary>Turn frames for `seconds` of WALL time. Stopwatch, not clock(): clock() does not
        /// advance across a sleep, and every wait here spans one.</summary>
        public static IEnumerator Wall(double seconds)
        {
            var sw = Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < seconds) yield return null;
        }

        /// <summary>Turn frames until `cond` holds or `timeoutSeconds` of WALL time passes. Returns
        /// through `hit`, so a caller can assert on the timeout instead of silently accepting it.</summary>
        public static IEnumerator Until(Func<bool> cond, double timeoutSeconds, Action<bool> hit)
        {
            var sw = Stopwatch.StartNew();
            while (!cond() && sw.Elapsed.TotalSeconds < timeoutSeconds) yield return null;
            hit(cond());
        }

        /// <summary>The median wall interval between Unity frames, measured over `n` of them. The
        /// sub-frame test asserts its clip against this rather than against TargetFrameRate, because
        /// what matters is the interval this run ACTUALLY produced.</summary>
        public static IEnumerator MeasureFrameInterval(int n, Action<double> result)
        {
            var samples = new double[n];
            var sw = Stopwatch.StartNew();
            double last = 0.0;
            for (int i = 0; i < n; i++)
            {
                yield return null;
                double now = sw.Elapsed.TotalSeconds;
                samples[i] = now - last;
                last = now;
            }
            Array.Sort(samples);
            result(samples[n / 2]);
        }

        /// <summary>Every log line while it lives, as "Type: message". For asserting that something
        /// did NOT log, which LogAssert cannot do for warnings.</summary>
        public sealed class LogCapture : IDisposable
        {
            public readonly System.Collections.Generic.List<string> Lines = new System.Collections.Generic.List<string>();
            public LogCapture() { Application.logMessageReceived += OnLog; }
            void OnLog(string msg, string stack, LogType t) { Lines.Add(t + ": " + msg); }
            public void Dispose() { Application.logMessageReceived -= OnLog; }
            public int Count(string needle) { int n = 0; foreach (var l in Lines) if (l.Contains(needle)) n++; return n; }
            /// <summary>Warnings, errors and exceptions, joined, or "" when there were none.</summary>
            public string Problems()
            {
                var sb = new System.Text.StringBuilder();
                foreach (var l in Lines) if (!l.StartsWith("Log: ")) sb.AppendLine(l);
                return sb.ToString();
            }
        }

        /// <summary>Log line prefix, so a failure in a batchmode log is findable.</summary>
        public static void Note(string msg) => Debug.Log("[bwa-test] " + msg);
    }
}
