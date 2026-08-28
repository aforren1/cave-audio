// CompletionTests.cs — the completion and loop feeds on an Emitter: bwa_poll_ended -> onFinished,
// bwa_poll_looped -> onLoop, and the explicit-halt fallback that covers the one end the engine does
// not report as an event.
//
// Several tests here turn the Engine component OFF for a stretch. That is the ordering trick the
// suite is built on, not a shortcut. bwa_source_play reaches the audio thread on its own (cmd_push
// publishes immediately; only bwa_commit is frame-gated), so with the pump off a voice still starts,
// plays and ends — while NOTHING calls bwa_commit, bwa_poll_ended or bwa_source_is_playing. Turning
// the pump back on then produces exactly ONE pump against a known state, which is what makes a
// "fires exactly once" claim decidable rather than a race the test happens to win.
using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class CompletionTests
    {
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

        // ---- 1. the sub-frame clip ---------------------------------------------------------------

        // The case the drain replaced the IsPlaying edge FOR. A clip this short begins and ends
        // entirely between two pumps, so no is-playing poll can ever observe it. The event feed is the
        // only thing that can report it, and this test fails if that feed is broken.
        [UnityTest]
        public IEnumerator SubFrameClipFiresFinishedExactlyOnce()
        {
            var em = Arm(Harness.SubFrame, loop: false);
            yield return Harness.Frames(3);                       // let SourceBase.TryInit mint the voice
            Assert.AreNotEqual(0ul, _eng.SoundFrames(Harness.SubFrame), "the fixture clip did not load");

            // Pump off: nothing commits, nothing drains, nothing polls is-playing.
            _eng.enabled = false;
            em.Play();
            yield return Harness.Wall(0.30);                      // >> the clip's 1.33 ms, on wall time

            Assert.AreEqual(0, _finished, "onFinished fired with the pump off - something other than " +
                                          "Engine's LateUpdate is driving it");
            // Not playing at the first pump opportunity: an is-playing edge detector reads false here
            // and false forever after, so it could never report this clip. The claim is not vacuous
            // for a source that failed to create - such a source also reads false, but then cannot
            // produce the completion the next assertion demands.
            Assert.IsFalse(em.IsPlaying, "the voice was still playing 0.30 s after a 1.33 ms clip started; " +
                                         "this test cannot claim to exercise the sub-frame path");

            _eng.enabled = true;
            yield return null;                                    // exactly ONE pump
            Assert.AreEqual(1, _finished, "the sub-frame clip's completion never reached onFinished");

            yield return Harness.Frames(20);
            Assert.AreEqual(1, _finished, "onFinished fired again after the completion was reported");
        }

        // The margin behind the test above, asserted rather than assumed: the clip really is shorter
        // than a frame at the engine's rate, by a wide factor, at the frame rate this run produced.
        [UnityTest]
        public IEnumerator SubFrameClipIsActuallySubFrame()
        {
            ulong frames = _eng.SoundFrames(Harness.SubFrame);
            Assert.AreEqual((ulong)Harness.SubFrameFrames, frames,
                            "the fixture is not the length this suite reasons about");
            double clipSeconds = frames / (double)_eng.sampleRate;

            double interval = 0.0;
            yield return Harness.MeasureFrameInterval(31, v => interval = v);
            Harness.Note($"clip = {clipSeconds * 1000.0:F3} ms, median frame = {interval * 1000.0:F3} ms, " +
                         $"ratio = {interval / clipSeconds:F1}x");

            Assert.Less(clipSeconds * 8.0, interval,
                        $"the clip ({clipSeconds * 1000.0:F3} ms) is not comfortably shorter than a frame " +
                        $"({interval * 1000.0:F3} ms), so SubFrameClipFiresFinishedExactlyOnce is not " +
                        "exercising the sub-frame path");
        }

        // ---- 2. a natural end fires once, not twice ----------------------------------------------

        // Both feeds are live here and both are ARMED: the pump runs long enough for the halt
        // fallback to latch _wasPlaying, then goes off across the clip's end so the completion event
        // is waiting when it comes back. One pump, both feeds able to fire, exactly one callback.
        [UnityTest]
        public IEnumerator NaturalEndFiresOnceWithBothFeedsArmed()
        {
            var em = Arm(Harness.Short, loop: false);             // 256 ms
            yield return Harness.Frames(3);
            em.Play();

            bool sawPlaying = false;
            yield return Harness.Until(() => em.IsPlaying, 1.0, v => sawPlaying = v);
            Assert.IsTrue(sawPlaying, "the voice never read as playing, so the halt fallback never armed " +
                                      "and this test would pass without the duplicate suppression");
            yield return Harness.Frames(2);                       // ... and let a pump SEE it playing

            _eng.enabled = false;                                 // the end happens with nobody watching
            yield return Harness.Wall(0.50);                      // > the clip's remaining 256 ms
            Assert.AreEqual(0, _finished, "onFinished fired with the pump off");

            _eng.enabled = true;
            yield return null;                                    // one pump: drain, then post-commit
            Assert.AreEqual(1, _finished, "a natural end did not reach onFinished");

            yield return Harness.Frames(20);
            Assert.AreEqual(1, _finished, "a natural end was reported TWICE - the event feed and the " +
                                          "halt fallback both fired for the same end");
        }

        // ---- 3. an explicit Stop still finishes ---------------------------------------------------

        // Unity's documented contract, and the reason the halt fallback survives the event feed: the
        // engine posts NO completion for a click-free stop, so only the is-playing edge can report it.
        [UnityTest]
        public IEnumerator ExplicitStopFiresFinished()
        {
            var em = Arm(Harness.Long, loop: false);              // 2 s, so nothing ends on its own
            yield return Harness.Frames(3);
            em.Play();

            bool sawPlaying = false;
            yield return Harness.Until(() => em.IsPlaying, 1.0, v => sawPlaying = v);
            Assert.IsTrue(sawPlaying, "the voice never read as playing");
            yield return Harness.Frames(2);
            Assert.AreEqual(0, _finished, "onFinished fired before the Stop");

            em.Stop();
            yield return Harness.Frames(10);
            Assert.AreEqual(1, _finished, "Stop() did not fire onFinished");

            yield return Harness.Frames(20);
            Assert.AreEqual(1, _finished, "Stop() fired onFinished more than once");
        }

        // ---- 4. a stop landing on the clip's final block ------------------------------------------

        // rt.c's seam: a voice that is STOPPING and runs out of asset in the same block still posts a
        // completion, so both feeds can describe one end. Scheduled on the dsp clock so the two land
        // together by construction rather than by luck.
        //
        // What this test can and cannot say: it pins "exactly one callback", which is the contract. It
        // cannot say WHICH feed reported - that depends on where the audio thread was when the pump
        // read is-playing, and the binding exposes no way to tell them apart.
        [UnityTest]
        public IEnumerator StopOnFinalBlockFiresOnce()
        {
            var em = Arm(Harness.Short, loop: false);
            yield return Harness.Frames(3);

            ulong t0 = _eng.DspTimeFrames + Harness.SampleRate / 2;   // half a second of margin
            em.PlayAt(t0);
            em.StopAt(t0 + Harness.ShortFrames - 1);                  // inside the block the clip ends in

            bool ended = false;
            yield return Harness.Until(() => _finished > 0, 3.0, v => ended = v);
            Assert.IsTrue(ended, "the scheduled play/stop pair never reported an end");
            Assert.AreEqual(1, _finished, "the stop-on-final-block end was reported more than once");

            yield return Harness.Frames(30);
            Assert.AreEqual(1, _finished, "a second callback arrived after the stop-on-final-block end");
        }

        // The duplicate-suppression latch must not eat a LATER end. A Stop arms it (the fallback
        // reports, and sets the latch); the next play must clear it, or the sub-frame completion that
        // follows is swallowed and onFinished never fires again for this source.
        [UnityTest]
        public IEnumerator LatchDoesNotEatTheNextEnd()
        {
            var em = Arm(Harness.Long, loop: false);
            yield return Harness.Frames(3);
            em.Play();

            bool sawPlaying = false;
            yield return Harness.Until(() => em.IsPlaying, 1.0, v => sawPlaying = v);
            Assert.IsTrue(sawPlaying, "the voice never read as playing");
            yield return Harness.Frames(2);

            em.Stop();                                            // -> the halt fallback fires and latches
            yield return Harness.Frames(10);
            Assert.AreEqual(1, _finished, "Stop() did not fire onFinished, so the latch was never armed " +
                                          "and the rest of this test proves nothing");

            em.clip = Harness.SubFrame;                           // a second end, reported by the EVENT feed
            _eng.enabled = false;
            em.Play();
            yield return Harness.Wall(0.30);
            _eng.enabled = true;
            yield return null;

            Assert.AreEqual(2, _finished, "the latch armed by Stop() swallowed the NEXT end - a fresh " +
                                          "play must clear it");
        }

        // A play the engine is still HOLDING has not bound, so it must not void the suppression the
        // last end armed - and it must void it the moment it DOES bind, or the clip it eventually
        // plays reports no end at all. This pins the second half, which is the half a fix gets wrong.
        //
        // The ordering is what makes the claim decidable, and every step of it matters:
        //
        //   * Stop() with the pump ON arms the latch (the halt fallback reports, and latches).
        //   * The pump then goes OFF, so nothing commits, drains, or reads is-playing.
        //   * The play issued there is HELD by construction: bwa_sound_acquire_async only QUEUES the
        //     decode, and nothing between it and the play is a pump point that could adopt one this
        //     young. The assertion right after says so out loud - a play that resolved READY would
        //     take the ordinary bind path and this test would pass without exercising the hold.
        //   * Engine.Find is a pump point (a pure by-path probe), so it adopts the decode and releases
        //     the held play WITHOUT turning a frame. The sub-frame clip then plays and ends with
        //     nobody watching, exactly as SubFrameClipFiresFinishedExactlyOnce arranges.
        //   * Turning the pump back on gives ONE pump. Push resolves the pending handle (and voids the
        //     latch), then the drain reports the end. The halt fallback cannot cover for it: the pump
        //     was off for the whole life of this play, so no post-commit pass ever saw it playing and
        //     _wasPlaying never latched. That is what makes the event feed the only feed here, and the
        //     latch the only thing standing in its way. It is also what the falsification shows: pin
        //     _endFired on and the count stops at 1, where an armed fallback would have carried it to
        //     2 regardless and the test would have proved nothing.
        //
        // What this suite does NOT claim to cover is the double-fire the FIRST half prevents. Unity
        // arms the latch only inside Emitter.PostCommit, which Engine runs AFTER the ended drain in
        // the same LateUpdate, so for the halt fallback to report an end whose completion event is
        // still undrained, the engine has to post that event in the microseconds BETWEEN those two
        // calls. No ordering this harness can impose widens that window - turning the pump off delays
        // both halves together, and the drain then wins every time - so the gap is stated here rather
        // than papered over with a test that would pass either way. Godot's demo/api.gd covers that
        // half deterministically, because BwaEmitter.stop() arms its latch synchronously.
        [UnityTest]
        public IEnumerator HeldPlayVoidsSuppressionOnceItBinds()
        {
            var em = Arm(Harness.Long, loop: false);              // 2 s, so nothing ends on its own
            yield return Harness.Frames(3);
            em.Play();

            bool sawPlaying = false;
            yield return Harness.Until(() => em.IsPlaying, 1.0, v => sawPlaying = v);
            Assert.IsTrue(sawPlaying, "the voice never read as playing, so the halt fallback never armed " +
                                      "and the latch this test is about was never set");
            yield return Harness.Frames(2);

            em.Stop();                                            // the fallback reports AND latches
            yield return Harness.Frames(10);
            Assert.AreEqual(1, _finished, "Stop() did not fire onFinished, so the latch was never armed " +
                                          "and the rest of this test proves nothing");

            _eng.enabled = false;                                 // no commit, no drain, no is-playing read
            em.loadAsync = true;
            em.clip = Harness.SubFrame;                           // not resident: every test gets a fresh engine
            em.Play();
            Assert.IsFalse(em.IsPlaying, "the async play was not HELD - the decode beat the play, so this " +
                                         "run did not exercise the held path at all");

            // Adopt the decode without turning a frame, then let the 1.33 ms clip come and go unseen.
            bool bound = false;
            yield return Harness.Until(() => { _eng.Find(Harness.SubFrame); return em.IsPlaying; },
                                       2.0, v => bound = v);
            Assert.IsTrue(bound, "the held play never bound: Engine.Find is the pump point that adopts a " +
                                 "finished decode and releases it, and nothing here can proceed without it");
            yield return Harness.Wall(0.30);                      // >> the clip's 1.33 ms
            Assert.AreEqual(1, _finished, "onFinished fired with the pump off");

            _eng.enabled = true;
            yield return null;                                    // exactly ONE pump
            Assert.AreEqual(2, _finished, "the held play's own end was SWALLOWED: a play that binds must " +
                                          "void the previous end's suppression, or onFinished never fires " +
                                          "for this source again");

            yield return Harness.Frames(20);
            Assert.AreEqual(2, _finished, "the held play's end was reported more than once");
        }

        // ---- 5. a looping source wraps and never finishes ------------------------------------------

        [UnityTest]
        public IEnumerator LoopingSourceFiresLoopNeverFinished()
        {
            var em = Arm(Harness.Short, loop: true);              // 256 ms per pass
            yield return Harness.Frames(3);
            em.Play();

            yield return Harness.Wall(1.6);                       // ~6 wraps at 256 ms

            Harness.Note($"looping source: onLoop x{_looped}, onFinished x{_finished}, " +
                         $"dropped {_eng.LoopEventsDropped}");
            Assert.GreaterOrEqual(_looped, 4, "a 256 ms clip looped for 1.6 s should wrap about six " +
                                              "times; onLoop is not being delivered");
            // Not a coin flip: the assertion above proves the voice ran and wrapped repeatedly, so a
            // zero here is the absence of a completion, not the absence of playback.
            Assert.AreEqual(0, _finished, "a LOOPING voice reported onFinished - it never ends");
            Assert.IsTrue(em.IsPlaying, "the looping voice stopped on its own");
        }
    }
}
