/**
 * xr_frame.test.mjs - the WebXR coordinate seam, checked under node.
 *
 * The seam is `bindings/web/xr/frame_xr.js`: the map from an XR reference space (right handed,
 * +y up, the viewer's ahead down -z, right at +x) to the engine's room frame (right handed,
 * +y up, ahead +z, LEFT at +x). It gets a test of its own because a wrong orientation map is
 * INAUDIBLE until you turn your head, and the XR page cannot be driven without a headset.
 *
 * Nothing here imports three.js or the engine. The seam is pure arithmetic on purpose.
 *
 * THE CHECKS ARE AGAINST THE ABI'S BASIS VECTORS, not against the algebra in the module. The
 * module derives `q_room = qY * q_xr * qY` and multiplies it out to `(x,-y,z,-w)`; this file
 * never mentions that. It asks the physical question instead: take the direction the head is
 * looking in the XR space, carry it into the room, and demand that the room quaternion turns
 * BWA_ROOM_AHEAD onto the same direction. A mis-derivation fails that whatever it wrote.
 */
import test from "node:test";
import assert from "node:assert/strict";
import {
  ROOM_AHEAD, ROOM_UP, ROOM_RIGHT, XR_AHEAD, XR_UP, XR_RIGHT, XR_TO_ROOM_QUAT,
  xrPointToRoom, roomPointToXr, xrQuatToRoom, roomQuatToXr, xrTransformToRoom,
  qrot, qmul, earSideOf, PoseLead,
} from "../xr/frame_xr.js";

const EPS = 1e-6;

/** The space map, spelled as a matrix rather than as the module's sign flips. */
function rotY180(v) {
  /* cos(pi) = -1, sin(pi) = 0: (x,y,z) -> (-x, y, -z). Written as the general rotation so the
   * reader can see it IS a rotation, determinant +1, and not a reflection. */
  const c = Math.cos(Math.PI);
  const s = Math.sin(Math.PI);
  return [v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c];
}

function quatAxisAngle(axis, rad) {
  const n = Math.hypot(...axis);
  const s = Math.sin(rad / 2) / n;
  return [axis[0] * s, axis[1] * s, axis[2] * s, Math.cos(rad / 2)];
}

function closeVec(a, b, eps = EPS) {
  return Math.abs(a[0] - b[0]) < eps && Math.abs(a[1] - b[1]) < eps && Math.abs(a[2] - b[2]) < eps;
}

/** q and -q are the same rotation, so a quaternion comparison has to allow the sign. */
function sameRotation(a, b, eps = EPS) {
  const same = a.every((v, i) => Math.abs(v - b[i]) < eps);
  const flip = a.every((v, i) => Math.abs(v + b[i]) < eps);
  return same || flip;
}

/** A deterministic spread of unit quaternions. No Math.random: a flaky seam test is useless. */
function sampleQuats() {
  const out = [[0, 0, 0, 1]];
  let s = 1234567;
  const rnd = () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return ((s >>> 8) & 0xffffff) / 16777216;
  };
  for (let i = 0; i < 64; ++i) {
    /* Shoemake's uniform quaternion sampling. Uniform coverage matters here: a seam bug can hide
     * in one octant, which is exactly what a handful of hand-picked yaws would miss. */
    const u1 = rnd(), u2 = rnd(), u3 = rnd();
    const r1 = Math.sqrt(1 - u1), r2 = Math.sqrt(u1);
    out.push([
      r1 * Math.sin(2 * Math.PI * u2), r1 * Math.cos(2 * Math.PI * u2),
      r2 * Math.sin(2 * Math.PI * u3), r2 * Math.cos(2 * Math.PI * u3),
    ]);
  }
  return out;
}

