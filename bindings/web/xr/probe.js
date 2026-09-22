/**
 * probe.js - the offline laterality probe, with a HEAD ORIENTATION.
 *
 * DUPLICATION, declared: this is `playground/probe.js` with one parameter added. That file's
 * `measureLaterality` hard-codes an identity listener quaternion, which is right for a page whose
 * head never turns and useless for this one, where the head turning IS the demonstration. It
 * takes no orientation argument, so there is nothing to pass. Fold the two together by giving the
 * playground's version a `headQuat` defaulting to identity and delete this file; the rest of the
 * body is character for character the same, and the reasons in its header apply here unchanged.
 *
 * Those reasons, in short, because they are the point: a page cannot tap its own AudioWorklet
 * output, so the only way to ASSERT what the listener hears is to open a SECOND engine on the
 * MANUAL sink and render the same scene offline. The numbers below are real engine output.
 *
 * THE STIMULUS IS A CLICK, never DC. CLAUDE.md's first trap: a DC-driven laterality assertion
 * found the default HRTF's per-ear DC gains opposing its audible ILD, mis-diagnosed a correct
 * encode and shipped a left and right mirror. Drive a signal with a spectrum.
 */
import { BwaEngine, SinkType } from "../dist/index.js";
import { click } from "../playground/stimulus.js";

/**
 * Render one source, with the listener at one pose, and report the two ears' energy.
 *
 * @param {object} opts
 * @param {number} opts.profile bwa_desc.profile, the same one the page is running
 * @param {number[]} opts.source room x,y,z
 * @param {number[]} [opts.head] room x,y,z of the listener
 * @param {number[]} [opts.headQuat] the listener's room orientation, xyzw. Identity faces room +z.
 * @param {number} [opts.blocks] blocks to measure after the ramps settle
 * @returns {Promise<{left:number, right:number, ratioDb:number, blockSize:number}>}
 */
export async function measureLateralityOriented({
  profile, source, head = [0, 1.5, 0], headQuat = [0, 0, 0, 1], blocks = 220,
}) {
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
    engine.listener.setPose(head[0], head[1], head[2],
                            headQuat[0], headQuat[1], headQuat[2], headQuat[3]);
    src.setPosition(source[0], source[1], source[2]);
    engine.pushFrame();

    let read = 0;
    let sumL = 0;
    let sumR = 0;
    let n = 0;
    /* The first blocks are thrown away on purpose. Per-voice gains RAMP (invariant 4), so the
     * opening blocks hold the interpolation from silence and not the steady state this measures.
     * The head pose reaches the mixer the same way, through one CMD_COMMIT. */
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
    const left = Math.sqrt(sumL / n);
    const right = Math.sqrt(sumR / n);
    return {
      left,
      right,
      ratioDb: 20 * Math.log10(Math.max(left, 1e-9) / Math.max(right, 1e-9)),
      blockSize: engine.info.blockSize,
    };
  } finally {
    await engine.destroy();
  }
}
