/*
 * render.test.mjs - the MANUAL sink through the binding, and the frame slab that feeds it.
 *
 * The manual sink is the offline path this binding offers in place of bwa_set_output_capture: no
 * thread, no device, the caller pumps and gets the planar bus back. It is also the only way to
 * assert what the engine actually RENDERED from JavaScript, so the two things worth pinning here
 * are pinned against real samples:
 *
 *   - the batched frame message really moves a source, under ONE commit;
 *   - a source on the right sounds right, on the left sounds left.
 *
 * TONES, never DC. The CLAUDE.md trap: a DC-driven laterality assertion reads the HRTF's per-ear
 * DC gains, which oppose its audible ILD, and that mistake shipped a left/right mirror once.
 */
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { haveModule, loadModule, loadSrc, tone } from "./helper.mjs";

if (!haveModule) {
  console.log("bw_audio.mjs not built; run tools/wasm/build-web.sh");
  process.exit(77);
}

const SINK_MANUAL = 3;
const PROFILE_BINAURAL = 1;

let Host, host, proto, slab, u32, f32;

before(async () => {
  const M = await loadModule();
  ({ Host } = await loadSrc("host.js"));
  proto = await loadSrc("protocol.js");
  slab = new SharedArrayBuffer(proto.slabBytes(8));
  u32 = new Uint32Array(slab);
  f32 = new Float32Array(slab);
  host = new Host();
  await host.init({
    moduleFactory: async () => M,
    engine: { profile: PROFILE_BINAURAL, sink: SINK_MANUAL, blockSize: 256 },
    slab,
  });
  host.handle("start", {});
});

after(() => { try { host.handle("destroy", {}); } catch { /* already gone */ } });

/** Push a tone and pump `blocks` blocks, returning the summed |sample| per channel. */
function energy(push, blocks) {
  const e = host.engine;
  const rate = e.sampleRate;
  let channels = 0;
  let sum = null;
  for (let b = 0; b < blocks; ++b) {
    const chunk = tone(e.blockSize, rate, 440);
    push(chunk);
    const r = host.handle("render", {});
    assert.ok(r, "the manual sink returned no block");
    if (!sum) { channels = r.channels; sum = new Float64Array(channels); }
    for (let c = 0; c < channels; ++c)
      for (let i = 0; i < r.nframes; ++i) sum[c] += Math.abs(r.data[c * r.nframes + i]);
  }
  return sum;
}

test("render_block hands back a planar block of the sink's width", () => {
  const src = host.engine.createPushSource();
  src.push(tone(host.engine.blockSize, host.engine.sampleRate));
  const r = host.handle("render", {});
  assert.ok(r, "no block");
  assert.equal(r.nframes, host.engine.blockSize);
  assert.ok(r.channels >= 2, `expected at least a stereo bus, got ${r.channels}`);
  assert.equal(r.data.length, r.channels * r.nframes);
  src.destroy();
});

test("the batched frame message moves a source, and laterality follows it", () => {
  const e = host.engine;
  const src = e.createPushSource();
  src.setGain(1.0);
  src.play(0);                 /* a push source plays what is pushed; no sound handle needed */

  /* Write the slab the way client.pushFrame does, then let the control side apply it. This is the
   * per-frame path under test: one pose, one position, one commit. */
  const setPos = (x, y, z) => {
    u32[proto.SLAB_FLAGS] = 1;                                  /* FLAG_POSE */
    u32[proto.SLAB_COUNT] = 1;
    f32[proto.SLAB_POSE + 0] = 0; f32[proto.SLAB_POSE + 1] = 0; f32[proto.SLAB_POSE + 2] = 0;
    f32[proto.SLAB_POSE + 3] = 0; f32[proto.SLAB_POSE + 4] = 0; f32[proto.SLAB_POSE + 5] = 0;
    f32[proto.SLAB_POSE + 6] = 1;                               /* identity: ahead is +Z */
    const o = proto.SLAB_HEADER;
    u32[o] = src.handle;
    f32[o + 1] = x; f32[o + 2] = y; f32[o + 3] = z;
    Atomics.store(u32, proto.SLAB_SEQ, Atomics.load(u32, proto.SLAB_SEQ) + 1);
    host.handle("frame", {});
  };

  /* BWA_ROOM_RIGHT is -X (bw_audio.h). Settle the gain ramps first: they interpolate across a
   * block by design (invariant 4), so the first blocks after a jump are a crossfade. */
  setPos(-3, 0, 0);
  energy((c) => src.push(c), 24);
  const right = energy((c) => src.push(c), 24);

  setPos(3, 0, 0);
  energy((c) => src.push(c), 24);
  const left = energy((c) => src.push(c), 24);

  assert.ok(right[0] + right[1] > 1, "the render was silent: nothing to compare");

  /* A ratio with a real margin, not "one is bigger": both sides landing near zero when the
   * mechanism breaks is the coin flip CLAUDE.md warns about. */
  const rRight = right[1] / right[0];
  const rLeft = left[1] / left[0];
  assert.ok(rRight > 1.15, `a source on the right gave R/L = ${rRight.toFixed(3)}`);
  assert.ok(rLeft < 0.87, `a source on the left gave R/L = ${rLeft.toFixed(3)}`);

  src.destroy();
});

test("a repeated seq is skipped, so a burst of frame messages collapses", () => {
  const e = host.engine;
  const src = e.createSource();
  u32[proto.SLAB_FLAGS] = 0;
  u32[proto.SLAB_COUNT] = 1;
  const o = proto.SLAB_HEADER;
  u32[o] = src.handle;
  f32[o + 1] = 1; f32[o + 2] = 0; f32[o + 3] = 0;
  Atomics.store(u32, proto.SLAB_SEQ, 12345);
  host.handle("frame", {});
  /* The first half of the assertion, and it is the half that stops this test from being one that
   * cannot fail: a frame path that never ran would leave lastSeq where it was, and the "a repeat
   * changed nothing" check below would then pass for the wrong reason. */
  assert.equal(host.lastSeq, 12345, "the frame was not applied at all");
  host.handle("frame", {});
  host.handle("frame", {});
  assert.equal(host.lastSeq, 12345, "the same seq was applied more than once");
  src.destroy();
});
