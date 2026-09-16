# Implementation log (historical)

This is the per-feature narration that used to live in CLAUDE.md's "Current state"
section. CLAUDE.md now carries a short "Current state" plus a "Traps" list, and
`docs/api.md` owns the feature catalog. This file is a record only — nothing here is
guidance, and git history is authoritative. Kept so the detail stops loading into every
agent session while remaining findable.

---

**Python binding (2026-09-16).** `bindings/python/`, nanobind on the stable ABI (cp312-abi3, one
wheel per platform), two layers: a raw 1:1 `bw_audio._bwa` over all 166 bindable ABI calls and a
thin Pythonic `bw_audio` over that. `bwa_set_output_capture` is the only exclusion, and the reason
is invariant 1 rather than difficulty. Three decisions worth the record. The wheel build points
scikit-build-core at the REPO ROOT (`cmake.source-dir = "../.."` plus `BWA_BUILD_PYTHON=ON`), not
at `bindings/python`, because the root CMakeLists resolves the ASIO SDK and the staged phonon
against `${CMAKE_SOURCE_DIR}`: an `add_subdirectory` arrangement configures, builds, tests green
and ships a no-SDK, no-ASIO engine. The package moved to a `src/` layout after a `pytest` run from
`bindings/python` imported the source tree, which holds no extension. And the golden test is
`test/golden_test.c`'s own scenario and its own constants driven from Python, which needed no
C-only step and is what proves the binding reaches the same DSP: perturbing the source position by
10 cm moved the energy total 168.17 to 193.05, well outside the 2e-3 tolerance, so the test was
seen red before it was trusted green. Worth knowing about that tolerance: it absorbs a 0.2 percent
nudge of the constant itself, so it catches a DSP change and not a typo in the reference.

