#!/usr/bin/env bash
# Fetch the web demo's third-party browser assets into bindings/web/dist/vendor/.
#
# WHY A FETCH AND NOT A COMMITTED FILE. three.js is 2.1 MB of unminified JavaScript and the
# playground is the only thing in the repository that wants it. bindings/web/dist/ is already a
# BUILD OUTPUT (it is gitignored, tools/wasm/build-web.sh writes it, bindings/web/deploy/stage.sh
# copies it into the Pages artifact), so a file staged there costs the repository nothing and still
# reaches the site. The pages workflow runs in a container with network, which is where this runs.
#
# WHY NOT A CDN. The site is served under COEP require-corp, where a cross-origin subresource
# without Cross-Origin-Resource-Policy is BLOCKED, not slowed. bindings/web/deploy/README.md states
# the rule; stage.sh greps the staged tree for violations. Vendor it, do not link it.
#
# PINNED AND HASHED. The version is a literal below and every file is checked against a sha256
# recorded here, so a compromised or re-published upstream file cannot land in the artifact
# silently. To move to a newer three.js: bump THREE_VERSION, run with BWA_VENDOR_PRINT=1, and paste
# the hashes it prints.
#
# SEPARATE FROM build-web.sh on purpose: that script needs an Emscripten toolchain and this one
# needs only curl, so a developer who did not rebuild the engine can still fill vendor/ (and the
# playground's ctest can say which of the two is missing).
#
# ENVIRONMENT
#   BWA_WEB_DIST      where dist/ lives.                    (default: bindings/web/dist)
#   BWA_VENDOR_PRINT  1: print each file's sha256 and skip the comparison (for a version bump).
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

DIST="${BWA_WEB_DIST:-$ROOT/bindings/web/dist}"
OUT="$DIST/vendor/three"

THREE_VERSION="0.186.0"
# path-under-the-npm-package  sha256  local-name
THREE_FILES="
build/three.module.js 9052042d676cb0fdc1ddfefe193053f34b7ac0513a616fdac4535d49987812ea three.module.js
build/three.core.js   9edde002b066a9a05676a6127f67735b62baf399bdea529f2f7e31657da769e6 three.core.js
LICENSE               8b378ebe60e2fe500158cb0ac71cb5e8b7d92953c2abcc63a0eb90499653b5bc LICENSE
"

sha_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  else echo "fetch-web-vendor: no sha256sum or shasum on PATH" >&2; exit 1
  fi
}

mkdir -p "$OUT"
echo "fetch-web-vendor: three.js $THREE_VERSION -> $OUT"

printf '%s\n' "$THREE_FILES" | while read -r path want name; do
  [ -n "${path:-}" ] || continue
  dest="$OUT/$name"
  # Already correct: leave it alone. The script runs on every build, and re-downloading 2 MB on a
  # warm tree would make an offline rebuild fail for no reason.
  if [ -f "$dest" ] && [ "${BWA_VENDOR_PRINT:-0}" != "1" ] && [ "$(sha_of "$dest")" = "$want" ]; then
    echo "  ok      $name"
    continue
  fi
  url="https://unpkg.com/three@$THREE_VERSION/$path"
  curl -sSL --fail --max-time 300 -o "$dest.tmp" "$url"
  got="$(sha_of "$dest.tmp")"
  if [ "${BWA_VENDOR_PRINT:-0}" = "1" ]; then
    echo "  $path $got"
  elif [ "$got" != "$want" ]; then
    rm -f "$dest.tmp"
    echo "fetch-web-vendor: sha256 mismatch for $url" >&2
    echo "  expected $want" >&2
    echo "  got      $got" >&2
    exit 1
  fi
  mv -f "$dest.tmp" "$dest"
  echo "  fetched $name"
done

# A record the page can read back, so a bug report says which three.js it ran.
printf '{ "three": "%s" }\n' "$THREE_VERSION" > "$DIST/vendor/versions.json"
ls -l "$OUT"
