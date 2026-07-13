# Profiling (Tracy) & real-time scheduling

The engine is instrumented for [Tracy](https://github.com/wolfpld/tracy) (pinned **v0.13.1**), opt-in.
The instrumentation answers two questions: **are you inside the per-block time budget**, and **is
anything (especially the ray-tracing sim threads) stealing time from audio**.

## Build

```
cmake -S . -B build -DBWAUDIO_TRACY=ON
cmake --build build --config RelWithDebInfo
```

This fetches Tracy and links its client into `bwaudio.dll`. The client is built **on-demand**, so it
costs almost nothing until a profiler actually connects. Without `-DBWAUDIO_TRACY=ON`, the macros in
[`src/profile.h`](../src/profile.h) compile to nothing — no dependency, no overhead.

## What's instrumented

- **A Tracy "frame" = one audio block.** That's the budget unit. At 256 samples / 48 kHz the block
  period is **5.33 ms**; the whole `asio block` (or `null block`) zone must stay well under it, with
  headroom for jitter. The audio thread shows up as `bw-audio (ASIO)` (the driver's `bufferSwitch`
  thread) or `bw-audio (null)` (the no-hardware render thread — same `render()`, so you can profile
  without a device).
- **Zones**: `asio block` / `null block` (whole callback) → `rt_render` → `mix voices`, `reflect tap`,
  `path tap`, `align`; plus `binaural decode` and `convert_out`. The sim threads show
  `occlusion ray-trace` / `reflection ray-trace` on `bw-sim (occlusion)` / `bw-sim (reflections)`;
  the pathing sim thread appears as `bw-sim (pathing)`.
- **Plot**: `rt voices` (active voice count) — load vs. block time at a glance.

## The headless bench

[`examples/profile_bench.c`](../examples/profile_bench.c) drives a representative load (moving voices
with Doppler / air / reverb-send / spread fanned across them) through the **null sink**, so it needs no
hardware:

```
bw_profile_bench [seconds=20] [voices=16] [cave|binaural]
```

It prints the block budget and renders the load on the engine's render thread exactly as the ASIO
callback would. Attach the Tracy GUI, or capture headless (below). Your real app or the playground work
too — anything that runs the engine.

**Firewall.** The Tracy client opens a listening socket (TCP 8086) and a UDP discovery broadcast, so
the profiled exe needs to be allowed through the Windows Firewall the first time (you'll get the
standard prompt). If you only ever profile on the **same machine**, configure with
`-DTRACY_ONLY_LOCALHOST=ON` (and optionally `-DTRACY_NO_BROADCAST=ON`): it binds to 127.0.0.1 and
skips the broadcast, which avoids the firewall prompt. Leave both off if the Tracy GUI runs on a
different box on the LAN.

## Text summaries (headless CSV) — no GUI

The Tracy CLI tools (in the [release](https://github.com/wolfpld/tracy/releases), or built from
`csvexport/` and `capture/` in the Tracy source) give a scriptable per-zone budget report:

```
bw_profile_bench 20 16 cave            # 1) start the load
tracy-capture -o bench.tracy -s 20     # 2) connect + record 20 s to a file
tracy-csvexport bench.tracy            # 3) per-zone CSV to stdout
```

`tracy-csvexport` emits one row per zone with **count, total, mean, median, min, max, std (ns)**. The
budget check is the `asio block` / `null block` (and `rt_render`) rows: `mean` and especially `max`
must sit comfortably below the block period (**5.33 ms = 5,333,333 ns** at 256/48 kHz). For reference,
a 16-voice load with all propagation effects + the reflection bed measures ~458 us mean / ~1550 us max
(≈9% / 29% of budget). The GUI's Statistics window has the same numbers and an Export-to-CSV button.

## Real-time scheduling / MMCSS

- **The hard-RT audio thread is the ASIO driver's `bufferSwitch` thread — we don't create it.** A real
  ASIO driver (including Dante Virtual Soundcard) already schedules its callback time-critical / via
  MMCSS "Pro Audio". The host shouldn't (and can't cleanly) re-register it. Nothing to do here.
- **Our own threads are soft/background**: the `null_sink` render loop (no-hardware fallback), the
  three Steam Audio **sim threads** — `bw-sim (occlusion)`, `bw-sim (reflections)`, `bw-sim (pathing)`
  — and the NatNet receiver. The one real risk is the sim threads: they ray-trace at ~30 / 12 / 10 Hz
  respectively and are CPU-heavy, so at normal priority they could preempt the audio callback and
  glitch it. All three are therefore dropped to `THREAD_PRIORITY_BELOW_NORMAL` (`steam_scene.c`,
  `steam_reflect.c`, `steam_path.c`). Tracy makes the interaction visible: the sim zones should
  yield, never overlap an overrunning audio block.

## Memory budget

The engine's heap is **static**: everything is allocated at `bw_create` (the voice table, the per-voice
Doppler rings, the 26-ch bus + scratch, sound PCM at load) and **nothing allocates on the audio thread**
(hard invariant — see `docs/concurrency.md`). So the "memory budget" is just the create-time footprint;
there is no real-time growth to chase. If you want allocation-level detail (footprint / leaks) in Tracy's
memory view, wrap the big allocations with the `BW_ALLOC` / `BW_FREE` hooks already in `profile.h`.
