// OnsetSyncTests.cs: scheduling a sound against a HOST time (Emitter.PlayAtHostTime, the cluster
// onset helper), and AmbisonicBed.IsPlaying.
//
// The helper is three terms on top of the engine's clock pair: the display latency moves the target
// later, the output latency moves the start earlier, and the pair maps host nanoseconds to dsp
// samples. The arithmetic is pinned exactly on the pure function. The live tests then check that
// the play really STARTS on the sample that arithmetic names, which is what catches a term the
// instance path forgot to pass: the offline sink reports an output latency of 0, so the engine's
// seam supplies one, and both latencies are large against the two blocks of readback slack.
using System;
using System.Collections;
using System.Reflection;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class OnsetSyncTests
    {
        Engine _eng;
        GameObject _thing;
        int _savedFrameRate;

        static readonly FieldInfo OutputLatencyCheck =
            typeof(Engine).GetField("OutputLatencyCheck", BindingFlags.NonPublic | BindingFlags.Static);

        const float  DisplayS  = 0.100f;                 // 4800 frames
        const uint   OutputLat = 2400;                   // 50 ms
        const ulong  SlackFrames = 4 * Harness.BlockSize; // readback granularity plus stamp jitter, far under either latency

        [OneTimeSetUp] public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            Assert.IsNotNull(OutputLatencyCheck, "Engine.OutputLatencyCheck is gone or renamed");
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
        }

        [TearDown]
        public void TearDown()
        {
            OutputLatencyCheck.SetValue(null, null);
            Engine.OwnsAudioPredicate = null;
            if (_thing) UnityEngine.Object.DestroyImmediate(_thing);
            Harness.DestroyEngine(_eng); _eng = null;
            Application.targetFrameRate = _savedFrameRate;
        }

        // ---- the arithmetic ---------------------------------------------------------------------------

        [Test]
        public void HostTimeArithmeticIsExact()
        {
            const ulong pairDsp = 1_000_000, pairNs = 5_000_000_000;
            // Half a second ahead of the pair, no latencies: half a second of samples.
            Assert.AreEqual(pairDsp + 24000, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, pairNs + 500_000_000, 0.0, 0));
            // The display lights 10 ms after the swap, so the sound is wanted 480 samples LATER.
            Assert.AreEqual(pairDsp + 24000 + 480, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, pairNs + 500_000_000, 0.010, 0));
            // The device plays a sample 300 frames after it is rendered, so it starts 300 EARLIER.
            Assert.AreEqual(pairDsp + 24000 - 300, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, pairNs + 500_000_000, 0.0, 300));
            Assert.AreEqual(pairDsp + 24000 + 480 - 300, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, pairNs + 500_000_000, 0.010, 300));
            // A target BEFORE the pair is a sample before it, not an unsigned wrap.
            Assert.AreEqual(pairDsp - 48000, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, pairNs - 1_000_000_000, 0.0, 0));
            // Before sample 0 there is nothing to schedule: clamp.
            Assert.AreEqual(0ul, Engine.DspTimeFramesForHostTime(100, pairNs, 48000.0, pairNs - 1_000_000_000, 0.0, 0));
            Assert.AreEqual(0ul, Engine.DspTimeFramesForHostTime(100, pairNs, 48000.0, pairNs, 0.0, 300));
            // Only the SPAN matters, whatever the epoch: a stamp from a machine up for 30 years.
            const ulong big = 1_000_000_000_000_000_123;
            Assert.AreEqual(pairDsp + 48, Engine.DspTimeFramesForHostTime(pairDsp, big, 48000.0, big + 1_000_000, 0.0, 0));
            // The rate is an argument: a fitted device rate scales the host span and nothing else.
            Assert.AreEqual(pairDsp + 44100 - 300, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 44100.0, pairNs + 1_000_000_000, 0.0, 300));
        }

        // A NaN display latency must not reach the cast to a sample number, and Math.Max(0, NaN) is
        // NaN, so "clamp it at zero" does not do it. A rate that is not a positive number has no answer.
        [Test]
        public void HostTimeArithmeticRefusesBadInput()
        {
            const ulong pairDsp = 1_000_000, pairNs = 5_000_000_000, swap = pairNs + 500_000_000;
            ulong plain = pairDsp + 24000 - 300;
            Assert.AreEqual(plain, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, swap, double.NaN, 300), "NaN display latency");
            Assert.AreEqual(plain, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, swap, -0.25, 300), "negative display latency");
            Assert.AreEqual(plain, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, swap, double.NegativeInfinity, 300));
            ulong capped = plain + (ulong)(Engine.MaxDisplayLatencySeconds * 48000.0);
            Assert.AreEqual(capped, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, swap, 1e9, 300), "a display latency of 30 years");
            Assert.AreEqual(capped, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, 48000.0, swap, double.PositiveInfinity, 300));
            foreach (double rate in new[] { 0.0, -48000.0, double.NaN, double.PositiveInfinity })
                Assert.AreEqual(0ul, Engine.DspTimeFramesForHostTime(pairDsp, pairNs, rate, swap, 0.010, 300), "rate " + rate);
        }

        // ---- the live path ----------------------------------------------------------------------------

        IEnumerator StartWithClock()
        {
            _eng = Harness.CreateStartedEngine(e => e.displayLatencySeconds = DisplayS);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            OutputLatencyCheck.SetValue(null, (Func<uint>)(() => OutputLat));
            bool clock = false;
            yield return Harness.Until(() => _eng.GetClock(out _, out _), 3.0, v => clock = v);
            Assert.IsTrue(clock, "the offline sink never stamped a block");
        }

        // What the test expects, from the engine's raw pair and the three terms spelled out here
        // rather than read back from the function under test.
        ulong Expected(ulong hostNs)
        {
            Assert.IsTrue(_eng.GetClock(out ulong cs, out ulong ct));
            double frames = ((double)hostNs - (double)ct) * 1e-9 * Harness.SampleRate
                          + DisplayS * Harness.SampleRate - OutputLat;
            return cs + (ulong)frames;
        }

        // The sample the voice started on, from the two block-granular readbacks. The playhead is as
        // of the END of the block DspTimeFrames names the start of.
        static IEnumerator StartedOn(Engine eng, Func<ulong> playhead, Action<ulong> result)
        {
            bool hit = false;
            yield return Harness.Until(() => playhead() > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "the scheduled play never started");
            for (int i = 0; i < 50; i++)
            {
                ulong d0 = eng.DspTimeFrames, ph = playhead(), d1 = eng.DspTimeFrames;
                if (d0 == d1) { result(d0 + Harness.BlockSize - ph); yield break; }
            }
            Assert.Fail("could not read the playhead and the dsp clock inside one block");
        }

        static void AssertNear(ulong want, ulong got, string what)
        {
            ulong off = want > got ? want - got : got - want;
            Assert.LessOrEqual(off, SlackFrames, what + ": wanted sample " + want + ", got " + got +
                               " (off by " + off + " frames = " + (off * 1000.0 / Harness.SampleRate).ToString("F1") + " ms)");
        }

        [UnityTest]
        public IEnumerator PlayAtHostTimeStartsOnThePredictedSample()
        {
            yield return StartWithClock();
            _thing = Harness.CreateEmitter(Harness.Long, loop: false).gameObject;
            var em = _thing.GetComponent<Emitter>();
            yield return Harness.Frames(2);

            ulong swap = Engine.HostTimeNs + 400_000_000;          // a frame 0.4 s from now
            ulong want = Expected(swap);
            Assert.IsTrue(_eng.TryDspTimeFramesForHostTime(swap, out ulong asked), "the engine has a clock and refused");
            AssertNear(want, asked, "TryDspTimeFramesForHostTime");
            Assert.Greater(asked, _eng.DspTimeFrames + Harness.SampleRate / 10, "the target is not in the future, so the start below proves nothing");

            Assert.IsTrue(em.PlayAtHostTime(swap), "PlayAtHostTime refused with a clock");
            Assert.AreEqual(0ul, em.PlayheadFrames, "the voice started before its scheduled sample");
            ulong got = 0;
            yield return StartedOn(_eng, () => em.PlayheadFrames, v => got = v);
            AssertNear(want, got, "the emitter's start");
        }

        // The inspector field goes through the same guard: a NaN typed into it (or set from a
        // script) schedules as a latency of 0 and does not poison the start sample.
        [UnityTest]
        public IEnumerator NaNDisplayLatencyFieldSchedulesAsZero()
        {
            yield return StartWithClock();
            ulong swap = Engine.HostTimeNs + 400_000_000;
            _eng.displayLatencySeconds = 0f;
            Assert.IsTrue(_eng.TryDspTimeFramesForHostTime(swap, out ulong zero));
            _eng.displayLatencySeconds = float.NaN;
            Assert.IsTrue(_eng.TryDspTimeFramesForHostTime(swap, out ulong nan));
            AssertNear(zero, nan, "a NaN display latency");
        }

        // DspTimeFramesAt takes Unity's realtime clock and the host-time helper takes the host clock.
        // They are one mapping (one block pair, one rounding), so for the same instant they name the
        // same sample, to within what a back-to-back read of the two clocks can say about "same".
        [UnityTest]
        public IEnumerator HostTimeAndRealtimeHelpersAgree()
        {
            _eng = Harness.CreateStartedEngine();
            bool clock = false;
            yield return Harness.Until(() => _eng.GetClock(out _, out _), 3.0, v => clock = v);
            Assert.IsTrue(clock);
            yield return Harness.Wall(1.5);          // DspTimeFramesAt's clock-offset estimate settles over frames

            double a = Time.realtimeSinceStartupAsDouble;
            ulong host = Engine.HostTimeNs;
            double b = Time.realtimeSinceStartupAsDouble;
            Assert.Less(b - a, 0.001, "the two clock reads were split by a hitch, so they do not name one instant");
            ulong byRealtime = _eng.DspTimeFramesAt((a + b) * 0.5 + 0.4);
            Assert.IsTrue(_eng.TryDspTimeFramesForHostTime(host + 400_000_000, out ulong byHost));
            // 5 ms: DspTimeFramesAt's estimate of the offset between the two clocks is good to about
            // one block's age on this sink. A second anchor or a missing term is tens of milliseconds.
            ulong off = byRealtime > byHost ? byRealtime - byHost : byHost - byRealtime;
            Assert.LessOrEqual(off, 240ul, "the two helpers disagree by " + off + " frames for one instant");
        }

        [UnityTest]
        public IEnumerator BedPlayAtHostTimeStartsOnThePredictedSample()
        {
            yield return StartWithClock();
            // Two seconds and no loop: the start is read back from the playhead, and a playhead that
            // wrapped (or stopped at the end) during an editor hitch would name another sample.
            _thing = Harness.CreateBed(Harness.BedTwoSec, loop: false).gameObject;
            var bed = _thing.GetComponent<AmbisonicBed>();
            yield return Harness.Frames(2);

            ulong swap = Engine.HostTimeNs + 400_000_000;
            ulong want = Expected(swap);
            Assert.IsTrue(bed.PlayAtHostTime(swap), "PlayAtHostTime refused with a clock");
            ulong got = 0;
            yield return StartedOn(_eng, () => bed.PlayheadFrames, v => got = v);
            AssertNear(want, got, "the bed's start");
        }

        // No clock, no play. The manual sink is the one configuration that holds that state still:
        // nothing pumps it here, so no block ever renders and the pair never becomes valid. A node
        // that does not own audio has no engine at all, and no Engine in the scene is the third case.
        [UnityTest]
        public IEnumerator PlayAtHostTimeRefusesWithoutAClock()
        {
            _thing = new GameObject("things");
            var em = _thing.AddComponent<Emitter>();
            em.clip = Harness.Long; em.playOnEnable = false;
            var bed = _thing.AddComponent<AmbisonicBed>();
            bed.clip = Harness.BedLong; bed.playOnEnable = false;
            ulong swap = Engine.HostTimeNs + 100_000_000;
            Assert.IsFalse(em.PlayAtHostTime(swap), "an emitter scheduled a play with no Engine");
            Assert.IsFalse(bed.PlayAtHostTime(swap), "a bed scheduled a play with no Engine");

            _eng = Harness.CreateStartedEngine(e => e.sink = BwaSinkType.Manual);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            yield return Harness.Frames(5);
            Assert.IsFalse(_eng.GetClock(out _, out _), "the unpumped manual sink has a clock, so this test shows nothing");
            Assert.IsFalse(_eng.TryDspTimeFramesForHostTime(swap, out _));
            Assert.IsFalse(em.PlayAtHostTime(swap), "an emitter scheduled a play against no clock");
            Assert.IsFalse(bed.PlayAtHostTime(swap), "a bed scheduled a play against no clock");
            yield return Harness.Frames(3);
            // A play that got through reads as playing at once: an enqueued play counts.
            Assert.IsFalse(em.IsPlaying, "the refused call played anyway");
            Assert.IsFalse(bed.IsPlaying, "the refused call played anyway");

            Harness.DestroyEngine(_eng);
            Engine.OwnsAudioPredicate = () => false;
            _eng = Harness.CreateStartedEngine();
            Assert.IsFalse(_eng.Ready);
            yield return Harness.Frames(2);
            Assert.IsFalse(em.PlayAtHostTime(swap), "a node that does not own audio scheduled a play");
            Assert.IsFalse(bed.PlayAtHostTime(swap), "a node that does not own audio scheduled a play");
            Assert.IsFalse(bed.IsPlaying);
        }

        // ---- AmbisonicBed.IsPlaying -------------------------------------------------------------------

        [UnityTest]
        public IEnumerator BedIsPlayingFollowsAOneShot()
        {
            _eng = Harness.CreateStartedEngine();
            _thing = Harness.CreateBed(Harness.BedLong, loop: false).gameObject;    // 256 ms
            var bed = _thing.GetComponent<AmbisonicBed>();
            int finished = 0;
            bed.onFinished.AddListener(() => finished++);
            yield return Harness.Frames(3);
            Assert.IsFalse(bed.IsPlaying, "a bed that was never played reads as playing");

            bed.Play();
            Assert.IsTrue(bed.IsPlaying, "an enqueued play does not read as playing");
            bool ended = false;
            yield return Harness.Until(() => !bed.IsPlaying, 3.0, v => ended = v);
            Assert.IsTrue(ended, "IsPlaying never fell after a 256 ms soundfield");
            yield return Harness.Frames(3);
            Assert.AreEqual(1, finished, "IsPlaying fell without the end being reported once");
        }

        [UnityTest]
        public IEnumerator LoopingBedReadsPlayingThroughAPauseUntilStopped()
        {
            _eng = Harness.CreateStartedEngine();
            _thing = Harness.CreateBed(Harness.BedLong, loop: true).gameObject;
            var bed = _thing.GetComponent<AmbisonicBed>();
            yield return Harness.Frames(2);
            bed.Play();
            yield return Harness.Wall(0.6);                       // more than two loops
            Assert.IsTrue(bed.IsPlaying, "a looping bed stopped reading as playing");

            _eng.Paused = true;                                   // the Emitter rule: paused still reads playing
            yield return Harness.Wall(0.3);
            ulong held = bed.PlayheadFrames;
            yield return Harness.Wall(0.2);
            Assert.AreEqual(held, bed.PlayheadFrames, "the engine is not paused, so the read below shows nothing");
            Assert.IsTrue(bed.IsPlaying, "a paused bed reads as stopped");
            _eng.Paused = false;

            bed.Stop();
            bool stopped = false;
            yield return Harness.Until(() => !bed.IsPlaying, 3.0, v => stopped = v);
            Assert.IsTrue(stopped, "IsPlaying stayed true after Stop");
        }

        // The bed's OWN pause (AmbisonicBed.Paused), not the engine-wide one above.
        [UnityTest]
        public IEnumerator BedPausedByItsOwnPauseStillReadsPlaying()
        {
            _eng = Harness.CreateStartedEngine();
            _thing = Harness.CreateBed(Harness.BedTwoSec, loop: true).gameObject;
            var bed = _thing.GetComponent<AmbisonicBed>();
            yield return Harness.Frames(2);
            bed.Play();
            bool started = false;
            yield return Harness.Until(() => bed.PlayheadFrames > 0, 3.0, v => started = v);
            Assert.IsTrue(started, "the bed never started");

            bed.Paused = true;
            yield return Harness.Wall(0.3);
            ulong held = bed.PlayheadFrames;
            yield return Harness.Wall(0.3);
            Assert.AreEqual(held, bed.PlayheadFrames, "the bed is not paused, so the read below shows nothing");
            Assert.IsTrue(bed.IsPlaying, "a bed paused with its own pause reads as stopped");

            bed.Paused = false;
            bool moved = false;
            yield return Harness.Until(() => bed.PlayheadFrames != held, 3.0, v => moved = v);
            Assert.IsTrue(moved, "the bed did not resume");
            Assert.IsTrue(bed.IsPlaying);
        }

        // Paused on a bed is SourceBase.Paused: it round-trips, it holds the playhead, a play
        // restarts un-paused, and a value set before Start survives the auto-play.
        [UnityTest]
        public IEnumerator BedPausedRoundTripsAndFreezesThePlayhead()
        {
            _eng = Harness.CreateStartedEngine();
            _thing = Harness.CreateBed(Harness.BedTwoSec, loop: true).gameObject;
            var bed = _thing.GetComponent<AmbisonicBed>();
            yield return Harness.Frames(2);
            Assert.IsFalse(bed.Paused, "a new bed reads as paused");
            bed.Play();
            bool started = false;
            yield return Harness.Until(() => bed.PlayheadFrames > 0, 3.0, v => started = v);
            Assert.IsTrue(started, "the bed never started");

            bed.Pause();
            Assert.IsTrue(bed.Paused, "Pause did not set Paused");
            yield return Harness.Wall(0.3);              // the pause ramps out over a block
            ulong held = bed.PlayheadFrames;
            yield return Harness.Wall(0.3);
            Assert.AreEqual(held, bed.PlayheadFrames, "Paused reads true but the bed is running");

            bed.UnPause();
            Assert.IsFalse(bed.Paused, "UnPause did not clear Paused");
            bool moved = false;
            yield return Harness.Until(() => bed.PlayheadFrames != held, 3.0, v => moved = v);
            Assert.IsTrue(moved, "UnPause did not resume the bed");

            bed.Paused = true;
            bed.Play();                                   // a play restarts un-paused
            Assert.IsFalse(bed.Paused, "a play left the bed marked paused");
            yield return Harness.Wall(0.3);
            ulong a = bed.PlayheadFrames;
            bool runs = false;
            yield return Harness.Until(() => bed.PlayheadFrames != a, 3.0, v => runs = v);
            Assert.IsTrue(runs, "the bed stayed paused across a play");

            // Before Start: the auto-play runs first, then the pause lands on it.
            var early = new GameObject("early paused bed");
            early.transform.SetParent(_thing.transform);
            var eb = early.AddComponent<AmbisonicBed>();
            eb.clip = Harness.BedTwoSec;
            eb.Paused = true;                             // before Start
            yield return Harness.Wall(0.5);
            Assert.IsTrue(eb.Paused, "Paused set before Start reads false");
            Assert.IsTrue(eb.IsPlaying, "the paused bed never auto-played");
            ulong h0 = eb.PlayheadFrames;
            yield return Harness.Wall(0.3);
            Assert.AreEqual(h0, eb.PlayheadFrames, "Paused set before Start did not hold the bed");
        }
    }
}
