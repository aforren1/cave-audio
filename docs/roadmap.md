# Implementation roadmap

Milestone-ordered so each step is independently testable. Build the device and
concurrency spine first, prove silence flows, then layer spatialization, then the
engines. Don't start with the engine bindings.

## M0 — Scaffolding
- CMake project, `third_party/` vendored (asiosdk, steam-audio-*, dr_wav).
- `include/bw_audio.h` compiles; stub `engine.c` returns a valid opaque handle.
- **Done when:** library builds and links on the target Windows toolchain.
- **Status: ✅ done.** `CMakeLists.txt` builds `bw_audio.dll` from a stub `src/engine.c`
  with a passing `test_smoke` lifecycle test (MSVC 19.4x / VS2022).

## M1 — ASIO sink, silence
- `asio_sink.cpp`: driver load → `ASIOCreateBuffers` → `ASIOStart`, writing 26 channels
  of silence to DVS. Capture the sample-position/`systemTime` timestamp.
- **Done when:** DVS shows 26 active output channels and a stable callback with no
  dropouts; timestamp advances monotonically.
- **Status: ~done, pending DVS hardware.** A device-sink seam (`src/sink.h`) keeps ASIO
  isolated. `asio_sink.cpp` implements the full bring-up against the vendored ASIO SDK
  and compiles, links, and runs: driver load → init → channel-count check → graceful
  fallback, verified on real (non-DVS) hardware. A `null_sink.c` offline backend runs the
  same render loop with no hardware and discharges the stable-callback +
  monotonic-timestamp criterion (`test_audio_sink`). **Remaining:** *26 active channels
  in DVS with no dropouts* — needs a Dante Virtual Soundcard endpoint on site.

## M2 — Concurrency spine
- Two SPSC rings, the voice table, `drain_commands`, the commit snapshot, generation
  handles, the event ring.
- `bwa_source_create`/`destroy`/`set_pos`/`set_gain` + `bwa_commit` wired to the ring.
- A test voice playing a generated tone routed to one channel, moved via `set_pos`.
- **Done when:** triggering/moving a voice from a test main thread is glitch-free and
  ThreadSanitizer/Helgrind is clean on the ring (test the ring logic off the RT path).
- **Status: ~done.** `src/rt.c` (behind `src/rt.h`) implements the two SPSC rings, the
  voice table, `drain_commands`, the staging→active commit snapshot, generation-counted
  handles + free-list, and the event ring. The full `bwa_source_*` /
  `bwa_set_listener_pose` / `bwa_commit` API forwards to it. `test_rt` drives the consumer
  off the RT path and verifies the commit snapshot, generation stale-drop, play/stop,
  gain scaling, and position routing. **Remaining:** the ThreadSanitizer/Helgrind pass
  needs a Clang/Linux build (MSVC has no TSan) — see `docs/build.md`. The
  `bwa_play_oneshot` / `EVT_VOICE_ENDED` natural-end recycle path landed with M3.

## M3 — wav + voice mixing
- `sound.c` (dr_wav load, buffer lifetime, retire-ack), `mix_voice` with gain ramp.
- `bwa_load_sound`/`bwa_play`/`bwa_play_oneshot`.
- **Done when:** multiple wav voices mix to chosen channels; unloading a playing
  sound is safe (no use-after-free, verified under ASan on the control side).
- **Status: ✅ done.** `src/sound.c` decodes wav to mono float via dr_wav (fetched +
  pinned, see third_party/README.md). `rt.c` gained the Sound table, generation-counted
  sound handles, and the full retire-ack handshake (`bwa_unload_sound` →
  `CMD_SOUND_RETIRE` → audio detaches voices + acks `EVT_SOUND_RETIRED` → control frees
  the buffer). `mix_voice` reads `sound->pcm` at the cursor (loop/end), and a oneshot
  recycles its transient voice via `EVT_VOICE_ENDED`. `test_sound` verifies multi-voice
  mixing, natural end, oneshot recycle, and unload-while-playing — and passes clean
  under AddressSanitizer (`-DBWA_ASAN=ON`), discharging the no-use-after-free
  criterion.

