# Device backends

Status: **phases 0, 1, 2 and 5 are implemented**; phases 3 and 4 are still specification. The engine
has six sinks today: ASIO (`src/asio_sink.cpp`), WASAPI (`src/wasapi_sink.cpp`), JACK
(`src/jack_sink.c`), ALSA (`src/alsa_sink.c`), null (`src/null_sink.c`), and manual
(`src/manual_sink.c`), over the two shared pieces (`src/sink_convert.h`, `src/sink_quant.c`) that
phase 0 built. The ABI those needed is in place: the appended `bwa_sink_type` values,
`bwa_desc.device` and `sink_flags`, the backend-agnostic device query, and `bwa_health.device_lost`,
all in the 0.14 minor bump. Phase 2 put the OS shim in (`src/os.h`, `src/os_win.c`,
`src/os_posix.c`), so the library, the tests and the console examples build and pass with gcc and
clang on Linux and macOS. What remains is the AAudio and CoreAudio backends.

Read [architecture.md](./architecture.md) for the bus seam first and
[concurrency.md](./concurrency.md) for the audio-thread rules every backend inherits.

## Why

The goals, per platform, as stated for this spec:

| platform | goal                                                                                          | backend                            |
|----------|-----------------------------------------------------------------------------------------------|------------------------------------|
| Windows  | ease of development at the desk, and reach for head-mounted VR games: a PC headset's audio is a WASAPI endpoint, not an ASIO driver | WASAPI, beside ASIO for the array |
| Android  | the same reach on standalone headsets (Meta Quest, Pico), which run Android                   | AAudio                             |
| macOS    | ease of development at the desk                                                               | CoreAudio                          |
| Linux    | a Windows alternative for the rig in future, and seated or VR headphone experiments now, driven from Psychtoolbox or PsychoPy | JACK, ALSA            |

