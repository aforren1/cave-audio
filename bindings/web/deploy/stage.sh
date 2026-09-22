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
mkdir -p "$OUT/dist" "$OUT/example" "$OUT/playground"
# The site root MIRRORS bindings/web: dist/, example/ and playground/ side by side, so each page's
# `../dist/index.js` import and its `../coi-serviceworker.js` script tag resolve the same way they
# do under example/serve.mjs (which serves the repo root). Rewriting paths at stage time would be a
# second copy of the pages' layout. -a would carry ownership and timestamps that
# upload-pages-artifact has no use for; contents and the directory shape are the whole payload.
#
# dist/ carries vendor/ too (three.js, fetched and hashed by tools/wasm/fetch-web-vendor.sh). The
# playground imports it from there rather than from a CDN, which under COEP require-corp is the
# difference between a page and a blank screen.
cp -R "$DIST"/. "$OUT/dist"/
cp -R "$HERE/../example"/. "$OUT/example"/
rm -f "$OUT/example/serve.mjs"   # the local dev server is not a page asset
cp -R "$HERE/../playground"/. "$OUT/playground"/

# CONTENT-ADDRESS dist/. GitHub Pages serves everything with `Cache-Control: max-age=600`, and a
# returning visitor's browser keeps whatever it fetched inside that window. The first playground
# deploy hit exactly this: the visitor's cached client.js came from the deploy BEFORE the one that
# added invokeBuf, the new playground/rig.js had never been cached, and the page died on
# "this.engine.invokeBuf is not a function". The dangerous form is bw_audio.mjs and bw_audio.wasm
# from two different builds, which does not throw a readable error at all. So dist/ is staged
# under a name derived from its own contents, and the pages' `../dist/` imports are rewritten to
# match: a changed build is a new URL, an unchanged one is the same URL and stays cacheable. The
# glue itself resolves its side files relative to import.meta.url, so nothing inside dist/ needs
# rewriting, and the pages reference dist/ only through that one `../dist/` prefix (checked below).
hash=$(cd "$OUT/dist" && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12)
DISTDIR="dist-$hash"
mv "$OUT/dist" "$OUT/$DISTDIR"
for f in $(find "$OUT/example" "$OUT/playground" -type f \( -name '*.html' -o -name '*.js' -o -name '*.mjs' \)); do
  sed -i "s|\.\./dist/|../$DISTDIR/|g" "$f"
done
# Every real reference is the `../dist/` prefix the rewrite targets, so any survivor is a path
# shape the rewrite does not know (prose in comments is free to say "dist/").
left=$(grep -rIn '\.\./dist/' "$OUT/example" "$OUT/playground" --include='*.html' --include='*.js' --include='*.mjs' || true)
if [ -n "$left" ]; then
  echo "::warning::a page still references ../dist/ after the rewrite:"; echo "$left"
fi
echo "dist staged as $DISTDIR"

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

# The demo pages the shell links to. Not fatal - the build may name them differently and the shell
# still reports isolation - but a broken link is the first thing anyone sees.
for demo in example playground; do
  if [ ! -f "$OUT/$demo/index.html" ]; then
    echo "::warning::no $demo/index.html in the artifact; the shell's link to it will 404"
  elif ! grep -q "coi-serviceworker.js" "$OUT/$demo/index.html"; then
    # A visitor who lands on a demo page FIRST (a deep link, a bookmark) has no service worker
    # registered yet, so that page is not isolated and the engine cannot start. The fix is one
    # script tag in the page; the shell cannot register on its behalf.
    echo "::warning::$demo/index.html does not load ../coi-serviceworker.js - a deep link to it will not be cross-origin isolated"
  fi
done

# The playground is the one page with a third-party dependency, and it is the one the
# vendor-everything rule exists for. A missing vendor/ is a blank screen, not a degraded demo.
if [ -f "$OUT/playground/index.html" ] && [ ! -f "$OUT/$DISTDIR/vendor/three/three.module.js" ]; then
  echo "::warning::no $DISTDIR/vendor/three in the artifact; the playground will not load. Run tools/wasm/fetch-web-vendor.sh"
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
