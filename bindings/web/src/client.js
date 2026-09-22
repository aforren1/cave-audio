/**
 * client.js - what a PAGE imports. The public surface of this binding.
 *
 * It is a facade over the control side (`host.js`), not a third layer: every method here either
 * posts one protocol message or writes the frame slab. The two layers proper are `raw.js` (the C
 * ABI, one for one, generated from the header) and `engine.js` (the idiomatic one, with the
 * auto-commit model the Python binding has). Both of those live on the control thread; this is how
 * a page reaches them.
 *
 * TWO TOPOLOGIES, and the difference is measured rather than stylistic.
 *
 *   "worker" (the DEFAULT, and what docs/web.md's decision requires). The wasm module and the
 *     engine live in a dedicated module Worker. Every `bwa_*` call runs there, so invariant 2 is
 *     structural, and the control side may BLOCK - which it must, because `bwa_destroy` joins
 *     threads and the asset loader parks on an `os_event`, and `Atomics.wait` is forbidden on a
 *     browser's main thread. Every sink works here EXCEPT the AudioWorklet one.
 *
 *   "main". The module and the engine live on the page's main thread. This is the ONLY topology
 *     in which `SinkType.WORKLET` can open, and the reason is a property of Emscripten rather than
 *     of this binding: `emscripten_create_audio_context` runs `new AudioContext()`, which needs a
 *     Window and throws in a Worker, and `libwebaudio.js` keeps its handle table in an ordinary
 *     per-scope JS variable, so a handle minted on the page means nothing in a Worker and
 *     emscripten proxies none of it. So an audible page pays for its sound with the Worker control
 *     thread, today. What it costs: `destroy()` joins threads on the main thread (Emscripten
 *     busy-waits and pumps its queue, so it survives, but it is a stall), and a future streaming
 *     or async-asset page would hit the `idle` failure docs/web.md measured. Use "worker" for
 *     anything offline, measured, or headless, and "main" for a demo that has to make noise.
 *     `worklet_sink.c` already proxies its Web Audio work to the main runtime thread, so the day
 *     Emscripten's handle table becomes reachable across threads this topology stops being needed
 *     and nothing in the C has to change.
 *
 * ASYNC is the one genuinely new surface, as docs/web.md predicted: fetching the wasm, spawning
 * the Worker and resuming an AudioContext are all promises. `BwaEngine.create()` owns all of it
 * and everything after it is a promise or a free slab write.
 */
import { FLAG_POSE, OPS, SLAB_COUNT, SLAB_FLAGS, SLAB_HEADER, SLAB_POSE, SLAB_SEQ, SLAB_STRIDE, slabBytes } from "./protocol.js";

export { Profile, SinkType, SinkFlags, BwaError } from "./engine.js";

const DEFAULT_MAX_SOURCES = 256;

class ClientHandle {
  constructor(engine, handle) {
    this.engine = engine;
    this.handle = handle;
  }
  valueOf() { return this.handle; }
}

/**
 * A source, page side. Position is STAGED (free, no message) and lands on the next
 * `engine.pushFrame()`; everything else is a promise, because it crosses to the control thread.
 */
export class ClientSource extends ClientHandle {
  constructor(engine, handle) {
    super(engine, handle);
    this.x = 0; this.y = 0; this.z = 0;
    this._dirty = false;
    this._alive = true;
  }
  /** Free: writes three numbers. Nothing crosses a thread until `pushFrame()`. */
  setPosition(x, y, z) {
    this.x = x; this.y = y; this.z = z;
    this._dirty = true;
  }
  play(sound, loop = false) { return this.engine.invoke("source_play", this.handle, +sound, loop); }
  stop() { return this.engine.invoke("source_stop", this.handle); }
  setGain(linear) { return this.engine.invoke("source_set_gain", this.handle, linear); }
  setSpread(amount) { return this.engine.invoke("source_set_spread", this.handle, amount); }
  setSize(radiusM) { return this.engine.invoke("source_set_size", this.handle, radiusM); }
  setPaused(on) { return this.engine.invoke("source_set_paused", this.handle, on); }
  isPlaying() { return this.engine.invoke("source_is_playing", this.handle); }
  async destroy() {
    this._alive = false;
    this.engine._sources.delete(this.handle);
    await this.engine.invoke("source_destroy", this.handle);
  }
}

export class ClientPushSource extends ClientSource {
  /**
   * Push mono float frames. The array is COPIED across the thread boundary (a structured clone),
   * which is the honest cost of a control thread that is a Worker; at 48 kHz a 20 ms chunk is 960
   * floats and the clone is not the expensive part of the frame.
   * @param {Float32Array} frames
   * @returns {Promise<number>} frames accepted
   */
  push(frames) { return this.engine._call(OPS.PUSH, { handle: this.handle, data: frames }); }
  pushEnd() { return this.engine.invoke("source_push_end", this.handle); }
  space() { return this.engine.invoke("source_push_space", this.handle); }
}