**Static phonon for Android (2026-09-15).** Both ABIs, built on a Windows host with the composite
action's own steps and run on an x86_64 emulator: 37 of 38, the five SDK tests included (the red is
`os`'s `os_sleep_until_ns` median-lateness bound, about 1.1 ms against 1 ms, which a no-SDK library
of the same commit misses the same way on the same emulator), and
`bwa_minimal` reports `backend: aaudio:default (steam HRTF direct)`. Android links pffft like every
other platform, not FFTS. Two android-only upstream patches went into `third_party/patches/`: the
scripts drop every `android-arm64` cmake-flag layer because they build that target as
`android-armv8`, and they spell the NDK host tag `windows-x86_64` in six places. The engine link
needed the system `log` library (phonon's logger calls `__android_log_print`) and
`-Wl,--exclude-libs,ALL` (phonon's Android build sets no hidden visibility, so 3000-odd archive
symbols were landing in the dynamic symbol table). Stripped arm64: 0.4 MB to 7.0 MB.

**Static phonon (2026-09-15).** Steam Audio is linked statically. `third_party/steam-audio-artifacts/`
went platform-neutral (`include/` plus `lib/<platform>/` holding phonon and the three companion
archives it needs: mysofa, zlib, pffft), the CMake detect keys on that set
rather than on a Windows import library, and `phonon.dll` left the Godot manifest, the Unity
package, the CI artifact and the release zips. `bw_audio.dll` grew 859,648 to 7,677,440 bytes and
the pair it replaced was 7,173,120, so one file costs 504,320 bytes more than two did. The recipe
moved into a composite action so the Linux, macOS and Android phonon builds can call it.

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
auditions binaural by ear across eight feature **scenes** (TAB): localization (with a SPACE auto-move
sweep), occlusion+materials, directivity, a channel-walk speaker check, an **ambisonic bed** room (a
synthesized 3rd-order field, world-locked: spin/tilt via bwa_bed_set_orientation, matrix vs
parametric renderer, max-rE decode weighting — the by-ear home for the bed knobs), a **blind A/B/X** harness
(X is secretly A or B over one live knob — dual-band, DBAP vs SPCAP/VBAP, spread, spread RENDER
(LOBE vs MDAP / LOBE vs SPECTRAL), decorrelation, air absorption —
answer over N trials and a one-sided binomial p-value says whether the difference is genuinely
audible, not just "sounds different to me"), a reverb-bed room (which
rebuilds the engine on entry/exit, since the bed + room geometry are load-time), and an
**underwater** medium boundary (SPACE dives: live FDN retune + speed of sound, cross-surface
manual occlusion, the pressure-release Lloyd's-mirror bounce; rebuilds like the reverb room —
its FDN is load-time). Its 3D scene shades
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
non-flat tilt colors the encoded field. See docs/materials.md.

**Opt-in per-source
propagation effects** are implemented (phonon-free,
pure `rt.c` DSP, default off): **Doppler** (`bwa_source_set_doppler`) renders each voice through a
per-voice fractional delay ring (`RtCore.dop_ring`, one power-of-two ring per voice, allocated at
create) whose delay glides toward `distance/c` — the glide rate is the pitch shift, saturating past
~8 m; and **air absorption** (`bwa_source_set_air_absorption`) a distance-driven one-pole HF low-pass.
Both compute the source↔listener distance per block, ramp per sample (invariant 4), and tap the
reflection send *before* themselves (direct path only); indices stay integer so a long-lived voice
never loses sample precision. **Source spread/size** (`bwa_source_set_spread`, 0=point..1=wide) is also
in, with three render modes behind **`bwa_set_spread_mode`** (atomic live A/B): LOBE (default) blends the
panner's point gains toward a width-controlled lobe centered on the source direction, renormalised to the
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
complementary crossover splits each voice, the low band panned amplitude-normalized (`Σg = gain`,
velocity-vector) and the high band power-normalized (the panners' usual `Σg² = gain²`) — SPAT's "VBP
Dual-Band", for sharper LF localization near the sweet spot; `compute_gains` derives `gtarget_lo` every
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
decode is no longer selectable** — the public enum was then `BWA_DECODE_ALLRAD` (0, the default) /
`BWA_DECODE_EPAD` (1), since renumbered to reserve 0 for default-init (see the 0.11.0 changelog);
the lit review was unanimous that SAD is dominated on irregular arrays, so it
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
off-center, which no matrix decode gives), the diffuse stream (√ψ·FOA) decodes through bed_decode
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
a QPC stamp at callback entry when the driver omits systemTime — FlexASIO does, the Digiface unknown); **`bwa_get_output_latency`**
surfaces `ASIOGetLatencies` through a new sink-vtbl entry (`output_latency`, queried after
CreateBuffers; the Digiface reports its Dante buffering there; null/manual = 0) — when a scheduled sample is
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
and lengthening the energy vector (better off-center localization, THE walking-listener case);
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
**`bwa_source_play_loop(loop_beg, loop_end)`** is the intro→loop pattern: playback starts at 0 but
the mix seam wraps `cursor` to `loop_beg` at `loop_end` instead of the clip end (a non-repeating
intro `[0,loop_beg)` then a looping body), resolved against the asset at `CMD_PLAY` and honored by
both `mix_voice` and `mix_bed` (0/0 = whole clip, so the existing loop path is unchanged; streams
loop their whole file). **`bwa_source_stop_at(stop_sample)`** schedules a click-free stop: once a
block reaches `stop_sample`, `rt_render` fires the SAME one-block gate fade as `bwa_source_stop`
(block-granular, never a hard cut — so a scheduled stop can't pop; a fresh play clears it).
**`bwa_source_queue(snd, loop)`** (+ `bwa_source_clear_queue`) is **gapless chaining**: a per-voice
FIFO (`Voice.queue[BWA_QUEUE]`, resolved `SoundData*`) the mixer pops at a non-looping end
(`queue_pop_valid`) and continues in the SAME block — no seam. A looping queued entry is the
terminal item (two-file intro→loop: `play(intro, false)` then `queue(body, true)`); in-memory mono
only; queue AFTER play (a fresh play clears it); `CMD_SOUND_RETIRE` NULL-tombstones any queued entry
it frees (the "detach before free" rule that already guards the current `sound`). And
**starts are click-free too**: `CMD_PLAY` zeroes `gcur`/`gcur_lo` (+ `fs_on`) so every (re)play
ramps up from silence over the first block — `gcur` is the final per-channel multiply, so this
fades in the whole direct path; a fresh voice is already zero (create memset), so goldens/first
plays are byte-unchanged, only a REPLAYED slot (which kept the prior solve's gains and would hit
the asset's first sample at full level) is fixed. The `rt` test pins the loop region wrap, the
scheduled stop (fade + on-schedule end + replay-cancel), and the gapless chain (a queued voice
outlives A's end vs an un-queued control, playhead restart, looping terminal, mid-block-seam
fullness, clear_queue); the golden test confirms fresh plays are untouched.

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
mix-up) and tens of ms flags the Dante latency setting; `--live` without `--latency` prints the loop as
the lower-bound starting value. `bwa_minimal` prints a measured device rate from two `bwa_get_clock`
stamps (the Stage-0 Dante clock-lock check in docs/hardware-validation.md). **Zylia ZM-1 single-position localization** (`zylia.c`, unit-tested off-hardware via the `zylia`
test) is the one-placement complement to the multi-position omni survey: the 19-capsule sphere sees each sweep
arrive at 19 times, so the arrival-time DIFFERENCES give a speaker's DIRECTION from ONE spot (latency-free,
sub-degree — `zylia_doa`), and with the known latency a Gauss-Newton refine against the exact spherical
wavefront gives the full position (`zylia_localize`). Distance is latency-limited (the array is too small to
self-calibrate latency at meters). Spatial room capture (early-reflection DOA) is still DESIGN ONLY — nothing
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
off the sphere center, so the solve re-centers on the best-fit SPHERE each iteration. Coplanar claps (a
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
order). **Dante is the unlock for the sweep path**: the ZM-1 can join the Dante network via Dante Via, so the Digiface
presents its 19 capsules as INPUTS ON THE SAME ASIO DEVICE as the 26 outputs — one driver, one clock domain,
which dissolved the two-device problem that used to block `--zylia` on hardware (and makes latency measurable,
so DISTANCE becomes real). The sweep path is WIRED on that basis: `calib_capture` grew
`calib_asio_open_multi` (N outs + `nin` lockstep inputs; the omni modes are `nin = 1`), and `calibrate
--zylia` sweeps each speaker, deconvolves all 19 capsules, and feeds `zylia_localize` — with `--survey`
(room-axes; body-frame refused, no tracker here) pinning channel order + orientation, and the distance
latency from `--latency` (loopback) or `--ref <spk> <m>` (ONE tape-measured distance solves it from that
speaker's mean arrival, wavefront-tilt corrected; sim check: recovers an injected 3.5 m to ~1 mm). No
latency given → directions print, writeback refused (distances would carry the full system latency
radially). Like the omni sweep shell, the capture is rig bring-up code — unverified on hardware.
`zylia_survey` is capture-agnostic: it takes (source position, 19 arrival times) and does not care
whether they came from a clap's cross-correlation or a sweep's deconvolved IR peak. See
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

**Physical-emulation batch (2026-07-24):** six engine features filling out the propagation family,
all following the established per-voice template (enqueue-only command, audio-thread ramp).
`bwa_source_set_proximity` is loudness comp's near mirror (an LF shelf rising inside ~1 m, +6 dB
max at 300 Hz — the spherical-wavefront proximity effect; the geometric half was already
`bwa_set_near_spread`). `bwa_set_speed_of_sound` makes `c` a live atomic (RtCore.sos) that Doppler
and ISM delays derive from per block — both glide, so a medium change bends rather than steps;
delays saturate at ring capacity. `bwa_source_set_directivity`/`_set_orientation` now work in
EVERY build: with a Steam scene the sim publishes as before; without one the audio thread
evaluates the same weighted dipole per block from a control-side forward cache (engine.c
src_fwd/src_dirw/src_dirp), with a dedicated handle-gated readback publish (RtCore.dir_pub) so
`bwa_source_get_directivity` reports the manual path too. `bwa_fdn_set_decay` is the live FDN
retune: fdn_set_decay stages the three params atomically + bumps a seq; the tap re-derives per-line
loss-gain targets on a seq change and ramps g_lf/g_hf/xa across ONE block (the loss gains scale the
tail signal directly, so a raw step would step the output). `bwa_scene_set_ground` is the outdoor
degenerate of the box (one horizontal mirror in IsmRoom.plane_only mode, face slot 2; a big quad
feeds the ray tracer with SDK), and `bwa_scene_set_pressure_release` flags box faces whose ISM
coefficient NEGATES (water surface from below, R ~ -1 — the Lloyd's-mirror comb; rt.c's hf-damping
ratio needed fabsf since both bands flip). Tests: ism (polarity + plane geometry), fdn (live retune
slope change + tail continuity), rt_feature (proximity shelf — measured with the limiter disabled,
a full-scale sine at 0.25 m rides the -1 dBFS ceiling and eats the boost; manual-directivity
nulls; sos arrival times; ground-bounce delay + polarity inversion). docs/api.md gained a
"Recipes: physical emulation" how-to (underwater listener, Lloyd's mirror, doorway portal, wind,
slow motion, arm's length). Both bindings carry the batch (Unity externs + Engine/SourceBase
surface, Godot properties/methods).

