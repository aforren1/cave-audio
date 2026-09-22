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
 * down with it. It is kicked from here and from an interval, and it is re-entrancy guarded. What
 * it does NOT do is fill the ring: the queue is a latency budget (rig.js's QUEUE_MS), because
 * everything already queued has to play before a stimulus change can be heard.
 *
 * THE METERS ARE NOT ON THIS LOOP EITHER, and for the opposite reason: `bwa_get_bus_levels` is the
 * LAST BLOCK's peak, a block is 5.3 ms, and an animation frame is three of them. Sampling on the
 * frame loop misses most blocks, which for a click train means missing all of them. The tick is an
 * interval faster than a block; this loop consumes the peak it held.
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
    afterRebuild();
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

/**
 * Everything the page has to redo after `rig.rebuild` - a profile switch or a layout load.
 *
 * A rebuild is a NEW ENGINE and a new AudioContext, so every handle the page was holding is stale:
 * the push source's, the speaker positions, the scene's per-source knobs, and the master gain,
 * which is engine state and comes back at 1. Doing it in one place is what keeps the two callers
 * (the profile picker and the layout upload) from drifting apart.
 */
function afterRebuild() {
  app.ctx.srcHandle = app.rig.src.handle;
  app.world.setSpeakers(app.rig.speakers, app.rig.speakerCount);
  if (app.masterGain !== undefined) app.rig.engine.setMasterGain(app.masterGain);
  drawStatus();
}

/** The one-line state readout the Start text is replaced by: profile, sink, context, layout. */
function drawStatus() {
  const i = app.rig.engine?.info;
  if (!i) return;
  const bits = [
    profileName(app.rig.profile),
    `sink ${i.backend}`,
    `context ${app.rig.ctx?.state ?? "-"}`,
    `${app.rig.speakerCount} speakers`,
    app.rig.layoutName ? `layout ${app.rig.layoutName}` : "default grid",
  ];
  /* Said next to the cones, not only in the panel: in this profile they are dark because the
   * voices never reach the bus, and a dark array with no explanation reads as a broken meter. */
  if (app.rig.profile === Profile.BINAURAL) bits.push("cones dark: point voices bypass the bus");
  const s = el("status");
  s.textContent = bits.join("  |  ");
  s.hidden = false;
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
          afterRebuild();
          await app.scene?.enter(app.ctx);
          log(`profile -> ${profileName(want)} (engine rebuilt on a fresh AudioContext)`);
        }
        drawControls();
      },
      hint: "A create-time choice, so switching rebuilds the engine, and with it the AudioContext.",
    },
    {
      kind: "file", label: "speaker layout (cave_layout.json)", accept: ".json,application/json",
      set: (text, name) => loadLayout(text, name),
      hint: "Rebuilds the array on the surveyed geometry in the file, and the cones follow " +
            "bwa_get_speakers. Leave it alone for the default 26-speaker grid.",
    },
    ...(app.rig.layoutName
      ? [{
          kind: "buttons", label: "",
          items: [{ label: "back to the default grid", onClick: () => loadLayout(null, null) }],
        }]
      : []),
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

/* ------------------------------------------------------------------ the layout upload */

/**
 * Check an uploaded layout against the parts of the loader contract a page can check itself
 * (docs/layout-schema.md, "Validation"), and say which one failed.
 *
 * It is a PRE-check, not the authority. The engine reads the file properly and rejects far more
 * than this (gain and delay ranges, the EQ caps), and its refusal comes back through
 * `bwa_last_error`. What this buys is a readable message for the three mistakes anyone actually
 * makes - the wrong file, a count outside 4..26, a position that is not a number - without a
 * rebuild in between.
 * @param {string} text the file's contents
 * @returns {string|null} the problem, or null when it is worth handing to the engine
 */