export class ClientListener {
  constructor(engine) {
    this.engine = engine;
    this.pose = new Float32Array([0, 0, 0, 0, 0, 0, 1]);
    this._dirty = false;
  }
  /** Free, staged, commit-gated: lands with the rest of the frame at `pushFrame()`. */
  setPose(px, py, pz, qx, qy, qz, qw) {
    const p = this.pose;
    p[0] = px; p[1] = py; p[2] = pz; p[3] = qx; p[4] = qy; p[5] = qz; p[6] = qw;
    this._dirty = true;
  }
  setPosePredictionSeconds(leadS) {
    return this.engine.invoke("set_pose_prediction", leadS);
  }
}

export class BwaEngine {
  /**
   * @param {object} opts
   * @param {string} [opts.wasmUrl] the engine module, default `./bw_audio.mjs` beside this file
   * @param {"worker"|"main"} [opts.topology] see the file header. Default "worker".
   * @param {AudioContext} [opts.audioContext] "main" topology only: the context the PAGE created
   *        inside its user-gesture handler. Supplying it selects `SinkType.WORKLET`, takes the
   *        engine's sample rate from it, and leaves the page owning the resume.
   * @param {number} [opts.maxSources] slab capacity, default 256
   * @param {number} [opts.profile] bwa_desc.profile
   * @param {number} [opts.sampleRate] Hz; 0 or absent = the engine default (48000)
   * @param {number} [opts.blockSize] frames; 0 or absent = the engine default (256)
   * @param {number} [opts.sink] bwa_desc.sink
   * @param {string} [opts.device] bwa_desc.device
   * @param {number} [opts.sinkFlags] BWA_SINK_FLAG_*
   * @param {string} [opts.layoutPath] cave_layout.json inside the wasm file system
   * @param {string} [opts.hrtfPath] a SOFA file inside the wasm file system
   * @returns {Promise<BwaEngine>}
   */
  static async create(opts = {}) {
    if (typeof SharedArrayBuffer === "undefined" || !globalThis.crossOriginIsolated) {
      /* Loudly, and with no fallback: docs/web.md's "Hosting" says a page that silently drops to a
       * single-threaded shape looks like the real thing and is not. Node has SharedArrayBuffer and
       * no crossOriginIsolated, so the check accepts a missing flag only when SAB exists. */
      if (typeof SharedArrayBuffer === "undefined") {
        throw new Error(
          "bw_audio: SharedArrayBuffer is not available, so the engine's two-thread model cannot " +
          "run. Serve this page cross-origin isolated: Cross-Origin-Opener-Policy: same-origin " +
          "and Cross-Origin-Embedder-Policy: require-corp (see bindings/web/example/serve.mjs)."
        );
      }
    }
    const e = new BwaEngine();
    await e._init(opts);
    return e;
  }

  constructor() {
    this._next = 1;
    this._pending = new Map();
    this._sources = new Map();
    this.listener = new ClientListener(this);
    this.info = null;
    this.audioContext = null;
  }

  async _init(opts) {
    const wasmUrl = opts.wasmUrl ?? new URL("./bw_audio.mjs", import.meta.url).href;
    const maxSources = opts.maxSources ?? DEFAULT_MAX_SOURCES;
    this._maxSources = maxSources;
    const slab = new SharedArrayBuffer(slabBytes(maxSources));
    this._u32 = new Uint32Array(slab);
    this._f32 = new Float32Array(slab);
    this._seq = 0;

    const engineOpts = {
      profile: opts.profile ?? 1,
      sampleRate: opts.sampleRate ?? 0,
      blockSize: opts.blockSize ?? 0,
      sink: opts.sink ?? 0,
      device: opts.device ?? null,
      sinkFlags: opts.sinkFlags ?? 0,
      layoutPath: opts.layoutPath ?? null,
      hrtfPath: opts.hrtfPath ?? null,
      bedDecoder: opts.bedDecoder ?? 0,
      enablePathing: opts.enablePathing ?? false,
    };

    const topology = opts.topology ?? (opts.audioContext ? "main" : "worker");
    this.topology = topology;

    if (topology === "main") {
      const factory = (await import(/* @vite-ignore */ wasmUrl)).default;
      const M = await factory({});
      this._module = M;
      if (opts.audioContext) {
        if (typeof M.emscriptenRegisterAudioObject !== "function") {
          throw new Error(
            "bw_audio: this module was linked without -sAUDIO_WORKLET, so it has no Web Audio " +
            "backend. Rebuild with tools/wasm/build-web.sh, which sets BWA_WITH_WORKLET=ON."
          );
        }
        this.audioContext = opts.audioContext;
        engineOpts.sink = 9;                                   /* SinkType.WORKLET */
        engineOpts.device = String(M.emscriptenRegisterAudioObject(opts.audioContext));
        engineOpts.sampleRate = Math.round(opts.audioContext.sampleRate);
      }
      const { Host } = await import("./host.js");
      this._host = new Host();
      this.info = await this._host.init({ moduleFactory: async () => M, engine: engineOpts, slab });
      return;
    }

    this._worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
    this._worker.onmessage = (ev) => {
      const { id, ok, value, error, result } = ev.data;
      const p = this._pending.get(id);
      if (!p) return;
      this._pending.delete(id);
      if (ok) p.resolve(value);
      else {
        const err = new Error(error);
        err.result = result;
        p.reject(err);
      }
    };
    this.info = await this._call(OPS.INIT, { wasmUrl, engine: engineOpts, slab });
  }

