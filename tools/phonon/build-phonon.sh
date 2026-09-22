#!/usr/bin/env bash
# Build the patched Steam Audio core (phonon) STATICALLY and stage it where CMake auto-detects it.
#
# THE ONE RECIPE, in one file. Two callers run it and neither carries a copy of the steps:
#   .github/actions/build-phonon/   the composite action the windows, android, linux and macos CI
#                                   jobs call. It is a thin wrapper now: it maps its inputs onto
#                                   the environment below and runs this script.
#   tools/phonon/cibw-before-all-linux.sh   the manylinux container's build. A composite action
#                                   runs on the RUNNER, and a wheel's phonon has to be built
#                                   inside the container that links it, so that path cannot use
#                                   the action - only this script.
# The prose version is third_party/README.md, section "Steam Audio". Change both together.
#
# It does NOT cache. The caller owns the cache step (key = submodule sha + patch hash) and runs
# this script on a miss, because only the caller knows which artifacts path it restored.
#
# ENVIRONMENT (one for one with the composite action's inputs)
#   BWA_PLATFORM    build.py -p value: windows | linux | osx | android | wasm.   (required)
#   BWA_ARCH        build.py -a value: x64 | arm64 | armv7 | x86.                (default: x64)
#   BWA_TOOLCHAIN   build.py -t value: vs2013 | vs2015 | vs2017 | vs2019 | vs2022. WINDOWS ONLY,
#                   and IGNORED on every other platform - both pinned scripts accept only those
#                   five spellings, and off Windows they pick the generator from -p alone (Unix
#                   Makefiles for linux and android, Xcode for osx).            (default: vs2022)
#   BWA_STAGE_DIR   the lib/<stage-dir> directory to stage into, under $BWA_ART/lib/. Must match
#                   a name CMake looks for (root CMakeLists.txt, BWA_PHONON_PLATFORMS):
#                   windows-x64, linux-x64, osx-universal, android-arm64, ...    (required)
#   BWA_FFT         which FFT dependency phonon links. pffft on every platform, Android included:
#                   phonon can link FFTS there instead, but only behind a non-default
#                   STEAMAUDIO_ENABLE_FFTS and with NEON turned off on arm64, so we do not.
#                                                                               (default: pffft)
#   BWA_CMAKE_FLAGS extra -D flags for the phonon configure, space separated. Known needs:
#                   Linux on gcc 13 or older wants -DSTEAMAUDIO_ENABLE_AVX=OFF, because the
#                   default ON adds -fabi-version=6 and that breaks <future> inside hrtf.cpp.
#                   Measured: gcc 11.5 and 13.3 fail, gcc 14.3 passes.           (default: empty)
#   BWA_ART         where to stage. Relative to the repo root, or absolute. Leave it alone unless
#                   STEAMAUDIO_DIR is overridden too.   (default: third_party/steam-audio-artifacts)
#   BWA_PYTHON      the interpreter that runs the two pinned scripts. Resolved automatically, and
#                   only worth setting where neither `python` nor `python3` is the one you want:
#                   the emsdk container ships python3 and no `python`.   (default: autodetected)
#   ANDROID_NDK     required when BWA_PLATFORM is android; both pinned scripts read it.
#   EMSDK           required when BWA_PLATFORM is wasm; both pinned scripts read it (build.py
#                   builds the Emscripten toolchain-file path out of it and crashes when it is
#                   unset). The emscripten/emsdk container exports it already.
#
# WASM NOTE. The wasm leg is -pthread on BOTH halves, and that is not a preference. The engine's
# own wasm build needs threads (the control thread, the asset loader and the stream refill are all
# threads), a -pthread module is a SHARED-MEMORY module, and wasm-ld refuses to put an object that
# carries no atomics or bulk-memory features into one:
#   wasm-ld: error: --shared-memory is disallowed by api_context.cpp.o because it was not
#            compiled with 'atomics' or 'bulk-memory' features.
# So phonon and its companions are built with -pthread here without being asked. The companions
# go through get_dependencies.py, which takes no flag argument, so the only lever on them is the
# environment CMake reads at their first configure. See docs/web.md.
#
# WHAT IT STAGES
#   include/phonon.h, include/phonon_version.h
#   lib/<stage-dir>/ the phonon archive plus its three companions (mysofa, zlib, pffft).
#   phonon's own archive carries only its core and hrtf objects, so the companions link beside it.
#   It fails if any of the four is missing, so a half-staged tree never reaches a cache.
#
# CRT NOTE (Windows): everything must be /MD. That is --sharedcrt on the dependencies and
# -DSTEAMAUDIO_STATIC_RUNTIME=OFF on phonon; mixing gives unresolved __imp_ symbols. Both are
# applied below, so a caller does not have to remember.

