/* The coordinate seam, in plain floats so it can be tested without linking godot-cpp.
 *
 * Room space is right-handed, +Y up, +Z forward, meters, origin on the floor — Motive's
 * default streamed frame. Godot is right-handed and +Y up too, so unlike Unity there is
 * NO handedness mirror: positions and directions need only the CAVE registration
 * transform, which is plain Basis/Transform3D arithmetic and lives in the node code.
 *
 * What does NOT pass through is the facing convention. Godot's Vector3.FORWARD is -Z; a
 * room-frame identity quaternion faces +Z. So an orientation picks up a 180 degree yaw,
 * and that single multiply is the one piece of this seam worth pinning down: get it wrong
 * and every source and the listener face backwards, which sounds like a plausible mix
 * rather than an obvious bug.
 */
#pragma once

#include <math.h>

/* Godot orientation quaternion -> room orientation quaternion. Both are (x, y, z, w).
 *
 * Derivation: q_room = q_godot * rotY(pi), with rotY(pi) = (0, 1, 0, 0). Expanding the
 * Hamilton product against that constant collapses to a swizzle:
 *
 *   (x, y, z, w)  ->  (-z, w, x, -y)
 *
 * Identity in gives (0, 1, 0, 0) out: a default-oriented Godot node faces room -Z, which
 * is exactly what Vector3.FORWARD == -Z means. */
static inline void bwa_room_facing_quat(const float q[4], float out[4]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	out[0] = -z;
	out[1] = w;
	out[2] = x;
	out[3] = -y;
}

/* The inverse: room orientation -> Godot orientation, for reading a tracked pose back into
 * the scene. q_godot = q_room * rotY(-pi), which collapses the same way:
 *
 *   (x, y, z, w)  ->  (z, -w, -x, y)
 */
static inline void bwa_godot_facing_quat(const float q[4], float out[4]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	out[0] = z;
	out[1] = -w;
	out[2] = -x;
	out[3] = y;
}

/* Room-frame yaw about +Y for a facing direction, the angle bwa_bed_set_orientation wants.
 * Because there is no mirror, the sense of rotation is preserved straight through — the
 * Unity binding has to reverse it, this one must not. */
static inline float bwa_room_yaw_from_dir(float x, float z) {
	return atan2f(x, z);
}
