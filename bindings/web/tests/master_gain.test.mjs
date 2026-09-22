/*
 * master_gain.test.mjs - the master gain scales the render in EVERY profile.
 *
 * Why this exists: bwa_set_master_gain used to scale the speaker bus alone, and under
 * BWA_PROFILE_BINAURAL point voices never touch that bus (they render through the direct SH
 * field), so the web playground's volume slider did nothing on headphones while the same slider
 * worked in cave_sim. Measured through the manual sink on 2026-09-22: a 0.10 ratio in cave and
 * cave_sim, 1.00 in binaural. The engine fix runs the same ramp over every buffer the decode
 * reads; this pins it at the binding, per profile, so the profiles cannot drift apart again.
 *
 * Worker topology, manual sink, a pushed tone, never DC. The ratio is asserted with a margin on
 * both sides: a test whose two sides both land near zero when the mechanism breaks is a coin flip.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { haveModule, loadModule, loadSrc, tone } from "./helper.mjs";

if (!haveModule) {
  console.log("bw_audio.mjs not built; run tools/wasm/build-web.sh");
  process.exit(77);
}

const SINK_MANUAL = 3;
const PROFILES = { cave: 0, binaural: 1, cave_sim: 2 };

async function energyRatio(profile) {
  const M = await loadModule();
  const { Host } = await loadSrc("host.js");
  const proto = await loadSrc("protocol.js");
  const slab = new SharedArrayBuffer(proto.slabBytes(8));
  const host = new Host();
  await host.init({ moduleFactory: async () => M, engine: { profile, sink: SINK_MANUAL, blockSize: 256 }, slab });
  host.handle("start", {});
  const e = host.engine;
  const src = e.createPushSource();
  /* Off-axis, so the direct-binaural field carries lateral channels too, not W alone. */
  src.setPosition(1.0, 1.5, 1.0);
  const run = (blocks) => {
    let sum = 0;
    for (let b = 0; b < blocks; ++b) {
      src.push(tone(e.blockSize, e.sampleRate, 440));
      const r = host.handle("render", {});
      assert.ok(r, "the manual sink returned no block");
      for (let i = 0; i < r.data.length; ++i) sum += Math.abs(r.data[i]);
    }
    return sum;
  };
  run(30);                              /* let the voice's gain ramp settle */
  const full = run(30);
  e.setMasterGain(0.1);
  run(4);                               /* the ramp lands within one block; four is margin */
  const tenth = run(30);
  e.setMasterGain(1.0);
  run(4);
  const back = run(30);
  host.handle("destroy", {});
  assert.ok(full > 1e-3, "the source renders at unity (the test needs signal to scale)");
  return { ratio: tenth / full, backRatio: back / full };
}

for (const [name, profile] of Object.entries(PROFILES)) {
  test(`master gain 0.1 scales the ${name} render by 0.1`, async () => {
    const { ratio, backRatio } = await energyRatio(profile);
    assert.ok(ratio > 0.08 && ratio < 0.12, `${name}: energy ratio ${ratio.toFixed(3)}, expected about 0.10`);
    assert.ok(backRatio > 0.95 && backRatio < 1.05, `${name}: back at unity, ratio ${backRatio.toFixed(3)}`);
  });
}
