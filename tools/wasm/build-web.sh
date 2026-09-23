#!/usr/bin/env bash
# Build bindings/web/dist: the engine as an ES module a browser page can load, with the Wasm
# Audio Worklet backend in it.
#
# THE SIBLING OF build-wasm.sh, and the split is deliberate. build-wasm.sh builds the ENGINE and
# runs the suite - that is the CI and offline leg, and it must not pay for -sAUDIO_WORKLET, which
# pulls in -sWASM_WORKERS and turns emscripten_get_now into a per-scope runtime probe. This script
# builds the SHIPPABLE PAGE: BWA_WITH_WORKLET=ON, BWA_BUILD_WEB=ON, and a staged dist/ with no
# server-side step of any kind.
#
# ENVIRONMENT
#   BWA_WEB_BUILD_DIR   where to build.                          (default: build-web)
#   BWA_WEB_DIST        where to stage.                          (default: bindings/web/dist)
#   CMAKE_MAKE_PROGRAM  a ninja binary, when ninja is not on PATH.         (default: empty)
#
# RUNNING IT. Emscripten is the toolchain, so this wants an emsdk. On a host without one, the
# container image is the same one build-wasm.sh and the phonon recipe use:
#
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src" -w /src emscripten/emsdk:latest \
#       bash -c 'apt-get update -qq && apt-get install -y -qq ninja-build && \
#                BWA_WEB_BUILD_DIR=/tmp/bw-web tools/wasm/build-web.sh'
#
# Build INSIDE the container and stage OUT to the bind mount: a bind-mounted build directory
# breaks FetchContent's file rename on a Windows host, which is why BWA_WEB_BUILD_DIR is a
# separate knob from BWA_WEB_DIST.
#
# WHAT LANDS IN dist/. Whatever emcc emitted beside the .mjs (the .wasm always; `bw_audio.aw.js`
# because AUDIO_WORKLET is on; a worker script on some settings) plus the binding's own
# JavaScript, flat. The copy is a glob rather than a list for that reason: emcc decides how many
# side files there are, and a list would be a second copy of that decision.
#
# Plus `vendor/`, the demo pages' pinned browser third party, fetched by
# tools/wasm/fetch-web-vendor.sh. dist/ is the tree bindings/web/deploy/stage.sh publishes, so a
# file staged here reaches the site and nothing large lands in git.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BWA_WEB_BUILD_DIR="${BWA_WEB_BUILD_DIR:-build-web}"
BWA_WEB_DIST="${BWA_WEB_DIST:-$ROOT/bindings/web/dist}"
MAKE_PROGRAM_FLAG=""
if [ -n "${CMAKE_MAKE_PROGRAM:-}" ]; then MAKE_PROGRAM_FLAG="-DCMAKE_MAKE_PROGRAM=$CMAKE_MAKE_PROGRAM"; fi

# The raw layer and the export list, regenerated from the header FIRST, so the module and the
# binding cannot be built from two different versions of the ABI. Skipped when there is no node:
# both files are committed, and a checkout that only wants to build is not owed a node.
if command -v node >/dev/null 2>&1; then
  node tools/wasm/gen-abi.mjs
else
  echo "build-web: no node on PATH; using the committed bindings/web/src/abi.js"
fi

# -msimd128 on the engine's own code: the mixer, the panner ramps and the align stage are straight
# float loops that clang vectorizes, every browser that ships threads ships SIMD, and the wasm
# phonon is built with it already (IPL_OS_WASM). Without it a headphone page on a slow machine ran
# the worklet close enough to its budget that dual-band panning (a second gain vector and a
# crossover per voice) pushed blocks late and the click train dropped out (reported 2026-09-22).
emcmake cmake -S . -B "$BWA_WEB_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release $MAKE_PROGRAM_FLAG \
  -DBWA_WITH_WORKLET=ON -DBWA_BUILD_WEB=ON \
  -DCMAKE_C_FLAGS="-pthread -msimd128" -DCMAKE_CXX_FLAGS="-pthread -msimd128"

cmake --build "$BWA_WEB_BUILD_DIR" --target bwa_web

OUT_DIR="$(dirname "$(find "$BWA_WEB_BUILD_DIR" -name bw_audio.mjs -print -quit)")"
if [ -z "$OUT_DIR" ] || [ ! -f "$OUT_DIR/bw_audio.mjs" ]; then
  echo "build-web: emcc produced no bw_audio.mjs under $BWA_WEB_BUILD_DIR"; exit 1
fi

mkdir -p "$BWA_WEB_DIST"
# Everything emcc emitted for this module, and nothing else in the build tree.
for f in "$OUT_DIR"/bw_audio.*; do
  case "$f" in *.a|*.wat) continue ;; esac
  cp -f "$f" "$BWA_WEB_DIST/"
done
cp -f bindings/web/src/*.js "$BWA_WEB_DIST/"
# The playground's default array (tools/layout/gen_dome.py writes it). In dist/ rather than beside
# the page so it rides the same content-addressed URL as the engine that loads it.
cp -f examples/dome_24.json "$BWA_WEB_DIST/"

# The playground's browser third party (three.js), pinned and hashed, into dist/vendor/. It is a
# separate script because it needs only curl: a developer with no emsdk can still fill vendor/, and
# the playground's ctest can tell "no engine" from "no three.js". Under COEP require-corp a CDN
# script is BLOCKED, so the demo ships its own copy (bindings/web/deploy/README.md).
bash "$ROOT/tools/wasm/fetch-web-vendor.sh"

echo "build-web: staged $BWA_WEB_DIST"
ls -l "$BWA_WEB_DIST"
