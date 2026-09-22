/**
 * session.js - the immersive-vr session: request it, hold a reference space, and pump one frame.
 *
 * THE POSE PATH DOES NOT DEPEND ON THE RENDERER, and that is a design decision rather than an
 * accident of testing. Binding a session to WebGL is a chain of things that can fail on their own
 * (`makeXRCompatible`, an `XRWebGLLayer` or a projection layer, `updateRenderState`), and none of
 * them has anything to do with where the user's head is. A listener pose that stopped following
 * the head because a graphics binding failed would be the wrong failure mode, and it would also
 * mean the head-tracking path could only ever be exercised on a headset. So: the session, the
 * reference space, the viewer pose and the conversion live here; the renderer is a SUBSCRIBER
 * that is asked to bind and is allowed to decline. When it binds, three.js drives the animation
 * loop (it has to, because it re-routes `setAnimationLoop` onto the session). When it does not,
 * this drives `session.requestAnimationFrame` itself and the same `onFrame` runs.
 *
 * `tests/run-xr.mjs` is the second beneficiary: a fake `navigator.xr` in a desktop browser takes
 * the second branch, so the head-pose path runs end to end with no headset and no WebXR.
 *
 * THE REFERENCE SPACE IS `local-floor` where it exists. Its y = 0 is the physical floor, and the
 * room frame's y = 0 is the room floor (bw_audio.h: "the origin sits ON THE FLOOR"), so the two
 * agree with no offset and a standing user's ears land at their real height. `local` is the
 * fallback, and then the page adds a nominal eye height, which is a guess and is reported as one.
 */
import { xrTransformToRoom, PoseLead } from "./frame_xr.js";

/** The eye height assumed when the runtime gives no floor. Matches the playground's head gizmo. */
export const NOMINAL_EYE_HEIGHT = 1.5;

export class XrRuntime {
  constructor() {
    this.session = null;
    this.refSpace = null;
    this.refSpaceType = null;
    this.renderReady = false;
    this.lead = new PoseLead();
    this.lastTime = 0;
    this.frames = 0;
    this.poseLost = 0;
    this.notes = [];
    this.domOverlay = false;
  }

  /** @returns {Promise<boolean>} whether this browser can open an immersive-vr session. */
  static async supported() {
    if (!globalThis.navigator || !navigator.xr || !navigator.xr.isSessionSupported) return false;
    try { return await navigator.xr.isSessionSupported("immersive-vr"); } catch { return false; }
  }

  get presenting() { return this.session !== null; }

  /**
   * Open a session and start pumping frames.
   *
   * @param {object} opts
   * @param {object} [opts.renderer] a three.js WebGLRenderer, or null for no rendering at all
   * @param {Element} [opts.domOverlayRoot] asked for as `dom-overlay`; see panel.js for why it is
   *        almost never granted on an immersive-vr session
   * @param {function} opts.onFrame `(timeMs, XRFrame, runtime)`, once per XR animation frame
   * @param {function} [opts.onEnd] called when the session ends, for any reason
   */
  async enter({ renderer, domOverlayRoot, onFrame, onEnd }) {
    if (!navigator.xr) throw new Error("this browser has no WebXR");
    const init = { optionalFeatures: ["local-floor", "bounded-floor"] };
    if (domOverlayRoot) {
      init.optionalFeatures.push("dom-overlay");
      init.domOverlay = { root: domOverlayRoot };
    }
    const session = await navigator.xr.requestSession("immersive-vr", init);
    this.session = session;
    this.onFrame = onFrame;
    this.lead.reset();
    this.frames = 0;
    this.poseLost = 0;
    this.notes = [];

    /* The overlay is reported by the runtime, never assumed from the request. */
    this.domOverlay = !!(session.domOverlayState && session.domOverlayState.type);

    this.refSpace = null;
    for (const type of ["local-floor", "local"]) {
      try {
        this.refSpace = await session.requestReferenceSpace(type);
        this.refSpaceType = type;
        break;
      } catch (e) {
        this.notes.push(`reference space "${type}" refused: ${e.message}`);
      }
    }
    if (!this.refSpace) {
      await this.end();
      throw new Error("no usable reference space (tried local-floor and local)");
    }
    if (this.refSpaceType !== "local-floor") {
      this.notes.push(`no local-floor reference space, so ear height is the nominal ` +
                      `${NOMINAL_EYE_HEIGHT} m rather than a measured one`);
    }

    session.addEventListener("end", () => {
      this.session = null;
      this.refSpace = null;
      this.renderReady = false;
      onEnd?.();
    });

    this.renderReady = false;
    if (renderer) {
      try {
        await renderer.xr.setSession(session);
        this.renderReady = true;
      } catch (e) {
        /* Keep going deliberately. The audio is the point of this page and it does not need a
         * framebuffer; a bound-less session still tracks the head. */
        this.notes.push(`the renderer could not bind the session (${e.message}); ` +
                        `head tracking and audio continue with no picture`);
      }
    }

    if (this.renderReady) {
      renderer.setAnimationLoop((t, frame) => this._tick(t, frame));
    } else {
      const loop = (t, frame) => {
        if (!this.session) return;
        this.session.requestAnimationFrame(loop);
        this._tick(t, frame);
      };
      session.requestAnimationFrame(loop);
    }
    return session;
  }

  _tick(timeMs, frame) {
    if (!frame || !this.session) return;
    this.frames++;
    this.lastTime = timeMs;
    this.onFrame?.(timeMs, frame, this);
  }

  /**
   * This frame's viewer pose, in ROOM space, with the prediction lead applied to the position.
   *
   * WebXR's viewer pose is already predicted to the frame's DISPLAY time, so `leadSeconds` here
   * is the extra distance from photons to EARS, not the whole motion-to-photon latency. See
   * `frame_xr.js` `PoseLead` for why the page leads the pose rather than
   * `bwa_set_pose_prediction`.
   *
   * @param {XRFrame} frame
   * @param {number} timeMs the animation callback's timestamp, which is the predicted display time
   * @param {number} leadSeconds
   * @returns {{p:number[], q:number[], emulated:boolean}|null} null when tracking dropped out
   */
  viewerPose(frame, timeMs, leadSeconds) {
    if (!this.refSpace) return null;
    const pose = frame.getViewerPose(this.refSpace);
    if (!pose) { this.poseLost++; this.lead.reset(); return null; }
    const { p, q } = xrTransformToRoom(pose.transform);
    if (this.refSpaceType !== "local-floor") p[1] += NOMINAL_EYE_HEIGHT;
    return {
      p: this.lead.update(p, timeMs / 1000, leadSeconds),
      q,
      emulated: pose.emulatedPosition === true,
    };
  }

  async end() {
    const s = this.session;
    this.session = null;
    if (s) { try { await s.end(); } catch { /* already gone */ } }
  }
}