function validateLayout(text) {
  let doc;
  try { doc = JSON.parse(text); } catch (e) { return `not JSON (${e.message})`; }
  const sp = doc && doc.speakers;
  if (!Array.isArray(sp)) return "no \"speakers\" array; this is not a cave_layout.json";
  if (sp.length < 4 || sp.length > 26) {
    return `${sp.length} speakers; the engine takes 4 to 26 (26 = BWA_CHANNELS)`;
  }
  const seen = new Set();
  for (let i = 0; i < sp.length; ++i) {
    const s = sp[i] || {};
    const p = s.position;
    if (!Array.isArray(p) || p.length < 3) return `speaker ${i} has no [x, y, z] position`;
    for (const v of p.slice(0, 3)) {
      if (typeof v !== "number" || !Number.isFinite(v)) return `speaker ${i} has a non-finite position`;
      if (Math.abs(v) > 1000) return `speaker ${i} is ${v} m from the origin; the limit is 1000 m`;
    }
    const idx = s.index;
    if (!Number.isInteger(idx) || idx < 0 || idx >= sp.length) {
      return `speaker ${i} has index ${idx}; the indices must be a permutation of 0..${sp.length - 1}`;
    }
    if (seen.has(idx)) return `index ${idx} is used twice`;
    seen.add(idx);
  }
  return null;
}

/**
 * Rebuild the array on an uploaded layout, or on the default grid when `text` is null.
 * @param {string|null} text
 * @param {string|null} name the file name, for the status line
 */
async function loadLayout(text, name) {
  let bytes = null;
  if (text !== null) {
    const problem = validateLayout(text);
    if (problem) { log(`layout "${name}": ${problem}`, true); return false; }
    bytes = new TextEncoder().encode(text);
  }
  try {
    await app.rig.setLayout(bytes);
    app.rig.layoutName = name;
    afterRebuild();
    await app.scene?.enter(app.ctx);
    log(`layout -> ${name ?? "the default grid"}: ${app.rig.speakerCount} speakers, engine rebuilt`);
  } catch (e) {
    /* The engine is the authority, and it refuses a bad layout in TWO steps (CLAUDE.md): create
     * stays usable on the default grid, and START refuses with BWA_ERR_LAYOUT carrying the reason.
     * That throw lands here, and the page has to SAY it rather than sit silent - then go back to a
     * layout that does start, or there is nothing to listen to. */
    log(`the engine refused that layout: ${e.message}`, true);
    app.rig.layoutName = null;
    await app.rig.setLayout(null);
    afterRebuild();
    await app.scene?.enter(app.ctx);
    drawControls();
    return false;
  }
  drawControls();
  return true;
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
  /* The PEAK HELD since the last frame, not the last block's: the meter tick samples faster than
   * a block (rig.js says why) and this is where the hold is consumed. */
  app.world.setSpeakerLevels(app.rig.takeBusPeaks(), dt, app.ctx.highlight);
  app.scene?.draw?.(app.ctx);
  app.world.render();

  app.rig.feed();
  drawMeters();
  if (now - app.slowT > 120) {
    app.slowT = now;
    app.rig.poll().then(() => {
      if (!app.running) return;
      renderReadout(el("readout"), app.scene ? app.scene.readout(app.ctx) : []);
      renderReadout(el("health"), healthRows());
      drawStatus();
    });
  }
  requestAnimationFrame(frame);
}

/** A 60 dB window, the same one the speaker cones use. */
function meterWidth(linear) {
  const d = 20 * Math.log10(Math.max(linear, 1e-6));
  return `${Math.max(0, Math.min(100, (d + 60) * (100 / 60)))}%`;
}

/**
 * The three strips: the loudest ARRAY bus channel, and the two channels the AudioContext is
 * actually playing.
 *
 * Both meters are here because the two profiles put the audio in different places, and a meter
 * that reads silence while the headphones are loud is worse than no meter. In CAVE_SIM every point
 * source pans into the bus, so the bus strip and the cones are the array's own output. In BINAURAL
 * point voices bypass the bus entirely, so the bus strip reads what is left there (the diffuse
 * field and any test signal) and the caption says so - the output strips are the ones with your
 * ears' content in them.
 */