**Review follow-up (2026-07-24):** four hardening fixes on the batch above. `rt_set_ism_room` now
publishes through a pose.h-style seqlock (RtCore.ism_room_sh/ism_seq; rt_render adopts a stable
copy at block start), making `bwa_scene_set_box`/`_set_ground`/`_set_pressure_release` LIVE-safe —
the Unity wrapper had already exposed them post-start (its engine starts in Awake, so scripts have
no pre-start window), which raced the old plain struct copy. `cave_both`'s monitor tap gets the
same output clamp as `render_binaural` (factored as engine_clamp2 — the headphone EQ can boost
past the limited level on that path too). Direct mode no longer seeds `dual_mix` at CMD_PLAY
(gcur_lo is never solved there; a dual-band toggle used to buy every play one block of LF dip).
The manual-directivity readback (dir_pub) clears when the pattern is disabled or the voice
destroyed, so `bwa_source_get_directivity` can't report a stale dipole gain.

**SPCAP tuning became runtime (2026-08-07):** `focus` and `density` were `#define`s in `spcap.c`
(12.0 / 2.0), so retuning the lobe meant a rebuild and the 12 was only ever right for the
26-speaker cave. `focus` now DERIVES from the array: `layout_derive_spcap_focus` (layout.c, beside
the `rolloff_r` derivation) takes the mean nearest-neighbor angle between speaker directions seen
from `ref` and picks the exponent that puts the lobe 6 dB down in energy there,
`n = ln(0.25)/ln((1+cos delta)/2)`, clamped 1..64. The cube grid measures 37.5 deg and derives
12.70, so the constant it replaces was about right for this array and wrong for a sparse one (a
6-speaker cross derives 2.0, a 12-speaker ring 20.0). `density` keeps a plain 2.0; nothing
measurable maps onto it. Neither is a layout-file field — the file is a geometry/calibration
artifact and nothing measures a lobe width, so they sit with the other live A/B knobs
(`bwa_set_near_spread`, `bwa_set_dual_band`) and persist nowhere.

