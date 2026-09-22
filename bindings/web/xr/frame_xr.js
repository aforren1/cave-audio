/**
 * frame_xr.js - THE WebXR coordinate seam, and the only place in the XR page that knows about it.
 *
 * It is a sibling of playground/frame.js, not a replacement. That file owns the ROOM to THREE.JS
 * WORLD half and finds it to be the identity: the engine's room frame and three.js both use a
 * right-handed basis with +y up, so room x,y,z ARE world x,y,z and the only hard part there is
 * where the camera stands. This file owns the half the flat playground never needs: the XR
 * REFERENCE SPACE to room map, for a POSITION and for an ORIENTATION.
 *
 * ---------------------------------------------------------------------------------------------
 * THE DERIVATION, in full, because getting it wrong is inaudible until you turn your head.
 *
 * (1) The two conventions.
 *     Room (bw_audio.h BWA_ROOM_*): +y UP, +z AHEAD, +x LEFT. An identity listener quaternion
 *     faces room +z and its RIGHT ear is at room -x.
 *     WebXR / three.js head-local: +y UP, -z AHEAD (a camera looks down its own -z), +x RIGHT.
 *
 * (2) The head-local basis map B: room-canonical -> XR-canonical.
 *     room ahead (0,0,1)  -> xr ahead (0,0,-1)
 *     room up    (0,1,0)  -> xr up    (0,1,0)
 *     room right (-1,0,0) -> xr right (1,0,0), so room +x -> xr -x
 *     B = diag(-1, 1, -1).
 *
 * (3) THAT IS NOT A MIRROR. diag(-1,1,-1) has determinant +1. It is a 180 degree rotation about
 *     the up axis. The tempting reading - "room +x is LEFT, three +x is RIGHT, so the seam is a
 *     reflection across the YZ plane" - is wrong, and it is wrong in the one direction that a
 *     static test never catches: a reflection and a yaw agree on where a source SITS and disagree
 *     on which way a head is FACING. The handedness flip a reflection would introduce is already
 *     spent by the second sign, the one on the forward axis, because the two conventions disagree
 *     about ahead as well as about left. Two sign flips compose to a proper rotation.
 *
 * (4) The space map M: XR reference space -> room space. This one is a CHOICE, and the choice is
 *     the same rotation, M = diag(-1, 1, -1), so that the direction a seated user is already
 *     facing when the session starts, XR -z, becomes room +z, the room's AHEAD. Pick the identity
 *     instead and a user boots up facing room -z, looking away from every scene's source. With
 *     a `local-floor` reference space, XR y = 0 is the physical floor and room y = 0 is the room
 *     floor, so the y axis needs no offset at all: the two conventions already agree.
 *
 * (5) The composition. A head-local direction d, written in the ROOM convention, is B*d in the XR
 *     convention; R_xr carries it into XR space; M carries that into room space. So
 *         R_room = M * R_xr * B,   with M and B both the 180 degree yaw qY = (0,1,0,0) in xyzw.
 *     As quaternions, q_room = qY * q_xr * qY, which multiplies out to
 *         (x, y, z, w) -> (x, -y, z, -w),
 *     the same rotation as (-x, y, -z, w) since q and -q are one rotation. An identity XR
 *     orientation maps to an identity room orientation, which is the property to remember: a
 *     user facing the way the session started is a listener facing room ahead.
 *
 * Every claim above is checked in tests/xr_frame.test.mjs against the ABI's own basis vectors
 * rather than against this algebra, so the test would fail if the algebra were wrong.
 * ---------------------------------------------------------------------------------------------
 *
 * Nothing here imports three.js. That is deliberate: the seam has to be unit-testable under node,
 * where there is no WebGL and no WebXR, and it is the one piece of this page that a mistake makes
 * silently plausible.
 */

/** The ABI's identity-listener basis, as data. Mirrors BWA_ROOM_AHEAD / UP / RIGHT. */
export const ROOM_AHEAD = [0, 0, 1];
export const ROOM_UP = [0, 1, 0];
export const ROOM_RIGHT = [-1, 0, 0];

