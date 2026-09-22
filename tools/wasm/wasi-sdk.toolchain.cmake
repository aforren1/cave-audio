# wasi-sdk.toolchain.cmake — build the engine for wasm32 with wasi-sdk (NOT Emscripten).
#
# Why this file exists rather than wasi-sdk's own share/cmake/wasi-sdk-pthread.cmake: that one
# targets the legacy `wasm32-wasi-threads` triple and sets only the two memory flags, which is
# enough to LINK and not enough to RUN. Three more facts have to be stated or the engine fails at
# run time in ways that look like engine bugs (see docs/web.md, "What the toolchain must say"):
#
#   --max-memory   A shared wasm memory that cannot grow makes every pthread_create fail with
#                  ENOMEM, which wasi-libc reports as EAGAIN - so the audio thread never starts
#                  and rt_create simply returns an error. MEASURED: without it, the threads
#                  probe returns pthread_create=6; with it, 0.
#   -lwasi-emulated-process-clocks
#                  wasi-libc has no clock(), because WASI has no process-associated clock. Only
#                  the tests use it (test/os_test.c, test/fuzz_api.c), but a link error in a test
#                  is still a link error.
#   -ldl           wasi-libc's dlopen is a stub in a separate archive. os.h's os_dl_* is compiled
#                  on every POSIX target and the stub always returns NULL, which is exactly the
#                  "library not present" answer the two Linux backends already handle.
#
# Usage:
#   cmake -S . -B build-wasi -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=tools/wasm/wasi-sdk.toolchain.cmake \
#         -DWASI_SDK_PREFIX=<wasi-sdk root> -DCMAKE_BUILD_TYPE=Release
#
# WASI_SDK_PREFIX may also come from the WASI_SDK_PATH environment variable.
#
# What you get: a STATIC libbw_audio.a plus the offline test and example binaries, on the null and
# manual sinks. There is no device backend and no shared library - see CMakeLists.txt's WASI
# branches, and docs/web.md for what a browser host then has to supply.

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

if(NOT WASI_SDK_PREFIX)
  if(DEFINED ENV{WASI_SDK_PATH})
    set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PATH}")
  else()
    message(FATAL_ERROR "wasi-sdk.toolchain.cmake: set -DWASI_SDK_PREFIX=<wasi-sdk root> or the "
                        "WASI_SDK_PATH environment variable.")
  endif()
endif()
# A toolchain file is re-read inside every try_compile, in a fresh scope that inherits only the
# cache and CMAKE_TRY_COMPILE_PLATFORM_VARIABLES. Without both lines the compiler probe re-enters
# this file with WASI_SDK_PREFIX empty and fails with "CMAKE_C_COMPILER not set, after
# EnableLanguage", which names neither this file nor the real cause.
set(WASI_SDK_PREFIX "${WASI_SDK_PREFIX}" CACHE PATH "wasi-sdk install root")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WASI_SDK_PREFIX)

# CMake has no Platform/WASI module of its own yet; wasi-sdk ships one. Without it the configure
# still works but prints "System is unknown to cmake" at every compiler probe.
if(EXISTS "${WASI_SDK_PREFIX}/share/cmake/Platform/WASI.cmake")
  list(APPEND CMAKE_MODULE_PATH "${WASI_SDK_PREFIX}/share/cmake")
endif()

if(CMAKE_HOST_WIN32)
  set(_wasi_exe ".exe")
else()
  set(_wasi_exe "")
endif()

# wasm32-wasip1-threads, not the `wasm32-wasi-threads` alias: same sysroot, current spelling.
set(_wasi_triple wasm32-wasip1-threads)
set(CMAKE_C_COMPILER   "${WASI_SDK_PREFIX}/bin/clang${_wasi_exe}")
set(CMAKE_CXX_COMPILER "${WASI_SDK_PREFIX}/bin/clang++${_wasi_exe}")
set(CMAKE_AR           "${WASI_SDK_PREFIX}/bin/llvm-ar${_wasi_exe}")
set(CMAKE_RANLIB       "${WASI_SDK_PREFIX}/bin/llvm-ranlib${_wasi_exe}")
set(CMAKE_C_COMPILER_TARGET   ${_wasi_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_wasi_triple})

# -pthread on the COMPILE line as well as the link line: it is what turns on atomics, bulk-memory
# and the thread-local model the C11 atomics in rt.c and the seqlock in pose.h are compiled
# against. -msimd128 is the one codegen knob worth taking by default - the panner and the mixer are
# straight float loops and every wasm engine that ships threads also ships SIMD.
set(CMAKE_C_FLAGS_INIT   "-pthread -msimd128")
set(CMAKE_CXX_FLAGS_INIT "-pthread -msimd128")

# Shared memory must be imported (the host creates it and hands it to every thread's instance) and
# exported (WASI's own preview1 adapter looks up "memory" on the instance). --max-memory is the
# growth headroom; 512 MB is generous for an engine whose steady state is a few MB of bus, voice
# table and decoded assets, and costs nothing until touched.
set(BWA_WASM_MAX_MEMORY 536870912 CACHE STRING "wasm shared-memory ceiling in bytes")

# THE SHADOW STACK, and it is the one flag whose absence is hardest to read. wasm-ld defaults to
# 64 KB, the engine needs more than 128 KB, and what you get below that is not an error message:
# the shadow-stack pointer wraps past zero and the next write lands at ~4 GB, so wasmtime reports
# `memory fault at wasm address 0xffff46fc in linear memory of size 0x170000` with a backtrace
# pointing at whatever libc function happened to be running. MEASURED by bisection on rt_create:
# 64 KB and 128 KB fault, 256 KB and up succeed. 1 MB is the desktop main-thread figure and costs
# only address space.
set(BWA_WASM_STACK_SIZE 1048576 CACHE STRING "wasm shadow-stack size in bytes (must exceed 128 KB)")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,--import-memory -Wl,--export-memory -Wl,--max-memory=${BWA_WASM_MAX_MEMORY} -Wl,-z,stack-size=${BWA_WASM_STACK_SIZE} -ldl -lwasi-emulated-process-clocks")
add_compile_definitions(_WASI_EMULATED_PROCESS_CLOCKS)

# The wasm runtime ctest drives the test binaries through. Point -DBWA_WASM_RUNTIME at a wasmtime
# (or any runtime taking the same flags) and `ctest --test-dir <build>` runs the suite unchanged:
# CMAKE_CROSSCOMPILING_EMULATOR is prepended to every add_test COMMAND for free. Both flags are
# needed and mean different things - -W is the wasm threads PROPOSAL (shared memory, atomics), -S
# is the WASI thread-spawn API. --dir gives the tests the working directory they write their
# scratch wav files into; without it every file open fails and the failure reads as a DSP bug.
set(BWA_WASM_RUNTIME "" CACHE FILEPATH "wasm runtime ctest runs the test binaries under (wasmtime)")
if(BWA_WASM_RUNTIME)
  set(CMAKE_CROSSCOMPILING_EMULATOR
      "${BWA_WASM_RUNTIME};run;-W;threads=y;-S;threads=y;--dir=.")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
