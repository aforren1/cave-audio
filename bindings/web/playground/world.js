/**
 * world.js - the three.js half: one scene, one camera rig, and the gizmos every scene shares.
 *
 * It owns nothing about audio. A scene updates room-space numbers and calls the setters here; the
 * conversion into three.js world space goes through `frame.js` and nowhere else.
 *
 * THE SHARED GIZMOS are the head (with ear and nose markers taken from BWA_ROOM_*), the 26 speaker
 * cones, the source marker, the head-to-source line and the trail. A scene adds its own props to
 * `sceneGroup`, which `clearScene()` empties on every switch, so a scene cannot leak a mesh into
 * the next one.
 *
 * THE CAMERA is a hand-rolled orbit rather than OrbitControls: one addon fewer to vendor and hash,
 * and the demo needs exactly azimuth, elevation, distance and a source drag. The drag is a plane
 * intersection through the source's current position, perpendicular to the view, which is the
 * behavior anyone who has moved a gizmo expects.
 */
import * as THREE from "../dist/vendor/three/three.module.js";
import { defaultCameraRoom, roomToWorld } from "./frame.js";

const SPEAKER_CAP = 26;

/** The idle-to-driven speaker colors, and the explicit highlight a scene can ask for. */
const SPK_IDLE = new THREE.Color(0x4a5568);
const SPK_HOT = new THREE.Color(0xf0a23c);
const SPK_PICK = new THREE.Color(0x4ade80);

export class World {
  /** @param {HTMLCanvasElement} canvas */
  constructor(canvas) {
    this.canvas = canvas;
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(Math.min(2, globalThis.devicePixelRatio || 1));
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0x0d1117);
    this.camera = new THREE.PerspectiveCamera(50, 1, 0.05, 200);

    this.headRoom = [0, 1.5, 0];
    this.sourceRoom = [0, 1.5, 2];

    /* orbit state, in room space: azimuth 0 = behind the listener (room -z), looking toward +z */
    const c = defaultCameraRoom(this.headRoom[1]);
    this.dist = Math.hypot(c.x, c.z - this.headRoom[2], c.y - this.headRoom[1]);
    this.azim = Math.atan2(c.x, c.z - this.headRoom[2]);
    this.elev = Math.asin((c.y - this.headRoom[1]) / this.dist);

    this.scene.add(new THREE.HemisphereLight(0xbfd4ff, 0x20242c, 2.0));
    const key = new THREE.DirectionalLight(0xffffff, 1.6);
    key.position.set(2, 6, -4);
    this.scene.add(key);

    const grid = new THREE.GridHelper(12, 12, 0x2b3444, 0x1b2230);
    grid.position.y = 0;
    this.scene.add(grid);

    this._buildHead();
    this._buildSpeakers();
    this._buildSource();

    this.sceneGroup = new THREE.Group();
    this.scene.add(this.sceneGroup);

    this._v = new THREE.Vector3();
    this._raycaster = new THREE.Raycaster();
    this._pointer = new THREE.Vector2();
    this._dragPlane = new THREE.Plane();
    this._dragHit = new THREE.Vector3();
    this._drag = null;
    this.onSourceDrag = null;      /* (x, y, z) in room space */
    this.dragEnabled = true;

