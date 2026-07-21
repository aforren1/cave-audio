# Hardware validation: the rig-day runbook

Everything in this engine that *can* be tested off-hardware *is* (the ctest suite, simulate
modes, off-wire parsers). This doc is the rest: the ordered checklist for the first day on
the real rig, and the re-check routine for every day after. Work top to bottom: each stage
assumes the one before it passed.

What only the rig can prove:

- the **ASIO full-duplex capture** (`bwa_calibrate` compiles but is unverified on hardware),
- **live Motive** (the NatNet parser + lifecycle are tested off-wire; real pose reception and
  the Motive-origin → room-frame agreement are not),
- the **Zylia channel order + azimuth reference** (both survive every off-hardware check),
- the **by-ear checks** (HRTF quality, the A/B/X knob bake-off, room EQ on the array).

## Before you go

- [ ] **Build**: RelWithDebInfo with `-DBWA_BUILD_PLAYGROUND=ON -DBWA_BUILD_CALIBVIEW=ON
      -DBWA_BUILD_CALIBRATE=ON` (or the CI artifact, which carries all of it). Run
      `ctest --test-dir build -C RelWithDebInfo` green *before* leaving—never debug a known
      failure through speakers. `phonon.dll` must sit beside `bw_audio.dll`.
- [ ] **DVS/Dante** configured per [build.md](./build.md): 48 kHz / 24-bit end-to-end, a
      **hardware leader clock** on the net (DVS can't lead alone), ASIO buffer ~512–1024, and
      Dante latency 4–10 ms to start. Calibrate at 48 kHz: DVS halves channels at 96 kHz.
- [ ] **Kit**: omnidirectional measurement mic routed into the same ASIO device (it rides
      input slot `n`, the speaker count), tape measure + install drawings, headphones,
      `examples/cave_layout.json` as the starting layout. Optional: the Zylia ZM-1.
- [ ] **Motive**: streaming enabled (defaults: multicast `239.255.42.99`, data 1511,
      command 1510), a rigid body on the tracked head; note its **streaming ID** and name.
      Ground-plane calibrate with the origin at the **working-area centre, on the floor**:
      that *is* the engine's room frame (right-handed, metres, +y up, +z forward), so poses
      pass through unchanged.

## Stage 0: device bring-up (no engine)

```
bwa_calibrate --list-drivers        (or bwa_playground --list-drivers)
```

- [ ] The DVS ASIO driver is listed with ≥ your layout's speaker count of outputs (26 for
      the CAVE), plus one input for the mic when calibrating.
- [ ] `bwa_minimal` runs and prints `backend: asio`, not the null fallback. The null sink
      keeps everything rendering silently, so a wrong driver *looks* alive—always check
      `bwa_get_audio_backend`.
- [ ] `bwa_minimal`'s **device clock** line reads within ~±100 ppm of 48000 Hz *on DVS*,
      and repeat runs agree. It measures the device's true rate from two `bwa_get_clock`
      stamps across the 6 s run; what it exists to catch is GROSS clocking faults: a
      wrong nominal rate reads as *thousands* of ppm (44.1 k versus 48 k is 8%), an unlocked
      Dante domain as a rate that wanders between runs. Fix clocking before calibrating:
      every sweep delay and every scheduled sample rides this clock. Interpretation notes:
      when the driver supplies no `systemTime` the stamps are QPC-synthesized at callback
      entry, which puts tens of ppm of run-to-run scatter on a window this short (that
      scatter is measurement noise, not drift); and a consumer bridge like FlexASIO
      legitimately reads ~1000+ ppm off (its callback pacing is synthesized over WASAPI
      and bursts). Deviations at that scale on DVS are a real problem.

Minimal opens a 2-ch device (binaural profile), so this only proves the ASIO plumbing.
The full 26-out DVS open is proven in Stage 1: `layout_tool` demands a real device and
fails loudly if DVS won't open at the layout's channel count.

## Stage 1: wiring + initial speaker positions

Goal: every channel drives the box you think it does, and the layout file holds each
speaker's real position. Positions here are the tape-measure/drawing values; Stage 2
replaces them with acoustically surveyed ones.

```
bwa_layout_tool cave_layout.json
```

- [ ] Enter each speaker's measured position (room frame: metres, +y up, +z forward,
      origin on the floor at the working-area centre; the same frame Motive streams in).
- [ ] Walk the test signal across every channel (the tool drives `bwa_set_test_signal`, a
      raw post-align tone on one output; no panner involved). For each channel, confirm by
      ear it's the intended box; fix the channel map in the tool, not by re-patching Dante.
- [ ] No dead channels: every speaker sounds, every gizmo lights (`bwa_get_bus_levels`
      feeds the meters).
- [ ] Save. This layout is the input to calibration.

The playground has the same walk as its channel-walk scene (TAB), but on the binaural
monitor; use `layout_tool` for the physical array.

