/**
 * protocol.js - the page/control-Worker contract, in one file so both halves cannot disagree.
 *
 * TWO CHANNELS, and the split is the whole design.
 *
 * 1. `postMessage` for everything that happens once: create the engine, load an asset, make a
 *    source, start, stop, read the health struct. Request and reply carry an id; the client
 *    returns a Promise.
 *
 * 2. A SHARED FRAME SLAB for everything that happens every frame: the listener pose and N source
 *    positions. The page writes it directly and posts ONE tiny message per frame. Why not a
 *    message per call, which is the obvious design and the one this replaced: a frame with a pose
 *    and 32 sources is 33 structured clones at 60 Hz, and every one of them lands on the Worker's
 *    task queue in its own turn, so the writes of one visual frame arrive spread across several
 *    audio blocks. That is exactly the incoherence `CMD_COMMIT` exists to prevent (invariant 6).
 *    Batched, the whole frame is one `engine.frame()` on the Worker side and therefore ONE commit,
 *    and the per-frame allocation on the page side is zero: the slab is written in place.
 *
 * The slab is a SharedArrayBuffer, which the design already requires (docs/web.md, "Hosting": no
 * cross-origin isolation, no engine). A page that is not isolated must fail loudly rather than
 * fall back, so there is deliberately no copied-ArrayBuffer path here.
 *
 * SEQ, and why a burst collapses. The page bumps `SEQ` on every write; the Worker remembers the
 * last seq it applied and skips a message whose seq it has already seen. So if the page posts
 * three frames while the Worker was busy, the Worker applies the NEWEST state once instead of
 * three stale ones. The slab holds state, not events, which is what makes that correct.
 */

/* u32 indices into the slab header. The header is 16 slots so the entries start 64-byte aligned. */
export const SLAB_FLAGS = 0;      /* bit 0: the pose fields are valid this frame */
export const SLAB_COUNT = 1;      /* how many source entries follow              */
export const SLAB_SEQ = 2;        /* bumped by the page on every write           */
export const SLAB_POSE = 4;       /* f32[4..10]: px py pz qx qy qz qw            */
export const SLAB_HEADER = 16;    /* u32/f32 slots before the first entry        */
export const SLAB_STRIDE = 4;     /* per entry: u32 handle, f32 x, f32 y, f32 z  */

export const FLAG_POSE = 1;

/** Bytes a slab needs for `maxSources` entries. */
export function slabBytes(maxSources) {
  return (SLAB_HEADER + maxSources * SLAB_STRIDE) * 4;
}

/** The ops the control Worker answers. Everything else is `invoke` or `raw`. */
export const OPS = Object.freeze({
  INIT: "init",           /* { wasmUrl, engine, slab, maxSources } -> { info }            */
  CREATE: "create",       /* { kind: "source" | "push" | "bed" }   -> handle              */
  PUSH: "push",           /* { handle, data: Float32Array }        -> frames accepted     */
  HEALTH: "health",       /* {}                                    -> the health object   */
  INFO: "info",           /* {}                                    -> the info object     */
  INVOKE: "invoke",       /* { name, args } -> engine.invoke(name, ...args)               */
  INVOKE_BUF: "invokebuf",/* { name, args } with BUFFER arguments; see host.js's invokeBuf  */
  RAW: "raw",             /* { name, args } -> raw[name](...args); no engine pointer      */
  START: "start",
  STOP: "stop",
  DESTROY: "destroy",
  RENDER: "render",       /* manual sink only: pump one block -> Float32Array (a COPY)    */
  FRAME: "frame",         /* the slab apply + one commit. No reply, by design.            */
});
