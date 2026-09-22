/**
 * main.js - the playground's wiring: one Start button, one frame loop, one scene at a time.
 *
 * THE FRAME LOOP is the shape bindings/web/README.md prescribes. Every scene writes room-space
 * numbers into `ctx`; this loop then does the staged writes (`setPosition`, `setPose`) and ONE
 * `pushFrame()`, which is one message and one `CMD_COMMIT` however many things moved. That is what
 * makes a visual frame coherent at the mixer (invariant 6), and it is the reason a scene never
 * calls `pushFrame` itself.
 *
 * THE FEED IS NOT ON THIS LOOP. `rig.feed()` paces on the push ring's own space, not on the
 * animation frame, because a background tab gets fewer frames and the audio thread does not slow
 * down with it. It is kicked from here and from an interval, and it is re-entrancy guarded.
 *
 * THE TEST HOOK, `window.__bwaPlayground`, exists because tests/run-playground.mjs has no other
 * way in: a headless browser cannot click a slider and read a cone's color. It exposes the same
 * state the controls write, so the check drives the real scene rather than a test-only path.
 */
import { Profile } from "../dist/index.js";
import { Rig } from "./rig.js";
import { World } from "./world.js";
import { SIGNALS } from "./stimulus.js";
import { renderControls, renderReadout } from "./ui.js";
import { screenSideOf } from "./frame.js";
import { SCENES } from "./scenes/index.js";

const el = (id) => document.getElementById(id);

const app = {
  rig: new Rig(),
  world: null,
  scene: null,
  ctx: null,
  running: false,
  wantProfile: Profile.CAVE_SIM,
  lastT: 0,
  slowT: 0,
  errors: [],
};

function log(msg, bad) {
  const p = document.createElement("div");
  p.textContent = msg;
  if (bad) { p.className = "bad"; app.errors.push(msg); }
  el("log").appendChild(p);
}

/* ------------------------------------------------------------------ isolation */

if (!globalThis.crossOriginIsolated) {
  log("This page is NOT cross-origin isolated yet, so SharedArrayBuffer is unavailable and the " +
      "engine cannot run its two-thread model. The service worker registers itself and reloads " +
      "once on a first visit; if this message survives a reload, service workers are blocked " +
      "here (a private window, for example). There is deliberately no single-threaded fallback.",
      true);
  el("go").disabled = true;
}

/* ------------------------------------------------------------------ the scene ctx */

function makeCtx() {
  const ctx = {
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
    refresh() { drawControls(); },
  };
  return ctx;
}

/* ------------------------------------------------------------------ scene switching */

async function selectScene(id) {
  const next = SCENES.find((s) => s.id === id);
  if (!next) throw new Error(`no scene "${id}"`);
  if (app.scene === next) return;
  if (app.scene) await app.scene.exit(app.ctx);
  app.world.clearScene();
  app.world.setTrail(false);
  app.ctx.highlight = -1;

  /* A scene that reads the array bus needs the array. CAVE_SIM is the one profile that both
   * renders through the 26-channel bus and comes out of headphones, so a bus scene forces it and
   * the page's own choice comes back when you leave. */
  const want = next.needsBus ? Profile.CAVE_SIM : app.wantProfile;
  if (app.rig.profile !== want) {
    await app.rig.setProfile(want);
    app.ctx.srcHandle = app.rig.src.handle;
    app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
    log(`profile -> ${profileName(want)} (engine rebuilt; backend "${app.rig.engine.info.backend}")`);
  }

  app.scene = next;
  await next.enter(app.ctx);
  el("sceneBlurb").textContent = next.blurb;
  el("scenePick").value = id;
  drawControls();
}

function profileName(p) {
  return p === Profile.BINAURAL ? "binaural" : p === Profile.CAVE_SIM ? "cave_sim" : String(p);
}

