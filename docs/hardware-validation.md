# Hardware validation: the rig-day runbook

Everything in this engine that *can* be tested off-hardware *is* (the ctest suite, simulate
modes, off-wire parsers). This doc is the rest: the ordered checklist for the first day on
the real rig, and the re-check routine for every day after. Work top to bottom: each stage
assumes the one before it passed.

What only the rig can prove:

- the **ASIO full-duplex capture**, in both tools (`calib_capture.cpp` for the sweep,
  `valid_capture.cpp` for the phantom measurement). Both compile and both are unverified on
  hardware; everything they feed is unit-tested,
- **live Motive** (the NatNet parser + lifecycle are tested off-wire; real pose reception and
  the Motive-origin → room-frame agreement are not),
- the **Zylia [channel order](./glossary.md#channel-order) +
  [azimuth reference](./glossary.md#azimuth-reference)** (both survive every off-hardware check, and
  both produce a confident *wrong* direction if they are wrong),
- the **by-ear checks** (HRTF quality, the A/B/X knob bake-off, room EQ on the array).

Of these, **live Motive** (Stage 3) and the **by-ear HRTF check** (Stage 5) are the last two open
engine milestones: every other subsystem is implemented and tested off-hardware. Clearing both
here closes the engine work; the remaining unbuilt piece is the Unreal binding, a control client
rather than engine work ([integration.md](./integration.md)).

## First day, speakers anywhere

The stages below assume the install. They do not require it: nothing in the engine cares where
the speakers stand, only that the layout says where they actually are. For a bring-up day with
the boxes wherever they fit, run the same list with three adjustments:

- Stage 1 positions can be rough. Eyeballed is fine; honest is the requirement. The channel
  walk matters as much as ever.
- With the ZM-1 on Dante, real positions come from one placement: capsule survey first, then
  the `--zylia` run (both in Stage 2) writes every speaker position, wherever the boxes ended
  up. Do not move or rotate the ZM-1 between survey and sweep; a room-axes survey is pinned
  to that mounting.
- Skip trims, `--eq` and `--room`. They are properties of the final geometry and get redone
  at the install.

What such a day proves for good: Stage 0, the capture path's first contact, and Stage 3, which
needs no audio device at all. Everything from the Stage 1 walk onward is simply repeated when
the speakers land in their real places, and by then every tool has been run once.

## Before you go

- [ ] **Build**: RelWithDebInfo with `-DBWA_BUILD_PLAYGROUND=ON -DBWA_BUILD_CALIBVIEW=ON
      -DBWA_BUILD_CALIBRATE=ON` (or the CI artifact, which carries all of it). Run
      `ctest --test-dir build -C RelWithDebInfo` green *before* leaving: never debug a known
      failure through speakers. `phonon.dll` must sit beside `bw_audio.dll`.
- [ ] **Dante** configured per [build.md](./build.md): 48 kHz / 24-bit end-to-end, exactly
      **one leader clock** on the net and you know which node it is (the Digiface is hardware,
      so it can lead), ASIO buffer ~512–1024, Dante latency 4–10 ms to start. Work at 48 kHz:
      confirm the device still offers 26 out plus 19 inputs at whatever rate you pick, since
      Dante endpoints commonly halve their channel count at 96 kHz.
- [ ] **Kit**: omnidirectional measurement mic routed into the same ASIO device (it rides
      input slot `n`, the speaker count), tape measure + install drawings, headphones,
      `examples/cave_layout.json` as the starting layout. The **Zylia ZM-1** if you intend
      Stage 4b, which needs it.
- [ ] **ZM-1 mount, if you are tracking it** (strongly recommended, see Stage 4b). This is
      *physical* prep that has to happen before rig day, not something the software can
      arrange later:
      - a **rigid** stand, and a rigid coupling. **No shock mount**: elastic suspension is
        normal for microphones and exactly wrong here, because you are propagating an
        orientation through the mount.
      - markers on the **stand**, not the sphere (nothing acoustically scattering on the
        array), defined as a rigid body in Motive with its streaming ID noted.
      - **probe the offset** from the stand's body origin to the array's acoustic center.
        For a rigid sphere that center is unambiguously the geometric center, so a Motive
        probe on a few equator points plus the pole settles it. Aim for a centimeter or
        better: at Stage 4b's 1.4 m source radius, 5 cm of position error injects ~2° of
        direction error, which is the size of the effect being measured.
      - **witness-mark the collar** and do not loosen it after surveying. A quarter turn on
        the thread is 90° of azimuth error and nothing downstream will notice.
- [ ] **Motive**: streaming enabled (defaults: multicast `239.255.42.99`, data 1511,
      command 1510), a rigid body on the tracked head; note its **streaming ID** and name.
      Ground-plane calibrate with the origin at the **working-area center, on the floor**:
      that *is* the engine's room frame (right-handed, meters, +y up, +z forward), so poses
      pass through unchanged. Full seam: [integration.md](./integration.md) → "Coordinate seam".

## Stage 0: device bring-up (no engine)

```
bwa_calibrate --list-drivers        (or bwa_playground --list-drivers)
```

- [ ] The Digiface's ASIO driver is listed with ≥ your layout's speaker count of outputs (26 for
      the CAVE), plus one input for the mic when calibrating, or **19 inputs** for Stage 4b,
      which needs the whole ZM-1 on the same device. A device exposing fewer than 19 is
      refused outright rather than fed silent channels.
- [ ] Note the driver's **exact name** from this listing. RME registers under its own string,
      which is not the product name; nothing should hardcode a guess at it.
- [ ] `bwa_minimal` runs and prints `backend: asio`, not the null fallback. The null sink
      keeps everything rendering silently, so a wrong driver *looks* alive: always check
      `bwa_get_audio_backend`.
- [ ] `bwa_minimal`'s **device clock** line reads within ~±100 ppm of 48000 Hz *on the Digiface*,
      and repeat runs agree. It measures the device's true rate from two `bwa_get_clock`
      stamps across the 6 s run; what it exists to catch is GROSS clocking faults: a
      wrong nominal rate reads as *thousands* of ppm (44.1 k versus 48 k is 8%), an unlocked
      Dante domain as a rate that wanders between runs. Fix clocking before calibrating:
      every sweep delay and every scheduled sample rides this clock. Interpretation notes:
      when the driver supplies no `systemTime` the stamps are QPC-synthesized at callback
      entry, which puts tens of ppm of run-to-run scatter on a window this short (that
      scatter is measurement noise, not drift); and a consumer bridge like FlexASIO
      legitimately reads ~1000+ ppm off (its callback pacing is synthesized over WASAPI
      and bursts). Deviations at that scale on the Digiface are a real problem.

Minimal opens a 2-ch device (binaural profile), so this only proves the ASIO plumbing.
The full 26-out Digiface open is proven in Stage 1: `layout_tool` demands a real device and
fails loudly if the Digiface won't open at the layout's channel count.

## Stage 1: wiring + initial speaker positions

Goal: every channel drives the box you think it does, and the layout file holds each
speaker's real position. Positions here are the tape-measure/drawing values; Stage 2
replaces them with acoustically surveyed ones.

```
bwa_layout_tool cave_layout.json
```

- [ ] Enter each speaker's measured position in the **room frame**: meters, floor origin at
      the working-area center, the frame Motive streams in (axes as in the Motive prep above).
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
      you the positions). Cross-check the recovered positions against the drawings. (With
      the ZM-1 surveyed and on Dante, `--zylia` does this from ONE placement; see the
      Zylia section below.)
