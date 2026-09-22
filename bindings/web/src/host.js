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
   * @param {object} msg the INIT payload: { moduleFactory | wasmUrl, engine, slab, files }
   */
  async init(msg) {
    const factory = msg.moduleFactory ?? (await import(/* @vite-ignore */ msg.wasmUrl)).default;
    const M = await factory(msg.moduleArgs ?? {});
    this.raw = makeRaw(M);
    /* BEFORE the engine, not after: bwa_create OPENS bwa_desc.layout_path and bwa_desc.hrtf_path
     * inside that one call, so a file a caller means the engine to read has to already be in the
     * module's file system. That is what `files` is for, and why a later writeFile cannot serve a
     * layout. */
    for (const [path, data] of Object.entries(msg.files ?? {})) this.writeFile(path, data);
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
      case OPS.INVOKE_BUF: return this.invokeBuf(msg.name, msg.args || []);
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
      case OPS.WRITE_FILE: return this.writeFile(msg.path, msg.data);
      default: throw new BwaError(`unknown op '${op}'`);
    }
  }

  /**
   * `invoke`, for the raw calls whose arguments are POINTERS.
   *
   * WHY IT EXISTS. `invoke` marshals scalars, which is most of the ABI, and a page therefore could
   * not reach the calls a VISUAL client needs most: `bwa_get_bus_levels` and `bwa_get_speakers`
   * fill a float array, `bwa_scene_set_mesh_mat` takes two, `bwa_source_set_occlusion_manual`
   * takes a 3-band tilt. A page cannot allocate wasm heap itself - the module lives on the control
   * thread, and in the default topology that is another thread entirely - so the alloc, the copy
   * in, the copy out and the free all have to happen here, around the one call.
   *
   * WHAT IT DOES NOT DO, deliberately: it does not lay out STRUCTS. JS has no offsetof, so a
   * caller building a `bwa_fdn_desc` by hand would hard-code field offsets a header change could
   * move without a word - the exact reason `src/bwa_web.c` exists. An array of one scalar type is
   * a different thing: `float[3]` is three floats in every ABI. A struct-taking call needs a
   * field-by-field wrapper in bwa_web.c, not a byte buffer from here.
   *
   * An argument is a plain number, or one of:
   *   { f32: [...] } | { i32: [...] } | { u32: [...] }   copied IN, passed as a pointer
   *   { out: "f32"|"i32"|"u32", len: n }                 zeroed, passed in, read back OUT
   *
   * @param {string} name the C name minus `bwa_`
   * @param {Array} args
   * @returns {*} the call's own return when there is no `out` argument, else
   *   `{ value, out: [TypedArray, ...] }` with one entry per `out`, in argument order.
   */
  invokeBuf(name, args = []) {
    const raw = this.raw;
    const temps = [];
    const outs = [];
    const call = new Array(args.length);
    try {
      for (let i = 0; i < args.length; ++i) {
        const a = args[i];
        if (a === null || a === undefined || typeof a !== "object") { call[i] = a; continue; }
        const kind = a.out || ("f32" in a ? "f32" : "i32" in a ? "i32" : "u32" in a ? "u32" : null);
        if (kind !== "f32" && kind !== "i32" && kind !== "u32")
          throw new BwaError(`invokeBuf("${name}"): argument ${i} is neither a number nor a ` +
                             `{f32|i32|u32} buffer nor an {out, len} readback`);
        const src = a.out ? null : a[kind];
        const len = a.out ? (a.len | 0) : src.length;
        if (!(len > 0)) throw new BwaError(`invokeBuf("${name}"): argument ${i} has no length`);
        const ptr = raw.alloc(len * 4);
        if (!ptr) throw new BwaError(`invokeBuf("${name}"): out of wasm heap for argument ${i}`);
        temps.push(ptr);
        /* raw.f32 and friends are GETTERS, re-read here on purpose: -sALLOW_MEMORY_GROWTH
         * detaches and replaces the views whenever the heap grows, and the alloc above is
         * exactly where that happens. */
        if (src) raw[kind].set(src, ptr >> 2);
        else raw.u8.fill(0, ptr, ptr + len * 4);
        if (a.out) outs.push({ ptr, len, kind });
        call[i] = ptr;
      }
      const value = this.engine.invoke(name, ...call);
      if (!outs.length) return value;
      /* A copy, because the heap under it is freed in the `finally` below and, in the default
       * topology, would be a structured clone across the Worker boundary anyway. */
      const out = outs.map((o) => raw[o.kind].slice(o.ptr >> 2, (o.ptr >> 2) + o.len));
      return { value, out };
    } finally {
      for (const ptr of temps) raw.free(ptr);
    }
  }

  /**
   * Put a file in the module's file system, where the engine can open it.
   *
   * WHY IT EXISTS. `bwa_desc.layout_path` and `bwa_desc.hrtf_path` are PATHS, and the engine opens
   * them with an ordinary fopen. A browser has no synchronous file system for that fopen to reach,
   * so the only place such a path can point is MEMFS, the module's own in-memory one. A page gets
   * bytes from a `<input type="file">`, a fetch or a string; this is where those bytes become a
   * path. It is deliberately NOT how audio gets in - see the README's "Where the audio comes from":
   * a wav belongs in a push source through the browser's own decoder.
   *
   * MEMFS is per MODULE INSTANCE and lives in its heap, so a file written here is gone when the
   * module goes and costs its own size in wasm memory. Write the layout, not the sample library.
   *
   * @param {string} path an absolute path inside the module file system, "/cave_layout.json"
   * @param {Uint8Array|ArrayBuffer} data the file's bytes
   * @returns {number} bytes written
   */
  writeFile(path, data) {
    const FS = this.raw.module.FS;
    if (!FS) {
      throw new BwaError(
        "bw_audio: this module was linked without FS in EXPORTED_RUNTIME_METHODS, so nothing can " +
        "put a file where the engine can open it. Rebuild with tools/wasm/build-web.sh."
      );
    }
    if (typeof path !== "string" || !path) throw new BwaError("writeFile: path must be a string");
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    const cut = path.lastIndexOf("/");
    if (cut > 0) {
      /* A directory that already exists throws EEXIST rather than returning, and a path whose
       * parent is there is the common case. */
      try { FS.mkdirTree(path.slice(0, cut)); } catch { /* already there */ }
    }
    FS.writeFile(path, bytes);
    return bytes.length;
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
