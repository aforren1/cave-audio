#!/usr/bin/env node
/*
 * run-browser.mjs - drive browser.html in headless Chrome and report its verdict.
 *
 * The AudioWorklet sink cannot be tested under node: node has no Web Audio. This is the one check
 * that exercises it, and it needs a Chromium. It is therefore OPT-IN and it SKIPS (exit 77, the
 * ctest skip code) when it cannot find one, rather than failing a build on a machine that has no
 * browser. CI runners usually do have one; the rig machines do not have to.
 *
 *   node bindings/web/tests/run-browser.mjs [--browser <path>] [--keep]
 *
 * It serves the repo root with the two cross-origin isolation headers (the same pair
 * example/serve.mjs sets, because without them SharedArrayBuffer is undefined and the engine
 * refuses to start), opens the page, and waits for the page to POST its verdict back. A POST is
 * the transport because a headless browser has no other way to hand a script a result without a
 * DevTools client, and --dump-dom fires at load rather than after two seconds of audio.
 *
 * --autoplay-policy=no-user-gesture-required is what lets the page resume its AudioContext with
 * nobody to click. A real page must do that from a gesture handler; example/index.html does.
 */
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { createReadStream, existsSync, mkdtempSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(fileURLToPath(new URL("../../..", import.meta.url)));
const PORT = 8189;
const SKIP = 77;

const args = process.argv.slice(2);
const argOf = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : null;
};

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
  console.log("no Chromium found; skipping the browser check (pass --browser <path> to force one)");
  process.exit(SKIP);
}
if (!existsSync(join(ROOT, "bindings/web/dist/bw_audio.mjs"))) {
  console.log("bindings/web/dist is not built; run tools/wasm/build-web.sh");
  process.exit(SKIP);
}

const TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
};

let done = null;
const verdict = new Promise((r) => { done = r; });

const server = createServer((req, res) => {
  if (req.method === "POST" && req.url === "/__result") {
    let body = "";
    req.on("data", (c) => { body += c; });
    req.on("end", () => {
      res.writeHead(204).end();
      try { done(JSON.parse(body)); } catch (e) { done({ pass: false, notes: ["bad verdict: " + body] }); }
    });
    return;
  }
  const path = decodeURIComponent(new URL(req.url, "http://x").pathname);
  const file = join(ROOT, normalize(path));
  if (!file.startsWith(ROOT)) { res.writeHead(403).end(); return; }
  let st;
  try { st = statSync(file); } catch { res.writeHead(404).end(); return; }
  if (st.isDirectory()) { res.writeHead(404).end(); return; }
  res.writeHead(200, {
    "Content-Type": TYPES[extname(file)] || "application/octet-stream",
    "Content-Length": st.size,
    "Cache-Control": "no-store",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
  });
  createReadStream(file).pipe(res);
});

server.listen(PORT, async () => {
  const profile = mkdtempSync(join(tmpdir(), "bwa-chrome-"));
  const child = spawn(browser, [
    "--headless=new",
    "--disable-gpu",
    "--no-first-run",
    "--no-default-browser-check",
    "--autoplay-policy=no-user-gesture-required",
    `--user-data-dir=${profile}`,
    "--enable-logging=stderr",
    "--v=0",
    `http://localhost:${PORT}/bindings/web/tests/browser.html`,
  ], { stdio: ["ignore", "pipe", "pipe"] });

  let stderr = "";
  child.stderr.on("data", (c) => { stderr += c; });
  child.stdout.on("data", (c) => { stderr += c; });

  const timeout = setTimeout(() => {
    done({ pass: false, notes: ["the page never reported back within 60 s"] });
  }, 60000);

  const result = await verdict;
  clearTimeout(timeout);
  child.kill();
  server.close();
  if (!args.includes("--keep")) { try { rmSync(profile, { recursive: true, force: true }); } catch {} }

  for (const n of result.notes) console.log(n);
  if (!result.pass) {
    const noise = stderr.split("\n").filter((l) => /ERROR|Uncaught|audio/i.test(l)).slice(0, 20);
    if (noise.length) console.log("\nbrowser output:\n" + noise.join("\n"));
  }
  console.log(result.pass ? "\nbrowser check OK" : "\nbrowser check FAILED");
  process.exit(result.pass ? 0 : 1);
});
