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
    { kind: "note", text: "Trigger on the panel presses a control. Trigger or grip away from it " +
                          "grabs the source with the nearer hand. Thumbstick X slides it, Y " +
                          "pushes it away, grip plus Y raises it." },
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
  if (h) {
    rows.push(["blocks rendered", h.blocks]);
    rows.push(["late blocks", h.lateBlocks]);
    rows.push(["device lost (host-paced)", h.deviceLost]);
  }
  return rows;
}
