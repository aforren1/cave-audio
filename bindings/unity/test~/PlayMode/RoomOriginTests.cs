// RoomOriginTests.cs — the scene-placed room registration (Engine.roomOrigin -> Room.UnityToRoom).
//
// The seam under test silently ruins spatial audio when it is wrong, and "wrong" here has a specific
// shape: a mirror applied twice, or a rotation composed on the wrong side, still gives a rigid map, so
// nothing crashes and every position is finite. The expectations are therefore spelled as room-frame
// coordinates worked out by hand, not recomputed from the code under test.
//
// The room frame: right-handed, +y up, +z ahead, room-RIGHT is -x. The origin is an empty whose
// forward is the room's +z and whose right is the room's right.
//
// The hand-worked case, an origin yawed 180 degrees (the fix for a layout whose +z is the CAVE's back
// wall). The origin faces Unity -z and its right is Unity -x, so, relative to the origin:
//   Unity +z  is BEHIND it           -> room -z
//   Unity +x  is on its LEFT         -> room +x   (room-right is -x, so room-left is +x)
//   Unity -z  is in FRONT of it      -> room +z
//   Unity +y                         -> room +y
// Both horizontal axes change sign against the identity registration (Unity +x -> room -x, Unity +z ->
// room +z), which is what a half turn is. A mirror applied twice would leave x with the identity's sign.
//
// A half turn is its own inverse, so it cannot tell R from R^-1. The second hand-worked case is a
// QUARTER turn, Euler(0, 90, 0): the origin faces Unity +x and its right is Unity -z, so
//   Unity +x  is in FRONT of it      -> room +z
//   Unity -z  is on its RIGHT        -> room -x
//   Unity +z  is on its LEFT         -> room +x
//   Unity -x  is BEHIND it           -> room -z
// With the inverse dropped, Unity +x lands on room -z instead.
using System.Collections;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace BwAudio.Tests
{
    public class RoomOriginTests
    {
        const float Tol = 1e-4f;

        GameObject _origin, _owner;
        Engine _eng;

        [SetUp]
        public void SetUp()
        {
            _owner  = new GameObject("registration owner");
            _origin = new GameObject("room origin");
            Assert.IsTrue(Room.UnityToRoom.Equals(Matrix4x4.identity),
                          "an earlier test left a registration behind; every result below would be relative to it");
        }

        [TearDown]
        public void TearDown()
        {
            Harness.DestroyEngine(_eng); _eng = null;
            Room.ReleaseOrigin(_owner);
            if (_origin) Object.DestroyImmediate(_origin);
            Object.DestroyImmediate(_owner);
            Room.UnityToRoom = Matrix4x4.identity;       // whatever a failed test left
        }

        static void AssertVec(Vector3 want, Vector3 got, string what)
            => Assert.Less((want - got).magnitude, Tol, what + ": want " + want.ToString("F4") + ", got " + got.ToString("F4"));

        [Test]
        public void NoOriginIsTheBareMirror()
        {
            AssertVec(new Vector3(-1f, 2f, 3f), Room.Pos(new Vector3(1f, 2f, 3f)), "identity Pos");
            AssertVec(Vector3.forward, Room.Rot(Quaternion.identity) * Vector3.forward, "identity heading");
        }

        [Test]
        public void HalfTurnOriginTurnsTheRoomAround()
        {
            var at = new Vector3(1f, 0f, 2f);            // off the world origin, so a dropped translation shows
            _origin.transform.SetPositionAndRotation(at, Quaternion.Euler(0f, 180f, 0f));
            Assert.IsTrue(Room.SetOrigin(_origin.transform, _owner));

            AssertVec(Vector3.zero,              Room.Pos(at),                               "the origin itself");
            AssertVec(new Vector3(0f, 0f, -1f),  Room.Pos(at + Vector3.forward),             "Unity +z of the origin (behind it)");
            AssertVec(new Vector3(1f, 0f, 0f),   Room.Pos(at + Vector3.right),               "Unity +x of the origin (its left)");
            AssertVec(new Vector3(0f, 0f, 1f),   Room.Pos(at + _origin.transform.forward),   "in front of the origin");
            AssertVec(new Vector3(-1f, 0f, 0f),  Room.Pos(at + _origin.transform.right),     "to the origin's right = room-right");
            AssertVec(new Vector3(0f, 1.5f, 0f), Room.Pos(at + Vector3.up * 1.5f),           "height");

            // Directions: the same map with no translation.
            AssertVec(new Vector3(0f, 0f, -1f), Room.Dir(Vector3.forward), "Dir(Unity +z)");
            AssertVec(new Vector3(1f, 0f, 0f),  Room.Dir(Vector3.right),   "Dir(Unity +x)");

            // A head with Unity identity rotation looks along Unity +z, which is the room's -z here,
            // and its right ear (room-right of ITS OWN frame, -x) is toward Unity +x = room +x.
            var head = Room.Rot(Quaternion.identity);
            AssertVec(new Vector3(0f, 0f, -1f), head * Vector3.forward,           "identity head's heading");
            AssertVec(new Vector3(1f, 0f, 0f),  head * new Vector3(-1f, 0f, 0f),  "identity head's right ear");

            // The inverse the gizmos draw through.
            AssertVec(at + Vector3.forward, Room.FromRoom(new Vector3(0f, 0f, -1f)), "FromRoom");
        }

        [Test]
        public void QuarterTurnOriginSeparatesTheRotationFromItsInverse()
        {
            var at = new Vector3(1f, 0f, 2f);
            _origin.transform.SetPositionAndRotation(at, Quaternion.Euler(0f, 90f, 0f));
            Room.SetOrigin(_origin.transform, _owner);

            AssertVec(Vector3.zero,             Room.Pos(at),                   "the origin itself");
            AssertVec(new Vector3(0f, 0f, 1f),  Room.Pos(at + Vector3.right),   "Unity +x of the origin (in front)");
            AssertVec(new Vector3(-1f, 0f, 0f), Room.Pos(at + Vector3.back),    "Unity -z of the origin (its right)");
            AssertVec(new Vector3(1f, 0f, 0f),  Room.Pos(at + Vector3.forward), "Unity +z of the origin (its left)");
            AssertVec(new Vector3(0f, 0f, -1f), Room.Pos(at + Vector3.left),    "Unity -x of the origin (behind)");
            AssertVec(new Vector3(0f, 0f, 1f),  Room.Dir(Vector3.right),        "Dir(Unity +x)");

            // A head with Unity identity rotation looks along Unity +z: the origin's left, room +x.
            // One facing Unity +x looks along the room's +z, which is the room's identity.
            AssertVec(new Vector3(1f, 0f, 0f), Room.Rot(Quaternion.identity) * Vector3.forward,       "identity head's heading");
            AssertVec(new Vector3(0f, 0f, 1f), Room.Rot(Quaternion.Euler(0f, 90f, 0f)) * Vector3.forward, "a head facing the room's front");

            AssertVec(at + Vector3.right, Room.FromRoom(new Vector3(0f, 0f, 1f)), "FromRoom");
        }

        // The ground plane's height comes from the origin's height and nothing else. Pos((0, y, 0)).y
        // gives the same number for a level origin, and cos(tilt) of the wrong column for a tilted one.
        [Test]
        public void GroundHeightUsesTheOriginHeightOnly()
        {
            Assert.AreEqual(2f, Room.Height(2f), Tol, "no registration");

            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0.5f, 2f), Quaternion.Euler(0f, 90f, 0f));
            Room.SetOrigin(_origin.transform, _owner);
            Assert.AreEqual(1.5f, Room.Height(2f), Tol, "a level origin 0.5 m up");

            _origin.transform.rotation = Quaternion.Euler(20f, 90f, 0f);
            Room.SetOrigin(_origin.transform, _owner);
            Assert.AreEqual(1.5f, Room.Height(2f), Tol, "the same origin pitched 20 degrees");
        }

        // The reset that keeps one Play session's registration out of the next when Domain Reload is
        // off. A test cannot re-enter Play mode, so this pins the two halves separately: the method is
        // wired to SubsystemRegistration, and calling it clears the matrix AND the owner.
        [Test]
        public void PlayEntryResetClearsTheRegistration()
        {
            var reset = typeof(Room).GetMethod("ResetOnPlay",
                System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
            Assert.IsNotNull(reset, "Room.ResetOnPlay is gone or renamed");
            var attr = (RuntimeInitializeOnLoadMethodAttribute)System.Attribute.GetCustomAttribute(
                reset, typeof(RuntimeInitializeOnLoadMethodAttribute));
            Assert.IsNotNull(attr, "the reset is not wired to Play-mode entry");
            Assert.AreEqual(RuntimeInitializeLoadType.SubsystemRegistration, attr.loadType);

            _origin.transform.rotation = Quaternion.Euler(0f, 90f, 0f);
            Room.SetOrigin(_origin.transform, _owner);
            int rev = Room.Revision;
            reset.Invoke(null, null);
            Assert.IsTrue(Room.UnityToRoom.Equals(Matrix4x4.identity), "the reset left the matrix");
            Assert.AreEqual(rev + 1, Room.Revision, "the reset changed the matrix without a Revision bump");
            Assert.IsFalse(Room.ReleaseOrigin(_owner), "the reset left the owner");
        }

        // The property the three conversions must share, for an origin and a pose with no symmetry to
        // hide behind: rotating a room-frame axis by Rot(q) is the same as converting the Unity axis
        // q carries. The room-frame "right" axis is -x, hence the sign on the second line. A second
        // mirror, or UnityToRoom.rotation composed on the wrong side, breaks these.
        [Test]
        public void RotAgreesWithDirForAnArbitraryOrigin()
        {
            _origin.transform.SetPositionAndRotation(new Vector3(-3f, 0.5f, 4f), Quaternion.Euler(10f, 73f, -20f));
            Room.SetOrigin(_origin.transform, _owner);
            var q = Quaternion.Euler(25f, -140f, 35f);

            AssertVec(Room.Dir(q * Vector3.forward), Room.Rot(q) * Vector3.forward,          "forward");
            AssertVec(Room.Dir(q * Vector3.right),   Room.Rot(q) * new Vector3(-1f, 0f, 0f), "right");
            AssertVec(Room.Dir(q * Vector3.up),      Room.Rot(q) * Vector3.up,               "up");

            var p = new Vector3(0.7f, 1.2f, -2.4f);
            AssertVec(p, Room.FromRoom(Room.Pos(p)), "FromRoom(Pos(p))");
            Assert.AreEqual(1f, Room.UnityToRoom.determinant, 1e-4f, "the registration is rigid; the mirror is not in it");
            Assert.IsTrue(Room.ReversesWinding(Matrix4x4.identity), "one mirror between Unity and the room, as with no origin");
        }

        [Test]
        public void ScaleOnTheOriginIsIgnored()
        {
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 40f, 0f));
            var rigid = Room.RegistrationFrom(_origin.transform);
            _origin.transform.localScale = new Vector3(2f, 3f, 0.5f);
            Assert.IsTrue(rigid.Equals(Room.RegistrationFrom(_origin.transform)));
        }

        [Test]
        public void OnlyTheOwnerOrADeadOwnerReleases()
        {
            _origin.transform.rotation = Quaternion.Euler(0f, 180f, 0f);
            Room.SetOrigin(_origin.transform, _owner);
            var other = new GameObject("not the owner");
            Assert.IsFalse(Room.ReleaseOrigin(other), "a second object released a live owner's registration");
            Assert.IsFalse(Room.UnityToRoom.Equals(Matrix4x4.identity));

            // The edit-mode case: the owner's scene closed and nothing ran OnDestroy for it. The draw
            // path notices.
            var dead = new GameObject("dead owner");
            Room.SetOrigin(_origin.transform, dead);
            Object.DestroyImmediate(dead);
            Room.RoomToUnityMatrix();
            Assert.IsTrue(Room.UnityToRoom.Equals(Matrix4x4.identity), "a destroyed owner's registration lingered");
            Object.DestroyImmediate(other);
        }

        [UnityTest]
        public IEnumerator EngineFollowsItsOriginAndReleasesOnDestroy()
        {
            _eng = Harness.CreateEngine();
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            yield return null;
            Assert.IsTrue(Room.UnityToRoom.Equals(Matrix4x4.identity), "no origin assigned must leave identity");

            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 180f, 0f));
            _eng.roomOrigin = _origin.transform;
            yield return null;                                       // one LateUpdate
            AssertVec(new Vector3(0f, 0f, -1f), Room.Pos(new Vector3(1f, 0f, 3f)), "after assigning the origin");

            // Revision is what SpeakerView and RoomView rebuild on, so a still origin must not move it.
            int rev = Room.Revision;
            yield return Harness.Frames(3);
            Assert.AreEqual(rev, Room.Revision, "an origin that did not move bumped Revision");

            _origin.transform.position = new Vector3(5f, 0f, 2f);    // a re-registration, followed per frame
            yield return null;
            AssertVec(Vector3.zero, Room.Pos(new Vector3(5f, 0f, 2f)), "after moving the origin");
            Assert.AreEqual(rev + 1, Room.Revision, "a moved origin must bump Revision exactly once");

            // A disabled Engine keeps the registration: it still owns the native engine and its sources.
            _eng.enabled = false;
            yield return null;
            AssertVec(Vector3.zero, Room.Pos(new Vector3(5f, 0f, 2f)), "while the Engine is disabled");
            _eng.enabled = true;

            // The origin dies with its scene while the Engine (DontDestroyOnLoad) lives on. The room did
            // not move, so the registration stays, and the Engine says so once.
            LogAssert.Expect(LogType.Warning, new Regex("Room Origin is gone"));
            Object.DestroyImmediate(_origin);
            yield return null;
            yield return null;
            AssertVec(Vector3.zero, Room.Pos(new Vector3(5f, 0f, 2f)), "after the origin was destroyed");

            Harness.DestroyEngine(_eng); _eng = null;
            Assert.IsTrue(Room.UnityToRoom.Equals(Matrix4x4.identity), "a destroyed Engine left its registration in the static");
        }

        // The static geometry bake and the FDN direction read the seam inside Awake, so an origin
        // assigned in the Inspector has to be applied there and not at the first LateUpdate. No frame
        // is turned between the activate and the assert.
        [Test]
        public void OriginAssignedBeforeAwakeIsAppliedInAwake()
        {
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 90f, 0f));
            var go = new GameObject("bwa_test_engine");
            go.SetActive(false);
            _eng = go.AddComponent<Engine>();
            _eng.sink = BwaSinkType.Null;
            _eng.profile = BwaProfile.Cave;
            _eng.layoutFile = "";
            _eng.sampleRate = Harness.SampleRate;
            _eng.blockSize = Harness.BlockSize;
            _eng.listener = go.transform;
            _eng.roomOrigin = _origin.transform;
            go.SetActive(true);                                      // -> Awake
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            AssertVec(new Vector3(0f, 0f, 1f), Room.Pos(new Vector3(2f, 0f, 2f)), "straight after Awake");
        }

        // Scene A's Engine is DontDestroyOnLoad. Scene B brings its own Engine with its own origin,
        // and that Engine destroys itself in Awake. B's origin is where the room is NOW, so the
        // survivor must take it, or B's audio is registered against A's room.
        [UnityTest]
        public IEnumerator DuplicateEngineHandsItsOriginToTheSurvivor()
        {
            var first = new GameObject("scene A origin");
            _eng = Harness.CreateEngine();
            Assert.IsTrue(_eng.Ready, "engine did not start: " + Bwa.LastError(_eng.Handle));
            _eng.roomOrigin = first.transform;                       // identity pose
            yield return null;

            // B's origin sits UNDER B's Engine, the worst case: it would die with the duplicate.
            var dupGo = new GameObject("scene B engine");
            dupGo.SetActive(false);
            var dup = dupGo.AddComponent<Engine>();
            _origin.transform.SetParent(dupGo.transform, worldPositionStays: false);
            _origin.transform.SetPositionAndRotation(new Vector3(1f, 0f, 2f), Quaternion.Euler(0f, 90f, 0f));
            dup.roomOrigin = _origin.transform;
            dupGo.SetActive(true);                                   // -> Awake -> hand over, Destroy(gameObject)

            Assert.AreSame(_origin.transform, _eng.roomOrigin, "the survivor did not take the new scene's origin");
            AssertVec(new Vector3(0f, 0f, 1f), Room.Pos(new Vector3(2f, 0f, 2f)), "straight after the hand-over");

            yield return null;
            yield return null;
            Assert.IsTrue(dup == null, "the duplicate Engine is still alive");
            Assert.IsTrue(_origin, "the origin died with the duplicate Engine");
            AssertVec(new Vector3(0f, 0f, 1f), Room.Pos(new Vector3(2f, 0f, 2f)), "after the duplicate is gone");
            Object.DestroyImmediate(first);
        }
    }
}
