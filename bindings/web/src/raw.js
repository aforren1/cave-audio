/**
 * raw.js - the RAW layer: the C ABI, one call for one, over an instantiated engine module.
 *
 * Same split bindings/python has, and for the same reason. This layer renames nothing and
 * re-units nothing: a call is its C name minus the `bwa_` prefix, the arguments are the header's
 * arguments in the header's order and units, and the return is the C return. Read
 * `include/bw_audio.h` or `docs/api.md` and you are reading this layer. The table it is built
 * from is GENERATED out of that header by `tools/wasm/gen-abi.mjs`, so a call cannot drift.
 *
 * WHERE IT RUNS. Wherever the wasm module is, which in the shipping shape is the control Worker
 * (see docs/web.md and `client.js`). It is synchronous and has no thread guard of its own - the
 * one-control-thread rule is structural here, because the engine pointer only exists inside the
 * Worker that made it.
 *
 * WHAT IT DOES NOT CARRY. `bwa_set_output_capture`, deliberately: its callback runs on the audio
 * thread and a JS callback must never run there. The manual sink plus `render_block` is the
 * offline path, exactly as in the Python and MATLAB bindings. The generator drops it, so it is
 * absent rather than present-and-broken.
 *
 * FOUR MARSHALLING CLASSES, no more (see the generator for how each is decided):
 *   num   a plain JS number. i32, f32, f64, an enum, a bool - the wasm signature already says
 *         which, so nothing here has to. `true` and `false` are accepted for a bool.
 *   i64   crosses as a BigInt, because emcc defaults to -sWASM_BIGINT. Converted both ways here,
 *         so a caller passes and receives ordinary numbers. Frame counts stay exact to 2^53,
 *         which is 5800 years at 48 kHz.
 *   cstr  an argument is copied onto the WASM STACK for the duration of the call (no malloc, no
 *         free, no leak on throw); a return is read with UTF8ToString and may be null.
 *   ptr   a wasm heap address the caller supplies. Use `alloc`/`free` and the typed-array views
 *         below; the views are GETTERS because -sALLOW_MEMORY_GROWTH detaches and replaces them
 *         whenever the heap grows, and a cached view would silently read a dead buffer.
 */
import { ABI, ABI_VERSION } from "./abi.js";

export { ABI, ABI_VERSION };

function wrap(M, f) {
  const fn = M["_" + f.name];
  if (typeof fn !== "function") return null;
  const args = f.args;
  const n = args.length;
  const ret = f.ret;
  const needsStack = args.includes("cstr");
  return function (...a) {
    const sp = needsStack ? M.stackSave() : 0;
    try {
      const c = new Array(n);
      for (let i = 0; i < n; ++i) {
        const t = args[i];
        const v = a[i];
        if (t === "cstr") {
          if (v === null || v === undefined) { c[i] = 0; continue; }
          const bytes = M.lengthBytesUTF8(v) + 1;
          const p = M.stackAlloc(bytes);
          M.stringToUTF8(v, p, bytes);
          c[i] = p;
        } else if (t === "i64") {
          c[i] = BigInt(v === undefined || v === null ? 0 : v);
        } else if (typeof v === "boolean") {
          c[i] = v ? 1 : 0;
        } else {
          c[i] = v === undefined || v === null ? 0 : v;
        }
      }
      const r = fn(...c);
      if (ret === "void") return undefined;
      if (ret === "i64") return Number(r);
      if (ret === "cstr") return r ? M.UTF8ToString(r) : null;
      return r;
    } finally {
      if (needsStack) M.stackRestore(sp);
    }
  };
}

/**
 * Build the raw layer over an instantiated Emscripten module.
 *
 * Throws when the module is missing a symbol the header declares, because the alternative is a
 * binding that looks complete and fails at the one call a page needed. `-sEXPORTED_FUNCTIONS`
 * comes from the same generated table, so a mismatch means the module was linked from a
 * different tree.
 *
 * @param {object} M an instantiated Emscripten module
 * @returns {object} the raw layer: one function per C entry point, C name minus `bwa_`
 */
export function makeRaw(M) {
  const raw = Object.create(null);
  const missing = [];
  for (const f of ABI) {
    const w = wrap(M, f);
    if (!w) { missing.push(f.name); continue; }
    raw[f.name.slice(4)] = w;
  }
  if (missing.length) {
    throw new Error(
      "bw_audio: the wasm module does not export " + missing.length + " of the " + ABI.length +
      " calls include/bw_audio.h declares (first: " + missing[0] + "). The module and this " +
      "binding came from different trees; rebuild both with tools/wasm/build-web.sh."
    );
  }

  /* The heap seam. Everything here is deliberately NOT a bwa_ call, so a reader can tell the ABI
   * from the plumbing at a glance. */
  raw.module = M;
  raw.alloc = (bytes) => M._malloc(bytes);
  raw.free = (ptr) => M._free(ptr);
  raw.readString = (ptr) => (ptr ? M.UTF8ToString(ptr) : null);
  Object.defineProperties(raw, {
    u8: { get: () => M.HEAPU8 },
    i32: { get: () => M.HEAP32 },
    u32: { get: () => M.HEAPU32 },
    f32: { get: () => M.HEAPF32 },
    f64: { get: () => M.HEAPF64 },
  });

  /* The same import-time guard the Python layer runs, and for the same reason: the generated
   * table was written from one header and the module was linked from another tree's. */
  const lib = raw.get_version();
  const want = (ABI_VERSION[0] << 16) | (ABI_VERSION[1] << 8) | ABI_VERSION[2];
  if (lib !== want) {
    const fmt = (v) => `${(v >> 16) & 0xff}.${(v >> 8) & 0xff}.${v & 0xff}`;
    throw new Error(
      `bw_audio ABI mismatch: this binding was generated against ${fmt(want)} but the engine ` +
      `module reports ${fmt(lib)}. They are from different builds.`
    );
  }
  return raw;
}
