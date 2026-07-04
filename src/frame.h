/*
 * frame.h — the room frame's identity basis, consumed from one place. The convention itself
 * (right-handed, +y up, +z forward = Motive's default; right ear at -x) is public ABI contract,
 * so the base vectors live in bwaudio.h (BW_ROOM_AHEAD/UP/RIGHT); this header adds the helper
 * the engine's orientation seams share. Flipping the convention = editing the bwaudio.h
 * constants, plus the mirrors a C constant cannot reach: the Unity binding's handedness flip
 * (bindings/unity Room.cs), the laterality tests' left/right speaker picks, and the docs.
 */
#ifndef BW_FRAME_H
#define BW_FRAME_H

#include "bwaudio.h"

/* rotate vector v by UNIT quaternion q (xyzw) — callers normalize degenerate input first */
static inline void frame_qrot(const float q[4], const float v[3], float o[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2.f * (y * v[2] - z * v[1]);
    float ty = 2.f * (z * v[0] - x * v[2]);
    float tz = 2.f * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

#endif /* BW_FRAME_H */
