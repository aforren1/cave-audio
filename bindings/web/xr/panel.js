/**
 * panel.js - the menu, as a plane in the room with a canvas painted on it.
 *
 * WHY AN IN-WORLD PANEL AND NOT THE DOM OVERLAY MODULE. The WebXR DOM Overlay module is specified
 * for HANDHELD AR and every shipping runtime exposes it only on `immersive-ar` sessions: request
 * `dom-overlay` on an `immersive-vr` session and either the feature is refused or
 * `session.domOverlayState` comes back undefined, so a headset user would be staring at a menu
 * that exists only on the phone screen behind them. The page still ASKS for it as an optional
 * feature and uses the DOM menu when the runtime says it granted one (`main.js` checks
 * `session.domOverlayState`); this panel is what runs otherwise, which in practice is every
 * headset. `bindings/web/README.md` states the same thing in one line.
 *
 * IT DRAWS THE SAME DESCRIPTORS THE DOM MENU DOES. `playground/ui.js` takes a list of
 * `{kind, label, get, set, ...}` objects and builds DOM; this takes the same list and paints it.
 * A scene therefore gets its controls in XR for free, with no XR code in the scene and no second
 * spelling of the option set. The kinds are ui.js's kinds: toggle, slider, select, buttons, note.
 *
 * THE HIT TEST IS THE SEAM AGAIN, and it is arithmetic rather than a `THREE.Raycaster` on
 * purpose: the controller ray already exists in ROOM space (main.js converted it once through
 * frame_xr.js), and handing it to a raycaster would mean converting into three's object space and
 * back. One plane, one dot product.
 */
import * as THREE from "../dist/vendor/three/three.module.js";

const CANVAS_W = 512;
const CANVAS_H = 896;
const PANEL_W = 0.52;                       /* meters. About an A4 page held at arm's length. */
const PANEL_H = 0.91;
const PAD = 18;

const INK = "#dde3ea";
const DIM = "#97a3b2";
const ACCENT = "#4ade80";
const LINE = "#29313d";

export class MenuPanel {
  /** @param {THREE.Scene} scene the room-space scene to add the plane to */
  constructor(scene) {
    this.canvas = document.createElement("canvas");
    this.canvas.width = CANVAS_W;
    this.canvas.height = CANVAS_H;
    this.g = this.canvas.getContext("2d");
    this.texture = new THREE.CanvasTexture(this.canvas);
    this.mesh = new THREE.Mesh(
      new THREE.PlaneGeometry(PANEL_W, PANEL_H),
      new THREE.MeshBasicMaterial({ map: this.texture, transparent: true })
    );
    this.mesh.visible = false;
    scene.add(this.mesh);

    /* Room-space pose. `center` is the plane's middle; `axU` runs along the canvas's +x as the
     * viewer sees it and `axV` along its +y (downward). `normal` points at the viewer. */
    this.center = [0, 1.4, 1];
    this.axU = [-1, 0, 0];
    this.axV = [0, -1, 0];
    this.normal = [0, 0, -1];

    this.items = [];
    this.rows = [];
    this.title = "";
    this.scroll = 0;
    this.maxScroll = 0;
    this.hover = -1;
    this.hoverU = 0;
    this._dirty = true;
    this._frozen = false;
  }

  get visible() { return this.mesh.visible; }
  set visible(v) { this.mesh.visible = v; }

  /**
   * Replace the control list. Cheap enough to call every frame, because it only repaints when the
   * list's SHAPE changed; a live slider's value change is caught by `refresh()` instead.
   * @param {string} title
   * @param {object[]} items ui.js control descriptors
   */
  setItems(title, items) {
    const sig = title + "|" + items.map((i) => `${i.kind}:${i.label || i.text || ""}`).join(",");
    this.items = items;
    this.title = title;
    if (sig !== this._sig) { this._sig = sig; this.scroll = 0; }
    this._dirty = true;
  }

  /** Mark the painting stale without changing the list. */
  refresh() { this._dirty = true; }

