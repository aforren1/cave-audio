# Profiling (Tracy) and real-time scheduling

The engine is instrumented for [Tracy](https://github.com/wolfpld/tracy) (pinned **v0.13.1**), opt-in.
The instrumentation answers two questions: **are you inside the per-block time budget**, and **is
anything (especially the ray-tracing sim threads) stealing time from audio**.

## Build

```
cmake -S . -B build -DBWA_TRACY=ON
cmake --build build --config RelWithDebInfo
```

This fetches Tracy and links its client into `bw_audio.dll`. The client is built **on-demand**, so it
costs almost nothing until a profiler actually connects. Without `-DBWA_TRACY=ON`, the macros in
[`src/core/profile.h`](../src/core/profile.h) compile to nothing: no dependency, no overhead.

## What's instrumented

- **A Tracy "frame" = one audio block.** That's the budget unit. At 256 samples / 48 kHz the block
  period is **5.33 ms**. The block zone must stay well under it, with headroom for jitter.
- **The block zone depends on the sink.** `asio block`, `null block`, `alsa block` and
  `manual block` time one block directly. WASAPI, JACK, AAudio and the worklet go through the
  fixed-quantum adapter, so each rendered block is a `quant block` zone inside the device callback
  (`wasapi block`, `jack cycle`, `aaudio callback`, `worklet quantum`).
- **Threads.** The audio thread shows up as `bw-audio (ASIO)` (the driver's `bufferSwitch`
  thread), `bw-audio (WASAPI)`, `bw-audio (ALSA)`, or `bw-audio (null)` (the no-hardware render
  thread; same `render()`, so you can profile without a device). JACK, AAudio and the worklet
  render on a thread the server, the OS or the browser owns; their named threads
  (`bw-audio (JACK lost)` and so on) are the host-paced fallbacks after a device is lost.
- **Zones** under the block: `rt_render` → `mix voices`, `decorrelate`, `reflect tap`,
  `path tap`, `align`; plus `array-sim room` (with `array-sim eq` inside it), `binaural decode`
  and `convert_out`. The sim threads show
  `occlusion ray-trace` / `reflection ray-trace` on `bw-sim (occlusion)` / `bw-sim (reflections)`;
  the pathing sim thread appears as `bw-sim (pathing)`.
- **Plot**: `rt voices` (active voice count); load versus block time at a glance.

## The benches (opt-in)

Both benches below are dev/profiling tools, off by default. Build them with `-DBWA_BUILD_BENCH=ON`
(pair with `-DBWA_TRACY=ON` when you want Tracy zones):

```
cmake -S . -B build -DBWA_BUILD_BENCH=ON
cmake --build build --config RelWithDebInfo
```

## The headless bench

[`examples/profile_bench.c`](../examples/profile_bench.c) drives a representative load (moving voices
with Doppler / air / reverb-send / spread fanned across them) through the **null sink**, so it needs no
hardware:

```
bwa_profile_bench [seconds=20] [voices=16] [cave|binaural]
```

It prints the block budget and renders the load on the engine's render thread exactly as the ASIO
callback would. Attach the Tracy GUI, or capture headless (below). Your real app or the playground work
too. So does anything that runs the engine.

## The per-situation bench (headless numbers, no GUI)

[`examples/bench_situations.c`](../examples/bench_situations.c) answers "**how much does each thing
cost**" directly, without Tracy tooling. It renders a MATRIX of configurations (idle, the limiter,
DBAP / SPCAP / VBAP scaling with a moving listener, dual-band and CAP, binaural HRTF, `cave_sim`,
occlusion EQ, spread/decorrelation, Doppler/air, ISM early reflections, 3rd-order beds, FDN, the
Steam reflection bed, pathing, everything-on) through the **manual sink** (`bwa_render_block`,
synchronous; no device thread). It times the per-block cost of each and prints mean / median / p99 /
max in microseconds and as a percentage of the block budget.

```
bwa_bench_situations [blocks=4000] [zones] [layout=<path>] [only=<substring>]
```

- `layout=` adds the `rig:` rows, rendered on that layout. The default grid carries no `directivity`
  block, so without a rig-shaped layout the tracked directivity comp and the array-sim room's EQ
  never run. Give it a 24-speaker layout with the 4410A model to see what the rig pays. Put `eq`
  arrays in its speakers to measure the correction FIR.
- `only=` runs the rows whose name contains the substring.

It measures the **audio-thread budget**: the ray-tracing sim threads are background and not counted
(only the audio-thread reflect/path decode they feed is). Because it drives blocks synchronously it's
deterministic and needs no capture tooling; build it with or without `-DBWA_TRACY=ON` (the numbers
match within a few us; the on-demand client is dormant). Caveat: the bench thread is not MMCSS, so
`max` includes OS preemption the real ASIO thread wouldn't see. Read `mean` / `median` for the DSP
cost, and `max` as a loose upper bound.

### Comparing two builds

On a hybrid CPU (performance plus efficiency cores, like most current laptops) a normal-priority
thread migrates between core types, and the same block measured 2x apart run to run. The bench
therefore pins its thread the way MMCSS places the real audio thread: on Windows it selects the CPU
sets of the highest efficiency class, opts out of power throttling and raises its priority.

Pinning is not enough on a laptop. Its clock state still drifted by up to 2x between two full runs
of the matrix, which is larger than most changes you will want to measure. So never compare a run of
build A against a later run of build B. Interleave them instead: run one row with `only=` on A, then
on B, then on A again, four or more times, and keep each side's minimum median. The drift then hits
both sides alike, and the ratio B/A is stable to a few percent where the absolute numbers are not. Any
two build trees work, since the bench uses only the public ABI: copy the old DLL's tree aside, or
build the old commit's source in a second tree.

