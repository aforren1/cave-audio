/**
 * engine.js - the IDIOMATIC layer: Engine, Sound, Source, PushSource, Bed, Listener.
 *
 * The second of the two layers `bindings/python` has, in the same shape and with the same one
 * convenience that has semantics: it COMMITS for you. A commit-gated write (a source position, the
 * listener pose) lands at once unless you are inside `frame()`, which defers to one commit at the
 * block's exit. `CMD_COMMIT` is what frame coherence MEANS in this engine (concurrency.md), and a
 * page's control loop is `requestAnimationFrame`, whose cadence is nothing like the audio block's,
 * so this matters more here than on a desktop.
 *
 * WHERE IT RUNS. Inside the control Worker (`worker.js`), never on the page's main thread. That is
 * the decision docs/web.md records and it is not a style preference: `bwa_destroy` joins threads
 * and the asset loader parks on an `os_event`, and `Atomics.wait` is FORBIDDEN on a browser's main
 * thread. The one-control-thread invariant is therefore structural rather than guarded - the
 * engine pointer exists only in the Worker that made it, so a second caller has nothing to call
 * with. A page reaches this through `client.js`.
 *
 * UNITS are the C ABI's, and the rule about borrowing a host name applies with force here: Web
 * Audio speaks SECONDS everywhere and the engine's frame-valued calls keep saying `Frames`. Both
 * spellings exist where both are useful (`seekFrames` / `seekSeconds`), the way the Godot binding
 * had to learn to do.
 */
import { makeRaw, CONSTANTS } from "./raw.js";

export class BwaError extends Error {
  constructor(message, result) {
    super(message);
    this.name = "BwaError";
    this.result = result ?? null;
  }
}

/* bwa_profile, bwa_sink_type and the two enums a page actually chooses from. Mirrors the header;
 * everything else stays reachable as a plain integer through `invoke`. */
export const Profile = { CAVE: 0, BINAURAL: 1, CAVE_SIM: 2, CAVE_BOTH: 3 };
export const SinkType = {
  AUTO: 0, ASIO: 1, NULL: 2, MANUAL: 3, WASAPI: 4, COREAUDIO: 5, ALSA: 6, AAUDIO: 7, JACK: 8,
  WORKLET: 9,
};
export const SinkFlags = {
  NONE: 0,
  EXCLUSIVE: CONSTANTS.SINK_FLAG_EXCLUSIVE,
  EXACT_RATE: CONSTANTS.SINK_FLAG_EXACT_RATE,
  TIGHT_BUFFER: CONSTANTS.SINK_FLAG_TIGHT_BUFFER,
  DEEP_BUFFER: CONSTANTS.SINK_FLAG_DEEP_BUFFER,
};

/* The header's integer constants, the names bindings/python's idiomatic layer uses. Read from the
 * generated table, so none of them is a second copy of the header. MAX_CHANNELS is the array
 * CAPACITY (size a static per-speaker buffer with it); an engine's ACTIVE count is its
 * `channelCount`, which is DEFAULT_GRID with no layout and the layout's count otherwise. */
export const MAX_CHANNELS = CONSTANTS.MAX_CHANNELS;
export const DEFAULT_GRID = CONSTANTS.DEFAULT_GRID;
export const CHANNEL_AUTO = CONSTANTS.CHANNEL_AUTO;
export const GROUPS = CONSTANTS.GROUPS;
export const EXTRA_LIS = CONSTANTS.EXTRA_LIS;

const HEALTH_FIELDS = [
  "blocks", "xruns", "droppedFrames", "driverResyncs", "lateBlocks", "streamStarves",
  "peakLoad", "deviceLost",
];

class Handle {
  constructor(engine, handle) {
    this.engine = engine;
    this.handle = handle;
  }
  valueOf() { return this.handle; }
}

