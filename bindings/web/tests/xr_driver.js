/**
 * xr_driver.js - the assertions run-xr.mjs runs inside the XR page, plus the fake XR device they
 * need.
 *
 * IT IS NOT PART OF THE PAGE. The runner's server injects a script tag for this file into the XR
 * page's HEAD when, and only when, the request carries `?__drive=1`, so the shipped page carries
 * no test code and the check still drives the REAL page. It is a CLASSIC script, not a module,
 * and it is in the head rather than at the end of the body, for one reason: `xr/main.js` asks
 * `navigator.xr` whether a headset exists as soon as it evaluates, and a module script is
 * deferred, so anything appended before `</body>` would arrive too late to answer.
 *
 * `?__xr=0` deletes `navigator.xr`; `?__xr=1` installs the fake below. Those are the page's first
 * two states and neither can be reached by a script that runs afterwards.
 *
 * THE FAKE IS A DEVICE, NOT A HOOK INTO THE PAGE. It implements the small part of the WebXR
 * surface the page uses - `isSessionSupported`, `requestSession`, `requestReferenceSpace`,
 * `requestAnimationFrame` with a synthetic `XRFrame`, `getViewerPose`, `getPose`, `inputSources`,
 * the select and squeeze events, and `end` - and the page cannot tell it is not a runtime. The
 * assertions then move a head and press a trigger, which is the only way to make the head-pose
 * path run without a headset.
 *
 * THE QUATERNION ARITHMETIC HERE IS WRITTEN OUT BY HAND rather than imported from
 * `xr/frame_xr.js`. That is deliberate: a test that computes its inputs with the code under test
 * proves only that the code agrees with itself. `tests/xr_frame.test.mjs` pins the seam against
 * the ABI's basis vectors under node; this file pins the PAGE against numbers a reader can check.
 *
 * THE LATERALITY STIMULUS IS A CLICK, never DC (xr/probe.js drives it). CLAUDE.md's first trap.
 */
