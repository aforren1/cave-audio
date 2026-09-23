/**
 * main.js - the XR page's wiring: one gesture, one frame loop, one scene at a time.
 *
 * WHAT THIS PAGE ADDS OVER THE FLAT PLAYGROUND, and the only reason it exists: THE HEAD TRACKS.
 * In an immersive-vr session every XR animation frame's viewer pose becomes the engine's listener
 * pose, position and orientation both, so the binaural render follows the head. That is the whole
 * claim of head-tracked binaural and it is the one thing headphones plus a mouse cannot show you:
 * on the flat page a source at room +x is louder in your left ear and stays there however you
 * turn, because there is nothing to turn.
 *
 * THE AUDIO IS THE PLAYGROUND'S, unchanged. `playground/rig.js` owns the engine, the worklet
 * sink, the push source and the stimulus; `playground/scenes/` owns the scenes; `playground/
 * world.js` owns every gizmo. This page adds the XR seam, the session, the controllers and a menu
 * that works at arm's length, and nothing else. Anything that looks like it belongs to the demo
 * rather than to XR should be imported, not rewritten.
 *
 * THE FRAME LOOP is still the shape bindings/web/README.md prescribes: everything writes room
 * numbers into `ctx`, then ONE `setPose` plus `setPosition` plus `pushFrame()`, which is one
 * message and one CMD_COMMIT however many things moved. In session that loop is driven by the XR
 * animation callback, at the headset's rate, which is exactly where a head pose should be sampled.
 *
 * THE TEST HOOK, `window.__bwaXr`, is the sibling of `window.__bwaPlayground`. See
 * tests/run-xr.mjs: without a headset the check installs a fake `navigator.xr`, so the pose path,
 * the seam, the menu and the controllers all run through this file for real.
 */
import { Profile } from "../dist/index.js";
import { Rig } from "../playground/rig.js";
import { SCENES } from "../playground/scenes/index.js";
import { renderControls, renderReadout } from "../playground/ui.js";
import { XrWorld } from "./world_xr.js";
import { MenuPanel } from "./panel.js";
import { Hands } from "./hands.js";
import { XrRuntime } from "./session.js";
import { buildOptions, statusRows, profileName, DEFAULT_LEAD_S } from "./options.js";
import { qrot, earSideOf } from "./frame_xr.js";

const el = (id) => document.getElementById(id);

const app = {
  rig: new Rig(),
  world: null,
  panel: null,
  hands: null,
  xr: null,
  scene: null,
  ctx: null,
  running: false,
  wantProfile: Profile.CAVE_SIM,
  masterGain: 1,
  leadSeconds: DEFAULT_LEAD_S,
  lastT: 0,
  slowT: 0,
  errors: [],
  xrSupported: false,
};

function log(msg, bad) {
  const p = document.createElement("div");
  p.textContent = msg;
  if (bad) { p.className = "bad"; app.errors.push(msg); }
  el("log").appendChild(p);
}

/* ------------------------------------------------------------------ the three states */

if (!globalThis.crossOriginIsolated) {
  log("This page is NOT cross-origin isolated yet, so SharedArrayBuffer is unavailable and the " +
      "engine cannot run its two-thread model. The service worker registers itself and reloads " +
      "once on a first visit; if this message survives a reload, service workers are blocked " +
      "here (a private window, for example). There is deliberately no single-threaded fallback.",
      true);
  el("enter").disabled = true;
  el("flat").disabled = true;
}

const supportReady = XrRuntime.supported().then((yes) => {
  app.xrSupported = yes;
  if (yes) {
    el("xrState").textContent =
      "This browser reports an immersive-vr device. Put the headset on, then press Enter VR.";
  } else {
    el("xrState").textContent =
      "No immersive-vr device here. WebXR is either missing or has no headset attached, so the " +
      "head cannot track and this page has nothing the flat playground does not already do.";
    el("noXr").hidden = false;
    el("enter").disabled = true;
  }
  return yes;
});

/* ------------------------------------------------------------------ the scene ctx */

function makeCtx() {
  return {
    rig: app.rig,
    world: app.world,
    srcHandle: app.rig.src.handle,
    sourceRoom: [0, 1.5, 2],
    headRoom: [0, 1.5, 0],
    headQuat: [0, 0, 0, 1],
    highlight: -1,
    /** Fire an engine call and forget it. Ordering is preserved; failures land in the log once. */
    set(name, ...args) {
      app.rig.engine?.invoke(name, ...args).catch((e) => this._once(name, e));
      return undefined;
    },
    setBuf(name, ...args) {
      return app.rig.engine?.invokeBuf(name, ...args).catch((e) => this._once(name, e));
    },
    _seen: new Set(),
    _once(name, e) {
      if (this._seen.has(name)) return;
      this._seen.add(name);
      log(`bwa_${name}: ${e.message}`, true);
    },
    refresh() { refreshMenu(); },
  };
}

