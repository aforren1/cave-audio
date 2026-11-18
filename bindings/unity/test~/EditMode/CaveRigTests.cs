// CaveRigTests.cs - GameObject > BwAudio > CAVE Rig, through CaveRig.Create (the menu item is that
// call plus the Unity-audio question, which needs a person).
//
// EditMode on purpose: the rig is editor code, and an Engine added in the stopped editor never runs
// Awake, so no native engine starts and no test here needs the DLL to do anything.
//
// Every test starts from an EMPTY scene. The editor's default scene has a Main Camera, which would
// decide the listener test before it ran.
using System.Collections;
using System.IO;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using BwAudio.EditorTools;

namespace BwAudio.Tests
{
    public class CaveRigTests
    {
        static readonly string[] Files = { "trims.json", "cave_layout.json", "constraints.json" };
        static string Dir => Application.streamingAssetsPath;
        const string SceneDir = "Assets/RigTestScenes";      // the driver clears it before every run

        [SetUp]
        public void SetUp()
        {
            // RefusesInPlayMode reloads the domain, and the framework runs SetUp again inside Play
            // mode, where the editor scene calls below throw.
            if (Application.isPlaying) return;
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            Directory.CreateDirectory(Dir);
            foreach (var f in Files)
                Assert.IsFalse(File.Exists(Path.Combine(Dir, f)),
                               f + " is already in StreamingAssets: this suite writes and deletes it, " +
                               "so it only runs in the scratch project");
        }

        [TearDown]
        public void TearDown()
        {
            foreach (var f in Files) File.Delete(Path.Combine(Dir, f));
            if (Application.isPlaying) return;               // a failed RefusesInPlayMode; see SetUp
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            AssetDatabase.DeleteAsset(SceneDir);
            Room.UnityToRoom = Matrix4x4.identity;
        }

        static void Put(string name) => File.WriteAllText(Path.Combine(Dir, name), "{}");

        static void ExpectNoLayoutWarning() => LogAssert.Expect(LogType.Warning, new Regex("default grid"));
        static void ExpectNoCameraWarning() => LogAssert.Expect(LogType.Warning, new Regex("no camera is tagged MainCamera"));
        // Every successful Create says what to do next. Expected by name, so that
        // LogAssert.NoUnexpectedReceived can refuse everything else: a warning that has become
        // unconditional would otherwise pass every test that happens not to look for it.
        static void ExpectCreated() => LogAssert.Expect(LogType.Log, new Regex("CAVE Rig created"));

        static GameObject MainCamera(string name)
        {
            var cam = new GameObject(name, typeof(Camera));
            cam.tag = "MainCamera";
            return cam;
        }

        static Engine[] Engines() => Object.FindObjectsByType<Engine>(FindObjectsInactive.Include, FindObjectsSortMode.None);

        [Test]
        public void BuildsTheHierarchyAndAssignsTheOrigin()
        {
            ExpectNoLayoutWarning(); ExpectNoCameraWarning();
            var eng = CaveRig.Create(out bool created);

            Assert.IsTrue(created);
            Assert.IsNotNull(eng);
            Assert.AreEqual("BwAudio", eng.name);
            Assert.IsNull(eng.transform.parent, "the Engine object must be a scene root (DontDestroyOnLoad)");
            Assert.AreEqual(Vector3.zero, eng.transform.position);

            var origin = eng.transform.Find("Room Origin");
            Assert.IsNotNull(origin, "no 'Room Origin' child under the Engine object");
            Assert.AreSame(origin, eng.roomOrigin, "Engine.roomOrigin is not the Room Origin child");
            Assert.AreEqual(Vector3.zero, origin.position);
            Assert.Less(Vector3.Angle(Vector3.forward, origin.forward), 1e-3f, "the origin must face +z");

            var view = eng.transform.Find("Speaker View");
            Assert.IsNotNull(view);
            var sv = view.GetComponent<SpeakerView>();
            Assert.IsNotNull(sv);
            Assert.IsFalse(sv.enabled, "SpeakerView renders in a build, so it starts off");

            Assert.IsNull(eng.transform.Find("Room"), "no constraints.json, so no Room object");
            Assert.AreSame(eng.gameObject, Selection.activeGameObject);
            Assert.AreEqual(1, SceneManager.GetActiveScene().rootCount, "the rig is ONE root object");
        }

