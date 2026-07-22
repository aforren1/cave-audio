/* The coordinate seam's property test.
 *
 * The claim under test is NOT "the swizzle is these four terms" — restating the
 * implementation proves nothing. It is the property the swizzle exists to satisfy:
 *
 *   a Godot node's FORWARD direction (its basis applied to -Z, because Vector3.FORWARD
 *   is -Z) must equal the room FACING direction of the converted quaternion (the room
 *   quaternion applied to +Z, because a room identity faces +Z).
 *
 * Rotating a vector by a quaternion here is written out longhand rather than reusing any
 * engine helper, so a sign error cannot cancel itself out across both sides.
 */
#include "../src/bwa_room.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI /* MSVC omits it without _USE_MATH_DEFINES; test/doppler_fft.c does the same */
#define M_PI 3.14159265358979323846
#endif

static int failures;

#define CHECK(cond, ...)                                     \
	do {                                                     \
		if (!(cond)) {                                       \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
			printf(__VA_ARGS__);                             \
			printf("\n");                                    \
			failures++;                                      \
		}                                                    \
	} while (0)

/* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + w*v) — the standard rotation, spelled out. */
static void qrot(const float q[4], const float v[3], float out[3]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	float t[3];
	t[0] = y * v[2] - z * v[1] + w * v[0];
	t[1] = z * v[0] - x * v[2] + w * v[1];
	t[2] = x * v[1] - y * v[0] + w * v[2];
	out[0] = v[0] + 2.0f * (y * t[2] - z * t[1]);
	out[1] = v[1] + 2.0f * (z * t[0] - x * t[2]);
	out[2] = v[2] + 2.0f * (x * t[1] - y * t[0]);
}

static void check_facing(const float q[4], const char *what) {
	static const float godot_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float room_facing[3] = { 0.0f, 0.0f, 1.0f };

	float room_q[4];
	bwa_room_facing_quat(q, room_q);

	/* The converted quaternion must still be a unit quaternion — a swizzle that dropped or
	 * duplicated a term would show up here before it showed up as a direction error. */
	const float n = room_q[0] * room_q[0] + room_q[1] * room_q[1] + room_q[2] * room_q[2] +
			room_q[3] * room_q[3];
	CHECK(fabsf(n - 1.0f) < 1e-5f, "%s: room quaternion not unit (|q|^2 = %g)", what, n);

	float a[3], b[3];
	qrot(q, godot_forward, a); /* where the Godot node points */
	qrot(room_q, room_facing, b); /* where the room orientation points */

	for (int i = 0; i < 3; i++) {
		CHECK(fabsf(a[i] - b[i]) < 1e-5f,
				"%s: axis %d — godot forward (%.4f %.4f %.4f) vs room facing (%.4f %.4f %.4f)",
				what, i, a[0], a[1], a[2], b[0], b[1], b[2]);
	}
}

static void quat_from_axis_angle(float ax, float ay, float az, float ang, float q[4]) {
	const float len = sqrtf(ax * ax + ay * ay + az * az);
	const float s = sinf(ang * 0.5f) / len;
	q[0] = ax * s;
	q[1] = ay * s;
	q[2] = az * s;
	q[3] = cosf(ang * 0.5f);
}

int main(void) {
	/* Identity: a default-oriented Godot node faces -Z, so the room quaternion must be a
	 * half turn about +Y. Pinned explicitly because it is the case every scene hits. */
	{
		const float ident[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float r[4];
		bwa_room_facing_quat(ident, r);
		CHECK(fabsf(r[0]) < 1e-6f && fabsf(r[1] - 1.0f) < 1e-6f && fabsf(r[2]) < 1e-6f &&
						fabsf(r[3]) < 1e-6f,
				"identity should map to rotY(pi) = (0,1,0,0), got (%g %g %g %g)", r[0], r[1], r[2],
				r[3]);
		check_facing(ident, "identity");
	}

	/* Cardinal yaws: the case a mirrored seam gets exactly backwards. A Godot node yawed
	 * +90 degrees about +Y faces room -X; if the sense were reversed it would face +X and
	 * everything would still look self-consistent. */
	{
		float q[4];
		quat_from_axis_angle(0, 1, 0, (float)M_PI * 0.5f, q);
		float r[4], facing[3];
		const float zp[3] = { 0.0f, 0.0f, 1.0f };
		bwa_room_facing_quat(q, r);
		qrot(r, zp, facing);
		CHECK(facing[0] < -0.99f, "+90 deg yaw should face room -X, got (%.4f %.4f %.4f)", facing[0],
				facing[1], facing[2]);
		check_facing(q, "yaw +90");
	}

	/* Pitch and roll must survive too — the swizzle touches every component, so a seam that
	 * only ever gets yaw-tested can be wrong about the other two axes. */
	{
		float q[4];
		quat_from_axis_angle(1, 0, 0, (float)M_PI * 0.25f, q);
		check_facing(q, "pitch +45");
		quat_from_axis_angle(0, 0, 1, (float)M_PI * 0.25f, q);
		check_facing(q, "roll +45");
	}

	/* ...and arbitrary orientations, which is where a term-order slip hides. */
	srand(1234);
	for (int i = 0; i < 200; i++) {
		float q[4];
		float n = 0.0f;
		for (int k = 0; k < 4; k++) {
			q[k] = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
			n += q[k] * q[k];
		}
		if (n < 1e-6f) {
			continue;
		}
		n = 1.0f / sqrtf(n);
		for (int k = 0; k < 4; k++) {
			q[k] *= n;
		}
		check_facing(q, "random");
	}

	/* The inverse must undo the forward map exactly — a tracked pose read back into the
	 * scene has to land where it started, or the visual head drifts away from the audible
	 * one and only the discrepancy, never the cause, is visible. */
	srand(99);
	for (int i = 0; i < 200; i++) {
		float q[4];
		float n = 0.0f;
		for (int k = 0; k < 4; k++) {
			q[k] = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
			n += q[k] * q[k];
		}
		if (n < 1e-6f) {
			continue;
		}
		n = 1.0f / sqrtf(n);
		for (int k = 0; k < 4; k++) {
			q[k] *= n;
		}
		float room[4], back[4];
		bwa_room_facing_quat(q, room);
		bwa_godot_facing_quat(room, back);
		for (int k = 0; k < 4; k++) {
			CHECK(fabsf(back[k] - q[k]) < 1e-5f,
					"round trip lost component %d: %g in, %g out", k, q[k], back[k]);
		}
	}

	/* Yaw extraction keeps its sense: room +X at 90 degrees, and no mirror means no
	 * reversal (the Unity binding has to negate here; this one must not). */
	{
		CHECK(fabsf(bwa_room_yaw_from_dir(0.0f, 1.0f)) < 1e-6f, "+Z should be yaw 0");
		CHECK(fabsf(bwa_room_yaw_from_dir(1.0f, 0.0f) - (float)M_PI * 0.5f) < 1e-6f,
				"+X should be yaw +90 deg, got %g", bwa_room_yaw_from_dir(1.0f, 0.0f));
	}

	if (failures) {
		printf("room: %d failure(s)\n", failures);
		return 1;
	}
	printf("room: ok\n");
	return 0;
}