export class Sound extends Handle {
  get frames() { return this.engine.raw.sound_get_frames(this.engine.ptr, this.handle); }
  get channels() { return this.engine.raw.sound_get_channels(this.engine.ptr, this.handle); }
  get seconds() { return this.frames / this.engine.sampleRate; }
  get ready() { return !!this.engine.raw.sound_is_ready(this.engine.ptr, this.handle); }
  unload() { this.engine.raw.unload_sound(this.engine.ptr, this.handle); }
  release() { this.engine.raw.sound_release(this.engine.ptr, this.handle); }
}

export class Source extends Handle {
  /* --- commit-gated: the mixer sees these only after a commit (invariant 6) --- */
  setPosition(x, y, z) {
    this.engine.raw.source_set_pos(this.engine.ptr, this.handle, x, y, z);
    this.engine._touch();
  }
  setOrientation(qx, qy, qz, qw) {
    this.engine.raw.source_set_orientation(this.engine.ptr, this.handle, qx, qy, qz, qw);
    this.engine._touch();
  }

  /* --- immediate --- */
  play(sound, loop = false) {
    this.engine.raw.source_play(this.engine.ptr, this.handle, +sound, loop);
  }
  playAtFrames(sound, startFrame, loop = false) {
    this.engine.raw.source_play_at(this.engine.ptr, this.handle, +sound, loop, startFrame);
  }
  queue(sound, loop = false) {
    this.engine.raw.source_queue(this.engine.ptr, this.handle, +sound, loop);
  }
  stop() { this.engine.raw.source_stop(this.engine.ptr, this.handle); }
  stopAtFrames(f) { this.engine.raw.source_stop_at(this.engine.ptr, this.handle, f); }
  seekFrames(f) { this.engine.raw.source_seek(this.engine.ptr, this.handle, f); }
  seekSeconds(s) { this.seekFrames(Math.round(s * this.engine.sampleRate)); }
  fadeTo(gain, seconds) {
    this.engine.raw.source_fade_to(this.engine.ptr, this.handle, gain, seconds);
  }
  fadeOut(seconds) { this.engine.raw.source_fade_out(this.engine.ptr, this.handle, seconds); }
  setGain(linear) { this.engine.raw.source_set_gain(this.engine.ptr, this.handle, linear); }
  setPitch(rate) { this.engine.raw.source_set_pitch(this.engine.ptr, this.handle, rate); }
  setPaused(on) { this.engine.raw.source_set_paused(this.engine.ptr, this.handle, on); }
  setSpread(amount) { this.engine.raw.source_set_spread(this.engine.ptr, this.handle, amount); }
  setSize(radiusM) { this.engine.raw.source_set_size(this.engine.ptr, this.handle, radiusM); }
  setGroup(group) { this.engine.raw.source_set_group(this.engine.ptr, this.handle, group); }
  destroy() { this.engine.raw.source_destroy(this.engine.ptr, this.handle); }

  get isPlaying() { return !!this.engine.raw.source_is_playing(this.engine.ptr, this.handle); }
  get playheadFrames() {
    return this.engine.raw.source_get_playhead_frames(this.engine.ptr, this.handle);
  }
}

export class PushSource extends Source {
  /**
   * Feed interleaved-by-nothing MONO float frames. This is the inbound exception CLAUDE.md names:
   * caller PCM into the engine, on the control thread, a source feed and not a render path. It is
   * also how a page gets audio in at all, because there is no synchronous file system to decode
   * from - `AudioContext.decodeAudioData` in JS, then push (docs/web.md).
   *
   * @param {Float32Array} frames mono samples
   * @returns {number} frames accepted (short of `frames.length` when the ring is full)
   */
  push(frames) {
    const raw = this.engine.raw;
    const n = frames.length;
    if (n === 0) return 0;
    const need = n * 4;
    if (need > this.engine._scratchBytes) this.engine._growScratch(need);
    raw.f32.set(frames, this.engine._scratch >> 2);
    return raw.source_push(this.engine.ptr, this.handle, this.engine._scratch, n);
  }
  get space() { return this.engine.raw.source_push_space(this.engine.ptr, this.handle); }
  pushEnd() { this.engine.raw.source_push_end(this.engine.ptr, this.handle); }
}

