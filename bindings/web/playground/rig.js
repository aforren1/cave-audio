/**
 * rig.js - the audio half: one engine, one source, the looped stimuli, and the two meters.
 *
 * THE PAGE IS NOT IN THE AUDIO PATH, and that is the whole shape of this file.
 *
 * It used to be. Every stimulus was fed to a PUSH source from a 20 ms timer on the page's main
 * thread, holding about 100 ms of audio ahead of the audio clock, because the binding's advice for
 * "a browser has no synchronous file system" was to decode in JavaScript and push. A main thread
 * stalls, though: a garbage collection, a window drag, a heavy three.js frame on a weak GPU, a tab
 * losing focus. Any stall longer than the queue is a HOLE IN THE SOUND, and the engine counts each
 * one as a push-ring starve. Reported as "the click train becomes inaudible for short periods"
 * (2026-09-22), and headless Chrome on a fast desktop collected 31 to 37 starves in one ordinary
 * check run before anyone stalled anything deliberately - and 45 to 59 more from one 400 ms stall.
 *
 * Every stimulus here is a FIXED BUFFER (see stimulus.js), and the engine loops a sound
 * sample-accurately with no help from anybody. So each one is encoded once as a float32 wav
 * (wav.js), written into the module's own file system with `engine.writeFile`, and loaded with
 * `bwa_load_sound`; one ordinary source then plays it with `loop = true`. A stimulus change is one
 * `bwa_source_play`, which the engine ramps click-free inside a block, so the switch is heard at
 * once rather than after a queue drains. Nothing the page does afterwards can make a gap: the page
 * can stop running altogether and the sound goes on.
 *
 * The push source is still the right answer for audio the page GENERATES or STREAMS as it goes
 * (bindings/web/example/index.html is that demo, and the README documents it). It is the wrong
 * answer for a loop that is known in full before it starts.
 *
 * WHY CAVE_SIM IS THE DEFAULT PROFILE. The playground is the CAVE, auditioned. In
 * `BWA_PROFILE_CAVE_SIM` every point source pans through the real DBAP/SPCAP/VBAP solve into the
 * array bus (one channel per layout speaker), and the bus is then HRTF-decoded to stereo for
 * headphones - so the speaker gizmos light from `bwa_get_bus_levels`, which is the array's own
 * output and not a drawing of one. `BWA_PROFILE_BINAURAL` is offered beside it because it is the
 * other thing the engine does: point voices bypass the panner entirely and get their own HRTF
 * convolution. Switching between them is a CREATE-time change, so it rebuilds the engine.
 *
 * TWO METERS, because the two profiles put the audio in different places.
 *   `busPeaks` is `bwa_get_bus_levels`: the array channels, which is what the speaker cones
 *   draw. It is the LAST BLOCK's peak, so it has to be sampled faster than a block (see
 *   `meterTick`).
 *   `outLevels` is the stereo the AudioContext is actually playing, read through an AnalyserNode
 *   on the sink's own node (`BwaEngine.outputNode`). In BINAURAL the point voices never reach the
 *   bus at all, so this is the only meter with anything in it there.
 */
import { BwaEngine, Profile, MAX_CHANNELS } from "../dist/index.js";
import { SIGNALS } from "./stimulus.js";
import { encodeWavFloat32, foldToMono } from "./wav.js";

const ZERO_PROBE = () => ({ l: 0, r: 0, msL: 0, msR: 0, n: 0 });

/** Where an uploaded layout lands in the module's file system. MEMFS, so it dies with the module. */
export const LAYOUT_PATH = "/cave_layout.json";

/** Where the stimulus wavs land, in the same file system. They die with the module too. */
const STIM_DIR = "/stim";
/** The index the uploaded clip takes in the picker: after every built-in signal. */
const UPLOAD_INDEX = SIGNALS.length;

