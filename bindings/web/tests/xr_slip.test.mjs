/**
 * xr_slip.test.mjs - the audio-clock slip (bindings/web/xr/slip.js), checked under node.
 *
 * The slip is the XR page's second dropout signal (playbackStats is the first, where the browser
 * has it) and its only one elsewhere, so its SIGN and its window are what matter: an
 * audio clock that runs slow must read POSITIVE, by the amount it ran slow, over the last window
 * only, and a suspended context must say so instead of reading 100 %. A fake context stands in
 * for the AudioContext; the module only reads `state` and `currentTime`.
 *
 * Each assertion was broken once on purpose (the sign flipped, the window trim removed, the
 * suspended branch removed, the reset on a new context removed) and went red first.
 */
import test from "node:test";
import assert from "node:assert/strict";
import { ClockSlip } from "../xr/slip.js";

/** Drive `seconds` of 60 Hz frames, the audio clock advancing at `rate` times the wall. */
function drive(slip, ctx, clock, seconds, rate, frameMs = 1000 / 60) {
  const n = Math.round((seconds * 1000) / frameMs);
  for (let i = 0; i < n; ++i) {
    clock.wall += frameMs;
    ctx.currentTime += (rate * frameMs) / 1000;
    slip.sample(clock.wall, ctx);
  }
}

test("a healthy audio clock reads no slip", () => {
  const slip = new ClockSlip();
  const ctx = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, ctx, clock, 6, 1.0);
  const r = slip.reading();
  assert.equal(r.state, "running");
  assert.ok(Math.abs(r.slipMs) < 1e-6, `slip ${r.slipMs} ms`);
  assert.ok(r.spanMs >= 5000 && r.spanMs < 5100, `span ${r.spanMs} ms`);
  assert.equal(r.longFrames, 0);
});

test("an audio clock running 25 % slow reads +25 %, positive", () => {
  const slip = new ClockSlip();
  const ctx = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, ctx, clock, 6, 0.75);
  const r = slip.reading();
  assert.ok(Math.abs(r.slipPct - 25) < 0.01, `slip ${r.slipPct} %`);
  assert.ok(Math.abs(r.slipMs - 0.25 * r.spanMs) < 0.01, `slip ${r.slipMs} ms over ${r.spanMs} ms`);
});

test("the window forgets a stall once it is 5 s old", () => {
  const slip = new ClockSlip();
  const ctx = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, ctx, clock, 2, 1.0);
  drive(slip, ctx, clock, 0.5, 0.0);          /* the render thread stalls for half a second */
  drive(slip, ctx, clock, 2, 1.0);
  assert.ok(Math.abs(slip.reading().slipMs - 500) < 20, `during: ${slip.reading().slipMs} ms`);
  drive(slip, ctx, clock, 6, 1.0);
  assert.ok(Math.abs(slip.reading().slipMs) < 1e-6, `after: ${slip.reading().slipMs} ms`);
});

test("long frames are counted inside the window only", () => {
  const slip = new ClockSlip();
  const ctx = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, ctx, clock, 1, 1.0);
  drive(slip, ctx, clock, 0.2, 1.0, 50);      /* four 50 ms frames: a stalled MAIN thread */
  const r = slip.reading();
  assert.equal(r.longFrames, 4);
  assert.ok(Math.abs(r.slipMs) < 1e-6, "a stalled main thread samples both clocks late, not slipped");
  drive(slip, ctx, clock, 6, 1.0);
  assert.equal(slip.reading().longFrames, 0);
});

test("a suspended context reports its state, not a 100 % slip", () => {
  const slip = new ClockSlip();
  const ctx = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, ctx, clock, 2, 1.0);
  ctx.state = "suspended";
  drive(slip, ctx, clock, 2, 0.0);
  assert.equal(slip.reading().state, "suspended");
  ctx.state = "running";
  drive(slip, ctx, clock, 1, 1.0);
  const r = slip.reading();
  assert.equal(r.state, "running");
  assert.ok(Math.abs(r.slipMs) < 1e-6, `the suspended stretch leaked into the window: ${r.slipMs} ms`);
});

test("a new AudioContext starts a new window", () => {
  const slip = new ClockSlip();
  const a = { state: "running", currentTime: 0 };
  const clock = { wall: 1000 };
  drive(slip, a, clock, 3, 0.5);
  const b = { state: "running", currentTime: 0 };
  drive(slip, b, clock, 1, 1.0);
  assert.ok(Math.abs(slip.reading().slipMs) < 1e-6, "the old context's slip survived a rebuild");
});
