# Device backends

This is the reference for the device sinks: the contract every backend meets, the shared pieces,
the ABI, the AUTO order, what each backend does, and how they build and test. Why the backends
exist, the design decisions behind them, the phase plan, the next-backend checklist and the open
verify list are in [backend-development.md](./backend-development.md).

Status: **phases 0 to 3, 5 and 6 are implemented**; phase 4 (CoreAudio) is still specification.
The engine has eight sinks: ASIO (`src/sink/asio_sink.cpp`), WASAPI (`src/sink/wasapi_sink.cpp`),
JACK (`src/sink/jack_sink.c`), ALSA (`src/sink/alsa_sink.c`), AAudio (`src/sink/aaudio_sink.c`),
Wasm Audio Worklet (`src/sink/worklet_sink.c`), null (`src/sink/null_sink.c`), and manual
(`src/sink/manual_sink.c`), over the two shared pieces (`src/sink/sink_convert.h`,
`src/sink/sink_quant.c`).
The ABI those need is in place: the appended `bwa_sink_type` values, `bwa_desc.device` and
`sink_flags`, the backend-agnostic device query, and `bwa_health.device_lost`. The OS shim
(`src/os/os.h`, `src/os/os_win.c`, `src/os/os_posix.c`) lets the library, the tests and the
console examples build and pass with gcc and clang on Linux, macOS and Android. What remains is
the CoreAudio backend.

Read [architecture.md](./architecture.md) for the bus seam first and
[concurrency.md](./concurrency.md) for the audio-thread rules every backend inherits.

## The contract a backend must meet

A backend is one file that fills the `bwa_sink_vtbl` in `src/sink/sink.h`, and the engine hands
it one `bwa_render_fn` and a planar float bus.
[backend-development.md: What stays fixed](./backend-development.md#what-stays-fixed) shows the
vtable and the rules that keep the core untouched. The rules below are numbered so a code review
can cite them.

### 1. Fixed quantum

Every render call passes the same `nframes`, and it equals `block_size()`. This is not a
preference. With the SDK, the headphone decode is created for one frame size and silences any
other (`steam_decode.c`, the `n != m->frame_size` guard in `steam_monitor_process`), and the
`cave_both` handoff exchanges blocks of exactly the array's size. The ASIO sink meets the rule
because the driver's buffer size is fixed once buffers exist.

The new APIs do not promise a fixed callback size:

| API       | callback size                                                            |
|-----------|--------------------------------------------------------------------------|
| WASAPI exclusive, event-driven | fixed: one buffer per event                                 |
| WASAPI shared, event-driven    | varies: `bufferFrameCount - GetCurrentPadding()` per event  |
| CoreAudio HAL                  | `kAudioDevicePropertyBufferFrameSize` is settable and normally honored, but can change on a route or rate change |
| JACK                           | fixed per cycle (`jack_get_buffer_size`); a buffer-size callback announces a change, which PipeWire does when another client asks for a smaller quantum |
| ALSA                           | you write, so you choose: fixed                              |
| AAudio                         | `setFramesPerDataCallback` is a hint; the callback can deliver other sizes |
| Wasm Audio Worklet             | fixed at 128 frames by the Web Audio 1.0 spec, and not negotiable. The 1.1 `renderSizeHint` moves it; nothing makes it vary |

So every backend except ALSA goes through the **fixed-quantum adapter** (`sink_quant`, below).
It renders whole engine blocks into a small FIFO and hands the device any count it asks for.
When the device size equals the block size it is a straight pass-through with no copy and no
added latency, so a backend that manages to pin the device size pays nothing. A rig box pins
its JACK or PipeWire period to the engine block size and gets that pass-through; a desktop
whose quantum wanders pays one FIFO.

### 2. Format

The bus is planar float, channel-major (`sink.h`). The backend converts to the device's format
and layout. The conversions live in a shared header (`sink_convert.h`), in planar and
interleaved variants, because most non-ASIO APIs want interleaved frames. NaN converts to 0, never to an out-of-range integer, for the reason noted in
`asio_sink.cpp`: the limiter is optional, and float-to-int out of range is undefined.

Preference order when the device offers a choice: float32, int32, int24, int16. Reject
anything else at open, so the audio thread never meets an unknown type. JACK ports are planar
float already, so the JACK sink neither converts nor interleaves: each bus channel copies to
its port buffer.

### 3. Timestamp

`bwa_timestamp` is `{sample_pos, system_time_ns}`. The core uses the pair for two things: the
dsp clock (`bwa_get_dsp_time_frames`, `bwa_source_play_at`) and the device-versus-host drift
fit (`bwa_get_clock_model`). Definitions:

- `sample_pos` is the **stream position of the block's first frame**: the number of frames the
  sink has handed to the device before this block. Monotonic, frames, counted from the sink's
  start. This
  is what ASIO's `samplePosition` is (the position of the buffer being filled), so the meaning
  does not move.
- `system_time_ns` is the host monotonic clock at the moment the pair was captured, on the
  platform's monotonic clock (QPC on Windows, `mach_absolute_time` converted to nanoseconds on
  macOS, `CLOCK_MONOTONIC` on Linux and Android). Backend-defined epoch, as the header already
  says. Only differences matter.
- The pair must be a **consistent correspondence**. Capture it the same way every block, so the
  offset between "this sample_pos" and "plays at this host time" is a constant the caller
  removes with `bwa_get_output_latency_frames`. The drift fit needs nothing more.
- When the adapter renders two engine blocks inside one device callback, the second block's
  `system_time_ns` is the callback's host time plus the nominal duration of the frames between
  them. Reusing one host time for two sample positions feeds the fit a zero-slope pair.
- That extrapolation must never let the stamp step **backward**, and on its own it does. A
  callback that renders several blocks stamps the later ones ahead of its own host time; a device
  catching up after a fault then fires its next callback at once, with a host time earlier than
  where the last batch reached. A negative interval against a forward sample position is worse for
  the fit than a zero-slope pair, and it makes `bwa_get_clock` run backward. The adapter advances
  by one nominal block instead, and re-anchors to the real clock as soon as it overtakes again.
  `test_audio_sink` caught this on a deliberately starved WASAPI stream; `test_sink_quant` pins it.

Where each backend gets the pair:

| backend   | position and time source                                                                |
|-----------|-----------------------------------------------------------------------------------------|
| ASIO      | `ASIOTime.timeInfo`: `samplePosition` when `kSamplePositionValid`, and for the time, the driver's `systemTime` ONLY when its stamps show it is on QPC, else QPC read at callback entry. See "ASIO's `systemTime`" below |
| WASAPI    | `IAudioClock::GetPosition(&pos, &qpc)`, normalized by `GetFrequency()` to frames. Do not assume the units: `frames = pos * rate / freq`. `qpc` is in 100 ns units |
| CoreAudio | the IOProc's `inOutputTime`: `mSampleTime` and `mHostTime` when both valid flags are set; `mach_timebase_info` converts host ticks to ns |
| JACK      | `jack_get_cycle_times` at the top of the process callback: `current_frames` and `current_usecs`, the server's DLL-filtered pair in microseconds on `CLOCK_MONOTONIC`. Filtered, so the drift fit reads the DLL rather than the raw device, the way a QPC-synthesized ASIO stamp does |
| ALSA      | `snd_pcm_status` after the write: `snd_pcm_status_get_htstamp` and `_get_delay`, with the timestamp type set to monotonic in the sw_params |
| AAudio    | `AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &pos, &ns)`. Fails until the stream has run a little; keep the previous pair and set `measured` only after the first success |
| Wasm Audio Worklet | the adapter's own stream position, and `os_monotonic_ns` for the host half. The processor scope's `currentFrame` is the same number by construction (one `process()` call per quantum while the node is connected), so reading it would cost a wasm-to-JS transition per quantum for a value the sink holds. One caveat: `AudioWorkletGlobalScope` has no `performance`, so Emscripten's `CLOCK_MONOTONIC` returns `ENOSYS` on the audio thread. `os_monotonic_ns` checks that and falls back to `emscripten_get_now`, which is `Date.now` in that scope: same epoch as the control thread's, a thousand times coarser, and not monotonic. That makes both of the adapter's backward-step rules load-bearing rather than defensive: a stamp never steps back, and a render time that would go negative books 0 |

**ASIO's `systemTime`.** The ASIO SDK says `systemTime` is derived from `timeGetTime` on Windows
(`asio.h`, `AsioTimeInfo`), and the SDK's sample driver does exactly that. `timeGetTime` ticks at
1 to 15.6 ms and is not on QPC: on the development box it reads 27 ms off. Some drivers stamp from
QPC instead, and the API does not say which. Passed through raw, a `timeGetTime` stamp puts every
`bwa_get_clock` pair tens of milliseconds off `bwa_host_time_ns` and feeds the drift fit a 1 ms
sawtooth. So the sink asks the stamps. `src/sink/sink_tsbase.h` holds the rule, and the ZM-1
capture shell uses the same classifier:

- Each block compares `systemTime` against QPC and `timeGetTime`, both read at callback entry, and
  keeps the minimum of each difference over about 2 s. A callback can be late, never early, so the
  minimum is the dispatch delay's floor. A QPC minimum inside [-0.5, +5] ms names the base QPC. A
  `timeGetTime` minimum in range names it `timeGetTime`. Anything else is unknown. The first 16
  blocks name nothing.
- The block's `system_time_ns` is `systemTime` only when the base is QPC.
- Otherwise it is QPC at callback entry, less the dispatch floor last measured on a QPC stamp (0 if
  there never was one). Both sources then mean the buffer switch, so a driver whose valid flag comes
  and goes moves the stamp by its jitter, not by its dispatch delay. With no QPC stamp ever, this is
  the plain entry read, which is what the sink always did without `kSystemTimeValid`.