function drawControls() {
  const global = [
    {
      kind: "select", label: "render profile", options: ["cave_sim (the array, auditioned)", "binaural (direct per-source HRTF)"],
      get: () => (app.wantProfile === Profile.BINAURAL ? 1 : 0),
      set: async (v) => {
        app.wantProfile = v === 1 ? Profile.BINAURAL : Profile.CAVE_SIM;
        const want = app.scene?.needsBus ? Profile.CAVE_SIM : app.wantProfile;
        if (app.rig.profile !== want) {
          await app.rig.setProfile(want);
          app.ctx.srcHandle = app.rig.src.handle;
          app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
          await app.scene?.enter(app.ctx);
          log(`profile -> ${profileName(want)} (engine rebuilt)`);
        }
        drawControls();
      },
      hint: "A create-time choice, so switching rebuilds the engine. Your AudioContext survives it.",
    },
    {
      kind: "select", label: "stimulus", options: SIGNALS.map((s) => s.name),
      get: () => app.rig.signal,
      set: (v) => app.rig.setSignal(v),
    },
    {
      kind: "slider", label: "master gain", min: 0, max: 1.5, step: 0.01,
      get: () => app.masterGain ?? 1,
      set: (v) => { app.masterGain = v; app.rig.engine?.setMasterGain(v); },
      format: (v) => `${(20 * Math.log10(Math.max(v, 1e-6))).toFixed(1)} dB`,
    },
  ];
  renderControls(el("globalControls"), global);
  renderControls(el("sceneControls"), app.scene ? app.scene.controls(app.ctx) : []);
}

/* ------------------------------------------------------------------ the frame loop */

function frame(now) {
  if (!app.running) return;
  const dt = Math.min(0.1, app.lastT ? (now - app.lastT) / 1000 : 0.016);
  app.lastT = now;

  app.ctx.highlight = -1;
  app.scene?.update(app.ctx, dt);

  const [sx, sy, sz] = app.ctx.sourceRoom;
  const [hx, hy, hz] = app.ctx.headRoom;
  const q = app.ctx.headQuat;
  app.rig.src?.setPosition(sx, sy, sz);
  app.rig.engine?.listener.setPose(hx, hy, hz, q[0], q[1], q[2], q[3]);
  app.rig.engine?.pushFrame();               /* ONE message, ONE commit: the frame is coherent */

  app.world.setHead(app.ctx.headRoom, app.ctx.headQuat);
  app.world.setSource(app.ctx.sourceRoom);
  app.world.setSpeakerLevels(app.rig.busLevels, dt, app.ctx.highlight);
  app.scene?.draw?.(app.ctx);
  app.world.render();

  app.rig.feed();
  if (now - app.slowT > 120) {
    app.slowT = now;
    app.rig.poll().then(() => {
      if (!app.running) return;
      renderReadout(el("readout"), app.scene ? app.scene.readout(app.ctx) : []);
      renderReadout(el("health"), healthRows());
      const { peak } = app.rig.loudestChannel();
      const db = 20 * Math.log10(Math.max(peak, 1e-6));
      el("meterFill").style.width = `${Math.max(0, Math.min(100, (db + 60) * (100 / 60)))}%`;
      el("meterText").textContent = peak > 1e-6 ? `${db.toFixed(1)} dBFS bus peak` : "silent";
    });
  }
  requestAnimationFrame(frame);
}

function healthRows() {
  const h = app.rig.health;
  const i = app.rig.engine?.info;
  if (!h || !i) return [];
  return [
    ["backend", i.backend],
    ["profile", profileName(app.rig.profile)],
    ["sample rate / block", `${i.sampleRate} Hz / ${i.blockSize}`],
    ["bus channels", `${i.channelCount}`],
    ["blocks rendered", h.blocks],
    ["late blocks", h.lateBlocks],
    ["peak load", h.peakLoad.toFixed(3)],
    ["device lost (host-paced)", h.deviceLost],
    ["active voices", app.rig.activeVoices],
    ["AudioContext", app.rig.ctx?.state ?? "-"],
  ];
}

