/**
 * playground_driver.js - the assertions run-playground.mjs runs inside the playground page.
 *
 * IT IS NOT PART OF THE PAGE. The runner's server appends a script tag for this file to the
 * playground's HTML when, and only when, the request carries `?__drive=1`, so the shipped page
 * carries no test code and the check still drives the REAL page rather than a copy of it. The one
 * thing the page does provide is `window.__bwaPlayground`, the hook main.js documents: a headless
 * browser cannot click a slider or read a cone's color, so the check reaches the same state a
 * control writes.
 *
 * It posts its verdict back to the runner, which is the same transport tests/browser.html uses and
 * for the same reason: a headless browser has no other way to hand a script a result.
 */
const notes = [];
const ok = (m) => notes.push("ok   " + m);
const fail = (m) => notes.push("FAIL " + m);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const db = (x) => (20 * Math.log10(Math.max(x, 1e-9))).toFixed(1);

async function main() {
  if (!globalThis.crossOriginIsolated) throw new Error("the page is not cross-origin isolated");
  ok("cross-origin isolated");

  const pg = await waitFor(() => globalThis.__bwaPlayground, 15000, "window.__bwaPlayground");
  /* --autoplay-policy=no-user-gesture-required lets the resume inside the click handler work with
   * nobody to click. A real visitor supplies the gesture; index.html says so. */
  document.getElementById("go").click();
  const started = await Promise.race([pg.ready, sleep(40000).then(() => "timeout")]);
  if (started !== true) throw new Error(`the page did not start (${started}); page log: ${pageLog()}`);
  ok("the page started from the Start button");

  /* ---- the sink is the one this demo exists for ---- */
  let s = pg.state();
  if (!/^worklet/.test(String(s.backend))) fail(`backend is "${s.backend}", not the worklet sink`);
  else ok(`backend "${s.backend}"`);
  if (s.sinkType !== 9) fail(`sink type ${s.sinkType}, expected 9 (WORKLET)`);
  else ok("sink type 9 (WORKLET)");
  if (s.channelCount !== 26) fail(`${s.channelCount} bus channels, expected the 26 of the CAVE layout`);
  else ok("26 bus channels");
  if (s.speakerCount !== 26) fail(`bwa_get_speakers gave ${s.speakerCount} speakers, expected 26`);
  else ok("bwa_get_speakers read back 26 speaker positions");

  /* ---- SCENE 1: the coordinate seam, on screen and in the ears ---- */
  await pg.selectScene("localization");
  if (pg.state().sceneId !== "localization") fail("scene 1 did not select");
  else ok("scene 1 (localization) selected");

  /* The visual half. Room +x is the listener's LEFT (BWA_ROOM_RIGHT is -x) and the default camera
   * stands behind the head, so a +x source must draw LEFT of center. Getting this backwards is the
   * single most confusing thing a spatial demo can do, which is why it is an assertion. */
  pg.setSourceRoom([2, 1.5, 0]);
  await sleep(120);
  const sideLeft = pg.screenSide([2, 1.5, 0]);
  const sideRight = pg.screenSide([-2, 1.5, 0]);
  if (sideLeft !== -1) fail(`a source at room +x drew on screen side ${sideLeft}, expected -1 (left)`);
  else ok("a source at room +x draws on the LEFT of the screen");
  if (sideRight !== 1) fail(`a source at room -x drew on screen side ${sideRight}, expected +1 (right)`);
  else ok("a source at room -x draws on the RIGHT of the screen");

  /* The audible half, measured through a real render on the manual sink (probe.js). The stimulus
   * is the page's own click: never DC, because the default HRTF's per-ear DC gains oppose its
   * audible ILD (CLAUDE.md's first trap). */
  const L = await pg.measureLaterality([2.0, 1.5, 0.0]);
  const R = await pg.measureLaterality([-2.0, 1.5, 0.0]);
  ok(`source at +x: L ${db(L.left)} dB, R ${db(L.right)} dB`);
  ok(`source at -x: L ${db(R.left)} dB, R ${db(R.right)} dB`);
  const MARGIN = 1.3;                       /* about 2.3 dB: a coin-flip margin proves nothing */
  if (!(L.left > L.right * MARGIN))
    fail(`a source on the listener's LEFT (+x) was not louder in the left ear by ${MARGIN}x`);
  else ok("a source at room +x is louder in the LEFT ear");
  if (!(R.right > R.left * MARGIN))
    fail(`a source on the listener's RIGHT (-x) was not louder in the right ear by ${MARGIN}x`);
  else ok("a source at room -x is louder in the RIGHT ear");

  /* ---- SCENE 2: the channel walk lights the channel it drove ---- */
  await pg.selectScene("channel-walk");
  const st = pg.sceneState();
  if (!st) throw new Error("the channel-walk scene exposed no state");
  st.auto = false;
  for (const ch of [0, 7, 19, 25]) {
    st.channel = ch;
    await sleep(500);
    const now = pg.state();
    const loud = now.loudest;
    if (loud.channel !== ch)
      fail(`drove channel ${ch}; the loudest bus channel was ${loud.channel} at ${db(loud.peak)} dB`);
    else if (!(loud.peak > 0.01))
      fail(`channel ${ch} lit at only ${db(loud.peak)} dB: the bus meter is not reading the test signal`);
    else ok(`channel ${ch} drove channel ${ch} on the bus, at ${db(loud.peak)} dB`);
    /* and nothing else, within 20 dB: two channels at once is the state that makes a wiring fault
     * unreadable, and it is the failure this scene exists to catch. */
    let other = 0;
    now.busLevels.forEach((v, i) => { if (i !== ch) other = Math.max(other, v); });
    if (other > loud.peak * 0.1)
      fail(`channel ${ch} also lit channel levels up to ${db(other)} dB`);
  }

  /* ---- the render is alive and the worklet, not the host-paced fallback, is driving ---- */
  const h0 = pg.state().health;
  await sleep(700);
  const h1 = pg.state().health;
  const blocks = h1.blocks - h0.blocks;
  ok(`${blocks} blocks rendered in 700 ms`);
  if (blocks < 40) fail(`only ${blocks} blocks in 700 ms: the render is not running`);
  if (h1.deviceLost !== 0)
    fail("device_lost is set: the sink is host-pacing silence, so process() is not being called");
  else ok("device_lost is 0: the AudioWorklet is driving");

  /* ---- SCENE 3: the ray-traced occlusion really occludes ---- */
  await pg.selectScene("occlusion");
  const occ = pg.sceneState();
  occ.sweep = false;
  occ.z = -1.4;                             /* between the head at z 0 and the source at z -3 */
  await sleep(1400);                        /* the sim thread publishes at its own 10-30 Hz */
  const blocked = pg.sceneState().factor;
  occ.z = 0.6;                              /* behind the listener: nothing in the line of sight */
  await sleep(1400);
  const clear = pg.sceneState().factor;
  ok(`occlusion factor: wall in the way ${blocked.toFixed(3)}, wall out of it ${clear.toFixed(3)}`);
  if (!(blocked < 0.85))
    fail(`a wall across the line of sight gave an occlusion factor of ${blocked.toFixed(3)}`);
  else if (!(clear > blocked + 0.1))
    fail(`moving the wall out of the way did not clear the occlusion (${clear.toFixed(3)})`);
  else ok("the wall occludes when it is in the way and does not when it is not");

  /* ---- SCENE 4: directivity really attenuates off axis ---- */
  await pg.selectScene("directivity");
  const dir = pg.sceneState();
  dir.spin = false;
  dir.preset = 2;                           /* figure-8: the null is deepest, so the margin is real */
  const src4 = pg.state().sourceRoom;
  const toward = Math.atan2(-src4[0], -src4[2]);    /* the source's +z aimed at the listener */
  dir.yaw = toward;
  await sleep(400);
  const onAxis = pg.sceneState().gain;
  dir.yaw = toward + Math.PI / 2;
  await sleep(400);
  const offAxis = pg.sceneState().gain;
  ok(`figure-8 directivity: on axis ${onAxis.toFixed(3)}, 90 deg off ${offAxis.toFixed(3)}`);
  if (!(onAxis > 0.9)) fail(`a figure-8 aimed at the listener read ${onAxis.toFixed(3)}, expected about 1`);
  else if (!(offAxis < 0.2)) fail(`a figure-8 turned 90 degrees read ${offAxis.toFixed(3)}, expected about 0`);
  else ok("the directivity gain follows the aim");

  /* ---- the profile rebuild: the other render, on the same AudioContext ---- */
  await pg.selectScene("localization");
  await pg.setProfile(1);                   /* BWA_PROFILE_BINAURAL */
  await sleep(700);
  const b = pg.state();
  if (b.profile !== 1) fail("the engine did not rebuild into the binaural profile");
  else if (!/^worklet/.test(String(b.backend)))
    fail(`after the rebuild the backend is "${b.backend}", not the worklet sink`);
  else ok(`rebuilt into the binaural profile on the same AudioContext: "${b.backend}"`);
  const bh0 = pg.state().health;
  await sleep(600);
  const bh1 = pg.state().health;
  if (bh1.blocks - bh0.blocks < 30)
    fail(`the rebuilt engine rendered only ${bh1.blocks - bh0.blocks} blocks in 600 ms`);
  else ok(`the rebuilt engine renders (${bh1.blocks - bh0.blocks} blocks in 600 ms)`);
  /* A bus scene must drag the profile back by itself, because binaural has no bus to walk. */
  await pg.selectScene("channel-walk");
  if (pg.state().profile !== 2) fail("the channel walk did not force the CAVE_SIM profile back");
  else ok("the channel walk forced CAVE_SIM back for itself");
  await pg.setProfile(2);

  /* ---- every other scene at least enters and runs without an engine error ---- */
  for (const id of pg.scenes) {
    await pg.selectScene(id);
    await sleep(350);
    if (pg.state().sceneId !== id) fail(`scene "${id}" did not select`);
  }
  const errs = pg.state().errors;
  if (errs.length) fail(`the page logged ${errs.length} engine error(s): ${errs.join(" | ")}`);
  else ok(`all ${pg.scenes.length} scenes entered and ran with no engine error`);
}

function pageLog() {
  return (document.getElementById("log")?.textContent || "(empty)").slice(0, 800);
}

async function waitFor(fn, ms, what) {
  const t0 = performance.now();
  for (;;) {
    const v = fn();
    if (v) return v;
    if (performance.now() - t0 > ms) throw new Error(`timed out waiting for ${what}`);
    await sleep(50);
  }
}

main().then(() => report(null)).catch((e) => report(e));

function report(err) {
  if (err) notes.push("FAIL threw: " + (err && err.stack ? err.stack : String(err)));
  const body = { pass: notes.filter((n) => n.startsWith("FAIL")).length === 0, notes };
  /* The runner's own server by default. A staged-site check serves the artifact from a plain
   * file server that cannot collect anything, so it overrides this with a collector of its own. */
  const url = globalThis.__BWA_RESULT_URL || "/__result";
  fetch(url, { method: "POST", body: JSON.stringify(body) }).catch(() => {});
}