- A stamp at or before the previous one becomes the previous one plus one nominal block, the
  adapter's rule above. The one step left is the block a driver's first QPC stamp arrives on, when
  the entry read moves back onto the switch by the measured dispatch delay.
- A `timeGetTime` stamp is not mapped onto QPC through its measured offset. That offset is a
  minimum over stamps quantized to the tick, so it is only known to the tick, and every mapped stamp
  would carry the same 1 to 15.6 ms quantization. The entry read carries tens of microseconds of
  dispatch jitter, which is better. The tick can also change mid-stream when any process calls
  `timeBeginPeriod`.

`bwa_sink_health` carries the result for ASIO only: `stamp_base` (`SINK_TS_*`) and
`stamp_offset_ns`, the floor of QPC minus `systemTime`. Nothing public reads them yet. On the rig,
read the Digiface driver's base from the Zylia tab: the ZM-1 arrives on the same driver. Which base
the Digiface's driver uses is unverified.

### 4. Health

`bwa_sink_health` keeps its fields. The rule for `measured` is unchanged: true only when the
backend can observe the device's own position, so a zero dropout count means "none happened"
and not "could not know". The gap arithmetic stays in `sink_position_gap`, and the adapter
measures `late_blocks` and `render_ns_peak` for every backend it serves, the way the null sink
does today. `render_ns_peak` is the worst block over the last 4 to 5 s of STREAM time
(`SinkPeakWindow` in `sink.h`: whole-second buckets, one packed 64-bit word each, no lock), not
since start, so a slow first block or one old stall stops pinning `bwa_health.peak_load`. Every
backend that measures a render time feeds the same window: the adapter, the null sink, ASIO and
ALSA.

| field            | WASAPI                                                   | CoreAudio                                  | JACK                                        | ALSA                                              | AAudio                              |
|------------------|----------------------------------------------------------|--------------------------------------------|---------------------------------------------|---------------------------------------------------|-------------------------------------|
| `dropouts`       | **exclusive:** the queued depth `written - device_pos` went negative, the device consumed frames never written. **shared:** the interval between two `ReleaseBuffer` calls exceeded the buffer depth. Every event refills the buffer to full, so the device is holding `bufferFrameCount` after each release and runs dry that many frames later; a later release proves it had nothing in between. The depth rule cannot serve shared mode: the request size is exactly what the engine consumed since the last callback, so `written` telescopes to follow the device position and the depth never goes negative | `sink_position_gap` on `mSampleTime`, as ASIO | the xrun callback (`jack_set_xrun_callback`) | `-EPIPE` from the write (an underrun)          | `AAudioStream_getXRunCount` delta   |
| `dropped_frames` | **exclusive:** the size of that deficit. **shared:** the release interval minus the buffer depth, at the nominal rate. Not an estimate: it is the measured excess | the gap                                    | `jack_get_xrun_delayed_usecs` at the nominal rate | the elapsed monotonic time since the last good write minus the buffer depth, at the nominal rate | unknown: 0                          |
| `driver_resyncs` | 0                                                        | the `kAudioDeviceProcessorOverload` listener | 0                                         | `-ESTRPIPE` (a suspend), after recovery         | 0                                   |
| `measured`       | **exclusive:** once `GetPosition` has succeeded. **shared:** `bufferFrameCount > period`, and nothing to do with `IAudioClock`. The release-interval rule needs headroom between a normal cycle and the dry deadline; with a one-period buffer a normal cycle sits on it and ordinary jitter would read as a fault, so the honest answer is that the configuration cannot judge | once a timestamp with both flags arrived   | always (the server reports xruns directly)  | always (the write reports underruns directly)     | once `getTimestamp` has succeeded   |

The **Wasm Audio Worklet** has no column because every cell would say the same thing: Web Audio
reports no device position, no xrun count and no underrun signal of any kind. `currentFrame`
advances by exactly one quantum per callback whether or not the output starved, and a browser
renders several quanta back to back inside one system audio callback, so a per-callback
host-interval rule would false-positive on every ordinary batch. What Chromium DOES expose is
`AudioContext.playbackStats`: `underrunEvents` and `underrunDuration`, the browser's own count of the
silence its output played because a render came back late. The sink reads it in `health()`, on the
control thread (a main-thread JavaScript read, never the audio thread), books it as `dropouts` and
`dropped_frames`, and sets `measured` **true exactly when the browser has it**. On a browser without
it `measured` stays false, the contract's way of saying "cannot know". `late_blocks` and
`render_ns_peak` are real, because the adapter measures those for every backend it serves, but the
clock under them is `Date.now` (whole milliseconds, see rule 3 above), so they are coarse. The XR
page also reads `AudioContext.currentTime` against `performance.now()` on the main thread, a cruder
estimate and the only one without playbackStats (docs/web.md). `device_lost` is set while the sink
is host-pacing, which on this platform means a suspended AudioContext as often as a lost one.

