# Hardware validation: the rig-day runbook

This is the procedure for the first day on the real rig, and the re-check for every day after.
Work top to bottom. Each stage assumes the one before it passed. The why behind each step is in
[calibration.md](./calibration.md) and [validation.md](./validation.md); this page says what to
do, what passes, and what to do when it does not.

The rig:

- an RME Digiface Dante, over ASIO;
- Genelec 4410A speakers on Dante: 24 now, maybe 36 later;
- a Zylia ZM-1 on Dante Via, the only measurement mic;
- OptiTrack and Motive. The cameras see about 4 speakers. The rest are behind the screens or
  overhead;
- the listening point at 4.75 ft (1.448 m);
- projectors that warm the room by about 3 F over a long run.

Everything that can be tested off the rig is tested (ctest, simulate modes, off-wire parsers).
What only the rig can prove:

- the ASIO full-duplex capture in `bwa_calibrate`, `bwa_calib_view` and `bwa_validate`;
- live Motive: pose reception, the Motive frame against the room frame, the tracked ZM-1 stand,
  the tracked clicker;
- the ZM-1's channel order, its orientation, and its rigid sphere;
- the Session tab, which has run only in simulate;
- the by-ear checks: HRTF quality, the A/B/X knob bake-off, room EQ on the array.

Live Motive (Stage 3) and the by-ear HRTF check (Stage 5) are the last two open engine
milestones. The Unreal binding is the one unbuilt piece, and it is a control client
([integration.md](./integration.md)).