`bwa_set_spcap_focus(e, focus, density)` is live, with `<= 0` on either argument meaning "revert
that one to its default". The sentinel resolves at SOLVE time (rt_render re-reads
`c->layout.spcap_focus` each block), so a later `rt_set_layout` can never strand a latched value.
The load-bearing part is the re-solve: focus changes the gain vector of every source including
motionless ones, and the two existing dirty-all sites write `v->dirty` from the CONTROL thread,
legal only because both document a stopped audio thread. A live setter cannot (invariant 3), so
RtCore gained `_Atomic uint32_t pan_gen` whose ONLY writer is the setter; each Voice carries a
plain `pan_gen`, the mixer loads the counter once per block and re-solves any voice whose stamp
lags. Release/acquire rather than relaxed: a voice that saw the new generation with the old focus
would stamp itself current and swallow the change. `rt_feature_test`'s new section is the
regression — a voice that never moves, in a scene whose listener never moves, must respond to the
knob.

Latent bug the change exposed: `powf(lobe, focus)` with a NEGATIVE base and a NON-INTEGER exponent
is NaN, and one NaN poisons the whole normalized gain vector. An antipodal speaker rounds `cosang`
a hair below -1, so `lobe` lands at about -5e-8. Invisible while focus was the integer 12; the
derived 12.70 turned every default-grid solve into NaN until `spcap.c` clamped the lobe at 0.

`bwa_spcap_focus_default(positions, n)` is the pure companion (same contract as
`bwa_panner_gains_batch`), so a tool can print what an array implies without an engine. Both GUI
tools drive the knobs: the playground's localization scene gained a panner combo plus focus and
density sliders and a "default" button, the layout tool's preview panel the same beside its `B`
panner A/B, each printing the derived number. Both `--tests` suites cover them.

**...and the scoring path followed (2026-08-07, ABI break, `BWA_VERSION` -> 0.11.0):**
`bwa_panner_gains_batch` gained `float focus, float density` before `out`, honoring the same `<= 0`
sentinel. One sentinel across the whole feature was the point: `<= 0` means "the default for THIS
array" at the setter, at `bwa_spcap_focus_default`, and now at the batch, where focus falls back to
`layout_derive_spcap_focus` on the CALLER's geometry rather than the default grid's. Both arguments
are inert for DBAP and VBAP.

Two floats on a general call rather than an options struct, because the sibling
`bwa_bed_gains_batch` already carries its decoder-specific knob (`max_re`) as a plain parameter, and
a struct would need its own answer to "what does zero mean" next to a sentinel that already has one.
The placement is trailing (before `out`) so the geometry/query arguments keep their positions; the
alternative worth naming is `(panner, focus, density, positions, ...)`, which would match
`bwa_bed_gains_batch`'s "mode then its knobs" front-loading.

The payoff is in `layout_tool`: `pv_focus`/`pv_density` now feed all three batch call sites (the X
score, the rE coverage overlay, the badness map), the same two globals the preview panel already
dialed by ear, so a knob change re-scores instead of only re-rendering. The sliders got a second
home on the analyze panel (where the overlays are actually visible; the preview panel hides them)
and both copies call `mark_score()`. Headless, `--score <file> focus=<n> density=<n>` sweeps it. The
new `viewer/spcap_focus_score` test is the regression, and it asserts the direction: raising focus
above the derived 12.7 must NARROW the Frank spread, lowering it must widen it, the overlay's
per-direction errors must move, and the DBAP/VBAP rows must be bit-identical whatever is passed.

