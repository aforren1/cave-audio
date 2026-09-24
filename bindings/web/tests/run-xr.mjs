#!/usr/bin/env node
/*
 * run-xr.mjs - drive bindings/web/xr in headless Chrome and report its verdict.
 *
 * The sibling of run-playground.mjs, in the same shape and for the same reasons: a Chromium, the
 * two cross-origin isolation headers, --autoplay-policy=no-user-gesture-required so the page's
 * resume works with nobody to click, and a POST back with the verdict because a headless browser
 * has no other way to hand a script a result. It SKIPS (exit 77, ctest's skip code) when it finds
 * no browser, no engine build, or no vendored three.js.
 *
 *   node bindings/web/tests/run-xr.mjs [--browser <path>] [--keep] [--site <staged dir>]
 *                                      [--no-layout | --layout-only] [--shots <dir>]
 *
 * IT RUNS THE PAGE TWICE, because the page has three states and two of them are decided before
 * any script of ours could change them:
 *
 *   pass 1, `__xr=0`: navigator.xr is DELETED before the page's module runs. That is the no-WebXR
 *           state: the page has to say so, offer the flat playground, refuse Enter VR, and still
 *           start the engine and render the flat view.
 *   pass 2, `__xr=1`: a minimal FAKE navigator.xr is installed instead, so an immersive-vr
 *           session opens, a synthetic XRFrame hands back a viewer pose of the driver's choosing,
 *           and the whole head-pose path runs end to end.
 *
 * WHAT THE FAKE PROVES, and what it cannot. It proves the seam (an XR viewer pose becomes the
 * right room listener pose), the per-frame pose write and commit, the prediction lead, the
 * in-world menu including its hit test, the controller grab, and - through a SECOND engine on the
 * manual sink - that a head turn really moves the image from centered to one ear. It cannot prove
 * three.js's stereo rendering, the projection layer, a real runtime's controller poses or gamepad
 * layout, reprojection, or anything about latency. Those need a headset. docs/web.md says so in
 * the same words.
 *
 * THE DRIVER IS INJECTED, not shipped, and it goes in the HEAD as a CLASSIC script. The runner
 * appends the tag for a request carrying `?__drive=1`, so the page a visitor loads carries no test
 * code. It has to be in the head and classic because the fake `navigator.xr` must exist BEFORE
 * xr/main.js runs, and a module script is deferred: the page's own module would run first and ask
 * the real navigator.xr whether a headset exists.
 *
 * THEN THE LAYOUT PASS (tests/layout_cdp.mjs, skipped with --no-layout): the page at phone,
 * headset-window and desktop sizes, one load each with the fake device installed, so Enter VR is
 * live and the check can hold it to "on screen without scrolling". --shots <dir> saves a
 * screenshot of every state it checks.
 */
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { createReadStream, existsSync, mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { LayoutCheck } from "./layout_cdp.mjs";

const ROOT = resolve(fileURLToPath(new URL("../../..", import.meta.url)));
const PORT = 8191;
const SKIP = 77;

const args = process.argv.slice(2);
const argOf = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : null;
};

/* --site <dir>: drive a STAGED site (the output of bindings/web/deploy/stage.sh) instead of the
 * repo tree. Same driver, same assertions, but the pages are the rewritten copies with the
 * content-addressed dist-<hash>/ imports, which is the one thing the repo-tree run cannot check.
 * The driver is served from the repo at a virtual path either way, so it never has to be staged. */
const SITE = argOf("--site") ? resolve(argOf("--site")) : null;
const SERVE_ROOT = SITE || ROOT;
const PAGE = SITE ? "/xr/index.html" : "/bindings/web/xr/index.html";
const DRIVER = "/__bwa_xr_driver.js";
const PROBE = "/__bwa_layout_probe.mjs";

const CANDIDATES = [
  argOf("--browser"),
  process.env.BWA_BROWSER,
  process.env.CHROME_PATH,
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium",
  "/usr/bin/chromium-browser",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
].filter(Boolean);

const browser = CANDIDATES.find((p) => existsSync(p));
if (!browser) {
  console.log("no Chromium found; skipping the XR check (pass --browser <path> to force one)");
  process.exit(SKIP);
}
if (SITE) {
  if (!existsSync(join(SITE, "xr/index.html"))) {
    console.log(`no xr/index.html under --site ${SITE}; run bindings/web/deploy/stage.sh first`);
    process.exit(SKIP);
  }
} else {
  if (!existsSync(join(ROOT, "bindings/web/dist/bw_audio.mjs"))) {
    console.log("bindings/web/dist is not built; run tools/wasm/build-web.sh");
    process.exit(SKIP);
  }
  if (!existsSync(join(ROOT, "bindings/web/dist/vendor/three/three.module.js"))) {
    console.log("bindings/web/dist/vendor is empty; run tools/wasm/fetch-web-vendor.sh (it needs network)");
    process.exit(SKIP);
  }
}

const TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
};
const ISOLATION = {
  "Cache-Control": "no-store",
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

let done = null;
const layout = new LayoutCheck({
  browser, name: "xr", shotsDir: argOf("--shots") ? resolve(argOf("--shots")) : null,
});
/** The layout pass's verdicts arrive on the same /__result as the driver passes'. */
function nextVerdict(ms) {
  return new Promise((settle) => {
    const t = setTimeout(() => finish({ pass: false, notes: [`a layout pass never reported back within ${ms / 1000} s`] }), ms);
    const finish = (r) => { clearTimeout(t); done = null; settle(r); };
    done = finish;
  });
}

const server = createServer((req, res) => {
  if (layout.handle(req, res)) return;
  if (req.method === "POST" && req.url === "/__result") {
    let body = "";
    req.on("data", (c) => { body += c; });
    req.on("end", () => {
      res.writeHead(204).end();
      try { done?.(JSON.parse(body)); } catch { done?.({ pass: false, notes: ["bad verdict: " + body] }); }
    });
    return;
  }
  const url = new URL(req.url, "http://x");
  const path = decodeURIComponent(url.pathname);

  if (path === PAGE && url.searchParams.has("__drive")) {
    /* The tag goes right after the service worker's, which is the first thing in the head, so the
     * driver's fake navigator.xr is in place before the page's deferred module runs. */
    const src = readFileSync(join(SERVE_ROOT, normalize(path)), "utf8");
    const anchor = '<script src="../coi-serviceworker.js"></script>';
    if (!src.includes(anchor)) {
      res.writeHead(500).end("the XR page no longer loads ../coi-serviceworker.js");
      return;
    }
    const html = src.replace(anchor, `${anchor}\n<script src="${DRIVER}"></script>`);
    res.writeHead(200, { ...ISOLATION, "Content-Type": TYPES[".html"] });
    res.end(html);
    return;
  }
  if (path === DRIVER) {
    res.writeHead(200, { ...ISOLATION, "Content-Type": TYPES[".js"] });
    createReadStream(join(ROOT, "bindings/web/tests/xr_driver.js")).pipe(res);
    return;
  }
  if (path === PROBE) {
    res.writeHead(200, { ...ISOLATION, "Content-Type": TYPES[".js"] });
    createReadStream(join(ROOT, "bindings/web/tests/layout_probe.mjs")).pipe(res);
    return;
  }

  const file = join(SERVE_ROOT, normalize(path));
  if (!file.startsWith(SERVE_ROOT)) { res.writeHead(403).end(); return; }
  let st;
  try { st = statSync(file); } catch { res.writeHead(404).end(); return; }
  if (st.isDirectory()) { res.writeHead(404).end(); return; }
  res.writeHead(200, {
    ...ISOLATION,
    "Content-Type": TYPES[extname(file)] || "application/octet-stream",
    "Content-Length": st.size,
  });
  createReadStream(file).pipe(res);
});

/** One browser, one page load, one verdict. */
function runPass(query, label, timeoutMs) {
  return new Promise((settle) => {
    const profile = mkdtempSync(join(tmpdir(), "bwa-xr-"));
    const child = spawn(browser, [
      "--headless=new",
      "--disable-gpu",
      "--use-gl=swiftshader",     /* the page needs a WebGL context; headless has no real one */
      "--no-first-run",
      "--no-default-browser-check",
      "--autoplay-policy=no-user-gesture-required",
      `--user-data-dir=${profile}`,
      "--enable-logging=stderr",
      "--v=0",
      `http://localhost:${PORT}${PAGE}?__drive=1&${query}`,
    ], { stdio: ["ignore", "pipe", "pipe"] });

    let stderr = "";
    child.stderr.on("data", (c) => { stderr += c; });
    child.stdout.on("data", (c) => { stderr += c; });

    const finish = (result) => {
      clearTimeout(timer);
      done = null;
      child.kill();
      if (!args.includes("--keep")) { try { rmSync(profile, { recursive: true, force: true }); } catch {} }
      settle({ ...result, label, stderr });
    };
    const timer = setTimeout(
      () => finish({ pass: false, notes: [`the ${label} pass never reported back within ${timeoutMs / 1000} s`] }),
      timeoutMs
    );
    done = finish;
  });
}

server.listen(PORT, async () => {
  const notes = [];
  let pass = true;
  let noise = "";
  /* --layout-only skips the two driver passes, for work on the pages' CSS. */
  const passes = args.includes("--layout-only") ? []
    : [["__xr=0", "no-WebXR", 120000], ["__xr=1", "fake-XR", 240000]];
  for (const [query, label, ms] of passes) {
    const r = await runPass(query, label, ms);
    notes.push(`---- ${label} ----`, ...r.notes);
    if (!r.pass) { pass = false; noise += r.stderr; }
  }
  if (!args.includes("--no-layout")) {
    const url = (vp) => `http://localhost:${PORT}${PAGE}?__drive=1&__xr=1&__layout=${vp.label}` +
                        `&__mode=${vp.mode}&__vw=${vp.w}` + (vp.rotate ? `&__rotate=${vp.rotate}` : "");
    const lr = await layout.run(url, nextVerdict);
    notes.push("---- layout ----", ...lr.notes);
    if (!lr.pass) pass = false;
  }
  server.close();
  for (const n of notes) console.log(n);
  if (!pass) {
    const lines = noise.split("\n").filter((l) => /ERROR|Uncaught|audio|WebGL/i.test(l)).slice(0, 25);
    if (lines.length) console.log("\nbrowser output:\n" + lines.join("\n"));
  }
  console.log(pass ? "\nXR check OK" : "\nXR check FAILED");
  process.exit(pass ? 0 : 1);
});
