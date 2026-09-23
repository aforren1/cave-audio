/**
 * options.js - the menu's contents, as ui.js control descriptors.
 *
 * ONE LIST, TWO RENDERERS. `playground/ui.js` turns these into DOM for the flat view before the
 * session starts, and `panel.js` paints the same objects onto the in-world panel once it has.
 * Neither renderer knows what the other exists for, and a control added here appears in both.
 *
 * DUPLICATION, named rather than hidden: the scene picker, the render profile, the stimulus and
 * the master gain are re-spelled from `playground/main.js`, which builds them inline inside its
 * `drawControls()` and exports nothing. The PER-SCENE knobs are not duplicated - they come
 * straight from `scene.controls(ctx)`, the same call the flat page makes, so every scene's own
 * options reach XR with no XR code in the scene. If `playground/main.js` ever exports its global
 * list, delete the middle block here and import it. bindings/web/README.md's "XR" section says
 * the same thing where a reader will look for it.
 */
import { Profile } from "../dist/index.js";
import { SCENES } from "../playground/scenes/index.js";

/** The lead a headset starts with. docs/web.md and the XR section of the README explain it. */
export const DEFAULT_LEAD_S = 0.03;

/**
 * The engine block the XR page opens with: ONE Web Audio render quantum. At 256 the worklet sink's
 * fixed-quantum adapter renders a whole 256-frame block inside every OTHER process() call and only
 * copies out in the one between, so the render cost lands in half the callbacks at twice the size.
 * On a mobile SoC running cave_sim (an HRTF convolution per virtual speaker) that spike was the
 * crackle reported from a Galaxy XR (2026-09-23). At 128 every callback renders one block straight
 * into its slot and copies it out: the adapter's pass-through case. The panel lets you A/B it.
 */
export const XR_DEFAULT_BLOCK = 128;
export const BLOCK_CHOICES = [128, 256, 512];
/** AudioContext latencyHint values offered. "interactive" is the default; "playback" asks the
 * browser for a bigger device buffer, trading latency for headroom. */
export const LATENCY_HINTS = ["interactive", "playback"];

/** Web Audio's render quantum: 128 in 1.0; a 1.1 context may report its own. */
export function quantumOf(app) {
  const q = app.rig?.ctx?.renderQuantumSize;
  return Number.isFinite(q) && q > 0 ? q : 128;
}

function pct(x) { return `${Math.round(x * 100)}%`; }

/**
 * The render load in one line, for the in-world panel (the DOM status table is out of sight in a
 * headset). ASCII only: it can reach a console through the log.
 *
 * peakLoad is bwa_health.peak_load: the WORST single block's render time over one BLOCK period,
 * since the engine started. When the block is bigger than the quantum the whole block still
 * renders inside ONE process() call, so the number that matters for that call is the same time
 * over one QUANTUM period: peakLoad * block / quantum.
 */
/** Late blocks over the recent window main.js keeps, or null before there are two samples. */
export function recentLate(app) {
  const hist = app.healthHist;
  if (!hist || hist.length < 2) return null;
  const a = hist[0], b = hist[hist.length - 1];
  return { late: b.late - a.late, blocks: b.blocks - a.blocks, seconds: (b.t - a.t) / 1000 };
}

export function healthLine(app) {
  const i = app.rig.engine?.info;
  const h = app.rig.health;
  if (!i || !h) return "audio health: waiting for the first poll";
  const q = quantumOf(app);
  const ratio = i.blockSize / q;
  const perCall = h.peakLoad * ratio;
  const r = recentLate(app);
  const recent = r ? `${r.late} of ${r.blocks} in the last ${r.seconds.toFixed(0)} s` : "measuring";
  return `Audio: block ${i.blockSize} / quantum ${q} (${ratio >= 1 ? ratio : ratio.toFixed(2)} ` +
         `quanta per block). Late blocks: ${recent}; ${h.lateBlocks} of ${h.blocks} since start. ` +
         `Peak render since start (includes start-up): ${pct(h.peakLoad)} of a block, ` +
         `${pct(perCall)} of one process() call. Latency hint "${app.latencyHint ?? "interactive"}".`;
}

export function profileName(p) {
  return p === Profile.BINAURAL ? "binaural" : p === Profile.CAVE_SIM ? "cave_sim" : String(p);
}

/**
 * The whole option set, in menu order.
 * @param {object} app the page state: `rig`, `scene`, `wantProfile`, `masterGain`, `leadSeconds`,
 *        `selectScene(id)`, `setProfile(p)`, `refresh()`, and `ctx` for the scene's own controls
 * @returns {object[]} ui.js descriptors
 */
