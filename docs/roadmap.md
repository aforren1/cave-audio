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

## M3 — wav + voice mixing
- `sound.c` (dr_wav load, buffer lifetime, retire-ack), `mix_voice` with gain ramp.
- `bw_load_sound`/`bw_play`/`bw_play_oneshot`.
- **Done when:** multiple wav voices mix to chosen channels; unloading a playing
  sound is safe (no use-after-free, verified under ASan on the control side).

## M4 — DBAP + layout + alignment
- `layout.c` (load surveyed geometry; per-speaker gain/delay), `dbap.c`
  (listener-relative gain solve), `align_speakers`.
- `dirty`-gated recompute; listener move dirties all voices.
- **Done when:** a source panned around the array localizes correctly for a centered
  listener, and tracks sensibly as the listener position is moved synthetically.

## M5 — Binaural monitor
- `binaural.c`: 26→ambisonics encode (head-oriented) → single ambisonics→binaural
  decode via Steam Audio; the `binaural` and `both` profiles.
- `bw_set_listener_pose` orientation feeds the monitor; array render ignores it.
- **Done when:** the `binaural` profile produces a convincing headphone render of the
  array with no Dante hardware present, and `both` runs array+monitor concurrently.

## M6 — OptiTrack ingest
- `natnet.c`: off-wire NatNet consumer; `track_internal` path samples freshest pose
  at block time.
- **Done when:** with `track_internal = true`, moving the tracked rigid body moves the
  rendered listener with low latency and no engine in the loop.

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
  (no new dependency).
- Ambisonic diffuse bed for ambient/reverb with a static decode matrix.
- `bw_source_create_stream` for procedural/engine-generated audio.
- Cross-platform device backend abstraction (ALSA/JACK/CoreAudio) behind the sink
  interface.

## Testing discipline throughout
- Keep ring and snapshot logic testable off the RT path (drive `drain_commands` from a
  unit test).
- Run the audio callback under a dropout counter in dev builds; any underrun is a bug
  in the invariants, not a tuning issue.
- Never let test scaffolding introduce allocation/locks into the callback.
