/**
 * hands.js - the controllers: where they are, what they are pointing at, and what they move.
 *
 * IT READS THE POSES ITSELF, out of the XRFrame, rather than through `renderer.xr.getController`.
 * One reason: the whole page has exactly one XR-to-room conversion, `frame_xr.js`, and it is the
 * piece a unit test can reach. A controller whose position came from a three.js object graph
 * would be converted by a different route (the rig group's matrix), and the day the two disagreed
 * the symptom would be a grab that misses by a meter with no test anywhere near it. The other
 * reason is smaller and practical: the grab, the nudge and the menu hit test all want room-space
 * numbers, which is what the scenes and the engine speak.
 *
 * THE MAPPING, which index.html prints for the user:
 *   trigger            on the menu panel, press the control the ray is on
 *   trigger / grip     anywhere else, grab the source with the nearer hand and carry it
 *   thumbstick X       slide the source left and right, relative to where you are looking
 *   thumbstick Y       push the source away or pull it in
 *   grip + thumbstick Y   raise and lower the source instead
 *   thumbstick Y       while pointing at the menu, scroll it
 *
 * A press is resolved in that order on purpose: the menu wins over the grab, because a ray that
 * lands on the panel is unambiguous and a grab is not.
 */
import { xrTransformToRoom, qrot } from "./frame_xr.js";

const GRAB_REACH = 0.75;        /* meters: how far from the source a direct grab reaches */
const RAY_GRAB_RADIUS = 0.35;   /* meters: how near the ray must pass the source to grab at range */
const NUDGE_RATE = 1.6;         /* m/s at full stick */
const DEAD_ZONE = 0.14;         /* a resting thumbstick is never exactly zero */
const SCROLL_RATE = 900;        /* canvas pixels per second at full stick */

/** Keep the source somewhere a listener can still hear it, whatever a stuck stick asks for. */
const BOUND_XZ = 7.0;
const BOUND_Y = [0.05, 4.0];

export class Hands {
  /** @param {import("./panel.js").MenuPanel} panel */
  constructor(panel) {
    this.panel = panel;
    this.session = null;
    this.state = [newHand(), newHand()];
    this.grabbing = -1;
    this.lastHit = null;
    this._listeners = [];
  }

  /** Wire the session's own select and squeeze events. */
  attach(session) {
    this.session = session;
    const on = (name, fn) => {
      session.addEventListener(name, fn);
      this._listeners.push([name, fn]);
    };
    /* The EVENTS are authoritative rather than the gamepad buttons: a runtime may expose a hand
     * or a gaze input with no gamepad at all, and `select` is the one activation every input
     * source has. The gamepad is read for the thumbstick only. */
    on("selectstart", (e) => this._press(e.inputSource, "select", true));
    on("selectend", (e) => this._press(e.inputSource, "select", false));
    on("squeezestart", (e) => this._press(e.inputSource, "squeeze", true));
    on("squeezeend", (e) => this._press(e.inputSource, "squeeze", false));
  }

  detach() {
    if (this.session) {
      for (const [name, fn] of this._listeners) this.session.removeEventListener(name, fn);
    }
    this._listeners = [];
    this.session = null;
    this.state = [newHand(), newHand()];
    this.grabbing = -1;
  }

  _index(inputSource) {
    const list = this.session ? Array.from(this.session.inputSources) : [];
    const i = list.indexOf(inputSource);
    if (i >= 0) return Math.min(1, i);
    return inputSource && inputSource.handedness === "left" ? 0 : 1;
  }

  _press(inputSource, what, down) {
    const h = this.state[this._index(inputSource)];
    if (!h) return;
    h[what] = down;
    if (down) h.pending = true;      /* consumed by the next update(), where the poses are fresh */
    else if (this.grabbing >= 0 && !h.select && !h.squeeze) {
      const i = this.state.indexOf(h);
      if (i === this.grabbing) this.grabbing = -1;
    }
  }