/* ------------------------------------------------------------------ menu + scenes */

function refreshMenu() {
  if (!app.ctx) return;
  const items = buildOptions(app);
  /* The DOM menu is the pre-session one, and it is also what a runtime that really grants
   * dom-overlay would show inside the session. Both renderers take the same descriptor list. */
  renderControls(el("menu"), items);
  app.panel?.setItems(app.scene ? app.scene.name : "bw_audio XR", items);
}

async function selectScene(id) {
  const next = SCENES.find((s) => s.id === id);
  if (!next) throw new Error(`no scene "${id}"`);
  if (app.scene === next) return;
  if (app.scene) await app.scene.exit(app.ctx);
  app.world.clearScene();
  app.world.setTrail(false);
  app.ctx.highlight = -1;

  /* A scene that reads the array bus needs the array, so it forces CAVE_SIM for itself and the
   * page's own choice comes back when you leave. Same rule as the flat playground. */
  const want = next.needsBus ? Profile.CAVE_SIM : app.wantProfile;
  if (app.rig.profile !== want) {
    await app.rig.setProfile(want);
    app.ctx.srcHandle = app.rig.src.handle;
    app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
    log(`profile -> ${profileName(want)} (engine rebuilt)`);
  }

  app.scene = next;
  await next.enter(app.ctx);
  el("sceneBlurb").textContent = next.blurb;
  refreshMenu();
}

async function setProfile(p) {
  app.wantProfile = p;
  const want = app.scene?.needsBus ? Profile.CAVE_SIM : p;
  if (app.rig.profile === want) { refreshMenu(); return app.rig.engine?.info; }
  const info = await app.rig.setProfile(want);
  app.ctx.srcHandle = app.rig.src.handle;
  app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
  await app.scene?.enter(app.ctx);
  await app.rig.engine.setMasterGain(app.masterGain);
  log(`profile -> ${profileName(want)} (engine rebuilt)`);
  refreshMenu();
  return info;
}

app.selectScene = selectScene;
app.setProfile = setProfile;
app.refresh = refreshMenu;

/* ------------------------------------------------------------------ the frame */

/**
 * One visual frame. `frame` and `runtime` are present in session and null in the flat preview, so
 * there is ONE body and the two states cannot drift apart.
 */
function step(nowMs, frame, runtime) {
  if (!app.running) return;
  const dt = Math.min(0.1, app.lastT ? (nowMs - app.lastT) / 1000 : 0.016);
  app.lastT = nowMs;

  /* ---- the head ---- */
  if (frame && runtime) {
    const pose = runtime.viewerPose(frame, nowMs, app.leadSeconds);
    if (pose) {
      app.ctx.headRoom = pose.p;
      app.ctx.headQuat = pose.q;
    }
  }

  /* ---- the scene, then the hands, because a hand on the source overrides the scene's idea of
   * where it should be. The flat page's pointer drag has the same precedence. ---- */
  app.ctx.highlight = -1;
  app.scene?.update(app.ctx, dt);
  if (frame && runtime && app.hands) {
    app.hands.update(frame, runtime.refSpace, app.ctx, dt, app.world, onSourceMoved);
  }

  /* ---- ONE message, ONE commit: the frame is coherent at the mixer (invariant 6) ---- */
  const [sx, sy, sz] = app.ctx.sourceRoom;
  const [hx, hy, hz] = app.ctx.headRoom;
  const q = app.ctx.headQuat;
  app.rig.src?.setPosition(sx, sy, sz);
  app.rig.engine?.listener.setPose(hx, hy, hz, q[0], q[1], q[2], q[3]);
  app.rig.engine?.pushFrame();

  /* ---- the picture ---- */
  app.world.setHead(app.ctx.headRoom, app.ctx.headQuat);
  app.world.setSource(app.ctx.sourceRoom);
  app.world.setSpeakerLevels(busPeaks(), dt, app.ctx.highlight);
  app.scene?.draw?.(app.ctx);
  if (app.panel) {
    app.panel.visible = !!runtime?.presenting && !runtime.domOverlay;
    if (app.panel.visible) {
      app.panel.follow(app.ctx.headRoom, app.ctx.headQuat, dt, qrot, app.hands?.gazeOnly() ?? false);
      app.panel.draw();
    }
  }
  app.world.render();

  app.rig.feed();
  if (nowMs - app.slowT > 250) {
    app.slowT = nowMs;
    app.panel?.refresh();          /* a scene's own state moves under the menu; repaint at 4 Hz */
    app.rig.poll().then(() => {
      if (!app.running) return;
      renderReadout(el("readout"), app.scene ? app.scene.readout(app.ctx) : []);
      renderReadout(el("status"), statusRows(app));
      const { peak } = app.rig.loudestChannel();
      const db = 20 * Math.log10(Math.max(peak, 1e-6));
      el("meterFill").style.width = `${Math.max(0, Math.min(100, (db + 60) * (100 / 60)))}%`;
      el("meterText").textContent = peak > 1e-6 ? `${db.toFixed(1)} dBFS bus peak` : "silent";
    });
  }

  /* In session the XR runtime owns the loop; out of it, this page does. The test is `app.xr` and
   * not the `runtime` argument: the flat loop is still in flight while `enter()` is awaiting, and
   * without this it would keep rescheduling itself alongside the session's loop, so every frame
   * would push twice. */
  if (!app.xr?.presenting) requestAnimationFrame((t) => step(t, null, null));
}

