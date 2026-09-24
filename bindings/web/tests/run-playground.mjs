#!/usr/bin/env node
/*
 * run-playground.mjs - drive bindings/web/playground in headless Chrome and report its verdict.
 *
 * The sibling of run-browser.mjs, in the same shape and for the same reasons: a Chromium, the two
 * cross-origin isolation headers, --autoplay-policy=no-user-gesture-required so the page's resume
 * works with nobody to click, and a POST back with the verdict because a headless browser has no
 * other way to hand a script a result. It SKIPS (exit 77, ctest's skip code) when it finds no
 * browser, no engine build, or no vendored three.js.
 *
 *   node bindings/web/tests/run-playground.mjs [--browser <path>] [--keep] [--site <staged dir>]
 *                                              [--no-layout | --layout-only] [--shots <dir>]
 *
 * WHAT IT DRIVES, and what run-browser.mjs already covers so this does not. run-browser.mjs proves
 * the SINK: the setup chain, process() on the worklet thread, the audio clock, the suspend and
 * resume handoff. This one proves the DEMO on top of it: that the page starts on that sink, that
 * the coordinate seam puts a source on the listener's left both on screen and in the ears, and
 * that the channel walk's bus meter lights the channel it drove.
 *
 * THE DRIVER IS INJECTED, not shipped. The server appends one script tag to the playground's HTML
 * for a request carrying `?__drive=1`, so the page a visitor loads has no test code in it and the
 * check still runs against the real page. The assertions live in tests/playground_driver.js.
 *
 * THEN THE LAYOUT PASS (tests/layout_cdp.mjs, skipped with --no-layout): the same page at phone,
 * headset-window and desktop sizes, one load each, with the driver in its `__layout` mode.
 * --shots <dir> saves a screenshot of every state it checks, which is how a person looks at what
 * the numbers passed.
 */
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { createReadStream, existsSync, mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { LayoutCheck } from "./layout_cdp.mjs";

const ROOT = resolve(fileURLToPath(new URL("../../..", import.meta.url)));
const PORT = 8190;
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
const PAGE = SITE ? "/playground/index.html" : "/bindings/web/playground/index.html";
const DRIVER = "/__bwa_playground_driver.js";
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
  console.log("no Chromium found; skipping the playground check (pass --browser <path> to force one)");
  process.exit(SKIP);
}
if (SITE) {
  if (!existsSync(join(SITE, "playground/index.html"))) {
    console.log(`no playground/index.html under --site ${SITE}; run bindings/web/deploy/stage.sh first`);
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

/* One verdict per page load: the driver pass, then one per layout viewport. */
let done = null;
function nextVerdict(ms, what = "the page") {
  return new Promise((settle) => {
    const t = setTimeout(() => finish({ pass: false, notes: [`${what} never reported back within ${ms / 1000} s`] }), ms);
    const finish = (r) => { clearTimeout(t); done = null; settle(r); };
    done = finish;
  });
}

const layout = new LayoutCheck({
  browser, name: "playground", shotsDir: argOf("--shots") ? resolve(argOf("--shots")) : null,
});

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
    const html = readFileSync(join(SERVE_ROOT, normalize(path)), "utf8").replace(
      "</body>",
      `<script type="module" src="${DRIVER}"></script>\n</body>`
    );
    res.writeHead(200, { ...ISOLATION, "Content-Type": TYPES[".html"] });
    res.end(html);
    return;
  }
  if (path === DRIVER) {
    res.writeHead(200, { ...ISOLATION, "Content-Type": TYPES[".js"] });
    createReadStream(join(ROOT, "bindings/web/tests/playground_driver.js")).pipe(res);
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

/** The driver's main pass: one browser, one page load, one verdict. */
async function mainPass() {
  const profile = mkdtempSync(join(tmpdir(), "bwa-pg-"));
  const child = spawn(browser, [
    "--headless=new",
    "--disable-gpu",
    "--use-gl=swiftshader",       /* the page needs a WebGL context; headless has no real one */
    "--no-first-run",
    "--no-default-browser-check",
    "--autoplay-policy=no-user-gesture-required",
    `--user-data-dir=${profile}`,
    "--enable-logging=stderr",
    "--v=0",
    `http://localhost:${PORT}${PAGE}?__drive=1`,
  ], { stdio: ["ignore", "pipe", "pipe"] });

  let stderr = "";
  child.stderr.on("data", (c) => { stderr += c; });
  child.stdout.on("data", (c) => { stderr += c; });

  const result = await nextVerdict(150000);
  child.kill();
  if (!args.includes("--keep")) { try { rmSync(profile, { recursive: true, force: true }); } catch {} }

  for (const n of result.notes) console.log(n);
  if (!result.pass) {
    const noise = stderr.split("\n").filter((l) => /ERROR|Uncaught|audio|WebGL/i.test(l)).slice(0, 25);
    if (noise.length) console.log("\nbrowser output:\n" + noise.join("\n"));
  }
  return result.pass;
}

server.listen(PORT, async () => {
  /* --layout-only skips the driver's main pass, for work on the pages' CSS. */
  let pass = args.includes("--layout-only") ? true : await mainPass();
  if (!args.includes("--no-layout")) {
    const url = (vp) => `http://localhost:${PORT}${PAGE}?__drive=1&__layout=${vp.label}&__mode=${vp.mode}&__vw=${vp.w}` +
                        (vp.rotate ? `&__rotate=${vp.rotate}` : "");
    const lr = await layout.run(url, (ms) => nextVerdict(ms, "a layout pass"));
    console.log("\n---- layout ----");
    for (const n of lr.notes) console.log(n);
    if (!lr.pass) pass = false;
  }
  server.close();
  console.log(pass ? "\nplayground check OK" : "\nplayground check FAILED");
  process.exit(pass ? 0 : 1);
});
