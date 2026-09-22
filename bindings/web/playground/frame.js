/**
 * frame.js - THE coordinate seam, and the only place in the playground that knows about it.
 *
 * THE ENGINE'S ROOM FRAME (bw_audio.h, BWA_ROOM_*): right-handed, meters, +y UP, +z AHEAD, and so
 * +x LEFT - an identity-orientation listener's right ear is at -x. The origin is on the FLOOR at
 * the working area's center, which is why the head sits at y = 1.5 and not at 0.
 *
 * THREE.JS: right-handed, +y up, and a default camera that looks down its own -z, so +z is "toward
 * the viewer". The AXES are therefore the same three numbers and the conversion is the IDENTITY -
 * `roomToWorld` copies. That is a real finding, not an omission, and it is written as a function
 * anyway for two reasons: every scene calls it instead of copying by hand, so the day a room
 * convention changes there is one edit; and the identity is the half of the seam that is easy, the
 * hard half being the one below.
 *
 * THE HARD HALF IS THE CAMERA. Room +x is the listener's LEFT, and where a left-handed point draws
 * on the screen depends entirely on where you stand. Put the camera in front of the listener
 * (room +z, looking back) and room +x draws on the RIGHT of the screen while it still sounds on
 * the left, which is the single most confusing thing a spatial demo can do. So the default camera
 * sits BEHIND the listener, at room -z looking toward +z, over the listener's shoulder. Then the
 * screen's right is room -x, the listener's right ear, and left is left in both senses.
 *
 * Worked through: a camera looking along f = (0,0,1) with up u = (0,1,0) has view-space
 * z = -f = (0,0,-1) and view-space x = u x z = (0,1,0) x (0,0,-1) = (-1,0,0). Screen right is room
 * -x. So a source at room +x lands at negative NDC x: on the left. `screenSideOf` below is that
 * statement as code, and tests/run-playground.mjs asserts it against the live camera at the same
 * moment it asserts the audio.
 */

/** Room x of a point on the listener's LEFT, as a sign. Room +x is left, so this is +1. */
export const ROOM_LEFT_SIGN = +1;

/**
 * Room space -> three.js world space. The identity; see the file header.
 * @param {number} x room meters, +x LEFT
 * @param {number} y room meters, +y UP, 0 = the floor
 * @param {number} z room meters, +z AHEAD
 * @param {{set:Function}} out a THREE.Vector3 to fill
 * @returns {{set:Function}} `out`
 */
export function roomToWorld(x, y, z, out) {
  out.set(x, y, z);
  return out;
}

/**
 * three.js world space -> room space. The inverse of the identity, which is the identity. Used by
 * the drag handlers, which work in world space because the raycaster does.
 * @param {{x:number,y:number,z:number}} v
 * @returns {[number, number, number]} room x, y, z
 */
export function worldToRoom(v) {
  return [v.x, v.y, v.z];
}

/**
 * Where the camera stands by default: behind the listener's head, raised and pulled back, looking
 * at the head. Every scene starts here, and the orbit control moves around this.
 * @param {number} headY the listener's ear height in room meters
 * @returns {{x:number, y:number, z:number}} a room-space camera position
 */
export function defaultCameraRoom(headY) {
  return { x: 0.0, y: headY + 2.2, z: -6.0 };
}

/**
 * The SIGN of a room point's horizontal screen position, for the given camera basis: -1 means it
 * draws left of the head, +1 right. Pure, so a test can check the seam without a renderer.
 *
 * `right` is the camera's screen-right axis in room space (three.js keeps it in the first column
 * of the camera's world matrix). The projection of the head-to-point vector onto it is the answer.
 * @param {number[]} point room x,y,z
 * @param {number[]} head room x,y,z of the listener
 * @param {number[]} right the camera's screen-right axis, room space
 * @returns {number} -1, 0 or +1
 */
export function screenSideOf(point, head, right) {
  const d = (point[0] - head[0]) * right[0] +
            (point[1] - head[1]) * right[1] +
            (point[2] - head[2]) * right[2];
  return d < -1e-6 ? -1 : d > 1e-6 ? 1 : 0;
}
