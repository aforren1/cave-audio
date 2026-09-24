/**
 * layout_probe.mjs - the responsive-layout assertions both page drivers run in layout mode.
 *
 * playground_driver.js and xr_driver.js import this (the runners serve it at a virtual path, like
 * the drivers themselves) when the page URL carries `__layout`. The runner, through
 * tests/layout_cdp.mjs, has already set the viewport and, for a phone, touch emulation, and it
 * names the layout the page must be in (`__mode=stacked|side`). Naming it is the point: a probe
 * that derived the expected mode from the same breakpoints as the CSS would agree with any CSS.
 *
 * WHAT IT CANNOT SEE. A real phone's URL bar (dvh equals vh in an emulated viewport), a real
 * finger, and the headset's 2D window, whose size nobody has measured. The runner's viewports are
 * guesses at those, and docs/web.md says so.
 */

const TABLE_MIN_PX = 0.82 * 14 - 0.05;    /* .82rem at the pages' 14 px root, less rounding */
const TOUCH_MIN_PX = 44 - 0.5;

/**
 * @param {object} o
 * @param {(m: string) => void} o.ok
 * @param {(m: string) => void} o.fail
 * @param {string[]} o.startButtons  ids that must be on screen, unscrolled, before the start
 * @param {string[]} o.runButtons    ids that must still be on screen once the engine runs
 * @param {() => Promise<void>} o.start  press the page's start button and wait for the engine
 */
export async function layoutPass(o) {
  const q = new URLSearchParams(location.search);
  const vp = q.get("__layout");
  const mode = q.get("__mode");
  const rotate = q.get("__rotate");        /* "WxH:mode", a viewport change after the start */
  const screenW = Number(q.get("__vw"));   /* the width the runner emulates */
  if (mode !== "stacked" && mode !== "side") throw new Error(`layout mode "${mode}" is not stacked or side`);
  const tag = (m) => `[${vp} ${innerWidth}x${innerHeight}] ${m}`;
  const ok = (m) => o.ok(tag(m));
  const fail = (m) => o.fail(tag(m));

  await frames(3);
  const coarse = matchMedia("(pointer: coarse)").matches;
  ok(`pointer is ${coarse ? "coarse" : "fine"}`);
  measure("before start", mode, screenW, o.startButtons, coarse, ok, fail);
  await shot("pre");

  await o.start();
  await frames(6);
  measure("running", mode, screenW, o.runButtons, coarse, ok, fail);
  canvasMatches("running", ok, fail);
  await shot("run");

  if (rotate) {
    const [size, mode2] = rotate.split(":");
    const [w, h] = size.split("x").map(Number);
    /* The page resizes its renderer from a window resize listener. A breakpoint change moves the
     * canvas without the canvas asking, so this is the check that the listener is enough. */
    const resized = new Promise((r) => addEventListener("resize", r, { once: true }));
    await fetch(`/__layout/viewport?w=${w}&h=${h}`, { method: "POST" });
    const fired = await Promise.race([resized.then(() => true), sleep(3000).then(() => false)]);
    if (!fired) fail("the viewport change fired no resize event");
    await frames(6);
    measure(`turned to ${w}x${h}`, mode2, w, o.runButtons, coarse, ok, fail);
    canvasMatches(`turned to ${w}x${h}`, ok, fail);
    await shot("turned");
  }
}