## Stage 2: acoustic survey + trims (`bwa_calibrate`)

The capture path (full-duplex ASIO sweep→record) is the one calibration piece with no
off-hardware test. First contact:

- [ ] One sweep works: run `bwa_calibrate --layout cave_layout.json --mic x y z --input <ch>`
      and watch the first speaker's capture. A garbage IR (no clear peak, an absurd delay)
      means a routing or clocking problem; fix that before sweeping the whole array.

Then the real sequence:

- [ ] **Positions**: `bwa_calibrate --localize positions.txt`. Capture at ≥ 5 known,
      non-coplanar mic positions (put an OptiTrack marker on the mic and let Motive hand
      you the positions). Cross-check the recovered positions against the drawings.
- [ ] **Latency residual sane**: the localize run prints the solved system latency next
      to the driver's own digital loop (`ASIOGetLatencies`, logged at capture open). The
      residual (DAC/ADC + analog) must be a *small positive* number: negative is
      physically impossible (wrong device / rate mismatch), tens of ms means an unexpected
      buffer (check the DVS latency setting). The solved value stays authoritative.
- [ ] **Trims**: a default run writes `delay_ms`/`gain_db`. Optionally `--eq` (per-speaker
      correction FIRs), `--save-irs prefix` (keep the kernels), `--room` (RT60 report: a
      treatment diagnostic, never numbers to copy into the reverb).
- [ ] **Review before accepting**: `bwa_calib_view before.json after.json`. The Diff tab
      highlights outliers. A swapped channel, bad mic spot, or bogus localize solve is one
      glance here. Only then overwrite the production layout.
- [ ] **Verify audibly**: re-run the Stage 1 walk with the calibrated layout; levels now
      match speaker to speaker from the centre. (Delay alignment has no by-ear check with
      the built-in tools; trust the Diff numbers.)

### Zylia ZM-1 (if present)

Two things no off-hardware test can catch; resolve both on the rig:

- [ ] **Channel order**: `bwa_zylia_probe` (`--list` to find the driver). Tap each capsule,
      watch its channel jump. A device exposing < 19 inputs is refused for DOA.
- [ ] **Orientation**: `bwa_calib_view`, Zylia tab. Clap from a known direction; the dot on
      the capsule sphere lands where you clapped, or the offset *is* your yaw error.
- [ ] Better: run the **capsule self-survey** (Zylia tab → Capsule survey): claps from ≥ 6
      known positions, high and low (coplanar claps are refused: heights would be
      unconstrained). Solve → residual sub-µs, radius ≈ 49 mm → Install → Save. The result
      *is* the channel order and orientation; re-survey if the unit or mount changes.

`--zylia` sweeps need the ZM-1 on Dante (one ASIO device for outs + capsules); see
[calibration.md](./calibration.md), "Getting the ZM-1 onto Dante".

## Stage 3: Motive / tracking

The parser and socket lifecycle are tested off-wire; this stage is real pose reception and
the frame agreement.

```
bwa_track_monitor [server-ip] [rigid-body-name-or-id]
```

No audio device needed: it runs tracking-only on the null sink. Omit `server-ip` for a
multicast-only stream, but note: rigid-body **names** need the server (the MODELDEF
exchange, NatNet ≥ 4); on multicast-only, select by numeric streaming ID or take the first
body in the frame.

- [ ] **Data flows**: the pose line updates when the rigid body moves. A readout stuck at
      the identity pose means no frames are arriving; check multicast routing/interface
      (`bwa_tracker_desc.local_iface` pins the NIC), firewall, and that Motive is actually
      streaming.
- [ ] **Right body**: hide/show the head markers; the readout freezes/resumes with *your*
      body, not someone else's wand.
- [ ] **Frame agreement** (this is the room-space calibration check):
      - stand at the room centre → position ≈ `[0, head-height, 0]`;
      - walk toward the front wall → **+z** grows; step right → **+x** grows; the y value
        *is* height above the floor in metres;
      - face front → quaternion ≈ identity; turn left/right and confirm the sign.
      If any axis disagrees, fix it in **Motive's calibration** (ground plane / axis
      convention), not with a transform in the client; the engine takes poses unchanged.
- [ ] **Lifecycle**: disconnect/reconnect with new settings mid-run (glitch-free by
      contract); a failed connect leaves the engine on the committed pose.

## Stage 4: end-to-end spatial verification (the array)

Now sound + geometry + tracking together, `cave` (and `both`) profile, calibrated layout,
tracker connected.

- [ ] **Coordinate seam / known-position source**: place a source *at a surveyed speaker's
      position*; `examples/minimal.c` with `profile = cave` and `layout_path` set is the
      ten-line client for this, or use your Unity/Unreal binding. `bwa_get_bus_levels`—and
      your own ears—must peak in exactly that speaker. Repeat for a few speakers on
      different walls. A consistent axis-swap or mirror here is a frame bug; find it before
      any by-ear tuning.