  /**
   * Put the panel where the user can read it: in front of the head and off to one side, facing
   * the head, with a lazy follow so it does not swim while you point at it.
   *
   * The follow is FROZEN while a controller ray is on the panel. A menu that slides away from the
   * pointer as you turn your head to look at it is the one interaction people cannot learn.
   *
   * @param {number[]} head room position of the listener
   * @param {number[]} q the head's room orientation, xyzw
   * @param {number} dt seconds
   * @param {function} rot a quaternion-rotate helper, `(q, v) => v`
   */
  follow(head, q, dt, rot) {
    /* Head-local, in the ROOM convention: ahead is +z, up is +y, LEFT is +x. So this sits three
     * quarters of a meter ahead, a third of a meter to the left and a little below eye height,
     * which is where a person naturally parks a clipboard. */
    const want = rot(q, [0.34, -0.14, 0.72]);
    const target = [head[0] + want[0], head[1] + want[1], head[2] + want[2]];
    if (!this._placed) { this.center = target; this._placed = true; }
    else if (!this._frozen) {
      const k = Math.min(1, 2.5 * dt);
      for (let i = 0; i < 3; ++i) this.center[i] += (target[i] - this.center[i]) * k;
    }
    this._aimAt(head);
  }

  /** Point the plane's face at `head` and rebuild the canvas basis from that. */
  _aimAt(head) {
    const n = norm([head[0] - this.center[0], head[1] - this.center[1], head[2] - this.center[2]]);
    if (!n) return;
    this.normal = n;
    /* The viewer looks along -n with world up, so the viewer's right is up x n (playground/
     * frame.js derives the same cross product for the flat camera). Canvas +x follows it. */
    const u = norm(cross([0, 1, 0], n)) || [1, 0, 0];
    const up = cross(n, u);
    this.axU = u;
    this.axV = [-up[0], -up[1], -up[2]];
    const m = new THREE.Matrix4().makeBasis(
      new THREE.Vector3(u[0], u[1], u[2]),
      new THREE.Vector3(up[0], up[1], up[2]),
      new THREE.Vector3(n[0], n[1], n[2])
    );
    this.mesh.quaternion.setFromRotationMatrix(m);
    this.mesh.position.set(this.center[0], this.center[1], this.center[2]);
  }

  /**
   * Where a room-space ray meets the panel.
   * @param {number[]} o ray origin, room space
   * @param {number[]} d unit ray direction, room space
   * @returns {{t:number, x:number, y:number, row:number, u:number}|null} canvas pixels and the row
   */
  hit(o, d) {
    if (!this.mesh.visible) return null;
    const dn = dot(d, this.normal);
    if (Math.abs(dn) < 1e-6) return null;
    const t = dot([this.center[0] - o[0], this.center[1] - o[1], this.center[2] - o[2]], this.normal) / dn;
    if (t <= 0 || t > 8) return null;
    const p = [o[0] + d[0] * t - this.center[0], o[1] + d[1] * t - this.center[1], o[2] + d[2] * t - this.center[2]];
    const su = dot(p, this.axU) / PANEL_W + 0.5;
    const sv = dot(p, this.axV) / PANEL_H + 0.5;
    if (su < 0 || su > 1 || sv < 0 || sv > 1) return null;
    const x = su * CANVAS_W;
    const y = sv * CANVAS_H;
    let row = -1;
    for (let i = 0; i < this.rows.length; ++i) {
      const r = this.rows[i];
      if (y >= r.y0 - this.scroll && y < r.y1 - this.scroll && r.index >= 0) { row = i; break; }
    }
    const r = row >= 0 ? this.rows[row] : null;
    return { t, x, y, row, u: r ? (x - PAD) / (CANVAS_W - 2 * PAD) : 0 };
  }

  /** Called every frame with the current pointing result, so the panel can light a row. */
  setHover(h) {
    this._frozen = !!h;
    const row = h ? h.row : -1;
    const u = h ? h.u : 0;
    if (row !== this.hover || Math.abs(u - this.hoverU) > 0.004) this._dirty = true;
    this.hover = row;
    this.hoverU = u;
  }

  /**
   * Everything a caller needs to aim at a row from outside: the plane's room-space pose, its
   * size in both units, and the row rectangles the last paint produced. `tests/run-xr.mjs` uses
   * it to point a controller at a real control rather than calling `activate()` behind the
   * panel's back, which would prove nothing about the hit test.
   */
  describe() {
    return {
      visible: this.mesh.visible,
      title: this.title,
      center: this.center.slice(),
      axU: this.axU.slice(),
      axV: this.axV.slice(),
      normal: this.normal.slice(),
      sizeMeters: [PANEL_W, PANEL_H],
      canvas: [CANVAS_W, CANVAS_H],
      scroll: this.scroll,
      maxScroll: this.maxScroll,
      hover: this.hover,
      rows: this.rows.map((r) => ({ y0: r.y0, y1: r.y1, index: r.index })),
    };
  }

