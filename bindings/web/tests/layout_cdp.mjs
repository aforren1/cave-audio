/**
 * layout_cdp.mjs - the runner half of the responsive-layout check, shared by run-playground.mjs
 * and run-xr.mjs.
 *
 * The drivers run INSIDE the page, and a page cannot resize its own viewport, emulate a finger or
 * take a picture of itself. So this launches its own Chromium with a DevTools port, sets each
 * viewport through the DevTools protocol (Emulation.setDeviceMetricsOverride, plus touch emulation
 * for a phone so `(pointer: coarse)` matches), loads the page with `__layout` in the query, and
 * waits for the driver's verdict on the runner's own /__result. The driver asks for a screenshot
 * at each state through /__layout/shot and for a viewport change through /__layout/viewport; the
 * runner's server hands both to handle() below.
 *
 * No Puppeteer: Node 22 and later carry a WebSocket client, and the protocol calls used here are
 * six. A Node without one SKIPS the layout pass with a note rather than failing the page's check.
 */
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

/* The sizes the pages are checked at. The headset's 2D window has not been measured, so two
 * guesses bracket it. `mode` is what the CSS must produce there, stated here and not derived. */
export const VIEWPORTS = [
  { label: "phone-portrait",   w: 390,  h: 844, phone: true,  mode: "stacked", rotate: "844x390:side" },
  { label: "phone-320",        w: 320,  h: 568, phone: true,  mode: "stacked" },
  { label: "phone-landscape",  w: 844,  h: 390, phone: true,  mode: "side" },
  { label: "phone-landscape-667", w: 667, h: 375, phone: true, mode: "side" },
  { label: "headset-1024",     w: 1024, h: 600, phone: false, mode: "side" },
  { label: "headset-800",      w: 800,  h: 500, phone: false, mode: "side" },
  { label: "desktop",          w: 1440, h: 900, phone: false, mode: "side" },
];

export class LayoutCheck {
  /**
   * @param {object} o
   * @param {string} o.browser     Chromium executable
   * @param {string} o.name        page name, the screenshot file prefix
   * @param {string|null} o.shotsDir  where the PNGs go; null takes no pictures
   */
  constructor(o) {
    this.browser = o.browser;
    this.name = o.name;
    this.shotsDir = o.shotsDir;
    this.vp = null;
    this.cdp = null;
  }

  /** The runner's server calls this first; true means the request was one of ours. */
  handle(req, res) {
    const url = new URL(req.url, "http://x");
    if (!url.pathname.startsWith("/__layout/")) return false;
    const answer = (p) => p.then(() => res.writeHead(204).end(),
                                 (e) => res.writeHead(500).end(String(e && e.message || e)));
    if (url.pathname === "/__layout/shot") {
      answer(this._shot(url.searchParams.get("tag") || "shot"));
    } else if (url.pathname === "/__layout/viewport") {
      const w = Number(url.searchParams.get("w")), h = Number(url.searchParams.get("h"));
      answer(this._metrics(w, h, this.vp ? this.vp.phone : false));
    } else {
      res.writeHead(404).end();
    }
    return true;
  }

