// Room.cs — the coordinate seam. The core works in ROOM SPACE, right-handed (matching
// OptiTrack/Motive); Unity is left-handed (Z forward). Getting this wrong silently ruins
// spatial audio (front/back or left/right swaps), so it lives in one place. See docs/integration.md.
using UnityEngine;

namespace CaveAudio
{
    public static class Room
    {
        /// <summary>Set once from CAVE registration (the rigid transform that maps Unity world to the
        /// physical room origin/axes). Identity is the baseline (just the handedness flip below).</summary>
        public static Matrix4x4 UnityToRoom = Matrix4x4.identity;

        /// <summary>Unity world position -> room space (RH metres).</summary>
        public static Vector3 Pos(Vector3 v)
        {
            v = UnityToRoom.MultiplyPoint3x4(v);
            return new Vector3(v.x, v.y, -v.z);          // baseline LH->RH; the real map lives in UnityToRoom
        }

        /// <summary>Unity world rotation -> room space (RH). Used for the listener head + source orientation.</summary>
        public static Quaternion Rot(Quaternion q)
        {
            q = UnityToRoom.rotation * q;
            return new Quaternion(-q.x, -q.y, q.z, q.w); // negate x,y to match the position handedness flip
        }
    }
}
