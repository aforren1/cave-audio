# Backend development

This is the explanation and contributor guide for the device backends: why they exist, what is in
scope, what stays fixed, the design decisions behind each sink, the phase plan, what to touch when
you add a backend, and what still needs hardware to verify. The contract and the per-backend
reference are in [backends.md](./backends.md).

Read [architecture.md](./architecture.md) for the bus seam first and
[concurrency.md](./concurrency.md) for the audio-thread rules every backend inherits.

## Why

The goals, per platform:

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

Three concrete problems the backends fix:

- **Headphones on Windows need no ASIO driver.** The `binaural` and `cave_sim` profiles open a
  WASAPI endpoint, the device the headphones are actually on. Without it a desk machine with no
  pro interface would need ASIO4ALL or FlexASIO, which are wrappers over the OS audio stack.
- **`cave_both` needs two devices at once.** The ASIO SDK holds one current driver per process,
  and the ASIO sink refuses a second open (`asio_sink.cpp`, the `g_sink` check at the top of
  `bwa_asio_sink_open`). So the monitor takes a second backend: ASIO for the array, WASAPI for
  the headphones. See [AUTO order](./backends.md#auto-order) for how `bwa_start` opens it. Only the rig can
  confirm the pair on hardware (see the verify list).
- **Unity and Godot projects run on macOS, Linux, Android and the browser.** CoreAudio, JACK,
  ALSA, AAudio and the Wasm Audio Worklet cover those hosts. Two of them can
  also drive the array, a bonus rather than the goal: CoreAudio, because the RME Digiface
  Dante has a macOS driver; Linux, because a JACK or ALSA client can drive a multichannel card
  directly, or reach the existing Dante network as an AES67 sender. See
  [Linux and the array](#linux-and-the-array). Both are unverified; see the checklist at the
  end.

What does not change: ASIO stays the Windows array transport (a locked decision in
architecture.md), the bus seam stays, and the render contract in `src/sink/sink.h` stays. A backend is
one more consumer of the bus. It must not touch the core.

## Scope

| backend   | platform             | API                                   | serves                                                     | phase |
|-----------|----------------------|---------------------------------------|------------------------------------------------------------|-------|
| WASAPI    | Windows 10 and later | MMDevice + `IAudioClient3` (COM)      | headphone profiles, the `cave_both` monitor, any 2-ch use. Multichannel through exclusive mode is possible but not the production path | 1 |
| CoreAudio | macOS 11 and later   | the HAL (`AudioDevice*`, not AUHAL)   | headphones at the desk; the array too if the Digiface's macOS driver exposes enough outputs for the layout | 4 |
| JACK      | Linux: a JACK server, or PipeWire through pipewire-jack | libjack, loaded at run time (`jack_client_open`, the process callback) | the Linux production path: headphones on a PipeWire desktop, the array through a multichannel card or the AES67 virtual card, per-speaker port routing in the host's patchbay | 5 |
| ALSA      | Linux                | alsa-lib, loaded at run time (`snd_pcm_*`) | the no-server path: headphones on a bare box (an experiment wanting the raw device clock), or the array on a `hw:` card (MADI, ADAT, or the AES67 daemon's card) opened directly | 5 |
| AAudio    | Android 8.0 (API 26) and later | `AAudio*`                   | headphones on standalone VR headsets                       | 3 |

Phase 2 is the OS shim that lets the engine build off Windows at all. Phase 6 is the Wasm Audio
Worklet. See [Phases](#phases).

### Linux and the array

Audinate ships no Dante host driver for Linux. Dante Virtual Soundcard and the Dante PCIe cards
are Windows and macOS only, so the Digiface plays no part in a Linux rig. Two routes reach the
array (24 speakers, up to 36) anyway:

- **A multichannel card wired to the amps.** A MADI or ADAT card with an in-kernel ALSA driver
  (RME's HDSPe family, `snd-hdspm`), or a class-compliant USB interface. The Dante network is
  not involved. This is different hardware from the CAVE's, so it is a second install, not a
  drop-in for the rig.
- **AES67 into the existing Dante network.** Dante devices with AES67 mode enabled receive
  AES67 multicast flows from any sender. On Linux the sender is `aes67-linux-daemon` on the
  RAVENNA ALSA kernel module (both GPL), which presents a virtual ALSA card; the engine opens
  it through the JACK or ALSA sink like any other card. The config changes are on the Dante
  side, in Dante Controller: AES67 mode per receiving device, multicast flows of at most 8
  channels each (24 channels is 3 flows, 36 is 5), 48 kHz only, and PTPv2 clocking with the Dante
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
  sample-accurate scheduler. This works on Windows and Linux. It is the shape to start with for
  any experiment that can pre-render.

**The Python binding** ([`bindings/python/`](../bindings/python/), and
[integration.md](./integration.md) puts it beside Unity and Godot) is a nanobind extension:
wheels for Python 3.10 and later on all three
desktops, with the engine library inside each one, and with `bwa_render_block`'s engine-owned
block exposed as a read-only numpy view so the offline shape costs no copy. PsychoPy is Python, so
it uses that package directly. `bindings/python/examples/offline_render.py` and `live_onset.py` are
the two shapes above, runnable.

**The MATLAB and Octave binding** ([`bindings/matlab/`](../bindings/matlab/)) is a MEX: one
gateway dispatching on a subcommand string,
the shape PsychPortAudio uses, written against the classic C MEX API, which Octave implements, and
not the MATLAB-only C++ Data API. One source builds a binary for each interpreter. Psychtoolbox is
MATLAB or Octave, so it uses that package, and
`bindings/matlab/examples/offline_render.m` and `live_onset.m` are the two shapes above, runnable.
`AudioTunnel3DDemo_bwa.m` is a third: Psychtoolbox's own `AudioTunnel3DDemo2` idea, re-done against
this engine.

One rule the MEX carries that the Python binding does not have to: PsychPortAudio and the engine
must not open the same device exclusively. Either the engine owns the device, or PsychPortAudio
does and the engine pre-renders through the manual sink, or both run shared and give up the lowest
latency.

One rule holds for both bindings, and both enforce it by refusing to expose the call: never install
the audio-thread capture tap (`bwa_set_output_capture`) from an interpreter, even through a wrapper
that takes the GIL. The manual sink is the offline path. The MEX keeps the subcommand and raises
with the reason, so a reader finds the explanation where they looked for the call.

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
  OS resample, with a warning. See [Sample rate](./backends.md#6-sample-rate).

## What stays fixed

`src/sink/sink.h` is the contract. A backend is one file that fills the vtable:

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
   its file: do not bake a backend's assumptions outside the `*_sink` files.
2. Adding a backend does not change `rt.c`, `engine.c`'s render callbacks, or the profiles. The
   only engine change is the selection logic in `sink.c` and the
   [ABI surface](./backends.md#abi-changes).
3. The audio-thread invariants apply to the backend's own code, not only to the render it
   calls. The convert and interleave loop, the timestamp read, and the health update all run
   on the audio thread. Preallocate at open. No locks, no syscalls that can block, no logging.

Two internal details of the contract:

- `bwa_sink_type type` sits on the vtable, so `bwa_get_sink_type` reads a field instead of
  matching the backend string.
- `bwa_sink_open` takes `(sink_type, device, flags, exact_rate)`. The engine sets `exact_rate`
  for the array sink and clears it for a headphone sink. See
  [rule 6](./backends.md#6-sample-rate).

## Design decisions per backend

The reference in [backends.md](./backends.md#per-backend-notes) says what each sink does. This is
why, for the choices the API left open.

### JACK: what the implementation settled

- **Step 4 runs at `open()`, not at `start()`.** "The server offers 2 playback ports and this sink
  has 24" is exactly the message a caller has to see, and `bwa_start` prints one fixed string for a
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
  xrun callback already reported, so `sink_quant_pull` is called with `device_pos_valid` clear and
  `measured` is set from the xrun route instead.
- **The buffer-size callback resizes nothing.** The adapter takes the count per pull, and its FIFO
  is allocated for 8192 frames at open, so the callback only records the new size. A cycle larger
  than that ceiling is silenced past it and booked as a dropout.

### ALSA: what the implementation settled

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
  constant, and the delay (`snd_pcm_status_get_delay`) can only be read after a write, so the
  open's buffer-size figure stands until the first successful write latches the measured delay in its place.
- **`stop()` joins before it touches the PCM.** An `snd_pcm_t` is not thread-safe, so calling
  `snd_pcm_drop` to cut short an in-flight `snd_pcm_writei` from the control thread is a race. The
  wait costs one blocking write, which is one period.
- **The device query SYNTHESIZES `default` when the hints do not carry it.**
  `snd_device_name_hint` lists only PCMs that declare a hint block, and a `pcm.!default` in an
  asoundrc declares none: that is exactly the PulseAudio and PipeWire plugin route, and every
  user-defined default. So the one name `snd_pcm_open` always understands was missing from the
  list. Index 0 is `default` whether or not a hint mentioned it.

### AAudio: what the implementation settled

- **Stereo only, said at the open.** Android AUTO already sends a wider request straight to the
  offline sink, so the only caller that can reach a multichannel AAudio open is an explicit
  `BWA_SINK_AAUDIO` with an array layout. That gets a message naming the width rather than a
  device error, because Android carries no array transport at all.
- **`setUsage` is never called.** `AAudioStreamBuilder_setUsage` is `__INTRODUCED_IN(28)` and the
  library targets API 26, so an unguarded call is a weak symbol that crashes on an API 26 device.
  The default usage is `AAUDIO_USAGE_MEDIA`, which is right for a game. A headset build that wants
  `AAUDIO_USAGE_GAME` can raise `ANDROID_PLATFORM` and add the call behind a runtime check.
- **`device_pos_valid` is false at every pull.** AAudio reports the fault directly through
  `getXRunCount`, and the adapter's queued-depth rule would count the same starve a second time:
  the callback keeps asking for a burst whether or not the render was late, so `written` lags the
  presented position by exactly the silence the service already counted. Same reasoning as JACK's.
- **The device's own rate is probed before the real open.** A shared-mode stream opened AT the
  engine rate reports the engine rate whether or not the service is resampling underneath, so
  without a probe the sink could not tell a native 48 kHz device from a converted 44.1 kHz one and
  rule 6's degradation channel would have nothing to say. The probe is one stream opened with
  everything unspecified and closed again, on the control thread, which is the only way to ask
  AAudio this from C. It also lets a failed open name both rates.

### AAudio on the emulator

Verified on an Android 34 `google_apis` x86_64 emulator: a 960-frame burst, a 1920-frame buffer,
1400 to 2050 frames of reported latency depending on the run, since it is latched off whichever
callback first gets a timestamp, a constant 256-frame render quantum through the adapter, a
monotonic timestamp pair, `measured` true, and an injected 120 ms stall that
`AAudioStream_getXRunCount` did report as one xrun. Breaking the xrun fold on purpose turned that
last assertion red, per the CLAUDE.md trap, before it was trusted green. `bwa_minimal` under AUTO
opened `aaudio:default` and its clock model measured 48000.00 Hz over a 6 second run, +0.1 ppm,
which is the timestamp pair checked through the whole engine rather than at the sink. The
emulator's audio HAL is virtual, so its block counts wander (54 to 96 across identical runs) and
say nothing; what it exercises is the code path.

### CoreAudio: what the implementation settled

- **The host stamp is `inNow`, not `inOutputTime`.** The plan paired `inOutputTime`'s sample time
  with its host time, and summed the device latency, the safety offset, the stream latency and
  the buffer for rule 5. Measured on a Mac, `inOutputTime->mHostTime` leads the IO cycle by the
  buffer plus the safety offset: 12.00 ms at 256 + 320 frames on an HDMI display, steady to about
  0.01 ms after the first cycle. So
  the two halves of the plan counted those 576 frames twice. The sink stamps `inNow->mHostTime`,
  the cycle's own time, which is also what every other backend's stamp means, and the latency
  keeps the full sum. `bwa_get_output_latency_frames` then stays the render-to-DAC delay its
  header promises. The position still comes from `inOutputTime->mSampleTime`.
- **The latency has no term for the adapter, and it follows the IO cycle.** The other adapter
  backends add a block when the device size differs from the block size. Measured here with the
  device tap, at 256 on 256, 8192 on 4096, 6000 on 4096, 256 on 1024, 1024 on 256 and 8 on 15: a
  block's first frame reached the device 0.3 to 5 frames after the block's own stamp. The
  adapter stamps a block with the callback time plus the frames queued ahead of it, so the
  stamp already holds what the adapter holds. With the term, an 8192 block reported 12696
  frames where 4504 is right. The IO buffer is read from the cycle the IOProc is handed, not
  latched at open, because the buffer size is per device per process: a second open at 1024
  moved a 256 sink from 812 frames to 1580, and a latched figure stayed at 812.
- **A dropout goes through the adapter's queued-depth rule.** The output sample time advances by
  exactly one buffer per cycle, and a cycle the HAL skips after an overload shows as a jump. The
  sink moves the sample time onto the adapter's stream with one anchor and passes it as the
  device position. A sample time that steps backward, or jumps past the sane window, is a new
  timeline, so it re-anchors and reports nothing.
- **The overload listener gets no dispatch queue.** With a queue, the HAL dispatches
  `kAudioDeviceProcessorOverload` synchronously from the IO thread (`AudioHardware.h` says so).
  The device-change handler runs on the sink's queue and calls `AudioDeviceStop`, which waits for
  the IO thread. Sharing the queue would let each wait on the other. The overload block is
  invoked directly and does one atomic add.
- **No listener block holds a pointer to the sink.** `AudioObjectRemovePropertyListenerBlock`
  does not say whether it waits for an invocation already under way. Each block captures a slot
  index into a static table. The device-change handler runs on one serial queue and looks the
  sink up in its slot, and `close()` clears the slot on that queue, so a notification that
  arrives late finds nothing. The overload count lives in the table and is never freed. The
  handler reads the device and does not trust the notification, so a stray call on a slot another
  sink reused does nothing. The table has 8 slots, which is the limit on open sinks per process.
- **A rate change under a running sink is a loss.** Another application, or Audio MIDI Setup,
  can move the device off the engine rate. Nothing in this path resamples, so playing on would be
  a pitched render that reports itself healthy. The sink treats it like a device that died: it
  sets `device_lost`, stops its IOProc, and starts the host-paced thread.
- **`AudioDeviceStop` is treated as synchronous, with one guard.** Across 60 stops, some with
  the IOProc overrunning, no callback was in flight when it returned and none followed. The
  header does not promise that. The IOProc and the host-paced thread raise a counter around
  each pull, and read the `running` and `lost` flags after raising it. `stop()` and the loss
  handler set a flag and then wait for the counter to reach 0. Both sides are sequentially
  consistent, so either the puller sees the flag or the stopper sees the pull. That covers a
  pull in flight. It does not cover a callback that begins after the wait, and nothing can.
- **The width check runs at the engine rate.** An interface can halve its outputs at a higher
  rate, so the stream layout is read again after the rate set, and the map is built from that
  one. A device that is too narrow and already at the engine rate fails before anything
  changes. A device that is too narrow after a rate move gets its old rate back.
- **The IOProc is turned off for the input streams.** By default an IOProc is on every stream
  of a device. On a duplex device that would start the microphone for a sink that never reads
  it. Both devices measured are output only, so this call has never met an input stream.
- **`start()` forgets the anchor.** A restart is a new sample timeline when the device stopped,
  and the old one when another IOProc in the process kept it running. The second case read as
  one dropout the length of the pause. The engine closes a sink on `bwa_stop`, so only a caller
  of the vtable could reach it.
- **The open sets the device's rate and `close()` leaves it.** Every other client of the device
  resamples through its own converter, so the change costs them one glitch. Restoring it would
  cost a second one on every editor play session.
- **No hog mode.** `BWA_SINK_FLAG_EXCLUSIVE` does nothing here. On WASAPI, exclusive mode buys
  latency. On a Mac a shared open already runs the IOProc on the device's own IO cycle, at the
  buffer size the sink set, so hog mode would buy only the exclusion.
- **A stereo open takes the device's preferred stereo pair.**
  `kAudioDevicePropertyPreferredChannelsForStereo` is where Audio MIDI Setup routes stereo on a
  multichannel interface. A wider open maps bus channels across the streams in order.
- **The IOProc zeroes every buffer, then writes through checked indices.** The channel map is
  built at open from the stream configuration. A configuration that changes later (an aggregate
  device edited while the engine runs) then costs channels, not memory safety.

### CoreAudio on a desk Mac

Verified on a Mac mini (Apple silicon, macOS 27.0) against two devices, an HDMI display and the
built-in speakers, both stereo, one stream each, at 48 kHz:

- The IO buffer took the 256-frame block, so the adapter ran in pass-through. Blocks of 8 and
  8192 fall outside the device's 15 to 4096 range and ran bridged.
- The IO buffer size is per process: one process held 1024 frames while a second read the
  device's 512. So the engine's block does not reach other applications, and it does reach
  every other IOProc in its own process.
- Reported latency: 664 frames on the display (88 device + 320 safety + 256 buffer), 812 on the
  built-in speakers (72 + 76 + 408 stream + 256).
- An injected 61 ms stall (2928 frames) produced one dropout of 2936 frames, one late block and
  one overload notification.
- The device tap saw every rendered frame on the right device channel, in order, at each block
  size. See [backends.md: Tests](./backends.md#tests).
- The loss path, both causes. A private aggregate device destroyed under a running sink set
  `device_lost` in 5 to 10 ms, and so did a second open at 44.1 kHz. The host-paced thread
  rendered 56 to 59 blocks in 300 ms against a nominal 56. Twelve races between the destroy and
  `close()`, 0 to 33 ms apart, all closed.
- The map across streams, on a private aggregate of the two devices: 2 streams, 4 channels.
  Widths 1 to 4 each put bus channel c on device channel c, and width 5 was refused.
- `bwa_minimal` under AUTO opened `coreaudio:M27Q` and its clock model measured 48000.27 Hz over
  a 6 second run, +5.6 ppm.
- The Python wheel's 87 tests, the Octave suite, and both `live_onset` examples ran against the
  same build. The Godot extension compiled. No Godot or Unity editor loaded it.
- ThreadSanitizer reported nothing in the sink. Its warnings were the adapter's plain health
  counters, as on Linux. AddressSanitizer reported nothing.

The first start of the built-in speakers once produced one overload and a dropout of about 400
frames in the first 200 ms, and four later runs did not. The count is the HAL's own report, so the section
prints it and does not fail on it.

What that run did not reach is on the [verify list](#verify-before-relying-on-it).

## Phases

| phase | deliverable                                                            | needs                                   | hardware                    | size                         |
|-------|------------------------------------------------------------------------|-----------------------------------------|-----------------------------|------------------------------|
| 0 **(done)** | `sink_convert.h`, `sink_quant`, their tests; the ASIO sink onto `sink_convert` | nothing                        | none                        | ~400 lines                   |
| 1 **(done)** | WASAPI sink; the ABI bump (enum, desc union and flags, device query, `device_lost`); `sink.c` AUTO order; both bindings mirror; docs | phase 0 | a Windows desk machine; the rig for the `cave_both` check | ~600 lines of sink, ~200 of ABI and bindings |
| 2 **(done)** | `os.h` shim; the library, tests, and examples build with clang and gcc; Linux and macOS CI jobs on the null sink | nothing (independent of phase 1) | none | ~300 lines of shim, a pass over ~40 files |
| 3 **(done)** | AAudio sink; NDK toolchain wiring; `tools/android/run-tests.ps1`; the CI cross-build of both ABIs, phonon for both ABIs, and both bindings' Android packaging | phases 0 and 2 | a standalone headset (an emulator covered the code paths) | ~400 lines of sink and test, plus the toolchain and the packaging |
| 4 **(done)** | CoreAudio sink; `sink.c`'s dispatch; the device tap in `test_audio_sink`; the Unity inspector's macOS entry | phases 0 and 2                   | a Mac; the Digiface only for the optional array check | ~650 lines of sink, ~400 of test |
| 5 **(done)** | JACK sink, then ALSA sink                                        | phase 2                                 | a Linux box with a card, and one with PipeWire; the AES67 daemon and an AES67-mode Dante receiver for the array check | ~250 lines JACK, ~400 lines ALSA |
| 6 **(done)** | Wasm Audio Worklet sink; the `BWA_SINK_WORKLET` enum value; `sink.c`'s browser AUTO entry; `bindings/web` and `tools/wasm/build-web.sh` | phase 2, and the wasm spike in [web.md](./web.md) | a browser, and a headless Chromium is enough: `ctest -R web_browser` drives the sink end to end and skips where there is none. Nobody has LISTENED to it yet | ~450 lines of sink, ~900 of binding |

Phases 3 to 6 depend only on phase 2 and not on each other.

## What to update when implementing

No backend is planned after CoreAudio. Adding one touches these places, and nothing in the core:

- `src/sink/<name>_sink.c` (the backend), one `BWA_WITH_*` option and CMake block, and the
  dispatch cases and the AUTO order in `src/sink/sink.c`.
- `include/bw_audio.h`: the enum value, and the per-backend lines under the sink flags.
- `test/audio_sink_test.c`: a section for the backend, with its own "no device" skip phrase.
- `docs/build.md`: the Platform section, the options table, and the license inventory.
- `docs/api.md`, `docs/integration.md` and `docs/hardware-validation.md` (the desk-day pass).
- The bindings: the Unity inspector's per-platform sink list (`EngineEditor.cs`), the Godot
  `sink` property hint, and each binding's README.
- `docs/backends.md`: the status line, the contract tables and a per-backend notes section.
- This document: [Phases](#phases), the design decisions, and the verify list.

## Deferred follow-ups

Known, deliberately not done yet. Each names its trigger.

- **A `monitor_device` field.** `bwa_desc.device` names the primary device, so the `cave_both`
  monitor takes the platform default and cannot be pinned. That is right for the rig, where the
  monitor is whatever headphones the operator has on. It is wrong for a machine with two
  candidate endpoints and a preference. The fix is one more `bwa_desc` field out of the
  reserved tail, with NULL keeping today's behavior. Do it when someone needs the monitor on a
  named endpoint.
- **Blocking waits for the three Steam sim threads.** The occlusion, reflection and pathing
  sims run at a fixed rate (30, 12 and 10 Hz), so they are paced, not polling, and they cost
  what their simulation costs. The asset loader and the stream thread block on `os_event`.
  Revisit the sim threads if a headset's battery says so.

## Verify before relying on it

- [x] `test_os` on a PHYSICAL Mac. A Mac mini (Apple silicon, macOS 27.0) measured the
      deadline sleep at p50 0.004 ms and p99 0.006 to 0.011 ms over three runs, on a thread with
      the `THREAD_TIME_CONSTRAINT_POLICY` the shim sets. The whole suite passes there. The Apple
      bound in the test is still the gross one, because the virtualized CI runner measured p50
      2.7 ms and p99 9.8 ms and has to pass it too.
- [ ] The RME Digiface Dante's macOS driver: one CoreAudio device, enough outputs for the layout (24, up to 36) at 48 kHz.
- [ ] The CoreAudio sink at array width, on a real multichannel interface. The map across
      streams has run only on a 4-channel aggregate of two stereo devices. A preferred stereo
      pair other than channels 1 and 2 has never been met, and neither has a device whose
      channel count changes with its rate, which the Digiface Dante may be. Run `test_audio_sink` with
      `BWA_TEST_COREAUDIO_DEVICE` and `BWA_TEST_COREAUDIO_CHANNELS` on such a device: the device
      tap checks the map.
- [ ] The CoreAudio sink on a device with an input (a USB headset). The IOProc is marked off
      for the input streams so that macOS shows no microphone indicator and asks for no
      permission. No such device has been tried.
- [ ] The adapter's share of the reported latency on WASAPI, JACK, AAudio and the worklet.
      Each adds a block, or what the adapter holds, when the device size differs from the block
      size. On CoreAudio that term measured as an overcount and was removed (see the CoreAudio
      decisions above). The stamp rule is shared, so the same is likely true of the others, but
      each has its own host-time source and nobody has measured them. A device-side tap like
      `sink_coreaudio_tap` is enough: no loopback is needed for this question.
- [ ] The CoreAudio sink BY EAR. The device tap proves the right samples reach the HAL. Nobody
      has listened to it, so the laterality tone in hardware-validation.md is still open.
- [ ] A PHYSICAL CoreAudio device that goes away: unplug a USB interface, or take a Bluetooth
      headset out of range, under a running engine. `device_lost` must set, the dsp clock must
      keep advancing, and `bwa_stop` then `bwa_start` must recover on the new default. The
      `kAudioDevicePropertyDeviceIsAlive` path has run only on an aggregate device the test
      destroyed.
- [ ] The CoreAudio rate set on a device that takes time over it. The two devices measured
      switched between 48 and 44.1 kHz inside the 2 s bound, and nobody timed how long they took.
      A USB interface relocks its clock, so the bound is a guess until one has been timed.
- [ ] The CoreAudio sink on a macOS older than 27, on an aggregate device and on a Bluetooth
      output. Intel Macs are not a target: the x86_64 slice of the universal build compiles and
      nobody plans to run it.
- [ ] `test_audio_sink` on the macOS CI runner. Nothing here knows whether that runner presents
      an output device. With none, the CoreAudio section skips. With a virtual one, the section
      runs against a device nobody has characterized, and a red there is a finding about the
      runner before it is one about the sink.
- [ ] Whether the Digiface is USB Audio Class compliant, which decides if ALSA's `snd-usb-audio`
      sees it at all. RME's product page is the source; do not assume. (Even then it is a
      Dante *transmitter* only through RME's driver; on Linux the AES67 route below is the way
      into the Dante network.)
- [ ] The rig's Dante receivers, the amp-side endpoints, offer AES67 mode, and Dante Controller
      can route 8-channel AES67 flows onto the layout's channels (24, up to 36) at 48 kHz.
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
- [ ] The ALSA sink at array width (24, up to 36). Every run so far was stereo, because no wide card was at hand.
- [ ] `SCHED_FIFO` actually granted. Under WSL the `RLIMIT_RTPRIO` probe answers no and the sink
      reports the degradation, which is the path that got exercised; the granted path has not run.
- [ ] `-ESTRPIPE` recovery. A suspend needs a real power event to produce, so the resume loop and
      its `driver_resyncs` tick are written and compiled but unexercised.
- [x] The minimum `IAudioClient3` shared period on the desk machine's driver. Onboard codecs
      often offer nothing under 10 ms, and this one is exactly that: `test_audio_sink` reports a
      480-frame period against a 1056-frame buffer on a Realtek endpoint at 48 kHz, so the
      shared-mode underrun rule is armed there and the monitor's added latency is one engine block
      plus 10 ms. A codec that answers a shorter period has not been seen yet.
- [x] WASAPI EXCLUSIVE mode on a real endpoint. `test_audio_sink`'s exclusive subsection opens the default endpoint at 256,
      128 and 64 frames and exercises the queued-depth dropout rule, which no other test reached:
      an injected stall produces exactly one dropout with a nonzero frame count at every size.
      Measured latency on a Realtek endpoint at 48 kHz: 736 frames shared, then 544, 416 and 352
      frames exclusive at those three block sizes. Two things it did NOT reach, and both are open
      below. The format walk stopped at the FIRST accepted entry, `int24`, so float32, int32 and
      int16 have still never converted a sample in exclusive mode. And
      `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` never fired: this driver takes its own 144-frame period
      at any request, so the realign-and-retry path is still unexercised.
- [ ] Standalone headsets: the AAudio native rate and burst size on the Quest and Pico
      generations in play (48 kHz and a 256-frame burst are typical; the block size should be
      a multiple of the burst), and that the Steam Audio Android build runs the mode-2
      per-voice HRTF fleet within the headset's CPU budget. The emulator answered 48 kHz with a
      960-frame burst, which is a virtual HAL's number and not a headset's.
- [ ] The AAudio MMAP path. `BWA_SINK_FLAG_EXCLUSIVE` is written and compiled but the emulator
      grants only the legacy shared path, so `AAUDIO_SHARING_MODE_EXCLUSIVE` and the lower burst
      it brings have never run. `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` is likewise requested and
      granted nominally on an image that has no low-latency path to give.
- [ ] `AAUDIO_ERROR_DISCONNECTED` on a REAL route change: unplug a headset or take a Bluetooth
      device mid-run. `device_lost` must set, the host-paced thread must keep the dsp clock
      advancing, and `bwa_stop` then `bwa_destroy` must both return. The error callback and its
      host-thread handshake are the JACK shutdown path's twin, which WAS verified by killing a
      server; the AAudio half is written and compiled and has never fired.
- [x] Static phonon for Linux. Built with the composite action's own steps under WSL (NixOS, gcc
      14.3.0), staged at `lib/linux-x64/`, and the whole suite passed against it, the SDK-gated tests
      included. `readelf -d libbw_audio.so` names no phonon, and
      `bwa_get_audio_backend` reports the `steam HRTF` suffix. What a static phonon adds to the
      link is `libstdc++.so.6`, which a no-SDK build does not carry.
- [ ] Static phonon for macOS, which runs ONLY in CI. Phonon's own CMake does not make itself
      universal (its `set(CMAKE_OSX_ARCHITECTURES ...)` comes after `project()`), so the composite
      action passes the architectures itself and lipo-checks every archive. The Xcode generator's
      tree layout is staged from by CI and nowhere else. The configure assertion
      (`Steam Audio ENABLED`) is what keeps a wrong guess loud instead of silently no-SDK.
- [x] Static phonon for Android, on the emulator. Both ABIs built on a Windows host with the
      composite action's own steps and staged at `lib/android-arm64/` and `lib/android-x64/`, and
      the whole suite run on an x86_64 emulator (API 34): every test but `os` passes every run, the
      SDK-gated tests included, with and without the emulator's audio device. The red `os` is the
      emulator rather than phonon: what fails is `os_sleep_until_ns` median lateness, which
      measures about 1.1 ms against a 1 ms bound on an image that refuses a real-time priority, and
      a no-SDK library of the same commit fails it the same way on the same emulator. Under host
      load `audio_sink` joins it on a block-rate floor. `bwa_minimal` on the device reports
      `backend: aaudio:default (steam HRTF direct)`. `readelf -d` names no phonon and no
      `libc++_shared.so`: the NDK's CMake toolchain links libc++ statically, and the C ABI means
      nothing C++ crosses the library boundary. What it costs is size, 0.4 MB stripped to 7.0 MB
      on arm64. Two android-only upstream patches are in `third_party/patches/`. Nothing in either
      binding's packaging changes: a static phonon ships inside the engine library.
- [ ] The CPU budget for that decode ON a headset. The emulator says it is correct, not that it
      fits: a Quest core is far slower than a desktop one, and the per-voice HRTF convolutions are
      the part that scales with the scene. Measure with the headless bench (docs/profiling.md)
      before committing a game to it.
- [x] The Android packaging for both bindings. The library cross-builds in CI for both ABIs, and
      both packages now carry the `arm64-v8a` library with its import settings: the Unity package at
      `Runtime/Plugins/Android/arm64-v8a/libbw_audio.so` (a committed `PluginImporter` meta, Android
      with CPU ARM64 and nothing else), the Godot addon at `addons/bw_audio/bin/` with the
      `android.template_release.arm64` entries in its manifest. Both pack scripts refuse to pack
      without them.
- [ ] A MAC COLLABORATOR loading both packages for the first time: the Unity package imported into
      a project on macOS (the Editor picks the `.dylib` up, `DllImport("bw_audio")` resolves, Play
      opens CoreAudio on the default output; Unity's own audio is a second client of that device
      in the same process, and the HAL's buffer size is per process, so the engine's block
      becomes Unity's callback size too) and the Godot addon unzipped into a project (the editor loads
      `macos.editor` and the extension finds `libbw_audio.dylib` through `@loader_path`). Both are
      built and name-checked in CI and neither has ever been opened on a Mac. Expect Gatekeeper
      first: the libraries are unsigned, so the quarantine flag on a downloaded archive has to be
      cleared (`xattr -dr com.apple.quarantine <folder>`) before either editor will load them.
- [ ] The same on LINUX: the Unity package in a Linux editor, and the Godot addon in a Linux
      editor. The Linux extension is the one whose engine library sits in `bin/linux/` rather than
      beside it, so what this check really exercises is the `$ORIGIN` run path - and an exported
      Linux game exercises the other half of it, where the exporter flattens every dependency
      beside the binary.
- [ ] An exported build from either binding, ON a headset: a Godot Android export and a Unity
      Android player. The packaging is verified only as far as the archives go (the files are in
      them, at the paths the manifests name, with the import settings Unity needs). Nobody has yet
      exported an APK, installed it, and heard the AAudio sink open under a game engine rather than
      under `tools/android/run-tests.ps1`. That run is also what would catch a Godot exporter that
      does not copy a listed Android dependency into the APK's `lib/` directory.
- [ ] `test_os` on a physical Android device. It is FLAKY on an emulator and only on its two
      tightest timing bounds: ten back-to-back runs on an idle image passed six times, with the
      `os_sleep_until_ns` median lateness straddling its 1.00 ms bound at p50 0.89 to 1.11 ms
      (p99 2.31 to 2.72 ms, max 2.43 to 3.11 ms), and one run also tripping "a past deadline
      returns at once", which allows 1 ms between two clock reads around a call that does not wait.
      Every other assertion passed every time. The `SCHED_FIFO` request is refused, as Android
      refuses it for every app thread. The primitive is the right one
      (`clock_nanosleep(TIMER_ABSTIME)`), so this reads as the virtual machine's scheduler rather
      than the shim, but nothing has measured a real device. The bound was deliberately NOT widened
      to fit one emulator - the same call the Apple row above makes, and for the same reason. A
      headset that also flaps is the finding; a headset that sits well under is the reason to
      leave it.
- [ ] Which binding the experiments reach for first, now that both exist: the nanobind module for
      PsychoPy, or the MEX for Psychtoolbox and Octave. Both are off-hardware verified only.
- [ ] The ASIO reset-request plumbing, when it is written, uses the `device_lost` design in
      [backends.md](./backends.md#4-health) rather than a second mechanism.
- [ ] `cave_both` on the rig: ASIO for the array and WASAPI for the monitor, at once, with the
      monitor's block matching the array's resolved one. Phase 1 makes it resolve; only the rig
      can confirm it. This is the check that closes the defect in [Why](#why).
- [ ] The reported WASAPI output latency against a loopback measurement. The sink reports
      `GetStreamLatency` plus one period (shared) or the buffer (exclusive), plus the one block
      the adapter can hold. On a desk machine that came to 736 frames at a 256 block; nothing has
      measured whether it is right.
- [ ] Exclusive mode past what the onboard codec reached: the format walk past its first accepted
      entry (`int24`), so float32, int32 and int16, and the `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`
      realign-and-retry path. Both need an endpoint that answers differently.
- [ ] The `device_lost` degradation, by unplugging a USB endpoint mid-run: the flag must set, the
      engine must keep advancing its clocks, and `bwa_stop` then `bwa_start` must recover.
