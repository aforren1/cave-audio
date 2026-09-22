/*
 * helper.mjs - locate the built module and the binding sources.
 *
 * ctest passes BWA_WEB_MODULE and BWA_WEB_SRC so the suite runs against what was JUST LINKED
 * rather than against whatever a previous staging left in dist/. Run by hand with neither, it
 * falls back to dist/, which is what a developer poking at a shipped build wants.
 */
import { existsSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const HERE = resolve(import.meta.dirname);

export const SRC = process.env.BWA_WEB_SRC
  ? resolve(process.env.BWA_WEB_SRC)
  : resolve(HERE, "..", "dist");

export const MODULE_PATH = process.env.BWA_WEB_MODULE
  ? resolve(process.env.BWA_WEB_MODULE)
  : resolve(HERE, "..", "dist", "bw_audio.mjs");

export const haveModule = existsSync(MODULE_PATH);

export async function loadModule() {
  const factory = (await import(pathToFileURL(MODULE_PATH).href)).default;
  return factory({});
}

export async function loadSrc(name) {
  return import(pathToFileURL(resolve(SRC, name)).href);
}

/* A wav-free mono test signal. DC is deliberately NOT used anywhere in this suite: the CLAUDE.md
 * trap is that a DC-driven laterality assertion reads the HRTF's per-ear DC gains, which oppose
 * its audible ILD, and it shipped a left/right mirror once already. Tones only. */
export function tone(frames, rate, hz = 440, amp = 0.5) {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; ++i) out[i] = amp * Math.sin((2 * Math.PI * hz * i) / rate);
  return out;
}
