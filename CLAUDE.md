# CLAUDE.md

Guidance for working in this repository. Read this first, then the file under
`docs/` relevant to the task. The design is settled and implemented through M6
plus the calibration/tooling work (see "Current state" below) — most work now
extends the engine or verifies it on hardware, against these specs.

## What this is

A self-hosted native (C/C++) spatial audio engine for a CAVE installation. It
drives a **26-speaker array** over **ASIO** into a **Dante Virtual Soundcard
(DVS)**, with a **binaural (HRTF) debug monitor** as a second output. Unity and
Unreal are *thin control clients* over a C ABI — no rendered audio crosses that
boundary, only control (sound triggers, source positions, listener pose). The one
inbound exception is the opt-in push-source feed (caller PCM *into* the engine,
control thread) — a source feed, not a render path.

The engine is deliberately *not* built on FMOD/Wwise/middleware. Self-hosting buys
direct access to ASIO timing hooks (`ASIOTime.systemTime`, `ASIOGetSamplePosition`)
and a clean, engine-agnostic core. See `docs/architecture.md` for the why.

## The seam that organizes everything

Sources → per-voice **listener-relative DBAP** panning → an in-memory
**26-channel master bus**. The bus has *consumers*:
- **ASIO device** (production): writes the 26-ch bus straight to DVS.
- **Binaural monitor** (debug): treats each bus channel as a virtual speaker at
  its room position, HRTFs to stereo, writes to a normal output device.

Adding the binaural path did not complicate the core — it is just a second
consumer of the same bus. Protect that property.

## Hard invariants (do not violate)

These are real-time-audio correctness rules. Breaking them causes dropouts/glitches
that are painful to debug, so they are non-negotiable:

1. **No allocation, locks, syscalls, or file I/O on the audio thread.** The audio
   thread is the ASIO `bufferSwitch` callback. wav decode, malloc/free, and logging
   all live on the control thread.
2. **One control thread.** All `bwa_*` calls come from a single thread. The command
   ring is single-producer/single-consumer; a second producer breaks it.
3. **Audio thread owns DSP state** (voice table, bus, panner gains, listener
   active fields). The control thread owns handle allocation and asset memory.
   They communicate only through the two SPSC rings.
4. **Gains ramp, never jump.** Per-voice `gcur -> gtarget` interpolated across the
   block. A discontinuous 26-gain change is audible zipper noise.
5. **Generation counts gate handle reuse.** A stale source handle must be dropped,
   not acted on. Sound *buffers* additionally need the retire-ack handshake before
   the control thread frees them.
6. **`CMD_COMMIT` defines frame coherence.** Position/pose write to *pending* fields;
   only commit promotes them to *active*. The mixer reads only active fields.

See `docs/concurrency.md` for the full model and reference code.

## Repo layout (intended)

```
include/bw_audio.h      Public C ABI (authoritative contract).
src/
  engine.c             public ABI: lifecycle + sink + forwards per-frame calls to rt. [M0/M1/M2]
  rt.h / rt.c          rings, voice table, commit snapshot, generation handles, mixer. [M2]
  sink.h / sink.c      device-sink abstraction + backend dispatch. [M1]
  null_sink.c          offline (no-hardware) sink: threaded silence + timestamps. [M1]
  asio_sink.cpp        ASIO host: driver load, bufferSwitch, sample-pos timestamp. [M1]
  sound.h / sound.c    wav decode to mono float via dr_wav (Sound table lives in rt.c). [M3]
  layout.h / layout.c  speaker geometry load (cave_layout.json via cJSON) + default grid. [M4]
  measure.c/calib.c    bwa_calibrate DSP: sweep+deconvolution, trims, trilateration, room report. [calib]
  zylia.h / zylia.c    Zylia ZM-1 single-position speaker localization (DOA + GN position). [calib]
  dbap.h / dbap.c      listener-relative, constant-power DBAP gain solve. [M4]
  fdn.h / fdn.c        directional FDN reverb bed (phonon-free; takes the reflection bus tap). [innovations]
  ism.h / ism.c        image-source EARLY reflections: shoebox mirrors, panned as point sources. [innovations]
  align.h / align.c    per-speaker gain trim + delay-line output stage. [M4]
  binaural.h/binaural.c  head-oriented 26->stereo monitor (Steam Audio HRTF is the upgrade). [M5]
  ambisonics.h/.c      3rd-order ACN/SN3D encode (+ phonon N3D scale) for the Steam decode. [M5]
  steam_decode.h/.c    production ambisonics->stereo HRTF decode via phonon (with-SDK). [M5]
  steam_scene.h/.c     materials occlusion: IPLScene+IPLSimulator on a sim thread (with-SDK). [materials]
  steam_reflect.h/.c   reflection bed: IPLSimulator reflections -> ambisonic IR -> SH->26 bus tap (with-SDK). [materials]
  steam_path.h/.c      sound pathing: indirect routing -> per-voice shCoeffs -> SH-encode -> bus tap (with-SDK). [materials]
  natnet.c             OptiTrack pose ingest (off-wire, see docs/build.md). [M6]
test/                  ctest suite; targets are prefixed test_* (test_smoke, test_rt, test_dsp, ...) so
                       the built tools (bwa_*) and the tests sort apart in the bin dir. xval_data.h is
                       GENERATED (tools/xval) — don't hand-edit.
tools/xval/            gen_reference.py: cross-validation golden generator (scipy SH / l1-LP VBAP /
                       qhull AllRAD / bilinear RBJ / lfilter) -> test/xval_data.h for the xval ctest.
                       Needs numpy+scipy; ctest itself does not (the header is committed).
bindings/
  unity/               P/Invoke + Engine/Emitter (see docs/integration.md).
  unreal/              module + component.
docs/                  Specs. Start here.
examples/              cave_layout.json (see docs/layout-schema.md); minimal.c (the client lifecycle),
                       ambisonic.c (beds: AmbiX/FuMa load, rotate/tilt, renderer + max-rE A/B),
                       streaming.c (disk streaming + push sources) — console walkthroughs, built every build.
third_party/           asiosdk/ (GPLv3 option, vendored), steam-audio-source/ (submodule) + steam-audio-artifacts/ (built phonon SDK); dr_wav + cJSON are
                       fetched by CMake (FetchContent, pinned) — see third_party/README.md.
```

## Build

Target: **Windows only** (ASIO is Windows-only; DVS is Windows/macOS). CMake.
A future cross-platform move means abstracting the device layer (ASIO is just the
Windows sink) — do not bake ASIO assumptions outside `asio_sink.c`.

