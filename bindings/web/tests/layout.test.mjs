/*
 * layout.test.mjs - the file system seam: a layout a page uploaded is the layout the engine loads.
 *
 * `bwa_desc.layout_path` is a PATH, and the engine opens it with an ordinary fopen inside
 * bwa_create. A browser has no synchronous file system for that to reach, so the only path that
 * can ever resolve is one in the module's own MEMFS - which is why the module exports FS and the
 * binding wraps it (host.js's writeFile, and `create({ files })` for the two paths create itself
 * opens).
 *
 * Offline, on the null sink: the seam is the file system and the loader, and neither of those
 * needs a device or a browser. The playground's own check (tests/run-playground.mjs) covers the
 * upload as a page does it, cones and all.
 */
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { haveModule, loadModule, loadSrc } from "./helper.mjs";

if (!haveModule) {
  console.log("bw_audio.mjs not built; run tools/wasm/build-web.sh");
  process.exit(77);
}

const SINK_NULL = 2;
const PROFILE_CAVE_SIM = 2;

/* Somewhere the default grid puts nothing, so reading it back cannot be a coincidence: the grid is
 * a 3x3x3 boundary shell at +/-1.5 m with y at 0, 1.5 and 3. */
const ODD = [0.37, 2.13, -1.91];

/** A valid N-speaker cave_layout.json (docs/layout-schema.md), with speaker 3 somewhere odd. */
function layoutBytes(n = 8, mutate = null) {
  const speakers = [];
  for (let i = 0; i < n; ++i) {
    const a = (2 * Math.PI * i) / n;
    speakers.push({
      index: i,
      position: i === 3 ? ODD.slice() : [2 * Math.cos(a), 1.2, 2 * Math.sin(a)],
      gain_db: 0,
      delay_ms: 0,
    });
  }
  const doc = { schema_version: 1, speakers };
  if (mutate) mutate(doc);
  return new TextEncoder().encode(JSON.stringify(doc));
}

let Host, M;
const live = [];

before(async () => {
  M = await loadModule();
  ({ Host } = await loadSrc("host.js"));
});

after(() => {
  for (const h of live) {
    try { h.handle("destroy", {}); } catch { /* already gone */ }
  }
});

/** One engine on the shared module, so the pthread pool is not paid for per test. */
async function openHost(engine, files) {
  const host = new Host();
  await host.init({ moduleFactory: async () => M, engine, files });
  live.push(host);
  return host;
}

function speakers(host) {
  const r = host.invokeBuf("get_speakers", [{ out: "f32", len: 26 * 3 }, 26]);
  return { count: r.value, xyz: r.out[0] };
}

test("the module exports FS, or nothing can hand the engine a file", () => {
  assert.ok(M.FS, "FS is not in EXPORTED_RUNTIME_METHODS; see bindings/web/CMakeLists.txt");
});

test("a layout passed as `files` is the layout bwa_create loads", async () => {
  const host = await openHost(
    { profile: PROFILE_CAVE_SIM, sink: SINK_NULL, blockSize: 256, layoutPath: "/t8.json" },
    { "/t8.json": layoutBytes(8) }
  );
  assert.equal(host.info().channelCount, 8, "the 8-speaker file did not become the channel count");
  const { count, xyz } = speakers(host);
  assert.equal(count, 8);
  for (let i = 0; i < 3; ++i) {
    assert.ok(Math.abs(xyz[9 + i] - ODD[i]) < 1e-3,
              `speaker 3 came back at ${xyz[9]},${xyz[10]},${xyz[11]}, not ${ODD}`);
  }
  host.handle("start", {});
  host.handle("destroy", {});
});

test("writeFile puts a file where a later engine can open it", async () => {
  /* The other half of the seam: `files` writes before create, this writes whenever, and both land
   * in the same module file system. A second engine on the same module proves the file is really
   * there rather than an argument create happened to keep. */
  const host = await openHost({ profile: PROFILE_CAVE_SIM, sink: SINK_NULL, blockSize: 256 });
  assert.equal(host.info().channelCount, 26, "no layout should be the default 26-speaker grid");
  const n = host.handle("writefile", { path: "/later/t6.json", data: layoutBytes(6) });
  assert.ok(n > 0);
  host.handle("destroy", {});

  const second = await openHost(
    { profile: PROFILE_CAVE_SIM, sink: SINK_NULL, blockSize: 256, layoutPath: "/later/t6.json" }
  );
  assert.equal(second.info().channelCount, 6);
  second.handle("destroy", {});
});

test("a layout the loader rejects survives create and refuses at start", async () => {
  /* The engine's rule, which a page has to show rather than hide (CLAUDE.md): create stays usable
   * on the default grid so nothing is dead, and bwa_start refuses with BWA_ERR_LAYOUT. gain_db 99
   * is outside the loader's [-100, 24]. */
  const bad = layoutBytes(8, (d) => { d.speakers[2].gain_db = 99; });
  const host = await openHost(
    { profile: PROFILE_CAVE_SIM, sink: SINK_NULL, blockSize: 256, layoutPath: "/bad.json" },
    { "/bad.json": bad }
  );
  assert.equal(host.info().channelCount, 26, "a refused layout should leave the default grid");
  assert.ok(host.info().lastError, "bwa_last_error says nothing about the refused layout");
  assert.throws(() => host.handle("start", {}), /layout/i,
                "bwa_start accepted an engine whose explicit layout failed to load");
  host.handle("destroy", {});
});

test("a missing layout path is refused the same way", async () => {
  const host = await openHost(
    { profile: PROFILE_CAVE_SIM, sink: SINK_NULL, blockSize: 256, layoutPath: "/nothing/here.json" }
  );
  assert.throws(() => host.handle("start", {}), /layout/i);
  host.handle("destroy", {});
});
