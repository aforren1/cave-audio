#!/usr/bin/env bash
# Assemble the GitHub Pages artifact: the web build's dist tree, plus the deploy shell and the
# COOP/COEP service worker overlaid at its root.
#
# THE ARTIFACT IS THE WHOLE SITE. Pages serves it as-is and sends no headers of its own, so
# cross-origin isolation comes from coi-serviceworker.js, and every asset the pages load has to be
# SAME-ORIGIN or carry Cross-Origin-Resource-Policy: under COEP require-corp a cross-origin script,
# style, font or wasm without it is blocked. That is why this script copies a tree rather than
# rewriting URLs, and why the check at the end looks for cross-origin subresources. README.md
# beside this file states the rule for whoever adds the next page.
#
# ENVIRONMENT
#   BWA_WEB_DIST   the web build's self-contained output.   (default: bindings/web/dist)
#   BWA_WEB_OUT    the artifact tree to write. REPLACED.    (default: bindings/web/_site)
#
# Callers: .github/workflows/pages.yml, and a developer checking the layout by hand. Run it from
# anywhere; it finds the repo root from its own path.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

DIST="${BWA_WEB_DIST:-bindings/web/dist}"
OUT="${BWA_WEB_OUT:-bindings/web/_site}"

# A missing dist is the one failure worth being loud about: it means the web build did not run, or
# it wrote somewhere else, and deploying the shell alone would publish a page whose only link 404s.
if [ ! -d "$DIST" ]; then
  echo "::error::no web build at $DIST - run tools/wasm/build-web.sh (or set BWA_WEB_DIST) first"
  exit 1
fi
if [ -z "$(ls -A "$DIST")" ]; then
  echo "::error::$DIST is empty"
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/dist" "$OUT/example"
# The site root MIRRORS bindings/web: dist/ and example/ side by side, so the example page's
# `../dist/index.js` import and its `../coi-serviceworker.js` script tag resolve the same way they
# do under example/serve.mjs (which serves the repo root). Rewriting paths at stage time would be a
# second copy of the page's layout. -a would carry ownership and timestamps that
# upload-pages-artifact has no use for; contents and the directory shape are the whole payload.
cp -R "$DIST"/. "$OUT/dist"/
cp -R "$HERE/../example"/. "$OUT/example"/
rm -f "$OUT/example/serve.mjs"   # the local dev server is not a page asset

# The overlay, and it OVERWRITES: the deploy shell owns the site root, because the service worker
# registers from its own URL and only a root registration scopes over example/.
if [ -f "$OUT/index.html" ]; then
  echo "note: $DIST carried a root index.html; the deploy shell replaces it"
fi
cp "$HERE/index.html" "$OUT/index.html"
cp "$HERE/coi-serviceworker.js" "$OUT/coi-serviceworker.js"
# The MIT text travels with the file it covers.
cp "$HERE/coi-serviceworker.LICENSE" "$OUT/coi-serviceworker.LICENSE"

# Pages runs Jekyll over an artifact unless this file is there, and Jekyll SKIPS every path that
# starts with an underscore. Emscripten does not emit one today, but a build that ever does would
# lose it silently.
: > "$OUT/.nojekyll"

# The demo page the shell links to. Not fatal - the build may name it differently and the shell
# still reports isolation - but a broken link is the first thing anyone sees.
if [ ! -f "$OUT/example/index.html" ]; then
  echo "::warning::no example/index.html in the artifact; the shell's demo link will 404"
elif ! grep -q "coi-serviceworker.js" "$OUT/example/index.html"; then
  # A visitor who lands on example/index.html FIRST (a deep link, a bookmark) has no service
  # worker registered yet, so that page is not isolated and the engine cannot start. The fix is
  # one script tag in the example page; the shell cannot register on its behalf.
  echo "::warning::example/index.html does not load ../coi-serviceworker.js - a deep link to it will not be cross-origin isolated"
fi

# The same-origin rule, checked rather than asserted in prose. hrefs are deliberately not matched:
# a LINK to another site is a navigation, not a subresource, and COEP does not touch it.
hits=$(grep -rIn -E '(src|href)[[:space:]]*=[[:space:]]*"https?://|from[[:space:]]+"https?://|importScripts\([[:space:]]*"https?://|url\([[:space:]]*"?https?://' \
        --include='*.html' --include='*.js' --include='*.css' --include='*.mjs' "$OUT" \
        | grep -v -E '(href)[[:space:]]*=[[:space:]]*"https?://' || true)
if [ -n "$hits" ]; then
  echo "::warning::cross-origin subresources in the artifact; under COEP require-corp each one is blocked unless it sends Cross-Origin-Resource-Policy. Vendor them instead."
  echo "$hits"
fi

echo "== staged $OUT =="
find "$OUT" -type f | sed "s|^$OUT/||" | sort
du -sh "$OUT" | cut -f1 | sed 's/^/total /'