export class Rig {
  constructor() {
    this.engine = null;
    this.ctx = null;
    this.src = null;
    this.sounds = [];                         /* one bwa_sound per SIGNALS entry, loaded at open */
    this.upload = null;                       /* {name, pcm, rate} an uploaded clip, kept across rebuilds */
    this.uploadSound = 0;
    this.signal = 0;
    this.profile = Profile.CAVE_SIM;
    this.channelCount = 0;
    /* Sized to the CAPACITY, not the active count, so a layout upload never reallocates them.
     * Everything that reads them stops at channelCount / speakerCount. */
    this.speakers = new Float32Array(MAX_CHANNELS * 3);
    this.speakerCount = 0;
    this.busPeaks = new Float32Array(MAX_CHANNELS);   /* peak-held since the last takeBusPeaks() */
    this.lastPeaks = new Float32Array(MAX_CHANNELS);  /* the last set taken, which the page drew */
    this.outLevels = { l: 0, r: 0 };          /* the same, for the stereo the context plays */
    this.outProbe = ZERO_PROBE();             /* the same measurement, on a check's own clock */
    this.activeVoices = 0;
    this.health = null;
    this.layoutBytes = null;                  /* the uploaded layout, kept across rebuilds */
    this.layoutName = null;
    this.module = null;                       /* the wasm module, reused by every rebuild */
    this._pollBusy = false;
    this._meterBusy = false;
    this._tap = null;
    this._tapBuf = null;
  }

  /**
   * Create the engine on an AudioContext the caller already made (and resumed) inside a user
   * gesture.
   * @param {object} opts
   * @param {AudioContext} opts.audioContext
   * @param {number} [opts.profile]
   * @param {number} [opts.blockSize]
   * @param {Uint8Array} [opts.layoutBytes] a cave_layout.json to load instead of the default grid
   * @param {object} [opts.module] an engine module to build on instead of instantiating one
   */
  async open({ audioContext, profile = Profile.CAVE_SIM, blockSize = 256,
               layoutBytes = null, module = null }) {
    this.ctx = audioContext;
    this.profile = profile;
    this.layoutBytes = layoutBytes;
    /* No layoutPath by default. The engine's default grid IS the BWA_DEFAULT_GRID (26) geometry
     * examples/cave_layout.json describes (a 3x3x3 boundary grid at +/-1.5 m, y 0/1.5/3, minus the
     * center), and the page reads the positions back with bwa_get_speakers rather than assuming
     * them - so the gizmos are the engine's layout whatever it turns out to be. An UPLOADED layout
     * takes the other route: its bytes go into the module's file system before bwa_create, because
     * create opens that path inside its own call. */
    this.engine = await BwaEngine.create({
      audioContext,
      profile,
      blockSize,
      module: module ?? undefined,
      layoutPath: layoutBytes ? LAYOUT_PATH : undefined,
      files: layoutBytes ? { [LAYOUT_PATH]: layoutBytes } : undefined,
      /* No `sink`: handing `audioContext` over IS the sink choice. client.js sets
       * SinkType.WORKLET and the "main" topology from it, and takes the engine's sample rate from
       * the context, because only the page may create or resume one. */
    });
    this.module = this.engine.module;
    /* An ORDINARY source, not a push source: the engine owns the samples and the loop. */
    this.src = await this.engine.createSource();
    await this.src.setGain(0.9);
    await this._loadStimuli();
    await this.engine.start();

    this.channelCount = this.engine.info.channelCount;
    const got = await this.engine.invokeBuf("get_speakers", { out: "f32", len: MAX_CHANNELS * 3 },
                                           MAX_CHANNELS);
    this.speakers = got.out[0];
    this.speakerCount = got.value;

    await this.setSignal(this.signal);
    this.busPeaks.fill(0);
    this.lastPeaks.fill(0);
    this.outLevels = { l: 0, r: 0 };
    this.outProbe = ZERO_PROBE();
    return this.engine.info;
  }

  /**
   * Tear the engine down and build another one - a different profile, a different layout, both of
   * which are CREATE-time.
   *
   * THE AUDIOCONTEXT IS REPLACED, and that is the whole fix for a rebuild that came back silent.
   * Emscripten's audio-worklet bootstrap calls `audioWorklet.addModule(bw_audio.mjs)` and that
   * script calls `registerProcessor("em-bootstrap", ...)` in the context's own
   * AudioWorkletGlobalScope. A second engine on the SAME context runs that registration a second
   * time in the same scope, which throws, so the setup chain never finishes, no node is ever
   * connected, and the sink quietly does what rule 4 says to do for a device that is not there: it
   * host-paces silence. The engine goes on rendering (the health block counts blocks, and the old
   * check believed that), but nothing reaches the speakers. A fresh AudioContext is a fresh
   * worklet scope, so the registration is a first one again.
   *
   * The wasm MODULE is reused across the rebuild, which is the other half: instantiating a second
   * 6 MB module and a second pthread pool per switch leaks both, and the dead copy's Workers never
   * exit.
   */
  async rebuild({ profile = this.profile, layoutBytes = this.layoutBytes } = {}) {
    const module = this.module;
    const blockSize = this.engine ? this.engine.info.blockSize : 256;
    const oldCtx = this.ctx;
    this._dropTap();
    await this.destroy();                 /* the engine first: its close takes the node out */
    try { await oldCtx.close(); } catch { /* already closed */ }
    const ctx = new AudioContext({ latencyHint: "interactive" });
    /* Allowed without a gesture of its own: the page has had one (the Start button), and a
     * browser's autoplay gate is per DOCUMENT, not per context. */
    await ctx.resume();
    return this.open({ audioContext: ctx, profile, blockSize, layoutBytes, module });
  }

  /** Rebuild the engine in a different profile, keeping the page's stimulus and layout. */
  async setProfile(profile) {
    if (!this.engine || profile === this.profile) return this.engine.info;
    return this.rebuild({ profile });
  }

  /**
   * Rebuild the engine on an uploaded `cave_layout.json`. `null` goes back to the default grid.
   * The caller validates the JSON first; the ENGINE is still the authority, and a file it rejects
   * leaves a usable engine on the default grid whose `bwa_start` then refuses with BWA_ERR_LAYOUT
   * (CLAUDE.md). That throw is the caller's to report.
   * @param {Uint8Array|null} bytes
   */
  async setLayout(bytes) {
    return this.rebuild({ layoutBytes: bytes });
  }

  /* ---------------------------------------------------------------- the stimuli, as assets */

  /**
   * Encode every stimulus as a wav, write it where the engine can open it, and load it.
   *
   * Once per engine, so a REBUILD reloads them: the sounds belong to the engine that loaded them
   * and a new engine knows nothing of the old handles. The FILES are once per module, because a
   * rebuild reuses the module and MEMFS goes with it - hence the rate check rather than a boolean,
   * since the only thing that could invalidate them is a new AudioContext at another rate.
   *
   * The wavs are at the ENGINE's rate, which is the AudioContext's, so the loader's resampler has
   * nothing to do.
   */
  async _loadStimuli() {
    const rate = this.engine.info.sampleRate;
    const write = this._stimRate !== rate;
    this.sounds = [];
    for (const sig of SIGNALS) {
      const path = `${STIM_DIR}/${sig.id}.wav`;
      if (write) await this.engine.writeFile(path, encodeWavFloat32(sig.make(rate), rate));
      const snd = await this.engine.invoke("load_sound", path);
      if (!snd) throw new Error(`the engine could not load the "${sig.name}" stimulus (${path})`);
      this.sounds.push(snd);
    }
    this._stimRate = rate;
    this.uploadSound = 0;
    if (this.upload) await this._loadUpload();
  }

  /** The uploaded clip, through the same route. Its own path, so a reload replaces the file. */
  async _loadUpload() {
    const path = `${STIM_DIR}/upload.wav`;
    await this.engine.writeFile(path, encodeWavFloat32(this.upload.pcm, this.upload.rate));
    const snd = await this.engine.invoke("load_sound", path);
    if (!snd) throw new Error(`the engine could not load "${this.upload.name}"`);
    const old = this.uploadSound;
    this.uploadSound = snd;
    /* Unload AFTER the replacement is in hand, and after the switch below has played it: an
     * unload is retire-acked internally, so it is safe on a sound that is still sounding, but a
     * gap between the two would be an audible hole for no reason. */
    if (old && old !== snd) {
      if (this.signal === UPLOAD_INDEX) await this.setSignal(UPLOAD_INDEX);
      await this.engine.invoke("unload_sound", old);
    }
  }

