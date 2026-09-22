/*
 * raw_layer.test.mjs - the generated table against the module's own exports.
 *
 * This is the half of the "one for one" claim that a generator cannot make on its own. gen-abi.mjs
 * proves the table came from the header; this proves the MODULE carries every entry in it, that
 * every wrapper is callable, and that the one deliberate exclusion really is absent rather than
 * broken.
 */
import { test, before } from "node:test";
import assert from "node:assert/strict";
import { haveModule, loadModule, loadSrc } from "./helper.mjs";

if (!haveModule) {
  console.log("bw_audio.mjs not built; run tools/wasm/build-web.sh");
  process.exit(77);                       /* ctest SKIP_RETURN_CODE */
}

let raw, ABI, ABI_VERSION, M;

before(async () => {
  M = await loadModule();
  const rawMod = await loadSrc("raw.js");
  ABI = rawMod.ABI;
  ABI_VERSION = rawMod.ABI_VERSION;
  raw = rawMod.makeRaw(M);
});

test("the table is not empty and the header was really parsed", () => {
  assert.ok(ABI.length > 150, `expected the whole ABI, got ${ABI.length} entries`);
  for (const f of ABI) {
    assert.match(f.name, /^bwa_/);
    assert.ok(["void", "num", "i64", "cstr", "ptr"].includes(f.ret), `${f.name}: ret ${f.ret}`);
  }
});

test("every declared call is exported by the module and wrapped", () => {
  /* makeRaw throws on a missing symbol, so reaching here already proves it. Check the shape too,
   * so a silently renamed entry cannot pass. */
  for (const f of ABI) {
    const jsName = f.name.slice(4);
    assert.equal(typeof raw[jsName], "function", `${f.name} is missing from the raw layer`);
    assert.equal(typeof M["_" + f.name], "function", `${f.name} is missing from the module`);
  }
});

test("bwa_set_output_capture is absent from both layers", () => {
  /* Its callback runs on the audio thread. Absent, not present-and-throwing, so there is nothing
   * to find and be tempted by - the same exclusion the Python and MATLAB bindings make. */
  assert.equal(ABI.find((f) => f.name === "bwa_set_output_capture"), undefined);
  assert.equal(raw.set_output_capture, undefined);
});

test("the module's ABI version matches the header the table came from", () => {
  const want = (ABI_VERSION[0] << 16) | (ABI_VERSION[1] << 8) | ABI_VERSION[2];
  assert.equal(raw.get_version(), want);
});

test("an engine-free call works, with its C units", () => {
  const a = raw.host_time_ns();
  const b = raw.host_time_ns();
  assert.ok(typeof a === "number" && a > 0, "host_time_ns should be a plain number of ns");
  assert.ok(b >= a, "the host clock must be monotonic");
  /* i64 crossing: -sWASM_BIGINT makes this a BigInt at the boundary and the raw layer converts.
   * A number in the BigInt millions here is the proof the conversion happened. */
  assert.ok(a > 1e6, `host_time_ns looks unconverted: ${a}`);

  /* AUTO, NULL and MANUAL report no devices (docs/backends.md rule 10's other half). */
  assert.equal(raw.get_device_count(0), 0);
  assert.equal(raw.get_device_count(2), 0);
});

test("a cstr return comes back as a string or null, never a pointer", () => {
  const s = raw.last_error(0);
  assert.ok(s === null || typeof s === "string", `last_error returned ${typeof s}`);
});
