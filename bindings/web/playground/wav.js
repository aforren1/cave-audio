/**
 * wav.js - a mono float32 WAV around a Float32Array, so a page's own samples can become an
 * ENGINE ASSET instead of a feed.
 *
 * WHY THIS EXISTS. `bwa_load_sound` takes a PATH, and the only path a browser can resolve is one
 * inside the module's own file system (MEMFS), which `engine.writeFile` writes into. So the
 * distance between "JavaScript has the samples" and "the engine owns the sound and loops it
 * itself" is one 44-byte header. That is worth far more than it costs: a loaded sound is looped by
 * the audio thread, sample accurate, and a page that stalls for 400 ms does not make a hole in it.
 * A push source keeps the page in the audio path forever, and the page is the thing that stalls.
 *
 * FLOAT32 AND NOT INT16, because the samples already are float32: the engine decodes to mono float
 * anyway (dr_wav), so a quantize here would only throw information away and add a dither question
 * nobody asked. 32-bit IEEE float is `wFormatTag = 3`, which dr_wav reads from a plain 16-byte
 * `fmt ` chunk.
 */

/**
 * @param {Float32Array} pcm mono samples
 * @param {number} rate sample rate, Hz
 * @returns {Uint8Array} the whole file
 */
export function encodeWavFloat32(pcm, rate) {
  const bytes = pcm.length * 4;
  const buf = new ArrayBuffer(44 + bytes);
  const v = new DataView(buf);
  const tag = (off, s) => { for (let i = 0; i < s.length; ++i) v.setUint8(off + i, s.charCodeAt(i)); };
  tag(0, "RIFF");
  v.setUint32(4, 36 + bytes, true);
  tag(8, "WAVE");
  tag(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 3, true);            /* WAVE_FORMAT_IEEE_FLOAT */
  v.setUint16(22, 1, true);            /* mono: the engine's point sources are mono */
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * 4, true);     /* byte rate */
  v.setUint16(32, 4, true);            /* block align */
  v.setUint16(34, 32, true);           /* bits per sample */
  tag(36, "data");
  v.setUint32(40, bytes, true);
  /* 44 is a multiple of 4, so the samples are aligned and this needs no byte-by-byte copy.
   * WAV is little-endian and so is every platform a browser runs on. */
  new Float32Array(buf, 44).set(pcm);
  return new Uint8Array(buf);
}

/**
 * Fold an AudioBuffer (what `decodeAudioData` hands back) to the one mono channel a point source
 * takes. The spatialization is the engine's job and a stereo asset would only fight it.
 * @param {AudioBuffer} buf
 * @returns {Float32Array}
 */
export function foldToMono(buf) {
  const n = buf.length;
  const out = new Float32Array(n);
  for (let c = 0; c < buf.numberOfChannels; ++c) {
    const ch = buf.getChannelData(c);
    for (let i = 0; i < n; ++i) out[i] += ch[i] / buf.numberOfChannels;
  }
  return out;
}
