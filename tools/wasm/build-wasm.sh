#!/usr/bin/env bash
# Build the engine for wasm32 and, when a runtime is reachable, run the suite under it.
#
# THE ONE RECIPE, the way tools/phonon/build-phonon.sh is the one phonon recipe: whatever ends up
# calling this (a CI job, a developer) runs these steps and not a copy of them.
#
# Two toolchains, and they are not interchangeable - see docs/web.md for which to pick:
#   wasi    wasi-sdk + wasm32-wasip1-threads. A STATIC libbw_audio.a and the offline test
#           binaries, runnable under wasmtime. No Web Audio, no JS glue: a browser host supplies
#           the AudioWorklet and the module loader itself.
#   emsdk   Emscripten. The same static library plus emcc's JS glue, which is what an
#           AudioWorklet build actually ships. -pthread is REQUIRED (see below).
#
# ENVIRONMENT
#   BWA_WASM_TOOLCHAIN   wasi | emsdk                                     (default: wasi)
#   WASI_SDK_PATH        wasi-sdk install root.                           (required for wasi)
#   BWA_WASM_RUNTIME     a wasmtime binary. When set, ctest drives the test binaries under it.
#                        Leave it empty to build only.                    (default: empty)
#   BWA_WASM_BUILD_DIR   where to build.                     (default: build-wasm-$TOOLCHAIN)
#   BWA_WASM_MAX_MEMORY  the shared-memory ceiling in bytes, wasi only.   (default: 512 MB)
#   BWA_WASM_PROXY       1 = link the emsdk executables with -sPROXY_TO_PTHREAD, so main() runs on
#                        a pthread and the browser (or node) main thread keeps its event loop.
#                        This is the shape docs/web.md's decision requires of a page - the control
#                        side must not be the main thread - and it is what turns the `os` and
#                        `idle` failures green. emsdk only.                    (default: empty)
#   CMAKE_MAKE_PROGRAM   a ninja binary, when ninja is not on PATH. Forwarded to the configure,
#                        because a toolchain file that sets CMAKE_FIND_ROOT_PATH_MODE_PROGRAM cannot
#                        be relied on to find one that only a shell PATH knows about. A Windows host
#                        with a downloaded ninja hits this first.              (default: empty)
#
# WHY -pthread IS NOT OPTIONAL ON EMSCRIPTEN. Without it the link fails on
# sched_get_priority_min, sched_get_priority_max and pthread_setschedparam, which emscripten
# DECLARES in its headers and defines only in the pthreads build. src/os/os_posix.c degrades those
# three away behind BWA_OS_NO_SCHED, so a single-threaded build links - but it still has no
# pthread_create, and the engine's control thread, asset loader and stream refill are all threads.
# A single-threaded web build is a DIFFERENT ENGINE SHAPE, not a flag; docs/web.md says what it
# would take.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BWA_WASM_TOOLCHAIN="${BWA_WASM_TOOLCHAIN:-wasi}"
BWA_WASM_RUNTIME="${BWA_WASM_RUNTIME:-}"
BWA_WASM_BUILD_DIR="${BWA_WASM_BUILD_DIR:-build-wasm-$BWA_WASM_TOOLCHAIN}"
BWA_WASM_MAX_MEMORY="${BWA_WASM_MAX_MEMORY:-536870912}"
MAKE_PROGRAM_FLAG=""
if [ -n "${CMAKE_MAKE_PROGRAM:-}" ]; then MAKE_PROGRAM_FLAG="-DCMAKE_MAKE_PROGRAM=$CMAKE_MAKE_PROGRAM"; fi

case "$BWA_WASM_TOOLCHAIN" in
  wasi)
    : "${WASI_SDK_PATH:?set WASI_SDK_PATH to the wasi-sdk install root}"
    cmake -S . -B "$BWA_WASM_BUILD_DIR" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/wasm/wasi-sdk.toolchain.cmake" \
      -DWASI_SDK_PREFIX="$WASI_SDK_PATH" \
      -DBWA_WASM_MAX_MEMORY="$BWA_WASM_MAX_MEMORY" \
      -DBWA_WASM_RUNTIME="$BWA_WASM_RUNTIME" \
      -DCMAKE_BUILD_TYPE=Release $MAKE_PROGRAM_FLAG
    ;;
  emsdk)
    # PROXY_TO_PTHREAD is the measured difference between the two emsdk rows in docs/web.md. Both
    # failures it fixes are the SAME finding: os_sleep_ms and os_event_wait block, and a blocked
    # main thread is a slow test under node and a hard error in a browser. With it, main() runs on
    # a pthread and both go green. The pool grows by one, because main now occupies a slot.
    PROXY_FLAGS=""
    if [ -n "${BWA_WASM_PROXY:-}" ]; then PROXY_FLAGS="-sPROXY_TO_PTHREAD"; fi
    # emcmake supplies the toolchain file and sets CMAKE_CROSSCOMPILING_EMULATOR to its own node,
    # so ctest runs the suite with no further help. ALLOW_MEMORY_GROWTH is emscripten's spelling of
    # the --max-memory headroom the wasi side needs for the same reason: a shared memory that
    # cannot grow makes every pthread_create fail. The two STACK_SIZE flags are the wasi side's
    # `-z stack-size`: emcc's default is the same 64 KB and the engine needs more than 128 KB, on
    # the main thread AND on every pthread (the audio thread renders on its own stack). Without
    # them the suite drops from 31/33 to 13/33 and the failures read as timeouts, not as stack
    # faults. The emscripten/emsdk image ships no ninja: `apt-get install ninja-build` first, or
    # point CMAKE_MAKE_PROGRAM at one.
    emcmake cmake -S . -B "$BWA_WASM_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release $MAKE_PROGRAM_FLAG \
      -DCMAKE_C_FLAGS="-pthread" -DCMAKE_CXX_FLAGS="-pthread" \
      -DCMAKE_EXE_LINKER_FLAGS="-pthread -sALLOW_MEMORY_GROWTH=1 -sPTHREAD_POOL_SIZE=9 -sEXIT_RUNTIME=1 -sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=1048576 $PROXY_FLAGS"
    ;;
  *)
    echo "BWA_WASM_TOOLCHAIN must be wasi or emsdk, got '$BWA_WASM_TOOLCHAIN'"; exit 1 ;;
esac

cmake --build "$BWA_WASM_BUILD_DIR"

if [ "$BWA_WASM_TOOLCHAIN" = "emsdk" ] || [ -n "$BWA_WASM_RUNTIME" ]; then
  # --timeout: a wasm test that goes wrong tends to HANG rather than fail (a shadow-stack fault
  # left test_golden spinning for minutes; `os` under emsdk sits in a blocked main-thread wait), and
  # an unattended run must end. 600 s is ten times the slowest honest test.
  ctest --test-dir "$BWA_WASM_BUILD_DIR" --output-on-failure --timeout 600
else
  echo "built $BWA_WASM_BUILD_DIR; set BWA_WASM_RUNTIME to a wasmtime to run the suite"
fi