  _call(op, msg = {}) {
    if (this._host) {
      return new Promise((resolve, reject) => {
        try { resolve(this._host.handle(op, msg)); } catch (e) { reject(e); }
      });
    }
    const id = this._next++;
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      this._worker.postMessage({ ...msg, op, id });
    });
  }

  /* --- the per-frame path: one slab write, one message, one commit --- */

  /**
   * Publish this frame's staged pose and source positions. ONE message whatever the source count,
   * and no allocation: the slab is a SharedArrayBuffer written in place. Call it once per visual
   * frame, after your `setPose`/`setPosition` calls.
   */
  pushFrame() {
    const u32 = this._u32;
    const f32 = this._f32;
    let flags = 0;
    if (this.listener._dirty) {
      const p = this.listener.pose;
      for (let i = 0; i < 7; ++i) f32[SLAB_POSE + i] = p[i];
      this.listener._dirty = false;
      flags |= FLAG_POSE;
    }
    let n = 0;
    for (const s of this._sources.values()) {
      if (!s._dirty) continue;
      if (n >= this._maxSources) break;         /* the rest go next frame; nothing is lost */
      const o = SLAB_HEADER + n * SLAB_STRIDE;
      u32[o] = s.handle;
      f32[o + 1] = s.x; f32[o + 2] = s.y; f32[o + 3] = s.z;
      s._dirty = false;
      n++;
    }
    if (!flags && n === 0) return;              /* nothing changed: do not wake the control thread */
    u32[SLAB_FLAGS] = flags;
    u32[SLAB_COUNT] = n;
    /* The seq goes LAST and atomically: the control side reads it first and treats everything
     * before it as published. Same publish-then-flag ordering rule the engine's own live knobs
     * use (CLAUDE.md), read in the other order. */
    Atomics.store(u32, SLAB_SEQ, ++this._seq);
    if (this._host) this._host.applyFrame();
    else this._worker.postMessage({ op: OPS.FRAME });
  }

  /* --- everything else --- */

  /** Any of the engine-taking raw calls, by C name minus `bwa_`. See bindings/web/README.md. */
  invoke(name, ...args) { return this._call(OPS.INVOKE, { name, args }); }
  /**
   * The same, for the calls whose arguments are POINTERS: an argument may be `{f32|i32|u32: [...]}`
   * to copy an array in, or `{out: "f32"|"i32"|"u32", len: n}` to read one back. With any `out` the
   * result is `{value, out: [TypedArray, ...]}`; with none it is the call's own return. This is how
   * a page reaches `get_bus_levels`, `get_speakers`, the mesh calls and the 3-band occlusion tilt.
   * It does not build structs - see `host.js`'s `invokeBuf` for why.
   * @returns {Promise<*>}
   */
  invokeBuf(name, ...args) { return this._call(OPS.INVOKE_BUF, { name, args }); }
  /** Any raw call that takes NO engine pointer: `host_time_ns`, `get_device_count`, ... */
  raw(name, ...args) { return this._call(OPS.RAW, { name, args }); }

  async createSource() {
    const h = await this._call(OPS.CREATE, { kind: "source" });
    const s = new ClientSource(this, h);
    this._sources.set(h, s);
    return s;
  }
  async createPushSource() {
    const h = await this._call(OPS.CREATE, { kind: "push" });
    const s = new ClientPushSource(this, h);
    this._sources.set(h, s);
    return s;
  }
  async createBed() {
    const h = await this._call(OPS.CREATE, { kind: "bed" });
    return new ClientHandle(this, h);
  }

  async start() { this.info = await this._call(OPS.START); return this.info; }
  stop() { return this._call(OPS.STOP); }
  health() { return this._call(OPS.HEALTH); }
  refreshInfo() { return this._call(OPS.INFO).then((i) => (this.info = i)); }
  /** Manual sink only: one block, planar, as a copy. `{channels, nframes, data}` or null. */
  renderBlock() { return this._call(OPS.RENDER); }
  setMasterGain(linear) { return this.invoke("set_master_gain", linear); }

  async destroy() {
    await this._call(OPS.DESTROY);
    if (this._worker) { this._worker.terminate(); this._worker = null; }
    this._host = null;
  }
}