test("the space map is a rotation, not a mirror", () => {
  /* A reflection would have determinant -1 and would flip a cross product. This is the claim the
   * module's header makes and the one a careless reading of "+x LEFT versus +x right" gets
   * wrong, so it is checked rather than asserted in prose. */
  const ex = xrPointToRoom(1, 0, 0);
  const ey = xrPointToRoom(0, 1, 0);
  const ez = xrPointToRoom(0, 0, 1);
  const det = ex[0] * (ey[1] * ez[2] - ey[2] * ez[1])
            - ex[1] * (ey[0] * ez[2] - ey[2] * ez[0])
            + ex[2] * (ey[0] * ez[1] - ey[1] * ez[0]);
  assert.ok(Math.abs(det - 1) < EPS, `the XR-to-room map has determinant ${det}, so it mirrors`);

  for (const v of [[1, 2, 3], [-4, 0.5, 7], [0, 0, 0]]) {
    assert.ok(closeVec(xrPointToRoom(...v), rotY180(v)), "the map is not the 180 degree yaw");
    assert.ok(closeVec(xrPointToRoom(...v), qrot(XR_TO_ROOM_QUAT, v)),
              "XR_TO_ROOM_QUAT does not agree with xrPointToRoom");
  }
  /* The three.js side applies XR_TO_ROOM_QUAT to the group that carries the XR camera. If that
   * ever stopped matching the point map, the picture and the audio would disagree about where
   * the user is, which is the one failure a headset makes obvious and a test never would. */
  assert.ok(closeVec(roomPointToXr(...xrPointToRoom(3, -1, 2)), [3, -1, 2]),
            "the point map is not self-inverse");
});

test("an XR viewer facing the way the session started is a listener facing room ahead", () => {
  const q = xrQuatToRoom([0, 0, 0, 1]);
  assert.ok(sameRotation(q, [0, 0, 0, 1]), `identity XR orientation gave ${q}`);
  /* And the position of that viewer is the room origin, so a session started at the array's
   * nominal listening point needs no offset. */
  assert.deepEqual(xrPointToRoom(0, 0, 0), [-0, 0, -0]);
});

test("the orientation map carries every head axis onto its room twin", () => {
  /* THE CHECK. For each of the viewer's three local axes, the physical direction it points is
   * M * (R_xr * xr_axis) in room space. The engine's listener quaternion has to turn the ROOM
   * convention's corresponding axis onto exactly that. Ahead, up and right at once pins the
   * rotation completely, including the handedness. */
  const pairs = [[XR_AHEAD, ROOM_AHEAD], [XR_UP, ROOM_UP], [XR_RIGHT, ROOM_RIGHT]];
  for (const qXr of sampleQuats()) {
    const qRoom = xrQuatToRoom(qXr);
    for (const [xrAxis, roomAxis] of pairs) {
      const inRoom = xrPointToRoom(...qrot(qXr, xrAxis));
      const byQuat = qrot(qRoom, roomAxis);
      assert.ok(closeVec(inRoom, byQuat, 1e-5),
                `axis ${xrAxis} under ${qXr}: XR gave ${inRoom}, the room quaternion gave ${byQuat}`);
    }
    assert.ok(sameRotation(roomQuatToXr(qRoom), qXr, 1e-5), "the orientation map is not invertible");
  }
});

test("a quarter turn left in XR is a quarter turn left in the room", () => {
  /* A positive rotation about +y takes the XR viewer's ahead (0,0,-1) toward XR -x, which is the
   * viewer's LEFT. In the room the same positive rotation takes room ahead (0,0,1) toward room
   * +x, which is the LISTENER's left because BWA_ROOM_RIGHT is -x. So the two conventions agree
   * about which way is left, and a Y rotation maps to itself. */
  const yawL = quatAxisAngle([0, 1, 0], Math.PI / 2);
  const room = xrQuatToRoom(yawL);
  assert.ok(sameRotation(room, yawL, 1e-6), `a left quarter turn became ${room}`);
  assert.ok(closeVec(qrot(room, ROOM_AHEAD), [1, 0, 0], 1e-6),
            "after turning left the listener does not face room +x");

  /* The audible consequence, which is what the driver asserts against a real render: with the
   * head turned a quarter to the left, a source straight ahead in the room is now on the
   * listener's RIGHT, and a source out on the room's +x wall is straight ahead. */
  const head = [0, 1.5, 0];
  assert.equal(earSideOf([0, 1.5, 2], head, room), -1, "the source ahead did not move to the right ear");
  assert.equal(earSideOf([2, 1.5, 0], head, room), 0, "the source at room +x is not centered");
  /* and with no turn at all, ahead is centered and room +x is on the left */
  const id = [0, 0, 0, 1];
  assert.equal(earSideOf([0, 1.5, 2], head, id), 0, "a source straight ahead is not centered");
  assert.equal(earSideOf([2, 1.5, 0], head, id), 1, "a source at room +x is not on the LEFT");
  assert.equal(earSideOf([-2, 1.5, 0], head, id), -1, "a source at room -x is not on the RIGHT");
});

