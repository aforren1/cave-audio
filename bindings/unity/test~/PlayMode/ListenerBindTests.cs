// ListenerBindTests.cs: the late-bound listener. A VR toolkit can create its head node after the
// scene starts, so Engine.listener cannot always be wired in the inspector. Engine.listenerName and
// Engine.ListenerProvider find it at run time: on a timer, on the node that owns audio, and again
// after the bound Transform is destroyed. Also the one registration line the Engine logs at start.
using System;
using System.Collections;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class ListenerBindTests
    {
        const string HeadName = "bwa_test_late_head";
        const string StartWarning = "no listener Transform is assigned";
        const string SearchWarning = "no listener after";

        Engine _eng;
        GameObject _head, _head2, _origin;
        int _savedFrameRate;

        static readonly FieldInfo WarnS =
            typeof(Engine).GetField("ListenerSearchWarnS", BindingFlags.NonPublic | BindingFlags.Static);
        float _savedWarnS;

        [SetUp]
        public void SetUp()
        {
            Assert.IsNotNull(WarnS, "Engine.ListenerSearchWarnS is gone or renamed");
            _savedWarnS = (float)WarnS.GetValue(null);
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
            Assert.IsNull(GameObject.Find(HeadName), "an earlier test left a head behind");
        }

        [TearDown]
        public void TearDown()
        {
            WarnS.SetValue(null, _savedWarnS);
            Engine.ListenerProvider = null;
            Engine.OwnsAudioPredicate = null;
            Harness.DestroyEngine(_eng); _eng = null;
            if (_head) UnityEngine.Object.DestroyImmediate(_head);
            if (_head2) UnityEngine.Object.DestroyImmediate(_head2);
            if (_origin) UnityEngine.Object.DestroyImmediate(_origin);
            Room.UnityToRoom = Matrix4x4.identity;
            Application.targetFrameRate = _savedFrameRate;
        }

        static GameObject Head(string name, Vector3 at)
        {
            var go = new GameObject(name);
            go.transform.position = at;
            return go;
        }

        Vector3 EngineListener()
        {
            Bwa.GetListenerPose(_eng.Handle, out var p, out _);
            return new Vector3(p[0], p[1], p[2]);
        }

        // Bound, and FED: the engine's own listener position against the Transform's, within 1 mm.
        IEnumerator Follows(GameObject head, string what)
        {
            bool hit = false;
            yield return Harness.Until(() => _eng.listener == head.transform &&
                                             (EngineListener() - Room.Pos(head.transform.position)).magnitude < 1e-3f,
                                       3.0, v => hit = v);
            Assert.AreSame(head.transform, _eng.listener, what + ": the Engine did not bind the listener");
            Assert.IsTrue(hit, what + ": the listener is bound but the engine's pose is " + EngineListener() +
                               ", not " + Room.Pos(head.transform.position));
        }

        // The MiddleVR shape: the scene starts with no head, the toolkit makes one a second later.
        [UnityTest]
        public IEnumerator NameBindsWhenTheObjectAppears()
        {
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateStartedEngine(e => { e.listener = null; e.listenerName = HeadName; });
                Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
                var sw = System.Diagnostics.Stopwatch.StartNew();
                yield return Harness.Wall(1.0);
                double waited = sw.Elapsed.TotalSeconds;      // more than 1.0 when the editor hitched
                int searched = _eng.ListenerSearches;
                Assert.IsTrue(_eng.listener == null);
                Assert.AreEqual(0, log.Count(StartWarning), "a listener that is still expected was reported as missing");
                Assert.AreEqual(0, log.Count(SearchWarning), "the search gave up after one second");
                Assert.AreEqual(1, log.Count("registration: no Room Origin (the room frame is Unity world, x mirrored); " +
                                             "no listener Transform yet."), "the start did not log the registration");

                // Four times a second, not every frame: GameObject.Find walks the scene. Bounded by
                // the time that really passed (one search per 0.25 s, the one at start, one of slack),
                // so a hitch lengthens the wait and the allowance together. Every frame would be 30.
                Assert.GreaterOrEqual(searched, 2, "the search is not running");
                Assert.LessOrEqual(searched, (int)(waited / 0.25) + 2,
                                   "the search runs more often than four times a second (" + waited.ToString("F2") + " s)");

                _head = Head(HeadName, new Vector3(0.5f, 1.4f, -0.25f));
                yield return Follows(_head, "after the head appeared");
                Assert.AreEqual(1, log.Count("listener '" + HeadName + "' at room (-0.500, 1.400, -0.250)"),
                                "the bind did not log where the registration puts the head");

                int searches = _eng.ListenerSearches;
                _head.transform.position = new Vector3(-1f, 1.5f, 0.75f);
                yield return Harness.Wall(0.7);
                Assert.AreEqual(searches, _eng.ListenerSearches, "the Engine kept looking after it bound");
                yield return Follows(_head, "after the head moved");
            }
        }

        // The provider is the code route and wins over the name. A destroyed head (the toolkit
        // rebuilt its rig) is looked for again.
        [UnityTest]
        public IEnumerator ProviderWinsAndABoundListenerThatDiesIsFoundAgain()
        {
            GameObject current = null;
            int asked = 0;
            Engine.ListenerProvider = () => { asked++; return current ? current.transform : null; };
            _head2 = Head(HeadName, new Vector3(3f, 1f, 3f));           // what the NAME would find
            _eng = Harness.CreateStartedEngine(e => { e.listener = null; e.listenerName = "bwa_test_nobody"; });
            yield return Harness.Frames(5);
            Assert.Greater(asked, 0, "the provider was never asked");
            Assert.IsTrue(_eng.listener == null, "bound with a provider that returns null and a name nobody has");

            _eng.listenerName = HeadName;
            _head = Head("provided head", new Vector3(0.25f, 1.6f, 0.5f));
            current = _head;
            yield return Follows(_head, "provider and name both resolve");

            UnityEngine.Object.DestroyImmediate(_head);
            current = null;
            _eng.listenerName = "";
            yield return Harness.Wall(0.4);
            Assert.IsTrue(_eng.listener == null);
            int before = asked;
            _head = Head("rebuilt head", new Vector3(-0.75f, 1.2f, 1f));
            current = _head;
            yield return Follows(_head, "after the first head was destroyed");
            Assert.Greater(asked, before, "the provider was not asked again after the listener died");
        }

        // Nothing ever resolves: one warning, after the wait, and the start warning stays out of it.
        // A provider that throws is one logged exception, and the name is still tried.
        [UnityTest]
        public IEnumerator WarnsOnceWhenNothingResolvesAndSurvivesAThrowingProvider()
        {
            WarnS.SetValue(null, 2.0f);
            Engine.ListenerProvider = () => throw new InvalidOperationException("bwa_test provider");
            LogAssert.Expect(LogType.Exception, new Regex("bwa_test provider"));
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateStartedEngine(e => { e.listener = null; e.listenerName = HeadName; });
                // The check is 0.3 s in and the wait is 2 s, so an editor hitch of a second and a half
                // between the start and this line still leaves it true.
                yield return Harness.Wall(0.3);
                Assert.AreEqual(0, log.Count(SearchWarning), "warned before the wait was over");
                bool warned = false;
                yield return Harness.Until(() => log.Count(SearchWarning) > 0, 8.0, v => warned = v);
                Assert.IsTrue(warned, "no warning that no listener resolved");

                // After the warning the search slows to once a second: a misspelled name must not
                // cost a scene walk four times a second for the rest of the session. Bounded by the
                // time that passed, as above. Four a second would be 8 or more here.
                int before = _eng.ListenerSearches;
                var sw = System.Diagnostics.Stopwatch.StartNew();
                yield return Harness.Wall(2.2);
                int slow = _eng.ListenerSearches - before;
                Assert.GreaterOrEqual(slow, 1, "the search stopped after the warning");
                Assert.LessOrEqual(slow, (int)sw.Elapsed.TotalSeconds + 2, "the search did not slow down after the warning");
                Assert.AreEqual(1, log.Count(SearchWarning), "expected exactly one warning that no listener resolved");
                Assert.AreEqual(1, log.Count("Engine.ListenerProvider returns null and no active GameObject is named '" + HeadName + "'"));
                Assert.AreEqual(0, log.Count(StartWarning));
                Assert.AreEqual(1, log.Count("bwa_test provider"), "the throwing provider was logged more than once");

                _head = Head(HeadName, new Vector3(0.1f, 1.3f, 0.2f));
                yield return Follows(_head, "the name, behind a provider that throws");
            }
        }

        // With neither a name nor a provider the old warning still fires at start, and nothing searches.
        [UnityTest]
        public IEnumerator NoNameAndNoProviderStillWarnsAtStart()
        {
            LogAssert.Expect(LogType.Warning, new Regex(StartWarning));
            _eng = Harness.CreateStartedEngine(e => e.listener = null);
            yield return Harness.Wall(0.6);
            Assert.AreEqual(0, _eng.ListenerSearches);
        }

        // A node that does not own audio feeds no listener, so it does not look for one.
        [UnityTest]
        public IEnumerator NonOwnerDoesNotSearch()
        {
            _head = Head(HeadName, Vector3.one);
            int asked = 0;
            Engine.ListenerProvider = () => { asked++; return _head.transform; };
            Engine.OwnsAudioPredicate = () => false;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateStartedEngine(e => { e.listener = null; e.listenerName = HeadName; });
                // Every node applies Room Origin, so every node says where it put the room.
                Assert.AreEqual(1, log.Count("registration: no Room Origin (the room frame is Unity world, x mirrored); " +
                                             "no listener is fed on this node."),
                                "a node that does not own audio did not log its registration:\n" + string.Join("\n", log.Lines));
            }
            Assert.IsFalse(_eng.Ready);
            yield return Harness.Wall(0.6);
            Assert.AreEqual(0, _eng.ListenerSearches, "a node with nothing to feed searched for a listener");
            Assert.AreEqual(0, asked);
            Assert.IsTrue(_eng.listener == null);
        }

        // The start line that makes a wrong Room Origin visible. Hand-worked as in RoomOriginTests: a
        // quarter-turned origin at (1,0,2) puts Unity (2, 1.5, 2) at room (0, 1.5, 1).
        [UnityTest]
        public IEnumerator StartLogsTheRegistrationAndTheListenersRoomPosition()
        {
            _origin = new GameObject("bwa_test_origin");
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 90f, 0f));
            _head = Head("bwa_test_head", new Vector3(2f, 1.5f, 2f));
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateStartedEngine(e => { e.listener = _head.transform; e.roomOrigin = _origin.transform; });
                Assert.IsTrue(_eng.Ready);
                yield return Harness.Frames(3);
                Assert.AreEqual(1, log.Count("[bw_audio] registration: "), "expected one registration line at start");
                // -?0.000: a quarter turn leaves a float epsilon where a zero belongs, of either sign.
                var want = new Regex(@"registration: Room Origin 'bwa_test_origin' at world \(1\.000, -?0\.000, 2\.000\), " +
                                     @"yaw 90\.0 deg; listener 'bwa_test_head' at room \(-?0\.000, 1\.500, 1\.000\)\.");
                int n = 0;
                foreach (var l in log.Lines) if (want.IsMatch(l)) n++;
                Assert.AreEqual(1, n, "the registration line is wrong:\n" + string.Join("\n", log.Lines));
            }
        }
    }
}
