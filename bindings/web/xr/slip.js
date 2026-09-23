/**
 * slip.js - a dropout signal that needs no clock on the audio thread.
 *
 * WHY THIS EXISTS. Web Audio reports no xrun, no underrun and no device position, so the engine's
 * health block cannot count a dropout on this backend (`measured` is false). And the AudioWorklet
 * thread has no `performance`, so the render times the engine does measure there come from
 * Date.now: whole milliseconds against a 2.67 ms quantum (src/os/os_posix.c). The MAIN thread has
 * both clocks a dropout needs: `performance.now()`, the wall clock, and `AudioContext.currentTime`,
 * the frames the graph has actually rendered. When the render thread misses its deadline, Chrome
 * plays fallback silence and the graph does not advance, so the audio clock falls behind the wall.
 * Measured in headless Chrome 153 with a worklet that busy-waits on purpose: a 100 ms stall once a
 * second read 119 ms of slip per 5 s, a 20 ms stall read nothing (the device buffer absorbed it).
 * So the slip is the part of a stall that outlasted the buffer: the silence, not the stall.
 *
 * WHAT IT READS. Over a trailing window (5 s): SLIP = wall elapsed minus audio-clock elapsed, in ms
 * and as a percent of the window. 0 is healthy. Positive means the audio clock ran slow, which is
 * the render thread not keeping up. The reading carries the resolution of `currentTime` on the
 * main thread, which Chrome updates once per device callback: a few ms either way, so treat
 * anything under about 0.3 % of a 5 s window as zero.
 *
 * WHAT IT CANNOT TELL APART, and the second number that helps. A stalled MAIN thread samples late
 * but samples both clocks at the same late moment, so it does not create slip by itself. It does
 * mean the page drops frames, which is worth knowing when you are deciding whose fault a glitch
 * is. So each window also counts the animation frames that took longer than 30 ms: slip with no
 * long frames is the audio thread; long frames with no slip is the main thread.
 *
 * A SUSPENDED CONTEXT is not a dropout. Its clock stops by design, so the window restarts whenever
 * the context is not "running" and the reading says why instead of reporting 100 %.
 *
 * Fixed-size typed rings, so sampling allocates nothing per frame.
 */

const CAP = 1024;               /* samples: 5 s at 120 Hz is 600, so 1024 never wraps inside a window */

export class ClockSlip {
  /**
   * @param {number} [windowMs] trailing window, default 5000
   * @param {number} [longFrameMs] an animation frame longer than this counts as long, default 30
   */
  constructor(windowMs = 5000, longFrameMs = 30) {
    this.windowMs = windowMs;
    this.longFrameMs = longFrameMs;
    this._wall = new Float64Array(CAP);
    this._audio = new Float64Array(CAP);
    this._long = new Float64Array(CAP);  /* wall times of long frames */
    this.reset(null);
  }

  /** Forget everything, for example because the page built a new AudioContext. */
  reset(ctx) {
    this._ctx = ctx;
    this._head = 0;      /* index of the oldest sample */
    this._n = 0;
    this._lHead = 0;
    this._lN = 0;
    this._lastWall = 0;
    this._state = ctx ? ctx.state : "none";
  }

  /**
   * Take one sample. Call it once per animation frame, with that frame's timestamp.
   * @param {number} wallMs a performance.now() timestamp (a rAF or XR frame time is one)
   * @param {AudioContext|null} ctx the context the engine renders into
   */
  sample(wallMs, ctx) {
    if (!ctx) return;
    if (ctx !== this._ctx) this.reset(ctx);
    if (this._lastWall && wallMs - this._lastWall > this.longFrameMs) this._pushLong(wallMs);
    this._lastWall = wallMs;
    this._state = ctx.state;
    if (ctx.state !== "running") { this._n = 0; this._head = 0; return; }

    const i = (this._head + this._n) % CAP;
    this._wall[i] = wallMs;
    this._audio[i] = ctx.currentTime * 1000;
    if (this._n < CAP) this._n++; else this._head = (this._head + 1) % CAP;

    /* Keep ONE sample at or beyond the window's far edge, so the reading spans the whole window. */
    while (this._n > 2 && wallMs - this._wall[(this._head + 1) % CAP] >= this.windowMs) {
      this._head = (this._head + 1) % CAP;
      this._n--;
    }
    while (this._lN > 0 && wallMs - this._long[this._lHead] > this.windowMs) {
      this._lHead = (this._lHead + 1) % CAP;
      this._lN--;
    }
  }

  _pushLong(wallMs) {
    this._long[(this._lHead + this._lN) % CAP] = wallMs;
    if (this._lN < CAP) this._lN++; else this._lHead = (this._lHead + 1) % CAP;
  }

  /**
   * @returns {{state: string, spanMs: number, slipMs: number, slipPct: number,
   *            frames: number, longFrames: number}|null}
   *   null before the context has run for two frames. `spanMs` is the window actually covered,
   *   which is shorter than `windowMs` for the first seconds after a start.
   */
  reading() {
    if (this._state !== "running") {
      return this._ctx ? { state: this._state, spanMs: 0, slipMs: 0, slipPct: 0, frames: 0,
                           longFrames: this._lN } : null;
    }
    if (this._n < 2) return null;
    const a = this._head, b = (this._head + this._n - 1) % CAP;
    const spanMs = this._wall[b] - this._wall[a];
    if (!(spanMs > 0)) return null;
    const slipMs = spanMs - (this._audio[b] - this._audio[a]);
    return {
      state: "running",
      spanMs,
      slipMs,
      slipPct: (100 * slipMs) / spanMs,
      frames: this._n - 1,
      longFrames: this._lN,
    };
  }
}