    this._bindPointer();
    this.resize();
  }

  /* ---------------------------------------------------------------- gizmos */

  _buildHead() {
    const g = new THREE.Group();
    const skin = new THREE.MeshStandardMaterial({ color: 0x6fb7e8, roughness: 0.6 });
    g.add(new THREE.Mesh(new THREE.SphereGeometry(0.16, 24, 16), skin));
    /* Ears from the ABI's own basis, not from a re-guessed convention: BWA_ROOM_RIGHT is -x, so
     * the RED (right) ear sits at -x and the white one at +x. That is the whole laterality claim
     * this demo makes, drawn. */
    const ear = (x, color) => {
      const m = new THREE.Mesh(new THREE.SphereGeometry(0.055, 12, 10),
                               new THREE.MeshStandardMaterial({ color }));
      m.position.set(x, 0, 0);
      return m;
    };
    g.add(ear(-0.17, 0xe05252));            /* right ear -> audio R */
    g.add(ear(+0.17, 0xf2f2f2));            /* left ear  -> audio L */
    const nose = new THREE.Mesh(new THREE.ConeGeometry(0.06, 0.18, 10),
                                new THREE.MeshStandardMaterial({ color: 0xf0912f }));
    nose.rotation.x = Math.PI / 2;          /* the cone's +y axis onto room +z, the ahead axis */
    nose.position.set(0, 0, 0.22);
    g.add(nose);
    this.head = g;
    this.scene.add(g);
  }

  _buildSpeakers() {
    this.speakers = [];
    this.speakerLevel = new Float32Array(SPEAKER_CAP);
    const geo = new THREE.ConeGeometry(0.12, 0.26, 12);
    for (let i = 0; i < SPEAKER_CAP; ++i) {
      const m = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({
        color: SPK_IDLE.clone(), roughness: 0.55, emissive: new THREE.Color(0x000000),
      }));
      m.visible = false;
      this.scene.add(m);
      this.speakers.push(m);
    }
    this.speakerCount = 0;
    this.speakerPos = [];
  }

  _buildSource() {
    this.source = new THREE.Mesh(
      new THREE.SphereGeometry(0.17, 20, 14),
      new THREE.MeshStandardMaterial({ color: 0xe6584a, emissive: 0x3a0f0a, roughness: 0.4 })
    );
    this.scene.add(this.source);

    const lg = new THREE.BufferGeometry();
    lg.setAttribute("position", new THREE.BufferAttribute(new Float32Array(6), 3));
    this.link = new THREE.Line(lg, new THREE.LineBasicMaterial({ color: 0x5bd67a }));
    this.scene.add(this.link);

    this.trailMax = 160;
    const tg = new THREE.BufferGeometry();
    tg.setAttribute("position", new THREE.BufferAttribute(new Float32Array(this.trailMax * 3), 3));
    tg.setDrawRange(0, 0);
    this.trail = new THREE.Line(tg, new THREE.LineBasicMaterial({ color: 0x4ade80, opacity: 0.5, transparent: true }));
    this.trail.visible = false;
    this.scene.add(this.trail);
    this.trailLen = 0;
  }

  /* ------------------------------------------------------------- the setters */

  /**
   * Place the speaker gizmos from the engine's own readback. Positions are room space, in channel
   * order, exactly as `bwa_get_speakers` filled them.
   * @param {Float32Array} xyz 3 floats per speaker
   * @param {number} count
   */
  setSpeakers(xyz, count) {
    this.speakerCount = Math.min(count, SPEAKER_CAP);
    this.speakerPos = [];
    for (let i = 0; i < SPEAKER_CAP; ++i) {
      const m = this.speakers[i];
      m.visible = i < this.speakerCount;
      if (i >= this.speakerCount) continue;
      const p = [xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]];
      this.speakerPos.push(p);
      roomToWorld(p[0], p[1], p[2], m.position);
      /* Aim each cone at the array's nominal listening point, the way the native playground does:
       * a cone that points at you reads as a driver, a cone that points anywhere else reads as a
       * mistake. */
      roomToWorld(this.headRoom[0], this.headRoom[1], this.headRoom[2], this._v);
      m.lookAt(this._v);
      m.rotateX(Math.PI / 2);
    }
  }

  /**
   * Light the speakers from the last block's per-channel bus peaks. Instant attack, ~1/3 s
   * release, over a 60 dB window, so a quiet DBAP tail still reads. Same rule as the native
   * playground's meter, for the same reason.
   * @param {Float32Array|null} peaks linear, one per channel
   * @param {number} dt seconds
   * @param {number} highlight a channel index to force green, or -1
   */
  setSpeakerLevels(peaks, dt, highlight = -1) {
    const rel = Math.min(1, 6 * dt);
    for (let i = 0; i < this.speakerCount; ++i) {
      const pk = peaks ? peaks[i] : 0;
      const db = pk > 1e-6 ? 20 * Math.log10(pk) : -120;
      const lv = db <= -60 ? 0 : db >= 0 ? 1 : 1 + db / 60;
      this.speakerLevel[i] = lv > this.speakerLevel[i]
        ? lv : this.speakerLevel[i] + (lv - this.speakerLevel[i]) * rel;
      const t = this.speakerLevel[i];
      const mat = this.speakers[i].material;
      if (i === highlight) {
        mat.color.copy(SPK_PICK);
        mat.emissive.setRGB(0.10, 0.35, 0.16);
      } else {
        mat.color.copy(SPK_IDLE).lerp(SPK_HOT, t);
        mat.emissive.setRGB(t * 0.45, t * 0.28, 0.0);
      }
      const s = 1 + t * 0.6;
      this.speakers[i].scale.setScalar(s);
    }
  }

  /** @param {number[]} room the listener position, room meters */
  setHead(room, quat) {
    this.headRoom = room;
    roomToWorld(room[0], room[1], room[2], this.head.position);
    if (quat) this.head.quaternion.set(quat[0], quat[1], quat[2], quat[3]);
  }

  /** @param {number[]} room the source position, room meters */
  setSource(room) {
    this.sourceRoom = room;
    roomToWorld(room[0], room[1], room[2], this.source.position);
    const a = this.link.geometry.attributes.position;
    a.array[0] = this.head.position.x; a.array[1] = this.head.position.y; a.array[2] = this.head.position.z;
    a.array[3] = this.source.position.x; a.array[4] = this.source.position.y; a.array[5] = this.source.position.z;
    a.needsUpdate = true;
  }

  setLinkColor(hex) { this.link.material.color.setHex(hex); }
  setSourceColor(hex) { this.source.material.color.setHex(hex); }

  /** Push the current source position onto the trail, or hide it when `on` is false. */
  setTrail(on) {
    this.trail.visible = on;
    if (!on) { this.trailLen = 0; this.trail.geometry.setDrawRange(0, 0); }
  }
  pushTrail() {
    if (!this.trail.visible) return;
    const a = this.trail.geometry.attributes.position;
    if (this.trailLen === this.trailMax) {
      a.array.copyWithin(0, 3);
      this.trailLen--;
    }
    const o = this.trailLen * 3;
    a.array[o] = this.source.position.x;
    a.array[o + 1] = this.source.position.y;
    a.array[o + 2] = this.source.position.z;
    this.trailLen++;
    a.needsUpdate = true;
    this.trail.geometry.setDrawRange(0, this.trailLen);
  }

  /* --------------------------------------------------------- per-scene props */

  clearScene() {
    for (const c of [...this.sceneGroup.children]) {
      this.sceneGroup.remove(c);
      /* traverse, not just the top level: addPanel returns a GROUP, and disposing the group alone
       * would leak its face and its outline on every scene switch. */
      c.traverse((n) => {
        n.geometry?.dispose?.();
        if (Array.isArray(n.material)) n.material.forEach((m) => m.dispose());
        else n.material?.dispose?.();
      });
    }
  }

  /** A translucent quad with a bright outline: the occluding wall, the water surface. */
  addPanel(w, h, color, opacity = 0.35) {
    const g = new THREE.Group();
    const face = new THREE.Mesh(
      new THREE.PlaneGeometry(w, h),
      new THREE.MeshStandardMaterial({ color, transparent: true, opacity, side: THREE.DoubleSide })
    );
    g.add(face);
    const edge = new THREE.LineSegments(
      new THREE.EdgesGeometry(new THREE.PlaneGeometry(w, h)),
      new THREE.LineBasicMaterial({ color })
    );
    g.add(edge);
    this.sceneGroup.add(g);
    return g;
  }

  /** A wireframe box, for a room or a shoebox. */
  addBoxWire(w, h, d, color) {
    const m = new THREE.LineSegments(
      new THREE.EdgesGeometry(new THREE.BoxGeometry(w, h, d)),
      new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.6 })
    );
    this.sceneGroup.add(m);
    return m;
  }

  /** A thin line a scene keeps and moves. */
  addLine(color, points = 2) {
    const g = new THREE.BufferGeometry();
    g.setAttribute("position", new THREE.BufferAttribute(new Float32Array(points * 3), 3));
    const l = new THREE.Line(g, new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.8 }));
    this.sceneGroup.add(l);
    return l;
  }

  /** Move a line built by `addLine`, from room-space points. */
  setLinePoints(line, pts) {
    const a = line.geometry.attributes.position;
    for (let i = 0; i < pts.length; ++i) {
      a.array[i * 3] = pts[i][0];
      a.array[i * 3 + 1] = pts[i][1];
      a.array[i * 3 + 2] = pts[i][2];
    }
    a.needsUpdate = true;
    line.geometry.setDrawRange(0, pts.length);
  }

  addSphere(r, color, opacity = 1) {
    const m = new THREE.Mesh(
      new THREE.SphereGeometry(r, 16, 12),
      new THREE.MeshStandardMaterial({ color, transparent: opacity < 1, opacity })
    );
    this.sceneGroup.add(m);
    return m;
  }

  /* ------------------------------------------------------------ camera + input */

  _bindPointer() {
    const c = this.canvas;
    c.addEventListener("contextmenu", (e) => e.preventDefault());
    c.addEventListener("pointerdown", (e) => {
      c.setPointerCapture(e.pointerId);
      this._ndc(e);
      this._raycaster.setFromCamera(this._pointer, this.camera);
      const hit = this.dragEnabled && e.button === 0 &&
                  this._raycaster.intersectObject(this.source, false).length > 0;
      if (hit) {
        this._drag = { kind: "source" };
        /* A plane through the source, facing the camera: the drag then tracks the pointer exactly
         * for as long as the camera does not move, which is what a direct-manipulation gizmo owes
         * you. */
        this.camera.getWorldDirection(this._v);
        this._dragPlane.setFromNormalAndCoplanarPoint(this._v, this.source.position);
      } else {
        this._drag = { kind: "orbit", x: e.clientX, y: e.clientY };
      }
    });
    c.addEventListener("pointermove", (e) => {
      if (!this._drag) return;
      if (this._drag.kind === "orbit") {
        this.azim -= (e.clientX - this._drag.x) * 0.006;
        this.elev = Math.max(-1.35, Math.min(1.35, this.elev + (e.clientY - this._drag.y) * 0.005));
        this._drag.x = e.clientX;
        this._drag.y = e.clientY;
        return;
      }
      this._ndc(e);
      this._raycaster.setFromCamera(this._pointer, this.camera);
      if (!this._raycaster.ray.intersectPlane(this._dragPlane, this._dragHit)) return;
      const y = Math.max(0.05, this._dragHit.y);
      this.onSourceDrag?.(this._dragHit.x, y, this._dragHit.z);
    });
    const end = (e) => {
      if (this._drag) { try { c.releasePointerCapture(e.pointerId); } catch { /* already gone */ } }
      this._drag = null;
    };
    c.addEventListener("pointerup", end);
    c.addEventListener("pointercancel", end);
    c.addEventListener("wheel", (e) => {
      e.preventDefault();
      this.dist = Math.max(1.5, Math.min(24, this.dist * (1 + Math.sign(e.deltaY) * 0.12)));
    }, { passive: false });
  }

  _ndc(e) {
    const r = this.canvas.getBoundingClientRect();
    this._pointer.x = ((e.clientX - r.left) / r.width) * 2 - 1;
    this._pointer.y = -((e.clientY - r.top) / r.height) * 2 + 1;
  }

  resize() {
    const r = this.canvas.getBoundingClientRect();
    const w = Math.max(1, Math.floor(r.width));
    const h = Math.max(1, Math.floor(r.height));
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  /** The camera's screen-right axis in room space, for `frame.js`'s `screenSideOf`. */
  cameraRight() {
    this.camera.updateMatrixWorld();
    const m = this.camera.matrixWorld.elements;
    return [m[0], m[1], m[2]];
  }

  render() {
    const cy = Math.cos(this.elev);
    roomToWorld(
      this.headRoom[0] + this.dist * cy * Math.sin(this.azim),
      this.headRoom[1] + this.dist * Math.sin(this.elev),
      this.headRoom[2] + this.dist * cy * Math.cos(this.azim),
      this.camera.position
    );
    roomToWorld(this.headRoom[0], this.headRoom[1], this.headRoom[2], this._v);
    this.camera.lookAt(this._v);
    this.renderer.render(this.scene, this.camera);
  }
}