**Every threshold in Stage 2 is provisional.** They were set in simulation, which has no noise
floor and no Motive jitter. Read [Record these numbers](#record-these-numbers) before you start,
and fill it in as you go.

## Before rig day

### Build and rehearse

- [ ] **Build** RelWithDebInfo with `-DBWA_BUILD_PLAYGROUND=ON -DBWA_BUILD_CALIBVIEW=ON
      -DBWA_BUILD_CALIBRATE=ON`, or take the CI artifact. Run
      `ctest --test-dir build -C RelWithDebInfo` and get it green before you leave. Never debug a
      known failure through speakers. `bw_audio.dll` is the only library to carry: Steam Audio is
      linked into it.
- [ ] **Rehearse the whole session in simulate.** `bwa_calib_view`, Session tab: type a folder,
      **New**, tick **simulate**, fill the inputs (see [Stage 2](#stage-2-calibration-the-session)).
      Set **simulated truth** to a copy of the plan with a few speakers moved several centimeters.
      Run every step. Pass: `as_built.json` lands on the truth, not on the plan.
- [ ] **Rehearse the tracker**: in any `bwa_calibrate` command below, replace the device and
      tracker flags with `--simulate --track-sim`. Add `--track-sim-bump N` or
      `--track-sim-twist N` to see a bump or a turn stop the run with exit 4. `bwa_speaker_survey <plan> --simulate` rehearses the frame check.
      [calibration.md: Rehearse without Motive](./calibration.md#rehearse-without-motive---track-sim).

### Make the plan

The plan is the layout file you design before the boxes go up. Keep it. The Session tab never
writes it, and every measuring writer (`--localize`, the `--zylia` position survey,
`bwa_speaker_survey --write`) copies a speaker's plan into `plan_position` and `plan_aim` the first
time it writes that speaker's position or aim. Live aiming aims at the plan.
[layout-schema.md: Plan versus as-built](./layout-schema.md#plan-versus-as-built).

**Every position is an ACOUSTIC CENTER**, in the plan and in the as-built alike. Place each plan
point where the box's acoustic center will be, not on its front baffle or its mounting point.
`--localize` measures acoustic centers, and the optical check moves its baffle markers back to one
(step 1).

- [ ] **Place the speakers** in `bwa_layout_tool plan.json`, with a `constraints.json` beside it
      for the room's bounds, keep-out boxes and obstacles
      ([layout-schema.md: Constraints](./layout-schema.md#constraints-constraintsjson)).
- [ ] **Optimize** if you want to: `bwa_layout_tool --optimize plan.json dbap` (or `spcap`,
      `vbap`). It hill-climbs inside the constraints and saves in place. Keep the default
      `moving` observer: one array serves every use, so do not optimize it for one seat
      ([layout-schema.md: The optimizer](./layout-schema.md#the-optimizer)).
- [ ] **Declare the listening point**: `bwa_layout_tool --export plan.json ears=1.448 listen`.
- [ ] **Add the speaker model**: `uv run tools/directivity/clf_to_json.py Genelec_Oy-4410A.CF2
      --into plan.json`. Live aiming, the aiming sheet and the trims read it.
- [ ] **Print the aiming sheet**: `bwa_calibrate --layout plan.json --aim-sheet aim.csv`. It
      opens no audio device. Check its summary: the listening point is `declared
      (listening_point_m)` at height 1.448 m (4.75 ft), and the directivity model is present.
      Exit 3 means a layout `aim` is more than 20 degrees off the listening point: fix the plan
      before anyone drills. Mount each box to its row's `bearing_deg` (clockwise from above, 0 =
      room-ahead +z, 90 = room-right -x) and `down_tilt_deg` (positive is down; read it with an
      inclinometer on a face parallel to the acoustic axis). Use a protractor from the room axes,
      not a compass. [calibration.md: The aiming sheet](./calibration.md#the-aiming-sheet---aim-sheet).

### Set up the network and the speakers

- [ ] **Dante** per [build.md: Dante configuration](./build.md#dante-configuration): 48 kHz,
      24-bit end to end, exactly one leader clock and you know which node it is, ASIO buffer 512
      to 1024, Dante latency 4 to 10 ms to start.
- [ ] **Every 4410A set the same** in Smart IP Manager: tone controls and any room-response or
      delay setting off, one fixed volume, one Dante latency, 48 kHz, one firmware, subscriptions
      matching the layout's `index`. The list and the reasons:
      [build.md: The speakers are Dante devices too](./build.md#the-speakers-are-dante-devices-too-genelec-4410a).
- [ ] **The ZM-1 on Dante Via.** Via presents 20 channels; the first 19 are the capsules, in
      order, and the 20th is unused. Patch them to the Digiface's first 20 inputs, so the first
      capsule is ASIO input 0 (`--input 0`, `--mic-in 0`). `bwa_zylia_probe` reads inputs 0 to 18
      only. [calibration.md: Getting the ZM-1 onto Dante](./calibration.md#getting-the-zm-1-onto-dante).

### Prepare the ZM-1 stand

The stand is tracked, and every placement in Stage 2 measures the ZM-1's center from its pose.
This is physical work. The software cannot fix it on the day.

- [ ] **A rigid stand and a rigid coupling.** No shock mount: you propagate an orientation through
      the mount.
- [ ] **Markers in a ring around the ZM-1's equator, unevenly spaced** (for example 0, 90, 180
      and 225 degrees). The uneven spacing gives the body a stable yaw. `--mount-offset ring` fits
      the ring's center and takes it as the array center, so the ring must sit at the height of
      the capsule sphere's equator
      ([calibration.md: The ring offset](./calibration.md#the-ring-offset---mount-offset-ring)).
- [ ] **Witness-mark the collar** and never loosen it after the capsule survey. A ZM-1 that turns
      in its collar turns the capsules and not the markers, and nothing downstream can see it.

### Prepare Motive

- [ ] **Streaming on, multicast** (the default group `239.255.42.99`, data port 1511, command port
      1510). Do not use unicast: unicast reaches one socket, and the stand, the clicker, a Session
      step and the Placement panel each listen on their own socket.
- [ ] **Ground plane**: the origin at the working-area center, on the floor. That is the engine's
      room frame: right-handed, meters, +y up, +z room-ahead, so room-right is -x. The engine takes
      poses unchanged ([integration.md: Coordinate seam](./integration.md#coordinate-seam-the-part-that-silently-ruins-spatial-audio)).
- [ ] **Rigid bodies.** One for the ZM-1 stand (note its name and streaming ID). One for each
      visible speaker, named `spk<N>` where N is the speaker's `index`, with 4 markers stuck flat on
      the baffle around the tweeter, 5 cm or more apart and not in a line. One for the tracked
      head. Optionally one for the clicker
      ([calibration.md: Optical speaker check](./calibration.md#optical-speaker-check-bwa_speaker_survey)).

### Kit

- [ ] The ZM-1 and its tracked stand, a tape measure, the install drawings, an inclinometer,
      headphones, a thermometer, and a sheet for [Record these numbers](#record-these-numbers).

## Stage 0: device bring-up

No engine yet. This proves the device and its clock.

```
bwa_calibrate --list-drivers        (or bwa_playground --list-devices for every backend)
```

- [ ] The Digiface's ASIO driver is listed with at least the layout's speaker count of outputs
      (24 now, 36 later) and the ZM-1's 20 inputs. A device with fewer than 19 inputs is refused
      for every ZM-1 mode.
- [ ] Copy the driver's **exact name** from the listing. RME registers its own string, which is
      not the product name.
- [ ] `bwa_minimal --device "<driver name>"` prints `backend: asio:<driver name>`. Pass the name:
      with no `--device` the binaural profile opens the default WASAPI output, which makes real
      sound and proves nothing about the Digiface.
- [ ] `bwa_minimal`'s `device clock:` line reads within about ±100 ppm of 48000 Hz on the
      Digiface, and repeat runs agree.
      - Thousands of ppm: a wrong nominal rate (44.1 kHz against 48 kHz is 8%). Fix it in Dante
        Controller.
      - A rate that wanders between runs: an unlocked Dante domain. Fix the leader clock.
      - Tens of ppm of run-to-run scatter is measurement noise when the driver supplies no
        `systemTime` on QPC.

      Fix clocking before you calibrate: every sweep rides this clock.
- [ ] `bwa_zylia_probe --driver "<driver name>"`: 19 channels live. Tap each capsule and watch
      its channel jump. A channel at digital silence is dead or unpatched; a channel far louder
      than the rest is hot. Fix the routing before Stage 2.

`bwa_minimal` opens 2 channels. Stage 1 proves the full-width open.

## Stage 1: the wiring (the channel walk)

Goal: every output drives the box the plan says it does.

```
bwa_layout_tool plan.json
```

- [ ] The panel reads `audio: asio:<driver name>`. `audio: none - editor only` means the array's
      ASIO device did not open at the layout's channel count: the console says why.
- [ ] Walk the test signal across every channel and confirm by ear that each one is the intended
      box. The tone is post-align on purpose: it ignores any trims, so it proves the wiring and
      nothing about levels.
- [ ] No dead channels: every speaker sounds and every meter lights.
- [ ] A wrong box: fix the channel map in the tool, not the Dante patch. Save. This plan file is
      the input to Stage 2.

## Stage 2: calibration (the session)

Run Stage 2 in `bwa_calib_view`'s **Session tab**. It runs each tool in order in one folder,
hands each step the file the step before wrote, records every result, keeps one log
(`session.log`) and reopens where you left off. The commands under each step are what the step
runs. Run them by hand only as a fallback, with the same file names.
[calibration.md: The rig-day session](./calibration.md#the-rig-day-session-bwa_calib_view-session-tab).

| # | Step | Reads | Writes |
| --- | --- | --- | --- |
| 1 | Motive frame check | the plan | `frame_check.csv` |
| 2 | Speaker positions | the plan, the localize rows | `as_built.json` |
| 3 | Capsule survey | `as_built.json` | `capsules.json` |
| 4 | Live aiming | `as_built.json`, `capsules.json` | the readings you accept |
| 5 | Trims and room report | `as_built.json`, `capsules.json` | `trims.json` |
| 6 | Verify | `trims.json` | an exit code |
| 7 | Room-EQ grid | `trims.json`, the grid rows | `grid.json` |
| 8 | Validate | `grid.json`, `capsules.json` | `validate.csv` |

Exit codes, for every tool: 0 passed, 1 an error, 2 refused before measuring (for the frame
check: no Motive data), 3 the tool's own check failed (flagged speakers, a disagreeing frame),
4 a bump, 5 the track self-check, 6 a speaker that does not fit the rest.

### Set up the session

- [ ] **Projectors on, room warm.** Run them as long as a session would, read the air
      temperature, and leave them on for every step. Their fans are the noise floor the sweeps
      must clear; the 3 F of warming is under the survey's other errors
      ([calibration.md: Air temperature](./calibration.md#air-temperature)).
- [ ] **Session tab**: a new folder for the day, **New**, untick **simulate**.
- [ ] **Inputs:**
      - **plan layout**: the plan from Stage 1 (read, never written);
      - **localize rows**: `x y z` per line, 5 or more, off one plane, spread over the working
        area;
      - **grid rows**: `x y z` per line, up to 16, 250 mm apart or more, at ear height;
      - **validate placements**: `x y z [label]` per line (empty is the tool's default envelope);
      - the ASIO driver, the first ZM-1 input (0), the temperature (`22.5C` or `72F`);
      - the stand's rigid body, Motive's IP, the multicast group, the mount offset (`ring`);
      - **baffle offset**: the depth from the speakers' baffles back to their acoustic centers,
        in meters. A guess (or 0) until step 1's second pass measures it;
      - **capsule speakers**: 6 or more, high and low (empty is all);
      - **aim speakers**: the boxes the cameras cannot see.
- [ ] **Run** each step in order and read its result before the next. A failed step says why in
      its row and in the log, and the steps after it stay blocked until it passes. A step that does
      not apply: type a note, then **Skip**.
- [ ] Tracked steps wait for the placement gate by themselves. Untracked, a step stops at each
      row; place the ZM-1, then press **Send Enter**.
- [ ] **Read the background line** the log prints after each step:
      `=== background: <step> <level> dBFS, <change> dB against <earlier step>`. `RISING` marks a
      rise of more than 6 dB. Find what got louder (a projector fan, an air handler, a door) and
      run that step again.
- [ ] **A box moved?** Run localize again. Every step after it reads `STALE`, by name, and blocks
      the next until you run it again.

### Place the ZM-1 (every tracked step)

Every step that sweeps places the ZM-1 the same way
([calibration.md: Placing the ZM-1 with the tracker](./calibration.md#placing-the-zm-1-with-the-tracker---track)).
The live line shows the measured center, the target, `dx dy dz` and `|d|` in mm, and `HOLD`
with the reason, or `OK`.

- **The gate** opens when the center is still (every center of the last 0.5 s within 2 mm of
  their mean), within tolerance on that mean, and both have held for 1 s. The tolerance is 10 mm
  for the trims, verify, live aiming and the capsule survey; 100 mm for the localize and grid
  rows, which record the measured position; 20 mm for `bwa_validate`.
- **The bump check** runs after every capture. A move past half the tolerance (5 mm; 10 mm for
  `bwa_validate`) stops the run with exit 4 and writes nothing.
- **The turn.** The direction modes (the capsule survey, the live-aim position readout, the
  `--zylia` position survey, `bwa_validate`) also hold the gate while the stand turns
  (`HOLD turning`, every pose of the window within 0.3 degrees of their mean) and stop with exit 4
  on a turn past their limit: 0.3 degrees, the floor, for the `bwa_calibrate` modes, and 0.41
  degrees for `bwa_validate`. The center-only modes (localize, trims, verify, grid, the tilt meter)
  do not stop on a turn.
- **The turn readout.** Each placement prints `mount yaw` and `tilt`; the Placement panel shows
  the mount yaw live. Every tracked run but localize ends with a `placement:` report line: the
  largest move and the largest turn over the run, and how many captures had no live pose to check.
- **A NatNet dropout.** A pose more than 0.25 s old reads `no live pose`, and the gate empties.
  When poses return, the stand reads `moving` for up to 0.5 s while the window refills, then the
  1 s hold starts again. During a run, a capture with no fresh pose within 0.5 s is counted as
  unchecked in the report line.
- **A key** takes the current reading anyway, with a warning (CLI only; a Session step has no
  console). `--place-timeout` (300 s) ends a wait that never opens: `bwa_calibrate` exits 1, and
  `bwa_validate` skips that placement.

On failure:

- Never opens, `moving`: something touches the stand, or the floor flexes. Untouched and still
  `moving`: write down the Placement panel's `spread`, and see
  [Record these numbers](#record-these-numbers).
- Never opens, `turning` on an untouched stand: Motive's orientation jitter is above the 0.3
  degree floor. Record it; do not loosen the check by hand on the day.
- `no live pose`: occluded markers, a wrong body name or ID, or Motive not streaming.
- Exit 4: re-place the ZM-1 and run the step again.

### Step 1: Motive frame check

Every tracked center in Stage 2 is a Motive coordinate, so Motive's frame must be the room frame
before anything is measured. No audio needed.

Layout positions are acoustic centers, in the plan and in the as-built alike. The speaker bodies'
markers sit on the baffle, in front of the acoustic center, so the tool moves each optical point
back along the box's aim by `--baffle-offset-m`. That depth is a measurement too, so this step runs
three times:

1. **Now, before step 2.** Pass your best guess for the depth, or 0. In the session it is the
   **baffle offset** input.

   ```
   bwa_speaker_survey plan.json --server <motive-ip> --require-frame --baffle-offset-m <guess> --csv frame_check.csv
   ```

2. **After step 2**, on the measured positions, to read the depth:

   ```
   bwa_speaker_survey as_built.json --server <motive-ip> --baffle-offset-m <guess>
   ```

   The `baffle depth` block compares each visible speaker with its measured acoustic position.
   It skips a speaker step 2 did not measure (no `plan_position`, or one equal to `position`), and
   it refits the frame with the depth as an unknown, so a wrong guess cannot fail it. It ends with
   `suggested --baffle-offset-m <x>`. With a guess, the per-speaker depths are the residual beyond
   it.

3. **Set the measured depth** as `--baffle-offset-m` and as the session's **baffle offset**, then
   run pass 1 again. The session marks the frame step stale when the value changes, and will not
   run localize again until the frame step passes with the new value.

- [ ] Pass: exit 0, summary ends `frame check PASS`: handedness `OK`, rotation under 0.5 degrees,
      translation under 10 mm, from 3 or more speakers, 4 off one plane to rule out a mirror by
      position.
- [ ] Each visible speaker's aim in the "after the fit" lines is within 3 degrees of the plan.
      Over that: aim the box again.
- [ ] Pass 2: a depth from 3 or more speakers, no `DISAGREES` line, and the per-speaker depths
      within a few mm of each other. They are the same box model, so they share one depth. Record
      the median and the spread.

On failure:

- `MIRRORED`: fix the axis convention in Motive, then run again.
- `off the room frame`: set the ground plane again in Motive. On pass 1, a translation alone,
  with the visible speakers all on one side, can be the guessed depth: do pass 2 and come back.
- `UNDETERMINED` or no fit: add a visible speaker off the others' plane.
- Exit 2: no frames from Motive; check streaming, the IP, the multicast group, the firewall.
- One speaker far off in "after the fit" while the rest agree: that box is off its plan, not the
  frame.
- Pass 2 says `not measured`: it names the reason. Fewer than 3 measured speakers with an aim,
  boxes that all face one way (the depth is then a translation), or a frame still off with the
  depth solved, which is a Motive problem: fix it first.
- Pass 2 says `DISAGREES`: that speaker's optical and acoustic surveys disagree by more than
  20 mm, along or across its aim. Check its body (markers flat on the baffle), its aim and its
  measured position.

**Optional: optical positions for `--ref-speakers`.** The same run can write the visible
speakers' measured positions into a copy:

```
bwa_speaker_survey plan.json --server <motive-ip> --require-frame --baffle-offset-m <x> --write optical.json --fields position
```

Use `optical.json` for a `--ref-speakers` latency (below). It refuses to write a mirrored frame.
The localize run later replaces these positions with acoustic ones.

### The latency

Some modes turn an arrival into a distance and need the system latency (the whole loop: Digiface,
Dante, Dante Via, the ZM-1). The Session tab hands the localize run's latency to the capsule
survey and to live aiming, so in a session you do nothing here. By hand, take it from one of:

| Source | How | Use it for |
| --- | --- | --- |
| localize (step 2) | it solves the latency: the `solved system latency <m> m` line, the median of the per-speaker `[system latency <m> m]` | everything after it; the best one |
| the capsule survey (step 3) | its `--latency <m> for live aiming` line | live aiming, the `--zylia` survey |
| `--ref-speakers <list>` | 3 or more speakers with MEASURED positions (`optical.json`, or a surveyed layout) | live aiming and the `--zylia` survey, before or without localize |
| `--ref <spk> <m>` | one tape-measured distance from the array center to a speaker | the same, when nothing is measured |
| `--latency <m>` | a value in meters at c, from any of the above or a loopback | every mode that takes it |

`--ref-speakers` prints each speaker's latency against the median. A speaker more than 0.1 ms off
the median is `FLAGGED`: its Dante latency setting differs from the others', or its position is
wrong along the line to the array. The median stands without it. When the unflagged speakers are
not a majority, nothing is used and the run exits 6. Check the flagged boxes' Dante latency in
Dante Controller first.
[calibration.md: The latency from measured speakers](./calibration.md#the-latency-from-measured-speakers---ref-speakers).

### Step 2: speaker positions

The ZM-1's center arrival does not change when the array turns, so this step needs no capsule
survey, only the mount offset.

```
bwa_calibrate --layout plan.json --out as_built.json --zylia --localize localize_rows.txt \
              --driver "<driver name>" --input 0 --temp <T> \
              --track <stand body> --natnet-server <motive-ip> --mount-offset ring
```

(Every `bwa_calibrate` command below takes the same `--driver`, `--input`, `--temp` and
`--track ... --natnet-server ...` flags. They are left out from here on.)

- [ ] **Turn the stand to a different yaw at each row**, by tens of degrees. Read `mount yaw` on
      each placement line. The capsule survey checks the mount offset against these positions,
      and a stand that never turned hides an offset error from that check.
- [ ] Each row waits until the stand is within 100 mm of the row and still, then records the
      MEASURED position. The run lists the measured positions it solved with.
- [ ] The writeback prints `localize: recorded the plan for 24 of 24 speaker(s)` (your count):
      `as_built.json` now carries the plan beside the measured positions.
- [ ] Pass: exit 0, no `trilateration failed` line, every position within a few centimeters of
      its plan or you know why, and a sane latency residual (below).
- [ ] **Latency residual**: the `-> residual <ms>` at the end of the `solved system latency`
      line must be positive. Negative is a wrong device or a sample-rate mismatch. With the ZM-1
      on Dante Via, tens of ms is a correct run: the driver does not report the Via leg (about
      60 ms for the whole chain on the rig), and the next line says so. A WARNING fires only past
      150 ms, the most the arrival window allows past the driver's loop: check the Via latency
      setting and any extra buffer. Record the value.
- [ ] Optional, by hand: add `--check-aim` to fit each box's acoustic axis from these captures.
      A box more than 15 degrees off its aim is flagged
      ([calibration.md: Modes](./calibration.md#modes)).
- [ ] **Then go back to step 1, passes 2 and 3**: read the baffle depth off `as_built.json`, set
      it, and run the frame check again.

On failure: `no clean measurement in 5 sweeps` stops the run; see [Sweep quality](#sweep-quality).
A capsule called dead: check its input with `bwa_zylia_probe`.

### Step 3: capsule survey

The ZM-1 at the listening point (`--mic` is the layout's listening point; `0 1.448 0` when it
sits at the room center), every listed speaker swept once. It writes a BODY-FRAME survey:
the channel order, the array's orientation in its mount, and an acoustic mount offset. Every
direction measurement after it reads this file.

```
bwa_calibrate --layout as_built.json --zylia --capsule-survey capsules.json --mic 0 1.448 0 \
              --latency <localize's m> [--speakers <list>] --mount-offset ring
```

- [ ] Pass `--latency` from localize. Without it the run solves the latency itself, and a dome
      around the listening point pins it badly: over a dilution of 3 it prints a WARNING and
      offers no latency.
- [ ] Pass, as printed: `residual` a few µs or less (a WARNING over 5 µs), `radius` near 49 mm (a
      WARNING outside 40 to 60 mm), the acoustic and tracked mount offsets within 5 mm of each
      other, and no speaker flagged.
- [ ] Record `residual` and `radius`: the real ZM-1's rigid sphere is not in the simulation, so a
      residual of several µs and a radius above 49 mm may be normal. Compare with a clap survey of
      the same mounting if you can (below).

On failure:

- **Exit 6**: a speaker does not fit the rest. Leave-one-out flags a direction error (held out
  over 3 µs and 5 times the rest's residual); the range check flags a distance error (over 10 mm
  from the median range residual). The message names the worst speaker and prints a `--speakers`
  list without it. Check that box's position (run localize again) and its Dante latency. Then run
  again with that list, or add `--drop-outliers`, which drops one speaker at most; a second
  flagged speaker stops the run with exit 6 again, and the layout itself is the suspect. In the
  session, put the list in **capsule speakers**.
- **Offsets more than 5 mm apart**: the mount offset or the positions are wrong. For an
  independent check, run `bwa_speaker_survey as_built.json --server <motive-ip>`: a translation
  there that the plan run did not show is the mount offset.
- Exit 2 before any sweep: fewer than 4 speakers, a speaker within 0.2 m of the center, or a set
  too flat (spread under 0.05). Add speakers above and below the array.
- Exit 4 on a turn: the stand turned more than 0.3 degrees. Re-place it and run again.

[calibration.md: The speaker-sweep capsule survey](./calibration.md#the-speaker-sweep-capsule-survey---capsule-survey).

**Alternative: the clap survey with the tracked clicker** (`bwa_calib_view`, Zylia tab, Capsule
survey). Use it when the speaker sweep cannot run, or as the cross-check above.

- [ ] Connect the stand in the section's Placement panel, so every clap is taken against the
      measured center. Pick **tracked clicker**, type its body, Motive's IP and the tip offset,
      **Connect**, and tick **record claps**.
- [ ] At each spot, hold the clicker still until the window arms, click once, and move at least
      5 cm to the next spot. 6 spots or more, high and low.
- [ ] A refused clap says why: moving (the tip wandered more than 4 mm in the 300 ms before the
      click), outside the armed window, a second transient, or, from the sixth clap on, a direction
      more than 10 degrees off. Do not move the stand: a turn of more than 0.3 degrees after the
      first clap refuses the rest.
- [ ] **Solve**. A clap that leave-one-out flags gets a **Drop clap N** button; drop the worst one
      and look again. **Install**, then **Save**: with every clap tracked it saves a body-frame
      survey.

[calibration.md: Clap positions from a tracked clicker](./calibration.md#clap-positions-from-a-tracked-clicker).

### Step 4: live aiming (the hidden boxes)

For every box the cameras cannot see, and any box the frame check or `--check-aim` flagged. Do it
before the trims: the trims are measured through the aim. The ZM-1 stays at the listening point.

In the session, **Run** on the aim step loads `as_built.json` into the Aim tab with the latency and
the capsule survey, and opens the first listed speaker.

```
bwa_calibrate --layout as_built.json --live <spk> --zylia --survey capsules.json \
              --latency <m> --aim-ref <reference spk>
```

- [ ] **Take a reference first**: a speaker the frame check showed within a degree or two of its
      plan aim, behind the same kind of screen as the boxes you aim. CLI: `--aim-ref <spk>`, or
      `r` on a reading. Aim tab: aim the reference speaker, **Store reference**, then switch
      speakers.
- [ ] **Check the header**: it must say `against its PLAN (plan_position, plan_aim)` and how far
      the last survey put the box from its plan. `the file carries no plan` means you loaded the
      wrong file: you would aim at the last survey's numbers, not at the plan.
- [ ] **Move the box** by the words on each line, for example `move 12 mm toward room-left, 30 mm
      toward the front wall` (axes under 5 mm are left out). Read a delta under about 1 cm as "on
      its plan".
- [ ] **Turn the box** while someone reads "below" aloud. The peak holds only when two clean
      readings in a row agree, so hold the box still for about two readings at the best spot. Until
      then the line says `peak --`. A reading marked `NOT HELD` says why. The top is flat for
      about 7 degrees: find the two sides where "below" reaches 0.5 dB and split the difference.
      The angle is a magnitude only; the tool cannot say which way to turn.
- [ ] Pass: the box reads `on axis (under N deg)` against the reference (under about 7 degrees on
      the 4410A), and its position is within about 1 cm of the plan. In the session, press
      **Accept for the session**, **Stop**, and press the next **Aim speaker N** on the Session
      tab. The step passes when every listed speaker has an accepted reading.
- [ ] **Run localize again after moving boxes**, so `as_built.json` follows them. The steps after
      it read `STALE` until you run them again.
- [ ] Record the reading rate: a CLI run ends with `live: N reading(s) in X s, Y per second`.

On failure: `position: n/a (no body-frame survey)` means the run has no capsule survey; the tilt
meter still works. No `--latency`, `--ref` or `--ref-speakers`: direction only, no distance.
[calibration.md: Live aiming](./calibration.md#live-aiming---live-n---zylia-the-aim-tab).

### Step 5: trims and the room report

The ZM-1's center at the listening point, at ear height.

```
bwa_calibrate --layout as_built.json --out trims.json --zylia --trims --room --survey capsules.json
```

- [ ] Each speaker takes two sweeps that agree (2 s each, so about 48 s more than one sweep on 24
      speakers). The run's `trims:` sweep line says how many were re-swept.
- [ ] Pass: exit 0, gate `OK` within 10 mm, no bump, no `aimed > 30 deg off the listening point`
      in the directivity report, and no `the mic is ... from the layout's listening point`
      WARNING.
- [ ] Read `room: mean RT60`. It is how live the room is, a floor on renderable reverb. Do not
      copy it into the engine's reverb.
- [ ] Record every speaker's `snr=` and the `background:` line.

`--eq` and `--room-eq` are refused with the ZM-1.
[calibration.md: The ZM-1 as the trim mic](./calibration.md#the-zm-1-as-the-trim-mic---zylia---trims---zylia---verify).

### Step 6: verify

Right after the trims, and do not move the ZM-1 between them: verify measures its residuals from
its own placement.

```
bwa_calibrate --layout trims.json --zylia --verify --survey capsules.json
```

- [ ] Pass: exit 0, that is no speaker beyond ±100 µs or ±1 dB. Expect far less (19 µs and 0.01 dB
      of spread in simulation).
- [ ] Record the `verify: arrival spread` line.

On failure (exit 3): `ARRIVAL` is a delay that is not what the trims wrote, a moved box, or a box
with a different Dante latency. `LEVEL` is a changed gain or a box's own volume setting. `DEAD` is
no signal. Fix the cause, then run the trims again; never hand-edit the file.
[calibration.md: Verify](./calibration.md#verify-the-second-pass---verify).

### Step 7: room-EQ grid

```
bwa_calibrate --layout trims.json --out grid.json --zylia --room-eq-grid grid_rows.txt --survey capsules.json
```

- [ ] Each row waits within 100 mm of its position, keys the grid entry at the MEASURED center,
      and takes one clean sweep per speaker. The grid is written once, after the last row, and
      nothing else changes.
- [ ] Pass: exit 0 and `merged N position(s) into room_eq_grid`. A bump or a timeout writes
      nothing.

Refused before any sweep (exit 2): more than 16 rows in all, or rows closer than 250 mm.
[calibration.md: Modes](./calibration.md#modes) (`--room-eq-grid`).

### Step 8: validate

See [Stage 4b](#stage-4b-instrumental-phantom-accuracy-bwa_validate). The session runs
`bwa_validate` with `grid.json` (or `trims.json` when the grid was skipped), `capsules.json` and
the validate inputs.

### Review and accept

- [ ] **Diff**: `bwa_calib_view plan.json grid.json`, or load them as A and B in the Diff tab. A
      swapped channel, a bad placement or a bogus position shows at a glance.
- [ ] Copy `grid.json` out of the session folder as the layout the engine loads.
- [ ] **Hear the trims**: play a source on one speaker at a time with
      `bwa_source_set_channel(e, s, ch)` and step `ch` across the array. That route takes the
      trims, so levels match speaker to speaker from the listening point. Do not use the Stage 1
      tone for this: it bypasses the trims.

### Sweep quality

Every sweep in Stage 2 passes the same checks
([calibration.md: Sweep quality](./calibration.md#sweep-quality-the-window-the-snr-floor-the-re-sweep)):

- **The expected-arrival window**: the arrival is searched only around the latency plus the layout
  distance over c. A capture whose strongest tap lies outside it is re-swept, and the line says
  where the tap was. Often a mis-patched output, or a box far off its layout position.
  `--window-m` widens the margin (0.10 m for a surveyed speaker, 0.50 m at its plan).
- **The IR SNR floor**: 40 dB, provisional. A capture under it is re-swept.
- **Two-sweep agreement** (the trims and verify only): a sample and 0.2 dB.
- **Five sweeps** with no clean result stop the run with nothing written (exit 1).
- **The background**: each run prints `background: <median> dBFS median, <loudest> dBFS loudest`,
  and the Session log compares each step with the last.

If clean sweeps read under about 50 dB, or more than a few speakers are re-swept in a quiet room,
the thresholds need the rig's numbers before you trust the trims.

## Stage 3: head tracking

The NatNet parser and lifecycle are tested off-wire. Stage 2's frame check already proved the
room frame from the speakers; this stage proves the head body.

```
bwa_track_monitor <motive-ip> <head body name or ID>
```

No audio device needed. Names need the server; on multicast only, pass the numeric streaming ID.

- [ ] **Data flows**: the pose updates when the body moves. A pose stuck at identity means no
      frames: check multicast routing and the interface, the firewall, and Motive's streaming.
- [ ] **Right body**: hide the head markers; the readout freezes with your body, not someone
      else's.
- [ ] **Frame**: stand at the room center and the position reads about `[0, head height, 0]`.
      Walk toward the front wall and z grows. Step to your right and x shrinks (room-right is -x).
      y is the height above the floor in meters. Face front and the quaternion is about identity;
      turn left and right and check the sign. Fix any disagreement in Motive, never in the client.
- [ ] **Lifecycle**: disconnect and reconnect with new settings mid-run, glitch-free. A failed
      connect leaves the engine on the last committed pose.

## Stage 4: end-to-end on the array

Sound, geometry and tracking together: the `cave` (and `cave_both`) profile, the accepted layout,
the tracker connected.

- [ ] **Known position**: place a source at a surveyed speaker's position (`examples/minimal.c`
      with `profile = cave` and `layout_path` set, or your binding). `bwa_get_bus_levels` and your
      ears peak in exactly that speaker. Repeat on several walls. An axis swap or a mirror here is a
      frame bug: find it before any by-ear tuning.
- [ ] **Walk test**: park a source at a fixed room position and walk around it. The image stays
      put in the room and does not follow you.
- [ ] **Pose prediction**: estimate the motion-to-ears lag (move your head side to side and listen
      for the image trailing; 20 to 40 ms is typical), then `bwa_set_pose_prediction` with that
      lead in seconds. Start at the measured value: too much lead overshoots.
- [ ] **`cave_both`**: the array and the headphone monitor together, and the monitor's image agrees
      with the array's as you walk.
- [ ] **Soak**: a busy scene (looping sources, a moving listener) for 30 minutes or more. On any
      dropout, widen the ASIO buffer or the Dante latency and soak again.
- [ ] **Limiter**: drive it with many loud sources. The gain reduction is linked across channels,
      so the image does not shift when it engages.

## Stage 4b: instrumental phantom accuracy (`bwa_validate`)

Stage 4 says the array puts sound in the right general place. This puts a number on it: render a
phantom in a known direction, measure where it landed, report the miss. The ZM-1 is the
instrument, so it needs the capsule survey from Stage 2.
[validation.md](./validation.md).

- [ ] **Dry run, before rig day**: `bwa_validate --layout plan.json --simulate`. It gives the
      rendering-term baseline the room then adds to.
- [ ] **Write the placements down**: `--positions mics.txt`, one `x y z [label]` per line. The
      built-in envelope is not your room.
- [ ] **Run it tracked**:

      ```
      bwa_validate --layout grid.json --survey capsules.json --out validate.csv --positions mics.txt \
                   --driver "<driver name>" --mic-in 0 --track <stand body> --natnet-server <motive-ip>
      ```

      Each placement becomes a plan: the run waits for the gate (20 mm, still, the orientation
      still), then scores every cell from the MEASURED center. A move over 10 mm or a turn over
      0.41 degrees during a placement drops that placement and stops with exit 4; the placements
      before it are kept, and the run names the ones to run again. Untracked, measure each position
      properly: it is an input to the scoring.
- [ ] **Prove the integrity layer once**: one run with `--inject-fault <ch>`. It exits nonzero
      unless the capsule check catches the fault.
- [ ] **Capsule health**: note every capsule the run excludes. A hot capsule is the dangerous one.
- [ ] **The background check**: each placement first takes one silent capture and prints
      `background: <level> dBFS`. A cell less than 20 dB over it is flagged (provisional). A
      flagged placement: quiet the room and run it again.
- [ ] **Read the physical floor first.** Each speaker is driven alone and captured like any cell,
      and the run prints a `physical floor` before any phantom number. The `cost:` line prints the
      capture count before anything plays. Expect 1 to 3 degrees. **Above about 5 degrees, stop**
      and fix the layout, the survey or the routing: nothing measured after it means anything.
- [ ] **The listening point is a null control**: the physical-against-phantom penalty is about 0
      there by symmetry. The off-center placements carry the result. Never pool them.
- [ ] **Read the tracked-against-fixed contrast**: intervals that exclude zero are the claim.
- [ ] **Read height apart from horizontal**: tracking fixes horizontal displacement, not height.
- [ ] **One placement on a tone**: `--tone 1000`, and `--tone 250` for the hard case. Compare with
      the simulated tone run for the same placement; the excess is the room's. Tone errors are
      repeatable and biased: do not read repeatability as accuracy.
- [ ] **The reference across stimuli**: a directly driven speaker localizes about the same on
      broadband and on a tone. If not, the analysis chain is at fault.
- [ ] **SPCAP focus**: `--focus 2,8,16,32,64`, targets on the speakers' own positions. Read comb
      depth as an excess over the physical floor, never as an absolute
      ([validation.md: Focus, and where the sweep has power](./validation.md#focus-and-where-the-sweep-has-power)).
- [ ] **Keep the CSV** (`--out`), one per stimulus.

Quote no number from this without its caveats: one point per placement, 400 to 1200 Hz, and the
stimulus it came from.

### The knobs the sweep can settle

`bwa_validate` renders through a real engine, so the live knobs are swept axes. Run these before
Stage 5: what the instruments settle costs no listening time.

- [ ] **Calibrate first.** Judge tracked alignment only on an accepted Stage 2 layout. On a layout
      with no measured delays it measures worse.
- [ ] **Tracked alignment**: `--tracked-align both`. Expect the largest effect of any knob, and no
      change at the listening point. A non-null row there means the calibration is wrong.
- [ ] **Dual band and CAP**: `--dual-band both --cap both`. Expect little: both act below 700 Hz,
      and most of the 400 to 1200 Hz analysis band sits above that. Settle them by ear.
- [ ] **Hole-aware spread floor**: `--hole-spread 0,1`. Read comb depth, not the miss, which gets
      worse by design. Take it to Stage 5.
- [ ] **Spread mode and decorrelation**: already settled against changing the defaults. Run them
      only if you doubt the simulation.

The default is one knob at a time against a baseline; `--factorial` takes the cross product. The
run prints the condition table and the cell count before it measures anything.

## Stage 5: by-ear checks

The checks with no assertion. Bring ears you trust.

- [ ] **HRTF monitor quality**: `bwa_playground`, localization scene, headphones. Timbre,
      externalization, front and back. Laterality is already pinned by tests.
- [ ] **Phantom against a real speaker** first, because every trial below is judged against it:
      play a stimulus on one speaker alone with `bwa_source_set_channel(e, s, i)`, then put the
      source back on the panner (`BWA_CHANNEL_AUTO`) at that speaker's surveyed position, and A/B.
      Both take the same output stage, so the levels match. Listen for image size, timbre, and how
      far off-center the phantom separates from the real one. Do it from the center and from at
      least one off-center spot: the center is a null control
      ([validation.md: The physical reference arm](./validation.md#the-physical-reference-arm)).
- [ ] **The screens and the two speaker populations**: play the same stimulus from a box that
      fires through a screen and one that fires over it. If they still differ in timbre after the
      trims, that difference is inside every phantom
      ([calibration.md: What the screens do](./calibration.md#what-the-screens-do)).
- [ ] **The knob bake-off**: the playground's blind A/B/X harness over the live knobs. A knob you
      cannot tell apart on the rig is a knob to retire. Judge a seated install and a roaming one
      apart.
      - **SPCAP focus** is the trial the instruments cannot settle: the layout tool's score, the rE
        error and the comb depth point different ways. Start at the derived default and bracket it
        both ways.
      - **max-rE** defaults on, on the strength of the layout tool's bed metric. This trial
        confirms it; if the rig disagrees, revert the default. Judge the decoder and the taper as
        pairs.
- [ ] **Start every trial from a preset**: `bwa_tuning_preset(BWA_SETUP_SEATED` or
      `BWA_SETUP_ROAMING, &t)`, then `bwa_apply_tuning`. Record the tuning beside each verdict.
- [ ] **Hole-aware spread floor** (`bwa_set_hole_spread`): aim a source below the horizon, where
      there is no speaker, and A/B 0 against 1. Is an honestly wide image better than a confident
      split one?
- [ ] **CAP** (`bwa_set_dual_band_cap`, needs dual band): sit at the listening point, play a lateral
      source, turn your head slowly, and listen for the image walking. A/B against dual band alone.
      It needs real tracking. The instrument that could measure this is not built yet
      ([validation.md: Not built yet](./validation.md#not-built-yet-the-rotating-two-mic-itd-rig)).
- [ ] **Tracked alignment by ear**: walk with it on. Pitch movement while you walk means the slew
      rate is too high for this room.
- [ ] **Tracked directivity** (`bwa_set_tracked_directivity`): pan a bright source to one wall and
      walk to the opposite wall. On, the far wall keeps its treble. Harsher near a wall with it on
      means the shelf cap is too generous.
- [ ] **Tracked room EQ** (`bwa_set_tracked_room_eq`, needs the Stage 2 grid): walk the room and
      A/B. Low-frequency evenness improves from position to position with it on.

### Driving these trials from a Unity scene

Build the scene to mirror the list above: one row per trial, a blind A/B, a recorded verdict.
Every knob is on the Unity binding; two cost a scene restart.

| Trial | Unity control | Cost |
| --- | --- | --- |
| Phantom against a real speaker | `Emitter.Channel` (`Bwa.CHANNEL_AUTO` to go back) | live |
| Preset baseline | `Engine.situation` + `ApplySituation()` | live |
| SPCAP focus | `spcapFocus` / `spcapDensity` | live |
| Hole-aware floor | `holeSpread`, 0 against 1 | live |
| CAP | `dualBandCap` (needs `dualBand`) | live |
| Tracked alignment | `trackedAlign` + dead zone / slew | live |
| Tracked room EQ | `trackedRoomEq` | live |
| Panner | `panner` | live |
| max-rE and its band split | `maxRe`, `maxReSplit` | live |
| Air absorption | `Emitter.airAbsorption` | live |
| Spread mode, decorrelation | `spreadMode`, `decorrelation` | live |
| Bed decoder, AllRAD vs EPAD | `bedDecoder` | restart |
| HRTF monitor quality | profile `Binaural` | restart |

- **Load twice, not four times**: `bedDecoder` is the only load-time half of the decoder and taper
  pair. Load AllRAD, then EPAD, and toggle `maxRe` inside each.
- **Build the reference row first**, then the three trials no instrument settles: SPCAP focus, the
  hole-aware floor, and CAP.
- **Encode the ordering rules**: refuse the tracked alignment trial before Stage 2 is accepted, and
  show its listening-point row as a null check.
- **Record the configuration beside every verdict** with `Engine.TryGetEngineTuning`, which reads
  what the engine has rather than what the inspector claims.
- Put spread mode and decorrelation last: Stage 4b already settled them.

## Record these numbers

Every default below came from simulation. Measure each one on the rig, write it down, then put it
where the last column says. A threshold that turns out wrong for this room changes in code, not on
the day.

| Number | Where you read it on the rig | Default now | Where it goes |
| --- | --- | --- | --- |
| IR SNR of a clean sweep | each `snr=` on the trims run (step 5) | floor 40 dB | `CALIB_SWEEP_MIN_SNR_DB`, `src/calib/calib.h`; [calibration.md: The noise floor](./calibration.md#the-noise-floor) |
| Background level | `background:` per run; the Session log's `=== background:` line per step; `bwa_validate`'s `background:` per placement | RISING at +6 dB; validate flags under 20 dB | `SES_BG_RISE_DB`, `examples/calib_session.h`; `VALID_BG_MIN_DB`, `src/calib/valid.h` |
| Motive position jitter, untouched stand | Placement panel `spread`; the largest move on a trims or verify `placement:` line | stillness 0.5 to 2 mm | `PLACE_STILL_MIN_M`, `PLACE_STILL_MAX_M`, `src/calib/placement.h` |
| Motive orientation jitter, untouched stand | the largest turn on a trims or verify `placement:` line (marked unchecked); Placement panel `orientation spread` | turn floor 0.3 deg | `PLACE_TURN_MIN_DEG`, `src/calib/placement.h` |
| Clicker stillness on a held hand | each refused clap's `the clicker was moving: its tip wandered X mm` | 4 mm over 300 ms | `CLICKER_STILL_M`, `examples/clicker_track.h` |
| Clap onset timing | how many still-held clicks are refused as moving, or for the window | 50 ms guard before the stamped onset | `CLICKER_GUARD_S`, `examples/clicker_track.h`; [calibration.md: Clap positions from a tracked clicker](./calibration.md#clap-positions-from-a-tracked-clicker) |
| Digiface ASIO clock (the ZM-1 capture and the array sink) | Zylia tab with the ZM-1 open, the trigger line: `block N, input latency N`, and `systemTime on QPC` (with `callback +X ms after it`), `on timeGetTime`, `on an unknown base` or `absent`; `bwa_zylia_probe`'s `streaming` line; a stamped clap's **onset** tooltip. The ZM-1 arrives on the Digiface's own driver, so this is also the base the array sink's block stamps see; the sink classifies it with the same code, but nothing public reads its result | `systemTime` used only on QPC, else the callback's QPC read, for the onset and for the array's `bwa_get_clock` stamps alike; no input latency given = one block. The Digiface's base is unverified | `SINK_TS_*`, `src/sink/sink_tsbase.h`; `zp_onset_s`, `examples/zylia_capture.h`; [backends.md: Timestamp](./backends.md#3-timestamp); [calibration.md: Clap positions from a tracked clicker](./calibration.md#clap-positions-from-a-tracked-clicker) |
| Arrival spread across boxes | `--ref-speakers` `us from the median`; verify's `arrival spread` | flag 0.1 ms; verify 100 µs | `REFSPK_FLAG_S`, `examples/calibrate.cpp`; `CALIB_VERIFY_ARRIVAL_US`, `src/calib/calib.h` |
| Range residuals | the capsule survey's per-speaker `from the median` (mm) | flag 10 mm | `CSURVEY_RANGE_FLAG_MM`, `examples/calibrate.cpp` |
| Rigid-sphere effect | the capsule survey's `residual` and `radius`, against a clap survey of the same mounting | WARNING over 5 µs, radius outside 40 to 60 mm | `CSURVEY_RESID_WARN_US`, `examples/calibrate.cpp`; [calibration.md: The speaker-sweep capsule survey](./calibration.md#the-speaker-sweep-capsule-survey---capsule-survey) |
| Live-aiming reading rate | `live: N reading(s) in X s, Y per second` | 1 to 1.2 per second, untimed | [calibration.md: Update rate](./calibration.md#update-rate) |
| Latency residual with the ZM-1 | localize's `-> residual` | WARNING over 20 ms | [calibration.md: Modes](./calibration.md#modes) (`--localize`) |
| Physical floor | `bwa_validate`'s `physical floor` | 1 to 3 deg expected, stop above 5 | [validation.md: The physical reference arm](./validation.md#the-physical-reference-arm) |

## Known limits

Things the tools cannot catch. Know them before you trust a green run.

- **Steady background noise biases both sweeps alike**, so two-sweep agreement cannot catch it. The
  SNR floor and the background line are the only guards. Keep the room in the state the listener
  will hear, and watch for `RISING`.
- **The clap capture ignores a second transient for about 375 ms after a trigger** (an 85 ms
  snapshot, then a 300 ms hold-off). A clap right after another is never seen, and two transients
  inside one snapshot are timed as one.
- **The clap's onset stamp trusts the driver's input latency.** Latency the driver does not report,
  such as the Dante network's or Dante Via's buffer, makes every stamp late by that much, toward the
  click. The 50 ms guard covers a few milliseconds, not tens. If still-held clicks get refused as
  moving, check the latency set on the ZM-1's Dante flow before you blame the hand.
- **A ZM-1 slipping in its collar turns the capsules and not the markers.** No turn check sees it,
  and every direction after it is wrong. Check the witness mark before every step that reads
  `capsules.json`.
- **A unicast NatNet stream feeds one socket.** The stand and the clicker, or a Session step and the
  Placement panel, cannot both listen. Stream multicast.
- **The ring offset assumes the ring sits at the capsule equator.** A ring higher or lower on the
  housing puts that vertical error straight into every center.
- **The baffle offset is a measurement, and the first frame check runs without it.** Until step 2
  has measured the acoustic positions, `--baffle-offset-m` (the Session's **baffle offset**) is a
  guess, and a wrong one shifts the optical points along their aims. On a lopsided set of visible
  speakers that alone can fail the frame check's 10 mm. Step 1's three passes fix it.
- **An optical `--write --fields position` is not an acoustic measurement.** It records a plan, so
  the baffle depth then reads it as measured, and reads about 0. Read the depth from `as_built.json`,
  never from a file this tool wrote positions into.

## The first day, with the speakers anywhere

Nothing in the engine cares where the speakers stand, only that the layout says where they are.
For a bring-up day with the boxes wherever they fit:

- The plan can be rough. Honest, not exact.
- The channel walk (Stage 1) matters as much as ever.
- Run steps 1 to 3 of Stage 2: localize writes every position wherever the boxes ended up, and the
  capsule survey follows from them.
- Skip the trims, verify and the grid: they belong to the final geometry.

That day proves Stage 0, the first contact of the capture path, and Stage 3 for good. Repeat
everything from Stage 1 when the speakers reach their real places.

## Every visit after

- [ ] **The channel walk** (Stage 1): a re-patched Dante route is the classic silent regression.
- [ ] **The frame**: `bwa_speaker_survey <production layout> --server <motive-ip> --require-frame`.
      A Motive recalibration moves the room frame silently.
- [ ] **Verify**: the ZM-1 at the listening point, `bwa_calibrate --layout <production layout>
      --zylia --verify --survey capsules.json` with the tracker flags. Exit 3: an `ARRIVAL` flag is
      a moved box or a changed Dante latency, so run Stage 2 again from localize; a `LEVEL` flag is
      a changed volume or gain, so fix the box and run the trims again. `bwa_calibrate --check`
      needs an omni mic and refuses `--zylia`, so it does not run on this rig.
- [ ] **Head tracking**: `bwa_track_monitor`, thirty seconds. Poses flow and the axes still agree.
