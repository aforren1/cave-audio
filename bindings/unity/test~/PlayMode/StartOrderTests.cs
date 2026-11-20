// StartOrderTests.cs — the ownership decision must not depend on Awake order. A cluster toolkit's
// manager learns "am I the server" in its own Awake, and it can share the Engine's execution order
// (-100), where Unity promises no order between the two. So Engine.Awake only claims the singleton
// and registers the room, and Engine.Start asks the predicate and brings the native engine up.
//
// The fake toolkits below flip a static in Awake, and the predicate reads it. Each one wakes in the
// SAME activation as the Engine (same GameObject), which is what makes Unity order the two Awakes
// by execution order, as it does for a loaded scene.
using System.Collections;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    /// <summary>Stands in for a cluster toolkit's manager: the role is known once its Awake ran.</summary>
    public abstract class FakeToolkit : MonoBehaviour
    {
        public static bool IsServer;             // what the predicate reads
        public static bool EngineAwakeRanFirst;  // which order this run actually produced
        public static bool Woke;
        public static void Reset() { IsServer = false; EngineAwakeRanFirst = false; Woke = false; }

        void Awake()
        {
            EngineAwakeRanFirst = Engine.Instance != null;   // Engine.Awake claims Instance
            IsServer = true;
            Woke = true;
        }
    }

    [DefaultExecutionOrder(-200)] public sealed class FakeToolkitBefore : FakeToolkit {}
    [DefaultExecutionOrder(-100)] public sealed class FakeToolkitSameOrder : FakeToolkit {}
    public sealed class FakeToolkitAfter : FakeToolkit {}    // default order 0: after the Engine's -100

    /// <summary>A scene script that plays from its own Awake, in the scene that loads the Engine.</summary>
    public sealed class PlaysInAwake : MonoBehaviour
    {
        public static bool EngineWasReady, EngineWasThere;
        void Awake()
        {
            EngineWasThere = Engine.Instance != null;
            EngineWasReady = EngineWasThere && Engine.Instance.Ready;
            GetComponent<Emitter>().Play();
        }
    }

    /// <summary>A scene script that plays ANOTHER object's Emitter from its Awake.</summary>
    public sealed class PlaysOtherInAwake : MonoBehaviour
    {
        public Emitter target;
        void Awake() => target.Play();
    }

    /// <summary>A scene script that needs the engine in its own Awake and says so.</summary>
    public sealed class StartsEngineInAwake : MonoBehaviour
    {
        public static bool Ran, ReadyBefore, ReadyAfter;
        public static uint Loaded;
        void Awake()
        {
            Ran = true;
            var e = Engine.Instance;
            ReadyBefore = e.Ready;
            e.StartNow();
            ReadyAfter = e.Ready;
            Loaded = e.Load(Harness.Short);
        }
    }

    public class StartOrderTests
    {
        const float Tol = 1e-4f;
        Engine _eng;
        GameObject _origin, _extra;
        int _savedFrameRate;

        [OneTimeSetUp]
        public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
            Assert.IsTrue(Engine.Instance == null, "an earlier test left an Engine behind");
            FakeToolkit.Reset();
            _origin = new GameObject("room origin");
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 90f, 0f));
        }

        [TearDown]
        public void TearDown()
        {
            Engine.OwnsAudioPredicate = null;
            if (_extra) Object.DestroyImmediate(_extra);
            Harness.DestroyEngine(_eng); _eng = null;
            if (_origin) Object.DestroyImmediate(_origin);
            Room.UnityToRoom = Matrix4x4.identity;
            FakeToolkit.Reset();
            Application.targetFrameRate = _savedFrameRate;
        }

        // Hand-worked as in RoomOriginTests: a quarter-turned origin at (1,0,2) puts Unity +x of it at room +z.
        static void AssertRegistered(string when)
            => Assert.Less((new Vector3(0f, 0f, 1f) - Room.Pos(new Vector3(2f, 0f, 2f))).magnitude, Tol,
                           "Room Origin was not applied " + when);

        static T Child<T>(Engine e, string name) where T : Component
        {
            var go = new GameObject(name);
            go.transform.SetParent(e.transform, worldPositionStays: false);
            return go.AddComponent<T>();
        }

        // One scene load: the Engine, the toolkit, and the scene's components all wake together.
        // Whichever Awake goes first, the node must come up owning audio, with everything live.
        IEnumerator ToolkitDecidesInItsAwake<T>(bool? engineFirst) where T : FakeToolkit
        {
            Engine.OwnsAudioPredicate = () => FakeToolkit.IsServer;
            Emitter em = null; AmbisonicBed bed = null; SpeakerView view = null;
            _eng = Harness.CreateEngine(e =>
            {
                e.roomOrigin = _origin.transform;
                e.gameObject.AddComponent<T>();
                em = Child<Emitter>(e, "scene emitter");            // play-on-enable, as authored in a scene
                em.clip = Harness.Long; em.loop = true;
                bed = Child<AmbisonicBed>(e, "scene bed");
                bed.clip = Harness.BedLong; bed.loop = true;
                view = Child<SpeakerView>(e, "scene speaker view");
            });

            // Straight after the Awakes, no frame turned.
            Assert.IsTrue(FakeToolkit.Woke, "the toolkit never woke, so this test measures nothing");
            if (engineFirst.HasValue)
                Assert.AreEqual(engineFirst.Value, FakeToolkit.EngineAwakeRanFirst,
                                "Unity did not produce the Awake order this case is about");
            Harness.Note(typeof(T).Name + ": Engine.Awake ran first = " + FakeToolkit.EngineAwakeRanFirst);
            Assert.AreSame(_eng, Engine.Instance, "Awake did not claim the scene's Engine");
            Assert.IsFalse(_eng.Decided, "the ownership decision was made inside Awake");
            Assert.IsFalse(_eng.Ready);
            AssertRegistered("in Awake");

            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Decided, "Start never ran");
            Assert.IsTrue(_eng.OwnsAudio, "the server node does not own audio: the predicate was asked before the toolkit knew");
            Assert.IsTrue(_eng.Ready, "the owning node has no native engine: " + Bwa.LastError(_eng.Handle));

            bool hit = false;
            yield return Harness.Until(() => em.IsPlaying && bed.PlayheadFrames > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "components that woke with the Engine never went live");
            // The default grid has no layout file, so markers can only come from the running engine:
            // a view that built before the decision would stand empty for good.
            yield return Harness.Until(() => view.transform.childCount > 0, 3.0, v => hit = v);
            Assert.AreEqual((int)_eng.ChannelCount, view.transform.childCount, "SpeakerView built before the Engine decided");
        }

        [UnityTest] public IEnumerator ToolkitAwakeAfterTheEngineStillOwns()  => ToolkitDecidesInItsAwake<FakeToolkitAfter>(true);
        [UnityTest] public IEnumerator ToolkitAwakeBeforeTheEngineOwns()      => ToolkitDecidesInItsAwake<FakeToolkitBefore>(false);
        // MiddleVR's case. Unity promises no order here, so none is asserted: the result must not care.
        [UnityTest] public IEnumerator ToolkitAtTheEnginesOwnOrderStillOwns() => ToolkitDecidesInItsAwake<FakeToolkitSameOrder>(null);

        // The predicate is asked once, and by Start.
        [UnityTest]
        public IEnumerator PredicateIsAskedOnceAndNotInAwake()
        {
            int asked = 0;
            Engine.OwnsAudioPredicate = () => { asked++; return true; };
            _eng = Harness.CreateEngine();
            Assert.AreEqual(0, asked, "Awake asked the predicate");
            yield return Harness.Started(_eng);
            yield return Harness.Frames(3);
            Assert.AreEqual(1, asked, "the predicate must be asked exactly once");
            Assert.IsTrue(_eng.Ready);
        }

        // A script's Awake in the Engine's own scene used to find the engine running, so its Play
        // landed. The engine is not up there now. The Play is held and lands when Start has it.
        [UnityTest]
        public IEnumerator PlayFromASceneAwakeStillPlays()
        {
            int finished = 0;
            PlaysInAwake.EngineWasReady = PlaysInAwake.EngineWasThere = false;
            _eng = Harness.CreateEngine(e =>
            {
                var em = Child<Emitter>(e, "awake player");
                em.clip = Harness.Short; em.loop = false; em.playOnEnable = false;
                em.onFinished.AddListener(() => finished++);
                em.gameObject.AddComponent<PlaysInAwake>();
            });
            Assert.IsTrue(PlaysInAwake.EngineWasThere, "the script woke before the Engine, which is not this case");
            Assert.IsFalse(PlaysInAwake.EngineWasReady, "the engine was up in Awake, so nothing was held");
            bool hit = false;
            yield return Harness.Until(() => finished > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "a Play issued in a scene Awake was dropped");
            yield return Harness.Wall(0.4);
            Assert.AreEqual(1, finished, "the held Play ran more than once");
        }

        // Geometry that is already enabled when Start bakes is in that bake: one build, not one
        // plus a rebuild for the enables that came before it. Measured on the occlusion a source
        // behind the wall gets, as in LifecycleTests.
        [UnityTest]
        public IEnumerator GeometryEnabledBeforeStartIsInTheFirstBake()
        {
            _extra = new GameObject("scene things");
            var head = new GameObject("head"); head.transform.SetParent(_extra.transform);
            head.transform.position = new Vector3(0f, 1.5f, 0f);
            var wall = GameObject.CreatePrimitive(PrimitiveType.Cube); wall.transform.SetParent(_extra.transform);
            wall.transform.position = new Vector3(0f, 1.5f, 1.5f);
            wall.transform.localScale = new Vector3(4f, 4f, 0.2f);

            _eng = Harness.CreateEngine(e => e.listener = head.transform);
            wall.AddComponent<AcousticGeometry>();                   // enabled after Awake, before Start
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            Assert.AreEqual(1, _eng.StaticRebuilds, "Start did not bake the geometry that was already enabled");

            var em = Harness.CreateEmitter(Harness.Long, loop: true);
            em.transform.SetParent(_extra.transform);
            em.transform.position = new Vector3(0f, 1.5f, 3f);
            em.occlusion = true;
            em.Play();
            em.ApplyDesc();
            bool hit = false;
            yield return Harness.Until(() => em.Occlusion < 0.5f, 3.0, v => hit = v);
            Assert.IsTrue(hit, "a wall enabled before Start does not occlude: " + em.Occlusion);
            Assert.AreEqual(1, _eng.StaticRebuilds, "the pre-Start enable cost a second build");
        }

        // The way out for a toolkit that learns its role later than Awake: the Engine component stays
        // disabled, Unity holds Start, and everything waits. Room Origin is applied all the same.
        [UnityTest]
        public IEnumerator DisabledEngineWaitsForItsEnable()
        {
            int asked = 0;
            Engine.OwnsAudioPredicate = () => { asked++; return FakeToolkit.IsServer; };
            _eng = Harness.CreateEngine(e => { e.roomOrigin = _origin.transform; e.enabled = false; });
            var em = Harness.CreateEmitter(Harness.Long, loop: true);
            _extra = em.gameObject;
            em.playOnEnable = true;

            yield return Harness.Frames(3);
            long waited = Engine.ComponentWaitFrames;
            yield return Harness.Frames(5);
            Assert.AreSame(_eng, Engine.Instance);
            Assert.IsFalse(_eng.Decided, "a disabled Engine decided anyway");
            Assert.AreEqual(0, asked, "a disabled Engine asked the predicate");
            AssertRegistered("on a disabled Engine");
            Assert.Greater(Engine.ComponentWaitFrames, waited, "the emitter gave up on an Engine that had not decided");

            FakeToolkit.IsServer = true;                             // the toolkit knows now
            _eng.enabled = true;
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.OwnsAudio && _eng.Ready, "the Engine did not start on its enable");
            bool hit = false;
            yield return Harness.Until(() => em.IsPlaying, 3.0, v => hit = v);
            Assert.IsTrue(hit, "the emitter that waited never went live");
        }

        // StartNow is the explicit way to have the engine in an Awake. It brings it up on the spot,
        // and Unity's own Start, which still comes, must not bring it up a second time.
        [UnityTest]
        public IEnumerator StartNowFromAnAwakeIsReadyThereAndRunsOnce()
        {
            int asked = 0;
            Engine.OwnsAudioPredicate = () => { asked++; return true; };
            StartsEngineInAwake.Ran = StartsEngineInAwake.ReadyBefore = StartsEngineInAwake.ReadyAfter = false;
            StartsEngineInAwake.Loaded = 0;
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine(e => Child<StartsEngineInAwake>(e, "needs the engine in Awake"));
                // Straight after the Awakes, no frame turned.
                Assert.IsTrue(StartsEngineInAwake.Ran, "the script never woke");
                Assert.IsFalse(StartsEngineInAwake.ReadyBefore, "the engine was up before StartNow, so this test measures nothing");
                Assert.IsTrue(StartsEngineInAwake.ReadyAfter, "StartNow did not bring the engine up in the caller's Awake");
                Assert.AreNotEqual(0u, StartsEngineInAwake.Loaded, "a load straight after StartNow failed");
                Assert.IsTrue(_eng.Decided && _eng.OwnsAudio);
                Assert.AreEqual(1, asked);
                System.IntPtr handle = _eng.Handle;

                yield return Harness.Frames(3);                      // Unity's Start has come and gone
                _eng.StartNow();                                     // and a second explicit call
                Assert.AreEqual(1, asked, "the bring-up ran again: the predicate was asked twice");
                Assert.AreEqual(1, log.Count("[bw_audio] started"), "the native engine was started twice");
                Assert.AreEqual(handle, _eng.Handle, "the native engine was replaced");
                Assert.AreEqual("", log.Problems());
            }
        }

        // A disabled Engine can wait minutes for its enable. Calls made meanwhile are NOT held: held
        // that long they would all replay in one frame at Start, and a Stop per frame would grow the
        // list without bound. They are the no-ops they are with no Engine in the scene.
        [UnityTest]
        public IEnumerator CallsAreNotHeldWhileTheEngineIsDisabled()
        {
            _eng = Harness.CreateEngine(e => e.enabled = false);
            var em = Harness.CreateEmitter(Harness.Long, loop: true);   // no play-on-enable
            _extra = em.gameObject;
            yield return Harness.Frames(2);
            em.Play();                                               // the Engine is not about to start
            yield return Harness.Frames(2);

            _eng.enabled = true;
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            yield return Harness.Wall(0.3);
            Assert.IsFalse(em.IsPlaying, "a Play made while the Engine was disabled was held and replayed at Start");
            em.Play();                                               // the emitter itself is fine
            bool hit = false;
            yield return Harness.Until(() => em.IsPlaying, 3.0, v => hit = v);
            Assert.IsTrue(hit, "the emitter cannot play at all, so the check above measured nothing");
        }

        // A held Play that the Engine drops (its Emitter is inactive when Start runs) must not have
        // used up that Emitter's play-on-enable: the first real enable still auto-plays.
        [UnityTest]
        public IEnumerator DroppedHeldPlayLeavesPlayOnEnable()
        {
            int finished = 0;
            GameObject sleeper = null;
            _eng = Harness.CreateEngine(e =>
            {
                var em = Child<Emitter>(e, "inactive emitter");      // play-on-enable, as authored
                em.clip = Harness.Short; em.loop = false;
                em.onFinished.AddListener(() => finished++);
                sleeper = em.gameObject;
                sleeper.SetActive(false);
                Child<PlaysOtherInAwake>(e, "awake player").target = em;
            });
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            yield return Harness.Wall(0.5);
            Assert.AreEqual(0, finished, "an inactive Emitter played");

            sleeper.SetActive(true);
            bool hit = false;
            yield return Harness.Until(() => finished > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "a held Play that was dropped cost the Emitter its play-on-enable");
        }

        // Paused has no inspector field, so no settings push carries it. Set before Start, it must
        // still reach the engine.
        [UnityTest]
        public IEnumerator PausedSetBeforeStartReachesTheEngine()
        {
            _eng = Harness.CreateEngine();
            _eng.Paused = true;                                      // before Start
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            var em = Harness.CreateEmitter(Harness.Long, loop: true);
            _extra = em.gameObject;
            em.Play();
            yield return Harness.Wall(0.3);
            ulong held = em.PlayheadFrames;
            yield return Harness.Wall(0.3);
            Assert.AreEqual(held, em.PlayheadFrames, "Paused set before Start reads true, but the engine is running");

            _eng.Paused = false;
            yield return Harness.Wall(0.3);
            Assert.Greater(em.PlayheadFrames, held + 4800, "the voice never ran, so the check above measured nothing");
        }

        // An Engine call that needs the native engine does nothing before Start. It says so, once.
        [UnityTest]
        public IEnumerator EngineCallBeforeStartWarnsOnce()
        {
            using (var log = new Harness.LogCapture())
            {
                _eng = Harness.CreateEngine();
                Assert.AreEqual(0u, _eng.Load(Harness.Short), "a load landed before Start");
                _eng.SetGroupGain(0, 0.5f);
                _eng.StopAll();
                Assert.AreEqual(1, log.Count("before the Engine's Start"), "want one warning:\n" + string.Join("\n", log.Lines));
                Assert.AreEqual(1, log.Count("Engine.Load was called before"), "the warning does not name the call");

                yield return Harness.Started(_eng);
                Assert.AreNotEqual(0u, _eng.Load(Harness.Short), "the load fails after Start too: " + Bwa.LastError(_eng.Handle));
                _eng.SetGroupGain(0, 1f);
                Assert.AreEqual(1, log.Count("before the Engine's Start"), "warned after Start");
            }
        }

        // A second scene's Engine destroys itself in Awake, but Destroy waits for the end of the
        // frame and Unity may still call its Start in between. It must not ask the predicate or
        // bring up a second native engine. Start is invoked here the way Unity would, because a
        // duplicate activated from a test coroutine is destroyed before its Start comes.
        [UnityTest]
        public IEnumerator DuplicateEngineNeverStarts()
        {
            int asked = 0;
            Engine.OwnsAudioPredicate = () => { asked++; return true; };
            _eng = Harness.CreateEngine();
            yield return Harness.Started(_eng);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            Assert.AreEqual(1, asked);

            var dupGo = new GameObject("scene B engine");
            dupGo.SetActive(false);
            var dup = dupGo.AddComponent<Engine>();
            dup.sink = BwaSinkType.Null; dup.profile = BwaProfile.Cave; dup.layoutFile = "";
            dup.listener = dupGo.transform;
            dupGo.SetActive(true);                                   // -> Awake -> Destroy(gameObject), pending
            var start = typeof(Engine).GetMethod("Start", BindingFlags.NonPublic | BindingFlags.Instance);
            Assert.IsNotNull(start, "Engine.Start is gone or renamed");
            start.Invoke(dup, null);
            dup.StartNow();                                          // the public form, same guard
            Assert.AreEqual(1, asked, "the duplicate asked the predicate");
            Assert.IsFalse(dup.Decided, "the duplicate decided");
            Assert.IsFalse(dup.Ready, "the duplicate brought up a second native engine");
            Assert.AreSame(_eng, Engine.Instance);
            yield return null;
            yield return null;
            Assert.IsTrue(dup == null, "the duplicate Engine is still alive");
        }

        // A throw inside Start must end as a failed start: logged, no native engine left reading as
        // Ready, and nothing waiting. The throw here is a null string field read after bwa_start
        // (a script cleared natnetServer), so the engine is created AND running when it comes.
        // A throw between bwa_create and bwa_start takes the same catch. Nothing in that window can
        // be made to throw from a test: Unity swallows an exception from a log handler.
        [UnityTest]
        public IEnumerator ThrowDuringBringUpEndsAsAFailedStart()
        {
            LogAssert.Expect(LogType.Exception, new Regex("NullReferenceException"));
            _eng = Harness.CreateEngine(e => { e.feedListener = false; e.natnetServer = null; });
            yield return Harness.Started(_eng);

            Assert.IsTrue(_eng.Decided, "a throw in Start left the Engine undecided");
            Assert.IsTrue(_eng.OwnsAudio, "the throw came before the ownership decision, which is not this case");
            Assert.IsFalse(_eng.Ready, "an engine whose bring-up threw reads as Ready");
            Assert.AreEqual(System.IntPtr.Zero, _eng.Handle);

            var em = Harness.CreateEmitter(Harness.Long, loop: true);
            _extra = em.gameObject;
            yield return Harness.Frames(3);
            long waited = Engine.ComponentWaitFrames;
            yield return Harness.Frames(5);
            Assert.AreEqual(waited, Engine.ComponentWaitFrames, "components keep polling an Engine whose start threw");
        }
    }
}
