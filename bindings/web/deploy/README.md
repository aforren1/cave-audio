# Pages deploy

The GitHub Pages half of the web build. The wasm engine, the JS binding and the demo pages' third
party are built elsewhere and land in `bindings/web/dist/`. This directory turns that tree into a site that
is **cross-origin isolated**, which is what `SharedArrayBuffer` needs and what GitHub Pages cannot
give you with headers.

| file | what it is |
|------|------------|
| `coi-serviceworker.js` | vendored [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker), MIT, pinned by commit in its header. Do not hand-edit it. |
| `coi-serviceworker.LICENSE` | the MIT text that travels with it. |
| `index.html` | the landing shell. Registers the worker, reports isolation, links to the two demos. |
| `stage.sh` | assembles the artifact: `dist/`, `example/`, `playground/`, `xr/`, plus this shell and the worker at the root, plus `.nojekyll`. |

`.github/workflows/pages.yml` runs the build, runs `stage.sh` and deploys. Nothing else in the
repository reads this directory.

## What the service worker does

The engine runs the desktop two-thread model in the browser, so it needs `SharedArrayBuffer`, so
the page must be cross-origin isolated. See [docs/web.md](../../../docs/web.md), "The decision"
and "Hosting". Isolation means two response headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

GitHub Pages sends neither and has no setting for them. The worker supplies them instead. The
script is loaded twice by the same URL. As a page script it registers itself as a service worker.
As that worker it intercepts every fetch the site makes and re-serves the response with both
headers added.

**The cost is one reload.** On a visitor's first load there is no worker yet, so the page is not
isolated. The script registers one and reloads the page once. The second load is controlled by the
worker, so it is isolated, and `crossOriginIsolated` is true from then on. The landing shell shows
both states, which is why its answers can flip a moment after it opens.

Two things follow. A private window with service workers disabled never becomes isolated, and a
demo must refuse to start rather than fall back to a single-threaded engine. And the worker's
scope comes from its own URL, so the script must stay at the site root: from there it controls
`example/`, `playground/` and `xr/` too.

## Every asset must be same-origin

Under `require-corp` a cross-origin subresource that does not send
`Cross-Origin-Resource-Policy` is **blocked**. Not slow, not degraded. Blocked. A link to another
site is fine, because a navigation is not a subresource, but a script, a stylesheet, a font, an
image, a wasm module or a worker is not.

So: **vendor it, do not link it**. The landing shell loads nothing but its own inline CSS and the
worker. The rule applies to everything the build puts in `dist/` as well. The playground and the XR
page are the pages that have a third-party dependency, and they obey the rule:
`tools/wasm/fetch-web-vendor.sh` fetches a pinned, hashed three.js into `dist/vendor/three/` at
build time, and both pages import it from there. Any future font or icon set goes the same way.

`stage.sh` greps the staged tree for cross-origin `src`, `import`, `importScripts` and `url()`
references and prints a warning for each. It warns rather than fails, because the check reads text
and cannot tell a comment from a tag. Treat a warning as a broken page until you have proved
otherwise.

## What this expects from the build

The site root mirrors `bindings/web`: `stage.sh` copies `bindings/web/dist/` to `dist-<hash>/`,
`bindings/web/example/` to `example/` (minus the local dev server) and
`bindings/web/playground/` to `playground/` and `bindings/web/xr/` to `xr/`, then puts this
directory's files at the root. The XR page imports from `../playground/`, so the two directories
are staged together or its imports 404. The
hash is the build's own content, and the pages' `../dist/` imports are rewritten to it at stage
time. That is what keeps a returning visitor's cached modules from being mixed with a new build:
Pages serves everything with a ten-minute `max-age`, and the first playground deploy died on a
cached pre-playground `client.js` next to a fresh `rig.js`. A changed build is a new URL; an
unchanged one keeps its URL and its cache. So each page's `../dist/index.js` import resolves on
the site exactly as it does under `example/serve.mjs`, and the root is owned by the shell:

- **`index.html` at the root is the landing shell.** It has to own the root, because that is
  where the worker registers.
- **`coi-serviceworker.js` and `coi-serviceworker.LICENSE` at the root belong to this directory.**

The shell links to **`playground/index.html`**, **`xr/index.html`** and
**`example/index.html`**. Those three paths are the assumptions this directory makes about the
build. Change a demo and this shell together, or
`stage.sh` warns that the link will 404. It also warns when `dist/vendor/three` is missing, because
a playground or an XR page with no three.js is a blank screen rather than a degraded demo.

One more thing every demo page must do: **load the worker itself**, with
`<script src="../coi-serviceworker.js"></script>` before anything that touches
`SharedArrayBuffer`. A visitor who follows a deep link straight to `example/index.html` or
`playground/index.html` has never run the landing shell, so no worker is registered yet and that
page is not isolated. The shell cannot register on another page's behalf. `stage.sh` warns when a
demo page does not reference the script.

## Enable Pages, once

The workflow deploys through the Pages API, which needs the repository set to build from Actions.
The owner does this once:

- **Settings > Pages > Source: GitHub Actions**, or
- `gh api -X POST repos/:owner/:repo/pages -f build_type=workflow`

Until then the deploy step fails with `Get Pages site failed` and nothing else is wrong. The site
is then at:

```
https://aforren1.github.io/cave-audio/
```

A project site serves from a subpath, so keep every reference in the pages **relative**. An
absolute path such as `/example/index.html` resolves to `aforren1.github.io/example/index.html`
and 404s.

## Check it locally

Build the web target first (`tools/wasm/build-web.sh`, which fills `bindings/web/dist/`), then:

```sh
bash bindings/web/deploy/stage.sh
python -m http.server 8080 --directory bindings/web/_site
```

`http.server` sets no headers at all, which is the point: it is the same situation GitHub Pages
puts you in. Open `http://localhost:8080/`, let it reload once, and read the table. All four rows
green means the worker is doing its job. `localhost` counts as a secure context, so a plain
`http://` origin is enough here and nowhere else.

**One trap on Windows.** Python's `mimetypes` reads the registry, where `.mjs` is often
`text/plain`, and a module script served with that type is rejected: the demo fails with
`Failed to fetch dynamically imported module`. GitHub Pages serves `.mjs` as `text/javascript`, so
this is a host quirk and not a property of the site. Register the type instead of chasing it:

```sh
python -c "import mimetypes,functools,http.server as h; \
           mimetypes.add_type('text/javascript','.mjs'); \
           mimetypes.add_type('application/wasm','.wasm'); \
           h.test(HandlerClass=functools.partial(h.SimpleHTTPRequestHandler, \
                  directory='bindings/web/_site'), port=8080)"
```

## Update the vendored worker

Replace `coi-serviceworker.js` with a newer upstream copy, keep the local header block at the top,
and change the commit and date in it. Upstream publishes no git tags, so the commit is the pin.
The page configures the worker through `window.coi` in `index.html` and never by editing the
script.

Two settings there are deliberate. `coepCredentialless` is **false**, so the worker serves
`require-corp`, which is the header docs/web.md commits to and the one that makes the
vendor-everything rule real. `coepDegrade` is **false**, so a page that fails to isolate stays
failed and says so, instead of quietly retrying in the weaker mode.