  /**
   * The room-space point a canvas pixel sits at, so a caller can aim at one.
   * @param {number} x canvas pixels
   * @param {number} y canvas pixels, already including `scroll`
   */
  pointAt(x, y) {
    const su = x / CANVAS_W - 0.5;
    const sv = y / CANVAS_H - 0.5;
    return [
      this.center[0] + this.axU[0] * su * PANEL_W + this.axV[0] * sv * PANEL_H,
      this.center[1] + this.axU[1] * su * PANEL_W + this.axV[1] * sv * PANEL_H,
      this.center[2] + this.axU[2] * su * PANEL_W + this.axV[2] * sv * PANEL_H,
    ];
  }

  /** Scroll by canvas pixels, clamped. */
  scrollBy(px) {
    const next = Math.max(0, Math.min(this.maxScroll, this.scroll + px));
    if (next !== this.scroll) { this.scroll = next; this._dirty = true; }
  }

  /**
   * Act on a trigger press at the last hover point. Returns true when it consumed the press,
   * which is how the caller knows this was a menu click and not a grab.
   */
  activate() {
    if (this.hover < 0) return false;
    const r = this.rows[this.hover];
    const it = this.items[r.index];
    if (!it) return false;
    const u = Math.max(0, Math.min(1, this.hoverU));
    switch (it.kind) {
      case "toggle":
        it.set(!it.get());
        break;
      case "select": {
        const n = it.options.length;
        const cur = Number(it.get()) || 0;
        /* The edges step, the middle advances. Two ways to reach the same option, because a
         * controller ray is not a mouse and a 20 percent target is about the smallest a person
         * hits reliably at arm's length. */
        const next = u < 0.2 ? (cur - 1 + n) % n : (cur + 1) % n;
        it.set(next);
        break;
      }
      case "slider": {
        const f = Math.max(0, Math.min(1, (u - 0.02) / 0.96));
        const step = it.step ?? (it.max - it.min) / 100;
        let v = it.min + f * (it.max - it.min);
        v = Math.round(v / step) * step;
        it.set(Math.max(it.min, Math.min(it.max, v)));
        break;
      }
      case "buttons": {
        const i = Math.max(0, Math.min(it.items.length - 1, Math.floor(u * it.items.length)));
        it.items[i].onClick();
        break;
      }
      default:
        return false;
    }
    this._dirty = true;
    return true;
  }

  /** Repaint if anything changed. Call once per frame; it is a no-op most frames. */
  draw() {
    if (!this._dirty || !this.mesh.visible) return;
    this._dirty = false;
    const g = this.g;
    g.clearRect(0, 0, CANVAS_W, CANVAS_H);
    g.fillStyle = "rgba(15, 21, 30, 0.93)";
    g.fillRect(0, 0, CANVAS_W, CANVAS_H);
    g.strokeStyle = LINE;
    g.lineWidth = 3;
    g.strokeRect(1.5, 1.5, CANVAS_W - 3, CANVAS_H - 3);

    this.rows = [];
    let y = PAD;
    g.textBaseline = "top";

    g.fillStyle = ACCENT;
    g.font = "600 25px system-ui, sans-serif";
    g.fillText(this.title, PAD, y);
    y += 36;
    g.strokeStyle = LINE;
    g.lineWidth = 2;
    line(g, PAD, y, CANVAS_W - PAD, y);
    y += 12;
    const top = y;

    g.save();
    g.beginPath();
    g.rect(0, top, CANVAS_W, CANVAS_H - top - PAD);
    g.clip();
    g.translate(0, -this.scroll);

    for (let i = 0; i < this.items.length; ++i) {
      const it = this.items[i];
      const y0 = y;
      y = this._row(g, it, i, y);
      this.rows.push({ y0, y1: y, index: it.kind === "note" ? -1 : i });
      y += 8;
    }
    g.restore();

    const visible = CANVAS_H - top - PAD;
    this.maxScroll = Math.max(0, y - top - visible);
    if (this.scroll > this.maxScroll) this.scroll = this.maxScroll;
    if (this.maxScroll > 0) {
      /* A scroll bar, so "there is more below" is visible rather than discovered. */
      const h = Math.max(30, (visible / (y - top)) * visible);
      const t = top + (this.scroll / this.maxScroll) * (visible - h);
      g.fillStyle = "#2b3444";
      g.fillRect(CANVAS_W - 10, top, 5, visible);
      g.fillStyle = DIM;
      g.fillRect(CANVAS_W - 10, t, 5, h);
    }
    this.texture.needsUpdate = true;
  }