## M4 — DBAP + layout + alignment
- `layout.c` (load surveyed geometry; per-speaker gain/delay), `dbap.c`
  (listener-relative gain solve), the align stage (`align.c`).
- `dirty`-gated recompute; listener move dirties all voices.
- **Done when:** a source panned around the array localizes correctly for a centered
  listener, and tracks sensibly as the listener position is moved synthetically.
- **Status: ✅ done.** `layout.c` loads `cave_layout.json` (via cJSON, fetched + pinned)
  into the `Layout` struct — positions, per-speaker gain_db→linear and delay_ms→samples,
  the DBAP blur `r` and distance-attenuation curve — with a default 3×3×3 grid when no
  file is given. `dbap.c` is the listener-relative, constant-power gain solve (see
  spatialization.md). `align.c` is the per-speaker gain trim + delay-line output stage.
  `rt.c` calls `dbap_gains` in the dirty-gated `compute_gains` and runs `align_process`
  after the mix; the engine loads the layout at `bwa_create`. `test_dsp` verifies it: a
  source at each speaker localizes to that channel, the solve is constant-power, two
  speakers split, moving the listener shifts the distribution, and align applies
  gain+delay. It also round-trips the committed `examples/cave_layout.json`. The DBAP
  exponents/r/curve are the documented tuning knobs to dial against the real array.

## M5 — Binaural monitor
- `binaural.c`: 26→ambisonics encode (head-oriented) → single ambisonics→binaural
  decode via Steam Audio; the `binaural` and `both` profiles.
- `bwa_set_listener_pose` orientation feeds the monitor; array render ignores it.
- **Done when:** the `binaural` profile produces a convincing headphone render of the
  array with no Dante hardware present, and `both` runs array+monitor concurrently.
