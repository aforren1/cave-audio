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

/* The page's layout when nothing was uploaded: examples/dome_24.json, the generated 24-speaker
 * dome, fetched from dist/. Not BWA_DEFAULT_GRID (26): the page passes a layout, so the engine's
 * built-in grid only runs if that fetch fails, and that failure is what these pins catch. */
const DOME = 24;
const DOME_NAME = "dome_24.json";

const db = (x) => (20 * Math.log10(Math.max(x, 1e-9))).toFixed(1);

/* The bound on "a stimulus change is heard". One engine block is 5.3 ms, the worklet sink's own
 * queue is a few more, and the analyser window this is measured through is 10.7 ms. 60 ms leaves
 * room for a headless browser's scheduling and still fails an accidental return of the queue. */
const SWITCH_MS = 60;

async function main() {
  if (!globalThis.crossOriginIsolated) throw new Error("the page is not cross-origin isolated");
  ok("cross-origin isolated");

  const pg = await waitFor(() => globalThis.__bwaPlayground, 15000, "window.__bwaPlayground");

  /* The runner's layout pass (tests/layout_cdp.mjs) loads the page once per viewport with
   * `__layout` set. Only the layout is checked there; everything below ran in the main pass. */
  if (new URLSearchParams(location.search).has("__layout")) {
    const { layoutPass } = await import("/__bwa_layout_probe.mjs");
    await layoutPass({
      ok, fail,
      startButtons: ["go"],
      runButtons: [],
      start: async () => {
        document.getElementById("go").click();
        const started = await Promise.race([pg.ready, sleep(40000).then(() => "timeout")]);
        if (started !== true) throw new Error(`the page did not start (${started}); page log: ${pageLog()}`);
      },
    });
    return;
  }

  /* --autoplay-policy=no-user-gesture-required lets the resume inside the click handler work with
   * nobody to click. A real visitor supplies the gesture; index.html says so. */
  document.getElementById("go").click();
  const started = await Promise.race([pg.ready, sleep(40000).then(() => "timeout")]);
  if (started !== true) throw new Error(`the page did not start (${started}); page log: ${pageLog()}`);
  ok("the page started from the Start button");

  /* ---- the start affordance is GONE once the engine is up, and something says what is running.
   * `hidden` alone did not do it: the UA's [hidden] rule loses to the panel's own id selector, so
   * the "a browser resumes an AudioContext only from a user gesture" text sat over the scene for
   * the whole session. Assert the COMPUTED style, because the attribute was already true. ---- */
  {
    const v = pg.state().view;
    if (v.startPanelShown) fail("the Start panel is still displayed over the running scene");
    else ok("the Start panel is gone once the engine is running");
    if (!v.status) fail("no status line replaced it");
    else if (!/cave_sim/.test(v.status) || !/sink worklet/.test(v.status) || !/context running/.test(v.status))
      fail(`the status line does not name profile, sink and context state: "${v.status}"`);
    else ok(`the status line reads "${v.status}"`);
  }

  /* ---- the sink is the one this demo exists for ---- */
  let s = pg.state();
  if (!/^worklet/.test(String(s.backend))) fail(`backend is "${s.backend}", not the worklet sink`);
  else ok(`backend "${s.backend}"`);
  if (s.sinkType !== 9) fail(`sink type ${s.sinkType}, expected 9 (WORKLET)`);
  else ok("sink type 9 (WORKLET)");
  if (s.channelCount !== DOME)
    fail(`${s.channelCount} bus channels, expected the ${DOME} of the default dome`);
  else ok(`${DOME} bus channels`);
  if (s.speakerCount !== DOME)
    fail(`bwa_get_speakers gave ${s.speakerCount} speakers, expected ${DOME}`);
  else ok(`bwa_get_speakers read back ${DOME} speaker positions`);
  {   /* the dome's geometry, read back out of the engine: 2 m from (0, 1.5, 0), none below the floor */
    const p = s.layout.speakers;
    let worst = 0, lowest = Infinity;
    for (let k = 0; k < s.speakerCount; ++k) {
      const x = p[3 * k], y = p[3 * k + 1], z = p[3 * k + 2];
      worst = Math.max(worst, Math.abs(Math.hypot(x, y - 1.5, z) - 2.0));
      lowest = Math.min(lowest, y);
    }
    if (!(worst < 1e-3) || !(lowest >= 0))
      fail(`the default layout is not the dome: radius error ${worst.toFixed(4)} m, lowest y ${lowest}`);
    else ok(`the default layout is the dome (radius error ${(worst * 1000).toFixed(2)} mm, lowest y ${lowest.toFixed(3)} m)`);
    if (!String(s.view.status).includes("layout " + DOME_NAME))
      fail(`the status line does not name ${DOME_NAME}: "${s.view.status}"`);
    else ok(`the status line names ${DOME_NAME}`);
  }

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

  /* ---- the meters and the cones FOLLOW the live bus, on the page's own default stimulus.
   *
   * This is the assertion the channel walk below could not make. That one drives
   * `bwa_set_test_signal`, which is on CONTINUOUSLY, so it passed while every ordinary scene read
   * "silent" and every cone stayed dark. The default stimulus is a click train whose burst is 2 ms
   * in every 250 ms, `bwa_get_bus_levels` publishes the LAST BLOCK's peak, and the page sampled it
   * every 120 ms - 22 blocks apart, so it saw 1 block in 22 and the click was in none of them
   * (measured: 0 of ~40 slow samples, 15 of 437 fast ones). What is asserted here is the DRAWN
   * state: the cone shading and the meter strip's width, not the numbers behind them. ---- */
  await pg.selectScene("localization");
  pg.setSignal(0);                          /* the click train: the page's default */
  pg.setSourceRoom([1.5, 1.5, 0.5]);
  await waitFor(() => pg.tapReady(), 8000, "the output tap on the sink's node");
  {
    let cone = 0;
    let strip = 0;
    let out = 0;
    const t0 = performance.now();
    while (performance.now() - t0 < 2500) {
      const v = pg.state().view;
      cone = Math.max(cone, ...v.coneLevels);
      strip = Math.max(strip, parseFloat(v.meterWidth) || 0);
      out = Math.max(out, parseFloat(v.outWidth[0]) || 0, parseFloat(v.outWidth[1]) || 0);
      await sleep(40);
    }
    ok(`click train: brightest cone ${cone.toFixed(2)}, bus strip ${strip.toFixed(0)}%, ` +
       `output strip ${out.toFixed(0)}%`);
    if (!(cone > 0.15)) fail(`the speaker cones never lit past ${cone.toFixed(3)} while a source played`);
    else ok("the speaker cones follow the live bus");
    if (!(strip > 10)) fail(`the bus meter strip never went past ${strip.toFixed(0)}%`);
    else ok("the bus meter strip follows the live bus");
    if (!(out > 10)) fail(`the output meter strips never went past ${out.toFixed(0)}%`);
    else ok("the output meter strips follow what the AudioContext is playing");
  }

  /* ---- a stimulus change is HEARD promptly, measured on the live output.
   *
   * There is no queue to drain any more: each stimulus is a loaded sound and a switch is one
   * `bwa_source_play`, which the engine ramps up over its first block. So the honest bound is one
   * block plus the sink's own latency plus the analyser's 10.7 ms window, and this asserts a tight
   * one. With the old push feed the same measurement read the feed's budget instead (100 ms by
   * design, and up to 1.4 s when the ring was filled rather than paced). ---- */
  {
    pg.setSignal(2);                        /* pink noise: continuous, so "hot" means arrived */
    await sleep(900);
    pg.outputPeaks();
    await sleep(300);
    const ref = pg.outputPeaks();
    const steady = Math.max(ref.rmsL, ref.rmsR);
    const thresh = steady * 0.3;
    /* The reference has to BE something. A page whose queue is a second deep has not started
     * playing the pink noise yet when this is measured, so `steady` comes back at silence, the
     * threshold collapses to zero and the detector below fires on the first read it takes - a
     * test that passes hardest exactly where the defect is worst (measured: "heard after 19 ms"
     * with the queue put back to 1.4 s). */
    if (!(steady > 1e-4)) {
      fail(`the live output was silent 1.2 s after switching to pink noise (${db(steady)} dB), ` +
           "so the latency below cannot be measured");
    }
    pg.setSignal(0);                        /* back to the click train: mostly silence */
    await sleep(700);
    pg.outputPeaks();
    const t0 = performance.now();
    pg.setSignal(2);
    /* A RUN, not one sample: a click also makes the output hot, for about as long as the
     * analyser's 10.7 ms window. Eight consecutive hot reads is 40 ms of continuous signal, which
     * a 2 ms burst cannot fake, and the time reported is the FIRST of them, so the run length is
     * not added to the answer. */
    let run = 0;
    let firstHot = -1;
    let heard = -1;
    while (performance.now() - t0 < 2000) {
      await sleep(5);
      const p = pg.outputPeaks();
      const hot = Math.max(p.rmsL, p.rmsR) > thresh;
      if (!hot) { run = 0; firstHot = -1; continue; }
      if (run === 0) firstHot = performance.now() - t0;
      if (++run >= 8) { heard = firstHot; break; }
    }
    ok(`stimulus switch heard after ${heard < 0 ? "never" : heard.toFixed(0) + " ms"} ` +
       `(steady pink RMS ${db(steady)} dB, threshold ${db(thresh)} dB)`);
    if (heard < 0) fail("the new stimulus never arrived on the output within 2 s");
    else if (heard > SWITCH_MS) fail(`the new stimulus took ${heard.toFixed(0)} ms to be heard; the bound is ${SWITCH_MS}`);
    else ok("a stimulus change reaches the output inside one block plus the sink's latency");
    pg.setSignal(0);
  }

  /* ---- a main-thread stall makes NO hole in the sound.
   *
   * This is the point of the change. The page used to feed a push source from a 20 ms timer on
   * this thread with about 100 ms queued ahead of the audio clock, so any stall longer than the
   * queue - a garbage collection, a window drag, a heavy three.js frame on a weak GPU - was a gap
   * in the audio and the engine counted it as a push-ring starve. Reported as "the click train
   * becomes inaudible for short periods" (2026-09-22). Now the stimulus is a loaded sound the
   * engine loops itself, so the page can stop running altogether and the sound goes on.
   *
   * A busy loop is the stall, because no timer and no frame can fire while it runs. What is
   * asserted: not one new starve, the audio thread went on rendering through it, and the output is
   * still carrying the signal afterwards. Confirmed RED against the previous feed-based rig.js
   * (which starved on the first 400 ms stall). ---- */
  {
    await pg.setSignal(2);                  /* pink noise: continuous, so a gap is a gap */
    await sleep(800);
    const h0 = pg.state().health;
    const STALL_MS = 400;
    const t0 = performance.now();
    while (performance.now() - t0 < STALL_MS) { /* hold the main thread */ }
    await sleep(700);                       /* the health poll runs at 120 ms; let it read */
    const h1 = pg.state().health;
    const blocks = h1.blocks - h0.blocks;
    ok(`main-thread stall of ${STALL_MS} ms: stream starves ${h0.streamStarves} -> ` +
       `${h1.streamStarves}, ${blocks} blocks rendered across it`);
    if (h1.streamStarves !== h0.streamStarves)
      fail(`a ${STALL_MS} ms main-thread stall starved the engine ` +
           `(${h0.streamStarves} -> ${h1.streamStarves}); the page is in the audio path again`);
    else ok("a main-thread stall produced no starve: the page is not in the audio path");
    /* The audio thread is on the AudioWorklet thread and owes nothing to this one. 1.1 s of wall
     * clock is about 206 blocks at 48 kHz / 256; 150 is a margin, not a coin flip. */
    if (blocks < 150) fail(`only ${blocks} blocks were rendered across the stall and the wait after it`);
    else ok("the audio thread rendered straight through the stall");
    pg.outputPeaks();
    await sleep(400);
    const after = pg.outputPeaks();
    if (!(Math.max(after.rmsL, after.rmsR) > 1e-4))
      fail(`the live output is silent after the stall (${db(Math.max(after.rmsL, after.rmsR))} dB)`);
    else ok(`the output still carries the stimulus after the stall ` +
            `(${db(Math.max(after.rmsL, after.rmsR))} dB RMS)`);
    await pg.setSignal(0);
  }

  /* ---- a clip the visitor drops on the page becomes a loaded sound too.
   *
   * The same route as the built-in stimuli: the browser decodes it, the rig folds it to mono,
   * writes it as a wav and loads it. A wav is what the file input would hand over, so the check
   * builds one. If the load failed the page logs an engine error, which the "no engine error"
   * assertion further down would catch as well.
   *
   * The INDEX is what says the clip is the thing playing. The level below only says the output is
   * not silent, and it would pass on the previous stimulus: measured at -43 dB against a clip that
   * loaded at -10.6 dB, with the load deliberately skipped. ---- */
  {
    const before = pg.state().stimuli.length;
    const heard0 = await pg.loadClip(driverWav(0.5, 660), "driver-tone.wav");
    await sleep(600);
    const after = pg.state();
    if (heard0 !== true) fail(`the page refused an uploaded clip: ${pageLog()}`);
    else if (after.stimuli.length !== before + 1)
      fail(`the stimulus picker did not gain the uploaded clip (${after.stimuli.length} entries)`);
    else if (after.stimulus !== before)
      fail(`the uploaded clip is not the stimulus playing (index ${after.stimulus})`);
    else ok(`an uploaded clip loaded and became the stimulus ("${after.stimuli[before]}")`);
    pg.outputPeaks();
    await sleep(400);
    const p = pg.outputPeaks();
    if (!(Math.max(p.rmsL, p.rmsR) > 1e-4))
      fail(`the uploaded clip is silent on the live output (${db(Math.max(p.rmsL, p.rmsR))} dB)`);
    else ok(`the uploaded clip is audible on the live output (${db(Math.max(p.rmsL, p.rmsR))} dB RMS)`);
    await pg.setSignal(0);
    if (pg.state().stimuli.length !== before + 1) fail("the uploaded clip left the picker");
  }

  /* ---- SCENE 2: the channel walk lights the channel it drove ---- */
  await pg.selectScene("channel-walk");
  const st = pg.sceneState();
  if (!st) throw new Error("the channel-walk scene exposed no state");
  st.auto = false;
  for (const ch of [0, 7, 19, DOME - 1]) {   /* the last is the layout's last channel */
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

  /* ---- the profile rebuild, and whether anything comes OUT of it.
   *
   * The old check asked whether the rebuilt engine rendered blocks, and it passed while the page
   * was silent: a sink whose worklet never came up host-paces silence and counts blocks exactly
   * the same way (rule 4). Two things say the difference. `device_lost` is the sink's own report
   * that the host-paced thread is doing the pacing, and the analyser tap on the sink's node is the
   * audio itself. Both are asserted in both directions, because the cause - a second
   * `registerProcessor` in one AudioContext's worklet scope - breaks every rebuild after the
   * first, not only the first one. ---- */
  await pg.selectScene("localization");
  await pg.setProfile(1);                   /* BWA_PROFILE_BINAURAL */
  await sleep(700);
  const b = pg.state();
  if (b.profile !== 1) fail("the engine did not rebuild into the binaural profile");
  else if (!/^worklet/.test(String(b.backend)))
    fail(`after the rebuild the backend is "${b.backend}", not the worklet sink`);
  else ok(`rebuilt into the binaural profile: "${b.backend}"`);
  const bh0 = pg.state().health;
  await sleep(600);
  const bh1 = pg.state().health;
  if (bh1.blocks - bh0.blocks < 30)
    fail(`the rebuilt engine rendered only ${bh1.blocks - bh0.blocks} blocks in 600 ms`);
  else ok(`the rebuilt engine renders (${bh1.blocks - bh0.blocks} blocks in 600 ms)`);
  {
    const live = await waitDeviceLive(pg);
    if (live < 0)
      fail("after the rebuild the sink is still host-pacing silence: the AudioWorklet never came back");
    else ok(`after the rebuild the AudioWorklet is driving again (device_lost 0 after ${live.toFixed(0)} ms)`);
  }
  /* The cones ARE dark in this profile, and that is correct rather than broken: point voices never
   * reach the array bus here. The page has to say so where the cones are. */
  {
    const v = pg.state().view;
    if (!/bypass the bus/.test(String(v.status)) || !/binaural/.test(String(v.meterText)))
      fail(`binaural does not explain the dark cones: status "${v.status}", meter "${v.meterText}"`);
    else ok("binaural says why the cones are dark, on the view and in the panel");
  }
  await audibleLaterality(pg, "binaural");

  /* A bus scene must drag the profile back by itself, because binaural has no bus to walk. */
  await pg.selectScene("channel-walk");
  if (pg.state().profile !== 2) fail("the channel walk did not force the CAVE_SIM profile back");
  else ok("the channel walk forced CAVE_SIM back for itself");
  await pg.setProfile(2);
  await pg.selectScene("localization");
  await sleep(500);
  {
    const live = await waitDeviceLive(pg);
    if (live < 0) fail("back in cave_sim the sink is host-pacing silence: the second rebuild lost the worklet");
    else ok(`back in cave_sim the AudioWorklet is driving (device_lost 0 after ${live.toFixed(0)} ms)`);
  }
  await audibleLaterality(pg, "cave_sim");

  /* ---- every other scene at least enters and runs without an engine error ---- */
  for (const id of pg.scenes) {
    await pg.selectScene(id);
    await sleep(350);
    if (pg.state().sceneId !== id) fail(`scene "${id}" did not select`);
  }
  const errs = pg.state().errors;
  if (errs.length) fail(`the page logged ${errs.length} engine error(s): ${errs.join(" | ")}`);
  else ok(`all ${pg.scenes.length} scenes entered and ran with no engine error`);

  /* ---- a layout file, uploaded, loaded and DRAWN. Last, because the refusal case below logs an
   * engine error on purpose and the check above counts those. ---- */
  await pg.selectScene("localization");
  await layoutChecks(pg);
}

/**
 * Is a source on the listener's left louder in the left ear, in the audio the AudioContext is
 * really playing?
 *
 * The offline probe (probe.js) answers the same question through a second engine on the manual
 * sink, which is deterministic and says nothing about whether the page is audible. This one reads
 * the AnalyserNode pair the page hangs on the sink's own node, so it fails when the output is
 * silent - which is exactly the defect a rebuilt engine had. Pink noise, never DC: CLAUDE.md's
 * first trap is a DC laterality assertion that shipped a left/right mirror.
 */
async function audibleLaterality(pg, label) {
  const MARGIN = 1.3;
  const at = async (p) => {
    pg.setSourceRoom(p);
    await sleep(450);                       /* the queue, plus the gain ramp settling */
    pg.outputPeaks();
    await sleep(450);
    return pg.outputPeaks();
  };
  pg.setSignal(2);                          /* pink noise: continuous, so RMS means something */
  await waitFor(() => pg.tapReady(), 8000, "the output tap after the rebuild");
  const left = await at([2.0, 1.5, 0.0]);
  const right = await at([-2.0, 1.5, 0.0]);
  pg.setSignal(0);
  ok(`${label} live output: +x source L ${db(left.rmsL)} R ${db(left.rmsR)} dB, ` +
     `-x source L ${db(right.rmsL)} R ${db(right.rmsR)} dB`);
  if (!(left.rmsL > 1e-5) || !(right.rmsR > 1e-5))
    fail(`${label}: the live output is silent, so the page makes no sound`);
  else if (!(left.rmsL > left.rmsR * MARGIN))
    fail(`${label}: a source at room +x was not louder in the LEFT ear of the live output`);
  else if (!(right.rmsR > right.rmsL * MARGIN))
    fail(`${label}: a source at room -x was not louder in the RIGHT ear of the live output`);
  else ok(`${label}: the live output carries the source, on the correct side`);
}

/** A small, valid cave_layout.json with one speaker somewhere no default grid puts one. */
function testLayout(n = 8, odd = [0.37, 2.13, -1.91]) {
  const speakers = [];
  for (let i = 0; i < n; ++i) {
    const a = (2 * Math.PI * i) / n;
    speakers.push({
      index: i,
      position: i === 3 ? odd.slice() : [2 * Math.cos(a), 1.2, 2 * Math.sin(a)],
      gain_db: 0,
      delay_ms: 0,
    });
  }
  return { schema_version: 1, speakers };
}

async function layoutChecks(pg) {
  const odd = [0.37, 2.13, -1.91];
  const doc = testLayout(8, odd);
  const loaded = await pg.loadLayout(JSON.stringify(doc), "driver-8.json");
  const s = pg.state();
  if (!loaded || s.speakerCount !== 8) {
    fail(`the uploaded 8-speaker layout did not load (${s.speakerCount} speakers): ${pageLog()}`);
  } else {
    ok(`an uploaded layout rebuilt the array: ${s.speakerCount} speakers, ` +
       `${s.channelCount} bus channels`);
    /* The DISTINCTIVE speaker, read back out of the engine rather than out of the file: this is
     * what says the engine loaded the upload and not something that happened to be there. */
    const got = s.layout.speakers.slice(9, 12);
    const near = got.every((v, i) => Math.abs(v - odd[i]) < 1e-3);
    if (!near) fail(`bwa_get_speakers put speaker 3 at ${got.map((v) => v.toFixed(2))}, expected ${odd}`);
    else ok(`bwa_get_speakers reads speaker 3 back at ${got.map((v) => v.toFixed(2)).join(", ")}`);
    if (s.view.coneLevels.length !== 8)
      fail(`${s.view.coneLevels.length} cones are drawn for an 8-speaker layout`);
    else ok("the scene draws one cone per loaded speaker");
    if (!/layout driver-8\.json/.test(String(s.view.status)))
      fail(`the status line does not name the loaded layout: "${s.view.status}"`);
    else ok("the status line names the loaded layout");
  }

  /* A file the PAGE can refuse, without an engine rebuild. */
  const tooFew = await pg.loadLayout(JSON.stringify(testLayout(3)), "too-few.json");
  if (tooFew !== false) fail("a 3-speaker layout was not refused");
  else if (pg.state().speakerCount !== 8)
    fail("a refused layout still rebuilt the engine");
  else ok("a layout with too few speakers is refused before the engine is touched");

  /* A file the page CANNOT refuse but the engine does: gain_db 99 is outside the loader's
   * [-100, 24]. The engine's rule is that create stays usable on its built-in grid and START then
   * refuses with BWA_ERR_LAYOUT, so what the page owes is the reason and a working engine, which it
   * gets by going back to the dome. */
  const bad = testLayout(8);
  bad.speakers[2].gain_db = 99;
  const refused = await pg.loadLayout(JSON.stringify(bad), "bad-gain.json");
  const after = pg.state();
  if (refused !== false) fail("the engine accepted a layout with gain_db 99");
  else if (!after.errors.some((e) => /refused that layout/.test(e)))
    fail(`the page did not report the engine's refusal: ${after.errors.join(" | ")}`);
  else if (after.speakerCount !== DOME || after.layout.name !== DOME_NAME)
    fail(`after the refusal the page is on a ${after.speakerCount}-speaker layout (${after.layout.name}), not the dome`);
  else if (after.health && after.health.deviceLost !== 0)
    fail("after the refusal the sink is host-pacing silence");
  else ok(`an engine-refused layout is reported and the page falls back to the dome`);

  /* reset: back to the dome from an upload, not to the engine's built-in grid */
  await pg.loadLayout(JSON.stringify(testLayout(8)), "driver-8b.json");
  await pg.loadLayout(null, null);
  const reset = pg.state();
  if (reset.speakerCount !== DOME || reset.layout.name !== DOME_NAME)
    fail(`reset left the page on ${reset.speakerCount} speakers (${reset.layout.name}), not the dome`);
  else ok("reset goes back to the dome");
}

/**
 * Wait for the sink to stop host-pacing, and say how long it took.
 *
 * `device_lost` is the sink's report that the AudioWorklet's heartbeat has gone quiet, and after a
 * REBUILD it is quiet for as long as the new context's asynchronous setup chain takes:
 * `addModule`, the node, the connect. That is a browser's own scheduling and on a loaded machine
 * it can run past a fixed sleep, so a single read at a fixed moment measures the machine and not
 * the engine (measured: the same read failed on the old build too, so it is not a regression -
 * it is a badly posed question). What the check MEANS is that the worklet comes back at all,
 * which is exactly what the defect behind it - a second `registerProcessor` in one context's
 * worklet scope - makes impossible forever. So poll, with a deadline.
 * @returns {number} milliseconds until it cleared, or -1 if it never did
 */
async function waitDeviceLive(pg, ms = 4000) {
  const t0 = performance.now();
  for (;;) {
    const h = pg.state().health;
    if (h && h.deviceLost === 0) return performance.now() - t0;
    if (performance.now() - t0 > ms) return -1;
    await sleep(60);
  }
}

/** A mono float32 wav of a tone, which is what the page's audio file input hands to `loadClip`. */
function driverWav(seconds, hz) {
  const rate = 48000;
  const n = Math.round(rate * seconds);
  const buf = new ArrayBuffer(44 + n * 4);
  const v = new DataView(buf);
  const tag = (o, t) => { for (let i = 0; i < t.length; ++i) v.setUint8(o + i, t.charCodeAt(i)); };
  tag(0, "RIFF"); v.setUint32(4, 36 + n * 4, true); tag(8, "WAVE");
  tag(12, "fmt "); v.setUint32(16, 16, true);
  v.setUint16(20, 3, true); v.setUint16(22, 1, true);
  v.setUint32(24, rate, true); v.setUint32(28, rate * 4, true);
  v.setUint16(32, 4, true); v.setUint16(34, 32, true);
  tag(36, "data"); v.setUint32(40, n * 4, true);
  const out = new Float32Array(buf, 44);
  for (let i = 0; i < n; ++i) out[i] = 0.3 * Math.sin((2 * Math.PI * hz * i) / rate);
  return buf;
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