/** The same three for a WebXR viewer, whose local ahead is -z and whose right is +x. */
export const XR_AHEAD = [0, 0, -1];
export const XR_UP = [0, 1, 0];
export const XR_RIGHT = [1, 0, 0];

/**
 * The seam's rotation, as a quaternion in xyzw: 180 degrees about +y. It is its own inverse up to
 * sign, which is why the same constant appears on both sides of the sandwich in `xrQuatToRoom`.
 * `world_xr.js` applies exactly this rotation to the three.js group that carries the XR camera,
 * so the picture and the audio cannot drift apart; tests/xr_frame.test.mjs pins them together.
 */
export const XR_TO_ROOM_QUAT = [0, 1, 0, 0];

/**
 * A point in the XR reference space -> a point in room space. The 180 degree yaw of step (4).
 * Self-inverse, so `roomPointToXr` is the same arithmetic and is spelled out separately only so
 * a caller's intent reads at the call site.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {number[]} room x, y, z in meters
 */
export function xrPointToRoom(x, y, z) {
  return [-x, y, -z];
}

/** Room space -> the XR reference space. The same self-inverse yaw. */
export function roomPointToXr(x, y, z) {
  return [-x, y, -z];
}

/**
 * A WebXR orientation quaternion -> the engine's listener quaternion.
 *
 * `q_room = qY * q_xr * qY`, multiplied out; see step (5) in the file header. The result is
 * normalized here rather than at the ABI, because a viewer pose arrives normalized and a
 * degenerate one would otherwise reach `bwa_set_listener_pose`, which silently substitutes
 * identity - and a head that snaps to identity for one frame is a click.
 *
 * @param {{x:number,y:number,z:number,w:number}|number[]} q xyzw, from XRRigidTransform.orientation
 * @returns {number[]} the room-space listener quaternion, xyzw, unit length
 */
export function xrQuatToRoom(q) {
  const x = q.x ?? q[0];
  const y = q.y ?? q[1];
  const z = q.z ?? q[2];
  const w = q.w ?? q[3];
  return normalizeQuat([x, -y, z, -w]);
}

/** The inverse map, room listener quaternion -> WebXR orientation. Same sandwich, same numbers. */
export function roomQuatToXr(q) {
  return normalizeQuat([q[0], -q[1], q[2], -q[3]]);
}

/**
 * A whole XRRigidTransform (a viewer pose, a controller's target ray) -> room position and room
 * orientation. The one call the per-frame path makes.
 * @param {{position:object, orientation:object}} t an XRRigidTransform
 * @returns {{p:number[], q:number[]}} room position in meters, room orientation xyzw
 */
export function xrTransformToRoom(t) {
  const p = t.position;
  return {
    p: xrPointToRoom(p.x, p.y, p.z),
    q: xrQuatToRoom(t.orientation),
  };
}

/** Rotate a vector by a unit quaternion, xyzw. The JavaScript twin of frame.h's `frame_qrot`. */
export function qrot(q, v) {
  const x = q[0], y = q[1], z = q[2], w = q[3];
  const tx = 2 * (y * v[2] - z * v[1]);
  const ty = 2 * (z * v[0] - x * v[2]);
  const tz = 2 * (x * v[1] - y * v[0]);
  return [
    v[0] + w * tx + (y * tz - z * ty),
    v[1] + w * ty + (z * tx - x * tz),
    v[2] + w * tz + (x * ty - y * tx),
  ];
}

/** Hamilton product, xyzw. Used by the seam's own tests and by the panel's billboard math. */
export function qmul(a, b) {
  return [
    a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  ];
}

/** @returns {number[]} a unit quaternion; a degenerate input becomes identity. */
export function normalizeQuat(q) {
  const n = Math.hypot(q[0], q[1], q[2], q[3]);
  if (!(n > 1e-8)) return [0, 0, 0, 1];
  return [q[0] / n, q[1] / n, q[2] / n, q[3] / n];
}

