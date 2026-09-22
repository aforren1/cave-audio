/**
 * host.js - the control-side dispatcher: one engine, one thread, one op table.
 *
 * It is deliberately transport-free. `worker.js` wraps it in a module Worker and speaks
 * `postMessage`; `client.js` can also drive it in place on the page's main thread for the one
 * topology that needs that (see the README's "Two topologies" and `client.js`). Keeping the
 * dispatch here rather than inside the Worker is what makes the two topologies the SAME code
 * rather than two implementations that agree today.
 *
 * Whichever way it is driven, every `bwa_*` call in the process goes through this object, on the
 * thread that constructed it. That IS invariant 2: there is no second caller, because the engine
 * pointer exists nowhere else.
 */
import { Engine, BwaError } from "./engine.js";
import { makeRaw } from "./raw.js";
import {
  FLAG_POSE, OPS, SLAB_COUNT, SLAB_FLAGS, SLAB_HEADER, SLAB_POSE, SLAB_SEQ, SLAB_STRIDE,
} from "./protocol.js";

export class Host {
  constructor() {
    this.raw = null;
    this.engine = null;
    this.u32 = null;
    this.f32 = null;
    this.lastSeq = -1;
    this.handles = new Map();     /* handle -> the idiomatic object, so `push` finds its source */
  }

  /**
   * @param {object} msg the INIT payload: { moduleFactory | wasmUrl, engine, slab }
   */
  async init(msg) {
    const factory = msg.moduleFactory ?? (await import(/* @vite-ignore */ msg.wasmUrl)).default;
    const M = await factory(msg.moduleArgs ?? {});
    this.raw = makeRaw(M);
    this.engine = new Engine(this.raw, msg.engine ?? {});
    if (msg.slab) {
      this.u32 = new Uint32Array(msg.slab);
      this.f32 = new Float32Array(msg.slab);
    }
    return this.info();
  }

  info() {
    const e = this.engine;
    return {
      sampleRate: e.sampleRate,
      blockSize: e.blockSize,
      channelCount: e.channelCount,
      backend: e.backend,
      sinkType: e.sinkType,
      outputLatencyFrames: e.outputLatencyFrames,
      lastError: e.lastError,
      abi: this.raw.get_version(),
    };
  }

  /** One frame: the pose and every source position land together, under one commit. */
  applyFrame() {
    const u32 = this.u32;
    if (!u32) return;
    const seq = Atomics.load(u32, SLAB_SEQ);
    if (seq === this.lastSeq) return;      /* a burst collapses to its newest state */
    this.lastSeq = seq;
    const flags = u32[SLAB_FLAGS];
    const n = u32[SLAB_COUNT];
    const f32 = this.f32;
    const e = this.engine;
    const raw = this.raw;
    const ptr = e.ptr;
    e.frame(() => {
      if (flags & FLAG_POSE) {
        raw.set_listener_pose(ptr, f32[SLAB_POSE], f32[SLAB_POSE + 1], f32[SLAB_POSE + 2],
                              f32[SLAB_POSE + 3], f32[SLAB_POSE + 4], f32[SLAB_POSE + 5],
                              f32[SLAB_POSE + 6]);
      }
      for (let i = 0; i < n; ++i) {
        const o = SLAB_HEADER + i * SLAB_STRIDE;
        raw.source_set_pos(ptr, u32[o], f32[o + 1], f32[o + 2], f32[o + 3]);
      }
      e._dirty = true;
    });
  }

  /**
   * @param {string} op one of protocol.js's OPS
   * @param {object} msg the payload
   */
  handle(op, msg) {
    const e = this.engine;
    switch (op) {
      case OPS.CREATE: {
        const o = msg.kind === "push" ? e.createPushSource()
                : msg.kind === "bed" ? e.createBed()
                : e.createSource();
        this.handles.set(o.handle, o);
        return o.handle;
      }
      case OPS.PUSH: {
        const o = this.handles.get(msg.handle);
        if (!o || typeof o.push !== "function")
          throw new BwaError(`handle ${msg.handle} is not a push source`);
        return o.push(msg.data);
      }
      case OPS.HEALTH: return e.health();
      case OPS.INFO: return this.info();
      case OPS.INVOKE: return e.invoke(msg.name, ...(msg.args || []));
      case OPS.RAW: {
        const fn = this.raw[msg.name];
        if (!fn) throw new BwaError(`bw_audio has no call named bwa_${msg.name}`);
        return fn(...(msg.args || []));
      }
      case OPS.START: e.start(); return this.info();
      case OPS.STOP: e.stop(); return null;
      case OPS.DESTROY: {
        e.destroy();
        this.handles.clear();
        return null;
      }
      case OPS.RENDER: return this.renderBlock();
      case OPS.FRAME: this.applyFrame(); return null;
      default: throw new BwaError(`unknown op '${op}'`);
    }
  }

  /**
   * Manual sink only: pump one block and hand back a COPY of the planar bus.
   *
   * A copy and not a view, unlike the Python binding's zero-copy numpy view, for one reason: the
   * buffer belongs to the engine and lives until the next call, and a structured clone across a
   * postMessage would copy it anyway. In the in-page topology the copy is the only thing keeping
   * a caller from holding a pointer into the engine's own memory past its lifetime.
   */
  renderBlock() {
    if (!this._rbScratch) this._rbScratch = this.raw.alloc(8);
    const chp = this._rbScratch;
    const nfp = this._rbScratch + 4;
    const bus = this.raw.render_block(this.engine.ptr, chp, nfp);
    if (!bus) return null;
    const channels = this.raw.u32[chp >> 2];
    const nframes = this.raw.u32[nfp >> 2];
    const base = bus >> 2;
    return { channels, nframes, data: this.raw.f32.slice(base, base + channels * nframes) };
  }
}
