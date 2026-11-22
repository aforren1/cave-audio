// Room.cs — the coordinate seam. The core works in ROOM SPACE: right-handed, +Y up, +Z forward
// (matching OptiTrack/Motive's default streamed frame); Unity is left-handed, +Y up, +Z forward.
// Both share "up" and "forward", so the baseline conversion is a single X mirror (Unity's +X/right
// becomes the room's -X, which IS the room's right for a +Z-facing listener). Getting this wrong
// silently ruins spatial audio (front/back or left/right swaps), so it lives in one place.
// See https://github.com/aforren1/cave-audio/blob/5885fc659412/docs/integration.md.
using UnityEngine;

namespace BwAudio
{
    public static class Room
    {
        /// <summary>The CAVE registration: the transform mapping Unity world to the physical room
        /// origin/axes. Assign Engine.roomOrigin and the Engine keeps this set from that Transform; write
        /// it yourself only when no origin is assigned. Identity is the baseline (just the handedness
        /// flip below). It should be a
        /// RIGID transform (rotation + translation, no scale/shear) — Pos() tolerates any affine, but
        /// Rot() extracts a rotation (ill-defined under reflection/non-uniform scale) and ReversesWinding
        /// assumes the linear part is well-formed.</summary>
        public static Matrix4x4 UnityToRoom = Matrix4x4.identity;

        // ---- the scene-placed registration (Engine.roomOrigin) ---------------------------------------
        //
        // UnityToRoom is a STATIC, so it outlives the object that set it: across scene loads, and across
        // play sessions when Domain Reload is off. A registration set from a Transform therefore records
        // its owner, and three things clear a stale one: the owner releasing it (Engine.OnDestroy), the
        // owner being found destroyed when a gizmo asks for the matrix (edit mode runs no OnDestroy), and
        // the reset on entering Play mode below. A value written straight into UnityToRoom by user code has
        // no owner and is cleared only by the Play-mode reset.
        static Object s_owner;
        static bool s_hasOwner;

        /// <summary>Counts changes made through SetOrigin / ReleaseOrigin. The views that BAKE the
        /// registration into world positions (SpeakerView, RoomView) compare it to know when to rebuild.
        /// A direct write to UnityToRoom does not count.</summary>
        public static int Revision { get; private set; }

        /// <summary>The registration for a room origin placed in the scene: an empty at the room-frame
        /// origin (the CAVE floor center) with its forward (+Z) along the layout's +Z. Unity world -> the
        /// origin's LOCAL frame, which is left-handed with x = the origin's right; Pos() then mirrors X,
        /// so the origin's right becomes room -x (room-right) and its forward stays room +z. The mirror
        /// is NOT in this matrix: Pos / Dir / Rot / RoomToUnityMatrix each apply it once.
        /// Built from position and rotation only, never worldToLocalMatrix: a scale on the origin or on
        /// a parent would rescale room meters and break Rot(), so it is ignored.</summary>
        public static Matrix4x4 RegistrationFrom(Transform origin)
            => Matrix4x4.Rotate(Quaternion.Inverse(origin.rotation)) * Matrix4x4.Translate(-origin.position);

        /// <summary>Register the room from <paramref name="origin"/> on behalf of
        /// <paramref name="owner"/>. Cheap enough to call every frame. True when the matrix changed.</summary>
        public static bool SetOrigin(Transform origin, Object owner)
        {
            if (!origin) return false;
            var m = RegistrationFrom(origin);
            s_owner = owner; s_hasOwner = true;
            if (m.Equals(UnityToRoom)) return false;       // exact compare: == on Matrix4x4 is approximate
            UnityToRoom = m; Revision++;
            return true;
        }

        /// <summary>Back to identity, if <paramref name="owner"/> holds the registration or its holder
        /// no longer exists. Another live owner's registration is left alone. True when the matrix
        /// changed.</summary>
        public static bool ReleaseOrigin(Object owner)
        {
            if (!s_hasOwner) return false;
            if (s_owner && !ReferenceEquals(s_owner, owner)) return false;
            s_owner = null; s_hasOwner = false;
            if (UnityToRoom.Equals(Matrix4x4.identity)) return false;
            UnityToRoom = Matrix4x4.identity; Revision++;
            return true;
        }

