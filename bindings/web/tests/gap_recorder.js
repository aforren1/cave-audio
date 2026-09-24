/*
 * gap_recorder.js - an AudioWorklet processor that watches EVERY sample the sink's node puts into
 * the graph and counts runs of silence. browser.html connects it to the worklet sink's
 * AudioWorkletNode, beside the sink's own connection to the destination.
 *
 * Why not an AnalyserNode, the way the playground's meters tap the node: an analyser is POLLED
 * from the main thread, so it sees a window of the most recent samples each time somebody asks and
 * nothing in between. A 5 ms hole that lands between two polls is invisible to it, and the main
 * thread is exactly what the busy-loop check stalls. A processor runs on the audio thread for every
 * quantum, so a hole cannot fall between its looks.
 *
 * What it can see: a quantum the sink filled with silence because its ring was empty (a starve).
 * What it cannot: the browser's own audio callback missing its deadline downstream of the graph.
 * The graph's samples stay continuous then and the device plays a glitch anyway, which is why the
 * page also reads AudioContext.playbackStats where the browser has it.
 */
class GapRecorder extends AudioWorkletProcessor {
  constructor() {
    super();
    this.reset();
    this.port.onmessage = (e) => {
      if (e.data === "reset") this.reset();
      else if (e.data === "report") {
        this.flush();
        this.port.postMessage({
          frames: this.frames, gaps: this.gaps, longest: this.longest,
          silentFrames: this.silentFrames, peak: this.peak,
        });
      }
    };
  }

  reset() {
    this.frames = 0;        /* samples per channel watched since the reset */
    this.run = 0;           /* the current run of silent samples */
    this.longest = 0;       /* the longest completed run */
    this.gaps = 0;          /* completed runs of at least GAP_FRAMES */
    this.silentFrames = 0;  /* every silent sample, in a run of any length */
    this.peak = 0;
  }

  flush() {
    if (this.run > this.longest) this.longest = this.run;
    if (this.run >= GAP_FRAMES) this.gaps++;
    this.run = 0;
  }

  process(inputs) {
    const inp = inputs[0];
    /* An input with no channels is a disconnected or inactive source: the whole quantum is silence. */
    const n = inp && inp.length ? inp[0].length : 128;
    const l = inp && inp.length ? inp[0] : null;
    const r = inp && inp.length > 1 ? inp[1] : l;
    for (let i = 0; i < n; ++i) {
      const a = l ? Math.abs(l[i]) : 0;
      const b = r ? Math.abs(r[i]) : 0;
      if (a > this.peak) this.peak = a;
      if (b > this.peak) this.peak = b;
      if (a < SILENT && b < SILENT) {
        this.run++;
        this.silentFrames++;
      } else if (this.run) {
        this.flush();
      }
    }
    this.frames += n;
    return true;
  }
}

/* A digital zero from the sink's memset; real pink noise never sits this low for long. */
const SILENT = 1e-7;
/* 5 ms at 48 kHz: the gap the check forbids. Shorter runs are counted in silentFrames only. */
const GAP_FRAMES = 240;

registerProcessor("bwa-gap-recorder", GapRecorder);