/* ------------------------------------------------------------------ start */

let readyResolve;
const ready = new Promise((r) => { readyResolve = r; });

async function start() {
  el("go").disabled = true;
  el("go").textContent = "starting";
  try {
    /* The AudioContext is created HERE, inside the gesture handler, and the page keeps it: the
     * worklet sink adopts it by handle through bwa_desc.device rather than making one of its own,
     * so resuming it stays the page's business. Only a gesture may do that. */
    const ctx = new AudioContext({ latencyHint: "interactive" });
    await ctx.resume();

    app.world = new World(el("view"));
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
      if (app.scene?.state) { app.scene.state.auto = false; app.scene.state.flyby = false; }
    };

    el("startPanel").hidden = true;
    app.world.resize();

    for (const s of SCENES) {
      const o = document.createElement("option");
      o.value = s.id;
      o.textContent = s.name;
      el("scenePick").appendChild(o);
    }
    el("scenePick").addEventListener("change", (e) => { selectScene(e.target.value); });

    app.running = true;
    await selectScene(SCENES[0].id);
    requestAnimationFrame(frame);
    setInterval(() => app.rig.feed(), 30);   /* the ring's own pacing, not the frame rate */
    readyResolve(true);
  } catch (err) {
    log(String(err && err.message ? err.message : err), true);
    el("go").disabled = false;
    el("go").textContent = "Start";
    readyResolve(false);
  }
}

el("go").addEventListener("click", start);

/* ------------------------------------------------------------------ the test hook */

globalThis.__bwaPlayground = {
  ready,
  start,
  scenes: SCENES.map((s) => s.id),
  selectScene,
  /** The live scene's own state object, so a check can set a knob the way a control would. */
  sceneState: () => app.scene?.state ?? null,
  state: () => ({
    sceneId: app.scene?.id ?? null,
    profile: app.rig.profile,
    backend: app.rig.engine?.info.backend ?? null,
    sinkType: app.rig.engine?.info.sinkType ?? null,
    sampleRate: app.rig.engine?.info.sampleRate ?? 0,
    blockSize: app.rig.engine?.info.blockSize ?? 0,
    channelCount: app.rig.engine?.info.channelCount ?? 0,
    speakerCount: app.rig.speakerCount ?? 0,
    sourceRoom: app.ctx?.sourceRoom ?? null,
    headRoom: app.ctx?.headRoom ?? null,
    busLevels: Array.from(app.rig.busLevels.slice(0, app.rig.channelCount)),
    loudest: app.rig.loudestChannel(),
    health: app.rig.health,
    crossOriginIsolated: globalThis.crossOriginIsolated === true,
    errors: app.errors.slice(),
  }),
  /** Switch the page's render profile, which rebuilds the engine. Same path the picker takes. */
  async setProfile(p) {
    app.wantProfile = p;
    const want = app.scene?.needsBus ? Profile.CAVE_SIM : p;
    if (app.rig.profile === want) return app.rig.engine.info;
    const info = await app.rig.setProfile(want);
    app.ctx.srcHandle = app.rig.src.handle;
    app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
    await app.scene?.enter(app.ctx);
    drawControls();
    return info;
  },
  setSourceRoom(p) {
    if (app.scene?.state) { app.scene.state.auto = false; app.scene.state.flyby = false; }
    app.ctx.sourceRoom = [p[0], p[1], p[2]];
  },
  /** -1 when the room point draws LEFT of the head on screen, +1 right. See frame.js. */
  screenSide(p) {
    return screenSideOf(p, app.ctx.headRoom, app.world.cameraRight());
  },
  /** The offline manual-sink probe. Lazy, so the demo does not carry it. */
  async measureLaterality(p) {
    const { measureLaterality } = await import("./probe.js");
    return measureLaterality({ profile: app.rig.profile, source: p, head: app.ctx.headRoom });
  },
};