test("a whole XRRigidTransform converts in one call", () => {
  const t = {
    position: { x: 0.4, y: 1.62, z: -1.1, w: 1 },
    orientation: { x: 0, y: Math.sin(Math.PI / 8), z: 0, w: Math.cos(Math.PI / 8) },
  };
  const r = xrTransformToRoom(t);
  assert.ok(closeVec(r.p, [-0.4, 1.62, 1.1]), `position came out ${r.p}`);
  assert.ok(sameRotation(r.q, [0, Math.sin(Math.PI / 8), 0, Math.cos(Math.PI / 8)], 1e-6),
            `orientation came out ${r.q}`);
  /* A viewer standing 1.1 m down the XR -z axis, the way they are facing, is 1.1 m INTO the room
   * along +z. A sign error here walks the listener backwards through every scene. */
  assert.ok(r.p[2] > 0, "walking forward in XR did not move the listener toward room ahead");
});

test("the composition matches a hand-built quaternion sandwich", () => {
  /* An independent route to the same answer, so a typo in the multiplied-out form cannot pass:
   * build q_room = qY * q_xr * qY with the general Hamilton product. */
  for (const q of sampleQuats()) {
    const sandwich = qmul(qmul(XR_TO_ROOM_QUAT, q), XR_TO_ROOM_QUAT);
    assert.ok(sameRotation(xrQuatToRoom(q), sandwich, 1e-6),
              `${q}: closed form ${xrQuatToRoom(q)} vs sandwich ${sandwich}`);
  }
});

test("the pose lead extrapolates a steady walk and gives up across a gap", () => {
  const lead = new PoseLead();
  const dt = 1 / 72;                       /* a headset frame */
  let p = [0, 1.6, 0];
  let out = null;
  /* Walk at 1.2 m/s along room +z for a second, so the 100 ms average is fully settled. */
  for (let i = 0; i < 72; ++i) {
    p = [0, 1.6, p[2] + 1.2 * dt];
    out = lead.update(p, i * dt, 0.03);
  }
  assert.ok(Math.abs(lead.speed() - 1.2) < 0.02, `the velocity estimate settled at ${lead.speed()}`);
  assert.ok(Math.abs(out[2] - (p[2] + 1.2 * 0.03)) < 2e-3,
            `a 30 ms lead at 1.2 m/s should be 3.6 cm ahead; got ${(out[2] - p[2]).toFixed(4)} m`);

  /* Zero lead is the identity, which is what the knob's minimum has to mean. */
  const same = lead.update(p, 2.0, 0);
  assert.deepEqual(same, [p[0], p[1], p[2]]);

  /* A gap longer than 250 ms means tracking was lost. Extrapolating across it would fling the
   * listener, so the estimator restarts instead. rt.c does the same, for the same reason. */
  lead.reset();
  lead.update([0, 1.6, 0], 0, 0.03);
  lead.update([0, 1.6, 0.05], 0.014, 0.03);
  assert.ok(lead.speed() > 0.1, "the estimator never started");
  const after = lead.update([0, 1.6, 0.06], 5.0, 0.03);
  assert.equal(lead.speed(), 0, "a five second gap did not reset the velocity estimate");
  assert.deepEqual(after, [0, 1.6, 0.06], "the pose after a gap was still extrapolated");
});