export class Bed extends Handle {
  play(sound, loop = false) { this.engine.raw.bed_play(this.engine.ptr, this.handle, +sound, loop); }
  stop() { this.engine.raw.bed_stop(this.engine.ptr, this.handle); }
  setGain(linear) { this.engine.raw.bed_set_gain(this.engine.ptr, this.handle, linear); }
  setOrientation(yawRad, pitchRad = 0, rollRad = 0) {
    this.engine.raw.bed_set_orientation(this.engine.ptr, this.handle, yawRad, pitchRad, rollRad);
  }
  destroy() { this.engine.raw.bed_destroy(this.engine.ptr, this.handle); }
  get isPlaying() { return !!this.engine.raw.bed_is_playing(this.engine.ptr, this.handle); }
}

export class Listener {
  constructor(engine) { this.engine = engine; }
  /** Commit-gated. Position in meters, orientation as a quaternion (see docs/api.md's frame). */
  setPose(px, py, pz, qx, qy, qz, qw) {
    this.engine.raw.set_listener_pose(this.engine.ptr, px, py, pz, qx, qy, qz, qw);
    this.engine._touch();
  }
  setPosePredictionSeconds(leadS) {
    this.engine.raw.set_pose_prediction(this.engine.ptr, leadS);
  }
}

export class Engine {
  /**
   * @param {object} raw the raw layer (see raw.js)
   * @param {object} opts the bwa_desc fields, in the header's names
   */
  constructor(raw, opts = {}) {
    this.raw = raw;
    this._autocommit = opts.autocommit !== false;
    this._frameDepth = 0;
    this._dirty = false;
    this._scratch = 0;
    this._scratchBytes = 0;

    this.ptr = raw.module._bwaw_create(
      opts.profile ?? Profile.BINAURAL,
      opts.sampleRate ?? 0,
      opts.blockSize ?? 0,
      opts.sink ?? SinkType.AUTO,
      this._cstr(opts.device),
      opts.sinkFlags ?? 0,
      this._cstr(opts.layoutPath),
      this._cstr(opts.hrtfPath),
      opts.bedDecoder ?? 0,
      opts.enablePathing ? 1 : 0
    );
    this._freeTemp();
    if (!this.ptr) throw new BwaError("bwa_create returned NULL: the engine could not be created");

    this.sampleRate = raw.get_sample_rate(this.ptr);
    this.blockSize = raw.get_block_size(this.ptr);
    this.channelCount = raw.get_channel_count(this.ptr);
    this.listener = new Listener(this);
    this._health = raw.alloc(8 * 8);
  }

  /* Strings handed to bwaw_create outlive the call only until bwa_create has copied them, but
   * bwa_desc holds POINTERS and bwa_create reads them inside that one call, so a heap temp freed
   * straight after is correct and a stack temp would be too. Heap, because there may be four of
   * them and the stack is the audio-safe resource. */
  _cstr(s) {
    if (s === undefined || s === null || s === "") return 0;
    const M = this.raw.module;
    const bytes = M.lengthBytesUTF8(s) + 1;
    const p = M._malloc(bytes);
    M.stringToUTF8(s, p, bytes);
    (this._temps ||= []).push(p);
    return p;
  }
  _freeTemp() {
    for (const p of this._temps || []) this.raw.free(p);
    this._temps = [];
  }

  _growScratch(bytes) {
    if (this._scratch) this.raw.free(this._scratch);
    this._scratchBytes = Math.max(bytes, 8192);
    this._scratch = this.raw.alloc(this._scratchBytes);
  }

  /* --- the commit model (mirrors bindings/python's Engine.autocommit / Engine.frame) --- */

  get autocommit() { return this._autocommit; }
  set autocommit(on) { this._autocommit = !!on; }
  get pending() { return this._dirty; }

