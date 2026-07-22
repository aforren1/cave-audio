// Room.cs — the coordinate seam. The core works in ROOM SPACE: right-handed, +Y up, +Z forward
// (matching OptiTrack/Motive's default streamed frame); Unity is left-handed, +Y up, +Z forward.
// Both share "up" and "forward", so the baseline conversion is a single X mirror (Unity's +X/right
// becomes the room's -X, which IS the room's right for a +Z-facing listener). Getting this wrong
// silently ruins spatial audio (front/back or left/right swaps), so it lives in one place.
// See docs/integration.md.
using UnityEngine;

namespace BwAudio
{
    public static class Room
    {
        /// <summary>Set once from CAVE registration: the transform mapping Unity world to the physical
        /// room origin/axes. Identity is the baseline (just the handedness flip below). It should be a
        /// RIGID transform (rotation + translation, no scale/shear) — Pos() tolerates any affine, but
        /// Rot() extracts a rotation (ill-defined under reflection/non-uniform scale) and ReversesWinding
        /// assumes the linear part is well-formed.</summary>
        public static Matrix4x4 UnityToRoom = Matrix4x4.identity;

        /// <summary>True if baking <paramref name="localToWorld"/>-transformed geometry into room space
        /// reverses triangle winding (so the caller swaps two indices to keep front faces). Folds the
        /// X-flip in Pos(), UnityToRoom, AND the object's own scale — a negative/mirrored scale flips
        /// winding by itself, so a fixed reversal would be wrong for it.</summary>
        public static bool ReversesWinding(Matrix4x4 localToWorld)
            => (-1f * UnityToRoom.determinant * localToWorld.determinant) < 0f;   // the -1 is the X negation

        /// <summary>Unity world position -> room space (RH metres).</summary>
        public static Vector3 Pos(Vector3 v)
        {
            v = UnityToRoom.MultiplyPoint3x4(v);
            return new Vector3(-v.x, v.y, v.z);          // baseline LH->RH; the real map lives in UnityToRoom
        }

        /// <summary>Room space -> Unity world, the inverse of Pos(). For DRAWING what the engine reports
        /// back in room coordinates (the acoustic room box, speaker positions from bwa_get_speakers): set
        /// it as Gizmos.matrix and then draw in plain room metres. Handy side effect — a wrong
        /// UnityToRoom makes the gizmo land visibly in the wrong place.</summary>
        public static Matrix4x4 RoomToUnityMatrix()
            => UnityToRoom.inverse * Matrix4x4.Scale(new Vector3(-1f, 1f, 1f));   // the X mirror is its own inverse

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
        /// from room +Z toward room +X) — the angle <c>bwa_bed_set_rotation</c> wants. The X mirror REVERSES
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
