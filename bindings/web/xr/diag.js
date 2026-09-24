/**
 * diag.js - a rolling diagnostics log for the headset, as plain text a person can paste.
 *
 * WHY. The headset is where the audio chops and where the window froze once (2026-09-24), and it
 * is also where nobody can attach a debugger. So the page keeps the last 30 s at 1 Hz, one line
 * per second, and shows it in a textarea when the session ends or on the "copy diagnostics"
 * button. Nothing leaves the page: no network, no storage.
 *
 * WHAT A LINE CARRIES, and why each one:
 *   - underruns and their ms: the browser's own count of silence its output played
 *     (AudioContext.playbackStats, read into bwa_health by worklet_sink.c), plus the browser's
 *     average output latency from the same object;
 *   - late blocks and the render peak against the block period;
 *   - the audio-clock slip (xr/slip.js), for a glitch downstream of the graph;
 *   - the AudioContext state and rate;
 *   - the JS heap, where performance.memory exists (Chromium), for memory growth;
 *   - the frame loop: frames this second, the longest frame interval, and how long ago the last
 *     frame ran, so a loop that STOPS shows as a growing "last frame" age;
 *   - the health poll: how long ago one last came back, and whether one has been outstanding
 *     for more than 2 s, so a poll that never returns shows as "poll STUCK";
 *   - any page error since the last line.
 * If the page freezes outright the 1 Hz timer stops too, and the LAST line is the state just before.
 *
 * Fixed-size: 30 lines, each built once a second. Nothing here runs per audio block.
 */

const KEEP = 30;
const STUCK_MS = 2000;

export class Diag {
  constructor() {
    this.lines = [];
    this.t0 = performance.now();
    this.frames = 0;
    this.frameMaxMs = 0;
    this.lastFrameT = 0;
    this.pollStart = 0;         /* when the outstanding poll started; 0 = none outstanding */
    this.pollDone = 0;          /* when a poll last came back */
    this.errors = [];
    this.timer = 0;
  }

  /** Every animation frame, flat or in session. */
  noteFrame(nowMs) {
    if (this.lastFrameT) {
      const dt = nowMs - this.lastFrameT;
      if (dt > this.frameMaxMs) this.frameMaxMs = dt;
    }
    this.lastFrameT = nowMs;
    this.frames++;
  }

  notePollStart() { if (!this.pollStart) this.pollStart = performance.now(); }
  notePollDone() { this.pollStart = 0; this.pollDone = performance.now(); }

  noteError(msg) {
    /* ASCII and one line: this text is pasted into a chat, and may reach a console. */
    const one = String(msg).replace(/[^\x20-\x7e]/g, "?").slice(0, 160);
    if (this.errors.length < 8) this.errors.push(one);
  }

  /** Start the 1 Hz sampler. `app` is the page state (rig, slip). */
  start(app) {
    if (this.timer) return;
    this.app = app;
    this.timer = setInterval(() => this.sample(), 1000);
  }

  sample() {
    const app = this.app;
    const now = performance.now();
    const parts = [`t=${((now - this.t0) / 1000).toFixed(0)}s`];
    const ctx = app.rig?.ctx;
    parts.push(ctx ? `ctx=${ctx.state}@${ctx.sampleRate}` : "ctx=-");
    const ps = ctx?.playbackStats;
    if (ps && typeof ps.averageLatency === "number")
      parts.push(`avglat=${(ps.averageLatency * 1000).toFixed(0)}ms`);
    const i = app.rig?.engine?.info;
    const h = app.rig?.health;
    if (i && h) {
      const periodMs = (1000 * i.blockSize) / i.sampleRate;
      parts.push(`blk=${i.blockSize} lat=${i.outputLatencyFrames}`);
      parts.push(h.measured
        ? `underruns=${h.xruns} (${((1000 * h.droppedFrames) / i.sampleRate).toFixed(0)}ms)`
        : "underruns=- (no playbackStats)");
      parts.push(`late=${h.lateBlocks}`);
      parts.push(`peak=${(h.peakLoad * periodMs).toFixed(1)}/${periodMs.toFixed(1)}ms`);
      parts.push(`lost=${h.deviceLost}`);
    } else {
      parts.push("health=-");
    }
    const s = app.slip?.reading();
    parts.push(s && s.state === "running" ? `slip=${s.slipPct.toFixed(1)}%` : `slip=${s ? s.state : "-"}`);
    parts.push(`frames=${this.frames} fmax=${this.frameMaxMs.toFixed(0)}ms`);
    parts.push(this.lastFrameT ? `lastframe=${((now - this.lastFrameT) / 1000).toFixed(1)}s` : "lastframe=never");
    if (this.pollStart && now - this.pollStart > STUCK_MS)
      parts.push(`poll STUCK ${((now - this.pollStart) / 1000).toFixed(1)}s`);
    else parts.push(this.pollDone ? `poll=${((now - this.pollDone) / 1000).toFixed(1)}s` : "poll=never");
    const mem = globalThis.performance?.memory;
    if (mem && mem.usedJSHeapSize) parts.push(`heap=${(mem.usedJSHeapSize / 1048576).toFixed(1)}MB`);
    if (app.xr?.presenting) parts.push("xr");
    if (this.errors.length) parts.push(`err: ${this.errors.join(" | ")}`);
    this.errors = [];
    this.frames = 0;
    this.frameMaxMs = 0;
    this.lines.push(parts.join(" "));
    if (this.lines.length > KEEP) this.lines.shift();
  }

  /** The whole log, oldest first, with a header naming what produced it. */
  text() {
    const ua = (globalThis.navigator?.userAgent ?? "").replace(/[^\x20-\x7e]/g, "?");
    return [`bw_audio XR diagnostics, last ${this.lines.length} s at 1 Hz`, `ua: ${ua}`,
            ...this.lines].join("\n");
  }
}