        [Test]
        public void ListenerIsTheMainCamera()
        {
            var cam = MainCamera("head");
            ExpectNoLayoutWarning(); ExpectCreated();
            var eng = CaveRig.Create();
            Assert.AreSame(cam.transform, eng.listener);
            LogAssert.NoUnexpectedReceived();
        }

        [Test]
        public void NoMainCameraLeavesTheListenerEmptyAndWarns()
        {
            new GameObject("untagged", typeof(Camera));      // a camera, but not THE camera
            ExpectNoLayoutWarning(); ExpectNoCameraWarning(); ExpectCreated();
            var eng = CaveRig.Create();
            Assert.IsNull(eng.listener);
            LogAssert.NoUnexpectedReceived();
        }

        // Two scenes open, the camera in the one the rig does NOT land in. Unity cannot save a
        // reference across scenes, so binding it would read as assigned and reload as null.
        [Test]
        public void MainCameraInAnotherSceneIsNotBound()
        {
            Directory.CreateDirectory(SceneDir);
            var a = SceneManager.GetActiveScene();           // additive needs the first scene saved
            Assert.IsTrue(EditorSceneManager.SaveScene(a, SceneDir + "/bwa_rig_test_a.unity"));
            var b = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Additive);
            var cam = MainCamera("head in b");
            SceneManager.MoveGameObjectToScene(cam, b);
            Assert.IsTrue(EditorSceneManager.SaveScene(b, SceneDir + "/bwa_rig_test_b.unity"));
            SceneManager.SetActiveScene(a);

            ExpectNoLayoutWarning();                         // LogAssert matches in ORDER of arrival
            LogAssert.Expect(LogType.Warning, new Regex("'head in b'.*is in scene 'bwa_rig_test_b'.*rig is in scene 'bwa_rig_test_a'"));
            ExpectCreated();
            var eng = CaveRig.Create();

            Assert.AreEqual(a, eng.gameObject.scene);
            Assert.IsNull(eng.listener, "the listener was bound across scenes");
            LogAssert.NoUnexpectedReceived();

            // And a camera in the rig's own scene wins over the one elsewhere.
            Undo.PerformUndo();
            var mine = MainCamera("head in a");
            SceneManager.MoveGameObjectToScene(mine, a);
            ExpectNoLayoutWarning(); ExpectCreated();
            Assert.AreSame(mine.transform, CaveRig.Create().listener);
            LogAssert.NoUnexpectedReceived();
        }

        // The three layout cases. The Engine's own default is "cave_layout.json", so the last case
        // only passes when the rig WRITES the empty string.
        [Test]
        public void LayoutPrefersTrims()
        {
            Put("trims.json"); Put("cave_layout.json");
            ExpectNoCameraWarning(); ExpectCreated();
            Assert.AreEqual("trims.json", CaveRig.Create().layoutFile);
            LogAssert.NoUnexpectedReceived();
        }

        // The field carries the file's ON-DISK name. Windows opens either spelling. A Linux or
        // Android player opens only the real one.
        [Test]
        public void LayoutKeepsTheOnDiskCase()
        {
            Put("Trims.JSON");
            ExpectNoCameraWarning(); ExpectCreated();
            Assert.AreEqual("Trims.JSON", CaveRig.Create().layoutFile);
            LogAssert.NoUnexpectedReceived();
        }

        // Spelled with capitals so the expected value is NOT the Engine's own default
        // ("cave_layout.json"): a rig that left the field alone must not pass.
        [Test]
        public void LayoutFallsBackToCaveLayout()
        {
            Put("Cave_Layout.json");
            ExpectNoCameraWarning(); ExpectCreated();
            Assert.AreEqual("Cave_Layout.json", CaveRig.Create().layoutFile);
            LogAssert.NoUnexpectedReceived();
        }

        [Test]
        public void LayoutIsEmptyWithNeitherFileAndWarns()
        {
            ExpectNoLayoutWarning(); ExpectNoCameraWarning(); ExpectCreated();
            Assert.AreEqual("", CaveRig.Create().layoutFile);
            LogAssert.NoUnexpectedReceived();
        }

