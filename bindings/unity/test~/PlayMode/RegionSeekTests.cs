// RegionSeekTests.cs — the play region and the seek, in both spellings.
//
// Two of these tests read the playhead while the voice is PAUSED. That is not incidental: rt's
// pause gate lands a pending seek only once the voice is inaudible, and then freezes the playhead
// ("paused: skip mixing, playhead stays frozen"). So a paused seek lands on an EXACT frame that a
// later read returns unchanged, and the frames/seconds equivalence becomes an equality instead of a
// tolerance around a playhead that keeps moving in wall time.
using System.Collections;
using System.Diagnostics;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class RegionSeekTests
    {
        // The region every test here uses: 0.5 s .. 1.0 s of the 2 s fixture, which is the same window
        // whether you spell it in frames or in seconds.
        const ulong RegionBeg = 24000;   // 0.5 s at 48 kHz
        const ulong RegionEnd = 48000;   // 1.0 s
        const double RegionBegSeconds = 0.5;
        const double RegionEndSeconds = 1.0;

        Engine _eng;
        Emitter _em;
        int _finished, _looped;
        int _savedFrameRate;

        [OneTimeSetUp]
        public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
            _finished = _looped = 0;
            _eng = Harness.CreateEngine();
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
        }

        [TearDown]
        public void TearDown()
        {
            if (_em) Object.DestroyImmediate(_em.gameObject);
            Harness.DestroyEngine(_eng);
            _em = null; _eng = null;
            Application.targetFrameRate = _savedFrameRate;
        }

        Emitter Arm(string clip, bool loop)
        {
            _em = Harness.CreateEmitter(clip, loop);
            _em.onFinished.AddListener(() => _finished++);
            _em.onLoop.AddListener(() => _looped++);
            return _em;
        }

        // Park a paused voice on a known frame. The pause ramp needs a block to reach silence and the
        // seek lands the block after that, so both waits are frames, not guesses.
        IEnumerator SeekPausedTo(Emitter em, ulong frame)
        {
            em.Paused = true;
            yield return Harness.Frames(3);
            em.SeekFrames(frame);
            yield return Harness.Frames(3);
        }

        // ---- 6. the region confines the playhead --------------------------------------------------

        [UnityTest]
        public IEnumerator RegionFramesConfinesPlayhead()
        {
            var em = Arm(Harness.Long, loop: true);              // 2 s, so the region is a real subset
            yield return Harness.Frames(3);
            em.Play();
            em.SetRegionFrames(RegionBeg, RegionEnd);

            // The region does not move the playhead, so the first pass runs from 0 and reaches the
            // region end before wrapping INTO it. Sample only after that first wrap.
            bool wrapped = false;
            yield return Harness.Until(() => _looped >= 1, 3.0, v => wrapped = v);
            Assert.IsTrue(wrapped, "the region never wrapped, so nothing here is being confined");

            ulong lo = ulong.MaxValue, hi = 0;
            int samples = 0;
            var sw = Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < 1.0)
            {
                yield return null;
                ulong p = em.PlayheadFrames;
                if (p < lo) lo = p;
                if (p > hi) hi = p;
                samples++;
            }
            Harness.Note($"region [24000,48000): {samples} playhead samples in [{lo},{hi}], " +
                         $"onLoop x{_looped}");

            Assert.Greater(samples, 10, "too few playhead samples to say anything");
            Assert.GreaterOrEqual(lo, RegionBeg, "the playhead ran BELOW the region start");
            // <= not <: the wrap test runs at the TOP of each sample, so a block can leave the cursor
            // parked exactly ON the region end and publish it there before the next block wraps it.
            Assert.LessOrEqual(hi, RegionEnd, "the playhead ran past the region end");
            // Without this the test would pass on a playhead pinned at the region start, which is what
            // a broken region and a stopped voice both look like.
            Assert.Greater(hi, RegionBeg + (RegionEnd - RegionBeg) / 2,
                           "the playhead never swept the second half of the region");
            Assert.GreaterOrEqual(_looped, 2, "a 0.5 s region played for 1 s should wrap at least twice");
        }

        // A one-shot ENDS at the region end exactly as it would at the clip end. The margin is the
        // fixture: the clip is 2 s and the region is 0.1 s, so an ignored region takes 20x as long.
        [UnityTest]
        public IEnumerator RegionFramesEndsOneShotEarly()
        {
            var em = Arm(Harness.Long, loop: false);             // 2.0 s
            yield return Harness.Frames(3);
            em.Play();
            em.SetRegionFrames(0, 4800);                         // 0.1 s

            var sw = Stopwatch.StartNew();
            bool ended = false;
            yield return Harness.Until(() => _finished > 0, 1.0, v => ended = v);
            double elapsed = sw.Elapsed.TotalSeconds;
            Harness.Note($"region-truncated one-shot finished after {elapsed:F3} s (clip is 2.000 s)");

            Assert.IsTrue(ended, $"a one-shot bounded to 0.1 s had not finished after {elapsed:F3} s");
            Assert.Less(elapsed, 0.6, "the one-shot ran well past its region end");
            Assert.AreEqual(1, _finished, "the region end fired onFinished more than once");
        }

        // ---- 7. the seconds spellings agree with the frames ones -----------------------------------

        // Exact, because the reads happen on a paused voice: a seek that landed is a frozen playhead.
        [UnityTest]
        public IEnumerator SeekSecondsMatchesSeekFrames()
        {
            var em = Arm(Harness.Long, loop: true);
            yield return Harness.Frames(3);
            em.Play();

            yield return SeekPausedTo(em, RegionEnd);            // 48000 frames = 1.0 s
            ulong byFrames = em.PlayheadFrames;
            Assert.AreEqual(RegionEnd, byFrames, "SeekFrames did not land on the frame it was given");

            em.SeekFrames(0);                                    // move away, so the next read is not stale
            yield return Harness.Frames(3);
            Assert.AreEqual(0ul, em.PlayheadFrames, "the playhead did not move, so the reads below " +
                                                    "prove nothing");

            em.SeekSeconds(RegionEndSeconds);                    // 1.0 s = the same place
            yield return Harness.Frames(3);
            ulong bySeconds = em.PlayheadFrames;
            Harness.Note($"SeekFrames({RegionEnd}) -> {byFrames}, SeekSeconds({RegionEndSeconds}) -> {bySeconds}");

            Assert.AreEqual(byFrames, bySeconds, "SeekSeconds and SeekFrames disagree at the engine's " +
                                                 "sample rate - the seconds spelling is not converting");
        }

        // The region's two spellings, compared through the seek CLAMP: a seek resolves inside the
        // region, so where a seek lands reports the region's bounds exactly. A seek below the start
        // clamps up to it; a seek past the end wraps modulo the region on a looping voice.
        [UnityTest]
        public IEnumerator RegionSecondsMatchesRegionFrames()
        {
            var em = Arm(Harness.Long, loop: true);
            yield return Harness.Frames(3);

            em.Play();
            em.SetRegionFrames(RegionBeg, RegionEnd);
            yield return SeekPausedTo(em, 0);                    // clamps UP to the region start
            ulong begByFrames = em.PlayheadFrames;
            em.SeekFrames(60000);                                // wraps: beg + (60000-beg) % span
            yield return Harness.Frames(3);
            ulong wrapByFrames = em.PlayheadFrames;

            Assert.AreEqual(RegionBeg, begByFrames, "a seek below the region start did not clamp to it");
            Assert.AreEqual(RegionBeg + (60000 - RegionBeg) % (RegionEnd - RegionBeg), wrapByFrames,
                            "a seek past the region end did not wrap inside it");

            // Same voice, same protocol, the region set in SECONDS this time.
            em.UnPause();
            em.Play();                                           // a fresh play resets the region
            em.SetRegionSeconds(RegionBegSeconds, RegionEndSeconds);
            yield return SeekPausedTo(em, 0);
            ulong begBySeconds = em.PlayheadFrames;
            em.SeekFrames(60000);
            yield return Harness.Frames(3);
            ulong wrapBySeconds = em.PlayheadFrames;

            Harness.Note($"region frames -> clamp {begByFrames}, wrap {wrapByFrames}; " +
                         $"region seconds -> clamp {begBySeconds}, wrap {wrapBySeconds}");

            Assert.AreEqual(begByFrames, begBySeconds, "SetRegionSeconds and SetRegionFrames disagree " +
                                                       "on the region START at the engine's sample rate");
            Assert.AreEqual(wrapByFrames, wrapBySeconds, "SetRegionSeconds and SetRegionFrames disagree " +
                                                         "on the region END at the engine's sample rate");
        }
    }
}
