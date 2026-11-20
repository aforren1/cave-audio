// LifecycleTests.cs — creating things at run time and the contracts around them: components added
// with AddComponent and configured afterwards, a bed that comes up before the Engine, static geometry
// that appears or goes away after start, absolute clip paths, the listener-placement warning, and
// the bed's yaw against a turned Room Origin.
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class LifecycleTests
    {
        Engine _eng;
        readonly List<GameObject> _made = new List<GameObject>();
        int _savedFrameRate;

        [OneTimeSetUp]
        public void OneTimeSetUp() => Harness.WriteFixtures();

        [SetUp]
        public void SetUp()
        {
            _savedFrameRate = Application.targetFrameRate;
            Application.targetFrameRate = Harness.TargetFrameRate;
            Assert.IsTrue(Engine.Instance == null, "an earlier test left an Engine behind");
        }

        [TearDown]
        public void TearDown()
        {
            foreach (var go in _made) if (go) Object.DestroyImmediate(go);
            _made.Clear();
            Harness.DestroyEngine(_eng); _eng = null;
            Room.UnityToRoom = Matrix4x4.identity;
            Application.targetFrameRate = _savedFrameRate;
        }

        GameObject Make(string name) { var go = new GameObject(name); _made.Add(go); return go; }

        void StartEngine(System.Action<Engine> configure = null)
        {
            _eng = Harness.CreateEngine(configure);
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
        }

        // ---- runtime creation (AddComponent on an ACTIVE object) ---------------------------------

        // The fields set right after AddComponent are the ones that play. The old OnEnable auto-play
        // fired inside AddComponent, on the default clip, before any of them were set.
        [UnityTest]
        public IEnumerator AddComponentThenSetFieldsPlaysTheFields()
        {
            StartEngine();
            int finished = 0;
            using (var log = new Harness.LogCapture())
            {
                var em = Make("spawned").AddComponent<Emitter>();
                em.clip = Harness.Short;
                em.loop = false;
                em.gain = 0.25f;                                  // a desc field: read only at create
                em.onFinished.AddListener(() => finished++);
                bool hit = false;
                yield return Harness.Until(() => finished > 0, 3.0, v => hit = v);
                Assert.IsTrue(hit, "the clip set after AddComponent never played to its end");
                // The editor calls OnValidate inside AddComponent. A create from there would carry
                // the initializer's gain in the editor and the field's gain in a build.
                Assert.IsTrue(em.TryGetDesc(out var d), "no live source to read back");
                Assert.AreEqual(0.25f, d.gain, 1e-6f, "the gain set after AddComponent never reached the voice");
                Assert.AreEqual("", log.Problems());
            }
        }

        // With nothing set, an added Emitter has nothing to play, and says nothing about it.
        [UnityTest]
        public IEnumerator AddComponentWithDefaultsIsSilent()
        {
            StartEngine();
            using (var log = new Harness.LogCapture())
            {
                var em = Make("spawned").AddComponent<Emitter>();
                var bed = Make("spawned bed").AddComponent<AmbisonicBed>();
                yield return Harness.Frames(5);
                Assert.IsFalse(em.IsPlaying);
                Assert.AreEqual(0ul, bed.PlayheadFrames);
                Assert.AreEqual("", log.Problems(), "a default component tried to load a clip");
            }
        }

        // A call that needs the voice before Start creates it on the spot, so the old pattern of
        // AddComponent then Play still works, and the Play replaces the auto-play rather than being
        // restarted by it. The auto-play would start the 2 s field clip over the 256 ms one played
        // here, and the end would not come inside a second.
        [UnityTest]
        public IEnumerator PlayBeforeStartCreatesTheVoice()
        {
            StartEngine();
            int finished = 0;
            var em = Make("spawned").AddComponent<Emitter>();
            em.clip = Harness.Long;                               // what the auto-play would play
            em.loop = false;
            em.onFinished.AddListener(() => finished++);
            em.Play(Harness.Short);                               // same frame: Start has not run
            bool hit = false;
            yield return Harness.Until(() => finished > 0, 1.0, v => hit = v);
            Assert.IsTrue(hit, "a Play issued before Start never played, or Start's auto-play restarted it");

            // A one-shot needs no voice of the emitter's, so it must not wait for Start either.
            var other = Make("spawned 2").AddComponent<Emitter>();
            other.playOnEnable = false;
            Assert.IsTrue(other.PlayOneShot(Harness.Short), "a one-shot before Start was refused: " + Bwa.LastError(_eng.Handle));
        }

        // A playback-state call before Start acts on the auto-play instead of being undone by it.
        [UnityTest]
        public IEnumerator StopAndPauseBeforeStartWin()
        {
            StartEngine();
            var stopped = Make("stopped").AddComponent<Emitter>();
            stopped.clip = Harness.Long;
            stopped.Stop();                                       // before Start
            var paused = Make("paused").AddComponent<Emitter>();
            paused.clip = Harness.Long;
            paused.Paused = true;                                 // before Start

            yield return Harness.Wall(0.5);
            Assert.IsFalse(stopped.IsPlaying, "Start's auto-play undid a Stop issued before it");
            Assert.IsTrue(paused.Paused, "Paused set before Start reads false");
            Assert.IsTrue(paused.IsPlaying, "the paused emitter never auto-played");
            ulong held = paused.PlayheadFrames;
            yield return Harness.Wall(0.3);
            Assert.AreEqual(held, paused.PlayheadFrames, "Paused reads true but the voice is running");

            paused.UnPause();
            yield return Harness.Wall(0.3);
            Assert.Greater(paused.PlayheadFrames, held + 4800, "UnPause did not resume the voice");
        }

        // ---- the shared wait ------------------------------------------------------------------------

        // A bed that comes up before the Engine waits for it, the way an Emitter always has. It used
        // to switch itself off for good.
        [UnityTest]
        public IEnumerator BedEnabledBeforeTheEngineWaitsForIt()
        {
            var bed = Make("early bed").AddComponent<AmbisonicBed>();
            bed.clip = Harness.BedLong;
            bed.loop = true;
            yield return Harness.Frames(3);
            Assert.IsTrue(bed.enabled, "the bed switched itself off while the Engine was missing");

            StartEngine();
            bool hit = false;
            yield return Harness.Until(() => bed.PlayheadFrames > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "the bed never started once the Engine came up");
        }

        // ---- static geometry after start ------------------------------------------------------------

        // Measured where it matters: the occlusion a source behind the wall actually gets. Needs the
        // Steam Audio build, which the staged library is. The wall is a 4 x 4 x 0.2 m slab between a
        // head and a source 3 m in front of it.
        [UnityTest]
        public IEnumerator StaticGeometryFollowsEnableAndDisable()
        {
            var head = Make("head");
            head.transform.position = new Vector3(0f, 1.5f, 0f);
            StartEngine(e => e.listener = head.transform);
            var em = Harness.CreateEmitter(Harness.Long, loop: true);
            _made.Add(em.gameObject);
            em.transform.position = new Vector3(0f, 1.5f, 3f);
            em.occlusion = true;
            em.Play();
            em.ApplyDesc();                                       // occlusion is a desc field
            yield return Harness.Frames(5);
            Assert.Greater(em.Occlusion, 0.9f, "the source reads occluded with no geometry");

            int rebuilds = _eng.StaticRebuilds;
            var wall = GameObject.CreatePrimitive(PrimitiveType.Cube); _made.Add(wall);
            wall.transform.position = new Vector3(0f, 1.5f, 1.5f);
            wall.transform.localScale = new Vector3(4f, 4f, 0.2f);
            var g = wall.AddComponent<AcousticGeometry>();
            var spare = GameObject.CreatePrimitive(PrimitiveType.Cube); _made.Add(spare);   // same frame
            spare.transform.position = new Vector3(5f, 0.5f, -5f);
            spare.AddComponent<AcousticGeometry>();
            yield return Harness.Frames(2);
            Assert.AreEqual(rebuilds + 1, _eng.StaticRebuilds, "two geometry adds in one frame: want ONE rebuild");

            bool hit = false;
            yield return Harness.Until(() => em.Occlusion < 0.5f, 3.0, v => hit = v);
            Harness.Note($"wall up: occlusion {em.Occlusion:F3}");
            Assert.IsTrue(hit, "a wall added after start does not occlude: " + em.Occlusion);

            wall.SetActive(false);
            yield return Harness.Until(() => em.Occlusion > 0.9f, 3.0, v => hit = v);
            Harness.Note($"wall deactivated: occlusion {em.Occlusion:F3}");
            Assert.IsTrue(hit, "a deactivated wall still occludes: " + em.Occlusion);

            wall.SetActive(true);
            yield return Harness.Until(() => em.Occlusion < 0.5f, 3.0, v => hit = v);
            Assert.IsTrue(hit, "a re-activated wall does not occlude: " + em.Occlusion);

            // The last one out: the static mesh must actually clear, not just log that it did.
            Object.DestroyImmediate(spare);
            g.enabled = false;
            yield return Harness.Until(() => em.Occlusion > 0.9f, 3.0, v => hit = v);
            Harness.Note($"no geometry left: occlusion {em.Occlusion:F3}");
            Assert.IsTrue(hit, "with no geometry left the old mesh still occludes: " + em.Occlusion);

            rebuilds = _eng.StaticRebuilds;
            yield return Harness.Frames(5);
            Assert.AreEqual(rebuilds, _eng.StaticRebuilds, "a rebuild with nothing changed");
        }

        // ---- absolute clip paths --------------------------------------------------------------------

        // A clip generated at run time is written somewhere writable and played by its full path.
        [UnityTest]
        public IEnumerator AbsoluteClipPathPlays()
        {
            string path = Path.Combine(Application.temporaryCachePath, "bwa_test_generated.wav");
            Harness.WriteTone(path, Harness.ShortFrames, 1);
            Assert.IsTrue(Path.IsPathRooted(path));
            StartEngine();
            int finished = 0;
            var em = Harness.CreateEmitter(path, loop: false);
            _made.Add(em.gameObject);
            em.onFinished.AddListener(() => finished++);
            em.Play();
            bool hit = false;
            yield return Harness.Until(() => finished > 0, 3.0, v => hit = v);
            Assert.IsTrue(hit, "a clip at an absolute path did not play");
        }

        // ---- listener placement -------------------------------------------------------------------

        [UnityTest]
        public IEnumerator ListenerFarFromTheArrayWarnsOnce()
        {
            var head = Make("head");
            head.transform.position = new Vector3(0f, 1.5f, 12f);   // the default grid spans +/-1.5 m
            StartEngine(e => e.listener = head.transform);
            using (var log = new Harness.LogCapture())
            {
                // A head that has not moved yet may be parked waiting for tracking: no warning.
                yield return Harness.Frames(10);
                Assert.AreEqual(0, log.Count("outside"), "warned before the listener ever moved");

                head.transform.position += new Vector3(0f, 0f, 0.5f);
                yield return Harness.Frames(10);
                Assert.AreEqual(1, log.Count("outside the speaker array's bounding box"),
                                "a listener 11 m outside the array: want one warning\n" + string.Join("\n", log.Lines));
            }
        }

        // At a desk the head is wherever the camera is, and binaural drives no speakers. Only an
        // enabled room box gives it a place it should be.
        [UnityTest]
        public IEnumerator BinauralListenerAnywhereIsQuietUntilARoomBox()
        {
            var head = Make("head");
            head.transform.position = new Vector3(0f, 1.5f, 12f);
            StartEngine(e => { e.profile = BwaProfile.Binaural; e.listener = head.transform; });
            using (var log = new Harness.LogCapture())
            {
                yield return null;
                head.transform.position += new Vector3(0f, 0f, 0.5f);   // armed: the head has moved
                yield return Harness.Frames(10);
                Assert.AreEqual(0, log.Count("outside"), "binaural warned about a desk listener:\n" + log.Problems());

                _eng.enableRoomBox = true;                         // 3 x 3 x 3 m by default
                yield return Harness.Frames(5);
                Assert.AreEqual(1, log.Count("outside the room box"), "a listener 10.5 m outside the room box did not warn");
            }
        }

        // ---- bed yaw ------------------------------------------------------------------------------

        // Yaw is in scene terms: 0 is Unity world +Z, whatever the origin says, and the field turns
        // with the scene when the origin moves. Measured on the bus, not on the angle the binding
        // computed: a plane wave from the field's front lights the speakers it points at. The default
        // grid has 9 speakers at room z = +1.5 and 9 at z = -1.5.
        [UnityTest]
        public IEnumerator BedYawIsInSceneTermsAndFollowsTheOrigin()
        {
            var origin = Make("room origin");
            origin.transform.rotation = Quaternion.Euler(0f, 180f, 0f);   // layout +z is Unity -z
            StartEngine(e => e.roomOrigin = origin.transform);
            var bed = Make("bed").AddComponent<AmbisonicBed>();
            bed.clip = Harness.BedFront;
            bed.loop = true;

            yield return Harness.Wall(1.0);                        // the orientation glides at ~1 turn/s
            float front = 0f, back = 0f;
            yield return FrontBack((f, b) => { front = f; back = b; });
            Harness.Note($"origin turned 180: front {front:F3}, back {back:F3}");
            Assert.Greater(back, 1.5f * front, "yaw 0 did not point the field along world +Z (room -z)");

            origin.transform.rotation = Quaternion.identity;      // the scene turns inside the room
            yield return Harness.Wall(1.5);
            yield return FrontBack((f, b) => { front = f; back = b; });
            Harness.Note($"origin identity: front {front:F3}, back {back:F3}");
            Assert.Greater(front, 1.5f * back, "the bed did not turn with a moved Room Origin");

            // 0 and 180 degrees survive a sign flip. A quarter turn does not: the origin faces Unity
            // +x, so Unity +z is on its LEFT, which is room +x (room-left; room-right is -x).
            origin.transform.rotation = Quaternion.Euler(0f, 90f, 0f);
            yield return Harness.Wall(1.5);
            float left = 0f, right = 0f;
            yield return Sides(0, (l, r) => { left = l; right = r; });
            Harness.Note($"origin 90: left {left:F3}, right {right:F3}");
            Assert.Greater(left, 1.5f * right, "yaw 0 under a quarter-turned origin did not face room-left (+x)");
        }

        IEnumerator FrontBack(System.Action<float, float> result) => Sides(2, result);

        // Summed bus peaks over 20 frames for the speakers above +0.5 and below -0.5 on room axis
        // `axis` (0 = x, where +x is room-LEFT; 2 = z, where +z is room-front).
        IEnumerator Sides(int axis, System.Action<float, float> result)
        {
            var xyz = _eng.SpeakerPositions();
            float f = 0f, b = 0f;
            for (int k = 0; k < 20; k++)
            {
                yield return null;
                var lv = _eng.BusLevels();
                for (int i = 0; i < lv.Length; i++)
                {
                    float c = xyz[i * 3 + axis];
                    if (c > 0.5f) f += lv[i]; else if (c < -0.5f) b += lv[i];
                }
            }
            Assert.Greater(f + b, 0f, "the bed is silent, so this test measures nothing");
            result(f, b);
        }
    }
}