function measure(when, mode, screenW, buttons, coarse, ok, fail) {
  const W = innerWidth, H = innerHeight;
  /* Under mobile emulation, as on a phone, content wider than the screen does not scroll: the
   * layout viewport GROWS to fit it and the page is drawn zoomed out. So scrollWidth alone passes
   * a page that overflows; the tell is innerWidth no longer being the screen's width. */
  if (screenW && W !== screenW)
    fail(`${when}: the layout viewport is ${W} px wide on a ${screenW} px screen: something is wider ` +
         "than the page, and a phone would show it zoomed out");
  const de = document.documentElement;
  const sw = Math.max(de.scrollWidth, document.body.scrollWidth);
  const sh = Math.max(de.scrollHeight, document.body.scrollHeight);
  if (sw > W) fail(`${when}: the page scrolls sideways (scrollWidth ${sw} > ${W})`);
  else ok(`${when}: no horizontal page scroll (scrollWidth ${sw})`);
  if (sh > H + 1) fail(`${when}: the page itself scrolls (scrollHeight ${sh} > ${H}); only the controls should`);

  const cv = document.getElementById("view").getBoundingClientRect();
  const panel = document.getElementById("panel");
  const pn = panel.getBoundingClientRect();
  const frac = cv.width / W;
  const pct = (x) => `${Math.round(x * 100)}%`;
  if (mode === "stacked") {
    if (frac < 0.85) fail(`${when}: stacked, but the view is ${pct(frac)} of the width (want >= 85%)`);
    else ok(`${when}: stacked, view ${Math.round(cv.width)}x${Math.round(cv.height)} (${pct(frac)} of the width)`);
    if (pn.top < cv.bottom - 1) fail(`${when}: the controls (top ${pn.top}) are not below the view (bottom ${cv.bottom})`);
    else ok(`${when}: the controls start below the view`);
    const hf = cv.height / H;
    if (hf < 0.38 || hf > 0.46) fail(`${when}: the view is ${pct(hf)} of the height (want about 42%)`);
    if (pn.bottom > H + 1) fail(`${when}: the controls run past the viewport (bottom ${pn.bottom})`);
  } else {
    if (frac < 0.5) fail(`${when}: side by side, but the view is ${pct(frac)} of the width (want >= 50%)`);
    else ok(`${when}: side by side, view ${Math.round(cv.width)}x${Math.round(cv.height)} (${pct(frac)} of the width), sidebar ${Math.round(pn.width)} px`);
    if (pn.right > cv.left + 1) fail(`${when}: the sidebar (right ${pn.right}) overlaps the view (left ${cv.left})`);
  }
  if (panel.scrollWidth > panel.clientWidth + 1) {
    /* Name the deepest element that sticks out, which is the one to fix. */
    let worst = null, over = 0;
    for (const e of panel.querySelectorAll("*")) {
      const d = e.getBoundingClientRect().right - (pn.left + panel.clientWidth);
      if (d > 0.5 && d >= over) { over = d; worst = e; }
    }
    fail(`${when}: the controls scroll sideways inside the sidebar (${panel.scrollWidth} > ${panel.clientWidth}; ` +
         `${describe(worst)} sticks out ${Math.round(over)} px)`);
  }

  for (const id of buttons) {
    const b = document.getElementById(id);
    const r = b.getBoundingClientRect();
    if (!(r.width > 0 && r.left >= 0 && r.top >= 0 && r.right <= W + 0.5 && r.bottom <= H + 0.5)) {
      fail(`${when}: #${id} is not on screen without scrolling (${Math.round(r.left)},${Math.round(r.top)} ` +
           `to ${Math.round(r.right)},${Math.round(r.bottom)})`);
      continue;
    }
    const hit = document.elementFromPoint((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    if (hit !== b && !b.contains(hit)) fail(`${when}: #${id} is on screen but covered by ${describe(hit)}`);
    else ok(`${when}: #${id} is on screen at y ${Math.round(r.top)}..${Math.round(r.bottom)}, unscrolled`);
  }

  if (coarse) {
    const small = [];
    for (const e of document.querySelectorAll("button, select, input[type=range], input[type=file]")) {
      const r = e.getBoundingClientRect();
      if (r.width === 0 && r.height === 0) continue;       /* not displayed */
      if (r.height < TOUCH_MIN_PX) small.push(`${describe(e)} ${r.height.toFixed(1)} px`);
    }
    if (small.length) fail(`${when}: ${small.length} touch target(s) under 44 px: ${small.slice(0, 6).join(", ")}`);
    else ok(`${when}: every button, select and slider is at least 44 px tall`);
  }

  const tiny = [];
  for (const e of document.querySelectorAll("th, td")) {
    if (e.getClientRects().length === 0) continue;
    const fs = parseFloat(getComputedStyle(e).fontSize);
    if (fs < TABLE_MIN_PX) tiny.push(`${describe(e)} ${fs} px`);
  }
  if (tiny.length) fail(`${when}: table text under .82rem: ${tiny.slice(0, 4).join(", ")}`);
}

/** The renderer's drawing buffer follows the canvas's CSS box, which is what resize() is for. */
function canvasMatches(when, ok, fail) {
  const c = document.getElementById("view");
  const r = c.getBoundingClientRect();
  const dpr = Math.min(2, devicePixelRatio || 1);
  const ww = Math.floor(r.width) * dpr, hh = Math.floor(r.height) * dpr;
  if (Math.abs(c.width - ww) > 2 || Math.abs(c.height - hh) > 2)
    fail(`${when}: the canvas buffer is ${c.width}x${c.height} for a ${Math.round(r.width)}x${Math.round(r.height)} box: ` +
         "the renderer missed the layout change");
  else ok(`${when}: the canvas buffer ${c.width}x${c.height} follows its box`);
}

function describe(e) {
  if (!e) return "nothing";
  if (e.id) return `#${e.id}`;
  const t = (e.textContent || "").trim().slice(0, 24);
  return `<${e.tagName.toLowerCase()}${e.type ? " " + e.type : ""}>${t ? ` "${t}"` : ""}`;
}

/** Ask the runner to screenshot the page now. It answers once the PNG is written. */
async function shot(name) {
  await frames(2);
  await fetch(`/__layout/shot?tag=${name}`, { method: "POST" }).catch(() => {});
}

/** n animation frames, or a timeout: a hidden tab gets no frames and must not hang the pass. */
function frames(n) {
  const raf = new Promise((r) => {
    const step = () => (--n <= 0 ? r() : requestAnimationFrame(step));
    requestAnimationFrame(step);
  });
  return Promise.race([raf, sleep(100 * n + 500)]);
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }
