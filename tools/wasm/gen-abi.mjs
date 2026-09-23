#!/usr/bin/env node
/*
 * gen-abi.mjs - read include/bw_audio.h and write bindings/web/src/abi.js.
 *
 * THE RAW LAYER IS GENERATED, for the reason bindings/python's raw layer is hand-written and
 * still one for one: the C ABI is the contract, and a JS layer that drifts from it is worse than
 * no layer. Here the header is parsed rather than transcribed, so a call added to bw_audio.h and
 * not to this file cannot exist. bindings/web/tests/raw_layer.test.mjs compares the generated
 * table against the module's own wasm exports, which is the other half: the table cannot claim a
 * symbol the engine does not carry.
 *
 * WHAT IT EMITS. One entry per BWA_API declaration: { name, ret, args }, where the name has kept
 * its bwa_ prefix (the raw layer strips it, the way Python's does) and the types are the four
 * marshalling classes JS actually has to tell apart:
 *
 *   num    i32 / f32 / f64 / an enum / a bool - a plain JS number either way
 *   i64    uint64_t or int64_t. emcc defaults to -sWASM_BIGINT, so these cross as BigInt and the
 *          raw layer converts both ways. Frame counts stay exact well past 2^53.
 *   cstr   a const char* - an argument copied onto the wasm stack, a return read with UTF8ToString
 *   ptr    any other pointer or array: a wasm heap address the caller supplies
 *
 * Plus CONSTANTS: every `#define BWA_<NAME> <integer literal>` in the header, keyed by the name
 * minus BWA_ (the raw layer's rule for calls, and bindings/python's for these same defines). Only
 * a PLAIN literal qualifies - decimal, hex, optionally parenthesized and negative, optionally
 * u-suffixed. Anything computed (BWA_VERSION) or not a number (BWA_API) is skipped rather than
 * evaluated, because a generator that evaluates C expressions is a second compiler. The
 * BWA_VERSION_* triple is ABI_VERSION already and stays out of this table.
 *
 * Run it from the repo root: node tools/wasm/gen-abi.mjs
 */
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const HEADER = join(ROOT, "include", "bw_audio.h");
const OUT = join(ROOT, "bindings", "web", "src", "abi.js");
const OUT_EXPORTS = join(ROOT, "bindings", "web", "src", "exported_functions.json");

/* Its callback runs on the audio thread, and a JS callback must never run there (invariant 1).
 * The same exclusion the Python and MATLAB bindings make; the manual sink plus render_block is
 * the offline path. Excluded from BOTH layers, so there is nothing to find and be tempted by. */
const EXCLUDED = new Set(["bwa_set_output_capture"]);

const raw = readFileSync(HEADER, "utf8");
/* Comments go first, but a block comment must leave its newlines behind: the next step deletes
 * preprocessor LINES, and collapsing a multi-line comment would weld the #define that follows it
 * onto the declaration above. */
let src = raw
  .replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, " "))
  .replace(/\/\/[^\n]*/g, " ")
  .replace(/^[ \t]*#.*$/gm, " ");

/* Join a declaration's continuation lines: several take more arguments than fit in 100 columns. */
const decls = [];
for (const m of src.matchAll(/BWA_API\s+([^;]*);/g)) decls.push(m[1].replace(/\s+/g, " ").trim());

function argType(text) {
  const t = text.trim();
  if (t.includes("(*")) return "ptr";                       /* a function pointer */
  if (/\bconst\s+char\s*\*/.test(t)) return "cstr";
  if (t.includes("*") || t.includes("[")) return "ptr";
  if (/\b(u?int64_t)\b/.test(t)) return "i64";
  return "num";
}

function retType(text) {
  const t = text.trim();
  if (t === "void") return "void";
  if (/\bconst\s+char\s*\*/.test(t)) return "cstr";
  if (t.includes("*")) return "ptr";
  if (/\b(u?int64_t)\b/.test(t)) return "i64";
  return "num";
}

const fns = [];
for (const d of decls) {
  const m = /^(.*?)\b(bwa_[A-Za-z0-9_]+)\s*\((.*)\)$/.exec(d);
  if (!m) {
    if (d.includes("bwa_")) throw new Error(`gen-abi: could not parse declaration: ${d}`);
    continue;                                               /* the BWA_API #define lines */
  }
  const [, ret, name, argsText] = m;
  if (EXCLUDED.has(name)) continue;
  const inner = argsText.trim();
  const args =
    inner === "" || inner === "void"
      ? []
      : splitArgs(inner).map(argType);
  fns.push({ name, ret: retType(ret), args });
}

/* Split on commas that are not inside parentheses: a function-pointer argument carries its own. */
function splitArgs(s) {
  const out = [];
  let depth = 0;
  let cur = "";
  for (const ch of s) {
    if (ch === "(") depth++;
    else if (ch === ")") depth--;
    if (ch === "," && depth === 0) { out.push(cur); cur = ""; } else cur += ch;
  }
  if (cur.trim()) out.push(cur);
  return out;
}

fns.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));

const version = (() => {
  const g = (k) => Number(new RegExp(`#define BWA_VERSION_${k}\\s+(\\d+)`).exec(raw)[1]);
  return [g("MAJOR"), g("MINOR"), g("PATCH")];
})();

/* Read from the RAW text, not the comment-stripped copy: that one has already blanked every
 * preprocessor line. The pattern is anchored at the start of a line, so a #define quoted inside a
 * comment's prose is not read. A trailing comment on the define itself is stripped first. */
const constants = [];
for (const m of raw.matchAll(/^[ \t]*#define[ \t]+BWA_([A-Z0-9_]+)[ \t]+([^\n]*)$/gm)) {
  const [, name, rest] = m;
  if (name.startsWith("VERSION")) continue;
  const text = rest.replace(/\/\*.*?\*\/|\/\/.*$/g, "").trim();
  const lit = /^\(?\s*(-?)\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\s*\)?$/.exec(text);
  if (!lit) continue;
  const v = (lit[1] ? -1 : 1) * Number(lit[2]);
  if (!Number.isSafeInteger(v))
    throw new Error(`gen-abi: BWA_${name} = ${text} is not a safe JS integer`);
  /* Keep the header's radix, so a flag reads as a flag in the generated file too. */
  const hex = /^0[xX]/.test(lit[2]);
  constants.push({ name, text: hex ? (lit[1] ? "-" : "") + lit[2].toLowerCase() : String(v) });
}
constants.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
for (const want of ["MAX_CHANNELS", "DEFAULT_GRID"]) {
  if (!constants.some((c) => c.name === want))
    throw new Error(`gen-abi: BWA_${want} not found in the header`);
}

const body = `/* GENERATED by tools/wasm/gen-abi.mjs from include/bw_audio.h. Do not hand-edit.
 *
 * ${fns.length} entries, ABI ${version.join(".")}. See the generator for what the four type
 * classes mean and why bwa_set_output_capture is not among them.
 */
export const ABI_VERSION = [${version.join(", ")}];

/* The header's plain integer #defines, minus the BWA_ prefix. */
export const CONSTANTS = Object.freeze({
${constants.map((c) => `  ${c.name}: ${c.text},`).join("\n")}
});

export const ABI = [
${fns.map((f) => `  ${JSON.stringify(f)},`).join("\n")}
];

/* What -sEXPORTED_FUNCTIONS has to carry: emcc strips anything no reachable JS names, and a
 * static engine library reaches nothing on its own. */
export const EXPORTED_FUNCTIONS = ABI.map((f) => "_" + f.name);
`;

writeFileSync(OUT, body);

/* The same list again, in the form emcc's -sEXPORTED_FUNCTIONS=@file wants. It is a SECOND file
 * rather than something CMake derives, because the CMake link line must not need node: a
 * checkout can build the web binding with the committed pair and regenerate only when the header
 * moves. _malloc and _free are not ABI and are here because the raw layer's heap seam calls them.
 * EXPORTED_FUNCTIONS also forces the archive members in, which a STATIC engine needs. */
const exported = ["_malloc", "_free", ...fns.map((f) => "_" + f.name)];
writeFileSync(OUT_EXPORTS, JSON.stringify(exported, null, 0) + "\n");
console.log(`gen-abi: ${fns.length} entries, ${constants.length} constants -> ${OUT}`);
console.log(`gen-abi: ${exported.length} exports -> ${OUT_EXPORTS}`);
