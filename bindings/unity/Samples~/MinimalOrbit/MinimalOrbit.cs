// MinimalOrbit.cs - the smallest realistic BwAudio client: orbit a click around the listener's
// head, then stop.
//
// Every binding has a copy of this demo and they all play the same stimulus, so you can A/B this
// one against the C reference (examples/minimal.c in the engine repo) by ear.
//
// SETUP. One GameObject with an Engine (profile Binaural, sink Auto), a Transform assigned to its
// `listener` field, and a second GameObject carrying a PushEmitter plus this script. Nothing else:
// the stimulus is synthesized here, so the sample ships no audio asset.
//
// WHAT IT SHOWS. Engine does the per-frame push and the single commit for you, so a client moves a
// transform and feeds a voice. There is no Commit call here and there should not be one.
using UnityEngine;

namespace BwAudio.Samples
{
    [RequireComponent(typeof(PushEmitter))]
    public sealed class MinimalOrbit : MonoBehaviour
    {
        [Tooltip("How far out the click orbits, in meters.")]
        public float radiusMeters = 2f;

        [Tooltip("Height of the orbit above the floor. The default grid puts the ears at 1.5 m.")]
        public float earHeightMeters = 1.5f;

        [Tooltip("Seconds for one revolution.")]
        public float lapSeconds = 6f;

        [Tooltip("Stop after one lap. Off = orbit until the object is disabled.")]
        public bool oneLap = true;

        PushEmitter _emitter;
        float[] _period;
        float _t;
        bool _done;

        void Start()
        {
            _emitter = GetComponent<PushEmitter>();
            _emitter.gain = 0.8f;

            var engine = Engine.Instance;
            uint rate = engine != null ? engine.sampleRate : 48000u;
            _period = ClickPeriod(rate);
        }

        void Update()
        {
            if (_done || _emitter == null) return;

            // Orbit: horizontal, one lap in lapSeconds. Starts in front (+z in ROOM space) and
            // passes the LEFT ear first. Going through Room.FromRoom rather than writing Unity
            // coordinates is the point: Unity is left-handed, so the engine's +x (left) is Unity's
            // -x, and a hand-written trajectory silently mirrors.
            float a = 2f * Mathf.PI * _t / Mathf.Max(lapSeconds, 0.001f);
            var roomPos = new Vector3(radiusMeters * Mathf.Sin(a), earHeightMeters,
                                      radiusMeters * Mathf.Cos(a));
            transform.position = Room.FromRoom(roomPos);

            // Keep the ring fed. Whole periods only: the ring holds 65536 frames and a period is
            // 12000, so a whole one always fits, and pushing whole periods keeps the click grid
            // aligned no matter how the frame rate jitters.
            while (_emitter.PushSpace >= _period.Length)
                _emitter.Push(_period);

            _t += Time.deltaTime;
            if (oneLap && _t >= lapSeconds)
            {
                _emitter.PushEnd();      // the voice ends once the ring drains; one-way
                _done = true;
                var engine = Engine.Instance;
                if (engine != null)
                    Debug.Log($"[MinimalOrbit] lap finished, xruns={engine.Xruns}");
            }
        }

        // ---- THE CLICK ------------------------------------------------------------------------
        // One 250 ms period: a 2 ms Hann-windowed noise burst at -12 dBFS peak, then silence.
        // Looped, that is four clicks a second, and an orbit reads as a trajectory rather than as a
        // level pan. A tone would not do: the cues that place a source in elevation and front-back
        // live in the SPECTRUM, and a narrowband tone carries almost none of them.
        //
        // The noise comes from a 32-bit LCG written out here rather than from Random, because every
        // binding's copy of this stimulus has to produce the same samples and no two languages'
        // generators agree. examples/minimal.c is the reference.

        public const float ClickPeak = 0.251189f;      // -12 dBFS
        public const float ClickPeriodSeconds = 0.25f; // four clicks a second
        public const float ClickBurstSeconds = 0.002f;

        /// <summary>One period of the stimulus as mono float frames: the burst, then silence.</summary>
        public static float[] ClickPeriod(uint sampleRate)
        {
            int period = Mathf.RoundToInt(ClickPeriodSeconds * sampleRate);
            int nburst = Mathf.RoundToInt(ClickBurstSeconds * sampleRate);

            var burst = new double[nburst];
            uint state = 12345u;
            double peak = 0.0;
            for (int i = 0; i < nburst; ++i)
            {
                state = unchecked(state * 1664525u + 1013904223u);
                double u = ((state >> 8) & 0xFFFFFFu) / 8388608.0 - 1.0;          // [-1, 1)
                double w = 0.5 - 0.5 * System.Math.Cos(2.0 * System.Math.PI * i / (nburst - 1));
                burst[i] = w * u;
                peak = System.Math.Max(peak, System.Math.Abs(burst[i]));
            }
            double scale = peak > 0.0 ? ClickPeak / peak : 0.0;                   // exactly -12 dBFS

            var outp = new float[period];
            for (int i = 0; i < nburst; ++i) outp[i] = (float)(burst[i] * scale);
            return outp;
        }
    }
}