export function buildOptions(app) {
  const re = () => app.refresh();
  const items = [
    {
      kind: "select", label: "scene",
      options: SCENES.map((s) => s.name),
      get: () => Math.max(0, SCENES.findIndex((s) => s.id === app.scene?.id)),
      set: (v) => { app.selectScene(SCENES[v].id); },
    },
    {
      kind: "select", label: "render profile",
      options: ["cave_sim (the array, auditioned)", "binaural (direct per-source HRTF)"],
      get: () => (app.wantProfile === Profile.BINAURAL ? 1 : 0),
      set: (v) => { app.setProfile(v === 1 ? Profile.BINAURAL : Profile.CAVE_SIM); },
      hint: "A create-time choice, so switching rebuilds the engine. Your AudioContext survives it.",
    },
    {
      /* A note with a LABEL: the panel's list signature reads the label, so the live text below
       * repaints at 4 Hz without resetting the scroll. ui.js ignores the label on a note. */
      kind: "note", label: "audio health",
      get text() { return healthLine(app); },
    },
    {
      kind: "select", label: "engine block (frames)",
      options: BLOCK_CHOICES.map((b) => (b === XR_DEFAULT_BLOCK ? `${b} (one quantum, default)` : `${b}`)),
      get: () => Math.max(0, BLOCK_CHOICES.indexOf(app.blockSize ?? XR_DEFAULT_BLOCK)),
      set: (v) => { app.setEngineOptions({ blockSize: BLOCK_CHOICES[v] }); },
      hint: "A create-time choice, so switching rebuilds the engine and the AudioContext. " +
            "128 renders one block per Web Audio callback; bigger blocks render a whole block " +
            "inside one callback and nothing in the next.",
    },
    {
      kind: "select", label: "output latency hint",
      options: ["interactive (default)", "playback (bigger device buffer)"],
      get: () => Math.max(0, LATENCY_HINTS.indexOf(app.latencyHint ?? "interactive")),
      set: (v) => { app.setEngineOptions({ latencyHint: LATENCY_HINTS[v] }); },
      hint: "Fixed when an AudioContext is created, so switching builds a new context and a new " +
            "engine. It needs no new gesture: the Enter VR press already unlocked audio for this " +
            "page. Try playback if the audio crackles and the late-block count stays at 0.",
    },
    {
      kind: "select", label: "stimulus", options: app.rig.stimulusNames(),
      get: () => app.rig.signal,
      set: (v) => { app.rig.setSignal(v); re(); },
    },
    {
      kind: "slider", label: "master gain", min: 0, max: 1.5, step: 0.01,
      get: () => app.masterGain ?? 1,
      set: (v) => { app.masterGain = v; app.rig.engine?.setMasterGain(v); re(); },
      format: (v) => `${(20 * Math.log10(Math.max(v, 1e-6))).toFixed(1)} dB`,
    },
    {
      /* The one control the flat playground has no use for. */
      kind: "slider", label: "pose prediction lead", min: 0, max: 0.08, step: 0.005,
      get: () => app.leadSeconds ?? DEFAULT_LEAD_S,
      set: (v) => { app.leadSeconds = v; re(); },
      format: (v) => (v <= 0 ? "off" : `${Math.round(v * 1000)} ms`),
      hint: "WebXR already predicts the pose to DISPLAY time, so this is the extra distance to " +
            "the EARS: output latency plus one block. Too much overshoots on a direction change.",
    },
    {
      /* What the thumbstick does, for hands: a hand has a pinch and nothing else. */
      kind: "buttons", label: "move source (no thumbstick needed)",
      items: ["left", "right", "up", "down"].map((w) => ({ label: w, onClick: () => app.stepSource(w) })),
    },
    {
      kind: "buttons",
      items: ["ahead", "back"].map((w) => ({ label: w, onClick: () => app.stepSource(w) })),
    },
    { kind: "note", text: "Trigger or pinch on the panel presses a control. Trigger, grip or " +
                          "pinch-and-hold away from it grabs the source with the nearer hand and " +
                          "carries it. Thumbstick X slides it, Y pushes it away, grip plus Y " +
                          "raises it; with hands, use the move buttons above." },
  ];
  const own = app.scene && app.ctx ? app.scene.controls(app.ctx) : [];
  return items.concat(own);
}

/** The status rows, for the DOM table and for the panel's own readout page. */
export function statusRows(app) {
  const i = app.rig.engine?.info;
  const h = app.rig.health;
  const x = app.xr;
  const rows = [
    ["mode", x?.presenting ? "immersive-vr" : "flat preview"],
    ["reference space", x?.refSpaceType ?? "-"],
    ["menu", x?.presenting ? (x.domOverlay ? "DOM overlay" : "in-world panel") : "DOM"],
    ["XR frames", x?.frames ?? 0],
    ["pose dropouts", x?.poseLost ?? 0],
    ["prediction lead", `${Math.round((app.leadSeconds ?? 0) * 1000)} ms`],
    ["head speed", `${(x?.lead.speed() ?? 0).toFixed(2)} m/s`],
  ];
  if (i) {
    rows.push(["profile", profileName(app.rig.profile)]);
    rows.push(["backend", i.backend]);
    rows.push(["sample rate / block", `${i.sampleRate} Hz / ${i.blockSize}`]);
    rows.push(["bus channels", `${i.channelCount}`]);
    rows.push(["output latency", `${i.outputLatencyFrames} frames`]);
  }
  const c = app.rig.ctx;
  rows.push(["latency hint", app.latencyHint ?? "interactive"]);
  if (c && Number.isFinite(c.baseLatency)) {
    const out = Number.isFinite(c.outputLatency) ? `${(c.outputLatency * 1000).toFixed(1)} ms` : "-";
    rows.push(["context latency (base / output)", `${(c.baseLatency * 1000).toFixed(1)} ms / ${out}`]);
  }
  if (i) {
    const q = quantumOf(app);
    rows.push(["block / quantum", `${i.blockSize} / ${q} = ${i.blockSize / q}`]);
  }
  if (h) {
    rows.push(["blocks rendered", h.blocks]);
    rows.push(["late blocks (since start)", h.lateBlocks]);
    const r = recentLate(app);
    if (r) rows.push(["late blocks (recent)", `${r.late} of ${r.blocks} in ${r.seconds.toFixed(1)} s`]);
    /* bwa_health.peak_load: worst block render time over one block period, since start, so it
     * includes the start-up blocks. */
    rows.push(["peak load (per block)", pct(h.peakLoad)]);
    if (i) rows.push(["peak load (per process() call)", pct(h.peakLoad * i.blockSize / quantumOf(app))]);
    rows.push(["device lost (host-paced)", h.deviceLost]);
  }
  return rows;
}
