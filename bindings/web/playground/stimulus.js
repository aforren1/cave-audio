/**
 * stimulus.js - the signals the playground loops. Each one is a fixed buffer, one period long,
 * which the rig encodes as a wav and hands to the engine to loop (rig.js says why).
 *
 * `click()` is THE minimal-example stimulus, character for character the same function
 * bindings/web/example/index.html carries and the one docs/integration.md's "The minimal example"
 * specifies for every binding: one 250 ms period holding a 2 ms Hann-windowed noise burst at
 * -12 dBFS, then silence, so four clicks a second. A click and not a tone because the cues that
 * place a source in elevation and front to back live in the spectrum. The noise is a hand-spelled
 * LCG rather than Math.random so every copy is sample-identical; the Hann divisor is N-1 and the
 * normalize-to-peak pass is part of the spec.
 *
 * The other three exist because different scenes need different things to hear. Noise is what
 * exposes a panner's timbre; a tone is what makes Doppler and the speed of sound audible; and the
 * 1 kHz tone is deliberately the AMBIGUOUS one, which is the point of offering it.
 */

/**
 * The shared click, one period long. Loop it.
 * @param {number} rate sample rate, Hz
 * @returns {Float32Array} one 250 ms period, mono
 */
export function click(rate) {
  const period = Math.round(rate * 0.25);
  const burst = Math.round(rate * 0.002);
  const out = new Float32Array(period);
  let state = 12345;
  let peak = 0;
  const b = new Float64Array(burst);
  for (let i = 0; i < burst; ++i) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const u = ((state >>> 8) & 0xffffff) / 8388608.0 - 1.0;
    const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (burst - 1));
    b[i] = w * u;
    peak = Math.max(peak, Math.abs(b[i]));
  }
  const scale = 0.251189 / peak;                  /* -12 dBFS exactly at the peak sample */
  for (let i = 0; i < burst; ++i) out[i] = b[i] * scale;
  return out;
}

/**
 * Pink-ish noise, a two-second loop. Voss-McCartney with 5 octaves, which is close enough to
 * -3 dB per octave for listening and costs nothing. Same LCG, so the loop is reproducible.
 * @param {number} rate
 * @returns {Float32Array}
 */
export function pink(rate) {
  const n = Math.round(rate * 2.0);
  const out = new Float32Array(n);
  let state = 987654321;
  const rnd = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return ((state >>> 8) & 0xffffff) / 8388608.0 - 1.0;
  };
  const rows = new Float64Array(5);
  let running = 0;
  for (let i = 0; i < n; ++i) {
    /* One row is re-rolled per sample, chosen by the lowest set bit of the index: row k changes
     * every 2^k samples, which is what makes the sum pink. */
    const k = i === 0 ? 0 : Math.min(4, 31 - Math.clz32(i & -i));
    running -= rows[k];
    rows[k] = rnd();
    running += rows[k];
    out[i] = (running / 5) * 0.35;
  }
  return out;
}

/**
 * Repeating 120 ms pink bursts with a 5 ms raised-cosine edge: the onsets a click gives you, over
 * a spectrum broad enough to hear timbre change. The default for the panner comparisons.
 * @param {number} rate
 * @returns {Float32Array}
 */
export function bursts(rate) {
  const src = pink(rate);
  const period = Math.round(rate * 0.4);
  const on = Math.round(rate * 0.12);
  const edge = Math.max(1, Math.round(rate * 0.005));
  const out = new Float32Array(period);
  for (let i = 0; i < on; ++i) {
    let w = 1.0;
    if (i < edge) w = 0.5 - 0.5 * Math.cos((Math.PI * i) / edge);
    else if (i > on - edge) w = 0.5 - 0.5 * Math.cos((Math.PI * (on - i)) / edge);
    out[i] = src[i] * w * 2.0;
  }
  return out;
}

/**
 * A steady tone, one exact cycle count long so the loop has no seam.
 * @param {number} rate
 * @param {number} hz
 * @returns {Float32Array}
 */
export function tone(rate, hz = 1000) {
  const cycles = Math.max(1, Math.round(hz * 0.5));
  const n = Math.round((cycles * rate) / hz);
  const out = new Float32Array(n);
  for (let i = 0; i < n; ++i) out[i] = 0.25 * Math.sin((2 * Math.PI * hz * i) / rate);
  return out;
}

/**
 * The picker's table: a label, a file-name stem, and the builder.
 *
 * `id` is the stem of the wav the rig writes into the module's file system (`/stim/<id>.wav`) and
 * loads with `bwa_load_sound`, because every one of these is a FIXED buffer and the engine loops a
 * loaded sound by itself. See rig.js: the page is deliberately not in the audio path.
 */
export const SIGNALS = [
  { id: "click", name: "click train (default)", make: click },
  { id: "bursts", name: "pink bursts", make: bursts },
  { id: "pink", name: "pink noise", make: pink },
  { id: "tone1k", name: "1 kHz tone (ambiguous)", make: (r) => tone(r, 1000) },
];