```
cmake -S . -B build -A x64      # default generator = newest installed Visual Studio
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo      # runs the full test suite (test_* targets)
```

**Current state (M6 + occlusion):** builds `bw_audio.dll` + the test suite (nineteen ctests with the Steam
Audio SDK, fifteen without; +`calib_view` with `BWA_BUILD_CALIBVIEW`, +`layout_tool` and
+`playground` with `BWA_BUILD_PLAYGROUND`). `rt.c` is the concurrency spine
(two SPSC rings, voice + sound tables, commit snapshot, generation handles) + retire-ack;
the whole `bwa_*` API forwards to it. `sound.c` decodes wav (dr_wav) and `mix_voice` plays
`sound->pcm` with a gain ramp. Spatialization is real: `layout.c` loads the surveyed
geometry, `dbap.c` is the listener-relative constant-power gain solve, `align.c` applies
the per-speaker gain trim + delay. `binaural.c` is the head-oriented 26→stereo monitor, and
`engine.c` wires all three profiles (`cave` 26→device, `binaural` 26→2ch via the monitor,
`both` array+monitor on two sinks via a double-buffer). Binaural/both reach headphones live
through an auto-picked 2-ch **ASIO** driver (sizes its render scratch to the device block, so
any driver buffer size works); `bwa_get_audio_backend()` reports the device actually opened.
**`natnet.c` is M6: an off-wire NatNet (OptiTrack) FrameOfData parser + a seqlock pose handoff
(`pose.h`); with a tracker connected the audio thread samples the freshest head pose at block
time** (`rt_set_tracker`), configured at runtime via `bwa_tracker_connect`/`bwa_tracker_desc`
(see docs/api.md). An
interactive **playground** (`examples/playground.cpp`, opt-in `-DBWA_BUILD_PLAYGROUND=ON`)
auditions binaural by ear across seven feature **scenes** (TAB): localization (with a SPACE auto-move
sweep), occlusion+materials, directivity, a channel-walk speaker check, an **ambisonic bed** room (a
synthesized 3rd-order field, world-locked: spin/tilt via bwa_bed_set_orientation, matrix vs
parametric renderer, max-rE decode weighting — the by-ear home for the bed knobs), a **blind A/B/X** harness
(X is secretly A or B over one live knob — dual-band, DBAP vs SPCAP/VBAP, spread, spread RENDER
(LOBE vs MDAP / LOBE vs SPECTRAL), decorrelation, air absorption —
answer over N trials and a one-sided binomial p-value says whether the difference is genuinely
audible, not just "sounds different to me"), and a reverb-bed room (which
rebuilds the engine on entry/exit, since the bed + room geometry are load-time). Its 3D scene shades
each speaker gizmo by that channel's live output level (`bwa_get_bus_levels`, mirrored as a meter
strip in the panel), and with no ASIO device the engine falls back to the null sink and keeps
rendering — visual-only mode is live, just silent. **`bwa_set_test_signal(channel, kind, gain)`** drives one
raw output channel with a 660 Hz sine/noise injected after align (`rt.c`) — a speaker-check / wiring
tool, not a spatial path. The **production Steam Audio HRTF decode** is built + smoke-tested:
`ambisonics.c` (3rd-order encode) → `steam_decode.c` (phonon `iplAmbisonicsDecodeEffect`), gated
`BWA_HAVE_STEAMAUDIO` (phonon built from the `third_party/steam-audio-source` submodule, staged in `steam-audio-artifacts/`; see
third_party/README.md), with the simple-pan monitor as the no-SDK fallback. The `steam_decode` test
drives the 26→stereo decode with a **660 Hz tone** and asserts gross laterality (right→right ear,
left→left, 180° flips) — the tone matters: an earlier DC-driven version of this assertion had
INVERTED polarity (the default HRTF's per-ear DC gains are laterally opposite its audible ILD), which
mis-diagnosed a correct encode and shipped an m<0 sign "fix" that mirrored left/right for all real
audio; only a by-ear report caught it. Laterality checks must never drive DC. The full-dll laterality
check in `smoke` (null-sink tap, `sink.h`) pins the same thing end-to-end, and `xval` pins the SH
encode against phonon's own table (m<0 rows included — trust it over any single-path test).
HRTF *quality* (timbre/externalization/front-back) is still the by-ear check. **Materials: occlusion +
per-band transmission EQ + source directivity are implemented** (`steam_scene.c`, same gate): a third
"simulation thread" owns an `IPLScene` + mesh + `IPLSimulator`, ray-traces volumetric occlusion +
transmission + directivity at 30 Hz, and publishes per source a (level, 3-band tilt, directivity-gain)
set via per-voice atomics (`rt`'s `occ_handle`/`occ_val`/`occ_eq`/`occ_dir`) gated on the audio
thread's own generation; the audio thread applies a 3-biquad transmission EQ (so a wall *muffles*, not
just attenuates — rate-derived, runs at 96 kHz too), a directivity dipole gain, and the level — all
ramped per sample. `bwa_scene_set_mesh_mat` / `bwa_source_set_occlusion` / `bwa_source_set_directivity` /
`bwa_source_set_orientation` drive it; the playground wall is a real occluder. The **reflection bed** is
implemented (`steam_reflect.c`, same gate): an `IPLSimulator` reflections sim → ambisonic IR → the
SH→26 decode → bus, registered as the rt bus tap at `bwa_start`; the `reflect` test proves it's
*directional*. Sources opt in via `bwa_source_set_reverb`, with a per-source wet-send level
(`bwa_source_set_reverb_send`) and an optional **distance→wet** scaling (`bwa_source_set_reverb_distance`,
near = drier / far = wetter; the send gain is distance-derived in `rt.c` and ramped). **Baked reflections**
(`bwa_reflections_desc.bake`, same gate) precompute the reverb at a probe grid at `bwa_start` so the sim thread looks it
up instead of ray-tracing; the `bake` test confirms it stays directional. **Sound pathing** is wired end to
end too (`steam_path.c`, same gate, opt in with `bwa_desc.enable_pathing`): a 10 Hz sim thread rides the same probe
machinery to route a blocked source around occluders / through openings and publishes each opted-in voice's
ambisonic shCoeffs to `rt.c` (`rt_set_pathing`, handle-gated double buffer); the mixer SH-encodes that
voice's *un-occluded* signal into a shared ambisonic accumulator (ramped) and the `path` rt tap decodes it
to the bus via phonon's own decoder (convention consistent encode→decode). `bwa_source_set_pathing` opts in;
the `path` test proves the route bends around a wall with the right direction, the `rt` test proves the
encode lands on `s·shCoeffs`. The **bending-loss EQ is rendered** too: the sim normalizes phonon's
`eqCoeffs[3]` to a pure spectral tilt (level stays in `shCoeffs`, loudest band = 1) and publishes it
alongside the shCoeffs; the mixer applies the same low-shelf/peak/high-shelf biquad cascade occlusion
uses to the *un-occluded* `s_raw` before the SH-encode (ramped + bypassed-when-flat), matching phonon's
own `path_effect.cpp` order (EQ the mono signal, then scale each SH channel) — the `rt` test asserts a
non-flat tilt colours the encoded field. See docs/materials.md. **Opt-in per-source
propagation effects** are implemented (phonon-free,
pure `rt.c` DSP, default off): **Doppler** (`bwa_source_set_doppler`) renders each voice through a
per-voice fractional delay ring (`RtCore.dop_ring`, one power-of-two ring per voice, allocated at
create) whose delay glides toward `distance/c` — the glide rate is the pitch shift, saturating past
~8 m; and **air absorption** (`bwa_source_set_air_absorption`) a distance-driven one-pole HF low-pass.
Both compute the source↔listener distance per block, ramp per sample (invariant 4), and tap the
reflection send *before* themselves (direct path only); indices stay integer so a long-lived voice
never loses sample precision. **Source spread/size** (`bwa_source_set_spread`, 0=point..1=wide) is also
in, with three render modes behind **`bwa_set_spread_mode`** (atomic live A/B): LOBE (default) blends the
panner's point gains toward a width-controlled lobe centred on the source direction, renormalised to the
panner's own power (widening never re-levels) — panner-agnostic; MDAP (Pulkki) pans a 12-direction
virtual-source ring with the SELECTED panner and sums, so the extent inherits the panner's character
(collapses to the point solve at spread 0); SPECTRAL (Zotter/Frank frequency-dependent panning, the
ambix_widening idea per source) splits the voice into 6 one-pole bands and pans EACH BAND to its own
direction inside the cone (LF stays on the source direction; band gains are real panner solves;
constant power via a precomputed band-overlap compensation, `fs_w`) — width with no coherent copies
to collapse or comb, the decorrelation alternative; engage/retire hand off exactly through the
single-path gains (`fs_on` 0/1/2 in `rt.c`; the `rt` test pins all three modes + the LF-stays/HF-moves
signature).
**Dual-band panning** (`bwa_set_dual_band`, off by default, live A/B) wraps the selected panner: a 700 Hz
complementary crossover splits each voice, the low band panned amplitude-normalised (`Σg = gain`,
velocity-vector) and the high band power-normalised (the panners' usual `Σg² = gain²`) — SPAT's "VBP
Dual-Band", for sharper LF localisation near the sweet spot; `compute_gains` derives `gtarget_lo` every
solve so it A/Bs live, and the mixer reads it only when on. **AllRAD adds imaginary pole speakers**: a
pole no real speaker covers within 60° (a floor-less array's nadir) closes the hull with an imaginary
speaker whose decode share is discarded, so downward diffuse energy is dropped instead of smeared onto
the bottom ring (`allrad.c`; the `dsp` test pins it on a floor-less grid). **EPAD is the second bed
decoder** (`bwa_desc.bed_decoder = BWA_DECODE_EPAD`, `epad.c`): the polar-factor decode
`D = c·Yᵀ(YYᵀ)^(-1/2)` (Zotter/Pomberger/Noisternig 2012) makes a panned plane wave's decoded energy
constant over direction by construction (the `dsp` test measures CV 0.09 vs sampling's 0.95 on a
clustered array; a 16×16 Jacobi eigensolve at load; `xval`
pins it against numpy's SVD polar factor; the FDN's line render follows EPAD too — AllRAD selected
keeps the FDN's house AllRAD, the `fdn` test pins the EPAD build). **The sampling (projection)
decode is no longer selectable** — the public enum is `BWA_DECODE_ALLRAD` (0, the default) /
`BWA_DECODE_EPAD` (1); the lit review was unanimous that SAD is dominated on irregular arrays, so it
survives only as the internal degenerate-array fallback (rt-internal decoder id 0; engine.c maps the
public enum to internal 1/2, and a bare rt core without rt_set_bed_decoder still defaults to SAD —
which the rt tests rely on). **The MDAP/spectral spread ring frame is
parallel-transported per voice** (`spread_frame`, `rt.c`): the old fixed-up-vector frame flipped ~180°
in one solve when a moving source left the |d·y| > 0.9 pole zone, teleporting the spectral bands'
directions — the `rt` test sweeps a wide source over the zenith and pins step continuity (0.0057 vs
0.075 before). **`rolloff_r` derives from geometry when the layout file omits it** (0.25 × mean
centroid→speaker distance, Sundstrom 2021; `layout.c`, explicit values win), and the `dsp` test pins
DBAP's boundary-crossing contract (continuity/monotone level/injectivity out through a corner speaker
— the documented hull-projection DBAP failures, impossible here by construction). **`layout_tool`'s
optimizer is multi-objective**: a "worst wt" slider scalarizes mean↔worst-case rE error (1 = pure
maximin — no direction/seat sacrificed for the average) and "focus wt" adds a Frank-spread image-focus
term (186.4°·(1−|rE|)+10.7°) the direction error can't see; scored in the HUD + headless `--score`,
pinned by the `layout_tool` suite's maximin logic test. **Tracked room EQ**
(`room_eq_grid` in the layout, written by `bwa_calibrate --room-eq-grid` one run per mic placement) is
the moving-listener form of `--room-eq`'s modal half: room mode FREQUENCIES don't move with the
listener, so each speaker gets one fc/Q ladder + per-position cut depths; each block `rt_render`
IDW-interpolates the depths at the live listener position (`room_eq_track`) and `align.c` slews its
biquads toward them at 24 dB/s (fixed-fc coefficients rebuilt from precomputed cw0/alpha — no trig
beyond a `powf` on the audio thread). `bwa_set_tracked_room_eq` is the live kill switch (glides to
flat); static `room_eq` and the grid are mutually exclusive (loader-enforced; the grid writeback
re-merges all positions via `calib_room_grid_merge` fc-clustering and strips a stale `room_eq`).
Covered in the `dsp` (loader + align slew), `rt` (listener-follow + kill switch), and `calib`
(merge + accumulating writeback) tests. **Decorrelation** (`bwa_set_decorrelation`, off by default,
live A/B): a spread source's wide part splits off through per-speaker sparse VELVET-NOISE filters
(`rt.c` `dc_*`: ~30 taps/30 ms, unit energy, per-channel seeds; a shared decor bus convolved after
the voice loop with a tail-flush + idle history wipe) so wide sources are made of mutually
INCOHERENT speaker feeds — no phantom collapse or walk-dependent comb filtering; constant power
(incoherent energy adds), split = sqrt(spread), ramped per sample. The `rt` test pins coherent→
incoherent→coherent round-trip + power. **Parametric bed renderer** (`bwa_set_bed_renderer`, live
crossfade per bed): first-order DirAC in 4 time-domain bands (`mix_bed`) — per band the smoothed
FOA intensity vector gives direction + diffuseness ψ; the direct stream (√(1−ψ)·W) re-pans through
the LISTENER-RELATIVE panner at the array shell (`ref + bed_radius·doa` — a walkable bed: parallax
off-centre, which no matrix decode gives), the diffuse stream (√ψ·FOA) decodes through bed_decode
into the decorrelators; loudness-matched to the matrix decode via a direction-averaged plane-wave
reference (`bed_pref`, derived in build_bed_decode). The `rt` test pins sharper-than-matrix
localization, diffuse spread, power match both ways, and the round-trip. **Directional FDN reverb**
(`bwa_fdn_config`, load-time, phonon-free,
`fdn.c`): a 16-line Householder FDN whose lines render as plane waves through the SH→26 bed decode,
2-band decay + per-direction decay scaling (diagonal Directional-FDN, Alary/Politis/Schlecht 2019);
it takes the reflection BUS TAP instead of the Steam bed (mutually exclusive; same aux send + send
levels, `bwa_reverb_set_gain` applies), so reverb now works in no-SDK builds. The `fdn` test
pins RT60 landing (0.8 s configured → 0.800 measured), the 2-band split, anisotropy, and stability.
**Image-source EARLY reflections** (`bwa_source_set_early_reflections` + `bwa_early_reflections_set_gain`,
per-source opt-in, phonon-free, `ism.c`): the FDN's other half — the six first-order shoebox mirrors,
each rendered as a POINT SOURCE at its mirrored position through the LISTENER-RELATIVE panner, so
reflections have direction AND parallax as the listener walks (a shared listener-centric bed cannot).
`bwa_scene_set_box` now captures the room with or without the SDK, so one call feeds both the
ray-traced scene and the ISM. Per image: gliding fractional delay (path/c, per-voice `ism_ring`),
one-pole HF damping from the material's high-vs-mid absorption, ramped panner gains; enable wipes the
ring (recycled slot) and snaps delays, disable ramps out over one block. Order 1 ONLY by design —
higher orders are the FDN's job. The `ism` test pins the mirror geometry; the `rt` test pins arrival
times against the geometric prediction (floor+ceiling pair at 546 samples), direction, and opt-out.
**Tier-2 rendering polish** (all rt-tested): **pose prediction** (`bwa_set_pose_prediction`, off by
default) — `pose.h` slots carry the writer's clock (`pose_write_t`; on NatNet 4.1–4.5 natnet stamps the
SERVER's clock from the frame suffix — mid-exposure ticks when the handshake gave the tick rate,
fTimestamp otherwise — so the velocity dt sees no delivery jitter and survives a mid-session
camera-rate change; outside that range — older, or NEWER than the vendored reference certifies
(`stamps_supported`, natnet.c) — stamps QPC at arrival; one clock per connection, fixed at open),
and `rt_render` leads the tracked position by a fixed user lead along a ~100 ms-smoothed,
speed-capped, dropout-reset velocity from those stamps ONLY (never cross-clock vs the device time);
**near-listener widening** (`bwa_set_near_spread`, off) — `compute_gains` floors every source's
spread at `1 − dist/radius` so a fly-by widens instead of snapping across the head; the decor split
follows the solved `spread_eff`; **metric source size** (`bwa_source_set_size`, radius m, 0 = point)
— spread floored at the subtended angle `asin(r/d)/(π/2)` (capped 1 when the listener is inside), so
physical size holds constant as the listener walks and a sized source subsumes near-spread; **equal-loudness distance compensation**
(`bwa_source_set_loudness_comp`, per-source opt-in) — a ~250 Hz one-pole LF shelf boosting 0.4 dB per
dB of the panner's own attenuation (cap +8 dB), ramped, direct-path-only like air/Doppler;
**multi-listener compromise panning** (`bwa_set_extra_listeners`, up to `BWA_EXTRA_LIS` = 3, commit-
gated) — per-listener point solves (each extra has its OWN SPCAP/VBAP cache: they're listener-keyed)
energy-meaned per speaker (`panner_gains_at` is the any-listener solve `panner_gains` now wraps);
spread/Doppler/air/monitor stay primary-relative. **QoL surface** (all rt-tested in one batch):
`bwa_set_master_gain` (one ramped scalar over the whole mix, pre-align so trims/test-signal stay
calibrated), `bwa_source_fade_to`/`bwa_source_fade_out` (audio-thread timed fades; fade-out lands on
the click-free stop path; an explicit set_gain cancels a fade), `bwa_set_paused` (global pause riding
`pause_gate` — one atomic loaded once per block), **mix groups** (`bwa_source_set_group` 0..7 +
`bwa_group_set_gain`/`bwa_group_set_paused`: group gain folds into the solve via `compute_gains`' `ug`,
group pause rides `pause_gate`; a group-gain change re-dirties its members), `bwa_get_active_voices`
(rt_render's active count, atomic-published), and `bwa_source_set_occlusion_manual` (control-thread
access to the sim's handle-gated occlusion/transmission-EQ publish path — no-SDK gameplay occlusion;
don't drive one source from both). **Integration QoL** (rt/smoke-tested): `bwa_get_asio_driver_count`/
`_name` (engine-free registry enumeration for device pickers, `asio_sink.cpp`; the auto-pick's
lazy-global gap on a process's first open is fixed alongside), `bwa_sound_get_frames`/`_channels`
(asset metadata at the engine rate; streams report the decoder's file length, push = 0 unknown), and
`bwa_source_set_attenuation_override` (per-source distance curve — same formula as the layout knob,
applied by RATIO in the solve so it's panner-agnostic and composes with spread/dual-band/decor;
loudness comp tracks the override's own curve; rolloff 0 = a constant-level direction-only source;
ref <= 0 clears). **`bwa_source_get_playhead`/`bwa_bed_get_playhead`** (rt-tested) is the per-voice
content-playhead readback (engine-rate frames) riding the same per-block republish as `is_playing`
(`pos_pub` packs gen<<48|pos48 in ONE atomic so the gen gate can't tear): cursor for memory/bed
voices, consumed frames for stream/push (an underrun slips it), frozen under pause, 0 while a
scheduled play holds — the AV-sync readback a client-side DspTime derivation can't get right
(Unity: `Emitter.Playhead`/`PlayheadSeconds`, `AmbisonicBed.Playhead`, and `Emitter.PlayAt` now
surfaces the long-bound `bwa_source_play_at`). **`bwa_get_clock`** (rt+smoke-tested) publishes the
device's (sample position, host-time-ns) pair from each block callback — the ASIOTime stamp the
sink always captured but rt_render used to discard — via a C11 seqlock (`clk_seq/clk_sample/
clk_time`, odd = write open; an unstamped block KEEPS the last valid pair), giving clients a
jitter-free wall↔dsp mapping for AV sync (epoch is backend-defined: drivers vary, the null sink
counts from stream start, the MANUAL sink never stamps — reproducibility; the ASIO sink synthesizes
a QPC stamp at callback entry when the driver omits systemTime — FlexASIO does, DVS unknown); **`bwa_get_output_latency`**
surfaces `ASIOGetLatencies` through a new sink-vtbl entry (`output_latency`, queried after
CreateBuffers; DVS reports its Dante buffering there; null/manual = 0) — when a scheduled sample is
HEARD, not just rendered. Unity: `Engine.GetClock`/`OutputLatency`/`DspTimeAt`/`RealtimeAt` (the
last two keep a decaying-max epoch-offset estimator refreshed each LateUpdate — converges through
minimal-age refreshes, tracks ppm drift, falls back to block-granular DspTime pairing stampless).
**Pitch** (`bwa_source_set_pitch`, [0.25, 4]): fractional-cursor
linear-interp resample of in-memory sounds in `mix_voice` (integer cursor + frac — no precision
loss; the rate GLIDES per sample; the loop seam handles multi-sample overshoot; streams/beds
unaffected; composes with Doppler). **Bed rotation** (`bwa_bed_set_rotation`, radians): closed-form
yaw SH rotation in `mix_bed` (`bed_rotate_z`: each degree's ±m pair rotates by m·yaw; per-sample
phasor recurrence, no per-sample trig), glided at ~1 turn/s, applied before BOTH bed renderers;
positive yaw turns the field from room +z toward +x (the `rt` test pins the convention + level
conservation). **Full 3-axis bed orientation** (`bwa_bed_set_orientation`, yaw/pitch/roll — level a
capture that wasn't upright): any pitch/roll engages the Ivanic-Ruedenberg SH rotation
(`ambi_rot_matrix`, `ambisonics.c`) — the live matrix rebuilds per block from the glided angles and
interpolates per sample; yaw-only stays on the exact phasor path and the two hand off seamlessly at
pitch=roll=0 (the `ambi` test pins M(R)·encode(d) == encode(R·d) for random rotations + block
orthogonality; the `rt` test pins the pitch-to-ceiling convention + the handoff). **max-rE decode
weighting** (`bwa_set_max_re`, off by default, live A/B): Zotter/Frank per-order tapers
(`ambi_max_re_weights`, diffuse-energy-normalized per content order so A/B stays level-fair) applied
where the engine's own SH→speaker decode renders a bed's signal — `mix_bed`'s matrix paths (per-voice
`re_mix` crossfade) and the FDN's line render (`dcomb`/`dcomb_re` pair) — suppressing decode sidelobes
and lengthening the energy vector (better off-centre localization, THE walking-listener case);
point-source panners and phonon's own decodes untouched (the `ambi`/`dsp`/`rt`/`fdn` tests pin the
weights, the rE lengthening through AllRAD, the rear-sidelobe shrink + level fairness, and the FDN
pair). **Band-split max-rE** (`bwa_set_max_re_split`, off by default, live A/B, needs max_re on): the
taper acts only above the 700 Hz crossover — the unweighted rV-optimal decode keeps the low band (the
literature-standard Gerzon basic-LF/max-rE-HF split; per-bed one-pole splitter `re_lp` + ramped split
share `re_sm` in `mix_bed`, so both toggles crossfade); bed matrix decodes only, the FDN stays
broadband (a diffuse tail has no LF image to sharpen); the `rt` test pins LF-stays-raw/HF-matches-
broadband with tones either side of the crossover. **Anisotropic source extent**
(`bwa_source_set_extent`, BS.2127-style width/height 0..1 each): the ring modes squash their
virtual-source cap per axis (affine tangent scaling — a 1×0 extent renders as a horizontal ARC of
panner solves, never a collapse), the lobe stretches its falloff on the extent ellipse (`ext_scale`,
ratios floored so zero extents stay well-defined); room-referenced, so anisotropic sources use the
up-anchored frame (accepting its pole ambiguity — BS.2127's own singularity) instead of the
transported one; equal extents are bit-exactly the isotropic spread (`bwa_source_set_spread` resets
to isotropic; the size/near floors apply to both axes); the `rt` test pins vertical-spill contrast
in lobe + MDAP modes and the iso-equality. **FuMa loading** (`bwa_load_fuma`): legacy B-format (WXYZ order, MaxN + W −3 dB) reordered +
rescaled to AmbiX at load (`sound.c`, phonon-matching published factors; the `sound` test pins the
conversion against the SN3D encode), so downstream a FuMa bed IS an AmbiX bed. **Runtime channel count**: `BWA_CHANNELS` (26, sink.h) is now the CAPACITY only — the
ACTIVE count is the layout's speaker count (4..26; loader accepts N speakers whose indices form a
complete 0..N-1 permutation), fixed per engine instance. `bwa_create` resolves the layout BEFORE
`rt_create` and passes `layout.count` to the rt core, sinks, monitor, FDN; `steam_decode`/
`steam_reflect`/`steam_path` are count-driven too (they previously hard-looped BWA_CHANNELS over the
bus — an overrun at N<26). `bwa_get_channel_count()` is the readback; a FAILED explicit layout load still leaves create
usable on the 26-grid fallback (reason via bwa_last_error) but bwa_start now REFUSES it with
BWA_ERR_LAYOUT (smoke-pinned) — only layout_path = NULL runs the default grid. The `dsp` test pins the loader rules; the `rt` test
runs a 24-channel core end to end with a canary proving nothing writes past the active count. The
GUI tools follow the count too: `playground` takes it from `bwa_get_speakers` (gizmos/meters/channel
walk), `layout_tool` carries `g_nspk` (load sets it from the file, save writes N records, a panel
count control grows onto the dome / drops the tail and REBUILDS the engine — its channel count is
the layout's), `calib_view` uses each loaded `Layout.count` (a Diff of two different-sized arrays
is refused, not silently mis-compared), and `calib_capture` takes the speaker count (ASIO opens
`n` outs + the mic at slot `n`). NSPK/BWA_CHANNELS remain array capacity in all four.
**Cross-validation** (`test_xval`, goldens generated by
`tools/xval/gen_reference.py`): the SH encode, hull/VBAP solve, AllRAD build (both grids, incl. the
imaginary speaker), EPAD build, the Ivanic-Ruedenberg SH rotation, RBJ biquads, and align's room_eq
rendering are pinned against INDEPENDENT references — scipy `lpmv` harmonics (+ phonon's hardcoded
SH table inside the generator), the Franck/Wang/Fazi 2017 l1 linear program (whose non-negative
solution IS VBAP, so scipy linprog/HiGHS validates `hull.c`'s geometric walk — they agree to ~1e-7),
a qhull+numpy AllRAD rebuild, the numpy-SVD polar factor for EPAD (a different factorization of the
same unique decode), rotation matrices RECOVERED from the defining property `encode(R·d) = M·encode(d)`
by lstsq over the scipy harmonics (no recursion in the reference path — a self-consistent slip in the
I-R recursion can't hide, unlike the engine's own `ambi` property test which uses the engine's encode
on both sides), bilinear-transformed RBJ analog prototypes, and a scipy `lfilter` golden. The header is
committed, so ctest stays hermetic; regenerating needs numpy+scipy. DBAP/SPCAP and the MDAP ring
parametrization are house designs with no external numerical reference (Sundstrom 2021 corroborates
`dbap.c`'s hull-free DESIGN — see docs/spatialization.md) — their contracts stay in the
`dsp`/`rt` property tests, and their shared cores (hull/VBAP, SH, biquads) are what xval pins. An **ambisonic bed** is implemented too (`bwa_load_ambix` + `bwa_bed_*`):
a file-fed AmbiX soundfield decoded world-locked to the 26-ch bus (`rt.c` `build_bed_decode`/`mix_bed`,
phonon-free), reusing the SH→26 decode the reflection bed will need. `sound.c` now decodes **WAV/FLAC/MP3**
(dr_libs, one pinned repo fetch) and **resamples to the engine rate at load** (windowed-sinc). **Streaming**
(`bwa_load_sound_streaming`, `stream.c`) plays long files without decoding them into RAM: a background
thread decodes chunks (WAV/FLAC/MP3, downmixed to mono, engine rate required) into a per-stream **SPSC ring**;
the audio thread `stream_pull`s from the ring in `mix_voice` (no I/O/alloc/locks), distinguishing a true EOF
from a transient underrun. One voice per stream; the retire handshake detaches voices before the control
thread closes the stream. **Push (procedural) sources** (`bwa_source_create_push` +
`bwa_source_push`/`_push_space`/`_push_end`) ride the same ring in push mode (`stream_open_push`: the CONTROL
thread is the producer, the streaming thread never touches the slot), so `mix_voice` is unchanged — the source
consumes from create (underrun renders silence without losing the caller's place; the data-driven clock slips,
never drops), `push_end` ends the voice once the ring drains (one-way; not restartable — stop/fade_out end
it the same way, refusing further pushes; pause is the temporary silence), the internal sound slot retires
with the source handle (destroy AND steal paths — a steal of an already-DRAINED victim finalizes + acks
immediately instead of waiting on a fade that never comes; a retire that hits a full command ring parks and
retries at drain_events), and `bwa_source_play` on a push source is refused (every play entry point,
bwa_bed_play included, reports the error). `bwa_source_is_playing` counts a still-queued play as playing,
so create→push→push_end→poll→destroy can't drop a clip in the first-block window. The `rt` test drives the
mix path + lifetime cycles (incl. steal-of-drained and the parked retire); the `stream` test pins the ring
mechanics (exact-capacity fill, wrap, NaN scrub, underrun-vs-end).
**Voice management + scheduling**: sources carry a control-side steal **priority** (`bwa_source_set_priority`,
255 = protected) — a full pool steals the lowest-priority voice instead of failing the create; and
**`bwa_source_play_at(start_sample)`** fires a voice sample-accurately off a published dsp clock
(`bwa_get_dsp_time`, device-anchored), the mixer holding it silent until the exact in-block offset.
**Pause/seek** (`bwa_source_set_paused` / `bwa_source_seek`): a per-voice gate ramps over one block
(invariant 4) and the playhead freezes only once silent, so resume continues exactly where pause
landed and a seek on a running voice is ramp-out → jump → ramp-in (click-free); pause covers
memory/stream/bed voices, seek is in-memory/bed only (the stream ring can't jump), and paused
still reads as playing. An **output protection limiter** (`bwa_set_limiter` / `bwa_set_limiter_ceiling`,
ON by default at -1 dBFS) is the FINAL stage in `rt_render` (after align + the test signal —
everything reaching the device passes through): LINKED across channels (one gain from the
cross-channel peak, so engaging never shifts the spatial image), ~1 ms attack / ~120 ms release +
a hard clamp at the ceiling; protection, not mastering. Both are covered in the `rt` test (freeze,
seek landing, ceiling, linkage). **Ray-tracing
acceleration**: `bwa_desc.embree` runs both sims on Intel Embree, opt-in with a graceful fallback to the
default tracer (the vendored prebuilt phonon isn't Embree-built, so it currently falls back — see docs/api.md).
**Speaker calibration** (`bwa_calibrate`, opt-in `-DBWA_BUILD_CALIBRATE=ON`; DSP in `measure.c`, solve +
JSON writeback in `calib.c`, both unit-tested off-hardware): a full-duplex ASIO tool that sweeps each speaker,
records an omni mic, and writes `cave_layout.json` — per-speaker delay/gain trims (`calib_solve`, arrival-align
+ sensitivity-equalize), acoustic **position self-survey** through the screens (`--localize` → `calib_trilaterate`,
which recovers positions + the system latency jointly), and a **room report** (`--room`, Schroeder RT60 + early
reflections — a treatment diagnostic, NOT a model to match: matching double-counts the real room). `--save-irs`
retains the per-speaker IR kernels (one capture serves trims, the room report, and a future headphone room
simulator). The ASIO capture compiles but is unverified on hardware; `--simulate` runs the whole pipeline
hardware-free. The capture shell logs the driver's OWN latencies at open (`ASIOGetLatencies`; the
`calib_asio_latencies` accessor survives close) and `--localize` cross-checks the solved system latency
against that digital loop — residual = DAC/ADC + analog, so negative is impossible (device/clocking
mix-up) and tens of ms flags the DVS latency setting; `--live` without `--latency` prints the loop as
the lower-bound starting value. `bwa_minimal` prints a measured device rate from two `bwa_get_clock`
stamps (the Stage-0 Dante clock-lock check in docs/hardware-validation.md). **Zylia ZM-1 single-position localization** (`zylia.c`, unit-tested off-hardware via the `zylia`
test) is the one-placement complement to the multi-position omni survey: the 19-capsule sphere sees each sweep
arrive at 19 times, so the arrival-time DIFFERENCES give a speaker's DIRECTION from ONE spot (latency-free,
sub-degree — `zylia_doa`), and with the known latency a Gauss-Newton refine against the exact spherical
wavefront gives the full position (`zylia_localize`). Distance is latency-limited (the array is too small to
self-calibrate latency at metres). Spatial room capture (early-reflection DOA) is still DESIGN ONLY — nothing
consumes `er_delay` directionally yet. The capsule geometry is REAL: the ZM-1's 19 capsules are a vertex-up
**dodecahedron minus the nadir vertex** (Zylia's published node table; built from closed forms —
`asin(√5/3)`, `asin(1/3)`, `atan(√(3/5))` — so nothing is rounded), and the `zylia` test pins the structure
(ring populations 1/3/6/6/3, the unpaired zenith, the sum-to-zenith identity, the 41.81° dodecahedral edge).
What the table CANNOT give you is the **channel order** (node i ≠ ASIO input i) or the **azimuth reference**
(which capsule faces front) — both survive every structural check, both yield a confident WRONG direction,
and no off-hardware test can catch either. Hence the **capsule self-survey** (`zylia_survey`, `zylia_set_capsules`,
`zylia_survey_save/load`): claps from ≥6 known positions recover the capsule positions INDEXED BY ASIO CHANNEL
in ROOM axes, so the result *is* the channel order and *is* the orientation. `τ[k][i] = t0_k − (1/c)·m_i·d_k`,
the unknowable per-observation constant `t0_k` dies under a mean-subtraction across the 19 capsules, and what's
left separates into nineteen independent 3-unknown solves sharing one 3×3 normal matrix — **so it needs no
sweep, no sample-sync and no second audio device**. Two subtleties the code documents: a clap at 2.5 m is a
SPHERE (curvature across the array is a systematic ~1.4 µs ≈ 2–3 mm), so the solver takes source POSITIONS and
iterates an exact-minus-plane-wave correction; and translation is a GAUGE (unobservable), where the default
choice is wrong — the capsule set is centroid-UNbalanced (missing nadir), its centroid sitting R/19 = 2.6 mm
off the sphere centre, so the solve re-centres on the best-fit SPHERE each iteration. Coplanar claps (a
horizontal ring) leave the capsules' HEIGHTS unconstrained; `spread` measures this and below 0.05 it refuses
rather than return a flattened array. The 19-ch ASIO capture is the rig-bound shell, factored into
`zylia_capture.cpp` (driver open, format conversion, transient trigger, snapshot publish via `ZpShared`;
trigger thresholds + the live noise floor are exposed in `ZpShared` for tuning at the rig) and shared by two
consumers: `bwa_zylia_probe` (opt-in `-DBWA_BUILD_CALIBRATE=ON`), the console bring-up meter (tap a capsule
→ its channel jumps — this is what resolves the channel order by hand); and `bwa_calib_view`'s **Zylia tab**,
the live DOA view — a clap is snapshotted, `zylia_tdoa` (onset + windowed cross-correlation with sub-sample
peaks) feeds `zylia_doa`, and a dot appears on the capsule sphere where the clap came from. The tab also hosts
the **Capsule survey** panel (bank claps → Solve → Install → Save). A device exposing FEWER than 19 inputs is
REFUSED for DOA (the unfilled snapshot channels would enter the fit as silent arrivals and point somewhere
confidently wrong). Simulate mode synthesizes claps through the identical pipeline; `zylia/sim_doa` asserts the
recovered direction lands within 2° of truth and `zylia/sim_survey` drives the whole survey flow, asserting it
recovers the built-in table back (which only holds if the UI fed it the right positions, arrivals AND channel
order). **Dante is the unlock for the sweep path**: the ZM-1 can join the Dante network via Dante Via, so DVS
presents its 19 capsules as INPUTS ON THE SAME ASIO DEVICE as the 26 outputs — one driver, one clock domain,
which dissolves the two-device problem that blocks `--zylia` on hardware (and makes latency measurable, so
DISTANCE becomes real). `zylia_survey` is capture-agnostic: it takes (source position, 19 arrival times) and
does not care whether they came from a clap's cross-correlation or a sweep's deconvolved IR peak. See
docs/calibration.md.
**`bwa_calib_view`** (opt-in `-DBWA_BUILD_CALIBVIEW=ON`) is the **calibration station** (imgui +
implot + implot3d on win32+d3d11 — the stack for new panel/plot tools; theme + embedded Roboto +
conventions ported from aforren1/lsl-viewer, the house reference — `examples/bwa_theme.h`): it loads
layouts through the engine's own `layout_load`, shows the array in 3D, gain/delay trims,
correction-EQ magnitude curves, `--save-irs` IR kernels, a layout DIFF (surveyed vs calibrated) with
outlier highlighting — the "did calibration write something sane?" check before accepting a
writeback — the **Capture tab** (bwa_calibrate's core flow in-window: a worker thread runs
sweep→measure→solve→writeback through the same `calib_capture.cpp` backends the CLI uses, rows
publish live via a done_count release/acquire, and the result loads straight into Diff: A = input,
B = what calibration wrote) — and the **Zylia tab** (live clap-DOA on the capsule sphere; see below).
`--tests [filter]` runs **imgui_test_engine**: fake inputs drive the real UI (type path → click Load →
assert 26 speakers / the known 100 mm fixture delta; Run calibration → wait for the worker → Load
into Diff → assert the wobble trims; enable simulate → Clap now → recovered DOA within 2° of truth),
screenshots are captured to output/captures/, pure-logic checks ride the same suite, and it all runs
under ctest (`calib_view`) — a GUI with an automated regression test (test-engine license: free for
open source, NOT MIT — see its LICENSE.txt). **`bwa_layout_tool`** (`examples/layout_tool.cpp`, under
`BWA_BUILD_PLAYGROUND`) is on the same imgui stack via **rlImGui** (pinned `Raylib_5_5` tag): the
3D room view (orbit + head-view cameras, ray-picked speakers, the coverage shell) stays raylib — it's
a *scene*, not a plot — while every control surface (panel/HUD/tooltips) is imgui with the station
theme, and the same `--tests` harness runs it under ctest (`layout_tool`: logic round-trips, panel
fake-input edits, save/reload, score/optimize, full-frame screenshots via a before-swap GL read).
Raylib input handlers gate on `io.WantCapture*`. **`bwa_playground` is on the same rlImGui stack**
(control panel + live output meters in imgui; the 3D scene stays raylib; raygui is gone from the
repo), with its own `--tests` suite under ctest (`playground`: p-value/signal logic, panel
fake-input edits, a scene cycle that rebuilds the engine across the reverb boundary, and
`meters_live` — the suite forces `BWA_SINK_NULL` and asserts the engine STAYS LIVE with no
audio device, pinning the null-sink fallback the tool's visual-only mode depends on). Test-ref
gotchas the code comments document: a `**/` wildcard hashes its LAST
segment as a literal string (use a plain window-relative path for `$$int` component refs, e.g.
DragFloat3 = `"pos/$$0"`), and bare `CaptureScreenshot()` needs `CaptureReset()` between shots;
test-engine synthetic input drives imgui only — raylib key polls (`kp`/`kd`) never see it, so UI
tests must go through panel widgets (or call the app's own functions directly).
**Per-speaker correction filters** (`--eq`) are the "inverse EQ" upgrade to the scalar
trims: `measure_correction` gates the IR to the direct sound (before the first reflection) and inverts that
magnitude into a minimum-phase FIR (`calib_eq` → the layout `eq` array → applied per channel in `align.c`,
before gain+delay), so it flattens the SPEAKER not the room (a moving listener can't be room-EQ'd from one
point — same trap as matching RT60); unit-tested in `measure`/`dsp`/`calib`. **`--room-eq`** is the opt-in
STATIC-listener upgrade (fixed-observer SPCAP/VBAP installs; mic at the seat): the FIR is designed from a
frequency-dependent window (`measure_correction_room` — direct-gated at HF, growing to include the room
toward LF, boosts capped +3 dB) covering 200 Hz up, and 30–200 Hz gets discrete modal CUTS
(`measure_room_cuts` → the layout `room_eq` array of {fc, gain_db, q}, cut-only by schema, rendered as
per-channel biquads in `align.c`) — the split means nothing is corrected twice, and EQ still can't fix
decay (that stays the `--room` report's treatment problem). See docs/calibration.md.
Remaining: the by-ear headphone check; and live Motive verification of M6 (parser + lifecycle are tested off-wire). Do not bake ASIO assumptions
outside `asio_sink.cpp`, and do not link the NatNet SDK (proprietary; reference only — GPLv3).
The atomics in `rt.c` need `/experimental:c11atomics` on MSVC (wired in CMake); `pose.h` uses
Interlocked intrinsics instead, so `natnet.c`/tests need no extra flag. `-DBWA_ASAN=ON`
builds `test_sound` under ASan.

## What NOT to do

- Do not introduce FMOD/Wwise or route audio through the engine's mixer.
- Do not use Unity's built-in audio (8-channel cap) or DVS's WDM driver
  (16-channel cap). 26 channels requires ASIO. This is settled.
- Do not pan via pure ambisonics for localized point sources — the listener moves
  across ~3×3 m and a single sweet spot fails. DBAP is recomputed per frame from
  tracked position. See `docs/spatialization.md`.
- Do not assume Steam Audio's Unity/FMOD *integration* limits apply to its C API.
  The C API supports custom speaker layouts; the integrations do not expose them.
- Do not run the Steam reflection bed AND the ISM early reflections together — the
  bed already contains early reflections, so they render twice (engine.c warns once).
- Do not model the CAVE room itself with the ISM. Its shoebox is the *virtual*
  environment; the physical room supplies its own reflections, and modelling it
  double-counts (same trap as matching the measured RT60 — docs/calibration.md).
- Do not let any `bwa_*` per-frame call block or allocate.

## Which acoustics path (the recommendation)

Three implementations now overlap here; they are complementary, not rivals (the full
comparison + rationale is `docs/materials.md` → "Choosing an acoustics path"):

- **Steam scene** (`steam_scene.c`) for occlusion + directivity + pathing — ray tracing
  earns its keep; the manual path needs the game to already know the answer.
- **ISM** (`ism.c`) for early reflections — the Steam bed is listener-CENTRIC (one field
  around one point, 30 Hz) so its reflections have no parallax; the ISM pans each bounce
  as a point source through the listener-relative panner, per block. That is the engine's
  own thesis applied to reflections. Cost: O(N) in sources vs the bed's O(1) — opt in on
  the few that matter.
- **FDN** (`fdn.c`) for the late tail — deterministic, infinite, designable.

That configuration never creates the Steam reflection bed. A **no-SDK build is fully
viable** (ISM + FDN + manual occlusion); what it loses is automatic occlusion, pathing,
and the real HRTF monitor — the last being a *developer-workstation* dependency, since
the production array render never uses HRTF.

## Doc index

- `docs/architecture.md` — system overview, the bus seam, the full signal-flow diagram
  (every signal kind, source → device, with tap-order rationale), locked decisions + rationale.
  `docs/signal-flow.md` is the same diagram as a rendered Mermaid graph (ASCII is canonical).
- `docs/concurrency.md` — threading model, SPSC rings, commit snapshot, lifetimes. **Most load-bearing.**
- `docs/api.md` — C ABI reference and per-call threading semantics.
- `docs/spatialization.md` — DBAP, moving observer, binaural decode (3rd-order), speaker alignment.
- `docs/materials.md` — material/geometry model → Steam Audio occlusion + reflections → the bus. **Design (Later).**
- `docs/integration.md` — Unity binding + coordinate seam; Unreal notes.
- `docs/build.md` — platform, dependencies, licensing, DVS/Dante config.
- `docs/layout-schema.md` — `cave_layout.json` format: speaker geometry, per-speaker gain/delay, DBAP knobs.
- `docs/calibration.md` — `bwa_calibrate`: acoustic position survey, delay/gain trims, room report → `cave_layout.json`.
- `docs/hardware-validation.md` — the rig-day runbook: staged on-hardware checks (device → wiring → calibration → Motive → end-to-end → by-ear) with pass criteria.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/roadmap.md` — milestone-ordered implementation plan.