        // With Domain Reload off, statics survive from one Play session into the next, so the last
        // session's registration would be this session's default.
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void ResetOnPlay()
        {
            s_owner = null; s_hasOwner = false;
            if (!UnityToRoom.Equals(Matrix4x4.identity)) { UnityToRoom = Matrix4x4.identity; Revision++; }
        }

        /// <summary>True if baking <paramref name="localToWorld"/>-transformed geometry into room space
        /// reverses triangle winding (so the caller swaps two indices to keep front faces). Folds the
        /// X-flip in Pos(), UnityToRoom, AND the object's own scale — a negative/mirrored scale flips
        /// winding by itself, so a fixed reversal would be wrong for it.</summary>
        public static bool ReversesWinding(Matrix4x4 localToWorld)
            => (-1f * UnityToRoom.determinant * localToWorld.determinant) < 0f;   // the -1 is the X negation

        /// <summary>Unity world position -> room space (RH meters).</summary>
        public static Vector3 Pos(Vector3 v)
        {
            v = UnityToRoom.MultiplyPoint3x4(v);
            return new Vector3(-v.x, v.y, v.z);          // baseline LH->RH; the real map lives in UnityToRoom
        }

        /// <summary>Unity world height -> room height, for a HORIZONTAL plane (the ground bounce). It uses
        /// the registration origin's height and nothing else. Pos((0, y, 0)).y is wrong here twice
        /// over: it reads the height above the world origin's column instead of the plane's, and under
        /// a pitched or rolled registration a level Unity plane is not level in the room, so it has
        /// no single room height at all. A CAVE floor origin is level; Engine warns when one is not.</summary>
        public static float Height(float worldY)
            => worldY - UnityToRoom.inverse.MultiplyPoint3x4(Vector3.zero).y;   // inverse * 0 = the origin's world position

        /// <summary>Room space -> Unity world, the inverse of Pos(). For DRAWING what the engine reports
        /// back in room coordinates (the acoustic room box, speaker positions from bwa_get_speakers): set
        /// it as Gizmos.matrix and then draw in plain room meters. Handy side effect — a wrong
        /// UnityToRoom makes the gizmo land visibly in the wrong place.</summary>
        public static Matrix4x4 RoomToUnityMatrix()
        {
            // The draw path is where a stale registration shows, and in the stopped editor nothing else
            // runs to clear it: the owner's scene closed, and no Engine is left to release it.
            if (s_hasOwner && !s_owner) ReleaseOrigin(null);
            return UnityToRoom.inverse * Matrix4x4.Scale(new Vector3(-1f, 1f, 1f));   // the X mirror is its own inverse
        }

        /// <summary>Room-space position -> Unity world (the inverse of Pos()).</summary>
        public static Vector3 FromRoom(Vector3 roomPos) => RoomToUnityMatrix().MultiplyPoint3x4(roomPos);

        /// <summary>Unity world DIRECTION -> room space (RH). Pos() without the translation — for axes and
        /// normals (the FDN's decay direction), which a registration OFFSET must not move. Not normalized.</summary>
        public static Vector3 Dir(Vector3 v)
        {
            v = UnityToRoom.MultiplyVector(v);
            return new Vector3(-v.x, v.y, v.z);
        }

        /// <summary>Unity world rotation -> room space (RH). Used for the listener head + source orientation.
        /// Unity identity faces +Z and so does the room's, so identity maps to identity.</summary>
        public static Quaternion Rot(Quaternion q)
        {
            q = UnityToRoom.rotation * q;
            return new Quaternion(q.x, -q.y, -q.z, q.w); // negate y,z to match the position handedness flip
        }

        /// <summary>Unity yaw (degrees about +Y) -> room yaw (radians about +Y, RH: positive turns the field
        /// from room +Z toward room +X) — the angle <c>bwa_bed_set_orientation</c> wants. The X mirror REVERSES
        /// the sense of rotation (turning a bed to Unity's right turns it toward room -X, a NEGATIVE room
        /// yaw), so a Unity euler angle passed straight to the engine spins the soundfield the wrong way.
        /// Routed through Rot() so any yaw baked into UnityToRoom is included, not just the mirror.</summary>
        public static float YawRad(float unityYawDegrees)
        {
            Vector3 ahead = Rot(Quaternion.Euler(0f, unityYawDegrees, 0f)) * Vector3.forward;  // room-space heading
            return Mathf.Atan2(ahead.x, ahead.z);                                              // RH yaw about +Y
        }
    }
}