**Firewall.** The Tracy client opens a listening socket (TCP 8086) and a UDP discovery broadcast, so
you must allow the profiled exe through the Windows Firewall the first time (you'll get the
standard prompt). If you only ever profile on the **same machine**, configure with
`-DTRACY_ONLY_LOCALHOST=ON` (and optionally `-DTRACY_NO_BROADCAST=ON`): it binds to 127.0.0.1 and
skips the broadcast, which avoids the firewall prompt. Leave both off if the Tracy GUI runs on a
different box on the LAN.

## Text summaries (headless CSV, no GUI)

The Tracy CLI tools (in the [release](https://github.com/wolfpld/tracy/releases), or built from
`csvexport/` and `capture/` in the Tracy source) give a scriptable per-zone budget report:

```
bwa_profile_bench 20 16 cave            # 1) start the load
tracy-capture -o bench.tracy -s 20     # 2) connect + record 20 s to a file
tracy-csvexport bench.tracy            # 3) per-zone CSV to stdout
```

`tracy-csvexport` emits one row per zone with **count, total, mean, median, min, max, std (ns)**. The
budget check is the block-zone row for your sink (`asio block`, `null block`, `quant block` and so
on) and the `rt_render` row: `mean` and especially `max`
must sit comfortably below the block period (**5.33 ms = 5,333,333 ns** at 256/48 kHz). For reference,
a 16-voice load with all propagation effects + the reflection bed measures ~458 us mean / ~1550 us max
(≈9% / 29% of budget). The GUI's Statistics window has the same numbers and an Export-to-CSV button.

## Built-in per-zone profiler (no Tracy tooling)

The Tracy CLI tools need a server dependency stack (zlib/png/bzip2/brotli via pkg-config or vcpkg)
that isn't always around. For a headless per-zone breakdown with **zero external dependencies**, build
with `-DBWA_PROFILE_SELF=ON` (mutually exclusive with `-DBWA_TRACY`). The same `BWA_ZONE_*` macros
then feed a tiny in-process accumulator (`src/core/profile_self.c`) instead of Tracy, and
`bwa_bench_situations zones` prints a per-zone table (calls / total ms / mean us / **us/block**) under each situation:

```
cmake -S . -B build-prof -DBWA_PROFILE_SELF=ON -DBWA_BUILD_BENCH=ON
cmake --build build-prof --config RelWithDebInfo --target bwa_bench_situations
bwa_bench_situations 1500 zones
```

`bwa_prof_reset()` / `bwa_prof_report()` are exported from the DLL, so any harness can bracket a window
and dump it. The `us/block` column is the audio-thread budget contributor per zone; the sim-thread
zones (`reflection ray-trace`, …) report their per-**tick** cost, not per-block (they run on their own
low-priority thread). Like Tracy, the QPC-per-zone instrumentation adds a little overhead, so from
this build read the per-zone *proportions*, not the absolute block total.

## Real-time scheduling / MMCSS

- **The hard-RT audio thread is the ASIO driver's `bufferSwitch` thread; the host doesn't create it.** A real
  ASIO driver (including RME Digiface Dante) already schedules its callback time-critical / via
  MMCSS "Pro Audio". The host shouldn't (and can't cleanly) re-register it. Nothing to do here.
- **The engine's own render threads ask for real-time.** Every render thread the engine owns (the null
  sink, the WASAPI and ALSA sinks, and the host-paced fallbacks) calls `os_thread_set_realtime` at
  its top: MMCSS "Pro Audio" on Windows, `SCHED_FIFO` on Linux (which can be refused; the ALSA
  sink reports that through `bwa_last_error`), a time-constraint policy on macOS. See
  [backends.md: Thread](./backends.md#7-thread).
- **The background threads**: the three Steam Audio **sim threads** (`bw-sim (occlusion)`,
  `bw-sim (reflections)`, `bw-sim (pathing)`), and the NatNet receiver. The one real risk is the sim threads: they ray-trace at ~30 / 12 / 10 Hz
  respectively and are CPU-heavy, so at normal priority they could preempt the audio callback and
  glitch it. The engine therefore drops all three below normal priority through
  `os_thread_lower_priority` (`steam_scene.c`, `steam_reflect.c`, `steam_path.c`). Tracy makes the
  interaction visible: the sim zones should yield, never overlap an overrunning audio block.
- **No sink raises the machine's timer resolution.** `timeBeginPeriod(1)` is system-wide in
  effect, so a sink that called it would hold the whole machine at a 1 ms timer tick. The
  self-paced loops wait on an absolute deadline through `os_sleep_until_ns`, which uses a
  high-resolution waitable timer on Windows 10 1803 and later and needs no global change to hit a
  sub-millisecond wake. Measured on a desk machine over 200 wakes 2 ms apart: median lateness
  0.43 ms, p99 0.74 ms, against 5.6 ms and 13.0 ms on the pre-1803 fallback path. Only that
  fallback still touches `timeBeginPeriod`, and only for the length of one wait.

## Memory budget

The engine's heap is **static**: everything is allocated at `bwa_create` (the voice table, the per-voice
Doppler rings; the speaker bus + scratch, sized to the `BWA_CHANNELS` capacity, though only the layout's
channel count is rendered; sound PCM at load). **Nothing allocates on the audio thread**
(hard invariant; see `docs/concurrency.md`). So the "memory budget" is the create-time footprint;
there is no real-time growth to chase. If you want allocation-level detail (footprint / leaks) in Tracy's
memory view, wrap the big allocations with the `BWA_ALLOC` / `BWA_FREE` hooks already in `profile.h`.
