/*
 * lifecycle.test.mjs - the idiomatic layer and the control-side dispatcher, end to end, offline.
 *
 * Everything here runs through Host, which is the SAME object the control Worker drives; only the
 * transport differs. So what a browser adds over this is the postMessage hop and the AudioWorklet
 * sink, and neither of those is what an engine bug hides in.
 *
 * On the NULL sink, so it needs no device: a real render thread, real commits, real handles.
 */
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { haveModule, loadModule, loadSrc } from "./helper.mjs";

if (!haveModule) {
  console.log("bw_audio.mjs not built; run tools/wasm/build-web.sh");
  process.exit(77);
}

const SINK_NULL = 2;
const PROFILE_BINAURAL = 1;

let Host, host, M, DEFAULT_GRID;

before(async () => {
  M = await loadModule();
  ({ Host } = await loadSrc("host.js"));
  ({ CONSTANTS: { DEFAULT_GRID } } = await loadSrc("raw.js"));
  host = new Host();
  await host.init({
    moduleFactory: async () => M,
    engine: { profile: PROFILE_BINAURAL, sink: SINK_NULL, blockSize: 256 },
  });
});

after(() => {
  try { host.handle("destroy", {}); } catch { /* already destroyed by a failing test */ }
});

test("create reports the engine's own geometry", () => {
  const i = host.info();
  assert.equal(i.sampleRate, 48000);
  assert.equal(i.blockSize, 256);
  assert.equal(i.channelCount, DEFAULT_GRID);  /* the binaural profile still renders the grid */
  assert.equal(i.sinkType, SINK_NULL);
});

test("start runs the render thread and the dsp clock advances", async () => {
  host.handle("start", {});
  const t0 = host.engine.dspTimeFrames;
  await new Promise((r) => setTimeout(r, 200));
  const t1 = host.engine.dspTimeFrames;
  assert.ok(t1 > t0, `the dsp clock did not advance: ${t0} -> ${t1}`);
  /* A margin, not "greater than zero": 200 ms at 48 kHz is 9600 frames, and a check that both
   * sides satisfy when the render thread is dead is a coin flip (CLAUDE.md's trap). */
  assert.ok(t1 - t0 > 2000, `only ${t1 - t0} frames in 200 ms`);
});

test("health is reported, and says honestly whether it MEANS anything", () => {
  const h = host.handle("health", {});
  assert.ok(h.blocks > 0, "the null sink rendered no blocks");
  assert.equal(typeof h.measured, "boolean");
  assert.equal(h.deviceLost, 0);
  assert.ok(h.peakLoad >= 0);
});

test("handles are generation-gated: a destroyed source is inert, not a crash", () => {
  const h = host.handle("create", { kind: "source" });
  assert.ok(h > 0);
  host.handle("invoke", { name: "source_set_pos", args: [h, 1, 0, 0] });
  host.handle("invoke", { name: "source_destroy", args: [h] });
  /* The stale handle must be DROPPED, not acted on (invariant 5). Nothing to assert but that it
   * neither throws nor takes the module down; a generation-gate regression traps here. */
  host.handle("invoke", { name: "source_set_pos", args: [h, 2, 0, 0] });
  assert.equal(host.handle("invoke", { name: "source_is_playing", args: [h] }), 0);
});

test("autocommit is on by default and frame() defers to one commit", () => {
  const e = host.engine;
  assert.equal(e.autocommit, true);
  const s = e.createSource();
  s.setPosition(1, 0, 0);
  assert.equal(e.pending, false, "an autocommitted write should leave nothing pending");
  e.frame(() => {
    s.setPosition(0, 0, 1);
    assert.equal(e.pending, true, "inside frame() the write must stay pending");
  });
  assert.equal(e.pending, false, "frame() must commit at its exit");
  s.destroy();
});

test("a push source accepts frames and reports its remaining space", () => {
  const e = host.engine;
  const p = e.createPushSource();
  const before = p.space;
  assert.ok(before > 0, "a fresh push source should have ring space");
  const n = Math.min(1024, before);
  const buf = new Float32Array(n).fill(0.25);
  const took = p.push(buf);
  assert.equal(took, n, `push took ${took} of ${n}`);
  assert.ok(p.space <= before - n + 256, "space did not fall after a push");
  p.destroy();
});

test("the ABI escape hatch reaches a call the idiomatic layer does not wrap", () => {
  const e = host.engine;
  /* bwa_set_spcap_focus is nowhere in engine.js, on purpose: the idiomatic layer stays small and
   * `invoke` carries the other 150-odd calls. */
  e.invoke("set_panner", 1);
  e.invoke("set_spcap_focus", 12.0, 0.0);
  assert.equal(typeof e.invoke("get_active_voices"), "number");
  assert.throws(() => e.invoke("no_such_call"), /no call named/);
});

test("stop then destroy leaves nothing running", () => {
  host.handle("stop", {});
  host.handle("destroy", {});
  assert.equal(host.engine.ptr, 0);
});