(function () {
  "use strict";

  var params = new URLSearchParams(location.search);
  var WANT_XR = params.get("__xr") === "1";

  var notes = [];
  function ok(m) { notes.push("ok   " + m); }
  function fail(m) { notes.push("FAIL " + m); }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  function db(x) { return (20 * Math.log10(Math.max(x, 1e-9))).toFixed(1); }
  function r3(x) { return Math.round(x * 1000) / 1000; }

  /* ---------------------------------------------------------------- hand arithmetic */

  function norm3(v) {
    var n = Math.hypot(v[0], v[1], v[2]);
    return n > 1e-9 ? [v[0] / n, v[1] / n, v[2] / n] : [0, 0, 1];
  }
  /* The room-to-XR maps, spelled out. A 180 degree yaw, self-inverse, for a point; the same
   * rotation sandwiched for an orientation. If the page's frame_xr.js disagrees with these four
   * lines, every pose assertion below goes red. */
  function roomPointToXr(p) { return { x: -p[0], y: p[1], z: -p[2] }; }
  function roomQuatToXr(q) {
    var n = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
    return { x: q[0] / n, y: -q[1] / n, z: q[2] / n, w: -q[3] / n };
  }
  /** The shortest rotation taking unit vector a onto unit vector b, as xyzw. */
  function quatFromTo(a, b) {
    var d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d > 0.999999) return [0, 0, 0, 1];
    if (d < -0.999999) {
      var ax = Math.abs(a[0]) < 0.9 ? [1, 0, 0] : [0, 1, 0];
      var c0 = [a[1] * ax[2] - a[2] * ax[1], a[2] * ax[0] - a[0] * ax[2], a[0] * ax[1] - a[1] * ax[0]];
      c0 = norm3(c0);
      return [c0[0], c0[1], c0[2], 0];
    }
    var c = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    var q = [c[0], c[1], c[2], 1 + d];
    var n = Math.hypot(q[0], q[1], q[2], q[3]);
    return [q[0] / n, q[1] / n, q[2] / n, q[3] / n];
  }
  /** A yaw about +y, radians. Positive turns a WebXR viewer, and a room listener, to the LEFT. */
  function yawQuat(rad) { return [0, Math.sin(rad / 2), 0, Math.cos(rad / 2)]; }
  function sameRot(a, b, eps) {
    var s = true, f = true;
    for (var i = 0; i < 4; ++i) {
      if (Math.abs(a[i] - b[i]) > eps) s = false;
      if (Math.abs(a[i] + b[i]) > eps) f = false;
    }
    return s || f;
  }

  /* ---------------------------------------------------------------- the fake device */

  var fake = {
    /* Everything here is in the XR REFERENCE SPACE, which is what a runtime would report. The
     * page converts; the assertions check the conversion. */
    pos: { x: 0, y: 1.6, z: 0 },
    quat: { x: 0, y: 0, z: 0, w: 1 },
    lost: false,
    session: null,
    controllers: [],
  };
  globalThis.__fakeXr = fake;

  function rigid(p, q) {
    return {
      position: { x: p.x, y: p.y, z: p.z, w: 1 },
      orientation: { x: q.x, y: q.y, z: q.z, w: q.w },
    };
  }

  function makeController(handedness) {
    return {
      handedness: handedness,
      targetRayMode: "tracked-pointer",
      profiles: ["generic-trigger-squeeze-thumbstick"],
      targetRaySpace: { __space: "ray-" + handedness },
      gripSpace: { __space: "grip-" + handedness },
      gamepad: { mapping: "xr-standard", axes: [0, 0, 0, 0], buttons: [] },
      /* the driver's own bookkeeping, invisible to the page */
      pos: { x: handedness === "left" ? -0.2 : 0.2, y: 1.2, z: -0.3 },
      quat: { x: 0, y: 0, z: 0, w: 1 },
      present: true,
    };
  }

  function makeSession() {
    var listeners = {};
    var ended = false;
    var session = {
      inputSources: fake.controllers,
      renderState: {},
      addEventListener: function (t, f) { (listeners[t] = listeners[t] || []).push(f); },
      removeEventListener: function (t, f) {
        var a = listeners[t]; if (!a) return;
        var i = a.indexOf(f); if (i >= 0) a.splice(i, 1);
      },
      /* A runtime dispatches to every listener and one listener's exception does not stop the
       * others. three.js registers eight of its own on a session it then failed to bind, so this
       * has to survive them throwing. */
      dispatch: function (type, extra) {
        var a = (listeners[type] || []).slice();
        for (var i = 0; i < a.length; ++i) {
          try { a[i](Object.assign({ type: type, session: session }, extra || {})); }
          catch (e) { /* a listener from a half-bound renderer; not the page's problem */ }
        }
      },
      updateRenderState: function () {},
      requestReferenceSpace: function (type) {
        if (type === "local-floor") return Promise.resolve({ __space: "local-floor" });
        return Promise.reject(new Error('unsupported reference space "' + type + '"'));
      },
      requestAnimationFrame: function (cb) {
        return requestAnimationFrame(function (t) {
          if (ended) return;
          cb(t, makeFrame(session));
        });
      },
      cancelAnimationFrame: function (h) { cancelAnimationFrame(h); },
      end: function () {
        if (ended) return Promise.resolve();
        ended = true;
        session.dispatch("end", {});
        return Promise.resolve();
      },
    };
    return session;
  }

  function makeFrame(session) {
    return {
      session: session,
      predictedDisplayTime: performance.now(),
      getViewerPose: function () {
        if (fake.lost) return null;
        return { transform: rigid(fake.pos, fake.quat), emulatedPosition: false, views: [] };
      },
      getPose: function (space) {
        for (var i = 0; i < fake.controllers.length; ++i) {
          var c = fake.controllers[i];
          if ((c.targetRaySpace === space || c.gripSpace === space) && c.present) {
            return { transform: rigid(c.pos, c.quat) };
          }
        }
        return null;
      },
    };
  }

  if (WANT_XR) {
    fake.controllers.push(makeController("left"), makeController("right"));
    Object.defineProperty(navigator, "xr", {
      configurable: true,
      value: {
        isSessionSupported: function (mode) { return Promise.resolve(mode === "immersive-vr"); },
        requestSession: function (mode) {
          if (mode !== "immersive-vr") return Promise.reject(new Error("unsupported mode"));
          fake.session = makeSession();
          return Promise.resolve(fake.session);
        },
        addEventListener: function () {},
        removeEventListener: function () {},
      },
    });
  } else {
    /* The no-WebXR state, which is most desktop browsers and every phone browser. */
    Object.defineProperty(navigator, "xr", { configurable: true, value: undefined });
  }

  /* ---------------------------------------------------------------- helpers */

  function waitFor(fn, ms, what) {
    var t0 = performance.now();
    return (function loop() {
      var v = fn();
      if (v) return Promise.resolve(v);
      if (performance.now() - t0 > ms) return Promise.reject(new Error("timed out waiting for " + what));
      return sleep(50).then(loop);
    })();
  }

  function pageLog() {
    var el = document.getElementById("log");
    return (el ? el.textContent : "(no log)").slice(0, 800);
  }

  function labelsOf(items) {
    return items.filter(function (i) { return i.label; }).map(function (i) { return i.label; });
  }

  /* ---------------------------------------------------------------- pass 1: no WebXR */

  async function noXrPass(xr) {
    var supported = await xr.supportReady;
    if (supported !== false) fail("the page thinks an immersive-vr device exists with no navigator.xr");
    else ok("no navigator.xr: the page reports no immersive-vr device");

    if (document.getElementById("noXr").hidden) fail("the no-WebXR note is hidden");
    else ok("the page shows the no-WebXR note");
    if (!document.getElementById("enter").disabled) fail("Enter VR is still enabled with no WebXR");
    else ok("Enter VR is disabled");
    var links = Array.prototype.map.call(document.querySelectorAll("#noXr a, header a"),
                                         function (a) { return a.getAttribute("href"); });
    if (links.indexOf("../playground/") < 0) fail("the page does not link to the flat playground");
    else ok("the page links to the flat playground");

    document.getElementById("flat").click();
    var started = await Promise.race([xr.ready, sleep(60000).then(function () { return "timeout"; })]);
    if (started !== true) throw new Error("the flat preview did not start (" + started + "); page log: " + pageLog());
    ok("the flat preview started and the engine is up");

    var s = xr.state();
    if (!/^worklet/.test(String(s.backend))) fail('backend is "' + s.backend + '", not the worklet sink');
    else ok('backend "' + s.backend + '"');
    if (s.sinkType !== 9) fail("sink type " + s.sinkType + ", expected 9 (WORKLET)");
    else ok("sink type 9 (WORKLET)");
    if (s.channelCount !== 26) fail(s.channelCount + " bus channels, expected the 26 of the CAVE layout");
    else ok("26 bus channels");
    if (s.speakerCount !== 26) fail("bwa_get_speakers gave " + s.speakerCount + " speakers, expected 26");
    else ok("bwa_get_speakers read back 26 speaker positions");
    if (s.presenting) fail("the page claims to be presenting with no WebXR");
    else ok("not presenting, as there is no device");

    /* The head sits at the nominal listening pose and does not move, which is exactly the thing
     * this page exists to improve on. */
    if (Math.abs(s.headRoom[0]) > 1e-6 || Math.abs(s.headRoom[2]) > 1e-6)
      fail("the flat preview's head is not at the room origin: " + s.headRoom);
    else ok("the flat preview's listener is at the nominal pose " + JSON.stringify(s.headRoom.map(r3)));

    var labels = labelsOf(xr.menuItems());
    var want = ["scene", "render profile", "stimulus", "master gain", "pose prediction lead"];
    for (var i = 0; i < want.length; ++i) {
      if (labels.indexOf(want[i]) < 0) fail('the menu has no "' + want[i] + '" control');
    }
    if (labels.length <= want.length)
      fail("the menu carries no per-scene controls: " + labels.join(", "));
    else ok("the menu carries " + want.length + " global controls plus " +
            (labels.length - want.length) + " from the scene");
    var scenePick = xr.menuItems()[0];
    if (!scenePick.options || scenePick.options.length !== xr.scenes.length)
      fail("the scene picker offers " + (scenePick.options || []).length + " of " + xr.scenes.length + " scenes");
    else ok("the scene picker offers all " + xr.scenes.length + " playground scenes");

    /* The health block is filled by the page's own slow poll, so the first read after a start can
     * legitimately be null. Waiting for it is not papering over a race; the alternative is a check
     * whose result depends on how long the engine took to open. */
    await waitFor(function () { return xr.state().health; }, 15000, "the first health poll");
    var h0 = xr.state().health;
    await sleep(800);
    var h1 = xr.state().health;
    var blocks = h1.blocks - h0.blocks;
    ok(blocks + " blocks rendered in 800 ms");
    if (blocks < 40) fail("only " + blocks + " blocks in 800 ms: the render is not running");
    if (h1.deviceLost !== 0) fail("device_lost is set: the sink is host-pacing silence");
    else ok("device_lost is 0: the AudioWorklet is driving");
  }

  /* ---------------------------------------------------------------- pass 2: fake XR */

  /** Put the fake head at a ROOM pose, and wait for the page to have picked it up. */
  async function head(roomP, roomQ) {
    fake.pos = roomPointToXr(roomP);
    fake.quat = roomQuatToXr(roomQ || [0, 0, 0, 1]);
    await sleep(120);
  }

  async function xrPass(xr) {
    var supported = await xr.supportReady;
    if (supported !== true) fail("the page did not see the fake immersive-vr device");
    else ok("the fake navigator.xr reports an immersive-vr device");
    if (document.getElementById("enter").disabled) fail("Enter VR is disabled with a device present");
    else ok("Enter VR is enabled");

    document.getElementById("enter").click();
    var started = await Promise.race([xr.ready, sleep(60000).then(function () { return "timeout"; })]);
    if (started !== true) throw new Error("the engine did not start (" + started + "); page log: " + pageLog());
    await waitFor(function () { return xr.state().presenting; }, 20000, "the session to open");
    ok("an immersive-vr session opened and the engine is up");

    var s = xr.state();
    if (s.refSpaceType !== "local-floor")
      fail('the page took a "' + s.refSpaceType + '" reference space, expected local-floor');
    else ok("local-floor reference space, so room y is the real floor with no offset");
    /* HONESTY, not an assertion: headless Chrome has no WebXR device, so three.js cannot bind a
     * framebuffer to the fake session. The page is built so the pose path does not care, which is
     * the property being exercised here. A headset binds it. */
    ok("renderer bound to the session: " + s.renderReady +
       " (headless has no XR framebuffer; the pose path runs either way)");

    var f0 = xr.state().xrFrames;
    await sleep(500);
    var f1 = xr.state().xrFrames;
    if (f1 - f0 < 10) fail("only " + (f1 - f0) + " XR frames in 500 ms: the session loop is not running");
    else ok((f1 - f0) + " XR animation frames in 500 ms");

    /* ---- THE SEAM, one axis at a time ---- */
    xr.setLeadSeconds(0);                     /* the lead is measured separately, below */
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    s = xr.state();
    if (Math.abs(s.headRoom[0]) > 2e-3 || Math.abs(s.headRoom[1] - 1.6) > 2e-3 || Math.abs(s.headRoom[2]) > 2e-3)
      fail("a viewer at the reference-space origin landed at room " + JSON.stringify(s.headRoom.map(r3)));
    else ok("a viewer at the origin is a listener at room " + JSON.stringify(s.headRoom.map(r3)));
    if (!sameRot(s.headQuat, [0, 0, 0, 1], 1e-4))
      fail("an unturned viewer gave listener quaternion " + JSON.stringify(s.headQuat.map(r3)));
    else ok("an unturned viewer is a listener facing room ahead (identity)");

    /* Walking FORWARD in XR is -z there and +z here. A sign error walks the listener backwards
     * through every scene and is inaudible in a still frame. */
    fake.pos = { x: 0, y: 1.6, z: -1.0 };
    await sleep(150);
    s = xr.state();
    if (Math.abs(s.headRoom[2] - 1.0) > 3e-3)
      fail("walking 1 m forward in XR put the listener at room z " + r3(s.headRoom[2]) + ", expected +1");
    else ok("walking forward in XR moves the listener toward room ahead (+z)");

    /* Stepping to the viewer's RIGHT is +x in XR and -x in the room, because BWA_ROOM_RIGHT
     * is -x. This is the sign the whole page is about. */
    fake.pos = { x: 0.5, y: 1.6, z: 0 };
    await sleep(150);
    s = xr.state();
    if (Math.abs(s.headRoom[0] + 0.5) > 3e-3)
      fail("stepping 0.5 m right in XR put the listener at room x " + r3(s.headRoom[0]) + ", expected -0.5");
    else ok("stepping right in XR moves the listener to room -x, the listener's right");

    /* ---- THE POINT OF THE PAGE: the head turns and the image moves ---- */
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    xr.setSourceRoom([0, 1.6, 2.0]);          /* straight ahead, at ear height */
    await sleep(200);
    if (xr.state().earSide !== 0)
      fail("a source straight ahead reads as ear side " + xr.state().earSide);
    else ok("a source straight ahead is centered on the head's own axes");
    var ahead = await xr.measureLaterality([0, 1.6, 2.0]);
    ok("looking at the source: L " + db(ahead.left) + " dB, R " + db(ahead.right) +
       " dB, ratio " + ahead.ratioDb.toFixed(2) + " dB");
    if (Math.abs(ahead.ratioDb) > 2.5)
      fail("a source the listener is looking straight at rendered " + ahead.ratioDb.toFixed(2) +
           " dB off center");
    else ok("a source the listener is looking at renders centered");

    /* Turn a quarter to the LEFT. The source that was ahead is now off the RIGHT ear. */
    await head([0, 1.6, 0], yawQuat(Math.PI / 2));
    s = xr.state();
    if (!sameRot(s.headQuat, yawQuat(Math.PI / 2), 1e-3))
      fail("a quarter turn left gave listener quaternion " + JSON.stringify(s.headQuat.map(r3)));
    else ok("a quarter turn left in XR is a quarter turn left in the room");
    if (s.earSide !== -1) fail("after turning left the source ahead reads as ear side " + s.earSide);
    else ok("after turning left the source ahead is on the listener's RIGHT");

    /* CLOSE THE LOOP. Everything above reads what the page COMPUTED. This reads what the page's
     * own engine is rendering with, through bwa_get_listener_pose, so a page that converted the
     * pose perfectly and then forgot to send it cannot pass. The offline probe cannot catch that:
     * it is a second engine and the driver hands it the pose itself. */
    var got = await xr.listenerPose();
    if (!sameRot(got.q, s.headQuat, 2e-3))
      fail("the engine is rendering with quaternion " + JSON.stringify(got.q.map(r3)) +
           " while the page computed " + JSON.stringify(s.headQuat.map(r3)));
    else ok("bwa_get_listener_pose reads back the turned head: " + JSON.stringify(got.q.map(r3)));
    if (Math.abs(got.p[0] - s.headRoom[0]) > 3e-3 || Math.abs(got.p[1] - s.headRoom[1]) > 3e-3 ||
        Math.abs(got.p[2] - s.headRoom[2]) > 3e-3)
      fail("the engine is rendering at " + JSON.stringify(got.p.map(r3)) +
           " while the page computed " + JSON.stringify(s.headRoom.map(r3)));
    else ok("bwa_get_listener_pose reads back the position too");
    var turnedL = await xr.measureLaterality([0, 1.6, 2.0]);
    ok("turned left: L " + db(turnedL.left) + " dB, R " + db(turnedL.right) +
       " dB, ratio " + turnedL.ratioDb.toFixed(2) + " dB");
    if (!(turnedL.ratioDb < -3))
      fail("with the head turned left the source did not move to the RIGHT ear (" +
           turnedL.ratioDb.toFixed(2) + " dB)");
    else ok("with the head turned left the render is louder in the RIGHT ear");

    /* And the mirror image, so a single-sided bug cannot pass. */
    await head([0, 1.6, 0], yawQuat(-Math.PI / 2));
    if (xr.state().earSide !== 1) fail("after turning right the source ahead reads as ear side " + xr.state().earSide);
    else ok("after turning right the source ahead is on the listener's LEFT");
    var turnedR = await xr.measureLaterality([0, 1.6, 2.0]);
    ok("turned right: L " + db(turnedR.left) + " dB, R " + db(turnedR.right) +
       " dB, ratio " + turnedR.ratioDb.toFixed(2) + " dB");
    if (!(turnedR.ratioDb > 3))
      fail("with the head turned right the source did not move to the LEFT ear (" +
           turnedR.ratioDb.toFixed(2) + " dB)");
    else ok("with the head turned right the render is louder in the LEFT ear");

    /* The DISCRIMINATING claim: the turn is what moved it. Both sides have to beat the centered
     * case by a margin, or two numbers near zero would "pass" a page that ignores orientation. */
    var margin = Math.min(Math.abs(turnedL.ratioDb), Math.abs(turnedR.ratioDb)) - Math.abs(ahead.ratioDb);
    if (!(margin > 4))
      fail("turning the head changed the laterality by only " + margin.toFixed(2) +
           " dB over the centered case");
    else ok("turning the head is worth " + margin.toFixed(2) + " dB of laterality over facing it");

    /* ---- the prediction lead ---- */
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    var lead0 = await walkAndMeasure(xr, 0);
    var lead80 = await walkAndMeasure(xr, 0.08);
    var want80 = 1.2 * 0.08;                  /* the walk below runs at 1.2 m/s */
    ok("pose lead: at 0 ms the listener trails the device by " + r3(lead0) + " m, at 80 ms by " +
       r3(lead80) + " m (the lead is worth " + r3(lead80 - lead0) + " m, " + r3(want80) + " m expected)");
    if (!(lead80 - lead0 > 0.5 * want80 && lead80 - lead0 < 1.8 * want80))
      fail("an 80 ms lead at 1.2 m/s should be about " + r3(want80) + " m; measured " +
           r3(lead80 - lead0) + " m");
    else ok("the prediction lead moves the rendered listener ahead by about the right distance");
    xr.setLeadSeconds(0.03);

    /* ---- the in-world menu ---- */
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    await sleep(600);                          /* the panel's lazy follow has to settle */
    var m = xr.menu();
    if (!m || !m.visible) fail("the in-world menu is not visible in session");
    else ok('the in-world menu is up, titled "' + m.title + '", ' + m.rows.length + " rows");
    if (m && m.rows.length < 5) fail("the in-world menu painted only " + m.rows.length + " rows");

    var before = xr.menuItems();
    var stimIndex = -1;
    before.forEach(function (it, i) { if (it.label === "stimulus") stimIndex = i; });
    if (stimIndex < 0) fail("no stimulus control on the panel");
    else {
      var hit = await pressRow(xr, stimIndex, 0.5);
      if (!hit) fail("the controller ray never reached the stimulus row");
      else {
        await sleep(300);
        var after = xr.menuItems()[stimIndex].value;
        if (after === before[stimIndex].value)
          fail("pressing the stimulus control did not change it (still " + after + ")");
        else ok("a trigger press on the in-world panel changed the stimulus to " + after);
      }
    }

    /* ---- a gaze input (a phone viewer) draws no controller at the eyes ---- */
    /* A Cardboard-style runtime reports ONE input with targetRayMode "gaze" whose ray starts at
     * the viewer. The page used to draw it as a hand, which put the grip sphere on the user's
     * face (reported from a phone, 2026-09-22). Swap the two controllers for a gaze that follows
     * the head and check what is drawn in two head poses. The session holds `fake.controllers`
     * by reference, so the array is mutated in place, never reassigned. */
    var savedControllers = fake.controllers.slice();
    var gaze = {
      handedness: "none",
      targetRayMode: "gaze",
      targetRaySpace: { __space: "gaze" },
      gripSpace: null,
      gamepad: null,
      present: true,
      get pos() { return fake.pos; },
      get quat() { return fake.quat; },
    };
    fake.controllers.length = 0;
    fake.controllers.push(gaze);
    var gazeStates = [];
    /* Expected states, from the gaze follow policy: the panel parks centered 0.72 m ahead and
     * 0.10 m down (0.52 by 0.91 m, so about 20 degrees wide and 32 degrees tall from the eyes),
     * holds while the gaze is within 40 degrees, and re-parks beyond that. */
    var poses = [
      ["ahead", [0, 0, 0, 1], true],                                       /* on the panel */
      ["30 degrees to one side", [0, 0.2588190, 0, 0.9659258], false],   /* past its edge, inside the hold */
      ["straight up", [-0.7071068, 0, 0, 0.7071068], true],              /* 90 degrees: it re-parks */
    ];
    for (var gi = 0; gi < poses.length; ++gi) {
      await head([0, 1.6, 0], poses[gi][1]);
      await sleep(900);                        /* the panel's re-park has to settle */
      var gh = xr.hands();
      var gv = xr.handVisual(0);
      var other = xr.handVisual(1);
      gazeStates.push({ name: poses[gi][0], hands: gh, visual: gv });
      if (gh && gh[0].present && gh[0].pointingAtMenu !== poses[gi][2])
        fail("looking " + poses[gi][0] + ": the gaze is " + (gh[0].pointingAtMenu ? "on" : "off") +
             " the menu, expected " + (poses[gi][2] ? "on" : "off") + " (the panel is not parking for a gaze)");
      else if (gh && gh[0].present)
        ok("looking " + poses[gi][0] + ": the gaze is " + (gh[0].pointingAtMenu ? "on" : "off") + " the menu, as the gaze follow policy says");
      if (!gh || !gh[0].present || gh[0].mode !== "gaze")
        fail("the gaze input is not reported as present with mode gaze (" + JSON.stringify(gh && gh[0]) + ")");
      if (!gv) fail("handVisual(0) returned nothing for the gaze input");
      else {
        if (gv.grip || gv.ray)
          fail("looking " + poses[gi][0] + ": the gaze input draws a grip or a ray at the eyes (grip " +
               gv.grip + ", ray " + gv.ray + ")");
        else ok("looking " + poses[gi][0] + ": the gaze input draws no grip and no ray");
        if (gv.reticle !== gh[0].pointingAtMenu)
          fail("looking " + poses[gi][0] + ": the gaze reticle is " + (gv.reticle ? "shown" : "hidden") +
               " while the gaze is " + (gh[0].pointingAtMenu ? "on" : "off") + " the menu");
        else ok("looking " + poses[gi][0] + ": the gaze reticle is " + (gv.reticle ? "shown" : "hidden") +
                ", matching the gaze being " + (gh[0].pointingAtMenu ? "on" : "off") + " the menu");
      }
      if (other && (other.grip || other.ray || other.reticle))
        fail("the empty second input slot draws something while only a gaze input exists");
    }
    var sawOn = false, sawOff = false;
    for (var si = 0; si < gazeStates.length; ++si) {
      var st = gazeStates[si].hands;
      if (st && st[0].pointingAtMenu) sawOn = true; else sawOff = true;
    }
    if (!(sawOn && sawOff))
      fail("the gaze reticle was not checked in both states (on the menu: " + sawOn + ", off: " + sawOff + ")");
    else ok("the gaze reticle was checked both on and off the menu");
    fake.controllers.length = 0;
    for (var ri = 0; ri < savedControllers.length; ++ri) fake.controllers.push(savedControllers[ri]);
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    await sleep(200);

    /* ---- the grab ---- */
    await head([0, 1.6, 0], [0, 0, 0, 1]);
    xr.setSourceRoom([0.0, 1.5, 1.2]);
    await sleep(150);
    aimAway(1);
    var c = fake.controllers[0];
    c.pos = roomPointToXr([0.0, 1.5, 1.2]);    /* the hand is ON the source */
    await sleep(150);
    fake.session.dispatch("selectstart", { inputSource: c });
    await sleep(200);
    var hands = xr.hands();
    if (!hands || !hands[0].grabbing) fail("a trigger press at the source did not grab it");
    else ok("the nearer controller grabbed the source");
    var src0 = xr.state().sourceRoom.slice();
    c.pos = { x: c.pos.x + 0.4, y: c.pos.y, z: c.pos.z };    /* move the hand 0.4 m XR +x */
    await sleep(250);
    var src1 = xr.state().sourceRoom.slice();
    if (Math.abs((src1[0] - src0[0]) + 0.4) > 0.02)
      fail("the grabbed source moved by " + r3(src1[0] - src0[0]) + " m in room x, expected -0.4");
    else ok("the grabbed source follows the hand through the same seam (room x -0.4 m)");
    fake.session.dispatch("selectend", { inputSource: c });
    await sleep(150);
    if (xr.hands()[0].grabbing) fail("releasing the trigger did not release the source");
    else ok("releasing the trigger released the source");

    /* ---- the thumbstick nudge ---- */
    aimAway(0);
    aimAway(1);
    var c1 = fake.controllers[1];
    var n0 = xr.state().sourceRoom.slice();
    c1.gamepad.axes = [0, 0, 1.0, 0];          /* full stick right */
    await sleep(400);
    c1.gamepad.axes = [0, 0, 0, 0];
    await sleep(150);
    var n1 = xr.state().sourceRoom.slice();
    /* The head is unturned, so the listener's right is room -x and the source should have gone
     * there. The nudge is relative to the HEAD, which is the whole reason it is not room axes. */
    if (!(n1[0] < n0[0] - 0.1))
      fail("a full thumbstick right moved the source by " + r3(n1[0] - n0[0]) + " m in room x");
    else ok("the thumbstick nudges the source toward the listener's right (" +
            r3(n1[0] - n0[0]) + " m of room x)");

    /* ---- the scene picker, from inside the session ---- */
    var sceneBefore = xr.state().sceneId;
    var pressed = await pressRow(xr, 0, 0.5);
    if (!pressed) fail("the controller ray never reached the scene row");
    else {
      await waitFor(function () { return xr.state().sceneId !== sceneBefore; }, 8000,
                    "the scene to change").catch(function () {});
      if (xr.state().sceneId === sceneBefore)
        fail('pressing the scene control left the page on "' + sceneBefore + '"');
      else ok('the scene menu works in session: "' + sceneBefore + '" -> "' + xr.state().sceneId + '"');
    }

    /* ---- ending the session drops back to the flat preview ---- */
    await fake.session.end();
    await waitFor(function () { return !xr.state().presenting; }, 8000, "the session to end");
    ok("ending the session returned the page to the flat preview");
    var menuAfter = xr.menu();
    if (menuAfter && menuAfter.visible) fail("the in-world menu is still visible after the session ended");
    else ok("the in-world menu went away with the session");
    var eh0 = xr.state().health;
    await sleep(700);
    var eh1 = xr.state().health;
    if (eh1.blocks - eh0.blocks < 40)
      fail("after the session ended the render stopped (" + (eh1.blocks - eh0.blocks) + " blocks in 700 ms)");
    else ok("the engine keeps rendering after the session ends (" + (eh1.blocks - eh0.blocks) + " blocks)");
  }

  /** Point controller `i` at nothing in particular, well away from the panel and the source. */
  function aimAway(i) {
    var c = fake.controllers[i];
    c.pos = { x: i === 0 ? -0.25 : 0.25, y: 1.1, z: 1.5 };
    c.quat = roomQuatToXr(quatFromTo([0, 0, 1], [0, -1, 0]));   /* pointing at the floor */
  }

  /**
   * Aim a controller at a row of the in-world panel and pull the trigger. Returns false when the
   * ray missed, which is a real failure and not a skip: the hit test is part of what is checked.
   */
  async function pressRow(xr, itemIndex, u) {
    var m = xr.menu();
    if (!m) return false;
    var row = null;
    for (var i = 0; i < m.rows.length; ++i) if (m.rows[i].index === itemIndex) row = m.rows[i];
    if (!row) return false;
    var PAD = 18;
    var x = PAD + u * (m.canvas[0] - 2 * PAD);
    var y = (row.y0 + row.y1) / 2 - m.scroll;
    var target = xr.menuPointAt(x, y);
    aimAway(0);                    /* so the other hand cannot own the panel's hover */
    var c = fake.controllers[1];
    /* Stand the hand off to one side of the head so the ray is not degenerate, then aim it. */
    var from = [0.25, 1.35, 0.1];
    c.pos = roomPointToXr(from);
    var dir = norm3([target[0] - from[0], target[1] - from[1], target[2] - from[2]]);
    c.quat = roomQuatToXr(quatFromTo([0, 0, 1], dir));
    await sleep(200);
    if (xr.menu().hover < 0) return false;
    fake.session.dispatch("selectstart", { inputSource: c });
    await sleep(120);
    fake.session.dispatch("selectend", { inputSource: c });
    return true;
  }

  /**
   * Walk the fake head at a steady 1.2 m/s along room +z and report how far the pose the page is
   * rendering with sits BEHIND the device's own position, averaged over many samples.
   *
   * The average is the point. A single sample carries whatever scheduling lag there was between
   * the page's frame and this script's read, which at 1.2 m/s is centimeters. That lag is the
   * same for both lead settings, so the DIFFERENCE of two averages is the lead and nothing else.
   */
  async function walkAndMeasure(xr, leadSeconds) {
    xr.setLeadSeconds(leadSeconds);
    var v = 1.2;
    var t0 = performance.now();
    var z = 0;
    var sum = 0;
    var n = 0;
    while (performance.now() - t0 < 1400) {
      var t = (performance.now() - t0) / 1000;
      z = v * t;
      fake.pos = { x: 0, y: 1.6, z: -z };      /* XR -z is room +z */
      await sleep(16);
      if (t > 0.6) {                           /* let the 100 ms average settle first */
        sum += xr.state().headRoom[2] - z;
        n++;
      }
    }
    fake.pos = { x: 0, y: 1.6, z: -z };
    await sleep(200);
    return n ? sum / n : 0;
  }

  /* ---------------------------------------------------------------- run */

  async function main() {
    if (!globalThis.crossOriginIsolated) throw new Error("the page is not cross-origin isolated");
    ok("cross-origin isolated");
    var xr = await waitFor(function () { return globalThis.__bwaXr; }, 20000, "window.__bwaXr");
    if (WANT_XR) await xrPass(xr);
    else await noXrPass(xr);
    var errs = xr.state().errors;
    if (errs.length) fail("the page logged " + errs.length + " engine error(s): " + errs.join(" | "));
    else ok("no engine errors");
  }

  function report(err) {
    if (err) notes.push("FAIL threw: " + (err && err.stack ? err.stack : String(err)));
    var body = { pass: notes.filter(function (n) { return n.indexOf("FAIL") === 0; }).length === 0, notes: notes };
    var url = globalThis.__BWA_RESULT_URL || "/__result";
    fetch(url, { method: "POST", body: JSON.stringify(body) }).catch(function () {});
  }

  /* The page's own module has to evaluate first, and the service worker may reload us once, so
   * the run waits for the hook rather than for an event. */
  main().then(function () { report(null); }, report);
})();