  /**
   * Decode an uploaded file with the browser's own decoder, fold it to mono, and make it the
   * stimulus. It survives a rebuild the way an uploaded layout does.
   * @param {ArrayBuffer} bytes the file as it came off the input
   * @param {string} name for the picker
   */
  async setUpload(bytes, name) {
    const buf = await this.ctx.decodeAudioData(bytes);
    this.upload = { name, pcm: foldToMono(buf), rate: Math.round(buf.sampleRate) };
    await this._loadUpload();
    /* _loadUpload already switched if the upload slot was the one playing; a second play here
     * would only restart the clip it just started. */
    if (this.signal !== UPLOAD_INDEX) await this.setSignal(UPLOAD_INDEX);
    return { frames: this.upload.pcm.length, rate: this.upload.rate };
  }

  /** The picker's labels: the built-in signals, and the uploaded clip when there is one. */
  stimulusNames() {
    const names = SIGNALS.map((s) => s.name);
    if (this.upload) names.push(`uploaded: ${this.upload.name}`);
    return names;
  }

  /**
   * Swap the looped stimulus. Index into `stimulusNames()`.
   *
   * ONE `bwa_source_play`, looping. A play on an already-playing source restarts it from frame 0
   * with a one-block gain ramp, so the switch is click-free and lands inside a block - there is no
   * queue of old audio to drain first, which is what a 100 ms feed budget was buying at the price
   * of a 100 ms hole whenever the page stalled for longer.
   */
  async setSignal(i) {
    const snd = i === UPLOAD_INDEX ? this.uploadSound : this.sounds[i];
    if (!snd) return;
    this.signal = i;
    if (!this.engine || !this.src) return;
    await this.src.play(snd, true);
  }

  /**
   * Sample both meters. CALL IT FASTER THAN A BLOCK.
   *
   * `bwa_get_bus_levels` publishes the LAST BLOCK's peak per channel, and a block is 5.3 ms at
   * 48 kHz / 256. The page used to read it on the same 120 ms tick as the health table, which is
   * 22 blocks apart, so it saw one block in 22 and missed the other 21 - and the default stimulus
   * is a click train whose burst is 2 ms in every 250 ms. Measured: 15 of 437 fast samples carried
   * the click, and NONE of the 8 Hz samples did, which is why the meter read "silent" and the
   * cones never lit while the click was plainly audible. Sampling faster than a block is the only
   * way to see every block, so this holds the PEAK since the last read and the frame loop takes
   * it.
   */
  async meterTick() {
    if (this._meterBusy || !this.engine) return;
    this._meterBusy = true;
    try {
      const r = await this.engine.invokeBuf("get_bus_levels", { out: "f32", len: MAX_CHANNELS },
                                             MAX_CHANNELS);
      const v = r.out[0];
      for (let i = 0; i < this.busPeaks.length; ++i) {
        if (v[i] > this.busPeaks[i]) this.busPeaks[i] = v[i];
      }
    } catch {
      /* a destroy racing the meter; the interval is cleared by the page */
    } finally {
      this._meterBusy = false;
    }
    this.readOutput();
  }

  /** The held per-channel peaks, and reset. The caller owns the smoothing (see world.js). */
  takeBusPeaks() {
    this.lastPeaks.set(this.busPeaks);
    this.busPeaks.fill(0);
    return this.lastPeaks;
  }

  /* ---------------------------------------------------------------- the output tap */

  /**
   * Attach an AnalyserNode pair to the sink's own AudioWorkletNode, so the page can meter what the
   * AudioContext is really playing. Additive: the sink's connection to the destination is left
   * alone, and the analysers are parked on a silent gain so the graph pulls them.
   *
   * It is the only honest meter in the BINAURAL profile, where point voices bypass the 26-channel
   * bus entirely and `bwa_get_bus_levels` therefore reads silence however loud the headphones are.
   * @returns {boolean} true once the tap is up; false while the sink's async setup is still going
   */
  attachTap() {
    if (this._tap || !this.engine || !this.ctx) return !!this._tap;
    const node = this.engine.outputNode();
    if (!node) return false;
    const splitter = this.ctx.createChannelSplitter(2);
    const mk = () => {
      const a = this.ctx.createAnalyser();
      /* 512 samples is 10.7 ms at 48 kHz: longer than the 4 ms meter tick, so no sample of the
       * output is ever missed, and short enough that "when did the sound change" is answerable to
       * about a block. A longer window would smear a click across 40 ms and turn a latency
       * measurement into a measurement of the window. */
      a.fftSize = 512;
      a.smoothingTimeConstant = 0;           /* a meter wants the samples, not a smoothed spectrum */
      return a;
    };
    const l = mk();
    const r = mk();
    const mute = this.ctx.createGain();
    mute.gain.value = 0;
    node.connect(splitter);
    splitter.connect(l, 0);
    splitter.connect(r, 1);
    l.connect(mute);
    r.connect(mute);
    mute.connect(this.ctx.destination);
    this._tap = { node, splitter, l, r, mute };
    this._tapBuf = new Float32Array(l.fftSize);
    return true;
  }