/**
 * Where a room point sits relative to a room-oriented head: +1 when it is on the listener's LEFT,
 * -1 on the right, 0 when it is within a degree of center. The audible claim the whole seam
 * exists to get right, as a pure function a test can call.
 *
 * "Left" is the room's own left: BWA_ROOM_RIGHT is -x, so the listener's left axis is the head
 * quaternion applied to (+1, 0, 0).
 *
 * @param {number[]} point room x,y,z
 * @param {number[]} head room x,y,z
 * @param {number[]} q the head's room orientation, xyzw
 * @returns {number} -1, 0 or +1
 */
export function earSideOf(point, head, q) {
  const left = qrot(q, [-ROOM_RIGHT[0], -ROOM_RIGHT[1], -ROOM_RIGHT[2]]);
  const d = [point[0] - head[0], point[1] - head[1], point[2] - head[2]];
  const n = Math.hypot(d[0], d[1], d[2]);
  if (!(n > 1e-9)) return 0;
  const c = (d[0] * left[0] + d[1] * left[1] + d[2] * left[2]) / n;
  return c > 0.018 ? 1 : c < -0.018 ? -1 : 0;
}

/**
 * A fixed-lead extrapolator for the listener POSITION, in room space.
 *
 * WHY THE PAGE DOES THIS AND NOT THE ENGINE. `bwa_set_pose_prediction` leads the pose that the
 * engine's OWN tracker publishes (src/core/rt.c reads it inside the `if (trk)` branch that
 * follows a NatNet connection). A pose pushed through `bwa_set_listener_pose` never passes
 * through it, so on this page that call is inert and calling it would be theater. The lead has to
 * happen where the poses are, which is here.
 *
 * WHAT THE LEAD IS FOR. WebXR already hands back a viewer pose predicted to the frame's DISPLAY
 * time, so the eyes are taken care of. The ears are not: the block the engine is about to render
 * reaches them one audio output latency later. So the lead here is motion-to-EARS minus
 * motion-to-photons, which is roughly the AudioContext's `outputLatency` plus `baseLatency` plus
 * the engine block, tens of milliseconds on a headset.
 *
 * The estimator copies the engine's own shape on purpose (rt.c, the pose-prediction block): an
 * exponential moving average over about 100 ms because a frame-to-frame velocity is jittery, a
 * speed cap so a tracking glitch cannot fling the listener, and a reset across a gap so nothing
 * extrapolates from a stale sample. Orientation is deliberately NOT led: the engine does not lead
 * it either, and an overshooting head rotation is far more audible than an overshooting position.
 */
export class PoseLead {
  constructor() {
    this.vel = [0, 0, 0];
    this.last = null;
    this.lastT = 0;
    this.valid = false;
  }

  /** Forget the velocity estimate. Call on session start and after a tracking dropout. */
  reset() {
    this.vel[0] = this.vel[1] = this.vel[2] = 0;
    this.valid = false;
    this.last = null;
  }

  /**
   * @param {number[]} p the measured room position
   * @param {number} tSeconds a monotonic timestamp in seconds (the XR frame's own time)
   * @param {number} leadSeconds how far ahead to extrapolate. 0 returns `p` unchanged.
   * @returns {number[]} the room position to hand the engine
   */
  update(p, tSeconds, leadSeconds) {
    if (this.valid && tSeconds > this.lastT) {
      const dt = tSeconds - this.lastT;
      if (dt < 0.25) {
        const a = 1 - Math.exp(-dt / 0.1);
        for (let j = 0; j < 3; ++j) {
          let vr = (p[j] - this.last[j]) / dt;
          if (vr > 5) vr = 5; else if (vr < -5) vr = -5;
          this.vel[j] += a * (vr - this.vel[j]);
        }
      } else {
        this.vel[0] = this.vel[1] = this.vel[2] = 0;   /* a gap: restart rather than guess */
      }
    }
    this.last = [p[0], p[1], p[2]];
    this.lastT = tSeconds;
    this.valid = true;
    const lead = Math.max(0, Math.min(0.2, leadSeconds || 0));
    if (lead === 0) return this.last;
    return [p[0] + this.vel[0] * lead, p[1] + this.vel[1] * lead, p[2] + this.vel[2] * lead];
  }

  /** The current smoothed speed, m/s, for the readout. */
  speed() {
    return Math.hypot(this.vel[0], this.vel[1], this.vel[2]);
  }
}