The two pure tools-API calls also got their first direct unit test, `test_tools_api` (a new target,
not a `test_smoke` extension: it links `bwa_core` alongside the DLL so it can cross-check
`bwa_spcap_focus_default` against the internal `layout_derive_spcap_focus`, which the DLL does not
export). It pins the sentinel by construction: `focus <= 0` must reproduce the gains you get by
passing `bwa_spcap_focus_default`'s answer for the same array, bit for bit.

---

**Device backends, phases 0 and 1 (`docs/backends.md`).** Two shared pieces, then the WASAPI sink,
then the ABI that lets a caller name either.

`sink_convert.h` is `asio_sink.cpp`'s `convert_out`/`to_i32` moved out whole, plus interleaved
variants of each format, because ASIO is the only API in the plan that hands you planar per-channel
buffers. Nothing about the rules changed, deliberately including the one asymmetry: the i16 path
clamps the FLOAT and scales, so full negative scale is -32767 where the i32 path gives INT32_MIN.
Preserving that beat "fixing" it, since the difference is one LSB and the alternative is a silent
change to bytes that reach a live driver. The NaN rule is the load-bearing one and is now stated in
one place: NaN slips BOTH clamp compares, and float-to-int out of range is UB, which on speakers is
a full-scale pop rather than the silence a limiter-off render needs.

