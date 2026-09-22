/**
 * world_xr.js - the three.js half of the XR page, as a thin layer over the playground's `World`.
 *
 * It EXTENDS `playground/world.js` rather than copying it. Every gizmo the scenes touch (the head,
 * the 26 speaker cones, the source marker, the link line, the trail, `addPanel` / `addBoxWire` /
 * `addLine` / `addSphere` and `clearScene`) is the playground's, so a scene runs here unmodified
 * and a fix to a gizmo lands on both pages at once. What this class adds is the three things a
 * flat page never needs.
 *
 * 1. THE XR RIG. three.js puts the XR camera's pose straight into WORLD space, and the world here
 *    is the ROOM (playground/frame.js: the room-to-three map is the identity). The XR reference
 *    space is not the room; it is the room turned 180 degrees about up, so that the direction the
 *    user already faces at session start is the room's AHEAD. `frame_xr.js` derives that. So the
 *    camera goes inside a Group carrying exactly that rotation, and three's `updateCamera`
 *    composes it (`camera.matrixWorld = parent.matrixWorld * poseMatrix`, WebXRManager). The
 *    audio path does the same rotation arithmetically through `xrTransformToRoom`, and
 *    tests/xr_frame.test.mjs pins the two to each other - if they ever disagreed, the picture and
 *    the sound would disagree about where the listener is.
 *
 * 2. CONTROLLER VISUALS, placed in ROOM space. They are NOT parented to the rig and they do not
 *    use three's own `getController`, because the page reads controller poses itself out of the
 *    XRFrame and converts them through the one seam. One conversion, one place, one test.
 *
 * 3. A RENDER SPLIT. Presenting, three owns the camera and the framebuffer and this class only
 *    calls `render`. Not presenting, the playground's orbit camera runs unchanged, so the page is
 *    a normal flat demo before you put the headset on.
 */
import * as THREE from "../dist/vendor/three/three.module.js";
import { World } from "../playground/world.js";
import { XR_TO_ROOM_QUAT } from "./frame_xr.js";

export class XrWorld extends World {
  /** @param {HTMLCanvasElement} canvas */
  constructor(canvas) {
    super(canvas);
    this.presenting = false;

    /* The rig carries the XR camera and nothing else. Its rotation IS the seam. */
    this.rig = new THREE.Group();
    this.rig.quaternion.set(XR_TO_ROOM_QUAT[0], XR_TO_ROOM_QUAT[1],
                            XR_TO_ROOM_QUAT[2], XR_TO_ROOM_QUAT[3]);
    this.scene.add(this.rig);

    this.hands = [this._buildHand(0x7fd4ff), this._buildHand(0xffc27f)];
    for (const h of this.hands) { h.group.visible = false; this.scene.add(h.group); }
  }

  /** A grip marker and a one meter aiming ray. Room space, so nothing about it is XR specific. */
  _buildHand(color) {
    const group = new THREE.Group();
    const grip = new THREE.Mesh(
      new THREE.SphereGeometry(0.035, 12, 10),
      new THREE.MeshStandardMaterial({ color, emissive: 0x101820, roughness: 0.4 })
    );
    group.add(grip);
    const g = new THREE.BufferGeometry();
    g.setAttribute("position", new THREE.BufferAttribute(new Float32Array(6), 3));
    const ray = new THREE.Line(g, new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.7 }));
    group.add(ray);
    return { group, grip, ray, color: new THREE.Color(color) };
  }

  /**
   * Draw one controller. `p` and `dir` are ROOM space, already through the seam.
   * @param {number} i 0 or 1
   * @param {number[]|null} p room position, or null to hide the controller
   * @param {number[]} dir a unit aim direction in room space
   * @param {number} len how far the ray reaches before it hits something, meters
   * @param {boolean} hot true while this controller is grabbing or pointing at the menu
   */
  setHand(i, p, dir, len = 1.2, hot = false) {
    const h = this.hands[i];
    if (!h) return;
    if (!p) { h.group.visible = false; return; }
    h.group.visible = true;
    h.grip.position.set(p[0], p[1], p[2]);
    const a = h.ray.geometry.attributes.position;
    a.array[0] = p[0]; a.array[1] = p[1]; a.array[2] = p[2];
    a.array[3] = p[0] + dir[0] * len;
    a.array[4] = p[1] + dir[1] * len;
    a.array[5] = p[2] + dir[2] * len;
    a.needsUpdate = true;
    h.ray.material.opacity = hot ? 1.0 : 0.45;
    h.grip.scale.setScalar(hot ? 1.35 : 1.0);
  }

  hideHands() {
    for (const h of this.hands) h.group.visible = false;
  }

  /**
   * Move the camera under the rig and let three drive it. Called once a session is bound; the
   * camera starts with no parent at all (the playground never adds it to the scene), so this is
   * the only thing that ever reparents it.
   */
  beginPresenting() {
    if (this.presenting) return;
    this.rig.add(this.camera);
    this.renderer.xr.enabled = true;
    this.presenting = true;
  }

  /**
   * Back to the flat orbit view. The hands are hidden unconditionally, because a session can end
   * without ever having been bound to the renderer (`session.js` allows that on purpose) and the
   * controller markers would otherwise be left floating in the flat view.
   */
  endPresenting() {
    this.hideHands();
    if (!this.presenting) return;
    this.rig.remove(this.camera);
    this.renderer.xr.enabled = false;
    this.presenting = false;
    this.resize();
  }

  render() {
    if (this.presenting) {
      /* three's WebXRManager already wrote the camera from this frame's viewer pose (and through
       * the rig's rotation), so touching the camera here would fight it. */
      this.renderer.render(this.scene, this.camera);
      return;
    }
    super.render();
  }
}