set -eu

# Run from the repo root whatever the caller's directory is. The composite action used to rely on
# the workspace being current; the container's before-all runs from the mounted project root, and
# a developer runs this from anywhere.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

: "${BWA_PLATFORM:?set BWA_PLATFORM (windows|linux|osx|android|wasm)}"
: "${BWA_STAGE_DIR:?set BWA_STAGE_DIR (windows-x64|linux-x64|osx-universal|android-arm64|wasm32|...)}"
BWA_ARCH="${BWA_ARCH:-x64}"
BWA_TOOLCHAIN="${BWA_TOOLCHAIN:-}"
BWA_FFT="${BWA_FFT:-pffft}"
BWA_CMAKE_FLAGS="${BWA_CMAKE_FLAGS:-}"
BWA_ART="${BWA_ART:-third_party/steam-audio-artifacts}"

# Both pinned scripts are invoked as `python`, which several images do not carry: the emsdk
# container has python3 alone. Resolve the interpreter once here rather than making every caller
# drop a shim on PATH first.
PY="${BWA_PYTHON:-}"
if [ -z "$PY" ]; then
  if command -v python >/dev/null 2>&1; then PY=python; else PY=python3; fi
fi
command -v "$PY" >/dev/null 2>&1 || { echo "no python interpreter: set BWA_PYTHON"; exit 1; }
echo "python: $PY ($("$PY" --version 2>&1))"

# The pinned deps (flatbuffers 1.12, zlib, pffft, mysofa) declare pre-3.5 cmake_minimum_required,
# which CMake 4 refuses outright. This env var (honored by CMake >= 3.31.7 / 4.0) is the official
# escape hatch. It lives here rather than in a caller because every caller needs it: the manylinux
# image ships CMake 4.
export CMAKE_POLICY_VERSION_MINIMUM="${CMAKE_POLICY_VERSION_MINIMUM:-3.5}"

# -t is a WINDOWS-ONLY argument in BOTH pinned scripts. Their argparse accepts only the vs20xx
# values, so "make" or "ndk" is an argparse ERROR, not a no-op, and off Windows the flag buys
# nothing anyway: build.py picks Unix Makefiles for linux and android and Xcode for osx from -p
# alone, and get_dependencies.py does the same. So the flag travels only on the windows branch,
# and BWA_TOOLCHAIN is ignored elsewhere.
TOPT=""
if [ "$BWA_PLATFORM" = "windows" ]; then TOPT="-t ${BWA_TOOLCHAIN:-vs2022}"; fi

# A fresh CI checkout has no submodule (the jobs check out without one and fetch it only on a
# phonon cache miss), so this is where it arrives. A tree that ALREADY carries the sources keeps
# them: that is a developer's checkout, and it is also the copy cibuildwheel puts inside the
# manylinux container, where the submodule is already populated and the git metadata that a
# submodule update would need is not worth trusting.
if [ ! -f third_party/steam-audio-source/core/build/build.py ]; then
  git submodule update --init --depth 1 third_party/steam-audio-source \
    || git submodule update --init third_party/steam-audio-source
fi

