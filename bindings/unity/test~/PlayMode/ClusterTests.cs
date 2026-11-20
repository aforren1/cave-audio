// ClusterTests.cs — an Engine that does not own audio. In a cluster every render node runs the same
// scene and exactly one may open the device. The others keep their Engine for the room registration
// (every node draws with Room.FromRoom) and must leave every BwAudio component a silent no-op.
using System.Collections;
using System.IO;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class ClusterTests
    {
        const float Tol = 1e-4f;
        const string Layout4 = "bwa_test_layout4.json";   // four speakers, read from the FILE on a non-owner
        Engine _eng;
        GameObject _origin, _things;

        [OneTimeSetUp]
        public void OneTimeSetUp()
        {
            Harness.WriteFixtures();
            File.WriteAllText(Path.Combine(Application.streamingAssetsPath, Layout4),
                "{\"speakers\":[{\"index\":0,\"position\":[1,1.5,1]},{\"index\":1,\"position\":[-1,1.5,1]}," +
                "{\"index\":2,\"position\":[1,1.5,-1]},{\"index\":3,\"position\":[-1,1.5,-1]}]}");
        }

        [TearDown]
        public void TearDown()
        {
            Engine.OwnsAudioPredicate = null;
            if (_things) Object.DestroyImmediate(_things);
            Harness.DestroyEngine(_eng); _eng = null;
            if (_origin) Object.DestroyImmediate(_origin);
            Room.UnityToRoom = Matrix4x4.identity;
        }

        static void AssertQuarterTurnRegistration(string when)
            // Hand-worked as in RoomOriginTests: a quarter-turned origin at (1,0,2) puts Unity +x of it at room +z.
            => Assert.Less((new Vector3(0f, 0f, 1f) - Room.Pos(new Vector3(2f, 0f, 2f))).magnitude, Tol,
                           "Room Origin was not applied " + when);

        GameObject QuarterTurnOrigin()
        {
            _origin = new GameObject("room origin");
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 90f, 0f));
            return _origin;
        }

        // The predicate is the cluster hook: the toolkit's "am I the server" call, set before scene
        // load. It says no here, with the field left at its default.
        [UnityTest]
        public IEnumerator NonOwningEngineOpensNothingAndStillRegistersTheRoom()
        {
            QuarterTurnOrigin();
            Engine.OwnsAudioPredicate = () => false;
            _eng = Harness.CreateEngine(e => { e.roomOrigin = _origin.transform; e.layoutFile = Layout4; });

            Assert.IsFalse(_eng.OwnsAudio, "the predicate said no, and the Engine took the device anyway");
            Assert.IsFalse(_eng.Ready, "a non-owning Engine created a native engine");
            Assert.AreEqual(System.IntPtr.Zero, _eng.Handle);
            Assert.AreSame(_eng, Engine.Instance, "a non-owning Engine must still be the scene's Engine");
            AssertQuarterTurnRegistration("on a node that does not own audio");

            // Every component kind, with play-on-enable and clips that do not exist: an owning Engine
            // would warn about each load. Here nothing may load, warn, play, or keep waiting.
            using (var log = new Harness.LogCapture())
            {
                _things = new GameObject("cluster client things");
                var em = _things.AddComponent<Emitter>();
                em.clip = "bwa_test_missing.wav";
                var bed = _things.AddComponent<AmbisonicBed>();
                bed.clip = "bwa_test_missing_bed.wav";
                var push = new GameObject("push").AddComponent<PushEmitter>();
                push.transform.SetParent(_things.transform);
                var geo = GameObject.CreatePrimitive(PrimitiveType.Cube);
                geo.transform.SetParent(_things.transform);
                geo.AddComponent<AcousticGeometry>();
                var dyn = GameObject.CreatePrimitive(PrimitiveType.Cube);
                dyn.transform.SetParent(_things.transform);
                dyn.AddComponent<DynamicAcousticGeometry>();
                var view = new GameObject("speaker view").AddComponent<SpeakerView>();
                view.transform.SetParent(_things.transform);
                yield return Harness.Frames(3);

                long waited = Engine.ComponentWaitFrames;
                yield return Harness.Frames(5);
                Assert.AreEqual(waited, Engine.ComponentWaitFrames, "components keep polling for an engine that will never come");

                em.Play();
                Assert.IsFalse(em.PlayOneShot(), "a one-shot was accepted with no engine");
                Assert.AreEqual(0, push.Push(new float[256]), "a push landed with no engine");
                yield return Harness.Frames(3);
                Assert.IsFalse(em.IsPlaying);
                Assert.AreEqual(0ul, bed.PlayheadFrames);
                // The array still shows on every node, from the layout file, unlit.
                Assert.AreEqual(4, view.transform.childCount, "SpeakerView drew no markers on a node without an engine");
                Assert.AreEqual("", log.Problems(), "a component on a non-owning node complained");
            }

            // The registration keeps following the origin, as on the owning node.
            _origin.transform.position = new Vector3(5f, 0f, 2f);
            yield return null;
            Assert.Less(Room.Pos(new Vector3(5f, 0f, 2f)).magnitude, Tol, "a moved origin was not followed");
        }

        // The field is the per-scene switch, for a project with no cluster toolkit hook.
        [Test]
        public void OwnsAudioFieldOffDeclinesToo()
        {
            _eng = Harness.CreateEngine(e => e.ownsAudio = false);
            Assert.IsFalse(_eng.OwnsAudio);
            Assert.IsFalse(_eng.Ready);
        }

        // A predicate that throws must not leave the node without an Engine: it reads as "not the owner".
        [Test]
        public void ThrowingPredicateReadsAsNotTheOwner()
        {
            Engine.OwnsAudioPredicate = () => throw new System.InvalidOperationException("toolkit not ready");
            LogAssert.Expect(LogType.Error, new Regex("OwnsAudioPredicate threw"));
            _eng = Harness.CreateEngine();
            Assert.AreSame(_eng, Engine.Instance, "a throwing predicate left the node with no Engine");
            Assert.IsFalse(_eng.OwnsAudio);
            Assert.IsFalse(_eng.Ready);
        }

        // With Domain Reload off the last session's predicate would decide this one.
        [Test]
        public void PlayEntryResetClearsThePredicate()
        {
            var reset = typeof(Engine).GetMethod("ResetOnPlay",
                System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
            Assert.IsNotNull(reset, "Engine.ResetOnPlay is gone or renamed");
            var attr = (RuntimeInitializeOnLoadMethodAttribute)System.Attribute.GetCustomAttribute(
                reset, typeof(RuntimeInitializeOnLoadMethodAttribute));
            Assert.IsNotNull(attr, "the reset is not wired to Play-mode entry");
            Assert.AreEqual(RuntimeInitializeLoadType.SubsystemRegistration, attr.loadType);
            Engine.OwnsAudioPredicate = () => false;
            reset.Invoke(null, null);
            Assert.IsNull(Engine.OwnsAudioPredicate, "the reset left the predicate");
        }

        // An owner whose start fails is still the scene's Engine and still registers the room, so the
        // nodes agree on the room, and nothing waits for it. A named sink with a device that does not
        // exist is a demand the start must refuse.
        [UnityTest]
        public IEnumerator FailedStartStillClaimsAndRegisters()
        {
            QuarterTurnOrigin();
            LogAssert.ignoreFailingMessages = true;               // the failed start logs errors on purpose
            _eng = Harness.CreateEngine(e =>
            {
                e.roomOrigin = _origin.transform;
                e.sink = BwaSinkType.Asio;
                e.device = "bwa_test_no_such_device";
            });
            Assert.IsTrue(_eng.OwnsAudio);
            Assert.IsFalse(_eng.Ready, "the start was expected to fail with a device that does not exist");
            Assert.AreSame(_eng, Engine.Instance, "a failed start left the node with no Engine");
            AssertQuarterTurnRegistration("after a failed start");

            _things = new GameObject("things");
            _things.AddComponent<Emitter>();
            yield return Harness.Frames(3);
            long waited = Engine.ComponentWaitFrames;
            yield return Harness.Frames(5);
            Assert.AreEqual(waited, Engine.ComponentWaitFrames, "components keep polling a failed Engine");
            LogAssert.ignoreFailingMessages = false;
        }
    }
}