  _dropTap() {
    if (!this._tap) return;
    for (const n of [this._tap.splitter, this._tap.l, this._tap.r, this._tap.mute]) {
      try { n.disconnect(); } catch { /* the context may already be closed */ }
    }
    this._tap = null;
  }

  /** Peak-hold the two output channels. Called from meterTick, cleared by the two takers. */
  readOutput() {
    if (!this._tap && !this.attachTap()) return;
    const buf = this._tapBuf;
    const measure = (a) => {
      a.getFloatTimeDomainData(buf);
      let m = 0;
      let sq = 0;
      for (let i = 0; i < buf.length; ++i) {
        const v = buf[i];
        sq += v * v;
        const av = v < 0 ? -v : v;
        if (av > m) m = av;
      }
      return { peak: m, ms: sq / buf.length };
    };
    try {
      const l = measure(this._tap.l);
      const r = measure(this._tap.r);
      /* TWO accumulators for one measurement, because they have two consumers with their own
       * clocks: the meter strip clears its copy every animation frame, and a check that polls the
       * other one would otherwise get only whatever landed since the last repaint. A destructive
       * read cannot be shared. */
      this.outLevels = { l: Math.max(this.outLevels.l, l.peak), r: Math.max(this.outLevels.r, r.peak) };
      const p = this.outProbe;
      /* Peak AND mean square: a peak answers "did anything come out", and an energy ratio is what
       * a laterality claim needs - one loud sample in the wrong ear is not an image. */
      this.outProbe = {
        l: Math.max(p.l, l.peak), r: Math.max(p.r, r.peak),
        msL: p.msL + l.ms, msR: p.msR + r.ms, n: p.n + 1,
      };
    } catch { /* the context went away under us */ }
  }

  /** The UI's copy of the output peaks, and reset. */
  takeOutLevels() {
    const v = this.outLevels;
    this.outLevels = { l: 0, r: 0 };
    return v;
  }

  /**
   * The same measurement on its own accumulator, for a check that samples at its own rate:
   * `{l, r}` peaks plus `{rmsL, rmsR}` over everything since the last call.
   */
  takeOutProbe() {
    const v = this.outProbe;
    this.outProbe = ZERO_PROBE();
    return {
      l: v.l, r: v.r, samples: v.n,
      rmsL: v.n ? Math.sqrt(v.msL / v.n) : 0,
      rmsR: v.n ? Math.sqrt(v.msR / v.n) : 0,
    };
  }

  /** Read the voice count and the health block. The meters are on their own, faster, tick. */
  async poll() {
    if (this._pollBusy || !this.engine) return;
    this._pollBusy = true;
    try {
      this.activeVoices = await this.engine.invoke("get_active_voices");
      this.health = await this.engine.health();
    } catch {
      /* same */
    } finally {
      this._pollBusy = false;
    }
  }

  /** The loudest bus channel and its level, for the meter and for the channel-walk check. */
  loudestChannel(peaks = this.lastPeaks) {
    let best = -1;
    let peak = 0;
    for (let i = 0; i < this.channelCount; ++i) {
      if (peaks[i] > peak) { peak = peaks[i]; best = i; }
    }
    return { channel: best, peak };
  }

  async destroy() {
    const e = this.engine;
    this.engine = null;
    this.src = null;
    this._dropTap();
    if (e) await e.destroy();
  }
}