- [ ] **Latency residual sane**: the localize run prints the solved system latency next
      to the driver's own digital loop (`ASIOGetLatencies`, logged at capture open). The
      residual (DAC/ADC + analog) must be a *small positive* number: negative is
      physically impossible (wrong device / rate mismatch), tens of ms means an unexpected
      buffer (check the Dante latency setting). The solved value stays authoritative.
- [ ] **Trims**: a default run writes `delay_ms`/`gain_db`. Optionally `--eq` (per-speaker
      correction FIRs), `--save-irs prefix` (keep the kernels), `--room` (RT60 report: a
      treatment diagnostic, never numbers to copy into the reverb).
- [ ] **Review before accepting**: `bwa_calib_view before.json after.json`. The Diff tab
      highlights outliers. A swapped channel, bad mic spot, or bogus localize solve is one
      glance here. Only then overwrite the production layout.
- [ ] **Verify audibly**: re-run the Stage 1 walk with the calibrated layout; levels now
      match speaker to speaker from the center. (Delay alignment has no by-ear check with
      the built-in tools; trust the Diff numbers.)

### Zylia ZM-1 (if present)

Channel order and azimuth reference are the two things no off-hardware test can catch (why:
[calibration.md](./calibration.md) → "The capsule self-survey"). Resolve both on the rig:

- [ ] **Channel order**: `bwa_zylia_probe` (`--list` to find the driver). Tap each capsule,
      watch its channel jump. A device exposing < 19 inputs is refused for DOA.
- [ ] **Orientation**: `bwa_calib_view`, Zylia tab. Clap from a known direction; the dot on
      the capsule sphere lands where you clapped, or the offset *is* your yaw error.
- [ ] Better: run the **capsule self-survey** (Zylia tab → Capsule survey): claps from ≥ 6
      known positions, high and low (coplanar claps are refused: heights would be
      unconstrained). Solve → residual sub-µs, radius ≈ 49 mm → Install → Save. The result
      *is* the channel order and orientation.