        [Test]
        public void ConstraintsFileAddsTheRoom()
        {
            Put("Constraints.json");                         // not the component's default spelling
            ExpectNoLayoutWarning(); ExpectNoCameraWarning();
            var room = CaveRig.Create().transform.Find("Room");
            Assert.IsNotNull(room);
            Assert.AreEqual("Constraints.json", room.GetComponent<RoomConstraints>().constraintsFile);
            Assert.IsFalse(room.GetComponent<RoomView>().enabled, "RoomView renders in a build, so it starts off");
        }

        [Test]
        public void SecondCallCreatesNothingAndSelectsTheFirst()
        {
            ExpectNoLayoutWarning(); ExpectNoCameraWarning();
            var first = CaveRig.Create();
            Selection.activeGameObject = null;
            int roots = SceneManager.GetActiveScene().rootCount;

            LogAssert.Expect(LogType.Log, new Regex("already an Engine on 'BwAudio' in an untitled scene"));
            var second = CaveRig.Create(out bool created);

            Assert.IsFalse(created);
            Assert.AreSame(first, second);
            Assert.AreEqual(1, Engines().Length);
            Assert.AreEqual(roots, SceneManager.GetActiveScene().rootCount, "the second call left an object behind");
            Assert.AreSame(first.gameObject, Selection.activeGameObject);
        }

        // An Engine the user made by hand, under another name and switched off, is still the scene's
        // Engine.
        [Test]
        public void AnInactiveHandMadeEngineCounts()
        {
            var go = new GameObject("my audio");
            var mine = go.AddComponent<Engine>();
            go.SetActive(false);

            LogAssert.Expect(LogType.Log, new Regex("already an Engine on 'my audio'"));
            var got = CaveRig.Create(out bool created);

            Assert.IsFalse(created);
            Assert.AreSame(mine, got);
            Assert.AreEqual(1, Engines().Length);
            Assert.AreEqual(1, SceneManager.GetActiveScene().rootCount);
        }

        [Test]
        public void OneUndoRemovesTheWholeRig()
        {
            Put("constraints.json");                         // the fullest rig: every optional child
            // An undoable step of the user's, left in the OPEN undo group. Create must start its own
            // group, or the one undo below takes this too.
            var bystander = new GameObject("bystander");
            Undo.RegisterCreatedObjectUndo(bystander, "bystander");
            ExpectNoLayoutWarning(); ExpectNoCameraWarning();
            var eng = CaveRig.Create();
            Assert.AreEqual(2, SceneManager.GetActiveScene().rootCount);
            Assert.AreEqual(3, eng.transform.childCount);

            Undo.PerformUndo();

            Assert.AreEqual(0, Engines().Length, "undo left the Engine");
            Assert.IsNull(GameObject.Find("Room Origin"), "undo left the Room Origin");
            Assert.IsNull(GameObject.Find("Speaker View"), "undo left the Speaker View");
            Assert.IsNull(GameObject.Find("Room"), "undo left the Room");
            Assert.IsTrue(bystander, "undo took an object the rig did not make");
            Assert.AreEqual(1, SceneManager.GetActiveScene().rootCount);
        }

        // In Play mode AddComponent runs Engine.Awake before the rig has set a single field.
        [UnityTest]
        public IEnumerator RefusesInPlayMode()
        {
            yield return new EnterPlayMode();
            LogAssert.Expect(LogType.Log, new Regex("CAVE Rig: not in Play mode"));
            var eng = CaveRig.Create(out bool created);
            bool wasNull = eng == null;
            int engines = Engines().Length;
            foreach (var e in Engines()) Object.DestroyImmediate(e.gameObject);   // a started engine must not outlive a failure
            yield return new ExitPlayMode();
            Assert.IsFalse(created);
            Assert.IsTrue(wasNull, "Create returned an Engine in Play mode");
            Assert.AreEqual(0, engines, "Create made an Engine in Play mode");
        }

        [Test]
        public void EngineRefusesASecondCopyOnOneObject()
        {
            var go = new GameObject("two engines");
            go.AddComponent<Engine>();
            LogAssert.ignoreFailingMessages = true;          // Unity words the refusal itself
            var second = go.AddComponent<Engine>();
            LogAssert.ignoreFailingMessages = false;
            Assert.IsTrue(second == null, "[DisallowMultipleComponent] is missing from Engine");
            Assert.AreEqual(1, go.GetComponents<Engine>().Length);
        }
    }
}