# One caller may run this more than once on one checkout (the android job builds two ABIs), and
# the second call finds the tree already patched, where a plain `git apply` fails with "patch does
# not apply". So: apply when it applies, skip when it is already in, and fail only when it is
# neither. `git apply` needs no repository, which is what makes this work inside the container AND
# on the downloaded dependency tree below, which is a plain directory of copied headers.
#
# apply_once <tree> <patch> [<subdir>]. The patch path is absolute so one function serves trees at
# two different depths. <tree> is a directory git runs IN; <subdir>, when given, is prepended to the
# patch's own a/ and b/ paths (git apply --directory).
#
# WHY THE THIRD ARGUMENT EXISTS, because the bug it fixes passed CI's own check: `git apply` run
# INSIDE a repository resolves the patch's paths against the repository root, and "patched paths
# outside the current directory are ignored". core/deps/flatbuffers is inside the submodule's
# repository, so `git -C core/deps/flatbuffers apply` found no path under its directory, ignored the
# whole patch, exited 0 for --check and for the apply, and printed "applied" while the header stayed
# unpatched - phonon then failed on the exact line the patch removes (Pages run 35716986768). A
# scratch copy with no .git behaved like plain patch(1) and hid it. Run from the submodule root with
# --directory=core/deps/flatbuffers and the same file is reached either way. The caller ALSO greps
# the result, because a check that can pass vacuously is not a check.
apply_once() {
  local tree="$1" name; name="$(basename "$2")"
  local abs="$ROOT/third_party/patches/$name"
  local dopt=""
  if [ -n "${3:-}" ]; then dopt="--directory=$3"; fi
  if git -C "$tree" apply $dopt --check "$abs" 2>/dev/null; then
    git -C "$tree" apply $dopt "$abs"; echo "applied $name"
  elif git -C "$tree" apply $dopt --reverse --check "$abs" 2>/dev/null; then
    echo "already applied $name"
  else
    echo "patch $name neither applies nor is already applied (tree: $tree${3:+/$3})"; exit 1
  fi
}
SUBMODULE=third_party/steam-audio-source
CORE="$SUBMODULE/core"
# REQUIRED for directional reflections (upstream steam-audio#546):
apply_once "$SUBMODULE" third_party/patches/phonon-multiplyaccumulate-align.patch

if [ "$BWA_PLATFORM" = "android" ]; then
  # Android needs more of the pinned scripts fixed than any other platform: they were written for
  # a Windows host and for one spelling of the arm64 target. Each patch says what it fixes; all of
  # them are android-only, so no other platform applies them.
  for p in third_party/patches/phonon-android-*.patch; do
    apply_once "$SUBMODULE" "$p"
  done
  # Both scripts read the NDK from here (get_dependencies.py's --ndk defaults to it), and
  # get_dependencies.py exits 1 without it. Fail on our own message rather than on theirs.
  test -n "${ANDROID_NDK:-}" || { echo "android: set ANDROID_NDK before calling this script"; exit 1; }
  echo "ANDROID_NDK=$ANDROID_NDK"
fi

if [ "$BWA_PLATFORM" = "wasm" ]; then
  # build.py reads EMSDK with no default and concatenates it into the toolchain-file path, so an
  # unset one is a TypeError deep inside their script. Say it here instead. emcc itself has to be
  # on PATH for the companions, which get_dependencies.py builds through the same toolchain file.
  test -n "${EMSDK:-}" || { echo "wasm: set EMSDK (run inside emscripten/emsdk, or source emsdk_env.sh)"; exit 1; }
  command -v emcc >/dev/null 2>&1 || { echo "wasm: emcc is not on PATH"; exit 1; }
  echo "EMSDK=$EMSDK  ($(emcc --version | head -1))"
  # -pthread on BOTH halves; see the WASM NOTE in this file's header for why it is mandatory.
  # The companions have no flag argument, so the environment is the only lever on them, and the
  # phonon configure below takes it as -D flags. Ours go FIRST so a caller's BWA_CMAKE_FLAGS can
  # still override them: for a repeated -D, CMake keeps the last.
  export CFLAGS="${CFLAGS:-} -pthread"
  export CXXFLAGS="${CXXFLAGS:-} -pthread"
  BWA_CMAKE_FLAGS="-DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread $BWA_CMAKE_FLAGS"
fi