- [ ] **Walk test**: park a source at a fixed room position, walk the tracked head around
      it. The image stays **world-anchored**—it must not follow you. Panning follows your
      position smoothly (DBAP re-solves per frame from the tracked pose).
- [ ] **Motion-to-ears latency → pose prediction**: estimate the lag (move the head
      side-to-side, listen for the image trailing; 20–40 ms is typical for the
      solve+network+block+DAC chain), then `bwa_set_pose_prediction(e, measured_ms)`.
      Start at the measured value: too much lead overshoots on direction changes.
- [ ] **`both` profile**: array + headphone monitor concurrently, same build, and the
      monitor's image agrees with the array's.
- [ ] **Soak**: leave a busy scene running (looping sources, moving listener) for 30+
      minutes. On any dropout, widen the ASIO buffer / Dante latency and soak again; keep
      a tighter setting only after it survives a full clean soak.
- [ ] **Limiter**: drive it (many loud sources) and confirm graceful, image-stable gain
      reduction: it's linked across channels, so the image must not shift when it engages.

## Stage 4b: instrumental phantom accuracy (`bwa_validate`)

Stage 4 confirms the array puts sound in the *right general place* and that the frame isn't
mirrored. This puts a **number** on it: render a phantom in a known direction, measure where
it actually landed, report the angular miss. Full doc: [validation.md](validation.md).

Needs the ZM-1 on the same ASIO device as the 26 outputs (Dante Via, same unlock as the
sweep path). Cheap in rig time: only the microphone moves, and one placement sweeps every
direction electronically.

- [ ] **Dry run first**: `bwa_validate --layout cave_layout.json --simulate`. No hardware,
      and it gives you the rendering-term baseline the room will then add to. Do this while
      you're still authoring the layout, not on rig day.
- [ ] **Capsule health**: the tool checks once per placement and reports anything faulty.
      A *hot* capsule is the one to watch: array power still looks fine while every
      spherical-harmonic channel is poisoned. Note every exclusion in the log—a direction
      from 17 capsules is fine, one you *believed* came from 19 is not.
- [ ] **Sweet spot**: `bwa_validate --driver <name> --mic-in <n>`, first placement at the
      listening point. Broadband miss here is the floor for everything else. If it's large,
      stop and re-check the layout and trims before collecting more cells.
- [ ] **The walking envelope**: work through the placements the tool prompts for. *Measure*
      each mic position; it's an input to the scoring, not a label.
- [ ] **Read the tracked-versus-fixed contrast**: this is the measurement that justifies
      tracking at all, and it's invisible from the sweet spot. Intervals that exclude zero
      are the claim.
- [ ] **Height separately from horizontal**: expect these to behave differently. Tracking
      fixes horizontal displacement and does not fix height, which is a placement and
      calibration problem instead. Don't pool them into one "off-centre" number.
- [ ] **Keep the CSV** (`--out cells.csv`). It's the before/after record for any later
      layout or calibration change, and re-running is cheap once the rig is set up.

Quote no number from this without its caveats: single point per placement, 400–1200 Hz,
broadband stimulus, and a microphone is a more pessimistic observer than a listener.

## Stage 5: by-ear checks

The checks with no assertion—bring ears you trust.

- [ ] **HRTF monitor quality** (the standing "remaining" item): `bwa_playground`,
      localization scene, headphones. Timbre, externalization, front/back: laterality is
      already pinned by tests; this is everything tests can't hear.
- [ ] **The knob bake-off**: playground's blind A/B/X harness over the live knobs
      (dual-band, DBAP vs SPCAP/VBAP, spread render modes, decorrelation, air absorption,
      max-rE…). N trials, one-sided binomial p-value: a knob that isn't distinguishable on
      the rig is a knob to retire. (A fixed-seat install and a roaming one need not pick
      the same winners; judge per install type, not once for all.)
- [ ] **Tracked room EQ** (if the install wants it): one `bwa_calibrate --room-eq-grid
      --mic x y z` run per mic placement, ~0.5–1 m spacing over the working area at ear
      height. Then walk the room A/B-ing `bwa_set_tracked_room_eq`; LF evenness should
      improve position-to-position with the switch on. (Static `--room-eq` is for fixed-seat
      installs only, and `bwa_start`/`bwa_tracker_connect` refuse the mismatch.)

## Every visit after

- [ ] `bwa_calibrate --check`: one fast pass from a fixed mic spot; flags any speaker whose
      distance drifted > ~20 mm (exit code 3, scriptable; cron it into the show-day
      preflight). Radial-only: it can't see a purely tangential move, so re-run `--localize`
      after any physical work on the array.
- [ ] `bwa_track_monitor`, thirty seconds: pose flows, axes still agree (a Motive
      re-calibration silently moves the room frame).
- [ ] The Stage 1 channel walk: a re-patched Dante route is the classic silent regression.