  /**
   * One page load per viewport.
   * @param {(vp: object) => string} urlFor      the page URL for a viewport, `__layout` included
   * @param {(ms: number) => Promise<object>} nextVerdict  the next /__result body, or a timeout
   * @returns {Promise<{pass: boolean, notes: string[], skipped?: boolean}>}
   */
  async run(urlFor, nextVerdict) {
    if (typeof WebSocket !== "function") {
      return { pass: true, skipped: true, notes: ["no WebSocket in this Node; the layout pass needs Node 22 or later"] };
    }
    if (this.shotsDir) mkdirSync(this.shotsDir, { recursive: true });
    const profile = mkdtempSync(join(tmpdir(), "bwa-layout-"));
    const child = spawn(this.browser, [
      "--headless=new",
      "--disable-gpu",
      "--use-gl=swiftshader",
      "--no-first-run",
      "--no-default-browser-check",
      "--autoplay-policy=no-user-gesture-required",
      "--hide-scrollbars",
      `--user-data-dir=${profile}`,
      "--remote-debugging-port=0",
      "about:blank",
    ], { stdio: ["ignore", "pipe", "pipe"] });
    const notes = [];
    let pass = true;
    try {
      const wsUrl = await new Promise((res, rej) => {
        let buf = "";
        const t = setTimeout(() => rej(new Error("Chromium printed no DevTools endpoint")), 20000);
        const on = (c) => {
          buf += c;
          const m = /DevTools listening on (ws:\/\/\S+)/.exec(buf);
          if (m) { clearTimeout(t); res(m[1]); }
        };
        child.stderr.on("data", on);
        child.stdout.on("data", on);
      });
      this.cdp = await Cdp.connect(wsUrl);
      const { targetId } = await this.cdp.send("Target.createTarget", { url: "about:blank" });
      const { sessionId } = await this.cdp.send("Target.attachToTarget", { targetId, flatten: true });
      this.cdp.session = sessionId;
      /* A tab that is not in front can have its animation frames paused, and the probe waits on
       * them. One first load hung once before this; that this was the cause is a guess. */
      await this.cdp.send("Page.bringToFront", {}, true);
      await this.cdp.send("Emulation.setFocusEmulationEnabled", { enabled: true }, true);

      for (const vp of VIEWPORTS) {
        this.vp = vp;
        await this._metrics(vp.w, vp.h, vp.phone);
        await this.cdp.send("Emulation.setTouchEmulationEnabled",
                            { enabled: vp.phone, maxTouchPoints: vp.phone ? 5 : 1 }, true);
        const verdict = nextVerdict(120000);
        await this.cdp.send("Page.navigate", { url: urlFor(vp) }, true);
        const r = await verdict;
        notes.push(`-- ${vp.label} (${vp.w}x${vp.h}, ${vp.mode}) --`, ...r.notes);
        if (!r.pass) pass = false;
      }
    } catch (e) {
      pass = false;
      notes.push("FAIL layout pass: " + (e && e.stack ? e.stack : String(e)));
    } finally {
      try { this.cdp?.close(); } catch {}
      child.kill();
      try { rmSync(profile, { recursive: true, force: true }); } catch {}
    }
    if (this.shotsDir) notes.push(`screenshots in ${this.shotsDir}`);
    return { pass, notes };
  }

  async _metrics(w, h, phone) {
    await this.cdp.send("Emulation.setDeviceMetricsOverride",
                        { width: w, height: h, deviceScaleFactor: 1, mobile: phone }, true);
  }

  async _shot(tag) {
    if (!this.shotsDir || !this.cdp) return;
    const { data } = await this.cdp.send("Page.captureScreenshot", { format: "png" }, true);
    writeFileSync(join(this.shotsDir, `${this.name}-${this.vp.label}-${tag}.png`), Buffer.from(data, "base64"));
  }
}

/** The smallest DevTools protocol client that works: one socket, flat sessions, no events. */
class Cdp {
  static connect(url) {
    return new Promise((res, rej) => {
      const ws = new WebSocket(url);
      const c = new Cdp(ws);
      ws.onopen = () => res(c);
      ws.onerror = () => rej(new Error(`could not open ${url}`));
    });
  }
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.session = null;
    ws.onmessage = (ev) => {
      const m = JSON.parse(typeof ev.data === "string" ? ev.data : Buffer.from(ev.data).toString());
      const p = m.id !== undefined && this.pending.get(m.id);
      if (!p) return;
      this.pending.delete(m.id);
      if (m.error) p.rej(new Error(`${p.method}: ${m.error.message}`));
      else p.res(m.result);
    };
  }
  /** @param {boolean} [onPage] send to the attached page session rather than the browser */
  send(method, params = {}, onPage = false) {
    const id = ++this.id;
    const msg = { id, method, params };
    if (onPage) msg.sessionId = this.session;
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej, method });
      this.ws.send(JSON.stringify(msg));
    });
  }
  close() { this.ws.close(); }
}