- [ ] **If you are tracking the stand, save the survey in BODY frame.** A room-axes survey
      is pinned to the orientation it was taken at, so every remount invalidates it, and
      `bwa_validate --track` refuses one. Sample the stand's pose at survey time, rotate the
      result into the mount's frame, and save it with the probed offset. Then the survey is
      good for every placement afterwards and a remount costs nothing, which is the whole
      reason to bother with the stand. See [validation.md](validation.md).
- [ ] **One-placement position survey** (the ZM-1 alternative to `--localize`): with the survey
      installed and the ZM-1 on Dante, `bwa_calibrate --zylia --survey s.json --input <first>
      --mic x y z --ref <spk> <m>` sweeps the array once and writes every speaker position from
      this single placement: no mic moves, and it doesn't matter where the speakers are, only
      where they turn out to be. `--ref` is one tape-measured center→speaker distance (it
      calibrates the system latency; a loopback-measured `--latency <m>` also works). Without
      either, directions print and the writeback is refused. (calibrate needs a ROOM-AXES
      survey: it has no tracker to re-aim a body-frame one.)

**Do this before Stage 4b, not after.** Everything downstream reads the capsule table, so an
unsurveyed or wrongly-oriented ZM-1 silently invalidates the whole session rather than failing
loudly.

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
      - stand at the room center → position ≈ `[0, head-height, 0]`;
      - walk toward the front wall → **+z** grows; step right → **+x** grows; the y value
        *is* height above the floor in meters;
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
      ten-line client for this, or use your Unity/Unreal binding. `bwa_get_bus_levels`,
      and your own ears, must peak in exactly that speaker. Repeat for a few speakers on
      different walls. A consistent axis-swap or mirror here is a frame bug; find it before
      any by-ear tuning.
- [ ] **Walk test**: park a source at a fixed room position, walk the tracked head around
      it. The image stays **world-anchored**: it must not follow you. Panning follows your
      position smoothly (DBAP re-solves per frame from the tracked pose).
- [ ] **Motion-to-ears latency → pose prediction**: estimate the lag (move the head
      side-to-side, listen for the image trailing; 20–40 ms is typical for the
      solve+network+block+DAC chain), then `bwa_set_pose_prediction` with the
      measured lead in **seconds** (0.02–0.04).
      Start at the measured value: too much lead overshoots on direction changes.
- [ ] **`cave_both` profile**: array + headphone monitor concurrently, same build, and the
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

**Prerequisite: the capsule survey (Stage 2) is done.** Not optional. Everything here reads
that table.

- [ ] **Dry run first**: `bwa_validate --layout cave_layout.json --simulate`. No hardware,
      and it gives you the rendering-term baseline the room will then add to. Do this while
      you're still authoring the layout, not on rig day.
- [ ] **Write your placements down**: `--positions mics.txt` (one `x y z [label]` per line)
      or repeated `--position x,y,z`. The built-in envelope is a plausible guess, not your
      room. Labels come back in the report and the CSV, which matters past about four.
- [ ] **Prove the integrity layer on your own signals**: one run with `--inject-fault <ch>`.
      It corrupts that capsule in every capture and exits nonzero unless the check catches
      it, so it exercises the check, the reporting and the exclusion threading against your
      real room and noise floor rather than a model of them. Costs one extra run.
- [ ] **Capsule health**: the tool checks once per placement and reports anything faulty.
      A *hot* capsule is the one to watch: array power still looks fine while every
      spherical-harmonic channel is poisoned. Note every exclusion in the log: a direction
      from 17 capsules is fine, one you *believed* came from 19 is not.
- [ ] **Read the physical floor FIRST.** The run drives each speaker alone and reports a
      `physical floor` before any phantom number. That is instrument + survey + room, and it
      is the fastest possible check that the whole chain is sane. If a directly driven
      speaker does not land near its surveyed position, stop and fix the layout, the survey
      or the routing: nothing measured afterwards is interpretable.
- [ ] **Sweet spot**: `bwa_validate --driver <name> --mic-in <n>`, first placement at the
      listening point. Note the matched physical-versus-phantom penalty is ~0 here by
      symmetry; that row is a null control, not a result. The off-center placements carry it,
      and the table is deliberately per-placement because pooling those two shapes together
      produces a meaningless middle.
- [ ] **The walking envelope**: work through the placements. If the stand is tracked, add
      `--track <id> --survey <body-frame survey>` and the pose supplies both the position and
      the mount orientation; your typed placements become the plan, and the tool prints
      tracked-versus-planned with the delta so a wrong rigid body shows up immediately.
      Without tracking, *measure* each position properly: it is an input to the scoring, not
      a label, and centimeters here are degrees in the result.