/**
 * The per-channel bus meter for this frame.
 *
 * `playground/rig.js` owns the meter and its shape is not this page's to fix: it publishes the
 * peak HELD since the last read (`takeBusPeaks`), sampled on an interval faster than an engine
 * block, because `bwa_get_bus_levels` reports the last block's peak and a reader slower than a
 * block misses most of them. This wrapper exists only so the XR page still works against the
 * older field-named shape while the two pages are edited in parallel; delete it once the flat
 * playground's API has settled.
 */
function busPeaks() {
  const r = app.rig;
  if (typeof r.takeBusPeaks === "function") return r.takeBusPeaks();
  return r.lastPeaks ?? r.busLevels ?? null;
}

function onSourceMoved() {
  if (app.scene?.state) { app.scene.state.auto = false; app.scene.state.flyby = false; }
}

/* ------------------------------------------------------------------ start */

let readyResolve;
const ready = new Promise((r) => { readyResolve = r; });

async function startEngine() {
  if (app.running) return true;
  /* The AudioContext is created HERE, inside the gesture handler, and the page keeps it. The
   * Enter VR click IS that gesture, which is the whole reason the engine starts from this button
   * and not on load: a browser resumes an AudioContext only from a user gesture. */
  const ctx = new AudioContext({ latencyHint: "interactive" });
  await ctx.resume();

  app.world = new XrWorld(el("view"));
  app.panel = new MenuPanel(app.world.scene);
  app.hands = new Hands(app.panel);
  globalThis.addEventListener("resize", () => app.world.resize());

  const info = await app.rig.open({ audioContext: ctx, profile: app.wantProfile });
  log(`engine up: ${info.sampleRate} Hz, block ${info.blockSize}, ${info.channelCount} bus ` +
      `channels, backend "${info.backend}"`);
  if (info.lastError) log(`open note: ${info.lastError}`);

  app.ctx = makeCtx();
  app.ctx.world = app.world;
  app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
  app.world.onSourceDrag = (x, y, z) => {
    app.ctx.sourceRoom = [x, y, z];
    onSourceMoved();
  };

  el("startPanel").hidden = true;
  app.world.resize();

  app.running = true;
  await selectScene(SCENES[0].id);
  requestAnimationFrame((t) => step(t, null, null));
  setInterval(() => app.rig.feed(), 20);     /* the ring's own pacing, not the frame rate */
  /* FASTER THAN A BLOCK, because bwa_get_bus_levels publishes the last BLOCK's peak and anything
   * slower reads one block in N. playground/rig.js carries the measurement behind the number. */
  if (typeof app.rig.meterTick === "function") {
    setInterval(() => app.rig.meterTick()?.catch?.((e) => {
      /* Once, not four hundred times a second. The meter is a readout and a page that cannot
       * draw one still plays, so this is a note rather than an engine error. A stale dist/ next
       * to a newer playground/rig.js lands here. */
      if (app.meterWarned) return;
      app.meterWarned = true;
      log(`the bus meter is not reading (${e.message})`);
    }), 4);
  }
  readyResolve(true);
  return true;
}

async function startFlat() {
  el("flat").disabled = true;
  el("enter").disabled = true;
  try {
    await startEngine();
    el("enter").disabled = !app.xrSupported;
  } catch (err) {
    log(String(err && err.message ? err.message : err), true);
    el("flat").disabled = false;
    readyResolve(false);
  }
}

async function enterVr() {
  el("enter").disabled = true;
  el("flat").disabled = true;
  try {
    await startEngine();
    const xr = new XrRuntime();
    app.xr = xr;
    await xr.enter({
      renderer: app.world.renderer,
      domOverlayRoot: el("overlay"),
      onFrame: (t, frame, rt) => step(t, frame, rt),
      onEnd: () => {
        app.hands?.detach();
        app.world.endPresenting();
        app.panel.visible = false;
        app.ctx.headRoom = [0, 1.5, 0];
        app.ctx.headQuat = [0, 0, 0, 1];
        app.lastT = 0;
        log("the XR session ended; back to the flat preview");
        el("enter").disabled = false;
        requestAnimationFrame((t) => step(t, null, null));
        refreshMenu();
      },
    });
    for (const n of xr.notes) log(n);
    if (xr.renderReady) app.world.beginPresenting();
    app.hands.attach(xr.session);
    app.panel.visible = !xr.domOverlay;
    log(`immersive-vr session up: "${xr.refSpaceType}" reference space, ` +
        `${xr.domOverlay ? "DOM overlay" : "in-world"} menu, ` +
        `${xr.renderReady ? "renderer bound" : "no renderer"}`);
    refreshMenu();
  } catch (err) {
    log(`Enter VR: ${String(err && err.message ? err.message : err)}`, true);
    el("enter").disabled = false;
  }
}

