#!/usr/bin/env bash
# Build, TEST and install the Linux engine inside the manylinux_2_28 container.
#
# WHY A CONTAINER AND NOT THE RUNNER. A manylinux_2_28 wheel needs an engine built against glibc
# 2.28, and ubuntu-latest carries 2.39. Until this existed the repository built TWO Linux engines:
# one on the runner for ctest, the MEX pair and the Godot addon, and a second inside cibuildwheel's
# container for the release wheel - so the file a stranger pip-installed was the one nothing had
# tested. Building the one engine here instead makes the tested binary and the shipped binary the
# same file, and every Linux package then links it. Bindings are ABI clients: a MEX or a
# GDExtension compiled on the runner against a glibc-2.28 shared object is fine, and it is the only
# arrangement that works, because neither MATLAB nor the Godot toolchain installs in AlmaLinux 8.
#
# It runs as ROOT with the workspace bind-mounted, so everything it writes (the build tree, the
# SDK, the phonon staging) is root-owned. Every later step on the runner only READS those, and
# writes into trees of its own - which is exactly why the bindings moved to -DBWA_ENGINE_SDK.
#
# ENVIRONMENT
#   BWA_BUILD_DIR   the build tree to write.                            (default: build)
#   BWA_SDK_DIR     the install prefix for the engine SDK.              (default: engine-sdk)
#
# Callers: .github/workflows/ci.yml, the linux job. Run it as
#   docker run --rm -v "$PWD:/project" -w /project <image> bash tools/ci/build-engine-manylinux.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="${BWA_BUILD_DIR:-build}"
SDK="${BWA_SDK_DIR:-engine-sdk}"

# The container's own preparation: the two device backends' headers, a `python` on PATH, and a
# static phonon built by this image's gcc. It is the SAME script cibuildwheel runs as its
# before-all, and it is idempotent - a phonon whose stamp names this image is reused, so a restored
# cache costs one `dnf install` and nothing else. It also marks the mounted workspace safe for git,
# which every step below inherits.
bash tools/phonon/cibw-before-all-linux.sh

echo "== toolchain =="
gcc --version | head -1
cmake --version | head -1
ldd --version | head -1

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBWA_BUILD_TESTS=ON | tee configure.log
# The same four assertions the runner build used to make. Each one guards a silent degradation:
# a missing header drops a backend, and a phonon staging that did not survive the cache round trip
# drops the HRTF decode, occlusion, pathing and the reflection bed - and every test still passes.
grep -q "Linux - JACK and ALSA backends available" configure.log
grep -q "JACK backend ENABLED" configure.log
grep -q "ALSA backend ENABLED" configure.log
grep -q "Steam Audio ENABLED" configure.log

cmake --build "$BUILD" -j "$(nproc)"

# 38: the 33 a default off-Windows build registers plus the five SDK-gated ones. The device
# sections of audio_sink find no JACK server and no sound card here, exactly as on the runner, and
# report SKIPPED rather than passing.
ctest --test-dir "$BUILD" --output-on-failure

# The SDK has to be cut HERE, not on the runner: cmake_install.cmake names absolute source paths,
# and those are the container's.
cmake --install "$BUILD" --prefix "$SDK" --component bwa_sdk
test -s "$SDK/lib/libbw_audio.so"
test -s "$SDK/include/bw_audio.h"
test -s "$SDK/lib/cmake/bw_audio/bw_audioConfig.cmake"
cmp "$BUILD/libbw_audio.so" "$SDK/lib/libbw_audio.so"
echo "engine SDK installed at $SDK"