function drawMeters() {
  const out = app.rig.takeOutLevels();
  app.outHold = {
    l: Math.max(out.l, (app.outHold?.l ?? 0) * 0.86),      /* a gentle fall, so a click is seen */
    r: Math.max(out.r, (app.outHold?.r ?? 0) * 0.86),
  };
  const { channel, peak } = app.rig.loudestChannel();
  el("meterFill").style.width = meterWidth(peak);
  el("outL").style.width = meterWidth(app.outHold.l);
  el("outR").style.width = meterWidth(app.outHold.r);
  const bus = channel < 0 || peak <= 1e-6
    ? "bus silent"
    : `bus ch ${channel} at ${(20 * Math.log10(peak)).toFixed(1)} dBFS`;
  const ears = `out L ${dbText(app.outHold.l)} R ${dbText(app.outHold.r)}`;
  el("meterText").textContent = app.rig.profile === Profile.BINAURAL
    ? `${bus} - binaural renders point voices straight to the ears, so only the diffuse field ` +
      `reaches the 26 bus channels and the cones show that. ${ears}`
    : `${bus}. ${ears}`;
}

function dbText(v) {
  return v > 1e-6 ? `${(20 * Math.log10(v)).toFixed(1)}` : "-inf";
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
    ["queued ahead", `${((app.rig.queued ?? 0) / i.sampleRate * 1000).toFixed(0)} ms`],
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

    /* The start affordance is REPLACED, not left under the scene: the panel is hidden (see the
     * `#startPanel[hidden]` rule in index.html, which the id selector would otherwise beat) and
     * the status line takes its corner. */
    el("startPanel").hidden = true;
    drawStatus();
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
    setInterval(() => app.rig.feed(), 20);   /* the ring's own pacing, not the frame rate */
    /* FASTER THAN A BLOCK (5.3 ms at 48 kHz / 256), because bwa_get_bus_levels publishes the last
     * BLOCK's peak and anything slower reads one block in N and misses the rest - which is how a
     * plainly audible click train read as "silent". rig.meterTick() holds the peak between reads. */
    setInterval(() => app.rig.meterTick(), 4);
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
    busLevels: Array.from(app.rig.lastPeaks.slice(0, app.rig.channelCount)),
    loudest: app.rig.loudestChannel(),
    health: app.rig.health,
    crossOriginIsolated: globalThis.crossOriginIsolated === true,
    errors: app.errors.slice(),
    /* What the page is SHOWING, so a check can assert the visualization and not only the numbers
     * behind it: the cone shading, the meter strips' widths, the start panel and the status line.
     * A headless browser cannot look at a cone, and a meter that stopped following the engine is
     * exactly the defect this reports. */
    view: {
      coneLevels: Array.from(app.world?.speakerLevel?.slice(0, app.rig.speakerCount) ?? []),
      meterWidth: el("meterFill").style.width,
      outWidth: [el("outL").style.width, el("outR").style.width],
      meterText: el("meterText").textContent,
      outPeak: app.outHold ?? { l: 0, r: 0 },
      startPanelShown: getComputedStyle(el("startPanel")).display !== "none",
      status: el("status").hidden ? null : el("status").textContent,
    },
    layout: { name: app.rig.layoutName, speakers: Array.from(app.rig.speakers.slice(0, app.rig.speakerCount * 3)) },
    queuedFrames: app.rig.queued ?? 0,
  }),
  /** The stimulus picker, by index into stimulus.js's SIGNALS. */
  setSignal(i) { app.rig.setSignal(i); },
  /** Upload a layout the way the file input does. `null` goes back to the default grid. */
  loadLayout(text, name) { return loadLayout(text, name); },
  /** Switch the page's render profile, which rebuilds the engine. Same path the picker takes. */
  async setProfile(p) {
    app.wantProfile = p;
    const want = app.scene?.needsBus ? Profile.CAVE_SIM : p;
    if (app.rig.profile === want) return app.rig.engine.info;
    const info = await app.rig.setProfile(want);
    afterRebuild();
    await app.scene?.enter(app.ctx);
    drawControls();
    return info;
  },
  /**
   * The LIVE stereo the AudioContext is playing, peak per channel since the last call, read from
   * the AnalyserNode pair on the sink's own node (rig.js's tap). This is the only place in the
   * suite that measures the audible path rather than a second engine on the manual sink, which is
   * what a "the rebuild came back silent" check needs: the offline probe renders the same content
   * whether or not anything reached the speakers.
   */
  outputPeaks() { return app.rig.takeOutProbe(); },
  /** True once the analyser tap is attached; a check should wait for it before measuring. */
  tapReady() { return app.rig.attachTap(); },
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
