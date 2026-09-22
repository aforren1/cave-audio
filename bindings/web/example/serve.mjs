#!/usr/bin/env node
/*
 * serve.mjs - a static file server that sets the two cross-origin isolation headers.
 *
 * WHY IT EXISTS. docs/web.md's decision is the faithful two-thread model over SharedArrayBuffer,
 * and every current browser gates SharedArrayBuffer behind cross-origin isolation:
 *
 *   Cross-Origin-Opener-Policy: same-origin
 *   Cross-Origin-Embedder-Policy: require-corp
 *
 * A page served without them gets `SharedArrayBuffer is not defined` and the binding refuses to
 * start rather than dropping to a shape that looks like the engine and is not. `python -m
 * http.server` cannot set headers, which is the whole reason this file is here rather than a line
 * in the README. Cross-Origin-Resource-Policy goes out too, so a subresource this server hands
 * over is CORP-clean against its own isolated page.
 *
 *   node bindings/web/example/serve.mjs [port]
 *
 * Node only, no dependencies. It serves the REPO ROOT, so the page at
 * /bindings/web/example/index.html can reach /bindings/web/dist/ with the relative import it
 * already uses. It is a development server: no caching, no ranges, no directory listing.
 */
import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(fileURLToPath(new URL("../../..", import.meta.url)));
const PORT = Number(process.argv[2] || 8181);

const TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
  ".wav": "audio/wav",
  ".css": "text/css; charset=utf-8",
  ".map": "application/json; charset=utf-8",
};

createServer((req, res) => {
  let path = decodeURIComponent(new URL(req.url, "http://x").pathname);
  if (path === "/") path = "/bindings/web/example/index.html";
  /* normalize before the prefix check: "/../" in a request must not reach outside the root. */
  const file = join(ROOT, normalize(path));
  if (!file.startsWith(ROOT)) { res.writeHead(403).end("forbidden"); return; }

  let st;
  try { st = statSync(file); } catch { res.writeHead(404).end("not found"); return; }
  if (st.isDirectory()) { res.writeHead(404).end("not found"); return; }

  res.writeHead(200, {
    "Content-Type": TYPES[extname(file)] || "application/octet-stream",
    "Content-Length": st.size,
    "Cache-Control": "no-store",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
  });
  createReadStream(file).pipe(res);
}).listen(PORT, () => {
  console.log(`bw_audio example: http://localhost:${PORT}/  (cross-origin isolated)`);
  console.log(`serving ${ROOT}`);
});
