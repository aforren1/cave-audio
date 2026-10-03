# Implementation log (historical)

This is the per-feature narration that used to live in CLAUDE.md's "Current state"
section. CLAUDE.md now carries a short "Current state" plus a "Traps" list, and
`docs/api.md` owns the feature catalog. This file is a record only — nothing here is
guidance, and git history is authoritative. Kept so the detail stops loading into every
agent session while remaining findable.

---

**Engine SDK: one engine per platform (2026-09-16).** Every binding used to link the in-tree
`bw_audio` target, so a desktop CI job compiled the engine FOUR times - once for its ctest run,
once per Godot extension flavour, and once more inside the wheel build's isolated scikit-build-core
tree - and the `wheels` job compiled a fifth and sixth. Four of those were shipped as if they were
the same file. The fix is an install tree (`install(TARGETS ... EXPORT)` + a generated
`bw_audioConfig.cmake`, component `bwa_sdk`) and ONE switch at the root, `-DBWA_ENGINE_SDK=<dir>`,
which makes the root compile nothing under src/ and import `bwa::bw_audio` instead. In-tree that
name is an ALIAS, so the four binding CMakeLists changed by one word each and take the engine file
from `$<TARGET_FILE:bwa::bw_audio>`.

Three things were learned doing it. The first is that a build tree carries install rules it does
not own: the first unfiltered `cmake --install` produced an SDK containing raylib's headers and
import library and the whole Python wheel payload, and the mirror image is worse, because
scikit-build-core installs every component by default and a wheel would have grown a second engine
at `bin/`. Hence `bwa_sdk` / `bwa_python` and `install.components` in pyproject. The second is that
an import library and its DLL are siblings in a BUILD tree and not in an INSTALL tree, which broke
the Octave MEX staging (`mkoctfile_mex.cmake` derived the DLL from the .lib's directory); the
runtime path is passed explicitly now. The third is about the import-time ABI guard the Python
package gained to match `+bwa/setup.m`'s: the first version formatted its message from
`VERSION_MAJOR/MINOR/PATCH` while comparing `VERSION`, and a deliberately broken run printed
"compiled against 0.15.0 ... reports 0.15.0" - a real mismatch that reads as a broken check. Both
sides of the message are decoded from the two numbers the condition compared now. The guard was
broken on purpose, seen red, and restored.

LINUX was the interesting decision. A `manylinux_2_28` wheel needs an engine built against glibc
2.28 and `ubuntu-latest` carries 2.39, so the two Linux engines were not redundant, they were
different - and the one a stranger pip-installed was the one nothing had tested. The engine build
and its ctest moved INTO the manylinux image (`tools/ci/build-engine-manylinux.sh`, a `docker run`
with the workspace mounted, reusing the before-all script cibuildwheel already had for its dnf
packages and its phonon). Only the engine step is containerized: MATLAB does not install in
AlmaLinux 8 and the Godot toolchain is the runner's, and neither needs to, because a binding
compiled on the runner against a glibc-2.28 shared object is an ordinary ABI client. The container
runs as root, so everything it writes is root-owned - which costs nothing only because the
bindings now configure their own trees and never write into the engine's. The `wheels` job then
builds no engine and no phonon at all, which is what pays for its new `needs: [linux, macos]`.
macOS needed one matching change: the engine is built with `CMAKE_OSX_DEPLOYMENT_TARGET=10.13`
now, because the universal2 wheel DECLARES 10.13 and a dylib built for the runner's own macOS
would install anywhere and load nowhere older.

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
The Pythonic layer then took on ONE semantic of its own (2026-09-16): it commits for you, because Python has no frame to hang the per-frame `bwa_commit` on the way Unity and Godot do, with `Engine.frame()` as the coherence path, a flush before every play so "set the position, play it" cannot render at the old one, and `autocommit=False` to get the C semantics back.

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

The manylinux wheel cost one factoring and one guard. The composite action's shell body became
`tools/phonon/build-phonon.sh` because a composite action runs on the RUNNER and a wheel's phonon
has to come from the container that links it, so the recipe needed two callers and could not stay
inside a YAML `run:`. The guard is the stamp beside the staged archives: cibuildwheel copies the
whole project directory into the container, a developer's own `lib/linux-x64` rides along, static
archives link happily whatever compiler made them, and the result would be a manylinux wheel with
a phonon built against a newer libstdc++ than the policy allows. The stamp names the image and the
compiler major version, a host staging has none, and the mismatch deletes the directory rather
than reusing it. Verified locally: the run on a Windows host through Docker Desktop discarded the
gcc 14.3 WSL archives, rebuilt phonon in the image in 232 s, and produced
`bw_audio-0.14.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl` with 55 pytest tests
and both examples passing inside the container. Two small facts worth keeping: auditwheel names
every tag a wheel is eligible for, so the filename carries `manylinux_2_27` as well and an equality
assertion on the name would have failed; and with `libasound` and `libjack` excluded the repair
grafts nothing at all, which is what leaves the extension's `$ORIGIN` runpath untouched.

The Linux backends stopped linking their libraries. `libbw_audio.so` had `libjack.so.0` and
`libasound.so.2` in its `NEEDED` list, which made a box with neither fail to load the WHOLE library
rather than lose one backend, so `jack_sink.c` and `alsa_sink.c` now `dlopen` them through a new
`os_dl_open`/`_sym`/`_close` trio in the shim and resolve 23 and 36 symbols into one table each. A
library that will not load is exactly "no device": a count of 0, an AUTO skip, and an explicit open
that fails naming the library, with the reason carried out on the degradation channel when AUTO
lands on the offline sink. Three things worth keeping. The ALSA `snd_pcm_*_params_alloca` macros
expand to a `snd_*_sizeof()` CALL and `snd_strerror` is a library symbol too, so neither could be
found by reading the call sites for library-looking names; what catches them is the empty link
line, where a missed symbol is an undefined reference at build time. The fall-through is cheap:
about 3 us per absent library, against the 3.7 ms an explicit JACK open costs when libjack IS
present and has to find out there is no server. And the whole path is testable on a box that HAS
the libraries, through a `bwa_sink_dl_override` hook that re-aims both loaders at a soname that
cannot exist; breaking the retry on purpose (caching the outcome forever) turns that section red.

**A direct host-clock read (`bwa_host_time_ns`, ABI 0.14 to 0.15, 2026-09-16).** Every backend
stamps `bwa_timestamp.system_time_ns` on the OS monotonic clock, but nothing let a CLIENT read that
clock, so mapping a caller's wall time onto the dsp clock meant estimating the epoch offset from the
block stamps: the decaying max in `Engine.DspTimeFramesAt`, `api.md`'s `clock_refresh`, and
`live_onset.py`'s inline bridge. That estimator works and stays as the fallback, but it has a
convergence period and no error bound. One engine-free counter read turns the offset into a
measurement: `a = my_clock(); h = bwa_host_time_ns(); b = my_clock()` gives `h - (a+b)/2` with the
error bounded by `(b-a)/2`, which the caller can SEE. Measured on this Windows box through nanobind
it is about 0.1 us, against the estimator's unbounded, unknowable error. The survey that had to
come first: WASAPI, ALSA, AAudio and the null sink read `os_monotonic_ns` (or the same
`CLOCK_MONOTONIC` through the device API), ASIO takes the driver's `ASIOTime.systemTime`, which on
Windows is the high-resolution counter and falls back to QPC when the driver omits it, and JACK
takes `jack_get_cycle_times`, which is `CLOCK_MONOTONIC` microseconds through the server's DLL
filter. Same clock everywhere, so the offset holds on all five. The manual sink is the one
exception and always was: its stamps are synthesized from the sample position, so they are exact
for arithmetic and unrelated to wall time. The Python side is `Engine.bridge() -> ClockBridge`
(`offset_ns`, `error_ns`, `dsp_at`, `time_at`, `play_at`, `drift_ppm`), which is what PsychoPy
wants: its `core.getTime` is `perf_counter`, which IS this clock on every platform. The trap worth
carrying is in the table that went into `api.md`: `time.monotonic` on Windows is `GetTickCount64`
at about 15.6 ms, and Psychtoolbox `GetSecs` on Linux is `CLOCK_REALTIME`, which NTP steps. Neither
is wrong to use, both just need re-measuring at a different rate, and the binding does not police
the caller's clock.

**A MATLAB and Octave binding (a MEX gateway, 2026-09-16).** `bindings/matlab/` closes the last
binding `docs/backends.md` had open, for Psychtoolbox. One `bwa_mex` gateway dispatching on a
subcommand string, which is PsychPortAudio's own shape, plus a `+bwa` class layer carrying the same
auto-commit model the Python layer has. Three findings are worth the line. The classic C MEX API
pin is what makes ONE source serve both interpreters, but they still need different binaries, so
the build stages them apart and `+bwa/setup.m` picks at run time. Octave refcounts handle objects
and cannot collect a REFERENCE CYCLE, so a stored `Engine.listener` pointing back at the engine
made `clear e` leak the device until the interpreter exited; it is a dependent property now, and
the suite's own mexLock-based leak check is what caught it. And MinGW's `ld` as shipped with Octave
10.1 for Windows hits an internal error ("aborting at ldlang.c:527 in compare_section") linking this
object with no sort order requested, which `-Wl,--sort-section=name` fixes outright; the same
mkoctfile needs `-Wl,-rpath,\$ORIGIN` escaped on Linux, because it hands its link line to a shell
that would otherwise expand it to an empty runpath.

---

## Moved out of the user docs (2026-10-03)

Development history, war stories and dated run logs removed from `README.md`, `docs/*.md` and
`bindings/*/README.md` in the 2026-10-03 accuracy and voice pass. Each doc now states the
present-tense rule; the story is kept here. Grouped by the doc it came from.

---

**Streaming thread wait cap is the old flat cadence (from docs/concurrency.md).** The streaming thread waits with a timeout: the shortest time any fillable ring could drain to empty at the engine rate, floored at 1 ms and capped at 3 ms. The cap is the flat cadence it used to sleep on, so an active stream is never served later than it was before the refill loop became an event wait. (The doc now states only the present rule: capped at 3 ms, `STREAM_POLL_MS`.)

---

**limiter_ceiling went from dB to linear (0.4.0; from docs/api.md).** `limiter_ceiling` changed from dB to linear in 0.4.0, and only its parameter name records it. This is the case behind the rule that a decibel value must say `_db`.

---

**Configuration env vars removed (from docs/api.md).** The output device (`bwa_desc.sink`/`device`), Embree (`bwa_desc.embree`), pathing (`bwa_desc.enable_pathing`), reflection baking (`bwa_reflections_desc.bake`) and the OptiTrack connection (`bwa_tracker_connect`) all used to be `BWA_*` environment variables. The engine reads none now.

---

**AUTO order change for headphone profiles (from docs/api.md).** A headphone profile now opens the Windows default output. That was the one behavior change from earlier versions: the old order picked the first registered ASIO driver, which on a rig machine could be the Digiface, and played the monitor into Dante channels 1 and 2. With WASAPI in the order, `cave_both` finally opens both devices at once (array on ASIO, monitor on WASAPI). The ASIO SDK holds one driver per process, which is why it could not before.

---

**Testing the async held window (from docs/api.md).** A test that means to cover the held window has a problem: a small asset usually decodes before the first render, so the play binds at once, the held path never runs, and the test passes anyway.

Ordering solves it, without any test hook. A finished decode is adopted only at a pump point: `bwa_sound_acquire`, `bwa_sound_is_ready`, `bwa_sound_find`, `bwa_sound_release`, and `bwa_commit`. `bwa_source_play` is not one. So a play issued straight after `bwa_sound_acquire_async`, with nothing pumping in between, is guaranteed to be held. Check it with `bwa_sound_get_channels`, which reads 0 while the slot is still reserved and does not pump.

Slipping a readiness check in before the play is what breaks this, and it breaks it silently: the decode gets adopted and the case turns into an ordinary bind while every assertion still passes. `test/assets_test.c` does it in the guaranteed order and says so.

Cancelling an in-flight load cannot be pinned down this way, because `bwa_sound_release` pumps on entry. Both outcomes are safe, so that test covers the behavior without being able to claim which branch ran.

---

**max-rE default and the rig trial (from docs/api.md).** max-rE has been ON by default since the offline bed sweep, which had the taper winning every axis under both decoders and both observer models. The rig trial confirms rather than gates; if it disagrees, revert the default.

---

**Box and custom mesh used to be alternatives (from docs/api.md).** Before `bwa_scene_set_ism_room` and `bwa_box_mesh`, `bwa_scene_set_box` replaced the static mesh, so the box and your own occluders were alternatives rather than layers. The workaround then was: call `_set_box` first (nothing else captured the ISM room), then ONE `_set_mesh_mat` carrying the box's 12 inward-facing triangles alongside your own geometry.

---

**Source desc: what it added (from docs/api.md).** `bwa_source_get_desc` let you print what a source is set to, which nothing could do before (only occlusion and directivity had getters, reporting the sim's live value). The preset also gave a source a reset, which the API had no way to express.

---

**peak_load windowing (0.16.0, 2026-09-23; from docs/api.md).** Up to 0.16.0 `bwa_health.peak_load` was a since-start maximum. It is now the worst block over the last 4 to 5 s of stream time; the field and the struct layout did not change, so no ABI bump.

---

**Default grid and capacity were once the same number (from docs/api.md).** `BWA_DEFAULT_GRID` (26) and the channel capacity were the same number before the capacity rose to 64, which is why the docs warn not to use one where you mean the other.

---

**Failed-layout start refusal (from docs/api.md).** "The old sharp edge here is now fenced": a failed explicit layout load used to leave the engine silently rendering the default grid's 26 channels. `bwa_start` now refuses it with `BWA_ERR_LAYOUT`.

---

**Tuning preset count asserted in smoke (from docs/api.md).** Seated and roaming differ in exactly three fields, and the `smoke` test asserts the count so a fourth opinion cannot arrive unannounced.

---

**FDN made reverb SDK-free (from docs/api.md).** The directional FDN meant the reverb path no longer required the Steam Audio SDK.

---

**The backend problems as first stated (from docs/backends.md, "Why").** Three problems motivated the backend work, in order of pain. Headphones on Windows needed an ASIO driver: the `binaural` and `cave_sim` profiles opened a 2-channel ASIO device, so a desk machine without a pro interface ran ASIO4ALL or FlexASIO. `cave_both` could not open its second device: the ASIO SDK holds one current driver per process, the sink refuses a second open (the `g_sink` check at the top of `bwa_asio_sink_open`), and `bwa_start` opened the monitor through the same path with the same driver name, so under `BWA_SINK_AUTO` the monitor fell to the silent null sink and under `BWA_SINK_ASIO` the start failed. The profile had never had a live monitor on hardware. And the engine was Windows-only, so a collaborator on a Mac could not run the headphone profiles and a standalone headset could not run the engine. The ABI for the backends (appended `bwa_sink_type` values, `bwa_desc.device` and `sink_flags`, the device query, `bwa_health.device_lost`) landed in one minor bump, `BWA_VERSION_MINOR` 13 to 14 (0.14). `bwa_get_sink_type` used to match `"asio"` by prefix on the backend string; the vtable gained `bwa_sink_type type` to replace it, and `bwa_sink_open` changed from `(sink_type, asio_driver)` to `(sink_type, device, flags, exact_rate)`. The ASIO sink's `convert_out` and `to_i32` moved out to `sink_convert.h` and gained interleaved variants.

---

**The bindings were once future work (from docs/backends.md, "Experiments from Psychtoolbox or PsychoPy").** The Python nanobind extension and the MATLAB/Octave classic C MEX were both described in backends.md as future work before they existed. The offline experiment shape (manual sink render, PsychPortAudio playback) was stated as "works on Windows today and on Linux after phase 2".

---

**AudioWorklet `os_monotonic_ns` read stack garbage (2026-09-23; from docs/backends.md, rule 3 table).** `AudioWorkletGlobalScope` has no `performance`, so Emscripten's `CLOCK_MONOTONIC` returns `ENOSYS` on the audio thread. Until 2026-09-23 `os_monotonic_ns` ignored that and returned uninitialized stack there (measured: one constant, so render times read 0 and the first one read a whole second). It now falls back to `emscripten_get_now`.

---

**Phase 2 OS shim: what the inventory did not predict (from docs/backends.md, the OS shim section).** Six things the Win32 inventory did not predict. Four were found while implementing phase 2, and two more came out of the macOS CI runner after it.

- `SRWLOCK` is not `CRITICAL_SECTION`: `steam_scene.c` guards the committed `IPLScene` with a reader/writer lock, so the shim gained `os_rwlock` beside `os_mutex`.
- `pose.h` stopped being a C++-visible header when its payload fields became `_Atomic`. `examples/validate.cpp` reached the seqlock through `natnet.h`, so `natnet.h` gained `natnet_read_pose`.
- `bwa_null_sink_skip_blocks` stayed a plain `volatile int` because `sink.h` is included by the C++ sinks.
- Path normalization was a Windows fact: `assets.c` folded case and backslashes into the cache key; both folds became `_WIN32`-only.
- Priority is not one call with three spellings. The WASAPI sink used to ask for MMCSS "Pro Audio" inline; `os_thread_set_realtime(period_ns)` replaced it. On Apple, Darwin coalesces timers for threads without `THREAD_TIME_CONSTRAINT_POLICY`: the macOS CI runner measured p50 2.749 ms and p99 9.759 ms against a 2 ms deadline, and `os_sleep_ms(50)` taking 100.7 ms, with the timebase conversions verified correct in both directions. The Windows effect was measured too: on a desk machine pinned at 100 percent CPU by unrelated work, the same 200-wake measurement gave p50 12 to 18 ms on an ordinary thread and p50 0.407 ms with p99 0.811 ms on the MMCSS thread.
- The null sink stamped its own epoch, and a zero stamp reads as no stamp. It used `os_monotonic_ns() - base`, with `base` captured a few instructions earlier on the same thread; every other backend stamps the platform clock. On Apple Silicon (`mach_absolute_time` at 41.67 ns) the FIRST block's stamp came out exactly 0, which `rt.c`'s publish gate reads as "no stamp": `bwa_get_clock` had nothing until block 1, and a late wake pushed that past the 30 ms window `test_smoke` allows. Windows and Linux avoided it by clock resolution, not by design.

The migration also moved `test/` and `examples/` off their own `Sleep`, `GetTickCount64` and `<windows.h>` (13 test files, 14 examples): around 300 lines of shim plus a mechanical pass over about 40 files. `test_idle`: putting the loader and stream-thread sleeps back measured 35 and 35 loop iterations in 500 ms on a Windows desk box (bound: under 5), and more on a machine whose timer resolution something else has raised. The null sink used to call `timeBeginPeriod(1)`, holding the whole machine at a 1 ms timer tick for as long as a visual-only tool had the offline sink open.

---

**Phase 2 CMake changes (from docs/backends.md, "CMake").** Phase 2 removed the `if(NOT WIN32) message(WARNING ...)` in favor of a status line, moved `ws2_32` and `winmm` behind one `bwa_link_os` helper, and gave two auto-detects a platform guard (`BWA_WITH_ASIO` requires `WIN32`; Steam Audio requires the platform's own `lib/<platform>/`). The `/experimental:c11atomics` list grew with the files that became atomic.

---

**Steam Audio CI on Linux and macOS: three fixes (from docs/backends.md, "CMake").** The follow-up that built phonon in CI for Linux and macOS turned out as small as predicted, because phonon links statically, but it cost three fixes. `-t` is a Windows-only argument in both pinned build scripts, so the composite action's `make` default was an argparse error rather than a no-op. `enable_language(CXX)` had to move into the phonon block: `project()` declares `LANGUAGES C` and the other `enable_language(CXX)` calls sat under Windows-only features, so a Linux configure set `LINKER_LANGUAGE CXX` on a project with no CXX link rule; CMake emitted an EMPTY link rule, the build reported "Linking CXX shared library libbw_audio.so", exited 0 and wrote no library, and every test still passed. `-Wl,--exclude-libs,ALL` went on the library because phonon's `IPLAPI` re-exported every `ipl*` entry point despite `-fvisibility=hidden`.

---

**Worklet host-thread quiet window flapped under a deep buffer (from docs/backends.md, "Wasm Audio Worklet").** The old fixed quiet window of eight blocks is only 43 ms at 256 frames. When the XR page moved to a 0.05 latencyHint, the browser rendered quanta in 50 ms bursts, the heartbeat went quiet inside every burst gap, the host thread took over, `device_lost` flapped, and the two pullers alternated on one adapter (`run-xr` caught it at block 128). The window became `max(8 blocks, 3 x baseLatency)`; putting the fixed eight back turns that check red again.

---

**Render ahead tried and rejected for the worklet sink (2026-09-24; from docs/backends.md, "Wasm Audio Worklet").** Rendering a 256-frame block inside a 128-frame `process()` puts 5.3 ms of work inside a 2.7 ms deadline, and a Galaxy XR headset chopped. On 2026-09-24 the obvious fix was built, tested and measured: a render thread (a pthread Worker) filling a ring of blocks while `process()` only copied, then a hybrid in which `process()` rendered a rescue block when the ring ran dry and the render thread was idle. It lost, for one reason: a Worker has NO priority, and the AudioWorklet thread is the one thread a page owns that the OS treats as audio. On a busy laptop (a 12-core hybrid i7-1360P running several CPU-bound jobs) the render thread was descheduled mid-render for 40 to 200 ms, and it held the non-reentrant render at exactly the moments a rescue would have helped (14 rescues against about 380 starves over eight hybrid runs). Paired runs, builds alternated at one pinned load, 3 s each:

| sources (mean block) | render in `process()`: device underrun | render ahead alone: ring starve | hybrid (rescue): ring starve |
|----------------------|----------------------------------------|---------------------------------|------------------------------|
| 64 (2.2 to 3.7 ms)   | 0, 0, 0, 0 s                           | 0.13, 0.03, 0, 0.03 s           | 0.20, 0, 0, 0.11 s           |
| 96 (3.1 to 5.3 ms)   | 0.18, 0.30 s                           | 0.31, 0.10 s                    | 0.11, 0.59 s                 |

Render ahead never beat rendering in `process()` at any load. The slack an overrun needs has to come from the BROWSER'S output buffer, which the priority thread already owns, and that is what `BWA_SINK_FLAG_DEEP_BUFFER` asks for.

---

**Worklet main-thread audit (2026-09-24; from docs/backends.md, "Wasm Audio Worklet").** "Nothing on the audio path waits on the main thread" was audited on 2026-09-24 after a headset froze with its window (cause still unknown).

---

**Android packaging and phonon landed in steps (from docs/backends.md, "AAudio").** Phase 2's shim covered the code, the NDK toolchain wiring landed with phase 3, the packaging with the follow-up, and phonon for Android after that. The phase table still said "done, except phonon" after phonon landed.

---

**Phase ordering (from docs/backends.md, "Phases").** Phase 1 went first because it fixed a live defect, removed a dependency for every desk user, and is the PC VR path. Phase 2 was independent of it and could run in parallel; it is mechanical and touches many files, so it wanted its own review. Phases 3 to 5 depend only on phase 2 and not on each other, so their order was a scheduling choice. Phase 5 went first of the three, because a Linux box with both a JACK server and an ALSA card was the hardware at hand, and phase 3 second, because an Android emulator is a download rather than hardware.

---

**Original "What to update when implementing" checklist (from docs/backends.md; all done as of 2026-10-03).** `include/bw_audio.h` (the enum, the desc union and `sink_flags`, the device query, `bwa_health`, the `BWA_VERSION_MINOR` bump, the `bwa_desc.block_size` comment "the device period hint"); `docs/api.md` (desc table, sink policy, "Device query", profile table wording, health); `docs/build.md` (Platform, options, libjack and alsa-lib, CI, Linux notes); `docs/architecture.md` ("ASIO for the array on Windows; any system backend for a stereo monitor"); `CLAUDE.md` (repo layout, the `*_sink` trap); `docs/integration.md`; `docs/hardware-validation.md` (desk day); `bindings/unity/Runtime/Bwa.cs`, `Engine.cs`, `Editor/EngineEditor.cs` (the `"asio:"` prefix check); `bindings/godot/src/bwa_engine_node.{h,cpp}` (the `Sink` enum, `asio_driver` -> `device`); `third_party/README.md` (sink selection); `examples/` (`playground.cpp` and `layout_tool.cpp` `asio` prefix checks, the ASIO-only picker and `--list-drivers`, `--driver` and ASIO4ALL wording in `minimal.c`); `NOTES.md` (the `engine.c` "WASAPI is future" comment). The deferred follow-up "the examples' backend awareness" pointed at the examples item and is also done (`playground.cpp` enumerates through `bwa_get_device_count`).

---

**Backend test history (from docs/backends.md, "Tests").** `test_sink_convert` and `test_sink_quant` shipped with phase 0, with the pop arithmetic broken on purpose and confirmed red before being trusted green. `test_os` shipped with phase 2. The WASAPI section of `test_audio_sink` shipped with phase 1, the JACK and ALSA sections with phase 5, and the AAudio section with phase 3. The WASAPI EXCLUSIVE subsection's queued-depth rule was the first live exercise of that rule; every earlier live dropout came through the shared-mode release-interval rule or the null sink. A 36 ms stall ("two buffers plus slack") tripped the exclusive depth rule at one block size out of three, which is why the stall is 150 ms. The EXACT_RATE subsection's first draft asked the strict open first, and a build with the fold removed came out SKIP rather than red. At the time of writing, a default no-SDK, no-tool Android build had 33 tests (32 passing every run on an emulator, `test_os` flaky), and the Linux and macOS CI jobs ran 38 tests (33 off-Windows defaults plus 5 SDK-gated; Windows had 45; a Linux tree with `BWA_BUILD_PLAYGROUND=ON` had 40 with phonon and 35 without, measured on Ubuntu 22.04, gcc 11.4, software GL). Current counts: CLAUDE.md, or `ctest -N` in the build tree.

---

**Verify-list history (from docs/backends.md, "Verify before relying on it").** WASAPI exclusive mode had been written and compiled and never opened until `test_audio_sink`'s exclusive subsection. Static phonon for Linux: 38 of 38 under WSL (NixOS, gcc 14.3.0). Static phonon for Android: 37 of 38 every run on an x86_64 API 34 emulator (the red was `os`). Static phonon for macOS: the job's `stage-dir: osx-universal` was first read off phonon's `core/CMakeLists.txt`, and that reading was wrong: its `set(CMAKE_OSX_ARCHITECTURES ...)` comes after `project()` and does nothing, so the first CI run staged an arm64-only core archive and the universal engine link failed. Two open items were removed as contradicted by the `[x]` exclusive-mode entry: "Exclusive mode on a real interface ... has only ever run in shared mode", and "The EXCLUSIVE-mode depth rule against a deliberately starved stream ... has never been run at all". The second carried the shared-mode measurement: a render stalled past two full buffers on a 1056-frame buffer with a 480-frame period counted exactly one dropout of about 2860 frames, and a healthy run counted none.

---

**Engine SDK and CI history (from docs/build.md).** Each binding used to pull the engine in as a source dependency, so a job that built the engine, two Godot extension flavors and a wheel compiled the engine four times and shipped four files that were only meant to be the same. Before the manylinux engine step, the repository built two Linux engines: one on the runner for ctest, the MEX pair and the Godot addon, and a second inside cibuildwheel for the release wheel. `bw_audio.dll` used to need a `phonon.dll` beside it before phonon was linked statically. The `wheels` job used to leave Windows out because a Windows wheel already names one ABI and one architecture; that was beside the point, since what one wheel could not cover was the interpreter. The binding tests used to be in the Linux and macOS ctest count.

---

**Release version merge (from docs/build.md, "Releasing").** Before 0.16.0 the header carried its own ABI counter (0.15.0 at the last such release) beside the tag (v0.7.0), and a wheel's version could not be related to the release it came from. The two were merged by continuing the header's sequence, which is why the tags jump from v0.7.0 to v0.16.0.

---

**Null sink timer resolution (from docs/profiling.md).** The null sink used to bracket its render loop with `timeBeginPeriod(1)`, which is system-wide in effect, so any tool that happened to open the offline sink held the whole machine at a 1 ms timer tick for as long as it ran.

---

**Sweeps used to be single and unwindowed (from docs/calibration.md, "Sweep quality").** Each speaker used to be swept once, and the arrival was the strongest tap anywhere in the IR. One loud event during a sweep could move a level, an EQ band, or, rarely, the arrival, and nothing said so. The expected-arrival window, the SNR floor and the re-sweep replaced that.

---

**Collaborators' over-screen elevation signature (from docs/calibration.md, "What the screens do").** The collaborators' 24-speaker array has the over-screen versus through-screen split and measured that signature, a median +12° of upward elevation displacement, which the paper attributes to speaker density. Both mechanisms (speaker density and over-screen speakers running hot) push the same way, so their data cannot separate them.

---

**The ZM-1 on Dante was the unblock, not the DSP (from docs/calibration.md, "Getting the ZM-1 onto Dante").** The obstacle to `--zylia` on hardware was never the DSP: the sweep plays out of the Digiface and the ZM-1 is a different USB device, and the ASIO SDK has one process-wide current-driver slot. Two drivers at once is not a flag, it's a rewrite. Putting the ZM-1 on the Dante network "dissolved" the problem: `calib_capture.cpp` went from "N outs + 1 mic input" to "N outs + 19 inputs", a parameter, not an architecture (`calib_asio_open_multi`).

---

**Collaborators' AES 161 handedness reflection (from docs/calibration.md, "The capsule geometry").** The collaborators' AES 161 measurements fit exactly the mirrored-capsule-numbering case: a fixed azimuth-handedness reflection present on every recording day, and it dominated their mount fit.

---

**ASIO systemTime on the development box (from docs/calibration.md, "Clap positions from a tracked clicker").** `systemTime` (documented as `timeGetTime`-derived) ran 27 ms off QPC on the development box. The onset path's fallback estimate (snapshot frame minus post-roll) was called "the old estimate" in the doc.

---

**Wrong clicker tip offset, full simulation numbers (from docs/calibration.md, "Clap positions from a tracked clicker").** In simulation, a tip offset left at 0 on a clicker whose tip is 11.6 cm out put every clap 11.6 cm off, in a different direction each time. The survey still solved: residual 3.2 µs against 0.035 µs, capsules off by up to 3.2 mm against 0.04 mm.

---

**`calib_view --tests clicker`, the seven checks (from docs/calibration.md, "Clap positions from a tracked clicker").** With the stand tracked, every banked clap is the clicker's true tip against the stand's measured center (not either typed field) and the survey recovers the capsule table; a tip left at 0 makes the residual and the geometry measurably worse; the ring is refused; an interferer is refused by the arm window or the direction check; one that slips past both is flagged by leave-one-out and dropped; with the 80 ms stall, every still click banks at its true tip when judged at its stamp; and judged at the old estimate instead, every click that is followed by a move is refused. `--tests onset_stamp_math` feeds the callback's arithmetic and its clock check the three bases a driver can stamp on.

---

**Clap interferer rehearsal, test detail (from docs/calibration.md, "Which transient is the clap?").** `--tests zylia` checks that both simulated interferers (one right after the second click, one timed like the eighth) are refused and the geometry lands within 0.2 mm of the capsule table the claps came from, as a clean run's does. It also checks one timed like the third click, before any provisional survey: it banks, leave-one-out flags it, and dropping it restores the geometry. `typed_arm` drives the typed window.

---

**Speaker-sweep capsule survey, simulation numbers and ctest pins (from docs/calibration.md, "The speaker-sweep capsule survey").** On the 26-speaker example in the simulated room, tracked and told an offset 18.1 mm from the stand's true one (`--track-sim-offset`): residual 0.04 µs, radius 49.50 mm, every capsule within 0.03 mm of the truth after a reload and a re-aim, and the acoustic offset within 0.1 mm of the true one. Untracked and anechoic, every capsule within 0.01 mm. Leave-one-out held every speaker under 0.1 µs on both. The `calibrate_capsule_survey` ctest pins 0.1 mm and 1 mm, a refusal by name for one speaker moved 80 mm in the layout, the clean table and center after `--drop-outliers` drops it, the stop on a second one, the refusal of a speaker 40 mm out radially by its range alone, a `--latency` 20 mm off that flags nobody, and the range check on the dome's upper side with the latency poorly pinned.

---

**Live aiming before capsule cross-correlation (from docs/calibration.md, "Accuracy, in simulation").** Before the capsules were cross-correlated, the live position direction read up to 0.6 degrees off (20 mm at 2 m) and a 10 cm move read 89 mm with 13 mm sideways (now 100.7 mm with nothing sideways).

---

**`calibrate_live_zylia` pins (from docs/calibration.md, "Accuracy, in simulation").** The ctest pins the 10 cm move, the 25 degree estimate with the reference, the file's larger error behind the screen, and the peak at 0 degrees. The `aim/sim_live` test turns a simulated box away and back through the real UI.

---

**Session tab full_sim numbers (from docs/calibration.md, "Simulate").** full_sim runs every step in order with the truth 72 to 100 mm off the plan on five speakers. `as_built.json` lands within 3.5 mm of the truth. `trims.json` carries `as_built.json`'s positions, and its gains match the simulator's sensitivities within 0.013 dB, where a solve against the plan's positions would miss by 0.39 dB. Every step read its predecessor's file in its current run, and the plan is byte for byte what it was.

---

**SPCAP focus NaN when the default stopped being an integer (from docs/glossary.md, Focus).** Trap: the moment the default stopped being the integer 12, `powf` of a slightly negative lobe with a fractional exponent returned NaN and silenced whole gain vectors. The code clamps the lobe at 0 now (`src/spatial/spcap.c:64`). Any future non-integer exponent reopens that class of bug.

---

**DC-driven laterality assertion shipped a left/right mirror (from docs/glossary.md, Laterality check).** A DC-driven `steam_decode` assertion had inverted polarity, because the default HRTF's per-ear DC gains oppose its audible ILD, so the assertion passed exactly when the field was mirrored. It mis-diagnosed a correct encode and shipped a left/right mirror that only a by-ear report caught. The current test drives a 660 Hz tone.

---

**Pooled physical-versus-phantom contrast read as "no effect" (from docs/validation.md, The physical reference arm; and docs/glossary.md, Matched-cell contrast).** Pooling the physical-versus-phantom contrast across placements happened during development: pooling one centered placement with one off-center one produced 0.13°, against 1.8° for the off-center placement alone. The distribution is bimodal (about 0 centered, degrees off-center), so a pooled median lands in the empty middle.

---

**The harness used to multiply a stimulus by panner gains (from docs/validation.md, What the phantom is rendered by).** "A harness that multiplied a stimulus by panner gains could sweep every one of those flags and measure nothing. That is exactly what it used to do." The phantom arm now renders through a real engine core.

---

**`--track` placement hook was passed in and never called (from docs/validation.md, Two safety behaviors).** The `--track-sim` exit-5 check exists because the hook was once passed into the session loop and never called. `--track` stayed inert while it announced the tracker was measuring. Asserting on the result would not have caught it: in simulate the field is synthesized from the same capsule table the estimator reads, so a wrong table cancels out and looks healthy.

---

**"Which is the old shape" (from docs/validation.md, The session shape).** With no sweep flags there is one condition per panner, "which is the old shape" (the session before render-knob sweeps existed).

---

**Stale validation.md figures replaced (2026-10-03; from docs/validation.md, Reading the output / What it has found so far).** The doc previously quoted SPCAP fixed-minus-tracked +10.2° CI [+6.4, +11.2] at 0.7 m horizontal, height "SPCAP 1.9° → 7.9°", SPCAP Spearman rho 0.44 (rE direction, within position) and 0.23 (Frank spread), and one-odd-capsule comb quality 1.00 to 0.58. Re-measured 2026-10-03 with the current binaries: the `valid` ctest prints +9.9 [+6.1, +11.1], 2.0° to 7.7°/7.8°, 0.40 / 0.22, and q 0.62; `bwa_validate --simulate --no-reference` prints SPCAP +10.1 [+5.8, +11.5], VBAP +4.4 [+2.0, +6.6] (ctest +4.6), and SPCAP sweet-spot/seated tracked misses 0.9°/6.8°. The doc now quotes the ctest and names it.

---

**Trimmed glossary procedure that lives in docs/calibration.md (2026-10-03; from docs/glossary.md).** Removed from the glossary as duplicate procedure, all still in calibration.md:
- Placement gate: "1 cm is up to 29 µs of arrival error", "a stand reads still the moment a hand pauses", "For `--localize` and the `--room-eq-grid` rows the tolerance is a loose 100 mm: trilateration and the grid key need each position known, not hit, but a gate on stillness alone would open at the previous row", "the window's mean orientation is what the run takes".
- Bump check: the 0.3° floor is "for Motive's single-frame jitter"; "The center-only modes do not check the turn"; cited `place_bump_pose`, `placement.c`.
- Capsule survey: "Clap positions are typed, or measured by a tracked clicker whose tip must hold still before each clap, against the tracked stand's center when there is one." "A clap banks only inside an armed window, and only the first transient in it. From the sixth clap on, its direction must also agree with a provisional survey. After Solve, leave-one-out (`zylia_survey_loo`) scores each clap against the geometry the others solve to and flags one that does not fit."
- Mount offset: "(11.5 mm for 0, 90, 180 and 225 degrees on a 60 mm ring)"; "With a zero offset the center does not depend on the body's orientation at all."
- Tracked directivity compensation: "The array-sim room applies the same model at full resolution as each virtual speaker's ABSOLUTE loss toward the listener, so the audition hears the comp's two-band residual against the loss it corrects." (shortened, not removed)
- Array-sim room: "(gains from a precomputed interaction matrix, Liski and Valimaki 2017)" shortened to the citation; the interaction matrix is in spatialization.md.
- Imaginary loudspeaker: "The cube grid's roughly 55 degree nadir gap stays under the threshold" was wrong (the default grid has a speaker directly below and above `ref`); replaced.

---

**SPCAP focus was a compile-time constant (2026-08-07; from docs/spatialization.md).** `focus` used to be a compile-time 12: right for the 26-speaker grid, arbitrary anywhere else. The default now comes from the geometry (the -6 dB-at-mean-nearest-neighbor-angle exponent). The cube grid sits at 37.5 degrees of mean spacing and lands on 12.7, close to the constant it replaced.

---

**The phonon m<0 sign flip that mirrored the field (2026-07-24; from docs/spatialization.md).** A negation of ACN 1, 4, 5, 9, 10, 11 lived in `steam_decode.c` briefly, added to satisfy a DC-driven laterality test. The default HRTF's per-ear DC gains are laterally opposite its audible ILD, so the DC assertion passed exactly when the field was mirrored. phonon's real-SH m<0 convention matches the engine's encode on every channel (the xval golden pins it), so no fix-up belongs there. The `steam_decode` laterality test now drives a 660 Hz tone.

---

**Layout tool scoring moved from sweet spot to the listener grid (from docs/layout-schema.md).** "Expect worst-case numbers to be higher than they used to be. Those are the off-center and off-height cells that were previously never evaluated. A layout optimized for SPCAP or VBAP before this change was tuned against an objective that could not move, and is worth re-running." Related phrasing removed at the same time: `3d` was "the historical default" condition; `bed=0` "keeps the historical point-source-only objective"; the AMBI row grades AllRAD with max-rE, "which is what the engine now defaults to".

---

**Panner metric versus acoustic measurement, rank correlations (2026-07-21; from docs/layout-schema.md).** Against the acoustic measurement in validation.md, rE direction error ranks DBAP cells well (Spearman 0.82) and VBAP cells barely at all (0.19), where the Frank spread is the strong predictor (0.77). This is why the badness map's metric and the optimizer's focus wt default follow the panner. The pins section cited the same numbers: "the Frank spread is VBAP's strong predictor (Spearman 0.77 vs 0.19 for direction error)".

---

**Narrow conditions, measured on the default dome (from docs/layout-schema.md).** DBAP: a wedge-only `visual` climb reached 4.3°/18.3° inside the wedge. A plain `3d` run scored the same wedge at 4.3°/15.6° while staying usable everywhere else (26.8° vs 84.1° full-sphere worst). VBAP: `visual,3d` staged beat the direct run inside the wedge (3.4°/14.3° vs 3.9°/20.0°) for about 3° of full-sphere worst. The wedge-only run still collapsed everywhere else (74.5° worst).

---

**Observer-model asymmetry, measured (from docs/layout-schema.md).** A sweet-spot-optimized layout scored under the roam came out WORSE than the unoptimized dome (12.6°/82.3° vs 10.2°/75.2° for DBAP). A roam-optimized layout at the sweet spot (2.6°/15.6° SPCAP) sat near the dedicated seated optimum (0.9°/3.9°).

---

**Guarded two-panner recipe, measured (from docs/layout-schema.md).** On the default dome, `--optimize L.json vbap 3d guard=dbap` from the DBAP optimum was the best two-panner result of every mechanism tried: it matched the pure-VBAP mean (4.5°), beat the pure-VBAP worst by 7° (28.9° vs 35.6°, the warm start plus the constraint acting as a regularizer), and held DBAP within 0.3° of its own optimum.

---

**Greedy climb versus anneal and restarts, measured (from docs/layout-schema.md).** The greedy climb is basin-limited: identical inputs landed 26.8° and 31.2° worst on different random paths; stiff seeds barely improved at all. On the default dome the anneal/restarts upgrade was a wash (4.8°/28.8° vs greedy's 26.8-31.2° spread, at 7x the iterations), but it rescued a stiff seed outright: the clamped r=3.2 sphere that greedy left at 28.3° worst reached 4.5°/24.1°, the best moving-listener result measured on that array. VBAP saw no rescue anywhere (eight restarts across two very different seeds landed within 2% of one cost: a flat-bottomed landscape). The doc called the anneal mode "Metropolis acceptance".

---

**Beds in the optimizer cost, measured on the dome (from docs/layout-schema.md).** Point-source optimization already did most of the bed's work (bed 23.9°/126.8° at the seed, 17.0°/54.0° after a plain DBAP climb). A naive `bed=1` co-optimization traded 2° of bed mean for 17° of DBAP worst. From the DBAP optimum, `--optimize L.json dbap 3d bed=3 guard=dbap` reached the best bed mean measured (14.1°) while DBAP improved to 4.8°/24.6°.

---

**Shared layout cost to a seated user (from docs/layout-schema.md).** Measured, the shared multi-consumer layout cost the seated user about 1.5° mean against a dedicated seated-only array.

---

**Leash versus free optimization, measured (from docs/layout-schema.md).** With the real constraints file, a 0.75 m leash scored the same as a free one (6.5°/42.5° vs 6.4°/41.1°) while the free run wandered speakers up to 1.4 m further along the truss. Projection of the incoming layout into the constraints and pin slabs at every optimize start was added later ("now also projects").

---

**Pins, measured on the then-default 26-speaker dome (from docs/layout-schema.md).** Without pins, across a `horizontal,3d` staged run, DBAP's in-slab population went 10 speakers to 5; VBAP's went 15 to 7, and a direct `3d` run left 3. With 12 pinned both panners matched or beat their plane-only run in-band (DBAP 3.8°/19.4° vs 3.9°/19.6°; VBAP 3.0°/16.3° with the best in-band spread, 18.0°). Full-sphere mean stayed within 0.2° of the unpinned result for both panners; the worst case gave up 3 to 5°. More pins: full-sphere mean DBAP 4.9° unpinned, 5.1° at 12 pins, 5.4° at 16; VBAP 4.6°, 4.6°, 5.1°. Plane gain past 12 was small and panner-dependent (DBAP 3.8° to 3.5°, VBAP 3.0° to 3.5°, so VBAP regressed). Single runs of a stochastic climb. On that dome 12 pins bought the plane ceiling.

---

**Runtime scene geometry used to be locked (from docs/materials.md).** The borrowing reflection and pathing sims now take the scene lock shared around their ray traces, so an `iplSceneCommit` "can no longer race" a `RunReflections`/`RunPathing` (the doc named the lock an `SRWLOCK`; it is `os_rwlock`). `scene_locked` in `src/engine.c` "now only means" no scene (no SDK or a failed create). Also from materials.md: "Two implementations now exist", "The array render no longer needs Steam Audio", "No-SDK builds are now fully viable".

---

**Old box-plus-mesh recipe (from docs/materials.md).** Before `bwa_scene_set_ism_room` and `bwa_box_mesh` existed, the doc said: "Want both? Call `bwa_scene_set_box` first, because nothing else captures the ISM room. Then call `bwa_scene_set_mesh_mat` once, with the box walls plus your geometry. (The Godot binding does exactly this; see `bindings/godot/src/bwa_geometry.h`.)" Godot now calls `scene_set_ism_room` and merges the box walls into one `set_mesh_mat`.

---

**Web playground stimuli moved from a push feed to loaded sounds (2026-09-22; from bindings/web/README.md).** The playground fed its stimuli as a push source from a 20 ms timer on the main thread with 100 ms queued ahead of the audio clock, and that was wrong in both directions. A stall longer than the queue was a hole in the sound, which is what "the click train becomes inaudible for short periods" was (reported 2026-09-22; headless Chrome on a fast desktop collected 31 to 37 stream starves in one ordinary check run before anyone stalled anything on purpose). And the queue was also the switch latency: a menu change waited for the old signal to drain. After the move to loaded sounds a stimulus switch is heard after 6 to 16 ms, where the feed took 97 to 170, and a deliberate 400 ms main-thread stall produces zero starves, where the feed produced 45 to 59.

---

**Web playground bus meter read silent at a 120 ms poll (from bindings/web/README.md).** The page used to read `bwa_get_bus_levels` every 120 ms. That readback is the last block's peak (5.3 ms), so it saw one block in 22, and the default click train's 2 ms burst in every 250 ms meant the meter read "silent" and the cones stayed dark while the click was plainly audible. Fixed with a 4 ms sampling tick that holds the peak for the frame loop.

---

**XR page drew the phone gaze as a hand (from bindings/web/README.md).** The first build drew a Cardboard-style `gaze` input as a hand, which put the grip sphere on the user's face. Now a gaze draws only a 1.2 cm dot at the end of its ray while the ray is on the menu, and a screen tap draws nothing.

---

**Godot convenience tier landed in 0.12.0 (from docs/integration.md).** The 0.12.0 convenience tier landed in the Godot binding as three things: assets (`BwaEngine` stopped keeping a cache of its own and acquires through the core's `(path, flags)` cache), opt-in `async_load`, and `get_desc` / `apply_desc` / `reset_to_preset` / `BwaSource.get_preset`.

---

**Web runtime changelog: what the runtime added (2026-09-22 to 2026-09-24; from docs/web.md).** The section as it stood, verbatim:

On top of [What the spike changed](#what-the-spike-changed) below.

- `src/sink/worklet_sink.c`: the backend, and the only new file in `src/`.
- `src/sink/sink.h`, `src/sink/sink.c`: the declarations, the dispatch, the device query and the
  browser AUTO entry. One `#elif defined(__EMSCRIPTEN__)` branch, placed BEFORE the `__linux__` and
  `__APPLE__` ones because Emscripten's sysroot defines `__unix__` and a plain chain would offer a
  backend no browser carries.
- `include/bw_audio.h`: `BWA_SINK_WORKLET = 9`, appended.
- `CMakeLists.txt`: the `BWA_WITH_WORKLET` block. OFF by default even on Emscripten, because
  `-sAUDIO_WORKLET` pulls in `-sWASM_WORKERS` and changes `emscripten_get_now` for every target
  that links the library, and the CI leg must not pay for that.
- `cmake/bwa_bindings.cmake`, `bindings/web/CMakeLists.txt`: a fourth binding, opt-in and
  Emscripten-only.
- `bindings/web/`: the two layers, the transport, the example page, the node suite, the README.
- `bindings/web/playground/`: the browser port of `examples/playground.cpp`, with three.js visuals
  and six scenes, on the generated 24-speaker dome (`examples/dome_24.json`, staged into `dist/`
  by `build-web.sh`) until you upload a layout, plus `tests/run-playground.mjs` that drives it in headless Chromium. Its check
  measures the AUDIBLE output now, through an AnalyserNode pair on `engine.outputNode()`: the
  speaker cones and the meter strips have to move while the default click train plays, a stimulus
  change has to reach the output inside 150 ms, and a profile rebuild has to keep `device_lost` at
  0 and still put a `+x` source in the left ear. Each of those replaced a check that a silent page
  passed.
- `bindings/web/xr/`: the head-tracked sibling of that page. In an `immersive-vr` session the
  viewer pose becomes the engine's listener pose every XR animation frame, which is the one thing
  headphones and a mouse cannot demonstrate. It imports the playground's rig, scenes, gizmos and
  control builder, and adds four things of its own: `frame_xr.js`, the XR reference space to room
  seam (a 180 degree yaw, derived and tested, NOT the mirror the two "+x" conventions suggest);
  `session.js`, whose pose path deliberately does not depend on the renderer binding; `panel.js`,
  the in-world menu, because DOM Overlay is an AR-only feature in practice; and a page-side
  prediction lead, because `bwa_set_pose_prediction` only leads the engine's own tracker and is
  inert for a pushed pose. `tests/run-xr.mjs` and `tests/xr_frame.test.mjs` are its checks, and
  `bindings/web/README.md` has the derivation in full.

  The XR page opens the engine at block 256 on an AudioContext it creates with `latencyHint: 0.05`,
  the page's side of `BWA_SINK_FLAG_DEEP_BUFFER` (the engine adopts the page's context, so the page
  sets the hint). Block 128 was tried first (2026-09-23), when the crackle looked like a 256 block
  landing whole in every other callback; the fix for a render that overruns its quantum turned out to
  be the browser's output buffer, so the page takes the block with half the per-block overhead. On
  the Windows desktop the deep context reports baseLatency 50 ms and the engine 2656 frames
  (55.3 ms) of output latency; the standard `"interactive"` context 10 ms and 736 frames (15.3 ms).
  The panel offers 128, 256 and 512, and deep or standard for the buffer. Either rebuilds the engine
  and the AudioContext. No new gesture is needed, because the Enter VR press already unlocked audio
  for the document. The flat playground stays `"interactive"`.

  **The diagnostics dump** is for the headset, where nobody can attach a debugger. `xr/diag.js`
  keeps the last 30 s at 1 Hz: device underruns and their ms (playbackStats through the health
  block), the browser's average output latency, late blocks, the render peak against the block
  period, the output latency, the slip, the context state and rate, the JS heap
  (`performance.memory`, Chromium only), frames that second with the longest frame interval, how
  long ago the last frame ran, how long ago a health poll came back (or "poll STUCK" after 2 s), and
  any page error. It shows as plain text in a textarea when the session ends, or on the "copy
  diagnostics" button in the flat view. No network, no storage. If the page freezes, the 1 Hz timer
  stops with it, so the LAST line is the state just before: a stopped frame loop reads as a growing
  `lastframe`, a poll that never returned as `poll STUCK`, memory growth as a climbing `heap`.

  What to read on the headset: the panel's "audio health" line, two lines, numbers first. The
  first number is the **device underrun count** since start, with the silence it cost in ms: the
  browser's own `playbackStats`, which the sink reads into `bwa_health` (dropouts and dropped
  frames). Then the render peak against the block period and the late blocks in the window. The
  second line leads with the **audio clock slip**: over the last 5 s, wall time from
  `performance.now()` minus audio time from `AudioContext.currentTime`, both read on the main
  thread every animation frame (`xr/slip.js`). It is the cruder estimate of the same silence, and
  the only one on a browser without playbackStats. When the render thread
  misses a deadline Chrome plays fallback silence and the graph does not advance, so the audio
  clock falls behind: 0 is healthy, positive is the audio thread not keeping up. Beside it is the
  count of animation frames over 30 ms in the same window, which tells a stalled main thread (long
  frames, no slip; the main thread samples both clocks late, not apart) from a stalled audio thread
  (slip, no long frames). Then late blocks in the window, then the render peak in milliseconds
  against the block budget, marked "(1 ms clock)" because on the worklet that is what it is. There
  is no percentage and no "per process() call" figure any more: both multiplied a 1 ms reading.
  Headless desktop Chrome, measured by `run-xr.mjs`: in the first 5 s after a start the slip reads
  2 to 3 percent (the context's clock sits at 0 for its first few hundred ms) and the render peak
  7 to 13 ms (the first blocks pay for wasm compilation); once the start is out of the window it
  reads 0.1 percent (3 ms) and 1 ms. A plain oscillator page reads the same 0.1 percent, so that is
  the resolution of `currentTime` on the main thread, not a loss. The direction and the scale
  were checked with a worklet that busy-waits on purpose, headless Chrome 153, 5 s windows: no
  stall reads -0.1 percent, a 20 ms stall once a second reads 0.1 percent (the device buffer
  absorbs it), a 100 ms stall once a second reads +2.4 percent (119 ms). So the slip counts the part
  of a stall that outlasted the buffer, which is the silence you hear, not the stall itself. The 330 to 480 percent peaks this
  section used to quote were not start-up cost at all: they were the worklet's uninitialized clock
  (see "The clock, and a trap that is invisible" above).

  **Why a bigger block gives fewer but longer glitches here.** The adapter renders a whole engine
  block synchronously inside ONE `process()` call, and the browser cannot hand the device that
  quantum until the call returns. So a render that overruns stalls the output for the whole render,
  not for the part that overran. At 512 a block renders once every four quanta and costs about four
  times as much, so an overrun happens a quarter as often and stalls about four times longer: fewer,
  longer bursts, which is what the Galaxy XR showed. At 128 the same cost is spread over every
  call. Block size moves the glitches around; it does not remove them. The cure is render cost
  (fewer voices, `binaural` instead of `cave_sim`, a cheaper scene), or a device buffer with more
  headroom (the deep latencyHint, now the XR default; "What is verified" has what it bought on a
  busy desktop). If the slip stays at 0 while the audio crackles, the render is keeping up
  and the problem is downstream of it.

  The page also asks for `hand-tracking` as an optional feature. A pinch is `select`, so
  pinch-and-hold grabs the source and carries it on all three axes. A hand has no thumbstick, so
  the panel has move buttons (left, right, up, down, ahead, back, 0.25 m each, relative to where
  you look). Hand joints are not drawn. The playground's head gizmo is hidden in session, because
  it sits at the tracked pose and its nose cone floated 22 cm in front of the eyes.

  Both pages share one responsive layout, in CSS only. Side by side, the sidebar is
  `clamp(16rem, 30vw, 22rem)`, so a small window gives up sidebar before it gives up view. Under
  48rem (768 px) of width the pages stack: the view on top at 42 percent of the dynamic viewport
  height, the controls scrolling under it. A landscape phone (under 30rem tall) stays side by side
  with a 16rem sidebar. Wherever the start panel may not fit, its buttons come first, so Start and
  Enter VR are on screen without scrolling; once the engine runs, the XR page keeps only Enter VR at
  the top of the view (the start text used to stay over the scene, because `hidden` loses to the
  panel's id selector). On a coarse pointer, buttons, selects and sliders are 44 px tall. Both page
  runners check it at 390x844, 320x568, 844x390, 667x375, 1024x600, 800x500 and 1440x900, with a
  touch pointer on the phone sizes (`tests/layout_cdp.mjs`). The Galaxy XR's 2D window size is not
  measured, so the two middle sizes are guesses. Nobody has checked the layout on a real phone or in
  the headset.
- `bindings/web/src/host.js`, `client.js`: `invokeBuf`, which reaches the raw calls whose arguments
  are pointers. A page cannot allocate wasm heap of its own, so the alloc, the copy and the free
  happen around the one call on the control thread. Plus `writeFile` and `create({ files })` for
  the two `bwa_desc` fields that are PATHS, `create({ module })` so a rebuild costs no second
  module, and `outputNode()` so a page can tap what it is playing.
- `tools/wasm/gen-abi.mjs`: the raw layer and the export list, generated from the header.
- `tools/wasm/build-web.sh`: the one recipe for a shippable `dist/`.
- `tools/wasm/build-wasm.sh`: `BWA_WASM_PROXY` for the `-sPROXY_TO_PTHREAD` measurement.

---

**Web spike changelog: what the spike changed (2026-09-21; from docs/web.md).** The section as it stood, verbatim:

- `src/os/os_posix.c`: the three degradations above, behind `BWA_OS_NO_SCHED` and `__wasi__`.
- `CMakeLists.txt`: a `BWA_WASM` flag near the top, one branch in the platform banner, a `wasm32`
  entry in the phonon platform list, and `bw_audio` built STATIC on wasm. wasm32 has no
  position-independent shared library in either toolchain's sysroot; asking for one gets
  `relocation R_WASM_MEMORY_ADDR_LEB cannot be used against symbol 'mparams'; recompile with
  -fPIC` out of wasi-libc's own `dlmalloc.o`.
- `CMakeLists.txt`, again: when `BWA_WASM` and the SDK are both on, every executable in the root
  directory takes `LINKER_LANGUAGE CXX`, for the reason the phonon section gives. The raylib
  tools are refused on wasm (and Android) with a status line, since there is no window system.
- `tools/wasm/wasi-sdk.toolchain.cmake` and `tools/wasm/build-wasm.sh`. The script forwards
  `CMAKE_MAKE_PROGRAM` for hosts whose ninja is not on PATH, passes the two Emscripten stack
  flags, and runs ctest with a 600 s per-test timeout because a wasm test that goes wrong hangs
  rather than fails.
- `tools/phonon/build-phonon.sh`: `wasm` as a first-class `BWA_PLATFORM`, the interpreter
  resolution, the `-pthread` defaults and the dependency-patch step, plus
  `third_party/patches/deps-flatbuffers-tablekeycomparator.patch`. The tree-matching rule there
  also learned that osx, ios and wasm name their build tree without an architecture.

Nothing in `src/` outside the OS shim was touched.

---

**Worklet clock read stack garbage (until 2026-09-23; from docs/web.md).** `os_monotonic_ns` did not check `clock_gettime`'s return in an AudioWorkletGlobalScope (no `performance` object, so Emscripten's CLOCK_MONOTONIC returns ENOSYS) and returned whatever the stack held. Measured through a debug export in headless Chrome: `clock_gettime` returned -1 with errno 52 on every quantum, and the value read back was the constant 1069547520. So every render time read 0 except the first, which read 1.07 s and pinned `peak_load` at 401 (40108 %), which is the "percentages with many digits" a Galaxy XR showed. The 330 to 480 percent XR start-up peaks web.md once quoted were this bug. Fixed by checking the return and falling back to `emscripten_get_now`.

---

**Web profile rebuild passed a health check over a silent page (2026-09-22; from docs/web.md).** When a second engine opened on an AudioContext that had already carried one, the worklet registration threw and the sink host-paced silence. "The rebuilt engine renders" was a green check over a silent page, because the host-paced fallback counts blocks exactly like a working sink. That is why `BwaEngine.outputNode()` exists and the playground check measures the rebuild through an AnalyserNode.

---

**Web push feed was the playground default until 2026-09-22 (from docs/web.md).** The push feed was the playground's default and it was the wrong one: a push feed puts the page in the audio path, and a page's main thread stalls. It was reported by ear ("the click train becomes inaudible for short periods"), and the engine had been counting it all along as a stream starve: 31 to 37 in one ordinary headless check run, and 45 to 59 more from a single deliberate 400 ms stall. Loading the same stimuli instead took both to zero, and took a stimulus switch from 97 to 170 ms down to 6 to 16 ms, because the queue was also the switch latency.

---

**Phonon wasm: simulator paths first untested (from docs/web.md).** An earlier pass of web.md said the `-pthread` phonon run exercised the HRTF decode only and that the simulator paths (`steam_scene.c`, `steam_reflect.c`, `steam_path.c`) run phonon's own threads and were not tried, "that is where kwaa's patch would matter". The later with-SDK suite run passed `reflect`, `bake`, `path` and `dynmesh`, so kwaa's synchronous-pool patch is not needed.

---

**Web open items closed on 2026-09-22 (from docs/web.md).** "A memory-buffer asset entry point, or the decision that the push feed is the answer" was answered by neither: a page writes a wav into MEMFS and calls `bwa_load_sound`, so the engine owns the samples and loops them itself; the push feed stays for audio a page generates or receives as it goes. "A way to put a file in the wasm file system" was built: `FS` is in `EXPORTED_RUNTIME_METHODS`, the binding writes through it (`create({ files })` and `engine.writeFile`), and the playground uploads a `cave_layout.json` with it. It cost one link flag and one method, as predicted.

---

**Web.md verification dates (from docs/web.md).** The verification runs recorded in "What is verified" date from 2026-09-22 (emcc 6.0.10 container; the web module then had 29 node tests), with XR pins added 2026-09-23 (Galaxy XR fixes) and 2026-09-24 (block 256 on a 0.05 context; the paired underrun table, run with ten to fourteen CPU-bound R jobs on a 16-thread laptop). The engine suite numbers (35 of 38, 36 of 38 with -sPROXY_TO_PTHREAD) are from 2026-09-22. XR block 128 was tried first (2026-09-23), when the crackle looked like a 256 block landing whole in every other callback; the fix turned out to be the browser's output buffer.

---

**Godot sound_get_channels answered 1 for every bed (from bindings/godot/README.md).** An earlier version of `sound_get_frames` / `sound_get_channels` decoded the path on a cache miss, and it always decoded MONO, so an ambisonic bed answered 1 channel forever. Both now go through `bwa_sound_find`, a pure lookup that never loads, and report 0 for a path the engine has not loaded. Before `get_desc()` existed, nothing in the binding read a source's settings back.

---

**Scene commit locking (from docs/api.md).** Static swaps and dynamic-mesh moves are runtime-safe because `iplSceneCommit` runs under an exclusive scene lock and every ray trace holds it shared, so a commit can never race a trace (phonon documents exactly this constraint).

---

**FDN feedback matrix (from docs/api.md).** The directional FDN uses Householder feedback: a lossless prototype in which the decay filters are the only loss.

---

**Dual-band panning lineage (from docs/api.md).** The engine's dual-band mode (amplitude normalization on the low band, power normalization on the high band) is SPAT's "VBP Dual-Band".

---

**NatNet bitstream pinning pointer (from docs/api.md).** If a future Motive outruns the parser's certified suffix layout, the unicast-only `Bitstream` command can pin the server to a known syntax; see `src/tracking/natnet.c`.