- [ ] **Read the tracked-versus-fixed contrast**: this is the measurement that justifies
      tracking at all, and it's invisible from the sweet spot. Intervals that exclude zero
      are the claim.
- [ ] **Height separately from horizontal**: expect these to behave differently. Tracking
      fixes horizontal displacement and does not fix height, which is a placement and
      calibration problem instead. Don't pool them into one "off-center" number.
- [ ] **Then repeat one placement on a tone**: `--tone 1000` and, if you want the hard case,
      `--tone 250`. This is where content dependence lives, and it is the number a
      stimulus-agnostic spec cannot give you. Two mechanisms contribute and only one is the
      room, so compare against the *simulated* tone run for the same placement: the excess
      is the room's share. Expect the tone error to be **precisely wrong**: sub-degree
      repeatable and possibly tens of degrees biased, so do not read repeatability as
      accuracy. (6 kHz is refused: it is above the array's first-order reach, as the
      published study also had to drop it.)
- [ ] **Sanity-check the reference across stimuli.** A directly driven speaker should
      localize the same broadband and on a tone; it is one source with nothing to interfere
      with. If it does not, the analysis chain is at fault and the tone result above is an
      artifact rather than a room measurement.
- [ ] **Sweep SPCAP focus, and aim it at speakers**: `--focus 2,8,16,32,64` reports comb
      depth per cell beside the angular miss. Comb depth is the timbral price of a phantom:
      N speakers radiating coherent copies of one signal comb-filter at the microphone, and
      focus is the knob that sets N. **Put the targets on the speakers' own positions.** In
      simulate the curve there runs 7.5 dB down to 1.1 dB across focus 2 to 64 against a
      0.8 dB floor, monotone the whole way, because a tight enough lobe collapses onto the
      one real speaker and stops combing. Aimed between speakers it goes flat and
      non-monotone: the tightest render available there is two near-equal copies, and two
      equal copies null harder than twenty spread ones do. That is physics rather than a
      tool limit, so plan the placements around it instead of expecting a curve everywhere.
- [ ] **Read comb depth as an excess, never as an absolute.** The room, the stimulus's own
      line structure and the analysis itself all put ripple in a spectrum, and all three are
      there when one speaker is driven alone. Subtract the physical floor, exactly as you do
      for the angular miss. Broadband only: a tone gets no comb number by design, because
      one frequency cannot show a frequency-dependent effect.
- [ ] **Keep the CSV** (`--out cells.csv`), one per stimulus. It's the before/after record
      for any later layout or calibration change, and re-running is cheap once set up.

Quote no number from this without its caveats: single point per placement, 400–1200 Hz,
**and the stimulus it came from**: broadband and tone figures are the two ends of a wide
range, not interchangeable. A microphone is also a more pessimistic observer than a
listener, who gets two ears, head movement and the precedence effect.

## Stage 5: by-ear checks

The checks with no assertion: bring ears you trust.

- [ ] **HRTF monitor quality** (the standing "remaining" item): `bwa_playground`,
      localization scene, headphones. Timbre, externalization, front/back: laterality is
      already pinned by tests; this is everything tests can't hear.
- [ ] **The knob bake-off**: playground's blind A/B/X harness over the live knobs
      (dual-band, DBAP vs SPCAP/VBAP, SPCAP focus, spread render modes, decorrelation, air
      absorption, max-rE…). N trials, one-sided binomial p-value: a knob that isn't
      distinguishable on the rig is a knob to retire. (A fixed-seat install and a roaming one
      need not pick the same winners; judge per install type, not once for all.)
      **The SPCAP focus trial is the one the instruments cannot settle**, and that is why it
      is here. The three measurements point different ways over the same move. Going from
      this array's derived 12.7 up to 40, the layout tool's score (`--score <layout>
      focus=<n>`) cuts the Frank spread from 35.7° to 19.5°, a visibly tighter image, while
      rE error gets worse over exactly that move, 6.9° to 8.4° mean. `bwa_validate --focus`
      measures the third axis, comb depth, which favors the tight end too. Sharper image,
      worse direction, cleaner timbre: the numbers bracket the answer and your ears pick
      inside it. Start the trial at the derived default and bracket it both ways.
      **Strong prior for the max-rE trial**: the layout tool's bed metric
      (`--score <layout> [epad] maxre`, 2026-08-04) has max-rE winning every axis on this
      array under both decoders and both observer models, including AT the sweet spot,
      where classical theory says plain decode should win. An irregular 26-array's decode
      sidelobes bend rE even at center; the taper suppresses them. If the rig agrees,
      `bwa_set_max_re` now defaults to ON on the strength of that metric, so this trial
      CONFIRMS rather than gates: if the rig disagrees, revert the default. EPAD is
      worst without the taper and best with it: bake off decoder and taper as PAIRS.
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