- **Status: ~done; the by-ear headphone check is the remaining piece.**
  - **First cut:** `binaural.c` is the head-oriented 26→stereo monitor. Each bus
    channel is a virtual speaker at its surveyed bearing from the listener, projected
    onto the head's right axis and constant-power panned to L/R (a 1st-order W/X
    ambisonic encode + two cardioid decoders). `engine.c` wires all three profiles:
    `cave` (26→device), `binaural` (26→memory→2-ch device via the monitor), and `both`
    (a 26-ch array sink + a 2-ch monitor sink sharing a double-buffer). The listener
    quaternion drives the monitor; the array render ignores it. `test_monitor` verifies
    L/R directionality, median balance, and a 180° head turn flipping the image;
    `test_smoke` runs all three profile lifecycles end-to-end (offline sink).
  - **Production HRTF decode, stage 1 (done):** the 26→16-ch 3rd-order ambisonic
    encode (`ambisonics.c`, ACN/SN3D real SH), unit-tested (`test_ambi`). This is the
    SDK-independent front half of the production monitor.
  - **Production HRTF decode, stage 2 (built + running):** `steam_decode.c` is the
    ambisonics→binaural HRTF decode via Steam Audio (`iplAmbisonicsDecodeEffect`),
    wired into `engine.c` for binaural + both, with the simple-pan monitor as the
    fallback (`BWA_HAVE_STEAMAUDIO`; CMake auto-detects
    `third_party/steam-audio-artifacts/`). phonon was built from the vendored
    submodule (minimal core; see third_party/README.md). `test_smoke` drives the real
    `iplContextCreate → iplHRTFCreate → iplAmbisonicsDecodeEffectApply` path each
    block, and the `steam_decode` ctest asserts gross laterality (right→right ear,
    left→left, 180° flips).
  - **Convention** (`test_ambi` vs phonon's hardcoded SH constants): phonon decodes
    orthonormal/N3D real SH, so the SN3D encode is scaled by
    `ambi_phonon_scale = sqrt(2l+1)/sqrt(4pi)`; axes + orientation match phonon as
    written. phonon's effect frameSize is fixed at create, so the decoder is built at
    the sink's actual block size (`bwa_sink_block_size`).
  - **Remaining:** the by-ear check on headphones (any free 2-ch ASIO driver). The
    playground (`bwa_playground`) is the by-ear bench.
  - **Optional:** a dedicated WASAPI stereo backend — a convenience, not a blocker.
    Live headphone output already works through any 2-ch ASIO driver (ASIO4ALL /
    FlexASIO / the Steinberg built-in).

## M6 — OptiTrack ingest
- `natnet.c`: off-wire NatNet consumer; internal-tracking path samples freshest pose
  at block time.
- **Done when:** with a tracker connected, moving the tracked rigid body moves the
  rendered listener with low latency and no engine in the loop.
- **Status: ~done (parser + handoff verified; live capture pending hardware).**
  `natnet.c` parses the NatNet **FrameOfData** multicast/unicast stream **off-wire** —
  the SDK is proprietary and would conflict with GPLv3 under distribution, so it is a
  wire-format reference only, never linked (see `build.md`). The parser walks to the
  selected rigid body's pose, fully bounds-checked: it handles the NatNet 3.x/4.0
  variable-length sections and the 4.1+ per-section size prefix, and rejects
  truncated/old-version packets without over-reading. A receiver thread publishes the
  pose into a **seqlock** (`pose.h`, single-writer / single-reader, bounded-retry so
  the audio thread never blocks). With a tracker connected, `rt_render` samples
  the freshest pose at block time — overriding the committed listener and dirtying
  every voice on a move. That is lower latency than routing pose through the command
  ring. The consumer auto-negotiates the bitstream version via a `NAT_CONNECT`
  handshake (config via `bwa_tracker_desc` — `bwa_tracker_connect`, see `api.md`).
  `test_natnet` verifies the parser (v3 and v4.1, rigid-body select, the
  tracking-valid flag, truncation safety) and the seqlock roundtrip; the
  open→receive→join socket lifecycle is verified live (UDP socket + multicast join,
  clean thread join on close). **Remaining:** confirm against a real Motive server —
  actual pose reception and the room-space calibration (Motive origin → CAVE centre)
  need the rig on site.

## M7 — Unity binding
- P/Invoke binding, `Engine` bootstrap, `Emitter`, the `Room` coordinate helper.
- Verify the coordinate seam with a known-position test source.
- **Done when:** a Unity scene drives the engine end-to-end in `binaural` at the desk
  and `cave`/`both` on the hardware, same build.
- **Status: ✅ done.** `bindings/unity` is a UPM package (`com.brainworks.bw_audio`).
  Runtime: `Bwa.cs` (the P/Invoke layer), `Engine.cs` (bootstrap/scene manager),
  `Emitter.cs` (positional emitter), `Room.cs` (the coordinate seam), plus
  `AcousticGeometry.cs`, `MaterialAsset.cs`, `AmbisonicBed.cs`, and
  `ClipAttribute.cs` for the acoustics/bed features. Editor:
  `ProjectCheck.cs` (warns when Unity's built-in audio is still enabled, with
  one-click disable) and `ClipDrawer.cs` (the `[Clip]` StreamingAssets picker).
  The engine's CMake build stages `bw_audio.dll` + `phonon.dll` into
  `Runtime/Plugins/x86_64/` automatically. The `cave`/`both`-on-hardware half of the
  done-criterion rides the same on-site DVS check as M1.

## M8 — Unreal binding
- Subsystem + component mirroring Unity; UE→room coordinate conversion.
- **Done when:** an Unreal scene drives the same library identically.

## Later / optional
- Steam Audio materials — **occlusion + per-band transmission EQ + directivity
  implemented** (`src/steam_scene.c` sim thread; `src/rt.c` applies a 3-biquad
  transmission EQ + a directivity gain on the pre-pan stage). `bwa_scene_set_mesh_mat` /
  `bwa_source_set_occlusion` / `bwa_source_set_directivity`.
- Reflection bed — **implemented** (`src/steam_reflect.c`): a reflections sim →
  ambisonic IR → SH→26 decode, registered as the rt bus tap at `bwa_start`. The
  `reflect` ctest proves it's directional. **Baking is in too**: `bwa_reflections_desc.bake`
  precomputes the reverb at a probe grid so the sim thread looks it up instead of
  ray-tracing (`bake` ctest). Model in [materials.md](materials.md).
- Sound pathing — **implemented** (`src/steam_path.c`, opt-in via `bwa_desc.enable_pathing`):
  a sim thread routes a blocked source around occluders / through openings, publishes
  per-voice ambisonic shCoeffs + a bending-loss EQ, and the mixer SH-encodes the
  un-occluded signal into the bus via a `path` rt tap. The `path` ctest proves the
  route bends around a wall with the right direction.
- Ambisonic bed — **implemented** (`bwa_load_ambix` + `bwa_bed_*`): a file-fed AmbiX
  soundfield decoded to the 26-ch bus via a static SN3D sampling decode (`rt.c`
  `build_bed_decode`/`mix_bed`). The reflection bed reuses the same SH→26 decode.
- Audio file formats — **WAV/FLAC/MP3 + resample-on-load implemented**
  (`src/sound.c`, dr_libs).
- `bwa_source_create_stream` for procedural/engine-generated audio. Still future —
  only file streaming exists today (`bwa_load_sound_streaming`).
- Cross-platform device backend abstraction (ALSA/JACK/CoreAudio) behind the sink
  interface.

## Shipped beyond the roadmap

Features that landed without a milestone of their own, one line each:

- **Output protection limiter** — `bwa_set_limiter` / `bwa_set_limiter_ceiling`, on by
  default at -1 dBFS; linked across channels, the final stage in `rt_render`.
- **Voice priority + click-free steal** — `bwa_source_set_priority` (255 = protected);
  a full pool steals the lowest-priority voice through a fade reserve.
- **Pause/seek** — `bwa_source_set_paused` / `bwa_source_seek`; ramped gates, so both
  are click-free.
- **Disk streaming** — `bwa_load_sound_streaming` (`src/stream.c`); a background
  decode thread feeds a per-stream SPSC ring the audio thread pulls from.
- **Dual-band panning** — `bwa_set_dual_band`; 700 Hz crossover, amplitude-normalised
  LF / power-normalised HF, live A/B.
- **SPCAP/VBAP** — `bwa_set_panner`; fixed-listener alternatives to DBAP
  (`src/spcap.c`, `src/vbap.c`).
- **Source spread** — `bwa_source_set_spread`, 0 = point to 1 = wide, panner-agnostic.
- **Doppler + air absorption** — `bwa_source_set_doppler` /
  `bwa_source_set_air_absorption`; phonon-free per-voice DSP in `src/rt.c`.
- **Calibration suite** — `bwa_calibrate` (sweep → trims → acoustic position survey →
  room report; `src/measure.c` / `src/calib.c`), Zylia ZM-1 single-position
  localization (`src/zylia.c`), and the `bwa_calib_view` station. See
  `docs/calibration.md`.
- **GUI tools** — `bwa_playground`, `bwa_calib_view`, `bwa_layout_tool`; all on the
  imgui stack, each with a `--tests` suite under ctest.

## Testing discipline throughout
- Keep ring and snapshot logic testable off the RT path (drive `drain_commands` from a
  unit test).
- Run the audio callback under a dropout counter in dev builds; any underrun is a bug
  in the invariants, not a tuning issue.
- Never let test scaffolding introduce allocation/locks into the callback.