  _row(g, it, index, y) {
    const w = CANVAS_W - 2 * PAD;
    const hot = this.rows.length === this.hover;
    if (hot && it.kind !== "note") {
      g.fillStyle = "rgba(74, 222, 128, 0.10)";
      g.fillRect(PAD - 6, y - 4, w + 12, rowHeight(g, it) + 8);
    }
    switch (it.kind) {
      case "note": {
        g.fillStyle = DIM;
        g.font = "17px system-ui, sans-serif";
        return wrap(g, it.text, PAD, y, w, 21);
      }
      case "toggle": {
        g.strokeStyle = it.get() ? ACCENT : DIM;
        g.lineWidth = 2;
        g.strokeRect(PAD, y + 2, 22, 22);
        if (it.get()) { g.fillStyle = ACCENT; g.fillRect(PAD + 5, y + 7, 12, 12); }
        g.fillStyle = INK;
        g.font = "19px system-ui, sans-serif";
        g.fillText(it.label, PAD + 34, y + 3);
        return y + 30;
      }
      case "select": {
        g.fillStyle = DIM;
        g.font = "16px system-ui, sans-serif";
        g.fillText(it.label, PAD, y);
        const v = it.options[Number(it.get()) || 0] ?? "";
        g.fillStyle = "#1e2733";
        g.fillRect(PAD, y + 20, w, 32);
        g.fillStyle = INK;
        g.font = "19px system-ui, sans-serif";
        g.fillText(clip(g, v, w - 70), PAD + 34, y + 25);
        g.fillStyle = ACCENT;
        g.fillText("<", PAD + 10, y + 25);
        g.fillText(">", PAD + w - 24, y + 25);
        return y + 56;
      }
      case "slider": {
        const val = Number(it.get());
        const f = (val - it.min) / (it.max - it.min || 1);
        g.fillStyle = DIM;
        g.font = "16px system-ui, sans-serif";
        g.fillText(it.label, PAD, y);
        const shown = it.format ? it.format(val) : String(round3(val));
        const tw = g.measureText(shown).width;
        g.fillText(shown, CANVAS_W - PAD - tw, y);
        g.fillStyle = "#1e2733";
        g.fillRect(PAD, y + 26, w, 14);
        g.fillStyle = ACCENT;
        g.fillRect(PAD, y + 26, Math.max(2, w * Math.max(0, Math.min(1, f))), 14);
        return y + 48;
      }
      case "buttons": {
        g.fillStyle = DIM;
        g.font = "16px system-ui, sans-serif";
        if (it.label) g.fillText(it.label, PAD, y);
        const y0 = it.label ? y + 20 : y;
        const n = it.items.length;
        const bw = w / n;
        g.font = "18px system-ui, sans-serif";
        for (let i = 0; i < n; ++i) {
          g.fillStyle = "#1e2733";
          g.fillRect(PAD + i * bw + 3, y0, bw - 6, 32);
          g.fillStyle = INK;
          const t = clip(g, it.items[i].label, bw - 16);
          g.fillText(t, PAD + i * bw + (bw - g.measureText(t).width) / 2, y0 + 5);
        }
        return y0 + 36;
      }
      default:
        return y;
    }
  }
}

function rowHeight(g, it) {
  switch (it.kind) {
    case "toggle": return 30;
    case "select": return 56;
    case "slider": return 48;
    case "buttons": return it.label ? 56 : 36;
    default: return 20;
  }
}

function wrap(g, text, x, y, w, lh) {
  const words = String(text).split(/\s+/);
  let ln = "";
  for (const word of words) {
    const t = ln ? ln + " " + word : word;
    if (g.measureText(t).width > w && ln) { g.fillText(ln, x, y); y += lh; ln = word; }
    else ln = t;
  }
  if (ln) { g.fillText(ln, x, y); y += lh; }
  return y;
}

function clip(g, s, w) {
  let t = String(s);
  while (t.length > 3 && g.measureText(t).width > w) t = t.slice(0, -2);
  return t;
}

function round3(v) { return Math.round(v * 1000) / 1000; }
function line(g, x0, y0, x1, y1) { g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke(); }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
function norm(v) {
  const n = Math.hypot(v[0], v[1], v[2]);
  return n > 1e-9 ? [v[0] / n, v[1] / n, v[2] / n] : null;
}
