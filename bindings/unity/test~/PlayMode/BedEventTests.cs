// BedEventTests.cs — the SECOND handle map. A bed is a voice, so the core reports its handle
// through the same bwa_poll_ended / bwa_poll_looped rings a source uses, but AmbisonicBed is not a
// SourceBase and so is not in the source registry. Engine keeps _bedsByHandle for it and consults it
// when a drained handle belongs to no source. Nothing here works unless that second lookup does.
using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class BedEventTests
    {
        Engine _eng;
        AmbisonicBed _bed;
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
            if (_bed) Object.DestroyImmediate(_bed.gameObject);
            Harness.DestroyEngine(_eng);
            _bed = null; _eng = null;
            Application.targetFrameRate = _savedFrameRate;
        }

        AmbisonicBed Arm(string clip, bool loop)
        {
            _bed = Harness.CreateBed(clip, loop);
            _bed.onFinished.AddListener(() => _finished++);
            _bed.onLoop.AddListener(() => _looped++);
            return _bed;
        }

        // The fixture has to be a real soundfield or every bed test below is testing a rejected load.
        // Acquired through the AMBIX loader, which is the one AmbisonicBed uses: the cache key is
        // (path, flags), and the same file taken through the default mono loader reports 1 channel
        // because that loader downmixes. Asking the wrong loader is how this check goes vacuous.
        [Test]
        public void BedFixtureIsAmbisonic()
        {
            uint snd = _eng.Acquire(Harness.BedShort, BwaLoadFlags.Ambix);
            Assert.AreNotEqual(0u, snd, "the bed fixture did not load as AmbiX: " + Bwa.LastError(_eng.Handle));
            Assert.AreEqual(4u, Bwa.bwa_sound_get_channels(_eng.Handle, snd),
                            "the bed fixture is not 4-channel B-format; bwa_load_ambix takes 4 / 9 / 16");
            Assert.AreEqual((ulong)Harness.SubFrameFrames, Bwa.bwa_sound_get_frames(_eng.Handle, snd),
                            "the bed fixture is not the length this suite reasons about");
        }

        // ---- 8. a bed finishes, through the second map ---------------------------------------------

        // Sub-frame, and pumped exactly once, for the same reason the Emitter test is: the completion
        // can only have arrived through bwa_poll_ended and _bedsByHandle. An is-playing edge never
        // saw this bed.
        [UnityTest]
        public IEnumerator BedFiresFinished()
        {
            var bed = Arm(Harness.BedShort, loop: false);        // 64 frames = 1.33 ms
            yield return Harness.Frames(3);

            _eng.enabled = false;                                // no commit, no drain, no is-playing poll
            bed.Play();
            yield return Harness.Wall(0.30);
            Assert.AreEqual(0, _finished, "onFinished fired with the pump off");

            _eng.enabled = true;
            yield return null;                                    // exactly ONE pump
            Assert.AreEqual(1, _finished, "a bed completion never reached AmbisonicBed.onFinished - " +
                                          "Engine's bed handle map did not route it");

            yield return Harness.Frames(20);
            Assert.AreEqual(1, _finished, "the bed completion was reported more than once");
        }

        [UnityTest]
        public IEnumerator BedFiresLoopNeverFinished()
        {
            var bed = Arm(Harness.BedLong, loop: true);          // 256 ms per pass
            yield return Harness.Frames(3);
            bed.Play();

            yield return Harness.Wall(1.6);                       // ~6 wraps

            Harness.Note($"looping bed: onLoop x{_looped}, onFinished x{_finished}");
            Assert.GreaterOrEqual(_looped, 4, "a 256 ms soundfield looped for 1.6 s should wrap about " +
                                              "six times; onLoop is not being routed to the bed");
            // Not a coin flip: the wrap count above proves the bed ran, so a zero here is a missing
            // completion rather than a bed that never played.
            Assert.AreEqual(0, _finished, "a LOOPING bed reported onFinished - it never ends");
        }

        // Beds and sources come out of ONE handle pool, which is what makes "look in the source map,
        // then the bed map" correct. With both live at once, a misrouted handle shows up as one of
        // them getting the other's event.
        [UnityTest]
        public IEnumerator BedAndSourceEventsDoNotCross()
        {
            int srcFinished = 0;
            var em = Harness.CreateEmitter(Harness.SubFrame, loop: false);
            em.onFinished.AddListener(() => srcFinished++);
            var bed = Arm(Harness.BedShort, loop: false);
            yield return Harness.Frames(3);

            _eng.enabled = false;
            bed.Play();                                           // only the BED plays
            yield return Harness.Wall(0.30);
            _eng.enabled = true;
            yield return null;

            Assert.AreEqual(1, _finished, "the bed's completion did not reach the bed");
            Assert.AreEqual(0, srcFinished, "a bed completion was routed to a SOURCE");

            _finished = 0;
            _eng.enabled = false;
            em.Play();                                            // now only the SOURCE plays
            yield return Harness.Wall(0.30);
            _eng.enabled = true;
            yield return null;

            Assert.AreEqual(1, srcFinished, "the source's completion did not reach the source");
            Assert.AreEqual(0, _finished, "a source completion was routed to a BED");

            Object.DestroyImmediate(em.gameObject);
        }
    }
}
