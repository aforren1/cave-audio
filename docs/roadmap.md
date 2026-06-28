# Implementation roadmap

Milestone-ordered so each step is independently testable. Build the device and
concurrency spine first, prove silence flows, then layer spatialization, then the
engines. Don't start with the engine bindings.

## M0 — Scaffolding
- CMake project, `third_party/` vendored (asiosdk, steamaudio, dr_wav).
- `include/bwaudio.h` compiles; stub `engine.c` returns a valid opaque handle.
- **Done when:** library builds and links on the target Windows toolchain.
- **Status: ✅ done** — `CMakeLists.txt` builds `bwaudio.dll` from a stub `src/engine.c`
  and a passing `bw_smoke` lifecycle test (MSVC 19.4x / VS2022). `third_party/` is wired
  in CMake but vendoring is deferred until M1 actually needs ASIO.

## M1 — ASIO sink, silence
- `asio_sink.cpp`: driver load → `ASIOCreateBuffers` → `ASIOStart`, writing 26 channels
  of silence to DVS. Capture the sample-position/`systemTime` timestamp.
- **Done when:** DVS shows 26 active output channels and a stable callback with no
  dropouts; timestamp advances monotonically.
- **Status: ~done, pending DVS hardware.** A device-sink seam (`src/sink.h`) keeps ASIO
  isolated; `asio_sink.cpp` implements the full bring-up against the vendored ASIO SDK
  and **compiles, links, and runs** (driver load → init → channel-count check → graceful
  fallback verified on real, non-DVS hardware). A `null_sink.c` offline backend runs the
  same render loop with no hardware and **verifies the stable-callback + monotonic-
  timestamp** criterion (`bw_audio_smoke`). The remaining piece — *26 active channels in
  DVS with no dropouts* — needs a Dante Virtual Soundcard endpoint to confirm on site.

## M2 — Concurrency spine
- Two SPSC rings, the voice table, `drain_commands`, the commit snapshot, generation
  handles, the event ring.
- `bw_source_create`/`destroy`/`set_pos`/`set_gain` + `bw_commit` wired to the ring.
- A test voice playing a generated tone routed to one channel, moved via `set_pos`.
- **Done when:** triggering/moving a voice from a test main thread is glitch-free and
  ThreadSanitizer/Helgrind is clean on the ring (test the ring logic off the RT path).
- **Status: ~done.** `src/rt.c` (behind `src/rt.h`) implements the two SPSC rings,
  voice table, `drain_commands`, the staging→active commit snapshot, generation-counted
  handles + free-list, and the event ring; the full `bw_source_*` / `bw_set_listener_pose`
  / `bw_commit` API forwards to it. `bw_rt_test` drives the consumer off the RT path and
  verifies the commit snapshot, generation stale-drop, play/stop, gain scaling, and
  position routing. The mixer is a **placeholder** — a 440 Hz test tone routed to one
  position-derived channel with a per-block gain ramp; M3 swaps in wav playback
  (`mix_voice` reading `sound->pcm`) and M4 the DBAP 26-gain solve. Remaining: the
  ThreadSanitizer/Helgrind pass needs a Clang/Linux build (MSVC has no TSan) — see
  `docs/build.md`; and the `bw_play_oneshot` / `EVT_VOICE_ENDED` natural-end recycle path
  lands with M3.

## M3 — wav + voice mixing
- `sound.c` (dr_wav load, buffer lifetime, retire-ack), `mix_voice` with gain ramp.
- `bw_load_sound`/`bw_play`/`bw_play_oneshot`.
- **Done when:** multiple wav voices mix to chosen channels; unloading a playing
  sound is safe (no use-after-free, verified under ASan on the control side).
- **Status: ✅ done.** `src/sound.c` decodes wav to mono float via dr_wav (fetched +
  pinned, see third_party/README.md); `rt.c` gained the Sound table, generation-counted
  sound handles, and the full retire-ack handshake (`bw_unload_sound` → `CMD_SOUND_RETIRE`
  → audio detaches voices + acks `EVT_SOUND_RETIRED` → control frees the buffer).
  `mix_voice` reads `sound->pcm` at the cursor (loop/end), and a oneshot recycles its
  transient voice via `EVT_VOICE_ENDED`. Routing is still the M2 placeholder until M4.
  `bw_sound_test` verifies multi-voice mixing, natural end, oneshot recycle, and
  unload-while-playing — and **passes clean under AddressSanitizer** (`-DBWAUDIO_ASAN=ON`),
  discharging the no-use-after-free criterion.

## M4 — DBAP + layout + alignment
- `layout.c` (load surveyed geometry; per-speaker gain/delay), `dbap.c`
  (listener-relative gain solve), `align_speakers`.
- `dirty`-gated recompute; listener move dirties all voices.
- **Done when:** a source panned around the array localizes correctly for a centered
  listener, and tracks sensibly as the listener position is moved synthetically.