if [ "$BWA_PLATFORM" = "windows" ]; then
  # The pinned build scripts hard-code "Visual Studio 17 2022"; rewrite that to CMake's default
  # generator (= the VS this machine actually has, and the same generator our own Configure step
  # will use).
  GEN="$(cmake --help | sed -n 's/^\* \(Visual Studio [0-9]* [0-9]*\).*/\1/p' | head -1)"
  test -n "$GEN"
  echo "using generator: $GEN"
  sed -i "s/Visual Studio 17 2022/$GEN/g" \
      third_party/steam-audio-source/core/build/build.py \
      third_party/steam-audio-source/core/build/get_dependencies.py
  SHAREDCRT="--sharedcrt"     # everything /MD; see the CRT note in this file's header
else
  SHAREDCRT=""
fi

cd "$CORE/build"
# flatbuffers is a BUILD TOOL (flatc), so it never takes the CRT flag and never links in.
# gcc 13 (ubuntu-latest) flags a false positive in flatbuffers 1.12's reflection.cpp
# (-Werror=stringop-overflow inside <bits/stl_algobase.h>), and flatbuffers builds itself with
# -Werror. Reproduced on gcc 13.3 and absent on gcc 14 and clang. Demote that one warning for the
# host tool only: the env var reaches flatbuffers' fresh configure and nothing else, so phonon and
# its companions keep their own flags.
if [ "$BWA_PLATFORM" = "windows" ]; then
  "$PY" get_dependencies.py --dependency flatbuffers -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
else
  CXXFLAGS="${CXXFLAGS:-} -Wno-error=stringop-overflow" \
    "$PY" get_dependencies.py --dependency flatbuffers -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
fi
# THE ONE PATCH ON A DOWNLOADED DEPENDENCY, and the reason this step is here rather than beside
# the submodule ones at the top: the tree it lands on does not exist until get_dependencies.py has
# cloned, built and copied it. The target is the COPIED include tree, which is what phonon then
# compiles against; flatc itself builds either way. The patch file says what it fixes.
apply_once "$SUBMODULE" third_party/patches/deps-flatbuffers-tablekeycomparator.patch core/deps/flatbuffers
# Prove it landed. The one line the patch introduces; absent means the apply was a no-op.
grep -q "TableKeyComparator &operator=(const TableKeyComparator &other));"   "$CORE/deps/flatbuffers/include/flatbuffers/flatbuffers.h"   || { echo "deps-flatbuffers patch reported applied but the header is unchanged"; exit 1; }
for d in zlib "$BWA_FFT" mysofa; do
  "$PY" get_dependencies.py --dependency "$d" $SHAREDCRT -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
done

# --minimal drops Embree / IPP / GPU / sample apps. Generate only: the configure below adds the
# two settings that make this a STATIC, /MD phonon.
"$PY" build.py -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT -c release --minimal -o generate

# build.py makes its tree in the CWD, and a single-config generator appends the config
# (linux-x64-release, not linux-x64). Don't guess - locate the CMakeCache. Prefer a tree whose
# name carries THIS platform and arch: Android calls this twice in one checkout, once per ABI, so
# "the first CMakeCache" would hand the second call the first ABI's tree and stage one ABI's
# archives twice.
# Not every platform puts the arch in the name, though: build.py names the osx, ios and wasm trees
# from -p and the config alone (wasm-release, not wasm-x64-release), so an arch in the pattern
# matches nothing there and the match has to drop it. Those three build one tree per checkout, so
# dropping it costs no precision.
case "$BWA_PLATFORM" in
  osx|ios|wasm) TREEPAT="$BWA_PLATFORM*" ;;
  *)            TREEPAT="$BWA_PLATFORM*$BWA_ARCH*" ;;
esac
TREE=""
for d in $(find . -maxdepth 3 -name CMakeCache.txt); do
  case "$(basename "$(dirname "$d")")" in
    $TREEPAT) TREE="$(dirname "$d")"; break ;;
  esac
