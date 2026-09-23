/*
 * constants.test.mjs - the header's integer #defines reach both layers.
 *
 * gen-abi.mjs parses them out of include/bw_audio.h, so the failure this guards is a SILENT one: a
 * header edit that changes a define's shape (an expression where a literal was) drops it from the
 * table, and every page that sized a buffer with it reads `undefined`.
 * `new Float32Array(undefined)` is an empty array, not an error. The pinned values are the header's contract as of the 64-channel
 * capacity split; a deliberate change to either is a change to this file too.
 *
 * Needs no wasm module: the table is plain JavaScript, so this runs on a checkout with no build.
 */
import { test, before } from "node:test";
import assert from "node:assert/strict";
import { loadSrc } from "./helper.mjs";

let CONSTANTS, eng;

before(async () => {
  ({ CONSTANTS } = await loadSrc("raw.js"));
  eng = await loadSrc("engine.js");
});

test("the capacity and the default grid are exported with the header's values", () => {
  assert.ok(CONSTANTS, "raw.js exports no CONSTANTS; regenerate with tools/wasm/gen-abi.mjs");
  assert.equal(CONSTANTS.MAX_CHANNELS, 64, "BWA_MAX_CHANNELS");
  assert.equal(CONSTANTS.DEFAULT_GRID, 26, "BWA_DEFAULT_GRID");
});

test("the rest of the plain integer defines are there too", () => {
  assert.equal(CONSTANTS.CHANNEL_AUTO, -1);
  assert.equal(CONSTANTS.GROUPS, 8);
  assert.equal(CONSTANTS.EXTRA_LIS, 3);
  assert.equal(CONSTANTS.SINK_FLAG_EXCLUSIVE, 0x1);
  assert.equal(CONSTANTS.SINK_FLAG_EXACT_RATE, 0x2);
  assert.equal(CONSTANTS.SINK_FLAG_TIGHT_BUFFER, 0x4);
  /* Computed or non-numeric defines are skipped, not evaluated, and the version is ABI_VERSION. */
  for (const k of Object.keys(CONSTANTS)) {
    assert.ok(Number.isInteger(CONSTANTS[k]), `${k} is not an integer`);
    assert.ok(!k.startsWith("VERSION"), `${k} belongs in ABI_VERSION`);
  }
});

test("the idiomatic layer re-exports them rather than keeping its own copy", () => {
  assert.equal(eng.MAX_CHANNELS, CONSTANTS.MAX_CHANNELS);
  assert.equal(eng.DEFAULT_GRID, CONSTANTS.DEFAULT_GRID);
  assert.equal(eng.CHANNEL_AUTO, CONSTANTS.CHANNEL_AUTO);
  assert.equal(eng.GROUPS, CONSTANTS.GROUPS);
  assert.equal(eng.EXTRA_LIS, CONSTANTS.EXTRA_LIS);
  assert.deepEqual(eng.SinkFlags, { NONE: 0, EXCLUSIVE: 0x1, EXACT_RATE: 0x2, TIGHT_BUFFER: 0x4 });
});