One field is added to the public `bwa_health` in the same ABI bump (see
[ABI](#abi-changes)): `device_lost`. A lost device makes the sink behave like the null sink,
pacing the render from the host clock so the engine's clocks and playheads keep advancing,
and sets the flag. The control thread decides what to do (`bwa_stop` and `bwa_start`, or
nothing). Nothing reopens on its own.

### 5. Latency

`output_latency()` returns the render-to-DAC delay in frames, 0 for unknown, as today. Sources:

| backend   | frames                                                                                                   |
|-----------|----------------------------------------------------------------------------------------------------------|
| WASAPI    | `IAudioClient::GetStreamLatency` (converted from 100 ns units) plus the buffer size in exclusive mode, or plus one period in shared mode |
| CoreAudio | `kAudioDevicePropertyLatency` + `kAudioDevicePropertySafetyOffset` + `kAudioStreamPropertyLatency` + the buffer frame size |
| JACK      | the max over the sink's ports of `jack_port_get_latency_range(port, JackPlaybackLatency)`, read after the connections are made (the server recomputes on connect) |
| ALSA      | `snd_pcm_delay` right after a write, which is the buffer fill                                            |
| AAudio    | `getFramesWritten - pos` from the timestamp pair, extrapolated to now at the nominal rate; `getBufferSizeInFrames` when the timestamp is not yet available |
| Wasm Audio Worklet | `AudioContext.baseLatency + outputLatency`, in seconds, read once at open. `webaudio.h` has no accessor for either, so the sink reads the two properties off the context object itself. 0 when the browser reports neither, which is "unknown" and not "none". `BWA_SINK_FLAG_DEEP_BUFFER` moves it: a context the sink creates gets latencyHint `"playback"` (baseLatency 20 ms on the Windows desktop, 1216 frames in all at block 256, against 736 with `"interactive"`); a page that hands the sink its own context sets the hint itself (0.05 on the XR page: 50 ms, 2656 frames) |

The adapter adds the frames it holds in its FIFO. The value stays constant for the life of the
sink, as the header promises.

`BWA_SINK_FLAG_TIGHT_BUFFER` in `bwa_desc.sink_flags` asks the backend for the smallest device
buffer it can take, which moves this number and nothing else about the contract. ALSA asks for 2
periods instead of 3, AAudio for 1 burst instead of 2. WASAPI ignores it: exclusive mode already
runs a one-period buffer, and a shared-mode buffer belongs to the OS mixer. So do ASIO and JACK,
where the driver or the server owns the buffer. The device can always refuse, so read the result
back through `output_latency()` rather than assuming the request was granted.

### 6. Sample rate

The engine is built at `cfg.sample_rate` and cannot change it after `bwa_create`. Two policies,
chosen by the `exact_rate` argument the engine passes to `bwa_sink_open`:

- **Array sink (`exact_rate` set):** the device runs at the engine rate or the open fails,
  with the device's actual rate in the message. This is the ASIO sink's rule today and it does
  not relax: a resampled array shifts every per-speaker delay.
- **Headphone sink (`exact_rate` clear):** the backend asks for the engine rate first. When the
  device cannot, it may let the OS resample (WASAPI shared mode with
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, AAudio shared mode, an ALSA `plug` device) and the sink
  reports the degradation through the existing "succeeded but degraded" channel
  (`bwa_last_error` after a successful `bwa_start`; the header documents that convention next
  to `bwa_last_error`). The timestamp pair is then in the stream's rate, which is the engine's,
  so nothing downstream changes.

A caller can demand the array's strictness on a headphone sink with
`BWA_SINK_FLAG_EXACT_RATE` in `bwa_desc.sink_flags`. `bwa_sink_open` ORs that bit into the
`exact_rate` argument before any backend sees either, so there is one notion of "must not
resample" and no backend reads the flag itself. It is PsychPortAudio's
`paMacCoreFailIfConversionRequired`, and it belongs to an experiment where the rate is part of the
measurement rather than a preference. The default without the flag is unchanged: the headphone
sink accepts the OS resampler and reports the degradation.

CoreAudio and JACK have no per-client resampler, so a headphone open fails on a mismatch there
too. JACK's rate is the server's (`jack_get_sample_rate`); a PipeWire desktop that runs its
graph at 44.1 kHz needs `default.clock.rate = 48000` in its config, or an engine created at the
graph's rate.

### 7. Thread

Who owns the audio thread, and what to do about its priority:

| backend   | thread                          | priority                                                                 |
|-----------|---------------------------------|--------------------------------------------------------------------------|
| WASAPI    | the sink's own, event-driven    | `AvSetMmThreadCharacteristics(L"Pro Audio")` (link `avrt`); revert on exit |
| CoreAudio | the HAL's IO thread             | already a time-constraint thread. Do nothing                             |
| JACK      | the server's process thread     | already `SCHED_FIFO`; the server started it. Do nothing                 |
| ALSA      | the sink's own, blocking writes | try `SCHED_FIFO` through `pthread_setschedparam`; on `EPERM` run at normal priority and report the degradation (needs `RLIMIT_RTPRIO` or the `audio` group; document in build.md) |
| AAudio    | AAudio's callback thread        | already elevated under `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`. Do nothing |
| Wasm Audio Worklet | the browser's audio rendering thread | already the highest-priority context a page can get, and a browser offers no knob. `os_thread_set_realtime` returns `ENOTSUP` on wasm anyway (docs/web.md, the OS shim) |

The sink's thread, where it has one, does exactly what `null_sink.c`'s does: wait, stamp,
render, convert, hand over, account. Name it with `BWA_THREAD_NAME` so Tracy shows it.
COM-based backends initialize COM on that thread (`COINIT_MULTITHREADED`) and honor
`RPC_E_CHANGED_MODE` on the control thread, because the GUI tools already initialize COM their
own way.

### 8. Teardown

`close()` returns only after the last callback has returned and no further one can start. The
ASIO sink's order (stop the device, dispose the buffers, then clear the global and free) is the
model; each API has its own equivalent (`AudioDeviceStop` then `AudioDeviceDestroyIOProcID`;
`jack_deactivate` then `jack_client_close`; `AAudioStream_requestStop` then
`AAudioStream_close`; join the thread). `engine_close_devices`
relies on this to free the `cave_both` handoff buffers safely.

### 9. Names

`backend()` returns `"<backend>:<device name>"`, ASCII backend, UTF-8 device name from the OS
(WASAPI's wide strings and CoreAudio's `CFString` convert at the seam, as the ASIO sink already
converts from the ANSI codepage). The engine's `backend_buf` holds a long WASAPI endpoint name
plus the decode suffix. Fixed strings stay ASCII per the repo rule.

### 10. Selection semantics

A `device` string is matched **exactly** against what `bwa_get_device_name` returned, then
exactly against the stable id from `bwa_get_device_id`. No substring matching: two endpoints
called "Speakers" are common, and a picker passes exact strings. Under `BWA_SINK_AUTO` with a
device name, a backend that has no device by that name is **skipped**, not opened on its
default device, so the name reaches the backend that owns it.

JACK has ports, not devices. There `device` is a port-name regular expression for
`jack_get_ports`, such as `system:playback_` or `RAVENNA:playback_`; NULL connects to the
physical playback ports in order. The connections are a starting point: the patchbay can
rewire them live, and the sink never reasserts them.

## Shared pieces

Every backend builds on these. They are pure, so they get the hardware-free tests the backends
cannot.

### `src/sink/sink_convert.h`

Planar float in, one of {float32, int32, int24 packed, int16} out, planar or interleaved, with
one set of NaN and clamp rules. The ASIO sink calls the planar variants. Test: every format times
both layouts, the clamps, NaN to 0, and a 26-channel interleave round trip.

### `src/sink/sink_quant.h`, `src/sink/sink_quant.c`

The fixed-quantum adapter. Owns: the engine block size `B`, the channel count, a FIFO of
rendered blocks, the render function, the health counters the adapter can measure
(`blocks`, `late_blocks`, `render_ns_peak`, `period_ns`), and the stream position.

One entry point for the backend's callback:

```
quant_pull(q, n, device_pos_now, device_pos_valid, host_now_ns, out_fn)
    while held < n:
        ts = { sample_pos = q.rendered, system_time_ns = host_now_ns + (q.rendered - q.written) * 1e9 / rate }
        render(B frames) into the next FIFO slot, timing it
        q.rendered += B
    for each contiguous run of the held frames:      # the backend converts + interleaves
        out_fn(run pointer, stride, frame_offset, run length)     # straight from the FIFO
    pop(n); q.written += n
```

**The FIFO is a ring of block-sized slots, not of frames.** `render()` fills a planar bus whose
channel stride IS `nframes`, so a block only lands somewhere without a copy if that somewhere has
a block-sized stride. Slots do; a frame-indexed ring does not, and would cost a per-channel copy
per block. The price is that a device request can span slots, so `out_fn` takes a frame offset
and is called once per contiguous run. Capacity is `ceil(n_max / B) + 1` slots, allocated at
open, where `n_max` is the device's buffer size.

When `n == B` and the FIFO is empty, the pull renders one slot and delivers it in one run with no
residue: the pass-through, which falls out of the general path rather than needing its own, and is
counted so a backend that pins the device size can see it paid nothing.

The dropout rule for position-reporting devices lives beside it: `depth = q.written -
device_pos_now` must sit at or above 0; a negative depth is a dropout of `-depth` frames, after
which the adapter re-anchors to `device_pos_now` the way the ASIO sink takes the driver's position
as the truth. It re-anchors `q.rendered` by the same amount, so the frames held in the FIFO stay
exact and the next block's timestamp lands on the device's stream. The gap goes through
`sink_position_gap`, so a backward or absurd jump is a reset rather than a dropout. Around 170
lines.

Test, in `test_sink_quant`: device requests of `B/3`, `B`, `2.5 B`, and 1 frame, in mixed
sequences. Assert every render call got exactly `B`; the device output is the exact
concatenation of the rendered blocks, nothing duplicated or dropped; each rendered block's
`sample_pos` equals the device frame index its first sample landed on; the pass-through path
is bit-identical to the FIFO path. Then break it on purpose (an off-by-one in the pop) and
confirm the test goes red before trusting it, per the CLAUDE.md trap.

### `src/os/os.h`, `src/os/os_win.c`, `src/os/os_posix.c`

**Implemented (phase 2).** The portability shim. Every backend off Windows needs it, because it
is what lets the whole library build there. This is the Win32 surface it replaces, and where each
call lived:

| Win32 use                                                   | files                                                              | shim                                  |
|-------------------------------------------------------------|--------------------------------------------------------------------|---------------------------------------|
| `CreateThread`, `WaitForSingleObject`, `CloseHandle`        | assets.c, stream.c, steam_scene.c, steam_path.c, steam_reflect.c, natnet.c, null_sink.c | `os_thread_create/join`               |
| `Sleep`                                                     | engine.c, assets.c, stream.c, steam_*.c, natnet.c, null_sink.c     | `os_sleep_ms`                         |
| `QueryPerformanceCounter/Frequency`                         | null_sink.c, asio_sink.cpp, natnet.c, profile_self.c               | `os_monotonic_ns`                     |
| `CRITICAL_SECTION`                                          | stream.c, steam_scene.c, steam_path.c, profile_self.c              | `os_mutex`                            |
| `Interlocked*` on `volatile LONG`                           | engine.c (the hpeq handoff, the `cave_both` seqlock), assets.c, stream.c, steam_*.c, natnet.c, null_sink.c, profile_self.c | C11 `<stdatomic.h>`; MSVC already gets `/experimental:c11atomics` per file |
| `_InterlockedExchange` intrinsics                           | pose.h                                                             | rewrite the seqlock on C11 atomics with acquire and release |
| `SetThreadPriority(BELOW_NORMAL)`                           | steam_scene.c, steam_path.c, steam_reflect.c                       | `os_thread_lower_priority`            |
| `AvSetMmThreadCharacteristics(L"Pro Audio")`                | wasapi_sink.cpp (was inline)                                       | `os_thread_set_realtime` / `_clear_realtime` |
| `timeBeginPeriod`                                           | null_sink.c                                                        | Windows-only inside the shim, no-op elsewhere |
| Winsock (`WSAStartup`, `socket`, `recvfrom`, `closesocket`) | natnet.c                                                           | a thin socket shim; BSD sockets need no startup and `close` |
| `_strdup`, `_stricmp`                                       | engine.c, assets.c, stream.c, sound.c                              | `os_strdup`, `os_strcasecmp`          |
| `__declspec(dllexport)` on internal test hooks              | sink.h, profile_self.h                                             | one `BWA_EXPORT` macro: `__attribute__((visibility("default")))` off Windows, with `-fvisibility=hidden` on the library |

Three more are not portability moves. They are in the shim because the shim is where a platform
difference belongs:

| need                                                        | files                                                              | shim                                  |
|-------------------------------------------------------------|--------------------------------------------------------------------|---------------------------------------|
| a UTF-8 path that opens on Windows                          | layout.c, hpeq.c, calib.c, zylia.c, sound.c, stream.c              | `os_fopen`, `os_mkdir`, `os_remove`, `os_rmdir`, and `os_utf8_to_wide` for the dr_libs `_w` openers |
| a worker that waits instead of polling                      | assets.c, stream.c                                                 | `os_event` (`_init`/`_destroy`/`_signal`/`_wait`) |
| a device library loaded at run time                         | jack_sink.c, alsa_sink.c                                           | `os_dl_open`, `os_dl_sym`, `os_dl_close` |

**`os_fopen`** is the whole UTF-8 path story. The ABI speaks UTF-8, but the Windows C runtime reads
a narrow path in the process ANSI codepage, so a path with an accent or a CJK character fails to
open through plain `fopen`. `os_fopen` converts to UTF-16 and calls `_wfopen`; off Windows it is `fopen`, since
the bytes there already mean what they say. Every `fopen` in `src/` goes through it, and the three
dr_libs decoders take their `_w` openers on Windows through `os_utf8_to_wide`, which converts into
a caller buffer so no path conversion allocates. One path does not go through it: `bwa_desc.hrtf_path`
is handed to phonon, which opens the SOFA file itself, so its encoding is the SDK's problem.
`test_utf8_path` writes a wav, a layout, and a headphone EQ file into a directory whose name carries
both an accented Latin letter and a CJK character, then loads all four through the real loaders.

**`os_dl_*`** is `dlopen` with `RTLD_NOW | RTLD_LOCAL` on POSIX and `LoadLibraryW` on Windows, and
it is control-thread only, because opening a library is file I/O. It exists for the two Linux
device backends, which load `libjack.so.0` and `libasound.so.2` themselves rather than linking
them. `RTLD_NOW` makes a library that is missing one of the symbols a backend calls fail the load
instead of aborting on the first call into it, and `RTLD_LOCAL` keeps what the library drags in out
of the global symbol namespace. Nothing on Windows calls it yet; the Windows half is there so the
shim has one API rather than a Linux-only corner.

**`os_event`** is an auto-reset event: one signal releases one waiter, and a signal that arrives
before anyone waits is remembered, so a worker can check its ring, find it empty, and then wait
without losing a push that landed in between. Windows gets `CreateEventW` plus
`WaitForSingleObject`; elsewhere it is a mutex, a condvar and a flag, with the timed wait on
`CLOCK_MONOTONIC` (`pthread_condattr_setclock`, or `pthread_cond_timedwait_relative_np` on Apple)
so a wall-clock step cannot stretch or cut a wait. Spurious wakeups are absorbed: a `false` return
means the timeout really elapsed.

Two threads use it, and they use it differently:

- **The asset loader** (`assets.c`) waits forever on an empty job ring. The control thread signals
  after every job push and once at stop. Idle costs zero wakeups, and an async acquire is picked up
  at once rather than at the next poll.
- **The stream thread** (`stream.c`) cannot be purely event-driven, because its consumer is the
  AUDIO thread and the audio thread may not signal (rule 1). So it waits with a timeout derived
  from the rings: the shortest time any fillable ring could drain to empty at the engine rate,
  floored at 1 ms and capped at 3 ms, the old flat cadence. An active stream is therefore never
  served later than it was before; a ring that just reset is served sooner. With no fillable stream
  open the wait is infinite, which is the whole saving. A push stream is fed by the control thread
  and a finished one has nothing left to read, so neither counts as fillable; both still get reaped
  on close, which signals. The control thread signals on open, start, close and stop.

`test_idle` pins both: with nothing loading and nothing streaming, each thread's loop-iteration
count must stay under 5 across 500 ms.

Tests may include `src/os/os.h`, because they compile the core in. Examples are client code of the
public ABI, so they use their own two-function `examples/portable.h` instead of reaching into
`src/`.

Rules the shim carries that the inventory does not show:

- **`os_rwlock` is not `os_mutex`.** `steam_scene.c` guards the committed `IPLScene` with a
  reader/writer lock: several sim threads borrow the scene for concurrent ray traces while the
  owner takes it exclusively around `iplSceneCommit`. A plain mutex would serialize traces that
  have no reason to wait on each other. `os_rwlock` is SRWLOCK on Windows and `pthread_rwlock_t`
  elsewhere.
- **`pose.h` is not C++-visible.** Its payload fields are `_Atomic`, and `_Atomic` is not C++.
  `pose.h` gives C++ the type as opaque, and C++ code (`examples/validate.cpp`) reads the pose
  through `natnet_read_pose`. `rt.h` and `natnet.h` forward-declare `PoseSlot` rather than
  including `pose.h`, which keeps `<stdatomic.h>` (and MSVC's `/experimental:c11atomics`) off the
  translation units that only pass the slot around by pointer.
- **`bwa_null_sink_skip_blocks` is a plain `volatile int`.** It is declared in `sink.h`, which the
  two C++ sinks include, so it cannot be an `_Atomic`. It has one test-thread writer and one
  sink-thread read-and-clear on an aligned `int`, and nothing depends on its ordering.
- **Path normalization is Windows-only.** `assets.c` folds case and backslashes into the cache key
  on `_WIN32` only. On a POSIX filesystem `A.wav` and `a.wav` are different files and a backslash
  is an ordinary character, so folding there would hand one cache entry to two files. The test
  section that pins the folds skips off Windows.
- **`os_thread_set_realtime(period_ns)` means three things.** Every self-paced render thread calls
  it at its top. Windows joins the "Pro Audio" MMCSS task. Linux asks for `SCHED_FIFO`, which can
  be REFUSED, so `os_thread_realtime_available()` exists beside it: the ALSA sink has to report
  that degradation through the open's `err` channel, which is gone by the time its render thread
  exists. Apple sets `THREAD_TIME_CONSTRAINT_POLICY`, and there it is about timers, not priority:
  Darwin coalesces timers for threads without one, so a correct `mach_wait_until` lands
  milliseconds late on an ordinary thread. `period_ns` exists for Apple alone.
- **Every sink stamps the absolute platform clock, never a private epoch.** `rt.c`'s publish gate
  reads a zero stamp as "no stamp", and a stamp relative to a start time captured a moment earlier
  can come out exactly 0 on a coarse clock (Apple Silicon's `mach_absolute_time` ticks at
  41.67 ns). The null sink keeps its start time for the deadline arithmetic only.

#### The absolute-deadline sleep

`os_sleep_until_ns(deadline_ns)` waits until a deadline on the `os_monotonic_ns` clock and returns
at once if that time has passed. It exists because a SELF-PACED render loop cannot pace accurately
with a relative sleep: the duration is computed from a clock reading that is already stale when the
kernel sees it, so the error accumulates block after block.

| platform | primitive |
|----------|-----------|
| Windows 10 1803 and later | `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`, one timer per thread in fiber-local storage so a thread exit closes it, then `SetWaitableTimerEx` with a negative relative due time and `WaitForSingleObject` |
| Windows before 1803 | the create call fails with `ERROR_INVALID_PARAMETER`; fall back to `timeBeginPeriod(1)` plus `Sleep`, which is the only path that still touches the global timer resolution |
| Linux, Android | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`, retried on `EINTR` |
| macOS | `mach_wait_until` on the deadline converted back to mach ticks through `mach_timebase_info` |

The point of the high-resolution timer is not only precision. `timeBeginPeriod` is system-wide in
effect, so a loop that raised it would hold the whole machine at a 1 ms timer tick for as long as
any tool had the offline sink open. No sink raises it.

Only SELF-PACED paths use this: the null sink's block loop, and the host-paced loops that take
over after a device is lost (WASAPI's `host_paced_block`, ALSA's `alsa_host_paced_block`, and the
JACK, AAudio and worklet host-paced threads). Device-paced paths keep their own wait, because the
device is the clock there: the WASAPI event wait, the ASIO callback, ALSA's blocking write, the
JACK process callback, the AAudio data callback and the worklet's `process()`.

`test_os` measures it: 200 wakes 2 ms apart, reporting median and p99 lateness. A desk Windows
machine gave 0.43 ms and 0.74 ms; WSL2 gave 0.13 ms and 0.33 ms. Forcing the pre-1803 fallback with
the resolution left alone gave 5.6 ms and 13.0 ms, which is what the p50 assertion is there to
catch.

It measures ON A THREAD THAT ASKED FOR REAL-TIME, because that is how the sinks use the primitive,
and on Darwin measuring anywhere else measures timer coalescing instead of the sleep. The Apple
bound is deliberately gross, p50 under 20 ms and p99 under 50 ms: the tight bound exists to catch
Windows losing its high-resolution timer path, Darwin has no counterpart to lose, and the only
Apple data point anyone has is a virtualized CI runner. Tightening it wants a physical Mac, which
is on the [verify list](./backend-development.md#verify-before-relying-on-it).

## ABI changes

Enum values are appended, the desc keeps its layout, and the bindings mirror the changes (for
example `bindings/unity/Runtime/Bwa.cs` and `bindings/godot/src/bwa_engine_node.h`).

### `bwa_sink_type`

```c
typedef enum { BWA_SINK_AUTO = 0, BWA_SINK_ASIO = 1, BWA_SINK_NULL = 2, BWA_SINK_MANUAL = 3,
               BWA_SINK_WASAPI = 4, BWA_SINK_COREAUDIO = 5, BWA_SINK_ALSA = 6, BWA_SINK_AAUDIO = 7,
               BWA_SINK_JACK = 8, BWA_SINK_WORKLET = 9,
               BWA_SINK_FORCE_U32 = 0x7FFFFFFF } bwa_sink_type;
```

An explicit backend fails `bwa_start` loudly when it cannot open, exactly as `BWA_SINK_ASIO`
does. A backend not compiled in fails with a message that says so. `bwa_get_sink_type` returns
the concrete backend after `bwa_start`.

### `bwa_desc`

Two changes, no layout change:

```c
union { const char* device; const char* asio_driver; };   /* asio_driver: the legacy spelling */
uint32_t sink_flags;        /* carved from reserved[0]; 0 = default */
uint32_t reserved[3];
```

`device` names the device for whichever backend opens, a friendly name or a stable id. NULL is
the backend's default device (ASIO: the first driver with enough outputs). The anonymous union
keeps `asio_driver` compiling and keeps the C# and GDExtension field offsets.

`sink_flags` carries four bits, all opt-in and all describing the PRIMARY device only:

| bit | value | effect |
|-----|-------|--------|
| `BWA_SINK_FLAG_EXCLUSIVE` | 0x1 | WASAPI opens in exclusive mode, AAudio requests exclusive sharing. Off by default, because exclusive mode takes the device from every other application on a desk machine, and shared mode with an `IAudioClient3` period is low-latency enough for a monitor |
| `BWA_SINK_FLAG_EXACT_RATE` | 0x2 | rule 6's array strictness on a headphone sink: the open fails naming both rates rather than accepting the OS resampler. Folded into `bwa_sink_open`'s `exact_rate` argument, so no backend reads the bit |
| `BWA_SINK_FLAG_TIGHT_BUFFER` | 0x4 | rule 5's smallest device buffer: ALSA 2 periods instead of 3, AAudio 1 burst instead of 2. A no-op on WASAPI, ASIO and JACK |
| `BWA_SINK_FLAG_DEEP_BUFFER` | 0x8 | a deeper output buffer: more latency, more slack for a render that overruns its callback. Only the worklet sink reads it (latencyHint `"playback"` on a context the sink creates); a no-op everywhere else |

`docs/api.md` maps PsychPortAudio's latency classes 0 to 4 onto these flags and a block size.

`bwa_sink_health` (internal, `src/sink/sink.h`) carries `device_lost` too, because the flag has to
reach `engine.c` from the sink. It is not ABI.

### AUTO order

Per platform, and on Windows per requested channel count:

| platform | 2 channels                     | more than 2 channels        |
|----------|--------------------------------|-----------------------------|
| Windows  | WASAPI, ASIO, null             | ASIO, null                  |
| macOS    | CoreAudio, null                | CoreAudio, null             |
| Linux    | JACK, ALSA, null               | JACK, ALSA, null            |
| Android  | AAudio, null                   | null                        |
| Browser  | Wasm Audio Worklet, null       | null                        |

The macOS entry is in `sink.c` already, but no CoreAudio sink is compiled until phase 4, so a Mac
opens the null sink today. Each backend is in the order only when the build carries it.

A headphone profile on Windows under AUTO opens the Windows default output, not the first
registered ASIO driver. That is the device the headphones are on, which an ASIO pick cannot know
(it could land on the Digiface and play the monitor into Dante channels 1 and 2).
`sink = BWA_SINK_ASIO` or a `device` naming an ASIO driver takes ASIO, by rule 10. AUTO never
chooses multichannel WASAPI: that would be an exclusive grab of a WDM device, and the locked
decision says the array goes over ASIO.

On Linux, `jack_client_open` gets `JackNoStartServer`, so a box with neither a JACK server nor
PipeWire falls through to ALSA at once instead of spawning a `jackd`.

`cave_both` on Windows resolves on its own: the array request takes ASIO, the monitor
request takes WASAPI, and the adapter makes the monitor's block size equal the array's, so the
"same buffer size" check in `bwa_start` passes. `bwa_start` opens the monitor for the **array's
resolved block**, not `cfg.block_size`, which is the part that makes that true when the ASIO
driver picks a size of its own.

That only works because **`device` and `sink_flags` describe the primary device only**. The
monitor opens with `BWA_SINK_AUTO`, no device name and no flags, so it takes the platform's
default stereo device. Passing the array's `device` down to it re-creates the exact defect this
backend was added to fix: on the rig `device` names the ASIO driver, rule 10 skips a backend
that has no device by that name, so the monitor's 2-channel AUTO request skips WASAPI, asks
ASIO for a driver whose one process-wide slot the array already holds, and falls to the silent
null sink. Under `BWA_SINK_ASIO` it fails the start outright.

The exception is a **requested** `BWA_SINK_NULL` or `BWA_SINK_MANUAL`: the monitor takes the
same sink, so CI and an offline render stay device-free. The rule reads the sink you asked for,
not the one that opened. An AUTO array whose device is missing falls to the null sink, and
reading the resolved sink there would take the monitor down with it, which is silence on the one
configuration where hearing the sim is how you find out the array never opened.

`bwa_get_audio_backend` names both devices under this profile, array first:
`asio:Digiface Dante + wasapi:Speakers (Realtek(R) Audio) (steam HRTF sim)`. It is
human-readable only. Program logic reads `bwa_get_sink_type`, which reports the array.

To pin the monitor's endpoint you need a `monitor_device` field, which does not exist yet. See
[backend-development.md: Deferred follow-ups](./backend-development.md#deferred-follow-ups).

### Device query

```c
BWA_API uint32_t bwa_get_device_count(bwa_sink_type backend);
BWA_API bool     bwa_get_device_name(bwa_sink_type backend, uint32_t index, char* buf, uint32_t cap);
BWA_API bool     bwa_get_device_id  (bwa_sink_type backend, uint32_t index, char* buf, uint32_t cap);
```

Control thread, no engine, nothing opened, as the ASIO pair promises today. `backend` must be
concrete: AUTO, NULL, and MANUAL report 0. Index 0 is the platform default device where the
backend has one (WASAPI, CoreAudio, AAudio); ALSA has no default beyond the PCM named
`default`, which it lists first. `bwa_get_asio_driver_count` and `_name` stay as wrappers over
`BWA_SINK_ASIO`, because both bindings and the tools call them.

Per backend: WASAPI enumerates `IMMDeviceEnumerator::EnumAudioEndpoints(eRender,
DEVICE_STATE_ACTIVE)` with `PKEY_Device_FriendlyName` and the endpoint id string. CoreAudio
lists `kAudioHardwarePropertyDevices` filtered to devices with output streams, with
`kAudioObjectPropertyName` and `kAudioDevicePropertyDeviceUID`. ALSA walks
`snd_device_name_hint(-1, "pcm", ...)` for output hints, name and description. JACK lists the
client prefixes of the physical playback ports (`system`, `RAVENNA`), one device each, so a
picker offers the card rather than one entry per port. AAudio has no
C enumeration: it reports one device, "default"; `device` also accepts a decimal Android
device id obtained from Java's `AudioManager`, passed to `AAudioStreamBuilder_setDeviceId`.

### `bwa_health`

`uint32_t device_lost` appended (rule 4). The struct has no reserved fields, so this is the
layout change the minor bump covers.

## Per-backend notes

The parts of each API that are not obvious, and the decisions taken.

### WASAPI

Files: `src/sink/wasapi_sink.cpp` (COM is nicer from C++, and the DLL already compiles C++20 for
the ASIO sink). Link `ole32`, `avrt`. Define `BWA_HAVE_WASAPI`. Windows 10 1607 or later for
`IAudioClient3`; fall back to `IAudioClient` on older builds.

Open sequence, shared mode (the default):

1. `IMMDeviceEnumerator` to the endpoint (`GetDefaultAudioEndpoint(eRender, eConsole)` or the
   selected one), `Activate(IAudioClient3)`.
2. Format: `WAVEFORMATEXTENSIBLE`, float32, the engine rate, the requested channel count.
   `IsFormatSupported(SHARED)` with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` in the flags lets a
   mismatched engine rate through with a warning (rule 6). Shared mode is stereo or the
   endpoint's own channel layout only; an array-width request in shared mode fails at open with
   a message pointing at exclusive mode.
3. Period: `GetSharedModeEnginePeriod` gives default, fundamental, min, max. Choose the engine
   block size when it is in range and a multiple of the fundamental, else the minimum. A
   period that is not the block size is fine: the adapter bridges it.
4. `InitializeSharedAudioStream(EVENTCALLBACK | AUTOCONVERTPCM | SRC_DEFAULT_QUALITY, period,
   fmt, NULL)`, `SetEventHandle`, `GetBufferSize`, `GetService(IAudioRenderClient)`,
   `GetService(IAudioClock)`.
5. Prefill the buffer with silence and `Start` on `start()`.

Exclusive mode (`BWA_SINK_FLAG_EXCLUSIVE`): try float32, int32, int24, int16 through
`IsFormatSupported(EXCLUSIVE)`; `GetDevicePeriod` for the minimum; `Initialize(EXCLUSIVE,
EVENTCALLBACK, hns, hns, fmt, NULL)` with the buffer duration equal to the period. On
`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`, recompute the duration from `GetBufferSize`, release the
client, and initialize again. Exclusive mode gives a fixed callback size, so the adapter runs
in pass-through when it equals the block size, and multichannel is possible. The array
through WASAPI stays a possibility, not a recommendation.

`BWA_SINK_FLAG_TIGHT_BUFFER` does nothing on this backend, deliberately. The exclusive path
already initializes with the buffer duration equal to the device period, which is the smallest
buffer WASAPI will accept, and a shared-mode buffer is the OS mixer's. `BWA_SINK_FLAG_EXACT_RATE`
needs no code either: it arrives folded into `exact_rate`, and the shared path already refuses a
resampled rate that way. What one desk endpoint gives, measured with
`bwa_get_output_latency_frames` at 48 kHz, is in `docs/api.md` under "Latency classes".

PC VR: a headset's audio (the Oculus or SteamVR endpoint) is a shared-mode WASAPI endpoint.
The runtime can make it the default output; otherwise `device` names it, by friendly name or
by the stable id, since two headsets or a headset and a dock produce duplicate friendly names.
Shared mode matters here: the runtime keeps its own audio open on the same endpoint.

The thread: wait on the event with a timeout of a few periods (a timeout means the device
stalled; count it and keep pacing from the host clock, rule 4), `GetCurrentPadding` (shared)
to size the request, `GetBuffer`, `quant_pull` converting straight into it, `ReleaseBuffer`, then
the release-interval check of rule 4.

`GetCurrentPadding() == 0` at a wake is **not** the shared-mode underrun test, though it reads like
the obvious one. This client refills the buffer to full on every event, and a starve leaves the
stream event already signaled, so the catch-up wake returns at once and reports the frames just
written rather than an empty buffer. Measured on a live endpoint: a render stalled past two full
buffers produced zero such wakes, and the release-interval rule caught it on the first cycle.

`AUDCLNT_E_DEVICE_INVALIDATED` from any of these sets `device_lost`. An `IMMNotificationClient`
is not required for that; it only adds "the default device changed", which the sink ignores
(rule: no automatic recovery).

CI: GitHub's Windows runners have no audio endpoint, so `GetDefaultAudioEndpoint` fails with
`E_NOTFOUND`. The sink must fail cleanly there, and the sink test skips (see
[Tests](#tests)).

### CoreAudio

Not implemented yet (phase 4). The planned design is in
[backend-development.md: CoreAudio](./backend-development.md#coreaudio).

### JACK

Files: `src/sink/jack_sink.c`. `pkg-config jack` in CMake for the HEADERS only. The same binary
runs on JACK2 (`libjack`, LGPL-2.1) and on PipeWire (pipewire-jack's drop-in `libjack`, MIT);
which one answers is decided at run time by what the box has installed. Define `BWA_HAVE_JACK`.

The library is not linked. The sink loads `libjack.so.0` with `os_dl_open` on the first device
query or open, resolves every symbol it calls into one table, and keeps it for the life of the
process. So `libbw_audio.so` names neither `libjack` in its `NEEDED` list nor any `jack_` symbol
in its undefined symbols, and a box with no JACK loads the engine and runs the offline sink.
pipewire-jack ships the same soname, so nothing about "which server answers" changes.

A library that will not load is exactly NO DEVICE:

- `bwa_get_device_count(BWA_SINK_JACK)` returns 0, so rule 10 skips the backend under AUTO the
  same way a missing device name does.
- An explicit `BWA_SINK_JACK` open fails, and the message names the library it could not load.
  Under AUTO the reason travels out on the degradation channel when nothing else opens either,
  so a caller that lands on the offline sink can read why.
- The attempt is made again on the next open. A library installed after a failed attempt is
  found without restarting the process.

The headers stay a build-time requirement: every type, enum and callback signature comes from
them, and the symbol table is generated from one X-macro list, so the typedefs, the table and the
resolve loop cannot drift apart.

This is the smallest sink of the set and the closest to ASIO: a fixed-size callback, float
non-interleaved ports that match the bus layout, an xrun callback, a filtered time pair, and
routing to any hardware channel from the host's tools.

Open sequence:

1. `jack_client_open("bw_audio", JackNoStartServer, &status)`. A failure here under AUTO is
   the fast path to ALSA. `jack_get_sample_rate` must equal the engine rate (rule 6).
2. `jack_get_buffer_size` is the period. Equal to the block size means pass-through; anything
   else the adapter bridges. Register `jack_set_buffer_size_callback`; a change resizes the
   adapter's request (its FIFO is allocated for `BWA_MAX_BLOCK` up front, so the callback
   allocates nothing). The sink never calls `jack_set_buffer_size`: on pipewire-jack that
   forces the global quantum for every application, which is the rig's decision to make in its
   PipeWire config (`default.clock.quantum`), not the engine's.
3. Register one output port per bus channel, `out_01` to `out_NN`, `JACK_DEFAULT_AUDIO_TYPE`,
   `JackPortIsOutput`. Register the process, xrun, and shutdown callbacks.
4. `jack_activate`, then connect: `jack_get_ports(device regex, JACK_DEFAULT_AUDIO_TYPE,
   JackPortIsInput)`, physical ports when `device` is NULL, in order, as many as the sink has.
   Fewer matching ports than channels is an error naming both counts.
5. Read the playback latency range on the ports after connecting (rule 5).

The process callback: `jack_get_cycle_times` for the pair, `quant_pull` with `nframes`,
copying each block channel into `jack_port_get_buffer` of its port. The xrun callback counts
one dropout and converts `jack_get_xrun_delayed_usecs` to frames. The shutdown callback (the
server went away) sets `device_lost` and starts the host-paced thread, per rule 4.

The PipeWire graph rate and quantum are pinned in [build.md](./build.md), under "Linux notes".
Note also that PipeWire links ports across devices with its own resampling, which would let
`cave_both` be one client (the array's ports plus two) on Linux later; the two-sink design stands for now.

Behavior worth knowing. The reasons are in
[backend-development.md](./backend-development.md#jack-what-the-implementation-settled).

- The client activates and connects at `open()`, so a port-count error reaches the caller
  verbatim. `start()` only lifts a gate; until then the process callback writes silence.
- The xrun callback runs on a notification thread. It parks the event and the frame estimate in
  two atomics, and the next process callback folds them in through `sink_quant_note_dropout`.
- `quant_pull` is called with `device_pos_valid` clear, and `measured` comes from the xrun route.
- The adapter's FIFO is allocated for 8192 frames at open. The buffer-size callback only records
  the new size; a cycle larger than the ceiling is silenced past it and booked as a dropout.

### ALSA

Files: `src/sink/alsa_sink.c`. `find_package(ALSA)` in CMake for the HEADERS only. alsa-lib is
LGPL-2.1, so the license inventory in build.md gains one line and nothing else changes. Define
`BWA_HAVE_ALSA`.

The library is not linked, for the reason the JACK section gives above and with the same
semantics: the sink loads `libasound.so.2` at the first device query or open, an absent library
is exactly no device, the failure message names the library, and the next open tries again.

Two things ALSA adds. The `snd_pcm_hw_params_alloca` family is a macro that expands to a
`snd_*_sizeof()` call, which is an ordinary library symbol, so the sink spells the macro itself
over its table. And `snd_strerror` is a library symbol too, although it reads like `strerror`.

Open sequence, all through `snd_pcm_hw_params_*`:

1. `snd_pcm_open(name, SND_PCM_STREAM_PLAYBACK, 0)`. `default` when `device` is NULL.
2. Access `SND_PCM_ACCESS_RW_INTERLEAVED`. Format in the rule 2 order, through
   `snd_pcm_hw_params_test_format`.
3. Rate: `snd_pcm_hw_params_set_rate_resample(0)` for a `hw:` device and for any device under
   `exact_rate`, then `set_rate` exact. A `plug` or `default` PCM under a headphone open may
   resample; report it (rule 6).
4. Channels exact. Period size near the block size; buffer size near three periods, or two under
   `BWA_SINK_FLAG_TIGHT_BUFFER`. Read both back: `set_buffer_size_near` is a request, and a
   server PCM can bound it. The ALSA PulseAudio plugin returns three periods either way,
   measured, while the hardware-free `null` PCM grants two. The write loop writes whole engine
   blocks, so the adapter is not needed: the period is a device-side number and the sink's
   `block_size()` stays the engine's.
5. sw_params: `set_tstamp_mode(SND_PCM_TSTAMP_ENABLE)` and
   `set_tstamp_type(SND_PCM_TSTAMP_TYPE_MONOTONIC)` so `snd_pcm_status_get_htstamp` is on the
   same clock as `os_monotonic_ns`. `set_start_threshold` to one buffer so the stream starts
   full.

The thread: render one block, convert and interleave into a preallocated frame buffer,
`snd_pcm_writei` (blocking; the wait is the pacing), then `snd_pcm_status` for the pair and
the delay. `-EPIPE` is an underrun: count it, estimate the frames from the elapsed time, and
`snd_pcm_prepare`. `-ESTRPIPE` is a suspend: `snd_pcm_resume` until it stops returning
`-EAGAIN`, then `prepare`, and tick `driver_resyncs`. Any other negative result sets
`device_lost` and the loop keeps pacing from the host clock.

PipeWire and PulseAudio: their ALSA plugins answer `default`. Latency and timestamps are then
theirs and are reported as they come. That is enough for a desk monitor, and on a PipeWire box
AUTO has already tried JACK first. A rig on Linux uses a `hw:` device: a MADI or ADAT card, or
the AES67 daemon's RAVENNA card, which is an ordinary ALSA PCM to the sink.

Behavior worth knowing. The reasons are in
[backend-development.md](./backend-development.md#alsa-what-the-implementation-settled).

- Each block's rule 3 pair comes from the PREVIOUS block's status: host time
  `htstamp + delay / rate`. The host clock stands in before the first status and after a
  restart, and no stamp steps backward.
- `S24_LE` (24 bits in a 32-bit container) is written by the sink itself, from
  `sink_to_i32(v) >> 8`. The clamp and NaN rules stay in `sink_convert.h`.
- The open asks `os_thread_realtime_available()` (it reads `RLIMIT_RTPRIO`); the render thread
  makes the `os_thread_set_realtime()` attempt.
- `output_latency()` reports the buffer size until the first successful write latches the
  measured delay.
- `stop()` joins the render thread before it touches the PCM.
- The device query lists `default` at index 0 whether or not a hint names it.

### AAudio

**Implemented (phase 3).** Files: `src/sink/aaudio_sink.c`. Link `aaudio`. `ANDROID_PLATFORM` 26 or
later in the NDK toolchain. Define `BWA_HAVE_AAUDIO`.

Builder: output direction, `AAUDIO_SHARING_MODE_SHARED` (exclusive under the flag; the OS
grants it rarely), `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`, `AAUDIO_FORMAT_PCM_FLOAT`, the
requested channel count, the engine rate, `setFramesPerDataCallback(block size)` as the hint it
is, a data callback and an error callback. After open, the buffer size is set to two bursts
(`AAudioStream_setBufferSizeInFrames(2 * getFramesPerBurst())`) and the actual value read back
for rule 5. `BWA_SINK_FLAG_TIGHT_BUFFER` asks for ONE burst there instead, which keeps no margin
at all: every late callback is then an xrun. The emulator grants it (960 frames instead of 1920),
and the read-back is what says whether a given device did.

The data callback calls `quant_pull` with the frame count it was given and returns
`AAUDIO_CALLBACK_RESULT_CONTINUE`. Position and time come from `AAudioStream_getTimestamp`,
read once per callback and tolerated when it fails early. Dropouts are the OS's own count,
`AAudioStream_getXRunCount`, read as a delta per callback. The error callback receives
`AAUDIO_ERROR_DISCONNECTED` on a route change; AAudio forbids reopening from that thread, so
the sink only sets `device_lost` and the callback thread ends. The sink then starts a host-paced
thread (the null-sink loop) so the engine keeps running, per rule 4.

Behavior worth knowing. The reasons are in
[backend-development.md](./backend-development.md#aaudio-what-the-implementation-settled).

- Stereo only: an explicit multichannel `BWA_SINK_AAUDIO` open fails with a message naming the
  width.
- `setUsage` is never called, so the usage stays `AAUDIO_USAGE_MEDIA`.
- `quant_pull` is called with `device_pos_valid` clear; dropouts are `getXRunCount` deltas.
- The device's own rate is probed with a throwaway stream before the real open, so a resampled
  shared-mode stream is reported and a failed open names both rates.

`output_latency()` is latched once, not recomputed: `getFramesWritten` minus the presented
position from the first successful timestamp, extrapolated to now at the nominal rate, plus what
the adapter holds. `sink.h` promises the value stays constant for the life of the sink, so the
buffer-size estimate from the open stands until that first timestamp replaces it.

What the emulator verified is in
[backend-development.md](./backend-development.md#aaudio-on-the-emulator).

Consumers: standalone VR headsets (Meta Quest, Pico) through a Godot Android export
(GDExtension, arm64-v8a) or a Unity Android build. The headset supplies the pose through the
game engine; nothing tracks over the network.

CI cross-builds both ABIs in its own `android` job, and both bindings carry the `arm64-v8a`
library: the Unity package at `Runtime/Plugins/Android/arm64-v8a/libbw_audio.so`, the Godot addon
at `addons/bw_audio/bin/` beside its own `arm64` extension. See build.md, "Android".

Phonon is in the Android build too. CI builds the Steam Audio core statically for both ABIs and
links it in, so a headset gets the real HRTF decode, automatic occlusion, pathing and the
reflection bed rather than the fallback pan. The DSP is scalar C with no SSE intrinsics, so ARM
needs no port beyond the shim. Phonon links statically, so it lands inside `libbw_audio.so` and
neither package gains a file. It costs size: the stripped arm64 library is about 7 MB instead of
0.4 MB.

### Wasm Audio Worklet

The browser, through Emscripten's Wasm Audio Worklets. `src/sink/worklet_sink.c`, and
[web.md](./web.md) is the document that owns the platform.

It is the easiest customer the fixed-quantum adapter will ever have. An AudioWorklet's `process()`
gets 128 frames, always, and 128 divides every sensible block size, so one rendered engine block
feeds exactly `block / 128` callbacks with no partial slot. The output buffer is planar float
already (`AudioSampleFrame.data` is `data[channel * samplesPerChannel + i]`), which is the bus
layout, so the adapter's runs copy straight out with no conversion and no interleave. That is the
same property the JACK sink has.

**Stereo only**, said at the open. A page can build a wide AudioContext and none of the array
profiles has a transport there, so AUTO sends anything wider to the offline sink, the way Android
does.

**Every `webaudio.h` call runs on the browser main thread**, and two facts force it: `new
AudioContext()` needs a `Window`, which a Worker global scope is not, and Emscripten's handle
table (`emAudio` in `libwebaudio.js`) is an ordinary per-thread JavaScript variable, so a handle
minted on one thread means nothing on another. None of it is proxied by Emscripten. The sink
proxies its own setup with `emscripten_proxy_sync` and calls through directly when it is already
there. Nothing in the render path proxies anything.

**`device` is a decimal AudioContext handle**, not a name, the same shape AAudio's decimal Android
device id takes. A page creates its AudioContext inside its own user-gesture handler, registers it
with `emscriptenRegisterAudioObject`, and hands the engine the number. NULL lets the sink create
one. The page keeps the right to resume it, which matters because only a user gesture may.

**A suspended context is a host-paced degradation, not a failure**, and on this platform it is the
normal state: every page starts suspended and a suspended context renders no quanta at all, so
`process()` is never called and the engine's dsp clock, playheads and fades would simply stop. Rule
4 already has the answer for a device that went away, and this sink gives the same one: a
host-paced thread paces silence so the clocks keep advancing, and `device_lost` says so.

The handoff between that thread and the worklet is the one piece with no precedent in the other
backends. Both pull the same `SinkQuant`, which is single-threaded by contract, so they share a
**wait-free try-lock**: whoever finds it taken bails at once rather than waiting. The worklet
writes one quantum of silence; the host thread skips one period. It is not a mutex and the audio
thread never blocks, which is what keeps invariant 1. It is contended only in the single quantum
where a context resumes or suspends, so bailing is the right answer rather than a cost. The host
thread decides the worklet is live from a heartbeat the `process()` callback bumps, and stands
down after a quiet window.

**The host thread's quiet window is `max(8 blocks, 3 x baseLatency)`, read at open.** A deep
output buffer makes the browser render its quanta in bursts as long as that buffer (50 ms of quanta
back to back, then nothing, under a 0.05 hint), and eight blocks is only 43 ms at 256 frames. A
shorter window lets the heartbeat go quiet inside every burst gap: the host thread takes over,
`device_lost` flaps, and the two pullers alternate on one adapter. The `run-xr` check catches it.

**Render ahead was tried and rejected.** It renders in a Worker thread, and a Worker has no
priority: the AudioWorklet thread is the one thread a page owns that the OS treats as audio, so
render ahead lost to rendering in `process()` at every load measured. The slack a slow render
needs comes from the BROWSER'S output buffer, which the priority thread already owns, and that is
what `BWA_SINK_FLAG_DEEP_BUFFER` asks for. docs/web.md has the latencyHint measurement. Do not
rebuild render ahead without new evidence; NOTES.md has the measurements.

**Nothing on the audio path waits on the main thread.** Every `emscripten_proxy_sync` in the sink is on the control
thread (open, start, close, the `health()` playbackStats read, and a context-state read nobody
calls), and in the one topology this sink opens in the control thread IS the main thread, so none
of them proxies. `process()` and the host-paced thread call nothing proxied.

**Teardown has no thread to join.** Returning `false` from `process()` is the only "this processor
is done" the API offers. `close()` sets a flag, destroys the node on the main thread (which stops
the browser scheduling it), and then waits for the heartbeat to go quiet before it frees anything
the callback touches. The wait is bounded, because a suspended context never calls `process()` at
all and would otherwise hang the close forever.

## CMake

| option               | default                         | effect                                                          |
|----------------------|---------------------------------|-----------------------------------------------------------------|
| `BWA_WITH_WASAPI`    | ON on `WIN32`                   | compiles `wasapi_sink.cpp`, links `ole32 avrt`, defines `BWA_HAVE_WASAPI` |
| `BWA_WITH_COREAUDIO` | planned (phase 4); the option does not exist yet | will compile `coreaudio_sink.c`, link the frameworks, and define `BWA_HAVE_COREAUDIO`, ON on `APPLE` |
| `BWA_WITH_JACK`      | ON on Linux when `pkg-config jack` succeeds | compiles `jack_sink.c`, takes its INCLUDE DIRS only (the sink dlopens `libjack.so.0`), defines `BWA_HAVE_JACK` |
| `BWA_WITH_ALSA`      | ON on Linux when `find_package(ALSA)` succeeds | compiles `alsa_sink.c`, takes its INCLUDE DIRS only (the sink dlopens `libasound.so.2`), defines `BWA_HAVE_ALSA` |
| `BWA_WITH_AAUDIO`    | ON on `ANDROID`                 | compiles `aaudio_sink.c`, links `aaudio`, defines `BWA_HAVE_AAUDIO` |
| `BWA_WITH_WORKLET`   | **OFF**, Emscripten only        | compiles `worklet_sink.c`, adds `-sAUDIO_WORKLET=1 -sWASM_WORKERS=1` to the link, defines `BWA_HAVE_WORKLET`. Off by default even on Emscripten, because those flags change `emscripten_get_now` into a per-scope runtime probe for every target that links the library and the CI leg must not pay for that. `tools/wasm/build-web.sh` turns it on; `tools/wasm/build-wasm.sh` does not |

Every one of them is off on the other platforms and cannot be forced on.

Off Windows, configure prints a status line naming the platform and what it can open. gcc and
clang get `-Wall -Wextra` per target (not globally, so the fetched third-party sources keep their
own levels) and the library gets `-fvisibility=hidden`. `libbw_audio.so` and `.dylib` fall out of
CMake's defaults, and the bindings load `bw_audio` either way. `ws2_32` and `winmm` sit behind one
`bwa_link_os` helper, which also supplies pthreads and `libm` off Windows; the dll needs its own
call, because it takes bwa_core's OBJECTS and so does not inherit its link interface.

Two auto-detects carry a platform guard, both for the same reason: a dev checkout has the SDKs
staged, so without the guard a Linux configure of that same tree "finds" a Windows artifact and
fails at compile or link. `BWA_WITH_ASIO` requires `WIN32` as well as the SDK, and Steam Audio
requires the archive set under the platform's own `lib/<platform>/` and not just `phonon.h`. The
calibration viewer and the ASIO capture tools are WIN32-only targets, skipped with a status message
elsewhere. The other two GUI tools are not: `bwa_layout_tool` and `bwa_playground` are raylib plus
Dear ImGui, so `-DBWA_BUILD_PLAYGROUND=ON` builds them on Linux too. The `/experimental:c11atomics`
per-file flags are under `if(MSVC)` (see build.md).

Steam Audio builds in CI for every platform, each job staging its own `lib/<platform>/` under its
own cache key (a cache is scoped to the repository, not to the runner OS, so one shared key would
serve the wrong archives). Three rules keep the static link correct off Windows:

- **`-t` is a Windows-only argument** in both pinned phonon build scripts. The composite action
  passes it on the Windows branch only.
- **`enable_language(CXX)` sits in the phonon block.** `project()` declares `LANGUAGES C`, and a
  static phonon makes the link C++. Without CXX enabled, CMake emits an EMPTY link rule rather than
  an error: the build reports "Linking CXX shared library libbw_audio.so", exits 0, and writes no
  library, and every test still passes because the tests link `bwa_core`'s objects.
- **`-Wl,--exclude-libs,ALL`** on the library. phonon's `IPLAPI` expands to
  `__attribute__((visibility("default")))` in its own objects, so without the flag a static link
  re-exports every `ipl*` entry point in spite of `-fvisibility=hidden`. A host that also loads a
  real `libphonon.so` (a Unity project with Valve's own plugin, say) would then have two
  definitions to choose between.

A no-SDK build stays fully viable (build.md, "Building without Steam Audio"), so none of this is
required. What phonon buys is the real HRTF decode, automatic occlusion, pathing and the
reflection bed.

## Tests

The rule from CLAUDE.md applies with force here, because CI has no audio hardware on any
runner: a test that cannot fail is the default outcome. Split what can be tested purely from
what needs a device, and make the device-dependent part **skip visibly** rather than pass.

- **`test_sink_convert`** and **`test_sink_quant`**: pure, always run, described above.
  `test_sink_quant`'s design is the part worth copying: the render writes a
  value derived from its own TIMESTAMP and the device side checks each frame against the value its
  own stream index implies, so one comparison pins the ordered concatenation, the no-duplication
  rule, and the `sample_pos` rule at once, and a wrong stamp corrupts audio rather than merely
  failing a stamp assertion. Per the CLAUDE.md trap, break the pop arithmetic on purpose and confirm
  the test goes red before you trust it green.
- **`test_os`**: thread create and join, `os_sleep_ms` bounds, the monotonic
  clock advancing at the right rate over 100 ms against the C clock, `os_sleep_until_ns` lateness
  percentiles, mutex exclusion under two threads, and the pose seqlock under a writer
  thread and a reader thread with torn-read detection (the writer stamps one generation number
  into all seven payload fields, so a straddled read reads back a mix).

  What that seqlock section pins, honestly: the ALGORITHM, not the memory ordering. Dropping the
  writer's release store to relaxed and deleting the reader's acquire fence leaves it GREEN on
  x86, because the hardware does not reorder those. Removing the reader's validating reload turns
  it red at once (122,837 torn reads in a 400 ms run). The ordering is pinned by review and by the
  fences being present, and the test says so rather than implying more.
- **`test_audio_sink`** grows one section per compiled backend. Open the default device.
  Either the open returns NULL with a non-empty message, or the sink starts, delivers at least
  ten blocks in 200 ms with a constant `nframes` equal to `block_size()`, monotonic position
  and time, `health.measured` consistent with the rule 4 table, then stops and closes without
  hanging. When the open fails for "no device" (the runner case), the test exits with the
  ctest skip code and the target carries `SKIP_RETURN_CODE 77`, so the dashboard says
  "skipped" and not "passed". The null and manual sections keep running everywhere.

  Only the *specific* "there is no device" message skips ("no active render
  endpoint", "no jack server", "no PCM named", "no usable AAudio output device"); any other open
  failure is a failure, because a machine WITH audio that cannot open it is exactly what the
  section exists to catch. The JACK section additionally skips on two
  CONFIGURATIONS it cannot run against, a server at another rate and a server with fewer playback
  ports than the run asked for, and prints the reason either way.

  The two Linux backends add one more skip phrase, "could not be loaded", because they load their
  libraries at run time: a box with no libjack has no JACK device, which is a skip and not a
  failure.

  The results AGGREGATE across the sections: a failure anywhere fails, a section that actually ran
  makes it a pass, and only "every device section had no device" reports the ctest skip. Returning
  the first skip would hide a backend that ran beside one that could not.

  **The unavailable-library section** runs before all of them and needs no device, because it is
  the path a developer box cannot otherwise reach: the box has the libraries. `bwa_sink_dl_override`
  (an internal test hook in `sink.h`, not public ABI) re-aims both run-time loaders at a soname
  that cannot exist, which is the same failure to a sink as a missing library. Per backend it
  demands a device count of 0, an explicit open that fails NAMING the library, and a next open that
  finds the real library again once the hook is cleared. Then it demands that AUTO with neither
  library loadable lands on the offline sink and carries the first backend's reason out on the
  degradation channel, because a silent offline sink reads exactly like working hardware that
  happens to be muted.

  One hook serves both backends on purpose, which is why the AUTO check lands on the offline sink
  rather than on the other Linux backend: with neither library loadable there is no next backend to
  fall through to. The cost of that fall-through was measured at about 3 microseconds per absent
  library, against the 3.7 milliseconds an explicit JACK open costs when libjack IS present and has
  to find out there is no server.

  Each section takes its device string from the environment (`BWA_TEST_ALSA_DEVICE`,
  `BWA_TEST_AAUDIO_DEVICE`, `BWA_TEST_JACK_PORTS`, plus `BWA_TEST_JACK_CHANNELS` for the width), so
  one binary reaches an array-width rig, a dummy server, a PulseAudio route, or a headset's Bluetooth
  output without editing code. Do not point the ALSA
  one at the hardware-free `null` PCM: it accepts writes as fast as they arrive and never paces, so
  the device cannot run dry and the underrun assertion fails by construction. It rendered 257,811
  blocks in the 470 ms a paced device spends on 76. Use it for the open sequence and the format
  walk, nothing else.

  Under ThreadSanitizer on Linux the suite reports ten races, and none is in a backend. Five are the
  plain health counters in `sink_quant.c`, which `sink_quant.h` declares as a deliberate tradeoff
  (monotonic counts nobody synchronizes on, kept a plain type so the file stays off MSVC's
  `/experimental:c11atomics` list); a Linux backend is simply the first thing to exercise them under
  the tool. Three are the sink test's own probe, read from the control thread while a sink runs, and
  one is the `bwa_null_sink_skip_blocks` volatile int that `sink.h` documents for the same reason.
  The last is inside a third-party `pthread_mutex_lock`. The JACK sink's xrun path reports nothing,
  which is what the notification-thread parking above is for.

  **The sink_flags subsections.** Three more sections cover `bwa_desc.sink_flags`, and all of them
  open through `bwa_sink_open` rather than through a backend's own open, because that is the path a
  caller's flags actually take and it is where `EXACT_RATE` is folded into `exact_rate`.

  The WASAPI EXCLUSIVE subsection opens the default endpoint at 256, 128 and 64 frames, skipping
  and printing any size the device refuses. It asserts lower latency than a shared open at the same
  block, `measured` true, and the injected stall producing exactly ONE dropout with a nonzero frame
  count. That last one is the queued-depth rule, which no other test exercises. It also prints which format the walk chose and whether the alignment retry fired, so a
  reader knows which device-dependent paths the run actually went down. The stall is 150 ms,
  not "two buffers plus slack": an exclusive buffer is one device period, three milliseconds on an
  onboard codec, and `IAudioClock` advances by only about half the wall time a starved stream
  spends starved, since it counts frames read FROM OUR BUFFER. A shorter stall trips the rule at
  some block sizes and not others.

  The WASAPI EXACT_RATE subsection has an ORDER that matters. It asks the DEFAULT open first and
  takes rule 6's degradation as ground truth about what the OS is resampling, then demands that the
  same open with the flag fails naming both rates. The other order cannot fail: with the fold
  removed it comes out SKIP rather than red, because a strict open that succeeds cannot
  distinguish "the endpoint takes both rates" from "the flag went nowhere".

  ALSA checks TIGHT_BUFFER twice: reported on the section's own PCM, demanded on the hardware-free
  `null` PCM. The split exists because the ALSA PulseAudio plugin returns three periods whether or
  not two are asked for, so demanding two on whatever PCM the box offers would fail a correct sink,
  while `null` bounds nothing and every install has one. Its EXACT_RATE check asserts that the flag
  and the `exact_rate` argument AGREE at an absurd rate, which is exactly what breaks when the fold
  goes. AAudio demands that the tight open come back shallower than the default one wherever the
  default actually got its two bursts.

  What the stall assertions can and cannot demand differs by backend, and the sections say which.
  ALSA can demand both halves: `-EPIPE` is the device's own report, the stall outlasts the buffer by
  a factor of two, and the frame estimate is a measurement. JACK can demand only the late block,
  which the adapter times itself; whether the SERVER calls a late client an xrun is the server's
  judgment, and a dummy or timer-driven backend need not. When no xrun arrives the section prints
  that the xrun path went unexercised rather than passing quietly. AAudio demands the late block
  and the xrun COUNT rising, because `getXRunCount` was confirmed to move on an emulator, but never
  `dropped_frames`: AAudio says how many times the stream ran dry and never for how long, so that
  field is asserted to stay 0 rather than filled with a guess. What the AAudio section does NOT
  demand is a clean healthy run, because nothing there says whether one was on offer - the callback
  thread belongs to AAudio, and a virtualized HAL or a busy headset can run dry through no fault of
  the sink. It prints the count instead.

  The AAudio section runs through `tools/android/run-tests.ps1`, not through ctest: the test
  binaries are ELF executables for the device and the host has nothing to run them with. The script
  reads the test list out of the build directory's `CTestTestfile.cmake` (so it and ctest cannot
  disagree about what the suite is), pushes `libbw_audio.so` and the executables to
  `/data/local/tmp/bwa`, runs each under `adb` with `LD_LIBRARY_PATH` set, and maps the skip code 77
  through. The whole suite runs there. `test_os` is flaky on an emulator, on its two tightest
  timing bounds alone. See the
  [verify list](./backend-development.md#verify-before-relying-on-it).
- **The whole suite under `BWA_SINK_NULL` on `ubuntu-latest` and `macos-latest`.** This is the
  port's regression gate, and none of it touches a device. The two jobs sit in `ci.yml` beside the
  Windows one and catch a Win32 call sneaking back in. Off Windows the suite is smaller:
  `bwa_calib_view` and the ASIO capture tools are skipped targets, so the viewer's suite and the
  `validate_*` and capture-tool calibrate runs are absent, and the two raylib tools sit behind an
  option the jobs leave off. Both jobs build phonon, so the SDK-gated tests run. `ctest -N` in a
  build tree gives the count for that configuration. Configure a Linux tree with
  `-DBWA_BUILD_PLAYGROUND=ON` and the `playground` and `layout_tool` suites come back too. They need
  a display, so run ctest inside an X or Wayland session, or under `xvfb-run` (software GL
  passes). A cold run pays for the phonon build, a warm one restores it from the cache. The macOS
  job is unverified locally, both its shim paths and its phonon build.
- **On hardware**, a "desk day" section for hardware-validation.md, one pass per backend on a
  real machine: the laterality tone left and right (never DC, per the trap); a ten-minute soak
  with a busy scene at a 256 block and zero `xruns` with `measured` true; and the reported
  output latency against a loopback measurement, within one block. The `cave_both` monitor on
  a Windows machine with the Digiface is the one check that closes the defect in
  [Why](./backend-development.md#why).
