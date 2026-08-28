// ChannelRouteTests.cs — SourceBase.Channel, the direct output-channel route.
//
// What is under test is the REFUSAL, not the routing: the engine already refuses an index outside
// [0, ChannelCount) and every negative but CHANNEL_AUTO, but it refuses into Bwa.LastError, where
// nothing reading this property would ever see it. If the setter cached the value anyway, the getter
// would report a speaker the voice is not on — and a reference source that quietly stays PANNED is
// read as the single-speaker ground truth in an experiment. The property therefore keeps the channel
// it had and says so, exactly as Godot's BwaSource.set_channel does. The two bindings must not
// disagree about what a route means.
//
// The warnings are asserted with LogAssert.Expect rather than ignored, so "it says so" is a claim
// this suite can fail on: an expected log that never arrives fails the test at teardown.
using System.Collections;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class ChannelRouteTests
    {
        Engine _eng;
        Emitter _em;
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
            if (_em) Object.DestroyImmediate(_em.gameObject);
            Harness.DestroyEngine(_eng);
            _em = null; _eng = null;
            Application.targetFrameRate = _savedFrameRate;
        }

        [UnityTest]
        public IEnumerator ChannelRefusesOutOfRangeAndKeepsTheOldRoute()
        {
            _em = Harness.CreateEmitter(Harness.Long, loop: false);
            yield return Harness.Frames(3);                    // let SourceBase.TryInit mint the voice

            int count = (int)_eng.ChannelCount;
            Assert.Greater(count, 1, "the default grid should report more than one channel; without a " +
                                     "range to be outside of, the rest of this test proves nothing");

            _em.Channel = 1;
            Assert.AreEqual(1, _em.Channel, "a valid channel did not round-trip");

            LogAssert.Expect(LogType.Warning, new Regex("out of range"));
            _em.Channel = count + 500;
            Assert.AreEqual(1, _em.Channel, "an out-of-range channel was CACHED: the getter now reports " +
                                            "a route the voice is not on, which is the whole failure");

            // CHANNEL_AUTO is the one negative that means anything, so every other one is refused the
            // same way. A guard that bounds only the top end lets -5 through, and then the engine
            // refuses it into Bwa.LastError while this property answers -5.
            LogAssert.Expect(LogType.Warning, new Regex("negative"));
            _em.Channel = -5;
            Assert.AreEqual(1, _em.Channel, "a negative that is not CHANNEL_AUTO was CACHED");

            _em.Channel = Bwa.CHANNEL_AUTO;
            Assert.AreEqual(Bwa.CHANNEL_AUTO, _em.Channel, "CHANNEL_AUTO did not round-trip");
        }
    }
}