- **Status: ✅ done.** `layout.c` loads `cave_layout.json` (via cJSON, fetched+pinned) into
  the `Layout` struct — positions, per-speaker gain_db→linear and delay_ms→samples, the
  DBAP blur `r` and distance-attenuation curve — with a default 3×3×3 grid when no file is
  given; `dbap.c` is the listener-relative, constant-power gain solve (see
  spatialization.md); `align.c` is the per-speaker gain trim + delay-line output stage.
  `rt.c` calls `dbap_gains` in the dirty-gated `compute_gains` and runs `align_process`
  after the mix; the engine loads the layout at `bw_create`. `bw_dsp_test` verifies it:
  a source at each speaker localizes to that channel, the solve is constant-power, two
  speakers split, moving the listener shifts the distribution, and align applies gain+delay;
  it also round-trips the committed `examples/cave_layout.json`. The DBAP exponents/r/curve
  are the documented tuning knobs to dial against the real array.

## M5 — Binaural monitor
- `binaural.c`: 26→ambisonics encode (head-oriented) → single ambisonics→binaural
  decode via Steam Audio; the `binaural` and `both` profiles.
- `bw_set_listener_pose` orientation feeds the monitor; array render ignores it.
- **Done when:** the `binaural` profile produces a convincing headphone render of the
  array with no Dante hardware present, and `both` runs array+monitor concurrently.
- **Status: ~done (first cut).** `binaural.c` is the head-oriented 26→stereo monitor —
  each bus channel is a virtual speaker at its surveyed bearing from the listener,
  projected onto the head's right axis and constant-power panned to L/R (a 1st-order
  W/X ambisonic encode + two cardioid decoders). `engine.c` wires all three profiles:
  `cave` (26→device), `binaural` (26→memory→2-ch device via the monitor), and `both`
  (a 26-ch array sink + a 2-ch monitor sink sharing a double-buffer). The listener
  quaternion drives the monitor; the array render ignores it. `bw_monitor_test` verifies
  L/R directionality, median balance, and a 180° head turn flipping the image;
  `bw_smoke` runs all three profile lifecycles end-to-end (offline sink). **Remaining for
  full M5:** (1) the production HRTF decode — a higher-order ambisonic encode → single
  ambisonics→binaural decode via **Steam Audio** (slots in behind `monitor_process`;
  needs the SDK, unverifiable by ear here); (2) optionally a dedicated **WASAPI** stereo
  backend — though live headphone output already works today through any 2-ch **ASIO**
  driver (ASIO4ALL / FlexASIO / the registered Steinberg built-in), so a WASAPI backend is
  a convenience, not a blocker. An interactive scene (raylib) is the natural way to evaluate
  "convincing".

## M6 — OptiTrack ingest
- `natnet.c`: off-wire NatNet consumer; `track_internal` path samples freshest pose
  at block time.
- **Done when:** with `track_internal = true`, moving the tracked rigid body moves the
  rendered listener with low latency and no engine in the loop.
- **Status: ~done (parser + handoff verified; live capture on-hardware-pending).** `natnet.c`
  parses the NatNet **FrameOfData** multicast/unicast stream **off-wire** — the SDK is
  proprietary and would conflict with GPLv3 under distribution, so it is used only as a
  wire-format reference, never linked (see `build.md`). The parser walks to the selected rigid
  body's pose, fully bounds-checked: it handles the NatNet 3.x/4.0 variable-length sections and
  the 4.1+ per-section size prefix, and rejects truncated/old-version packets without
  over-reading. A receiver thread publishes the pose into a **seqlock** (`pose.h`, single-writer
  / single-reader, bounded-retry so the audio thread never blocks); with `track_internal = true`
  `rt_render` samples the freshest pose at block time — overriding the committed listener and
  dirtying every voice on a move (lower latency than routing pose through the command ring). The
  consumer auto-negotiates the bitstream version via a `NAT_CONNECT` handshake (env-overridable;
  config is via `BWAUDIO_NATNET_*`, see `api.md`). `bw_natnet_test` verifies the parser (v3 and
  v4.1, rigid-body select, the tracking-valid flag, truncation safety) and the seqlock roundtrip;
  the open→receive→join socket lifecycle is verified live (UDP socket + multicast join, clean
  thread join on close). **Remaining:** confirm against a real Motive server — actual pose
  reception and the room-space calibration (Motive origin → CAVE centre) need the rig on site.

## M7 — Unity binding
- P/Invoke binding, `BwAudio` bootstrap, `BwEmitter`, the `Room` coordinate helper.
- Verify the coordinate seam with a known-position test source.
- **Done when:** a Unity scene drives the engine end-to-end in `binaural` at the desk
  and `cave`/`both` on the hardware, same build.

## M8 — Unreal binding
- Subsystem + component mirroring Unity; UE→room coordinate conversion.
- **Done when:** an Unreal scene drives the same library identically.

## Later / optional
- Steam Audio occlusion + reflections feeding the per-source and diffuse paths
  (no new dependency) — **materials/geometry model designed in [materials.md](materials.md)**.
- Ambisonic diffuse bed for ambient/reverb with a static decode matrix (the reflection
  bed in [materials.md](materials.md)).
- `bw_source_create_stream` for procedural/engine-generated audio.
- Cross-platform device backend abstraction (ALSA/JACK/CoreAudio) behind the sink
  interface.

## Testing discipline throughout
- Keep ring and snapshot logic testable off the RT path (drive `drain_commands` from a
  unit test).
- Run the audio callback under a dropout counter in dev builds; any underrun is a bug
  in the invariants, not a tuning issue.
- Never let test scaffolding introduce allocation/locks into the callback.
