/**
 * probe.js - the offline laterality probe the headless check drives.
 *
 * WHY IT IS NEEDED AT ALL. The page's audio leaves through the AudioWorklet sink into the
 * AudioContext's destination, and a page cannot tap that: it never holds the sink's node. So there
 * is no way to ASSERT from inside the running demo that a source on the listener's left is louder
 * in the left ear - which is the one claim a spatial demo has to earn.
 *
 * WHAT IT DOES INSTEAD. It opens a SECOND engine, in the default "worker" topology on the MANUAL
 * sink, feeds it the page's own stimulus through a push source, places the source with the page's
 * own room coordinates, and pumps `bwa_render_block` - the offline path the Python and MATLAB
 * bindings use for exactly this. The numbers it reports are real engine output.
 *
 * It is imported lazily by the test hook, so the demo never pays for it.
 *
 * THE STIMULUS IS A CLICK, never DC. CLAUDE.md's first trap: a DC-driven laterality assertion
 * found the default HRTF's per-ear DC gains opposing its audible ILD, mis-diagnosed a correct
 * encode and shipped a left/right mirror. Drive a signal with a spectrum.
 */
import { BwaEngine, SinkType } from "../dist/index.js";
import { click } from "./stimulus.js";

/**
 * Render a source at one room position and report the two ears' energy.
 *
 * @param {object} opts
 * @param {number} opts.profile bwa_desc.profile, the same one the page is running
 * @param {number[]} opts.source room x,y,z
 * @param {number[]} [opts.head] room x,y,z of the listener, default [0, 1.5, 0]
 * @param {number} [opts.blocks] blocks to render after the ramps settle
 * @returns {Promise<{left:number, right:number, channels:number, blockSize:number}>}
 *   `left` and `right` are RMS over the measured blocks, linear.
 */
export async function measureLaterality({ profile, source, head = [0, 1.5, 0], blocks = 220 }) {
  const engine = await BwaEngine.create({
    topology: "worker",
    sink: SinkType.MANUAL,
    profile,
    blockSize: 256,
  });
  try {
    const rate = engine.info.sampleRate;
    const pcm = click(rate);
    const src = await engine.createPushSource();
    await src.setGain(0.9);
    await engine.start();
    engine.listener.setPose(head[0], head[1], head[2], 0, 0, 0, 1);
    src.setPosition(source[0], source[1], source[2]);
    engine.pushFrame();

    let read = 0;
    let sumL = 0;
    let sumR = 0;
    let n = 0;
    /* The first blocks are thrown away on purpose. Per-voice gains RAMP (invariant 4), so the
     * opening blocks hold the interpolation from silence and not the steady state this measures. */
    const warm = 40;
    for (let b = 0; b < blocks + warm; ++b) {
      let space = await src.space();
      while (space >= 512) {
        const chunk = new Float32Array(512);
        for (let i = 0; i < 512; ++i) { chunk[i] = pcm[read]; read = (read + 1) % pcm.length; }
        await src.push(chunk);
        space -= 512;
      }
      const blk = await engine.renderBlock();
      if (!blk || b < warm) continue;
      const { channels, nframes, data } = blk;
      if (channels < 2) throw new Error(`the manual sink gave ${channels} channels, expected a stereo pair`);
      for (let i = 0; i < nframes; ++i) {
        const l = data[i];                        /* planar: channel c starts at c * nframes */
        const r = data[nframes + i];
        sumL += l * l;
        sumR += r * r;
      }
      n += nframes;
    }
    if (!n) throw new Error("the manual sink rendered nothing");
    return {
      left: Math.sqrt(sumL / n),
      right: Math.sqrt(sumR / n),
      channels: 2,
      blockSize: engine.info.blockSize,
    };
  } finally {
    await engine.destroy();
  }
}
