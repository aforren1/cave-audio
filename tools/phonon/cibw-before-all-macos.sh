#!/usr/bin/env bash
# cibuildwheel CIBW_BEFORE_ALL_MACOS: make sure a UNIVERSAL static phonon is staged.
#
# macOS has no container, so this runs on the runner itself and there is no stale-image question
# the Linux side has to answer: whatever built the archives is the machine that will link them.
# The wheels job restores the macos job's phonon cache before cibuildwheel starts, so the normal
# path here is "already staged, do nothing"; the build below is the cache-miss path.
#
# It needs `python` on PATH (both pinned Steam Audio scripts are driven as `python`, never
# `python3`), which is why the job sets up a Python before calling cibuildwheel.

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

STAGE=third_party/steam-audio-artifacts/lib/osx-universal
have_all=1
for n in libphonon.a libmysofa.a libz.a libpffft.a; do
  [ -f "$STAGE/$n" ] || have_all=0
done
if [ "$have_all" = 1 ]; then
  echo "phonon: already staged at $STAGE"
  # Both slices, or the universal2 link fails far from here. build-phonon.sh checks this after a
  # build; check it again after a RESTORE, because a cache is the other way a half-right stage
  # arrives.
  for n in libphonon.a libmysofa.a libz.a libpffft.a; do
    lipo -info "$STAGE/$n"
    lipo -info "$STAGE/$n" | grep -q x86_64 || { echo "$n has no x86_64 slice"; exit 1; }
    lipo -info "$STAGE/$n" | grep -q arm64  || { echo "$n has no arm64 slice"; exit 1; }
  done
  exit 0
fi

# No AVX flag: -fabi-version=6 is a Linux-only branch in phonon's core/CMakeLists.txt, and Apple
# clang never sees it.
BWA_PLATFORM=osx BWA_ARCH=x64 BWA_STAGE_DIR=osx-universal \
  bash tools/phonon/build-phonon.sh