el("enter").addEventListener("click", enterVr);
el("flat").addEventListener("click", startFlat);

/* ------------------------------------------------------------------ the test hook */

globalThis.__bwaXr = {
  ready,
  supportReady,
  startFlat,
  enterVr,
  scenes: SCENES.map((s) => s.id),
  selectScene,
  setProfile,
  sceneState: () => app.scene?.state ?? null,
  /** The live descriptor list, flattened, so a check can compare it with the flat page's. */
  menuItems: () => (app.ctx ? buildOptions(app) : []).map((it) => ({
    kind: it.kind,
    label: it.label ?? null,
    options: it.options ?? null,
    value: it.kind === "note" || it.kind === "buttons" ? null : it.get(),
  })),
  menu: () => app.panel?.describe() ?? null,
  /** The room point a canvas pixel of the in-world panel occupies, for aiming a controller. */
  menuPointAt: (x, y) => app.panel?.pointAt(x, y) ?? null,
  hands: () => app.hands?.report() ?? null,
  /** What the scene draws for hand `i` (grip, ray, reticle), so a gaze input can be checked. */
  handVisual: (i) => app.world?.handVisual(i) ?? null,
  /**
   * The pose the PAGE'S OWN engine is rendering with, read back through `bwa_get_listener_pose`.
   * Everything else here reports what the page computed; this reports what the engine received,
   * which is the one link a probe on a second engine cannot close.
   */
  async listenerPose() {
    const r = await app.rig.engine.invokeBuf("get_listener_pose",
                                             { out: "f32", len: 3 }, { out: "f32", len: 4 });
    return { p: Array.from(r.out[0]), q: Array.from(r.out[1]) };
  },
  setSourceRoom(p) { onSourceMoved(); app.ctx.sourceRoom = [p[0], p[1], p[2]]; },
  setLeadSeconds(s) { app.leadSeconds = s; refreshMenu(); },
  state: () => ({
    xrSupported: app.xrSupported,
    presenting: !!app.xr?.presenting,
    renderReady: !!app.xr?.renderReady,
    refSpaceType: app.xr?.refSpaceType ?? null,
    xrFrames: app.xr?.frames ?? 0,
    poseLost: app.xr?.poseLost ?? 0,
    headSpeed: app.xr?.lead.speed() ?? 0,
    leadSeconds: app.leadSeconds,
    sceneId: app.scene?.id ?? null,
    profile: app.rig.profile,
    backend: app.rig.engine?.info.backend ?? null,
    sinkType: app.rig.engine?.info.sinkType ?? null,
    sampleRate: app.rig.engine?.info.sampleRate ?? 0,
    blockSize: app.rig.engine?.info.blockSize ?? 0,
    channelCount: app.rig.engine?.info.channelCount ?? 0,
    speakerCount: app.rig.speakerCount ?? 0,
    headRoom: app.ctx?.headRoom ?? null,
    headQuat: app.ctx?.headQuat ?? null,
    sourceRoom: app.ctx?.sourceRoom ?? null,
    /** Where the source sits relative to the HEAD's own axes: +1 left ear, -1 right ear. */
    earSide: app.ctx ? earSideOf(app.ctx.sourceRoom, app.ctx.headRoom, app.ctx.headQuat) : 0,
    busLevels: Array.from((app.rig.lastPeaks ?? app.rig.busLevels ?? new Float32Array(0))
                          .slice(0, app.rig.channelCount)),
    loudest: app.rig.loudestChannel(),
    health: app.rig.health,
    crossOriginIsolated: globalThis.crossOriginIsolated === true,
    errors: app.errors.slice(),
  }),
  /**
   * The offline manual-sink probe, at a head POSE of the caller's choosing. Lazy, so the page
   * never carries it. `head` and `quat` default to the page's live listener pose, which is how a
   * check asserts that what the page is sending really lands where it says.
   */
  async measureLaterality(source, head, quat) {
    const { measureLateralityOriented } = await import("./probe.js");
    return measureLateralityOriented({
      profile: app.rig.profile,
      source,
      head: head ?? app.ctx.headRoom,
      headQuat: quat ?? app.ctx.headQuat,
    });
  },
};
