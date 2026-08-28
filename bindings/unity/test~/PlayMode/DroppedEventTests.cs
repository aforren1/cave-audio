// DroppedEventTests.cs — Engine.EndedEventsDropped / LoopEventsDropped.
//
// Both counters are supposed to sit at 0 forever and move only when nothing read the rings in time,
// which is exactly why they are worth a test: a readback that can only ever report 0 is
// indistinguishable from a readback that is not wired up. So each test here STARVES the ring on
// purpose (Engine's pump off while the audio thread keeps posting) and demands a number well clear
// of zero, then confirms the counter was 0 before the starvation.
//
// The bound being crossed is rt's notice reserve: EVT_CAP is 1024 and the ownership events reserve
// voice_cap + sound_cap of it, so about 500 undrained completions or wraps is all that fits. Past
// that the AUDIO thread refuses to post and counts the refusal, which is what these read back.
using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class DroppedEventTests
    {
        Engine _eng;
        int _savedFrameRate;

        [OneTimeSetUp]
        public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
            _eng = Harness.CreateEngine();
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
        }

        [TearDown]
        public void TearDown()
        {
            Harness.DestroyEngine(_eng);
            _eng = null;
            Application.targetFrameRate = _savedFrameRate;
        }

        // ---- 9a. loop drops ------------------------------------------------------------------------

        // A 64-frame loop region wraps 750 times a second. Starve the ring for 2 s and about 1500
        // wraps compete for ~500 slots, so the overflow is not marginal even on a slow machine.
        [UnityTest]
        public IEnumerator LoopEventsDroppedRises()
        {
            var em = Harness.CreateEmitter(Harness.Short, loop: true);
            yield return Harness.Frames(3);
            em.Play();
            em.SetRegionFrames(0, 64);                            // 1.33 ms per wrap

            yield return Harness.Frames(5);
            Assert.AreEqual(0ul, _eng.LoopEventsDropped, "wraps were already being dropped before the " +
                                                         "pump was starved, so the rise below proves nothing");

            _eng.enabled = false;                                 // nothing drains the loop ring
            yield return Harness.Wall(2.0);
            _eng.enabled = true;
            yield return Harness.Frames(3);                       // pump: bwa_poll_looped reports the total

            Harness.Note($"LoopEventsDropped after a 2 s starve = {_eng.LoopEventsDropped}");
            Assert.Greater(_eng.LoopEventsDropped, 100ul,
                           "LoopEventsDropped stayed near zero through 2 s of unread wraps - the " +
                           "counter is not surfacing what the engine dropped");

            Object.DestroyImmediate(em.gameObject);
        }

        // ---- 9b. ended drops -----------------------------------------------------------------------

        // A voice ends once per play, so the drops have to come from many sources replayed many times.
        // 40 emitters on a sub-frame clip, re-played every frame with the pump off: each frame spans
        // about six audio blocks, so every play starts and ends before the next one is issued.
        [UnityTest]
        public IEnumerator EndedEventsDroppedRises()
        {
            const int sources = 40, rounds = 45;
            var ems = new Emitter[sources];
            for (int i = 0; i < sources; i++) ems[i] = Harness.CreateEmitter(Harness.SubFrame, loop: false);
            yield return Harness.Frames(3);

            Assert.AreEqual(0ul, _eng.EndedEventsDropped, "completions were already being dropped before " +
                                                          "the pump was starved");

            _eng.enabled = false;                                 // nothing drains the ended ring
            for (int r = 0; r < rounds; r++)
            {
                for (int i = 0; i < sources; i++) ems[i].Play();
                yield return null;
            }
            _eng.enabled = true;
            yield return Harness.Frames(3);                       // pump: bwa_poll_ended reports the total

            Harness.Note($"EndedEventsDropped after {sources * rounds} unread completions = " +
                         $"{_eng.EndedEventsDropped}");
            Assert.Greater(_eng.EndedEventsDropped, 100ul,
                           $"EndedEventsDropped stayed near zero through {sources * rounds} unread " +
                           "completions - the counter is not surfacing what the engine dropped");

            for (int i = 0; i < sources; i++) Object.DestroyImmediate(ems[i].gameObject);
        }

        // The other half of the claim: on a normally pumped engine the counters do NOT move. Without
        // this, both tests above would still pass against a counter that only ever counts up.
        [UnityTest]
        public IEnumerator CountersStayZeroWhenPumped()
        {
            var em = Harness.CreateEmitter(Harness.Short, loop: true);
            int looped = 0;
            em.onLoop.AddListener(() => looped++);
            yield return Harness.Frames(3);
            em.Play();

            yield return Harness.Wall(1.5);                        // ~6 wraps, all of them drained

            Harness.Note($"pumped: onLoop x{looped}, ended dropped {_eng.EndedEventsDropped}, " +
                         $"loop dropped {_eng.LoopEventsDropped}");
            Assert.GreaterOrEqual(looped, 4, "the voice did not wrap, so a zero drop count means nothing");
            Assert.AreEqual(0ul, _eng.LoopEventsDropped, "wraps were dropped on a normally pumped engine");
            Assert.AreEqual(0ul, _eng.EndedEventsDropped, "completions were dropped on a normally pumped engine");

            Object.DestroyImmediate(em.gameObject);
        }
    }
}