`sink_quant` is the fixed-quantum adapter, and it exists because ASIO is the ONLY backend whose
callback size is fixed. WASAPI shared mode hands out `bufferFrameCount - GetCurrentPadding()` per
event, which moves; with the SDK the headphone decode is built for one frame size and SILENCES any
other (`steam_decode.c`'s `n != m->frame_size` guard). A backend that forwarded the device's own
count would therefore produce silence on some machines and not others.

The design decision inside it was the FIFO shape. The obvious frame-indexed ring is wrong: render()
fills a planar bus whose channel stride IS nframes, so a block only lands in a frame-indexed ring
through a per-channel copy. A ring of block-sized SLOTS already has that stride, so the engine
renders straight into the FIFO and the backend converts straight out of it, no copy on either side.
The cost is that a device request can span slots, so `sink_quant_out_fn` takes a frame offset and is
called once per contiguous run. Pass-through (the device asked for exactly one block and the FIFO is
empty) then falls out as one run with no residue rather than needing a separate path; it is counted
(`h_passthrough`) so a backend that pins the device size can see it paid nothing.

Two rules the spec is right to be emphatic about, both now pinned by `test_sink_quant`. Each
rendered block's `sample_pos` is the device frame index its first sample lands on, not a block
counter. And two blocks rendered inside ONE device callback must not share `system_time_ns`: the
second gets the callback's host time plus the nominal duration of the frames queued ahead of it,
because a repeated host time feeds the device-versus-host drift fit a zero-slope pair, which is
worse than no pair at all.

The test's shape is worth keeping. The render writes a value derived from its own TIMESTAMP, and the
device side checks each frame against the value its own stream index implies, so one comparison pins
three claims at once (ordered concatenation, nothing duplicated or dropped, and the sample_pos rule)
and a wrong stamp corrupts audio rather than merely failing a stamp assertion, which is what a wrong
stamp does in the field. Per the CLAUDE.md trap the pop arithmetic was broken on purpose (the slot
retired one frame early) and the test went red on the concatenation AND the pass-through-versus-FIFO
bit-identity checks before being trusted green.

`wasapi_sink.cpp` closes a live defect: `cave_both` had never had a monitor on hardware, because the
ASIO SDK holds one current driver per process and the second open was refused. Under AUTO the array
takes ASIO and the 2-channel monitor request takes WASAPI, so the profile resolves on its own. The
monitor is now opened for the ARRAY's block rather than `cfg.block_size`, which is what makes the
"same buffer size" check pass when the driver picks its own size: the adapter can render exactly
that. Shared mode is the default because a VR runtime, a browser and the OS all keep audio open on
the same endpoint; `BWA_SINK_FLAG_EXCLUSIVE` opts out. A lost device (`AUDCLNT_E_DEVICE_INVALIDATED`,
or the event simply not firing) sets `health.device_lost` and the sink degrades to the null-sink loop
so clocks, playheads and scheduled plays keep advancing while the audio goes silent. Nothing reopens
on its own, matching the existing gap on ASIO's reset request.

The AUTO order is the one behavior change: a headphone profile on Windows now opens the Windows
default output instead of the first registered ASIO driver, which on a rig machine could be the
Digiface, playing the monitor into Dante channels 1 and 2. The array never chooses WASAPI. A `device`
string matches EXACTLY against the friendly name then the stable id, and under AUTO a backend with no
device by that name is skipped rather than opened on its default. This machine lists five render
endpoints, three of them named "Speakers (...)", which is exactly why there is no substring matching
and why the id is the string to persist.

ABI: one minor bump, 0.13 to 0.14. `bwa_desc` keeps its layout, with `asio_driver` becoming an
anonymous union alongside `device` (both spellings compile, and the C# and GDExtension field offsets
do not move) and `sink_flags` carved out of `reserved[0]`. `bwa_health` gains `device_lost`, which is
the layout change the bump covers. `bwa_get_sink_type` stops matching the backend STRING by prefix
and reads a `type` field the vtable now carries, a rule that could not have survived a second backend
whose name starts with the same letters.

**The shared-mode WASAPI dropout, settled.** The first cut left `measured = true` on a shared stream
whose dropout count was structurally zero, which is exactly the lie `measured` exists to prevent.
The queued-depth rule cannot serve shared mode: the request size is `bufferFrameCount -
GetCurrentPadding()`, which is precisely what the engine consumed since the last callback, so
`written` telescopes to follow the device position and the depth never goes negative however late
the render is.

The obvious replacement, `GetCurrentPadding() == 0` at a wake, does not work either, and it took a
live starve to show it. This client refills the buffer to FULL every event, and a starve leaves the
stream event already signalled, so the catch-up wake returns immediately and reports the frames just
written rather than an empty buffer. A render stalled past two full buffers produced zero
padding-is-zero wakes on a real endpoint.

What does work is the release interval, and it is a measurement rather than an inference: because
every event tops the buffer up to full, the device holds `bufferFrameCount` frames after each
`ReleaseBuffer` and runs dry exactly that many frames later. A release landing after that deadline
proves the device had nothing of ours in between, and the excess IS the silent frame count. The same
headroom argument the padding rule needed still applies and now decides `measured`: the buffer must
be deeper than one period, or a normal cycle sits on the deadline and jitter reads as a fault. On
this endpoint (1056-frame buffer, 480-frame period) the rule is armed; on a one-period endpoint
`measured` reports false, which is the honest answer.

The counters stay in `sink_quant` (`sink_quant_note_dropout`) rather than in the backend, so a
backend that detects a fault its own way still reports through one readback path and the two cannot
drift. `test_audio_sink` pins both halves: a healthy 200 ms run counts zero, then a one-shot
`Sleep` of two full buffers inside the render callback counts exactly one dropout of about 2860
frames. The deliberate-red check made the threshold one notch too strict (four buffers instead of
one) and the starve assertion went red before the rule was trusted.

---

**Device backends, phase 2 (`docs/backends.md`).** The OS shim: every platform call the engine made
outside the sinks moved behind `src/os/os.h`, with `src/os/os_win.c` and `src/os/os_posix.c` behind that, so
the library, the tests and the console examples build and pass with gcc and clang. The point is not
Linux audio (there is no backend there yet) but the three things that fall out of it: the offline
render path works anywhere, ThreadSanitizer becomes runnable, and CI gains a job that catches a
Win32 call sneaking back into the core.

Mechanically it is `Interlocked*` on `volatile LONG` becoming C11 atomics in the .c files,
`CreateThread`/`Sleep`/`QueryPerformanceCounter`/`CRITICAL_SECTION`/`_strdup`/`_stricmp` becoming
`os_*`, and Winsock becoming a wrapped UDP shim so `natnet.c` needs no socket header at all. Four
things the spec's inventory did not predict, each of which changed a decision:

The shim needed a READER/WRITER lock, not just a mutex. `steam_scene.c` guards its committed
`IPLScene` with an `SRWLOCK` taken shared by borrowing sim threads and exclusively around
`iplSceneCommit`; a plain mutex would have serialized ray traces that have no reason to wait on
each other, so `os_rwlock` exists beside `os_mutex`.

`pose.h` stopped being a header anything may include. The seqlock rewrite follows Boehm 2012: the
payload fields are relaxed atomics (a seqlock reader deliberately reads data a writer may be
writing, which on plain fields is undefined behavior rather than a stale value), and fences carry
the ordering — a seq_cst fence after the writer's odd store, an acquire fence before the reader's
validating reload. That puts `<stdatomic.h>` in the header, and with it MSVC's
`/experimental:c11atomics` for every consumer. `rt.h` and `natnet.h` therefore forward-declare
`PoseSlot`, which keeps the flag off about twenty translation units. Two knock-ons: `_Atomic` is
not C++, so `examples/validate.cpp` now reads the pose through a new `natnet_read_pose` and the
type is opaque to C++; and an atomic payload is not memcpy-able, so `rt.c`'s pre-seeding of the
readback slot went away (`rt_read_pose` already fell back to the active fields when nothing had
been published, which is exactly the case the seeding covered).

Path normalization turned out to be a Windows FACT, not a convenience. `assets.c` folded case and
backslashes into its cache key; on a POSIX filesystem `A.wav` and `a.wav` are different files and a
backslash is an ordinary character in a name, so folding either there hands one cache entry to two
files. Both folds are `_WIN32`-only now. The test section that pins them skips off Windows rather
than asserting the inverse, because under WSL the Windows drive is mounted case-insensitively and
the inverse assertion would pin the mount instead of the code.

And two CMake auto-detects needed a platform guard for one reason: a dev checkout has the SDKs
staged, so a Linux configure of that same tree found a Windows `phonon.h` and a Windows ASIO SDK
and then failed at link. ASIO now requires `WIN32` as well as the SDK, and Steam Audio requires the
platform's import library rather than just the header.

**The self-paced loops moved to an absolute deadline.** `os_sleep_until_ns` waits on a deadline on
the monotonic clock: a high-resolution waitable timer on Windows 10 1803+, `clock_nanosleep`
with `TIMER_ABSTIME` on Linux, `mach_wait_until` on Apple. Two problems, one fix. A relative sleep
is computed from a clock reading that is already stale when the kernel sees it, so pacing error
accumulates. And the old way of making a relative sleep precise on Windows was `timeBeginPeriod(1)`,
which is system-wide in effect — the null sink was holding the whole machine at a 1 ms timer tick
for as long as any visual-only tool had the offline sink open. Only the pre-1803 fallback still
touches it. Measured over 200 wakes 2 ms apart: median lateness 0.43 ms and p99 0.74 ms on a desk
Windows machine, 0.13 ms and 0.33 ms under WSL2, against 5.6 ms and 13.0 ms with the fallback
forced. Device-paced paths are untouched: the device is the clock there.

**What the new `test_os` does and does not pin.** Threads, sleep bounds, the monotonic clock rate
against the C clock, the deadline-sleep percentiles, mutex exclusion under two threads, and the
seqlock under a writer and a reader with torn-read detection. The seqlock section pins the
ALGORITHM, not the memory ordering, and says so: weakening the writer's release store to relaxed
and deleting the reader's acquire fence leaves it green on x86, because the hardware does not
reorder those. Removing the reader's validating reload turns it red at once — 122,837 torn reads in
a 400 ms run. The deadline section's p50 bound is the one that catches a real regression: losing
the high-resolution timer path degrades wakes to the 15.6 ms default granularity and fails it by an
order of magnitude, which was confirmed by forcing exactly that.

**Linux gets device backends (backends.md phase 5).** `jack_sink.c` is the production path and the
one sink that neither converts nor interleaves, because JACK ports are planar float already; it
activates and connects at OPEN so a "the server has 2 playback ports and this sink has 26" error can
reach `bwa_last_error`, and it parks xruns from JACK's notification thread for the process thread to
fold into the adapter, which keeps every count in one place and stays clean under ThreadSanitizer.
`alsa_sink.c` is the no-server path and the ONE backend with no fixed-quantum adapter: it writes, so
it picks the size. Two things the shim gained for it. `os_thread_set_realtime(period_ns)` is what
every self-paced render thread now calls, and the three platforms mean three different things by it
(MMCSS "Pro Audio", `SCHED_FIFO`, `THREAD_TIME_CONSTRAINT_POLICY`); the Windows effect was measured
rather than assumed, at p50 12 to 18 ms on an ordinary thread against 0.407 ms on the MMCSS one, on
a box pinned at 100 percent CPU by unrelated work. And the null sink stopped stamping
`os_monotonic_ns() - base`: on Apple Silicon's 41.67 ns tick the first block's stamp came out
exactly 0, which `rt.c` reads as "no stamp", and macOS CI caught it as `bwa_get_clock` having no
pair after 30 ms. It stamps the absolute clock now, like every other backend.

**Two deferred backend follow-ups, plus the defect the second one uncovered.** UTF-8 paths on
Windows: the ABI has always said UTF-8 and the Windows C runtime has always read a narrow path in
the ANSI codepage, so a path with an accent or a CJK character failed to open with nothing more
informative than "cannot open file". `os_fopen` (UTF-16 plus `_wfopen`) now carries every `fopen`
in `src/`, and the three dr_libs decoders take their `_w` openers; `sound.c` reaches one level
deeper than the public wide API goes, because dr_flac and dr_mp3 publish a wide OPEN but no wide
read-all, and it is the dr_libs implementation translation unit so the two private helpers are in
scope. `hrtf_path` is the one path left alone: phonon opens the SOFA file itself. Blocking waits:
the asset loader and the stream thread were the only two threads with no clock of their own, and
both sleep-polled. They wait on an `os_event` now. The loader waits forever; the stream thread
cannot, because its consumer is the audio thread and the audio thread may not signal, so it takes a
timeout derived from the ring depths, floored at 1 ms and capped at the old 3 ms. Idle wakes went
from 35 and 35 per 500 ms to 0 and 0 on a Windows desk box, and a streamed voice rendered at real
time went from 2 stream starves to 0. Writing the idle test taught the same lesson twice: an
absolute millisecond bound on the async-acquire latency passed alone and tripped once under the
full suite, so it is a comparison against a 2 ms sleep-poll timed in the same process now; and the
FIRST async acquire can never show the difference at all, because it is the call that starts the
loader thread, whose first pass finds the job already in the ring.

The `cave_both` monitor was inheriting `bwa_desc.device` and `sink_flags` from the array, which
re-created the exact defect the WASAPI sink was added to fix. On the rig `device` names the ASIO
driver; rule 10 skips a backend that has no device by that name, so the monitor's 2-channel AUTO
request skipped WASAPI, asked ASIO for a driver whose one process-wide slot the array already held,
and fell to the silent null sink. The profile had never had a live monitor. It opens AUTO with no
device now. The reason this survived every test is worth keeping: with no device string there is
nothing to inherit, so an offline suite cannot see it, and the arm that does see it needs a real
stereo device. The exception clause reads the REQUESTED sink rather than the resolved one, so an
AUTO array whose device is missing still gets a live monitor - silence on both is the worst answer
on the one configuration where hearing the sim is how you find out the array never opened. The same
work turned up `bwa_get_device_name` failing instead of truncating on WASAPI: `WideCharToMultiByte`
into a short buffer returns 0 and writes nothing, so a picker got an empty name rather than the
"truncated to cap-1" the header promises.

The AAudio sink (backends phase 3) cost two decisions the spec had left open, and both came from
the same place: AAudio tells you less than the other APIs and you have to stop pretending
otherwise. Dropouts are `getXRunCount`, which counts EVENTS and never their length, so
`dropped_frames` stays 0 by design and the test asserts it stays 0 rather than accepting a
plausible estimate from the callback interval. And a shared-mode stream opened at the engine rate
reports the engine rate whether or not the service is resampling underneath it, so rule 6's
"succeeded but degraded" channel had nothing to say until the sink started PROBING the device rate
with a throwaway stream opened with everything unspecified. Both the emulator verification and the
deliberate-red check landed: breaking the xrun fold on purpose turned the injected-stall assertion
red before it was trusted green. What the emulator could not reach is on the verify list, and the
honest headline there is that `test_os` is FLAKY on an emulator - six of ten runs pass, and the
two that trip are its tightest timing bounds (p50 sleep lateness straddling 1 ms at 0.89 to 1.11,
and a "past deadline returns at once" check that allows 1 ms between two clock reads), with
SCHED_FIFO refused as Android refuses it for every app thread. The bounds were left alone rather
than widened to fit a virtual machine nobody will ship on.

Static phonon for Linux and macOS cost three fixes rather than the zero the "just call the
composite action" plan assumed, and all three were silent. The action's `-t make` was an argparse
ERROR, because both pinned scripts declare `--toolchain` with the five `vs20xx` spellings and
nothing else. `LINKER_LANGUAGE CXX` on a project whose `project()` says `LANGUAGES C` does not
error either: CMake emits an EMPTY link rule, so the build printed "Linking CXX shared library
libbw_audio.so", exited 0, wrote no library, and every test still passed because the tests link
`bwa_core`'s objects rather than the library. And `-fvisibility=hidden` does not contain a static
phonon, because `IPLAPI` is `visibility("default")` inside phonon's own objects: 561 dynamic
symbols came out of `libbw_audio.so`, 216 of them `ipl*`, until `-Wl,--exclude-libs,ALL` brought it
back to the 173 `bwa_*` the ABI declares. The AVX trap also turned out to be the COMPILER and not
the distribution: `-fabi-version=6` breaks `<future>` in `hrtf.cpp` on gcc 11.5 and 13.3 and
compiles on 14.3, so CI (gcc 13) passes `-DSTEAMAUDIO_ENABLE_AVX=OFF` and a modern desk box does
not have to.
