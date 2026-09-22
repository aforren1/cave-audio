/**
 * worker.js - the CONTROL THREAD, as a module Worker.
 *
 * This is the thread docs/web.md's decision names: not the page's main thread, and not the audio
 * thread. It owns the engine pointer and every `bwa_*` call in the process, so invariant 2 holds
 * structurally rather than by a guard. It is also allowed to BLOCK, which the main thread is not:
 * `Atomics.wait` is forbidden there, and `bwa_destroy` joins the stream and loader threads while
 * the asset loader parks on an `os_event`.
 *
 * The transport is three lines of glue over `Host`, on purpose: every decision about what the
 * protocol IS lives in `protocol.js` and every decision about what a call DOES lives in
 * `host.js`, so the in-page topology runs the same code with no Worker at all.
 *
 * `frame` gets no reply. It is the one message on a per-frame path, and a reply would put a
 * structured clone and a task-queue turn on that path for a value nobody reads.
 */
import { OPS } from "./protocol.js";
import { Host } from "./host.js";

const host = new Host();

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (msg.op === OPS.FRAME) { host.applyFrame(); return; }
  try {
    const value = msg.op === OPS.INIT ? await host.init(msg) : host.handle(msg.op, msg);
    self.postMessage({ id: msg.id, ok: true, value });
  } catch (err) {
    /* An Error does not survive every structured clone intact, so send the parts that matter. */
    self.postMessage({
      id: msg.id,
      ok: false,
      error: String(err && err.message ? err.message : err),
      result: err && err.result != null ? err.result : null,
    });
  }
};