  _touch() {
    this._dirty = true;
    if (this._autocommit && this._frameDepth === 0) this.commit();
  }

  /**
   * One coherent frame: every commit-gated write inside lands together, at the exit.
   * @param {Function} body
   */
  frame(body) {
    this._frameDepth++;
    try {
      return body();
    } finally {
      this._frameDepth--;
      if (this._frameDepth === 0 && this._dirty && this._autocommit) this.commit();
    }
  }

  commit() {
    this.raw.commit(this.ptr);
    this._dirty = false;
  }

  /* --- lifecycle --- */

  start() {
    const r = this.raw.start(this.ptr);
    if (r !== 0) throw new BwaError(this.lastError || "bwa_start failed", r);
  }
  stop() { this.raw.stop(this.ptr); }
  destroy() {
    if (!this.ptr) return;
    this.raw.destroy(this.ptr);
    if (this._health) this.raw.free(this._health);
    if (this._scratch) this.raw.free(this._scratch);
    this.ptr = 0;
    this._health = 0;
    this._scratch = 0;
  }

  /* --- readbacks --- */

  get backend() { return this.raw.get_audio_backend(this.ptr); }
  get sinkType() { return this.raw.get_sink_type(this.ptr); }
  get lastError() { return this.raw.last_error(this.ptr) || null; }
  get activeVoices() { return this.raw.get_active_voices(this.ptr); }
  get dspTimeFrames() { return this.raw.get_dsp_time_frames(this.ptr); }
  get dspTimeSeconds() { return this.dspTimeFrames / this.sampleRate; }
  get outputLatencyFrames() { return this.raw.get_output_latency_frames(this.ptr); }
  get outputLatencySeconds() { return this.outputLatencyFrames / this.sampleRate; }

  /** @returns {object} the eight bwa_health fields plus `measured` (see bwa_web.c). */
  health() {
    const ok = this.raw.module._bwaw_health(this.ptr, this._health);
    const base = this._health >> 3;
    const out = { measured: !!ok };
    for (let i = 0; i < HEALTH_FIELDS.length; ++i) out[HEALTH_FIELDS[i]] = this.raw.f64[base + i];
    return out;
  }

  /* --- assets and handles --- */

  loadSound(path) {
    const h = this.raw.load_sound(this.ptr, path);
    if (!h) throw new BwaError(this.lastError || `bwa_load_sound failed for ${path}`);
    return new Sound(this, h);
  }
  createSource() {
    const h = this.raw.source_create(this.ptr);
    if (!h) throw new BwaError(this.lastError || "bwa_source_create failed");
    return new Source(this, h);
  }
  createPushSource() {
    const h = this.raw.source_create_push(this.ptr);
    if (!h) throw new BwaError(this.lastError || "bwa_source_create_push failed");
    return new PushSource(this, h);
  }
  createBed() {
    const h = this.raw.bed_create(this.ptr);
    if (!h) throw new BwaError(this.lastError || "bwa_bed_create failed");
    return new Bed(this, h);
  }

  setMasterGain(linear) { this.raw.set_master_gain(this.ptr, linear); }
  setPaused(on) { this.raw.set_paused(this.ptr, on); }
  stopAll() { this.raw.stop_all(this.ptr); }

  /**
   * The escape hatch, and the reason this layer stays small: any of the 167 raw calls, with the
   * engine pointer supplied. `engine.invoke("source_set_reverb_send", src, 0.3)` is the same call
   * `raw.source_set_reverb_send(engine.ptr, src, 0.3)` is. Calls that do not take an engine
   * pointer (`host_time_ns`, `spcap_focus_default`, the device query) are on `raw` directly.
   */
  invoke(name, ...args) {
    const fn = this.raw[name];
    if (!fn) throw new BwaError(`bw_audio has no call named bwa_${name}`);
    return fn(this.ptr, ...args);
  }
}

export { makeRaw };