done
if [ -z "$TREE" ]; then TREE=$(dirname "$(find . -maxdepth 3 -name CMakeCache.txt | head -1)"); fi
test -n "$TREE" && test -d "$TREE" || { echo "generated tree not found:"; ls; exit 1; }
echo "phonon build tree: $TREE"
# macOS: phonon's core/CMakeLists.txt sets CMAKE_OSX_ARCHITECTURES with a plain set() AFTER
# project(), which CMake ignores for languages already enabled, so the core archive comes out
# host-arch only (arm64 on macos-latest) while its companions, which get the flag on the command
# line, come out universal. The engine links universal, so say it here, where it counts, and prove
# it below with lipo.
OSXARCH=""
if [ "$BWA_PLATFORM" = "osx" ]; then OSXARCH="-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64"; fi
cmake -DSTEAMAUDIO_STATIC_RUNTIME=OFF -DBUILD_SHARED_LIBS=OFF $OSXARCH $BWA_CMAKE_FLAGS "$TREE"
cmake --build "$TREE" --config Release --target phonon
PHONON_TREE="$(cd "$TREE" && pwd)"
cd "$ROOT"

# Stage where the root CMakeLists auto-detects it. find: the build-tree layout is phonon's
# business; only the staged layout is ours (third_party/README.md).
mkdir -p "$BWA_ART/include" "$BWA_ART/lib/$BWA_STAGE_DIR"
cp "$CORE/src/core/phonon.h"                                 "$BWA_ART/include/"
cp "$(find "$PHONON_TREE" -name phonon_version.h | head -1)"  "$BWA_ART/include/"

# Where get_dependencies.py put THIS target's companions. It names that directory after the target
# platform, and spells 64-bit ARM Android "armv8" where build.py says "arm64".
case "$BWA_PLATFORM" in
  osx|ios|wasm) DEPPLAT="$BWA_PLATFORM" ;;
  android)      if [ "$BWA_ARCH" = "arm64" ]; then DEPPLAT=android-armv8; else DEPPLAT="android-$BWA_ARCH"; fi ;;
  *)            DEPPLAT="$BWA_PLATFORM-$BWA_ARCH" ;;
esac

if [ "$BWA_PLATFORM" = "windows" ]; then
  NAMES="phonon.lib mysofa.lib zlibstatic.lib ${BWA_FFT}.lib"
else
  NAMES="libphonon.a libmysofa.a libz.a lib${BWA_FFT}.a"
fi
for n in $NAMES; do
  # This build tree first (phonon's own archive), then this target's dependency directory. Both
  # are scoped, for the same reason TREE is: a second ABI must not stage the first's. Xcode also
  # keeps a per-architecture INTERMEDIATE of every archive under Objects-normal/<arch>/ beside the
  # fat one it lipo-creates; a bare find | head -1 staged the arm64 intermediate on macOS and the
  # universal check below caught it. Skip those.
  src="$(find "$PHONON_TREE" -name "$n" -type f -not -path "*/Objects-normal/*" | head -1)"
  if [ -z "$src" ]; then
    src="$(find "$CORE/deps" -path "*/$DEPPLAT/*" -name "$n" -type f -not -path "*/Objects-normal/*" | head -1)"
  fi
  test -n "$src" || { echo "staging FAILED: $n was not built ($DEPPLAT)"; exit 1; }
  echo "staging $n <- $src"
  cp "$src" "$BWA_ART/lib/$BWA_STAGE_DIR/"
done

# A half-staged tree must never reach the cache: the consuming configure treats a present but
# incomplete lib/<platform> as a fatal error, and a cache hit would make it permanent.
test -f "$BWA_ART/include/phonon_version.h"
for n in $NAMES; do test -f "$BWA_ART/lib/$BWA_STAGE_DIR/$n"; done
ls -l "$BWA_ART/lib/$BWA_STAGE_DIR"
# A universal stage must hold BOTH slices in EVERY archive, or the engine's x86_64 link fails with
# "found architecture 'arm64', required architecture 'x86_64'" far from here.
if [ "$BWA_PLATFORM" = "osx" ]; then
  for n in $NAMES; do
    f="$BWA_ART/lib/$BWA_STAGE_DIR/$n"
    lipo -info "$f"
    lipo -info "$f" | grep -q x86_64 || { echo "staging FAILED: $n has no x86_64 slice"; exit 1; }
    lipo -info "$f" | grep -q arm64  || { echo "staging FAILED: $n has no arm64 slice"; exit 1; }
  done
fi
