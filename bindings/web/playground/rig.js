/**
 * rig.js - the audio half: one engine, one push source, and the per-frame feed.
 *
 * WHY A PUSH SOURCE AND NOT AN ASSET. There is no synchronous file system in a browser, so
 * `bwa_load_sound` has nothing to open unless a file is already in the wasm file system, and this
 * binding does not put one there (bindings/web/README.md, "Where the audio comes from"). The route
 * the binding recommends is the one here: make or decode the samples in JavaScript and feed a PUSH
 * source, which is the engine's one inbound exception and a source feed rather than a render path.
 *
 * WHY CAVE_SIM IS THE DEFAULT PROFILE. The playground is the CAVE, auditioned. In
 * `BWA_PROFILE_CAVE_SIM` every point source pans through the real DBAP/SPCAP/VBAP solve into the
 * 26-channel bus, and the bus is then HRTF-decoded to stereo for headphones - so the speaker
 * gizmos light from `bwa_get_bus_levels`, which is the array's own output and not a drawing of
 * one. `BWA_PROFILE_BINAURAL` is offered beside it because it is the other thing the engine does:
 * point voices bypass the panner entirely and get their own HRTF convolution. Switching between
 * them is a CREATE-time change, so it rebuilds the engine; the AudioContext survives, because the
 * page owns it.
 *
 * THE FEED IS PACED ON THE RING, not on the animation frame. A background tab gets fewer frames
 * and the audio thread does not slow down with it (bindings/web/README.md says so, and the
 * example page does the same).
 */
import { BwaEngine, Profile } from "../dist/index.js";
import { SIGNALS } from "./stimulus.js";

const CHUNK = 2048;

export class Rig {
  constructor() {
    this.engine = null;
    this.ctx = null;
    this.src = null;
    this.pcm = null;
    this.readPos = 0;
    this.signal = 0;
    this.profile = Profile.CAVE_SIM;
    this.channelCount = 0;
    this.speakers = new Float32Array(26 * 3);
    this.busLevels = new Float32Array(26);
    this.activeVoices = 0;
    this.health = null;
    this._chunk = new Float32Array(CHUNK);
    this._busy = false;
    this._pollBusy = false;
  }

  /**
   * Create the AudioContext (the caller must be inside a user gesture), then the engine.
   * @param {object} opts
   * @param {AudioContext} opts.audioContext
   * @param {number} [opts.profile]
   * @param {number} [opts.blockSize]
   */
  async open({ audioContext, profile = Profile.CAVE_SIM, blockSize = 256 }) {
    this.ctx = audioContext;
    this.profile = profile;
    /* No layoutPath. The engine's default grid IS the 26-speaker geometry
     * examples/cave_layout.json describes (a 3x3x3 boundary grid at +/-1.5 m, y 0/1.5/3, minus the
     * center), and the page reads the positions back with bwa_get_speakers rather than assuming
     * them - so the gizmos are the engine's layout whatever it turns out to be. The file's only
     * extra content is the measured per-speaker delay trim, which nothing here can hear, and
     * getting a file into the wasm file system needs an entry this binding does not have. */
    this.engine = await BwaEngine.create({
      audioContext,
      profile,
      blockSize,
      /* No `sink`: handing `audioContext` over IS the sink choice. client.js sets
       * SinkType.WORKLET and the "main" topology from it, and takes the engine's sample rate from
       * the context, because only the page may create or resume one. */
    });
    this.src = await this.engine.createPushSource();
    await this.src.setGain(0.9);
    await this.engine.start();

    this.channelCount = this.engine.info.channelCount;
    const got = await this.engine.invokeBuf("get_speakers", { out: "f32", len: 26 * 3 }, 26);
    this.speakers = got.out[0];
    this.speakerCount = got.value;

    this.setSignal(this.signal);
    return this.engine.info;
  }

  /** Rebuild the engine in a different profile, keeping the page's AudioContext. */
  async setProfile(profile) {
    if (!this.engine || profile === this.profile) return this.engine.info;
    const ctx = this.ctx;
    const blockSize = this.engine.info.blockSize;
    await this.engine.destroy();
    this.engine = null;
    this.src = null;
    return this.open({ audioContext: ctx, profile, blockSize });
  }

  /** Swap the looped stimulus. Index into `stimulus.js`'s SIGNALS. */
  setSignal(i) {
    this.signal = i;
    this.pcm = SIGNALS[i].make(this.engine.info.sampleRate);
    this.readPos = 0;
  }

  /**
   * Top the push ring up. Re-entrant-safe: an overlapping call returns at once rather than
   * interleaving two writers into one SPSC ring.
   */
  async feed() {
    if (this._busy || !this.src || !this.pcm) return;
    this._busy = true;
    try {
      let space = await this.src.space();
      let guard = 8;                     /* a cap, so a huge ring cannot stall the frame */
      while (space >= CHUNK && guard-- > 0) {
        const c = this._chunk;
        for (let i = 0; i < CHUNK; ++i) {
          c[i] = this.pcm[this.readPos];
          this.readPos = (this.readPos + 1) % this.pcm.length;
        }
        await this.src.push(c);
        space -= CHUNK;
      }
    } catch {
      /* a destroy racing the frame loop; the loop stops on its own */
    } finally {
      this._busy = false;
    }
  }

  /** Read the per-channel bus meter, the voice count and the health block. */
  async poll() {
    if (this._pollBusy || !this.engine) return;
    this._pollBusy = true;
    try {
      const r = await this.engine.invokeBuf("get_bus_levels", { out: "f32", len: 26 }, 26);
      this.busLevels = r.out[0];
      this.activeVoices = await this.engine.invoke("get_active_voices");
      this.health = await this.engine.health();
    } catch {
      /* same */
    } finally {
      this._pollBusy = false;
    }
  }

  /** The loudest bus channel and its level, for the meter and for the channel-walk check. */
  loudestChannel() {
    let best = -1;
    let peak = 0;
    for (let i = 0; i < this.channelCount; ++i) {
      if (this.busLevels[i] > peak) { peak = this.busLevels[i]; best = i; }
    }
    return { channel: best, peak };
  }

  async destroy() {
    const e = this.engine;
    this.engine = null;
    this.src = null;
    if (e) await e.destroy();
  }
}
