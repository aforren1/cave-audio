#!/usr/bin/env bash
# cibuildwheel CIBW_BEFORE_ALL_LINUX: prepare the manylinux container for a bw_audio wheel.
#
# It runs ONCE per container, before any wheel is built, inside the image named by
# [tool.cibuildwheel] manylinux-x86_64-image. Everything the wheel links must be built HERE, by
# the container's own toolset, or the wheel is manylinux in name only.
#
# Three jobs:
#   1. the two device backends' headers (ALSA and JACK), so the wheel's engine carries the same
#      backends the Linux CI build does;
#   2. a `python` on PATH, because the pinned Steam Audio scripts are driven as `python` and the
#      image's /usr/bin/python3 is 3.6;
#   3. a static phonon, built by tools/phonon/build-phonon.sh - the same recipe the composite
#      action runs, which is why that recipe is a script and not the action's own shell body.
#
# THE STALE-STAGING TRAP. cibuildwheel copies the whole project directory into the container, so a
# developer's own third_party/steam-audio-artifacts/lib/linux-x64 arrives with it, built by
# whatever gcc that machine has. Those archives link (a static archive is happy to), and the wheel
# then carries a phonon compiled against a NEWER libstdc++ than the manylinux policy allows, which
# auditwheel reports far from the cause. So the staging is trusted only when it carries a stamp
# naming the image and compiler that produced it. No stamp, or a different one, and the whole
# lib/linux-x64 is deleted and rebuilt. A host's archives can never satisfy the check.

set -eu

# cibuildwheel mounts the checkout at /project and runs this as root, while the files belong to
# the runner's user; git refuses such a repository ("dubious ownership") and the recipe's
# submodule and patch steps go through git. Trust every path in this throwaway container. (Docker
# Desktop on Windows presents the mount as root-owned, which is why a local run never showed it.)
git config --global --add safe.directory '*'

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# ---- 1. the Linux backends' dev packages -----------------------------------------------------
# alsa-lib-devel is in AppStream and jack-audio-connection-kit-devel is in EPEL; the manylinux_2_28
# image enables both repositories already. The wheel links libasound and libjack rather than
# dlopening them, so a missing header here does not fail the build - it silently drops a backend.
# Hence the pkg-config assertions below: a wheel with no JACK and no ALSA must fail the job.
dnf -y install alsa-lib-devel jack-audio-connection-kit-devel
pkg-config --exists alsa || { echo "ALSA dev package missing after install"; exit 1; }
pkg-config --exists jack || { echo "JACK dev package missing after install"; exit 1; }
echo "alsa $(pkg-config --modversion alsa), jack $(pkg-config --modversion jack)"

# ---- 2. `python` on PATH ---------------------------------------------------------------------
# Both pinned Steam Audio scripts are invoked as `python`, and the image's system interpreter is
# 3.6. The wheel's own interpreter is chosen per build by cibuildwheel and is not on PATH here, so
# point `python` at the 3.12 the image ships.
if ! command -v python >/dev/null 2>&1; then
  ln -sf /opt/python/cp312-cp312/bin/python /usr/local/bin/python
fi
echo "python: $(command -v python) ($(python -V 2>&1))"

# ---- 3. static phonon, built in this container ------------------------------------------------
STAGE=third_party/steam-audio-artifacts/lib/linux-x64
STAMP="$STAGE/.phonon-built-by"
# The image's platform tag plus the compiler that will also build the engine. Both matter: the tag
# is the glibc floor, and the compiler is the libstdc++ the archives were produced against. Only
# a manylinux image sets AUDITWHEEL_PLAT, so a developer's own staging can never match whatever
# this says. The compiler is the MAJOR version (-dumpversion, not -dumpfullversion), which is the
# version libstdc++ compatibility actually turns on: CI primes this cache with its own
# `docker run` and cibuildwheel then runs the container itself, and a stamp sensitive to the patch
# release would rebuild phonon on any drift between those two references. A toolset change still
# misses, which is the case that matters.
WANT="phonon-v1 ${AUDITWHEEL_PLAT:-unknown-image} gcc-$(gcc -dumpversion 2>/dev/null || echo unknown)"

have_all=1
for n in libphonon.a libmysofa.a libz.a libpffft.a; do
  [ -f "$STAGE/$n" ] || have_all=0
done
if [ "$have_all" = 1 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ]; then
  echo "phonon: reusing the staged archives, built by [$WANT]"
  exit 0
fi

if [ -e "$STAGE" ]; then
  echo "phonon: discarding $STAGE - stamp [$( [ -f "$STAMP" ] && cat "$STAMP" || echo none )] is not [$WANT]"
  rm -rf "$STAGE"
fi
# And any build tree or dependency the host left for this target, for the same reason:
# get_dependencies.py and build.py both reuse what they find.
rm -rf third_party/steam-audio-source/core/build/linux-x64-release
rm -rf third_party/steam-audio-source/core/deps/*/lib/linux-x64

# STEAMAUDIO_ENABLE_AVX=OFF: the default ON adds -fabi-version=6, which breaks <future> inside
# hrtf.cpp on gcc 13 and older (third_party/README.md has the measurement). Today's
# manylinux_2_28 image carries gcc 14 and would compile it either way, but an older image release
# carries gcc-toolset-12 or 13 and does not, and OFF is the one setting that builds in all of
# them. It costs phonon's AVX paths, which the ubuntu-latest wheel gives up too.
BWA_PLATFORM=linux BWA_ARCH=x64 BWA_STAGE_DIR=linux-x64 \
BWA_CMAKE_FLAGS=-DSTEAMAUDIO_ENABLE_AVX=OFF \
  bash tools/phonon/build-phonon.sh

printf '%s\n' "$WANT" > "$STAMP"
echo "phonon: staged $STAGE, built by [$WANT]"