Nothing in the backend layer is VR-specific. A headset's pose reaches the engine through the
existing listener API (`bwa_set_listener_pose` each frame, with the prediction lead for
motion-to-sound latency); the OptiTrack path stays CAVE-only. What the goals change is the
order in [Phases](#phases) and one default: shared-mode device access everywhere, because a
VR runtime and an experiment framework both keep their own audio open beside the engine.

Three concrete problems the backends fix, in order of pain:

- **Headphones on Windows need an ASIO driver.** The `binaural` and `cave_sim` profiles open a
  2-channel ASIO device, so a desk machine without a pro interface runs ASIO4ALL or FlexASIO.
  Both are wrappers over the OS audio stack. A native WASAPI sink removes that dependency and
  opens the device the user's headphones are actually on.
- **`cave_both` cannot open its second device.** The ASIO SDK holds one current driver per
  process, and the sink refuses a second open (`asio_sink.cpp`, the `g_sink` check at the top of
  `bwa_asio_sink_open`). `bwa_start` opens the monitor through the same path with the same driver
  name (`engine.c`, the `cave_both` branch of `bwa_start`), so under `BWA_SINK_AUTO` the monitor
  falls to the silent null sink, and under `BWA_SINK_ASIO` the start fails. The profile has never
  had a live monitor on hardware. The fix is a second backend for the second device: ASIO for the
  array, WASAPI for the headphones.
- **Unity and Godot projects run on macOS, Linux, and Android.** The engine is Windows-only, so a
  collaborator on a Mac cannot run the headphone profiles at all, and a standalone headset
  cannot run the engine. CoreAudio, JACK, ALSA, and AAudio cover those hosts. Two of them can
  also drive the array, a bonus rather than the goal: CoreAudio, because the RME Digiface
  Dante has a macOS driver; Linux, because a JACK or ALSA client can drive a multichannel card
  directly, or reach the existing Dante network as an AES67 sender. See
  [Linux and the array](#linux-and-the-array). Both are unverified; see the checklist at the
  end.

What does not change: ASIO stays the Windows array transport (a locked decision in
architecture.md), the bus seam stays, and the render contract in `src/sink.h` stays. A backend is
one more consumer of the bus. It must not touch the core.

## Scope

| backend   | platform             | API                                   | serves                                                     | phase |
|-----------|----------------------|---------------------------------------|------------------------------------------------------------|-------|
| WASAPI    | Windows 10 and later | MMDevice + `IAudioClient3` (COM)      | headphone profiles, the `cave_both` monitor, any 2-ch use. Multichannel through exclusive mode is possible but not the production path | 1 |
| CoreAudio | macOS 11 and later   | the HAL (`AudioDevice*`, not AUHAL)   | headphones at the desk; the array too if the Digiface's macOS driver exposes 26 outputs | 4 |
| JACK      | Linux: a JACK server, or PipeWire through pipewire-jack | libjack (`jack_client_open`, the process callback) | the Linux production path: headphones on a PipeWire desktop, the array through a multichannel card or the AES67 virtual card, per-speaker port routing in the host's patchbay | 5 |
| ALSA      | Linux                | alsa-lib (`snd_pcm_*`)                | the no-server path: headphones on a bare box (an experiment wanting the raw device clock), or the array on a `hw:` card (MADI, ADAT, or the AES67 daemon's card) opened directly | 5 |
| AAudio    | Android 8.0 (API 26) and later | `AAudio*`                   | headphones on standalone VR headsets                       | 3 |

Phase 2, between WASAPI and the rest, is the OS shim that lets the engine build off Windows at
all. It is done, and so is phase 5: see [Phases](#phases).

### Linux and the array

Audinate ships no Dante host driver for Linux. Dante Virtual Soundcard and the Dante PCIe cards
are Windows and macOS only, so the Digiface plays no part in a Linux rig. Two routes reach 26
speakers anyway:

- **A multichannel card wired to the amps.** A MADI or ADAT card with an in-kernel ALSA driver
  (RME's HDSPe family, `snd-hdspm`), or a class-compliant USB interface. The Dante network is
  not involved. This is different hardware from the CAVE's, so it is a second install, not a
  drop-in for the rig.
- **AES67 into the existing Dante network.** Dante devices with AES67 mode enabled receive
  AES67 multicast flows from any sender. On Linux the sender is `aes67-linux-daemon` on the
  RAVENNA ALSA kernel module (both GPL), which presents a virtual ALSA card; the engine opens
  it through the JACK or ALSA sink like any other card. The config changes are on the Dante
  side, in Dante Controller: AES67 mode per receiving device, multicast flows of at most 8
  channels each (26 channels is 4 flows), 48 kHz only, and PTPv2 clocking with the Dante
  leader as grandmaster. The PTP part is a gain, not a cost: it puts the audio clock on the
  network clock that api.md's clock-model section wants the display machines on too.

Either way the sink sees an ordinary ALSA card or a set of JACK ports. Nothing in a backend
knows about AES67. The verify list at the end carries the hardware questions.

### Experiments from Psychtoolbox or PsychoPy

Two integration shapes, and the second needs no Linux backend at all:

- **Live.** The engine owns the device; the experiment drives sources through a binding and
  schedules onsets with `bwa_source_play_at` against `bwa_get_clock` and
  `bwa_get_output_latency_frames` (api.md, "Land a sound on a visual event"). When onset
  precision is the measurement, open the card directly through ALSA: `hw:`, the raw device
  clock, the engine as the card's only client. When the framework keeps its own audio open
  beside the engine, JACK on PipeWire shares the card.
- **Offline.** The manual sink renders stimuli deterministically (`bwa_render_block`,
  bit-identical run to run) to files, and PsychPortAudio plays them with its own
  sample-accurate scheduler. This works on Windows today and on Linux after phase 2. It is the
  shape to start with for any experiment that can pre-render.

The bindings are the open item, not the backends. PsychoPy is Python: a nanobind extension
over the C ABI, one compiled module per platform, with `bwa_render_block`'s engine-owned block
exposed as a zero-copy array view for the offline shape. Psychtoolbox is MATLAB or Octave: a
MEX on the classic C MEX API, which Octave implements, and not the MATLAB-only C++ Data API.
Neither exists yet. One rule for both: never install the audio-thread capture tap from an
interpreter, even through a wrapper that takes the GIL; the manual sink is the offline path.

Out of scope, on purpose:

- **Capture.** The calibration and validation tools run their own full-duplex ASIO host
  (`examples/asio_session.cpp`). This spec covers output sinks only. Capture on other platforms
  is a later spec.
- **A native PipeWire client (`pw_stream`).** pipewire-jack runs the JACK backend unchanged,
  with the same fixed quantum and planar float ports. Write a `pw_stream` backend only if
  pipewire-jack's quantum handling proves inadequate on the rig. PulseAudio natively: a
  desktop-only path, reachable through its ALSA plugin, and PipeWire has replaced it.
- **PortAudio, miniaudio, RtAudio, Oboe.** A third-party abstraction would replace the seam this
  repo already owns and hide the timing hooks that justify self-hosting. Oboe wraps AAudio for
  the same callback the engine already handles.
- **Automatic device recovery.** A lost device is reported, not reopened. The ASIO reset request
  has the same gap today (`asio_sink.cpp`, `kAsioResetRequest`). One later design covers both.
- **Resampling inside the engine.** The array never resamples. A headphone stream may let the
  OS resample, with a warning. See [Sample rate](#6-sample-rate).

## What stays fixed

`src/sink.h` is the contract. A backend is one file that fills the vtable:

```c
typedef struct {
    bwa_sink_type type;                                              /* which backend this is */
    int          (*start)(bwa_sink*);
    void         (*stop)(bwa_sink*);
    void         (*close)(bwa_sink*);
    const char*  (*backend)(bwa_sink*);
    uint32_t     (*block_size)(bwa_sink*);
    uint32_t     (*output_latency)(bwa_sink*);
    const float* (*render_block)(bwa_sink*, uint32_t*, uint32_t*);   /* manual only */
    void         (*health)(bwa_sink*, bwa_sink_health*);
} bwa_sink_vtbl;
```

The engine hands every sink one `bwa_render_fn` and a planar float bus. The sink calls it once
per block, on the audio thread, with a `bwa_timestamp`. Three rules from the existing sinks
carry over unchanged:

1. One backend, one file, one `BWA_HAVE_*` define, one CMake block. No backend type leaves
   its file. The CLAUDE.md trap "do not bake ASIO assumptions outside `asio_sink.cpp`" becomes
   "outside the `*_sink` files".
2. Adding a backend does not change `rt.c`, `engine.c`'s render callbacks, or the profiles. The
   only engine change is the selection logic in `sink.c` and the ABI surface below.
3. The audio-thread invariants apply to the backend's own code, not only to the render it
   calls. The convert and interleave loop, the timestamp read, and the health update all run
   on the audio thread. Preallocate at open. No locks, no syscalls that can block, no logging.

Two things the vtable gains, both internal:

- `bwa_sink_type type` on the vtable, so `bwa_get_sink_type` stops sniffing the backend string
  (`engine.c`, `bwa_get_sink_type` matches `"asio"` by prefix today).
- `bwa_sink_open` takes `(sink_type, device, flags, exact_rate)` instead of `(sink_type,
  asio_driver)`. `exact_rate` is set by the engine for the array sink and clear for a
  headphone sink. See rule 6.

## The contract a backend must meet

Numbered so a code review can cite them.

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

So every backend except ALSA goes through the **fixed-quantum adapter** (`sink_quant`, below).
It renders whole engine blocks into a small FIFO and hands the device any count it asks for.
When the device size equals the block size it is a straight pass-through with no copy and no
added latency, so a backend that manages to pin the device size pays nothing. A rig box pins
its JACK or PipeWire period to the engine block size and gets that pass-through; a desktop
whose quantum wanders pays one FIFO.

### 2. Format

The bus is planar float, channel-major (`sink.h`). The backend converts to the device's format
and layout. The ASIO sink's `convert_out` and `to_i32` move to a shared header
(`sink_convert.h`) and gain interleaved variants, because every non-ASIO API wants interleaved
frames. NaN converts to 0, never to an out-of-range integer, for the reason noted in
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
| WASAPI    | `IAudioClock::GetPosition(&pos, &qpc)`, normalized by `GetFrequency()` to frames. Do not assume the units: `frames = pos * rate / freq`. `qpc` is in 100 ns units |
| CoreAudio | the IOProc's `inOutputTime`: `mSampleTime` and `mHostTime` when both valid flags are set; `mach_timebase_info` converts host ticks to ns |
| JACK      | `jack_get_cycle_times` at the top of the process callback: `current_frames` and `current_usecs`, the server's DLL-filtered pair in microseconds on `CLOCK_MONOTONIC`. Filtered, so the drift fit reads the DLL rather than the raw device, the way a QPC-synthesized ASIO stamp does |
| ALSA      | `snd_pcm_status` after the write: `snd_pcm_status_get_htstamp` and `_get_delay`, with the timestamp type set to monotonic in the sw_params |
| AAudio    | `AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &pos, &ns)`. Fails until the stream has run a little; keep the previous pair and set `measured` only after the first success |

### 4. Health

`bwa_sink_health` keeps its fields. The rule for `measured` is unchanged: true only when the
backend can observe the device's own position, so a zero dropout count means "none happened"
and not "could not know". The gap arithmetic stays in `sink_position_gap`, and the adapter
measures `late_blocks` and `render_ns_peak` for every backend it serves, the way the null sink
does today.

| field            | WASAPI                                                   | CoreAudio                                  | JACK                                        | ALSA                                              | AAudio                              |
|------------------|----------------------------------------------------------|--------------------------------------------|---------------------------------------------|---------------------------------------------------|-------------------------------------|
| `dropouts`       | **exclusive:** the queued depth `written - device_pos` went negative, the device consumed frames never written. **shared:** the interval between two `ReleaseBuffer` calls exceeded the buffer depth. Every event refills the buffer to full, so the device is holding `bufferFrameCount` after each release and runs dry that many frames later; a later release proves it had nothing in between. The depth rule cannot serve shared mode: the request size is exactly what the engine consumed since the last callback, so `written` telescopes to follow the device position and the depth never goes negative | `sink_position_gap` on `mSampleTime`, as ASIO | the xrun callback (`jack_set_xrun_callback`) | `-EPIPE` from the write (an underrun)          | `AAudioStream_getXRunCount` delta   |
| `dropped_frames` | **exclusive:** the size of that deficit. **shared:** the release interval minus the buffer depth, at the nominal rate. Not an estimate: it is the measured excess | the gap                                    | `jack_get_xrun_delayed_usecs` at the nominal rate | the elapsed monotonic time since the last good write minus the buffer depth, at the nominal rate | unknown: 0                          |
| `driver_resyncs` | 0                                                        | the `kAudioDeviceProcessorOverload` listener | 0                                         | `-ESTRPIPE` (a suspend), after recovery         | 0                                   |
| `measured`       | **exclusive:** once `GetPosition` has succeeded. **shared:** `bufferFrameCount > period`, and nothing to do with `IAudioClock`. The release-interval rule needs headroom between a normal cycle and the dry deadline; with a one-period buffer a normal cycle sits on it and ordinary jitter would read as a fault, so the honest answer is that the configuration cannot judge | once a timestamp with both flags arrived   | always (the server reports xruns directly)  | always (the write reports underruns directly)     | once `getTimestamp` has succeeded   |

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

The adapter adds the frames it holds in its FIFO. The value stays constant for the life of the
sink, as the header promises.

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
converts from the ANSI codepage). The engine's `backend_buf` grows to hold a long WASAPI
endpoint name plus the decode suffix. Fixed strings stay ASCII per the repo rule.

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

Build these before the first backend. They are pure, so they get the hardware-free tests the
backends cannot.

### `src/sink_convert.h`

Planar float in, one of {float32, int32, int24 packed, int16} out, planar or interleaved.
Moved out of `asio_sink.cpp` with its NaN and clamp rules intact; the ASIO sink keeps calling
the planar variants. Around 120 lines. Test: every format times both layouts, the clamps, NaN
to 0, and a 26-channel interleave round trip.

### `src/sink_quant.h`, `src/sink_quant.c`

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

### `src/os.h`, `src/os_win.c`, `src/os_posix.c`

**Implemented (phase 2).** The portability shim. Not needed for WASAPI. Required before CoreAudio,
ALSA, or AAudio, because those backends imply building the whole library off Windows, and the
library calls Win32 outside the sinks. The inventory, from a grep of `src/`:

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

`test/` and `examples/` carry their own `Sleep`, `GetTickCount64`, and `<windows.h>` (13 test
files, 14 examples). They move onto the shim in the same phase. Around 300 lines of shim plus a
mechanical pass over about 40 files. Tests may include `src/os.h`, because they compile the core
in; examples are client code of the public ABI, so they get their own two-function
`examples/portable.h` instead of reaching into `src/`.

Six things the inventory above did not predict. Four were found while implementing phase 2, and
two more came out of the macOS CI runner after it:

- **`SRWLOCK` is not `CRITICAL_SECTION`.** `steam_scene.c` guards the committed `IPLScene` with a
  reader/writer lock: several sim threads borrow the scene for concurrent ray traces while the
  owner takes it exclusively around `iplSceneCommit`. Collapsing that to a plain mutex would
  serialize traces that have no reason to wait on each other, so the shim gains `os_rwlock`
  (SRWLOCK on Windows, `pthread_rwlock_t` elsewhere) beside `os_mutex`.
- **`pose.h` cannot be a C++-visible header any more.** Its payload fields are `_Atomic` now (see
  below), and `_Atomic` is not C++. `examples/validate.cpp` reached the seqlock through
  `natnet.h`, so `natnet.h` gained `natnet_read_pose` and the tool calls that; `pose.h` gives C++
  the type as opaque. `rt.h` and `natnet.h` forward-declare `PoseSlot` rather than including
  `pose.h`, which keeps `<stdatomic.h>` (and MSVC's `/experimental:c11atomics`) off the twenty-odd
  translation units that only pass the slot around by pointer.
- **`bwa_null_sink_skip_blocks` stays a plain `volatile int`.** It is declared in `sink.h`, which
  the two C++ sinks include, so it cannot be an `_Atomic`. One test-thread writer and one
  sink-thread read-and-clear on an aligned `int`: the ordering nobody depends on is the only thing
  the change gives up.
- **Path normalization was a Windows fact.** `assets.c` folded case and backslashes into the cache
  key. On a POSIX filesystem `A.wav` and `a.wav` are different files and a backslash is an ordinary
  character in a name, so folding either there would hand one cache entry to two files. Both folds
  are now `_WIN32`-only, and the test section that pins them skips off Windows.
- **Priority is not one call with three spellings.** `os_thread_set_realtime(period_ns)` is what
  every SELF-PACED render thread now calls at its top, and the three platforms mean three different
  things by it. Windows joins the "Pro Audio" MMCSS task, which the WASAPI sink used to ask for
  inline. Linux asks for `SCHED_FIFO`, the one that can be REFUSED, which is why
  `os_thread_realtime_available()` exists beside it: the ALSA sink has to report that degradation
  through the open's `err` channel, and by the time its render thread exists that channel is gone.
  Apple sets `THREAD_TIME_CONSTRAINT_POLICY`, and there it is not about priority at all: Darwin
  COALESCES timers for threads without one, so a correct `mach_wait_until` lands milliseconds late
  on an ordinary thread. The macOS CI runner measured p50 2.749 ms and p99 9.759 ms against a 2 ms
  deadline, and `os_sleep_ms(50)` taking 100.7 ms, with the timebase conversions verified correct
  in both directions. `period_ns` exists on the call for that platform alone.

  The Windows effect is larger than it sounds, and it was measured rather than assumed: on a desk
  machine pinned at 100 percent CPU by unrelated work, the same 200-wake measurement gave p50
  12 to 18 ms on an ordinary thread and p50 0.407 ms with p99 0.811 ms on the MMCSS thread.
- **The null sink stamped its own epoch, and a zero stamp reads as no stamp.** It used
  `os_monotonic_ns() - base`, with `base` captured a few instructions earlier on the same thread.
  Every other backend stamps the platform clock (`sink_quant_now_ns`, or QPC in the ASIO sink), so
  this was the one backend with a private epoch. Worse, on a clock whose tick is coarse enough
  (Apple Silicon's `mach_absolute_time` is 41.67 ns) the FIRST block's stamp came out exactly 0,
  which `rt.c`'s publish gate reads as "no stamp": `bwa_get_clock` then had nothing until block 1,
  and a late wake pushed that past the 30 ms window `test_smoke` allows. Windows and Linux avoided
  it by clock resolution, not by design. The sink stamps the absolute monotonic clock now, and
  `base` survives for the deadline arithmetic only.

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
effect, so the old null-sink loop held the whole machine at a 1 ms timer tick for as long as a
visual-only tool had the offline sink open. The sink no longer raises it at all.

Only SELF-PACED paths use this: the null sink's block loop and the WASAPI sink's `host_paced_block`
(what runs after the device is lost). Device-paced paths keep their own wait, because the device is
the clock there: the WASAPI event wait and the ASIO callback are untouched.

`test_os` measures it: 200 wakes 2 ms apart, reporting median and p99 lateness. A desk Windows
machine gave 0.43 ms and 0.74 ms; WSL2 gave 0.13 ms and 0.33 ms. Forcing the pre-1803 fallback with
the resolution left alone gave 5.6 ms and 13.0 ms, which is what the p50 assertion is there to
catch.

It measures ON A THREAD THAT ASKED FOR REAL-TIME, because that is how the sinks use the primitive,
and on Darwin measuring anywhere else measures timer coalescing instead of the sleep. The Apple
bound is deliberately gross, p50 under 20 ms and p99 under 50 ms: the tight bound exists to catch
Windows losing its high-resolution timer path, Darwin has no counterpart to lose, and the only
Apple data point anyone has is a virtualized CI runner. Tightening it wants a physical Mac, which
is on the verify list.

## ABI changes

All in one minor bump, `BWA_VERSION_MINOR` 13 to 14. Enum values are appended, the desc keeps
its layout, and both bindings mirror the changes (`bindings/unity/Runtime/Bwa.cs`,
`bindings/godot/src/bwa_engine_node.h`).

### `bwa_sink_type`

```c
typedef enum { BWA_SINK_AUTO = 0, BWA_SINK_ASIO = 1, BWA_SINK_NULL = 2, BWA_SINK_MANUAL = 3,
               BWA_SINK_WASAPI = 4, BWA_SINK_COREAUDIO = 5, BWA_SINK_ALSA = 6, BWA_SINK_AAUDIO = 7,
               BWA_SINK_JACK = 8,
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
the backend's default device (ASIO: the first driver with enough outputs, as today). The
anonymous union keeps `asio_driver` compiling and keeps the C# and GDExtension field offsets.

`sink_flags` has one bit for now: `BWA_SINK_FLAG_EXCLUSIVE`. WASAPI opens in exclusive mode
and AAudio requests exclusive sharing. Off by default, because exclusive mode takes the
device from every other application on a desk machine, and shared mode with an
`IAudioClient3` period is low-latency enough for a monitor.

`bwa_sink_health` (internal, `src/sink.h`) gains `device_lost` in the same change, because the
flag has to reach `engine.c` from the sink somehow. It is not ABI.

### AUTO order

Per platform, and on Windows per requested channel count:

| platform | 2 channels                     | more than 2 channels        |
|----------|--------------------------------|-----------------------------|
| Windows  | WASAPI, ASIO, null             | ASIO, null (unchanged)      |
| macOS    | CoreAudio, null                | CoreAudio, null             |
| Linux    | JACK, ALSA, null               | JACK, ALSA, null            |
| Android  | AAudio, null                   | null                        |

This is the one behavior change: a headphone profile on Windows under AUTO opens the
Windows default output instead of the first registered ASIO driver. That is the device the
headphones are on, which the ASIO auto-pick could not know (it may have picked the Digiface and
played the monitor into Dante channels 1 and 2). `sink = BWA_SINK_ASIO` or a `device` naming an
ASIO driver keeps the old path, by rule 10. Multichannel WASAPI is never chosen by AUTO: an
unintended exclusive grab of a 26-channel WDM device would surprise, and the locked decision
says the array goes over ASIO.

On Linux, `jack_client_open` gets `JackNoStartServer`, so a box with neither a JACK server nor
PipeWire falls through to ALSA at once instead of spawning a `jackd`.

`cave_both` on Windows then resolves on its own: the array request takes ASIO, the monitor
request takes WASAPI, and the adapter makes the monitor's block size equal the array's, so the
"same buffer size" check in `bwa_start` passes. `bwa_start` opens the monitor for the **array's
resolved block**, not `cfg.block_size`, which is the part that makes that true when the ASIO
driver picks a size of its own.

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
picker offers the card rather than 26 ports. AAudio has no
C enumeration: it reports one device, "default"; `device` also accepts a decimal Android
device id obtained from Java's `AudioManager`, passed to `AAudioStreamBuilder_setDeviceId`.

### `bwa_health`

`uint32_t device_lost` appended (rule 4). The struct has no reserved fields, so this is the
layout change the minor bump covers.

## Per-backend notes

The parts of each API that are not obvious, and the decisions taken.

### WASAPI

Files: `src/wasapi_sink.cpp` (COM is nicer from C++, and the DLL already compiles C++20 for
the ASIO sink). Link `ole32`, `avrt`. Define `BWA_HAVE_WASAPI`. Windows 10 1607 or later for
`IAudioClient3`; fall back to `IAudioClient` on older builds.

Open sequence, shared mode (the default):

1. `IMMDeviceEnumerator` to the endpoint (`GetDefaultAudioEndpoint(eRender, eConsole)` or the
   selected one), `Activate(IAudioClient3)`.
2. Format: `WAVEFORMATEXTENSIBLE`, float32, the engine rate, the requested channel count.
   `IsFormatSupported(SHARED)` with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` in the flags lets a
   mismatched engine rate through with a warning (rule 6). Shared mode is stereo or the
   endpoint's own channel layout only; a 26-channel request in shared mode fails at open with
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
stream event already signalled, so the catch-up wake returns at once and reports the frames just
written rather than an empty buffer. Measured on a live endpoint: a render stalled past two full
buffers produced zero such wakes, and the release-interval rule caught it on the first cycle.

`AUDCLNT_E_DEVICE_INVALIDATED` from any of these sets `device_lost`. An `IMMNotificationClient`
is not required for that; it only adds "the default device changed", which the sink ignores
(rule: no automatic recovery).

CI: GitHub's Windows runners have no audio endpoint, so `GetDefaultAudioEndpoint` fails with
`E_NOTFOUND`. The sink must fail cleanly there, and the sink test skips (see
[Tests](#tests)).

### CoreAudio

Files: `src/coreaudio_sink.c`. The HAL is a C API; no Objective-C. Link the `CoreAudio` and
`CoreFoundation` frameworks. Define `BWA_HAVE_COREAUDIO`.

The HAL, not AUHAL, because the array needs the device's raw channel count and no converter
in the path. Open sequence:

1. Resolve the device: `kAudioHardwarePropertyDefaultOutputDevice`, or match the UID or name.
2. Read `kAudioDevicePropertyStreamConfiguration` for the output scope: an `AudioBufferList`
   with one buffer per **stream**, each with its own channel count. A device can present 26
   outputs as one 26-channel stream or as several streams; the sink maps bus channels across
   streams in order. Read each stream's `kAudioStreamPropertyVirtualFormat`; it is float32,
   interleaved within the stream, on every device the HAL virtualizes. Reject anything else.
3. Set `kAudioDevicePropertyNominalSampleRate` to the engine rate. The property set is
   asynchronous: listen for the change or poll with a bounded wait, then read it back. A
   device that stays at another rate fails the open (rule 6, and the headphone case does not
   relax this one: the HAL has no per-client resampler without AUHAL).
4. Set `kAudioDevicePropertyBufferFrameSize` to the block size, clamped to
   `kAudioDevicePropertyBufferFrameSizeRange`. Read it back; the adapter handles a mismatch.
5. `AudioDeviceCreateIOProcID`, then `AudioDeviceStart` on `start()`.
6. Add property listeners for `kAudioDeviceProcessorOverload` (a `driver_resyncs` tick) and
   `kAudioDevicePropertyDeviceIsAlive` (`device_lost`).

The IOProc gets `inOutputTime` with `mSampleTime` and `mHostTime`. Both feed rule 3; the
host time converts through `mach_timebase_info`. The IOProc's `outOutputData` is the ABL
from step 2, already sized; the sink writes every buffer or the HAL plays stale data.

The goal on a Mac is the desk, and the HAL design permits the array as a bonus. If that is
ever wanted, verify first that the RME macOS driver presents the Digiface Dante's outputs as
one device, and that the count at 48 kHz is 26 or more.

### JACK

Files: `src/jack_sink.c`. `pkg-config jack` in CMake, link `jack`. The same binary runs on
JACK2 (`libjack`, LGPL-2.1) and on PipeWire (pipewire-jack's drop-in `libjack`, MIT); which one
answers is decided at run time by what the box has installed. Define `BWA_HAVE_JACK`.

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
`cave_both` be one 28-port client on Linux later; the two-sink design stands for now.

Four things the implementation settled that the sequence above left open:

- **Step 4 runs at `open()`, not at `start()`.** "The server offers 2 playback ports and this sink
  has 26" is exactly the message a caller has to see, and `bwa_start` prints one fixed string for a
  sink's `start()` failure while it surfaces an open failure verbatim. So the client activates and
  connects during the open, and `start()` only lifts a gate that the process callback reads. Before
  the gate opens, the callback writes silence into the ports, which is also what a patchbay wants to
  see while the engine finishes starting.
- **The xrun callback runs on a notification thread, not on the process thread.** It may not touch
  the adapter's plain counters, so it parks the event and the frame estimate in two atomics and the
  next process callback folds them in through `sink_quant_note_dropout`. Every count stays in one
  place, and the whole thing stays race-free under ThreadSanitizer.
- **The adapter never sees a device position.** JACK hands out exactly `nframes` every cycle, so
  `written` telescopes to follow the server's frame counter and the queued-depth rule can never
  fire. On a real xrun the server's counter jumps and that rule would report the same fault the
  xrun callback already reported, so `quant_pull` is called with `device_pos_valid` clear and
  `measured` is set from the xrun route instead.
- **The buffer-size callback resizes nothing.** The adapter takes the count per pull, and its FIFO
  is allocated for 8192 frames at open, so the callback only records the new size. A cycle larger
  than that ceiling is silenced past it and booked as a dropout.

### ALSA

Files: `src/alsa_sink.c`. `find_package(ALSA)` in CMake, link `ALSA::ALSA`. alsa-lib is
LGPL-2.1, dynamically linked, so the license inventory in build.md gains one line and nothing
else changes. Define `BWA_HAVE_ALSA`.

Open sequence, all through `snd_pcm_hw_params_*`:

1. `snd_pcm_open(name, SND_PCM_STREAM_PLAYBACK, 0)`. `default` when `device` is NULL.
2. Access `SND_PCM_ACCESS_RW_INTERLEAVED`. Format in the rule 2 order, through
   `snd_pcm_hw_params_test_format`.
3. Rate: `snd_pcm_hw_params_set_rate_resample(0)` for a `hw:` device and for any device under
   `exact_rate`, then `set_rate` exact. A `plug` or `default` PCM under a headphone open may
   resample; report it (rule 6).
4. Channels exact. Period size near the block size; buffer size near three periods. Read both
   back. The write loop writes whole engine blocks, so the adapter is not needed: the period
   is a device-side number and the sink's `block_size()` stays the engine's.
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

Five things the implementation settled that the sequence above left open:

- **Rule 3's pair is built from the PREVIOUS block's status.** `snd_pcm_status` can only be read
  after a write, so the correspondence it gives, "the frame at `written - delay` was playing at
  `htstamp`", stamps the NEXT block: that block starts `delay` frames later on the same stream, so
  its host time is `htstamp + delay / rate`. The offset between a `sample_pos` and its host time is
  then constant, which is all the drift fit needs. Before the first status, and after a restart, the
  host clock stands in, and every stamp is floored at the previous one plus a nominal block so none
  can step backward.
- **`S24_LE` needs a conversion `sink_convert.h` does not carry.** The shared header's `int24` is
  three packed bytes, which is `S24_3LE`. `S24_LE` is 24 valid bits inside a 32-bit container, so
  the sink writes that container itself, from `sink_to_i32(v) >> 8`. The clamp and NaN rules stay in
  the shared header, where the float becomes an integer; only the container is ALSA's business.
- **The `SCHED_FIFO` question is asked at open, and answered on the thread.** The degradation has to
  reach the caller through the open's `err` channel, and by the time the render thread exists that
  channel is gone. So the open asks `os_thread_realtime_available()`, which reads `RLIMIT_RTPRIO`,
  the same limit the kernel checks, and the render thread makes the actual
  `os_thread_set_realtime()` attempt.
- **`output_latency()` reports the buffer size until the first block.** The header promises a
  constant, and `snd_pcm_delay` can only be read after a write, so the open's buffer-size figure
  stands until the first successful write latches the measured delay in its place.
- **`stop()` joins before it touches the PCM.** An `snd_pcm_t` is not thread-safe, so calling
  `snd_pcm_drop` to cut short an in-flight `snd_pcm_writei` from the control thread is a race. The
  wait costs one blocking write, which is one period.
- **The device query SYNTHESIZES `default` when the hints do not carry it.**
  `snd_device_name_hint` lists only PCMs that declare a hint block, and a `pcm.!default` in an
  asoundrc declares none: that is exactly the PulseAudio and PipeWire plugin route, and every
  user-defined default. So the one name `snd_pcm_open` always understands was missing from the
  list. Index 0 is `default` whether or not a hint mentioned it.

### AAudio

Files: `src/aaudio_sink.c`. Link `aaudio`. `ANDROID_PLATFORM` 26 or later in the NDK toolchain.
Define `BWA_HAVE_AAUDIO`.

Builder: output direction, `AAUDIO_SHARING_MODE_SHARED` (exclusive under the flag; the OS
grants it rarely), `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`, `AAUDIO_FORMAT_PCM_FLOAT`, 2 channels,
the engine rate (shared mode converts when it must: rule 6), `setFramesPerDataCallback(block
size)` as the hint it is, a data callback and an error callback. After open, set the buffer
size to two bursts (`AAudioStream_setBufferSizeInFrames(2 * getFramesPerBurst())`) and read
the actual value back for rule 5.

The data callback calls `quant_pull` with the frame count it was given and returns
`AAUDIO_CALLBACK_RESULT_CONTINUE`. Position and time come from `AAudioStream_getTimestamp`,
read once per callback and tolerated when it fails early. Dropouts are the OS's own count,
`AAudioStream_getXRunCount`, read as a delta per callback. The error callback receives
`AAUDIO_ERROR_DISCONNECTED` on a route change; AAudio forbids reopening from that thread, so
the sink only sets `device_lost` and the callback thread ends. The sink then starts a host-paced
thread (the null-sink loop) so the engine keeps running, per rule 4.

Consumers: standalone VR headsets (Meta Quest, Pico) through a Godot Android export
(GDExtension, arm64-v8a) or a Unity Android build. The headset supplies the pose through the
game engine; nothing tracks over the network. Two things ride along with the sink and are
bigger than it. The NDK cross-build of the whole library: phase 2's shim covers the code, the
toolchain wiring is this phase. And phonon for Android: the no-SDK fallback is a lateral pan
that build.md calls useless for timbre, and a VR game is the case where timbre is the point.
Steam Audio ships Android arm64 builds. The DSP is scalar C with no SSE intrinsics (a grep of
`src/` finds none), so ARM needs no porting beyond the shim.

## CMake

| option               | default                         | effect                                                          |
|----------------------|---------------------------------|-----------------------------------------------------------------|
| `BWA_WITH_WASAPI`    | ON on `WIN32`                   | compiles `wasapi_sink.cpp`, links `ole32 avrt`, defines `BWA_HAVE_WASAPI` |
| `BWA_WITH_COREAUDIO` | ON on `APPLE`                   | compiles `coreaudio_sink.c`, links the frameworks, defines `BWA_HAVE_COREAUDIO` |
| `BWA_WITH_JACK`      | ON on Linux when `pkg-config jack` succeeds | compiles `jack_sink.c`, links `jack`, defines `BWA_HAVE_JACK` |
| `BWA_WITH_ALSA`      | ON on Linux when `find_package(ALSA)` succeeds | compiles `alsa_sink.c`, links `ALSA::ALSA`, defines `BWA_HAVE_ALSA` |
| `BWA_WITH_AAUDIO`    | ON on `ANDROID`                 | compiles `aaudio_sink.c`, links `aaudio`, defines `BWA_HAVE_AAUDIO` |

Every one of them is off on the other platforms and cannot be forced on.

Phase 2 did the rest of that list. The `if(NOT WIN32) message(WARNING ...)` is gone, replaced by
a status line naming the platform and what it can open. gcc and clang get `-Wall -Wextra` per
target (not globally, so the fetched third-party sources keep their own levels) and the library
gets `-fvisibility=hidden`. `libbw_audio.so` and `.dylib` fall out of CMake's defaults, and the
bindings load `bw_audio` either way. `ws2_32` and `winmm` moved behind one `bwa_link_os` helper,
which also supplies pthreads and `libm` off Windows; the dll needs its own call, because it takes
bwa_core's OBJECTS and so does not inherit its link interface.

Two auto-detects gained a platform guard, both for the same reason: a dev checkout has the SDKs
staged, so without the guard a Linux configure of that same tree "finds" a Windows artifact and
fails at compile or link. `BWA_WITH_ASIO` now requires `WIN32` as well as the SDK, and Steam Audio
requires the platform's import library (`lib/windows-x64/phonon.lib`) and not just `phonon.h`. The
three GUI tools and the ASIO capture tools are WIN32-only targets, skipped with a status message
elsewhere. The `/experimental:c11atomics` per-file flags are still under `if(MSVC)`; the list grew
with the files that became atomic (see build.md).

Steam Audio builds for macOS, Linux, and Android, but the recipe in `third_party/README.md` is
the Windows one. A no-SDK build is fully viable (build.md, "Building without Steam Audio"), so
the port does not wait on phonon. Phonon on the other platforms is its own follow-up.

## Tests

The rule from CLAUDE.md applies with force here, because CI has no audio hardware on any
runner: a test that cannot fail is the default outcome. Split what can be tested purely from
what needs a device, and make the device-dependent part **skip visibly** rather than pass.

- **`test_sink_convert`** and **`test_sink_quant`**: pure, always run, described above. Both
  shipped with phase 0. `test_sink_quant`'s design is the part worth copying: the render writes a
  value derived from its own TIMESTAMP and the device side checks each frame against the value its
  own stream index implies, so one comparison pins the ordered concatenation, the no-duplication
  rule, and the `sample_pos` rule at once, and a wrong stamp corrupts audio rather than merely
  failing a stamp assertion. Per the CLAUDE.md trap the pop arithmetic was broken on purpose and
  the test confirmed red before being trusted green.
- **`test_os`** (phase 2, shipped): thread create and join, `os_sleep_ms` bounds, the monotonic
  clock advancing at the right rate over 100 ms against the C clock, `os_sleep_until_ns` lateness
  percentiles, mutex exclusion under two threads, and the rewritten pose seqlock under a writer
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

  The WASAPI section shipped with phase 1 and the JACK and ALSA sections with phase 5. Only the
  *specific* "there is no device" message skips ("no active render endpoint", "no jack server",
  "no PCM named"); any other open failure is a failure, because a machine WITH audio that cannot
  open it is exactly what the section exists to catch. The JACK section additionally skips on two
  CONFIGURATIONS it cannot run against, a server at another rate and a server with fewer playback
  ports than the run asked for, and prints the reason either way.

  The results AGGREGATE across the sections: a failure anywhere fails, a section that actually ran
  makes it a pass, and only "every device section had no device" reports the ctest skip. Returning
  the first skip would hide a backend that ran beside one that could not.

  Each section takes its device string from the environment (`BWA_TEST_ALSA_DEVICE`,
  `BWA_TEST_JACK_PORTS`, plus `BWA_TEST_JACK_CHANNELS` for the width), so one binary reaches a
  26-port rig, a dummy server, or a PulseAudio route without editing code. Do not point the ALSA
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

  What the stall assertions can and cannot demand differs by backend, and the sections say which.
  ALSA can demand both halves: `-EPIPE` is the device's own report, the stall outlasts the buffer by
  a factor of two, and the frame estimate is a measurement. JACK can demand only the late block,
  which the adapter times itself; whether the SERVER calls a late client an xrun is the server's
  judgment, and a dummy or timer-driven backend need not. When no xrun arrives the section prints
  that the xrun path went unexercised rather than passing quietly.
- **The whole suite under `BWA_SINK_NULL` on `ubuntu-latest` and `macos-latest`** (phase 2,
  shipped). This is the port's regression gate: 30 tests at the default options, none of which
  touch a device. The two jobs sit in `ci.yml` beside the Windows one. They are cheap (no phonon
  build) and they catch a Win32 call sneaking back in. Note what the count means: off Windows the
  GUI tools and the ASIO capture tools are skipped targets, so their suites and the four
  `validate_*` runs are absent, on top of the five SDK-gated tests. The macOS job is unverified
  locally.
- **On hardware**, a "desk day" section for hardware-validation.md, one pass per backend on a
  real machine: the laterality tone left and right (never DC, per the trap); a ten-minute soak
  with a busy scene at a 256 block and zero `xruns` with `measured` true; and the reported
  output latency against a loopback measurement, within one block. The `cave_both` monitor on
  a Windows machine with the Digiface is the one check that closes the defect in
  [Why](#why).

## Phases

| phase | deliverable                                                            | needs                                   | hardware                    | size                         |
|-------|------------------------------------------------------------------------|-----------------------------------------|-----------------------------|------------------------------|
| 0 **(done)** | `sink_convert.h`, `sink_quant`, their tests; the ASIO sink onto `sink_convert` | nothing                        | none                        | ~400 lines                   |
| 1 **(done)** | WASAPI sink; the ABI bump (enum, desc union and flags, device query, `device_lost`); `sink.c` AUTO order; both bindings mirror; docs | phase 0 | a Windows desk machine; the rig for the `cave_both` check | ~600 lines of sink, ~200 of ABI and bindings |
| 2 **(done)** | `os.h` shim; the library, tests, and examples build with clang and gcc; Linux and macOS CI jobs on the null sink | nothing (independent of phase 1) | none | ~300 lines of shim, a pass over ~40 files |
| 3     | AAudio sink; NDK toolchain wiring; phonon for Android; the bindings' Android packaging | phases 0 and 2         | a standalone headset        | ~300 lines plus the toolchain and packaging |
| 4     | CoreAudio sink                                                         | phases 0 and 2                          | a Mac; the Digiface only for the optional 26-channel check | ~450 lines |
| 5 **(done)** | JACK sink, then ALSA sink                                        | phase 2                                 | a Linux box with a card, and one with PipeWire; the AES67 daemon and an AES67-mode Dante receiver for the array check | ~250 lines JACK, ~400 lines ALSA |

Phase 1 goes first because it fixes a live defect, removes a dependency for every desk user,
and is the PC VR path. Phase 2 is independent of it and can run in parallel; it is mechanical
and touches many files, so it wants its own review. Phases 3 to 5 depend only on phase 2 and
not on each other, so their order was a scheduling choice. Phase 5 went first of the three,
because a Linux box with both a JACK server and an ALSA card was the hardware at hand. CoreAudio
is the cheapest of the two that remain and can slot in whenever a Mac is available.

## What to update when implementing

- `include/bw_audio.h`: the enum, the desc union and `sink_flags`, the device query, `bwa_health`,
  the `BWA_VERSION_MINOR` bump, and the `bwa_desc.block_size` comment ("the ASIO buffer-size
  hint" becomes "the device period hint; the render quantum is exact regardless").
- `docs/api.md`: the desc table, the sink policy paragraph, "ASIO device query" becomes "Device
  query", the profile table's "any 2-ch ASIO device" wording, the health section.
- `docs/build.md`: the Platform section, the options table, libjack and alsa-lib in the
  dependency table, the CI section, and a Linux notes section: `RLIMIT_RTPRIO` for ALSA, the
  PipeWire rate and quantum config, the AES67 daemon and the Dante-side AES67 settings.
- `docs/architecture.md`: the bus consumers list and the "Transport: ASIO" locked decision,
  which becomes "ASIO for the array on Windows; any system backend for a stereo monitor".
- `CLAUDE.md`: the repo layout entries, and the ASIO trap generalized to the `*_sink` files.
- `docs/integration.md`: the bindings' enum values and the `device` property.
- `docs/hardware-validation.md`: the desk-day section.
- `bindings/unity/Runtime/Bwa.cs`, `Engine.cs`, `Editor/EngineEditor.cs` (the `"asio:"` prefix
  check that decides the inspector's warning); `bindings/godot/src/bwa_engine_node.{h,cpp}`
  (the `Sink` enum, `asio_driver` becomes `device` with the old setter kept, the no-ASIO
  warning that assumes `cave` needs ASIO).
- `third_party/README.md`: the sink selection paragraph.
- `examples/` (deferred until the backends settle): `playground.cpp` decides "silent" by an
  `asio` prefix check on the backend string, so a WASAPI open shows as NO SOUND; use
  `bwa_get_sink_type`. Its picker and `--list-drivers` enumerate ASIO only; list both backends
  through `bwa_get_device_count` and set `cfg.device`. Same prefix check in `layout_tool.cpp`;
  stale `--driver` and ASIO4ALL wording in `minimal.c`. The capture tools stay ASIO-only.
- `NOTES.md`: the `engine.c` comment that says "WASAPI is future" and its NOTES twin.

## Deferred follow-ups

Known, deliberately not done yet. Each names its trigger.

- **UTF-8 file paths on Windows.** The ABI speaks UTF-8, but every file open (`fopen` in
  layout.c, hpeq.c, calib.c, zylia.c, and the dr_libs `*_init_file` openers in sound.c and
  stream.c) hands the bytes to the C runtime, which reads them as the ANSI codepage. A
  non-ASCII path fails to open on Windows today. Fix: an `os_fopen` in the shim that converts
  to wide and calls `_wfopen`, and the `_w` variants of the dr_libs openers. About 30 lines.
  Independent of the port; do it whenever a path with an accent bites.
- **Blocking waits for the polling threads.** The asset loader (2 ms), the stream thread
  (3 ms), and the three Steam sim threads all sleep-poll. Idle cost is invisible on a desk and
  is battery on a headset, so the trigger is phase 3. Options: one event primitive in the shim,
  or c89thread (condition variables, semaphores, events; public domain or MIT-0 like dr_libs),
  or a core-only SDL3 behind `os.h`, which also brings rtkit realtime priority on Linux for
  phase 5 and a precise sleep the shim now has on its own. Decide at phase 3 or 5, whichever
  comes first. SDL_net is not a candidate for NatNet: it has no multicast join.
- **The examples' backend awareness**, listed under
  [What to update when implementing](#what-to-update-when-implementing).

## Verify before relying on it

- [ ] `test_os` on a PHYSICAL Mac, so the Apple deadline bound can be tightened from the gross one
      the coalescing evidence justifies. Everything Apple in the shim is CI-verified only: nobody
      here has a Mac, and the `THREAD_TIME_CONSTRAINT_POLICY` call was written from the macOS CI
      log plus the documentation.
- [ ] The RME Digiface Dante's macOS driver: one CoreAudio device, 26 or more outputs at 48 kHz.
- [ ] Whether the Digiface is USB Audio Class compliant, which decides if ALSA's `snd-usb-audio`
      sees it at all. RME's product page is the source; do not assume. (Even then it is a
      Dante *transmitter* only through RME's driver; on Linux the AES67 route below is the way
      into the Dante network.)
- [ ] The rig's Dante receivers, the amp-side endpoints, offer AES67 mode, and Dante Controller
      can route four 8-channel AES67 flows onto their 26 channels at 48 kHz.
- [ ] `aes67-linux-daemon` and the RAVENNA ALSA kernel module build against the rig box's
      kernel, and their PTPv2 slave locks to the Dante leader in AES67 mode.
- [ ] pipewire-jack on the box's PipeWire version honors a pinned quantum and reports port
      latency; otherwise run JACK2's `jackd` on the card directly. Everything verified so far ran
      against JACK2's own `jackd` under WSL; pipewire-jack has never answered this sink.
- [ ] The JACK sink against a REAL driver rather than `jackd -d dummy`. The dummy driver paces from
      a timer, so what it cannot exercise is the xrun path (it never called the client late), the
      port latency of a real card, and the buffer-size callback.
- [x] The JACK shutdown path, by killing the server under a running engine. Verified against a
      dummy `jackd` under WSL: `device_lost` set, the dsp clock kept advancing at real time on the
      host-paced thread (23,808 frames in 500 ms against a nominal 24,000), and `bwa_stop` and
      `bwa_destroy` both returned. What is still open is the same check on ALSA, where losing the
      device means a write returning something other than `-EPIPE` or `-ESTRPIPE`, which needs real
      hardware to unplug.
- [ ] The ALSA sink against a REAL card. It has run against a `null` PCM (the API flow only, since
      that PCM does not pace) and against the alsa-plugins PulseAudio route into WSLg, which is what
      timed the blocks and produced the underruns. A `hw:` card is untested, and with it the whole
      format walk past float32: `S32_LE`, `S24_3LE`, the `S24_LE` container, and `S16_LE` have
      never converted a sample on a device that asked for them.
- [ ] The ALSA sink at 26 channels. Every run so far was stereo, because no wide card was at hand.
- [ ] `SCHED_FIFO` actually granted. Under WSL the `RLIMIT_RTPRIO` probe answers no and the sink
      reports the degradation, which is the path that got exercised; the granted path has not run.
- [ ] `-ESTRPIPE` recovery. A suspend needs a real power event to produce, so the resume loop and
      its `driver_resyncs` tick are written and compiled but unexercised.
- [ ] The minimum `IAudioClient3` shared period on the desk machine's driver. Onboard codecs
      often offer nothing under 10 ms; a 10 ms period is 480 frames at 48 kHz, so the monitor's
      added latency is one engine block plus that.
- [ ] Standalone headsets: the AAudio native rate and burst size on the Quest and Pico
      generations in play (48 kHz and a 256-frame burst are typical; the block size should be
      a multiple of the burst), and that the Steam Audio Android build runs the mode-2
      per-voice HRTF fleet within the headset's CPU budget.
- [ ] Which binding the experiments need first: the nanobind module for PsychoPy, or the MEX
      for Psychtoolbox and Octave. Neither is in this spec.
- [ ] The ASIO reset-request plumbing, when it is written, uses the `device_lost` design above
      rather than a second mechanism.
- [ ] `cave_both` on the rig: ASIO for the array and WASAPI for the monitor, at once, with the
      monitor's block matching the array's resolved one. Phase 1 makes it resolve; only the rig
      can confirm it. This is the check that closes the defect in [Why](#why).
- [ ] The reported WASAPI output latency against a loopback measurement. The sink reports
      `GetStreamLatency` plus one period (shared) or the buffer (exclusive), plus the one block
      the adapter can hold. On a desk machine that came to 736 frames at a 256 block; nothing has
      measured whether it is right.
- [ ] Exclusive mode on a real interface. The path is written and compiled but has only ever run
      in shared mode, so `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` and the format walk past float32 are
      both unexercised.
- [ ] The `device_lost` degradation, by unplugging a USB endpoint mid-run: the flag must set, the
      engine must keep advancing its clocks, and `bwa_stop` then `bwa_start` must recover.
- [ ] The EXCLUSIVE-mode depth rule against a deliberately starved stream. Shared mode is settled
      (the release-interval rule in rule 4, pinned by `test_audio_sink`: a render stalled past two
      full buffers on a 1056-frame buffer with a 480-frame period counted exactly one dropout of
      about 2860 frames, and a healthy run counted none). Exclusive mode keeps the depth rule and
      has never been run at all, so it is unverified along with the rest of that path.