  /**
   * One frame of controller work.
   *
   * @param {XRFrame} frame
   * @param {XRReferenceSpace} refSpace
   * @param {object} ctx the scene context: `sourceRoom`, `headRoom`, `headQuat` are read/written
   * @param {number} dt seconds
   * @param {object} world the XrWorld, for the controller visuals
   * @param {function} onSourceMoved called when a hand moved the source, so a scene can stop
   *        driving it automatically the way the flat page's drag handler does
   */
  update(frame, refSpace, ctx, dt, world, onSourceMoved) {
    const sources = this.session ? Array.from(this.session.inputSources).slice(0, 2) : [];
    this.lastHit = null;
    let anyHit = null;

    for (let i = 0; i < 2; ++i) {
      const h = this.state[i];
      const src = sources[i];
      h.present = false;
      if (!src) { world?.setHand(i, null, [0, 0, 1]); continue; }

      /* A controller that is out of view has no pose for this frame. That is normal, not an
       * error, and the right answer is to leave everything where it was. */
      const pose = src.targetRaySpace ? frame.getPose(src.targetRaySpace, refSpace) : null;
      if (!pose) { world?.setHand(i, null, [0, 0, 1]); h.pending = false; continue; }
      const { p, q } = xrTransformToRoom(pose.transform);
      h.present = true;
      h.p = p;
      h.q = q;
      /* The target ray points down the controller's own -z, like every other XR "forward". Once
       * the orientation is in the room convention that axis is room AHEAD. */
      h.dir = qrot(q, [0, 0, 1]);

      const hit = this.panel ? this.panel.hit(h.p, h.dir) : null;
      if (hit && (!anyHit || hit.t < anyHit.t)) { anyHit = hit; this.pointing = i; }
      h.hit = hit;

      const gp = src.gamepad;
      h.axes = gp && gp.axes && gp.axes.length >= 4 ? [gp.axes[2], gp.axes[3]] : [0, 0];
      if (gp && gp.axes && gp.axes.length === 2) h.axes = [gp.axes[0], gp.axes[1]];
    }

    this.panel?.setHover(anyHit);
    this.lastHit = anyHit;

    /* ---- presses ---- */
    for (let i = 0; i < 2; ++i) {
      const h = this.state[i];
      if (!h.pending) continue;
      h.pending = false;
      if (!h.present) continue;
      if (h.hit && h.hit.row >= 0 && h.select) {
        /* The menu press. `activate` reads the hover the panel already resolved, so a press and
         * the highlight the user was looking at cannot disagree. */
        this.panel.setHover(h.hit);
        this.panel.activate();
        continue;
      }
      if (this._canGrab(i, ctx.sourceRoom)) {
        this.grabbing = i;
        h.grabOffset = [
          ctx.sourceRoom[0] - h.p[0], ctx.sourceRoom[1] - h.p[1], ctx.sourceRoom[2] - h.p[2],
        ];
      }
    }

    /* ---- the grab carries the source ---- */
    if (this.grabbing >= 0) {
      const h = this.state[this.grabbing];
      if (!h.present || (!h.select && !h.squeeze)) this.grabbing = -1;
      else {
        ctx.sourceRoom = clampRoom([
          h.p[0] + h.grabOffset[0], h.p[1] + h.grabOffset[1], h.p[2] + h.grabOffset[2],
        ]);
        onSourceMoved?.();
      }
    }

    /* ---- the thumbstick: scroll the menu, or nudge the source ---- */
    for (let i = 0; i < 2; ++i) {
      const h = this.state[i];
      if (!h.present) continue;
      const ax = dead(h.axes[0]);
      const ay = dead(h.axes[1]);
      if (h.hit && h.hit.row !== undefined && this.panel && h.hit.t > 0 && Math.abs(ay) > 0) {
        this.panel.scrollBy(ay * SCROLL_RATE * dt);
        continue;
      }
      if (i === this.grabbing || (ax === 0 && ay === 0)) continue;
      /* Nudges are relative to where the user is LOOKING, not to the room's axes: "push it away"
       * has to mean away from me, or the control is unusable the moment you turn around. */
      const right = qrot(ctx.headQuat, [-1, 0, 0]);      /* BWA_ROOM_RIGHT, turned with the head */
      const ahead = qrot(ctx.headQuat, [0, 0, 1]);
      const up = [0, 1, 0];
      const vy = h.squeeze ? up : ahead;
      const k = NUDGE_RATE * dt;
      ctx.sourceRoom = clampRoom([
        ctx.sourceRoom[0] + (right[0] * ax - vy[0] * ay) * k,
        ctx.sourceRoom[1] + (right[1] * ax - vy[1] * ay) * k,
        ctx.sourceRoom[2] + (right[2] * ax - vy[2] * ay) * k,
      ]);
      onSourceMoved?.();
    }

    /* ---- draw ---- */
    for (let i = 0; i < 2; ++i) {
      const h = this.state[i];
      if (!h.present) { world?.setHand(i, null, [0, 0, 1]); continue; }
      const len = h.hit ? h.hit.t : (this.grabbing === i ? 0.25 : 1.2);
      world?.setHand(i, h.p, h.dir, len, this.grabbing === i || !!h.hit);
    }
  }

  _canGrab(i, source) {
    const h = this.state[i];
    const other = this.state[1 - i];
    const d = dist(h.p, source);
    /* "The nearer controller": if both are in reach, the far one does not steal the source. */
    if (other.present && dist(other.p, source) < d - 1e-3 && (other.select || other.squeeze)) return false;
    if (d <= GRAB_REACH) return true;
    /* At range, the ray has to pass close to the marker. Distance from a point to the ray. */
    const w = [source[0] - h.p[0], source[1] - h.p[1], source[2] - h.p[2]];
    const t = w[0] * h.dir[0] + w[1] * h.dir[1] + w[2] * h.dir[2];
    if (t <= 0) return false;
    const c = [h.p[0] + h.dir[0] * t, h.p[1] + h.dir[1] * t, h.p[2] + h.dir[2] * t];
    return dist(c, source) <= RAY_GRAB_RADIUS;
  }

  /** For the readout and for the test hook. */
  report() {
    return this.state.map((h, i) => ({
      present: h.present,
      grabbing: this.grabbing === i,
      pointingAtMenu: !!h.hit,
      position: h.present ? h.p.map((v) => Math.round(v * 1000) / 1000) : null,
    }));
  }
}

function newHand() {
  return {
    present: false, p: [0, 0, 0], q: [0, 0, 0, 1], dir: [0, 0, 1],
    select: false, squeeze: false, pending: false, hit: null,
    axes: [0, 0], grabOffset: [0, 0, 0],
  };
}

function dead(v) {
  const x = Number(v) || 0;
  return Math.abs(x) < DEAD_ZONE ? 0 : x;
}

function dist(a, b) { return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

function clampRoom(p) {
  return [
    Math.max(-BOUND_XZ, Math.min(BOUND_XZ, p[0])),
    Math.max(BOUND_Y[0], Math.min(BOUND_Y[1], p[1])),
    Math.max(-BOUND_XZ, Math.min(BOUND_XZ, p[2])),
  ];
}
