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
#   BWA_PLATFORM    build.py -p value: windows | linux | osx | android.          (required)
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
#   ANDROID_NDK     required when BWA_PLATFORM is android; both pinned scripts read it.
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

: "${BWA_PLATFORM:?set BWA_PLATFORM (windows|linux|osx|android)}"
: "${BWA_STAGE_DIR:?set BWA_STAGE_DIR (windows-x64|linux-x64|osx-universal|android-arm64|...)}"
BWA_ARCH="${BWA_ARCH:-x64}"
BWA_TOOLCHAIN="${BWA_TOOLCHAIN:-}"
BWA_FFT="${BWA_FFT:-pffft}"
BWA_CMAKE_FLAGS="${BWA_CMAKE_FLAGS:-}"
BWA_ART="${BWA_ART:-third_party/steam-audio-artifacts}"

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
# the second call finds the submodule already patched, where a plain `git apply` fails with "patch
# does not apply". So: apply when it applies, skip when it is already in, and fail only when it is
# neither. `git apply` needs no repository, which is what makes this work inside the container.
apply_once() {
  local rel="../patches/$(basename "$1")"
  if git -C third_party/steam-audio-source apply --check "$rel" 2>/dev/null; then
    git -C third_party/steam-audio-source apply "$rel"; echo "applied $(basename "$1")"
  elif git -C third_party/steam-audio-source apply --reverse --check "$rel" 2>/dev/null; then
    echo "already applied $(basename "$1")"
  else
    echo "patch $(basename "$1") neither applies nor is already applied"; exit 1
  fi
}
# REQUIRED for directional reflections (upstream steam-audio#546):
apply_once third_party/patches/phonon-multiplyaccumulate-align.patch

if [ "$BWA_PLATFORM" = "android" ]; then
  # Android needs more of the pinned scripts fixed than any other platform: they were written for
  # a Windows host and for one spelling of the arm64 target. Each patch says what it fixes; all of
  # them are android-only, so no other platform applies them.
  for p in third_party/patches/phonon-android-*.patch; do
    apply_once "$p"
  done
  # Both scripts read the NDK from here (get_dependencies.py's --ndk defaults to it), and
  # get_dependencies.py exits 1 without it. Fail on our own message rather than on theirs.
  test -n "${ANDROID_NDK:-}" || { echo "android: set ANDROID_NDK before calling this script"; exit 1; }
  echo "ANDROID_NDK=$ANDROID_NDK"
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

cd third_party/steam-audio-source/core/build
# flatbuffers is a BUILD TOOL (flatc), so it never takes the CRT flag and never links in.
# gcc 13 (ubuntu-latest) flags a false positive in flatbuffers 1.12's reflection.cpp
# (-Werror=stringop-overflow inside <bits/stl_algobase.h>), and flatbuffers builds itself with
# -Werror. Reproduced on gcc 13.3 and absent on gcc 14 and clang. Demote that one warning for the
# host tool only: the env var reaches flatbuffers' fresh configure and nothing else, so phonon and
# its companions keep their own flags.
if [ "$BWA_PLATFORM" = "windows" ]; then
  python get_dependencies.py --dependency flatbuffers -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
else
  CXXFLAGS="${CXXFLAGS:-} -Wno-error=stringop-overflow" \
    python get_dependencies.py --dependency flatbuffers -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
fi
for d in zlib "$BWA_FFT" mysofa; do
  python get_dependencies.py --dependency "$d" $SHAREDCRT -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT
done

# --minimal drops Embree / IPP / GPU / sample apps. Generate only: the configure below adds the
# two settings that make this a STATIC, /MD phonon.
python build.py -p "$BWA_PLATFORM" -a "$BWA_ARCH" $TOPT -c release --minimal -o generate

# build.py makes its tree in the CWD, and a single-config generator appends the config
# (linux-x64-release, not linux-x64). Don't guess - locate the CMakeCache. Prefer a tree whose
# name carries THIS platform and arch: Android calls this twice in one checkout, once per ABI, so
# "the first CMakeCache" would hand the second call the first ABI's tree and stage one ABI's
# archives twice.
TREE=""
for d in $(find . -maxdepth 3 -name CMakeCache.txt); do
  case "$(basename "$(dirname "$d")")" in
    "$BWA_PLATFORM"*"$BWA_ARCH"*) TREE="$(dirname "$d")"; break ;;
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
CORE=third_party/steam-audio-source/core
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
