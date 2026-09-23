# CLAUDE.md

Guidance for working in this repository. Read this first, then the file under
`docs/` relevant to the task. The design is settled and implemented through M6
plus the calibration/tooling work (see "Current state" below) — most work now
extends the engine or verifies it on hardware, against these specs.

## What this is

A self-hosted native (C/C++) spatial audio engine for a CAVE installation. It
drives a **speaker array (up to 64 channels; the CAVE installation starts with 24)** over **ASIO**
into an **RME Digiface Dante** (a hardware Dante endpoint), with **binaural (HRTF) headphone output** as a second path — a
first-class direct render (`BWA_PROFILE_BINAURAL`) and an array-audition monitor
(`BWA_PROFILE_CAVE_SIM`). Unity and
Unreal are *thin control clients* over a C ABI — no rendered audio crosses that
boundary, only control (sound triggers, source positions, listener pose). The one
inbound exception is the opt-in push-source feed (caller PCM *into* the engine,
control thread) — a source feed, not a render path.

The engine is deliberately *not* built on FMOD/Wwise/middleware. Self-hosting buys
direct access to ASIO timing hooks (`ASIOTime.systemTime`, `ASIOGetSamplePosition`)
and a clean, engine-agnostic core. See `docs/architecture.md` for the why.

## The seam that organizes everything

Sources → per-voice **listener-relative DBAP** panning → an in-memory
**N-channel master bus** (N = the layout's speaker count; 24 on the CAVE). The bus has
*consumers*:
- **ASIO device** (production): writes the speaker bus straight to the Digiface.
- **Array-sim monitor** (`cave_sim`, and `cave_both`'s tap): treats each bus channel
  as a virtual speaker at its room position, HRTFs to stereo, writes to a normal
  output device.

Adding the sim path did not complicate the core — it is just a second consumer of
the same bus. Protect that property. The one deliberate extension is the
**direct binaural render** (`BWA_PROFILE_BINAURAL`): point voices bypass the
panner — per-voice HRTF convolutions with the SDK (mode 2: rt exposes per-voice
mono taps + directions; spread power-splits toward the field), a 16-ch SH direct
field otherwise — beds pass SH->SH into that field (one diagonal,
ambi_canon_to_phonon) and pathing sums in raw; ONE HRTF decode + the per-voice
convolutions produce stereo. The bus keeps the synthesized-diffuse taps
(FDN/reflection bed). These are profile-gated render targets, not a parallel
engine — anything synthesized-diffuse still belongs on the bus.

## Hard invariants (do not violate)

These are real-time-audio correctness rules. Breaking them causes dropouts/glitches
that are painful to debug, so they are non-negotiable:

1. **No allocation, locks, syscalls, or file I/O on the audio thread.** The audio
   thread is the ASIO `bufferSwitch` callback. wav decode, malloc/free, and logging
   all live on the control thread.
2. **One control thread.** All `bwa_*` calls come from a single thread. The command
   ring is single-producer/single-consumer; a second producer breaks it.
3. **Audio thread owns DSP state** (voice table, bus, panner gains, listener
   active fields). The control thread owns handle allocation and asset memory.
   They communicate only through the two SPSC rings.
4. **Gains ramp, never jump.** Per-voice `gcur -> gtarget` interpolated across the
   block. A discontinuous per-speaker gain change is audible zipper noise.
5. **Generation counts gate handle reuse.** A stale source handle must be dropped,
   not acted on. Sound *buffers* additionally need the retire-ack handshake before
   the control thread frees them.
6. **`CMD_COMMIT` defines frame coherence.** Position/pose write to *pending* fields;
   only commit promotes them to *active*. The mixer reads only active fields.

See `docs/concurrency.md` for the full model and reference code.

## Repo layout (intended)

```
include/bw_audio.h      Public C ABI (authoritative contract).
src/
  engine.c             public ABI: lifecycle + sink + forwards per-frame calls to rt. [M0/M1/M2]
  core/
    rt.h / rt.c          rings, voice table, commit snapshot, generation handles, mixer. [M2]
    sound.h / sound.c    wav decode to mono float via dr_wav (Sound table lives in rt.c). [M3]
    assets.h / assets.c  the SHARED-ownership asset tier: a by-path cache (key = normalized path +
                         load flags, so "one file, memory vs streamed vs ambisonic" is just different
                         entries) with a refcount, over the SAME rt loaders bwa_load_* drives — the
                         Dictionary<path,handle> every binding was rebuilding, moved inward. Plus ONE
                         loader thread for bwa_sound_acquire_async: it decodes off the control thread
                         and hands the buffer over through an SPSC result ring; the control thread
                         publishes it (rt_sound_reserve/publish/abandon) and a play issued meanwhile
                         is HELD control-side until then, so the audio thread only ever sees a
                         finished asset and gains no new branch. The loader BLOCKS on an os_event
                         (signalled after every job push and at stop) rather than sleep-polling, so
                         idle costs zero wakeups. [assets]
    layout.h / layout.c  speaker geometry load (cave_layout.json via cJSON) + default grid. [M4]
    stream.h / stream.c  background file streaming for long sounds (music / ambience), so
                         they never decode the whole file into RAM: a streaming thread fills
                         a per-stream SPSC ring from disk. [streaming]
    sane.h               input sanitizing for values crossing the ABI into audio-thread state:
                         the bwa_finite_* checks over the BWA_MAX_* caps, with the bound a
                         REQUIRED argument (see Traps). Header-only.
    bits.h               bit-twiddling shared by the engine and the offline DSP. Header-only.
    frame.h              the room frame identity basis (BWA_ROOM_*) plus frame_qrot, consumed
                         from one place. The convention itself is public ABI contract.
    profile.h            the zone-profiling seam: Tracy macros that compile to nothing unless
                         BWA_TRACY is defined. [profiling]
    profile_self.h/.c    the headless in-process zone profiler behind BWA_PROFILE_SELF:
                         per-zone numbers with no server tooling (bwa_prof_report). [profiling]
  os/
    os.h / os_win.c / os_posix.c  the OS portability shim: every platform call the engine makes
                         OUTSIDE the sinks (threads, sleep + os_sleep_until_ns, the monotonic clock,
                         mutex + rwlock, os_event (auto-reset, sticky signal, monotonic timed wait),
                         thread priority up (os_thread_set_realtime + its RLIMIT_RTPRIO probe) and
                         down, strdup/strcasecmp, os_fopen + the UTF-8 path family, os_dl_open/_sym/
                         _close (the run-time library load the two Linux sinks use; the Windows half
                         is LoadLibraryW and has no caller yet), natnet's UDP
                         sockets, BWA_EXPORT). Exactly one half compiles. Windows, Linux and Android
                         have device backends; macOS runs the null and manual sinks until phase 4
                         lands. Android takes the POSIX half unchanged (bionic has pthreads,
                         clock_nanosleep, pthread_condattr_setclock); what it refuses is SCHED_FIFO
                         for an app thread, which is the same reported degradation WSL produces.
                         [backends p2]
  sink/
    sink.h / sink.c      device-sink abstraction + backend dispatch: the AUTO order (Windows: 2-ch
                         requests try WASAPI then ASIO, wider ones ASIO only. Linux: JACK then ALSA at
                         every width) and the exact-match device rule. [M1/backends]
    sink_convert.h       planar float bus -> the device's format (f32/i32/i24/i16), planar or
                         interleaved. Moved out of asio_sink.cpp so every backend shares one set of
                         clamp + NaN rules. [backends]
    sink_quant.h/.c      the FIXED-QUANTUM adapter. render() must always see the engine block, but no
                         device API past ASIO promises a fixed callback size, so this renders whole
                         blocks into a ring of block-sized SLOTS (the stride render() already wants,
                         so nothing copies) and serves the device any count. Owns the per-block
                         timestamp rule, the written-versus-device-position dropout, and the
                         late_blocks/render_ns_peak accounting for every backend it serves. [backends]
    jack_sink.c          JACK host (Linux): the production Linux path, and the one backend whose ports
                         are PLANAR FLOAT already, so the bus copies straight out - no convert, no
                         interleave. One JACK2 or pipewire-jack answers at RUN time; JackNoStartServer
                         makes "no server" the fast fall-through to ALSA. Activates and connects at
                         OPEN (that is where a port-count error can still reach the caller) and gates
                         the render on start(). xruns arrive on a notification thread and are parked
                         for the process thread to fold into the adapter. libjack is NOT LINKED:
                         the sink dlopens libjack.so.0 through os_dl_* and resolves its 23 symbols
                         into one table (an X-macro list generates the typedefs, the table and the
                         resolve loop together), so a box with no JACK still loads the engine. A
                         library that will not load is exactly "no device": count 0, an AUTO skip,
                         and an explicit open that fails naming the library; the next open retries.
                         [backends p5]
    alsa_sink.c          ALSA host (Linux): the no-server path - a raw `hw:` card for an experiment
                         that wants the device clock, a MADI/AES67 card for a rig, a plug PCM for a
                         desk monitor. The ONE backend with no fixed-quantum adapter: it writes, so it
                         picks the size, and it writes whole engine blocks. Blocking snd_pcm_writei IS
                         the pacing; -EPIPE is the underrun report; SCHED_FIFO is asked for and its
                         refusal is a reported degradation, never a failure. libasound is NOT
                         LINKED either - same loader shape, 36 symbols, and the snd_*_alloca macros
                         plus snd_strerror respelled over the table because they are library calls
                         too. [backends p5]
    aaudio_sink.c        AAudio host (Android): the headphone path on a standalone VR headset, and
                         STEREO ONLY - Android carries no array transport, so AUTO sends anything
                         wider straight to the offline sink and an explicit open says so. AAudio owns
                         the callback thread and treats setFramesPerDataCallback as a HINT, so the
                         fixed-quantum adapter is in the path. Dropouts are the OS's own
                         getXRunCount DELTA with dropped_frames 0 (AAudio counts events, never their
                         length); device_pos_valid is false, because the queued-depth rule would count
                         the same starve twice. The device rate is PROBED before the real open (one
                         throwaway stream, everything unspecified), since a shared-mode stream reports
                         the rate it was asked for whether or not the service is resampling -
                         without it rule 6 would have nothing to report. setUsage is never called:
                         it is __INTRODUCED_IN(28) and this targets API 26. AAUDIO_ERROR_DISCONNECTED
                         sets device_lost and starts the host-paced thread; nothing reopens.
                         [backends p3]
    worklet_sink.c       Wasm Audio Worklet host (the browser): the headphone path on a PAGE, and
                         STEREO ONLY - a browser carries no array transport, so AUTO sends anything
                         wider to the offline sink the way Android does. The worklet's process()
                         callback IS the audio thread (Emscripten instantiates the module in the
                         AudioWorkletGlobalScope against the SAME shared memory), and it gets 128
                         frames always, so the fixed-quantum adapter is in the path and has its
                         easiest customer: 128 divides every sensible block, and the output buffer
                         is PLANAR FLOAT already, so the runs copy straight out like JACK's.
                         Every webaudio.h call is PROXIED to the browser main thread, because
                         `new AudioContext()` needs a Window and emscripten's handle table is a
                         per-scope JS variable. `device` is a decimal AudioContext HANDLE, so a page
                         keeps the context it created in its gesture handler and keeps the right to
                         resume it. A SUSPENDED context is the normal state and is treated as rule
                         4's host-paced degradation (silence, clocks keep advancing, device_lost
                         set), which means two pullers on one SinkQuant: they share a WAIT-FREE
                         try-lock where whoever finds it taken BAILS, never waits. Health is
                         measured=false, because Web Audio reports no position, no xrun and no
                         underrun of any kind; late_blocks and render_ns_peak are still real.
                         VERIFIED in headless Chrome (bindings/web/tests/run-browser.mjs, the
                         `web_browser` ctest): worklet:1 renders at the audio clock and the
                         suspend/resume handoff flips device_lost both ways. Nobody has LISTENED
                         to it. [web]
    null_sink.c          offline (no-hardware) sink: threaded silence + timestamps. [M1]
    manual_sink.c        offline/deterministic sink: no thread, the caller pumps (bwa_render_block). [M1]
    asio_sink.cpp        ASIO host: driver load, bufferSwitch, sample-pos timestamp. [M1]
    wasapi_sink.cpp      WASAPI host: the headphone profiles + the cave_both monitor, on the endpoint
                         the headphones are actually on. Shared mode by default (IAudioClient3 period;
                         a VR runtime keeps its own audio open beside us), exclusive under
                         BWA_SINK_FLAG_EXCLUSIVE. IAudioClock for the timestamp pair,
                         AUDCLNT_E_DEVICE_INVALIDATED -> health.device_lost with the sink degrading to
                         host-paced silence so the engine's clocks keep advancing. It is also what
                         finally lets cave_both open TWO devices: the ASIO SDK allows one driver per
                         process. [backends]
  spatial/
    dbap.h / dbap.c      listener-relative, constant-power DBAP gain solve. [M4]
    spcap.h / spcap.c    Speaker-Placement Correction Amplitude Panning: the smooth,
                         all-speaker, power-conserving panner for the FIXED-observer case.
                         [spatialization]
    vbap.h / vbap.c      Vector Base Amplitude Panning: the hull triangle that contains the
                         source bearing carries it, at constant power. [spatialization]
    hull.h / hull.c      convex-hull triangulation of unit directions + the VBAP gains within
                         it. Pure and alloc-free; shared by allrad.c (the load-time decode
                         build) and vbap.c. [spatialization]
    allrad.h / allrad.c  All-Round Ambisonic Decoding for the diffuse layer: the SH->speaker
                         bed-decode matrix built over a virtual layer (bed_decoder = 1).
                         [spatialization]
    epad.h / epad.c      Energy-Preserving Ambisonic Decoding for the diffuse layer, same
                         shape and convention (bed_decoder = 2). [spatialization]
    cap.h / cap.c        compensated amplitude panning: projects the dual-band LOW band so the rendered
                         ITD matches a real source's for the head's CURRENT orientation. NOT a panner -
                         a modifier on whatever panner is selected, so it reduces to that panner facing
                         the source. The one place head ORIENTATION reaches the speaker path.
                         bwa_set_dual_band_cap. [spatialization]
    hole.h / hole.c      hole-aware spread floor: a source aimed where the array has NO speaker (the
                         barrel's open poles) is floored WIDE instead of split across the hull triangle
                         that closes the hole. Cached per listener like spcap/vbap; bwa_set_hole_spread. [spatialization]
    align.h / align.c    per-speaker gain trim + delay-line output stage. [M4] Also the tracked-listener
                         re-reference (bwa_set_tracked_align): re-aims the trims from the layout's fixed
                         ref onto the live head, slewed + dead-zoned because every delay change is a
                         resampling event. Off = the exact integer tap, bit-identical. [spatialization]
    ambisonics.h/.c      3rd-order ACN/SN3D encode (+ ambi_encode_phonon, the monitor-basis encode
                         shared by steam_decode and rt's direct mode). [M5]
  acoustics/
    fdn.h / fdn.c        directional FDN reverb bed (phonon-free; takes the reflection bus tap). [innovations]
    ism.h / ism.c        image-source EARLY reflections: shoebox mirrors, panned as point sources. [innovations]
    steam_scene.h/.c     materials occlusion: IPLScene+IPLSimulator on a sim thread (with-SDK). [materials]
    steam_reflect.h/.c   reflection bed: IPLSimulator reflections -> ambisonic IR -> SH->speaker bus tap (with-SDK). [materials]
    steam_path.h/.c      sound pathing: indirect routing -> per-voice shCoeffs -> SH-encode -> bus tap (with-SDK). [materials]
  binaural/
    binaural.h/binaural.c  head-oriented array->stereo monitor + the no-SDK cardioid decode of the
                         direct-binaural field (Steam Audio HRTF is the upgrade). [M5]
    hpeq.h / hpeq.c      headphone correction EQ: AutoEq ParametricEQ.txt -> RBJ biquad cascade on
                         the headphone profiles' final stereo (bwa_load_headphone_eq). [binaural]
    steam_decode.h/.c    production ambisonics->stereo HRTF decode via phonon (with-SDK); sums the
                         direct field into the virtual-speaker encode pre-decode. [M5]
  dsp/
    biquad.h             RBJ "Audio EQ Cookbook" coefficients, a0-normalized, Direct Form I.
                         Shared by the transmission/pathing EQ, the room EQ and the headphone EQ.
    fft.h                small in-place radix-2 FFT (double precision) for the OFFLINE DSP:
                         measurement and the Doppler probe. NEVER the audio thread.
    sos.h                speed of sound for the MEASUREMENT tools and the layout schema. Control
                         side only, never the audio thread.
  tracking/
    natnet.c             OptiTrack pose ingest (off-wire, see docs/build.md). [M6]
    pose.h               the lock-free single-slot pose handoff (seqlock) from the receiver
                         thread to the audio thread. The ONE header that carries stdatomic.h,
                         so nothing else may include it casually (see Traps). [M6]
  calib/
    measure.c/calib.c    bwa_calibrate DSP: sweep+deconvolution, trims, trilateration, room report. [calib]
    zylia.h / zylia.c    Zylia ZM-1: single-position speaker localization (TDOA + GN position) AND the
                         validation-grade estimators — active-intensity DOA, capsule integrity,
                         SRP-PHAT cross-check, comb depth (spectral ripple: what coherent multi-speaker
                         copies cost in timbre, the measurable side of SPCAP focus). [calib]
    valid.h / valid.c    phantom-localization validation: render a source, measure where the array
                         actually put it (feeds/simulate/score + medians, bootstrap, matched-cell
                         contrasts). The PHANTOM arm renders through a REAL ENGINE CORE (a cached
                         RtCore + push voice, limiter off, ramps settled, deterministic timestamp --
                         the same path bwa_render_block/BWA_SINK_MANUAL drives), so every live A/B knob
                         (ValidRender: focus/density, dual-band, CAP, hole spread, tracked align,
                         spread mode/decorrelation/near spread) is sweepable and valid_simulate just
                         propagates those feeds to the 19 capsules. The PHYSICAL REFERENCE arm (drive
                         one speaker alone = a real source, so a phantom miss reads against a floor --
                         also the comb-depth floor) deliberately does NOT: no panner, no knob, no
                         engine state. valid_speaker_feeds_direct is the pre-engine builder, kept as
                         the regression baseline the ctest pins the engine render against. Also
                         stimulus selection (broadband or a tone, analysis band follows). Drives
                         bwa_validate. [validation]
test/                  ctest suite; targets are prefixed test_* (test_smoke, test_rt_core, test_rt_feature,
                       test_dsp, ...) so the built tools (bwa_*) and the tests sort apart in the bin dir.
                       The rt test is split: test_rt_core (concurrency/lifecycle spine) + test_rt_feature
                       (spatial-feature DSP toggles), sharing test/rt_test_util.h. xval_data.h is
                       GENERATED (tools/xval) — don't hand-edit.
tools/android/         run-tests.ps1: ctest cannot drive an Android target, so this pushes
                       libbw_audio.so plus the test executables to /data/local/tmp/bwa over adb and
                       runs each one there, mapping the skip code 77 through. It reads the test list
                       out of the build dir's CTestTestfile.cmake, so it and ctest cannot disagree
                       about what the suite IS. [backends p3]
tools/phonon/          build-phonon.sh: THE phonon recipe (env-driven: BWA_PLATFORM, BWA_ARCH,
                       BWA_STAGE_DIR, BWA_CMAKE_FLAGS, BWA_ART). Two callers and no second copy of
                       the steps - the composite action .github/actions/build-phonon, and
                       cibw-before-all-linux.sh, which builds phonon INSIDE the manylinux container
                       that links the Python wheel (a composite action runs on the runner, so it
                       cannot). cibw-before-all-macos.sh is the universal2 side. [python wheels]
tools/ci/              build-engine-manylinux.sh: the LINUX engine, built, ctested and installed as
                       an SDK INSIDE the manylinux_2_28 image. A manylinux wheel needs a glibc-2.28
                       engine and ubuntu-latest carries 2.39, so CI used to build two Linux engines
                       and ship the one nothing had tested. Only the engine step is containerized -
                       MATLAB and the Godot toolchain do not install in AlmaLinux 8 and do not need
                       to, since a binding compiled on the runner against a glibc-2.28 shared object
                       is an ordinary ABI client. [engine sdk]
tools/wasm/            wasi-sdk.toolchain.cmake + build-wasm.sh: the wasm32 build. TWO toolchains and they
                       are not interchangeable: wasi-sdk (wasm32-wasip1-threads, plain clang, a STATIC
                       libbw_audio.a plus the offline test binaries, 33/33 under wasmtime) is the CI and
                       offline leg; Emscripten with -pthread is the browser leg (Web Audio + Workers;
                       35/38 under node with the wasm phonon staged, the three reds being `os` and
                       `idle`, control-on-main-thread blocking waits, and `fuzz_api`, a phonon throw
                       aborting the module because the wasm phonon has exceptions off). The
                       toolchain file names the four link flags a run needs and what breaks without each;
                       the shadow-stack one is the trap below. Null and manual sinks only: the AudioWorklet
                       sink is the HOST's job. os_posix.c is the shim (BWA_OS_NO_SCHED + wasi socket stubs),
                       following the Android precedent. Decided 2026-09-21: faithful two-thread over
                       SharedArrayBuffer, control on a Worker, COOP/COEP accepted. The files:
                       build-wasm.sh (THE engine recipe for wasm32: wasi-sdk or emsdk, BWA_WASM_PROXY=1
                       for the -sPROXY_TO_PTHREAD shape the decision wants, CMAKE_MAKE_PROGRAM forwarded
                       for a host whose ninja is off PATH, ctest under a 600 s per-test timeout because
                       a broken wasm test hangs rather than fails), wasi-sdk.toolchain.cmake,
                       build-web.sh (THE shippable-page recipe: BWA_WITH_WORKLET + BWA_BUILD_WEB, staged
                       into bindings/web/dist/, three.js fetched by fetch-web-vendor.sh with sha256
                       pins) and gen-abi.mjs (the web binding's raw layer + export list, from the
                       header). docs/web.md. [wasm]
tools/layout/          gen_dome.py: the playgrounds' default array, a 24-speaker dome evenly spread over
                       the sphere ABOVE the floor. Writes examples/dome_24.json and the Godot addon's
                       copy (playground/dome_24.json) - regenerate, never hand-edit either. Stdlib only.
tools/xval/            gen_reference.py: cross-validation golden generator (scipy SH / l1-LP VBAP /
                       qhull AllRAD / bilinear RBJ / lfilter) -> test/xval_data.h for the xval ctest.
                       Needs numpy+scipy; ctest itself does not (the header is committed).
bindings/
  unity/               P/Invoke + Engine/Emitter (see docs/integration.md).
  godot/               GDExtension (godot-cpp, opt-in -DBWA_BUILD_GODOT=ON): addons/bw_audio/ is the
                       drop-in addon (which CONTAINS playground/, the Godot port of
                       examples/playground.cpp, so the demo ships with every install), demo/ the
                       self-checking test scenes (CI fixtures, never shipped). No 1:1 shim — each call lives on the class owning
                       its handle (BwaEngine / BwaSource → BwaEmitter, BwaPushSource / BwaBed), plus
                       BwaMaterial, Bwa{Acoustic,Dynamic}Geometry, BwaRoomBox, BwaSpeakerView.
  python/              nanobind extension (opt-in -DBWA_BUILD_PYTHON=ON; scikit-build-core wheel via
                       `uv build --wheel`). TWO layers, unlike the other two bindings: bw_audio._bwa
                       is the RAW 1:1 ABI (C name minus the bwa_ prefix, C field names, C units) and
                       bw_audio is a thin Pythonic layer over it (Engine context manager, Sound /
                       Source / PushSource / Bed / Listener, exceptions from bwa_result). The one
                       EXCLUSION is bwa_set_output_capture — its callback runs on the audio thread
                       and an interpreter must never run there (invariant 1, and docs/backends.md
                       says so for this binding by name); the manual sink + render_block is the
                       offline path, handing back a READ-ONLY numpy view of the engine's own planar
                       buffer with no copy. The GIL is released around the blocking/IO calls and
                       render_block; the one-control-thread rule gets a runtime guard here (Python
                       makes threads cheap where a game loop made the rule free). pyproject.toml
                       points scikit-build-core at the REPO ROOT with BWA_BUILD_PYTHON=ON, never at
                       bindings/python — the ASIO and phonon lookups resolve against
                       ${CMAKE_SOURCE_DIR} and a top-level project here would silently ship a
                       no-SDK, no-ASIO engine. The package version is BWA_VERSION from the header,
                       so a wheel cannot claim a version the library is not. Audience: PsychoPy
                       (Psychtoolbox is MATLAB and Octave, and gets the MEX below).
  matlab/              ONE classic-C-MEX gateway (opt-in -DBWA_BUILD_MATLAB=ON), for Psychtoolbox.
                       Two layers like python's: bwa_mex is the RAW 1:1 ABI, dispatching on a
                       SUBCOMMAND STRING (`bwa_mex('source_play', h, src, snd, false)`) because that
                       is PsychPortAudio's own shape and this audience reads it without being told;
                       +bwa is the class layer (Engine / Source / PushSource / Bed / Sound /
                       Listener / ClockBridge, plus Const for the enums) carrying the SAME
                       auto-commit model the python layer has, in the form MATLAB can express
                       (beginFrame/endFrame plus an onCleanup-based frame()). Same one EXCLUSION,
                       bwa_set_output_capture, kept as a subcommand that REFUSES with the reason so
                       a reader finds it where they looked. The API pin is the whole portability
                       story: the classic C MEX API is what OCTAVE implements, so one source builds
                       for both interpreters — but they need different BINARIES (different mexext,
                       different compiler), staged apart under bin/<matlab|octave>/<platform>/ and
                       picked at run time by +bwa/setup.m. The one-control-thread rule is free here
                       (a MEX call runs on the interpreter's main thread), so what needs care is
                       LIFETIME: mexLock while any engine is live so `clear mex` cannot unload the
                       gateway under a running audio thread, and mexAtExit stops + destroys the rest.
                       render_block hands back an [nframes, channels] single matrix, a COPY where
                       python's is a view (an mxArray cannot alias engine memory) — one memcpy and no
                       transpose, because the planar block IS column-major for that shape. Octave's
                       classdef support is partial and the layer is written to the intersection; the
                       one construct that BITES rather than failing to parse is a reference cycle
                       (Octave refcounts handle objects), so Engine.listener is DEPENDENT and not a
                       stored object pointing back.
  web/                 JS/ES-module binding (opt-in -DBWA_BUILD_WEB=ON, Emscripten only; built by
                       tools/wasm/build-web.sh into a self-contained bindings/web/dist/ with no
                       bundler and no server-side step). TWO LAYERS like python's: src/raw.js is the
                       RAW 1:1 ABI (C name minus bwa_, C units) built from src/abi.js, which is
                       GENERATED from the header by tools/wasm/gen-abi.mjs along with the
                       -sEXPORTED_FUNCTIONS list - so the layer cannot drift and the module cannot
                       carry a call it never heard of. src/engine.js is the thin idiomatic layer
                       (Engine / Sound / Source / PushSource / Bed / Listener, the same auto-commit
                       model python's has). Same ONE EXCLUSION, bwa_set_output_capture, absent from
                       BOTH layers rather than present-and-refusing. The one-control-thread rule is
                       STRUCTURAL here: the engine pointer lives only in the control Worker, and the
                       page reaches it through src/client.js over a small typed postMessage
                       protocol. The PER-FRAME path is a SharedArrayBuffer frame slab plus ONE
                       message (pose + N moved source positions + one commit), because a message
                       per call spreads one visual frame's writes across several audio blocks, which
                       is exactly what CMD_COMMIT exists to prevent. TWO TOPOLOGIES, and the second
                       is a measured deviation from docs/web.md's decision: "worker" (the default,
                       every sink but WORKLET) and "main" (the ONLY one where the AudioWorklet sink
                       can open, because new AudioContext() needs a Window and emscripten proxies
                       none of its Web Audio). example/ is a one-button page + serve.mjs, the tiny
                       COOP/COEP node server a local try needs. tests/ runs under node against the
                       SHIPPED module (-sENVIRONMENT includes node for that). [web]
  unreal/              module + component — planned, not yet implemented (docs/integration.md has the notes).
cmake/                 bw_audioConfig.cmake.in + bwa_bindings.cmake. The first is the package config
                       the ENGINE SDK installs (`cmake --install <build> --prefix <sdk> --component
                       bwa_sdk`), which is how a binding links a PREBUILT engine instead of
                       compiling one: `-DBWA_ENGINE_SDK=<sdk>` at the root compiles nothing under
                       src/, imports `bwa::bw_audio` from there and includes the bindings only. The
                       second holds the three binding add_subdirectory blocks in ONE macro, because
                       both root modes call it and a second copy would let one mode gain a binding
                       the other has not. In-tree, `bwa::bw_audio` is an ALIAS of the real target,
                       so a binding never asks which mode built it. [engine sdk]
docs/                  Specs. Start here.
examples/              cave_layout.json (see docs/layout-schema.md); minimal.c (the client lifecycle),
                       ambisonic.c (beds: AmbiX/FuMa load, rotate/tilt, renderer + max-rE A/B),
                       streaming.c (disk streaming + push sources), convenience.c (the convenience
                       tier: shared/async assets, bwa_source_desc, group + scene stops, each part
                       naming the core calls it replaces) — console walkthroughs, built every build.
third_party/           asiosdk/ (GPLv3 option, fetched not committed), steam-audio-source/ (submodule) + steam-audio-artifacts/ (built phonon SDK, static archives per platform); dr_wav + cJSON are
                       fetched by CMake (FetchContent, pinned) — see third_party/README.md.
```

## Build

Target: **Windows** is production (ASIO is Windows-only; the Digiface is Windows/macOS). CMake.
Linux is a second host: JACK and ALSA are device backends there (docs/backends.md phase 5).
**Android** is a third: AAudio is a device backend there (phase 3), stereo only, cross-built with
the NDK against API 26 or later - `tools/android/run-tests.ps1` runs the suite on a device or
emulator, and docs/build.md's "Android" section has the toolchain. CI cross-builds both ABIs in its
own `android` job and ships them as their own artifact (`bw_audio-android-<ver>`); both bindings carry
the arm64-v8a library, and the `package` job takes it from that job rather than cross-building one of
its own. It carries phonon too now, built per ABI by the same composite action; nothing in either
binding's packaging changed for that, because phonon is linked STATICALLY and lands inside the
engine library (which costs 0.4 MB -> 7.0 MB stripped on arm64).
macOS builds on the null/manual sinks until phase 4 lands. Do not bake any backend's assumptions outside its own `*_sink` file.

The `linux` and `macos` jobs SHIP the same way since 2026-09-15: each builds its own Godot
GDExtension (both flavours) and uploads a standalone engine artifact (`bw_audio-linux-x64-<ver>`,
`bw_audio-macos-universal-<ver>`, the latter a UNIVERSAL x86_64+arm64 build) plus a fixed-name
`linux-pack-input` / `macos-pack-input` the `package` job downloads. So both bindings now carry FOUR
platforms' engine libraries and a tagged release has EIGHTEEN assets (nine of them Python wheels,
see below).

BUILDING AND PACKING ARE TWO JOBS since 2026-09-16. `windows` builds and tests the Windows engine
and every binding that links it, waits for NOTHING, and ends by uploading its own fixed-name
`windows-pack-input` the way the other three jobs do. `package` is the one that waits (`needs:
[windows, android, linux, macos, wheels]`): it compiles nothing, downloads the four pack inputs plus
the release wheels, packs the Godot addon, the Unity package and the MATLAB toolbox, and on a tag
cuts the release. The per-platform engine artifacts are uploaded by the jobs that built them, so
they survive a packing failure. Two consequences in the scripts: the windows job now builds the
`template_release` Godot flavour itself (the pack used to), and `tools/godot/pack.ps1` has a
`-WindowsFrom` switch beside its three cross-platform ones. Two wrinkles worth knowing: the Godot addon keeps the Linux engine
library in `bin/linux/` because Android's has the same file name (the extension finds it through an
`$ORIGIN` run path), and the macOS binaries are UNSIGNED and un-notarized, so a downloaded addon or
package needs its quarantine flag cleared before either editor loads it.

```
cmake -S . -B build -A x64      # default generator = newest installed Visual Studio
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo      # runs the full test suite (test_* targets)
```

**Two root modes, and ONE engine per platform.** A plain configure is the above. With
`-DBWA_ENGINE_SDK=<dir>` the root compiles NOTHING under src/ - no engine, no test, no tool - and
instead imports `bwa::bw_audio` out of an install tree a previous build wrote
(`cmake --install <build> --prefix <dir> --component bwa_sdk`), then includes whichever bindings
are enabled. The component matters: the same tree carries raylib's install rules and the Python
binding's, and an unfiltered install mixes all three into one prefix. Both modes hand the bindings
the one name `bwa::bw_audio` and take the engine file from `$<TARGET_FILE:bwa::bw_audio>`, and the
repo root stays the CMake source dir either way, which is what lets pyproject keep
`cmake.source-dir = "../.."`; a wheel reaches SDK mode through
`uv build --wheel -C cmake.define.BWA_ENGINE_SDK=<dir>`. This exists because each binding used to
pull the engine in as a source dependency: a desktop CI job compiled the engine for its ctest run,
again for each of the two Godot flavours, and again inside the wheel build - four binaries that
were only meant to be the same. CI now installs the SDK right after ctest and configures every
binding against it, and each staging step asserts the file it staged against the SDK's. The Linux
engine is built inside the manylinux_2_28 container (tools/ci/build-engine-manylinux.sh) so the
release wheel and the tested binary are the same file; the `wheels` job builds no engine and no
phonon and therefore `needs: [linux, macos]`. Local test counts do not change - configure
everything in one tree and you get the same suite. What changes is CI's per-tree split: the engine
tree registers 45 on Windows at full options and 38 off it, and each job's bindings tree registers
the binding tests alone.

**Current state (M6 + occlusion).** The engine builds `bw_audio.dll` and the full ctest
suite — 45 tests with the Steam Audio SDK, 40 without (the 5 SDK-gated ones are `reflect`,
`bake`, `path`, `dynmesh`, `steam_decode`) — a count that INCLUDES the three GUI-tool suites
(`calib_view`, `layout_tool`, `playground`), the four `validate_*` runs, and the four
`example_*` runs (the console examples driven with `--tests`: offline sink, short waits), all
under their build flags. On Linux, macOS or Android at the DEFAULT options it is 33: `calib_view`
and the ASIO capture tools are WIN32-only targets there, which drops the viewer's suite and the
`validate_*` runs on top of the SDK-gated five, and `layout_tool` + `playground` sit behind
`BWA_BUILD_PLAYGROUND`, which defaults OFF. Those two are NOT Windows-bound (raylib + rlImGui +
imgui and nothing else, since 2026-09-21) - turn the option on in a Linux tree and their suites come
back, for 40 with phonon and 35 without, MEASURED on Ubuntu 22.04 / gcc 11.4. They need a DISPLAY:
a WSLg or X session, or `xvfb-run ctest` (software GL passes both suites). Android runs those 33
through `tools/android/run-tests.ps1` rather
than ctest, because the binaries are the device's. **Linux, macOS and Android now stage phonon
too** (CI builds it per platform into `lib/linux-x64` / `lib/osx-universal` /
`lib/android-arm64` + `lib/android-x64`), so the count there is **38**:
those 33 plus the SDK-gated five. Linux is verified locally, 38/38 against a static phonon built
with gcc 14.3; Android is verified locally too, 37/38 on an x86_64 emulator (the red is `os`'s
sleep-lateness bound, which a no-SDK library of the same commit misses identically); macOS is CI-only. Phase 5 added no target - the JACK and ALSA sections live inside
`test_audio_sink`, and on a box with no server and no card that one test reports SKIPPED rather
than passing. The UTF-8 path work added three (`utf8_path`, `idle`, `cave_both`), on every
platform. `-DBWA_BUILD_PYTHON=ON` adds four more on top of whatever the rest of the flags give
(`python_bindings` plus `python_example_minimal` / `python_example_offline_render` /
`python_example_live_onset`), so the full-options Windows tree is 49 and the default Windows tree
42; `python_bindings` reports SKIPPED rather than failing when pytest is missing, because a C
developer should not need it. The `minimal` example is the SAME demo in every binding since
2026-09-22 (a hand-spelled LCG click orbiting the head, docs/integration.md "The minimal
example"), so a change to the stimulus is a change to five files plus the web page.
`-DBWA_BUILD_WEB=ON` is EMSCRIPTEN-ONLY and adds four (`web_bindings`, the node tests against the
module that was just linked, 32 of them as of 2026-09-22 including the per-profile master-gain pin
and the XR frame seam; `web_browser`, the AudioWorklet sink driven in headless Chromium;
`web_playground` and `web_xr`, the two demo pages driven the same way, each with a `--site` mode
that drives the STAGED copy with its content-addressed dist), each SKIPPING rather than failing
when node or a browser is missing. It changes no desktop count, because a desktop configure
refuses the option with a status line. The two page checks are load-sensitive (a 4 ms meter tick
against a 2 ms click): one ran red while the full native suite ran beside it and green twice
after, so run them alone before believing a red.
`-DBWA_BUILD_MATLAB=ON` adds up to FIVE PER INTERPRETER it finds (`<matlab|octave>_tests` plus
`_example_minimal` / `_example_offline_render` / `_example_live_onset` /
`_example_AudioTunnel3DDemo_bwa`), so a Windows box with both MATLAB and Octave installed reaches
55 (45 + 10) and 59 at full options; a Linux or macOS box with Octave alone reaches 43 (38 + 5) and
with both 48 (38 + 10). Each suite exits 77
(SKIPPED) when the MEX for the running interpreter
was not staged, and neither half is registered when its toolchain was not found at configure time -
ctest cannot run a MATLAB test with no MATLAB. In CI BOTH MEX files are built INSIDE each desktop
job (so each links that job's own engine, backends intact) and the `package` job assembles all six
into ONE toolbox folder, bin/{win64,glnxa64,maca64} each holding that platform's pair plus the ONE
engine library both load, shipped as its own release asset. Every desktop job installs its own
Octave (apt on Linux, chocolatey's `octave.portable` on Windows, homebrew on macOS), configures
`BWA_BUILD_MATLAB=ON` before the engine build, and runs the four `octave_*` tests through ctest:
that is the 42 the linux and macos jobs report, and on Windows 49 registered with 46 run (the three
GUI suites need a display). MATLAB's four never run under ctest in CI - the license exists only
inside matlab-actions' run-command, so each job drives them there instead. Those MATLAB steps are
UNVERIFIED LOCALLY - no runner MATLAB is
reachable from here, so they follow mathworks/ci-configuration-examples. The Windows Octave build
IS verified locally (Octave 10.1.0, the four tests green); the macOS one is not. CI also
builds a `cp312-abi3` WHEEL on each of the three desktops, installs it into a fresh venv, and runs
that same pytest suite from the INSTALLED wheel rather than the source tree - shipped as
`bw_audio-python-<platform>-<ver>`. Those three are the FAST GATE and are tagged for the runner
that built them AND for the one interpreter the step pins, so none of them is a release asset; a
separate `wheels` job builds the NINE a RELEASE ships with cibuildwheel, on all three desktops
(`manylinux_2_28` in a container, macOS `universal2`, Windows in place with nuget-installed
CPythons). Three per platform: `cp310` and `cp311` version-specific, `cp312-abi3` covering 3.12 and
every later Python, which is nanobind's stable-ABI floor; 3.10 is the language floor, set by the
pinned nanobind's own `Requires-Python`. cibuildwheel's Windows `delvewheel` repair is turned OFF -
it fails on this wheel and a working repair would mangle the engine's filename out from under
`os.add_dll_directory`. The wheels job is also why the phonon recipe is now ONE
script, `tools/phonon/build-phonon.sh`, that both the composite action and the container's
before-all call - a composite action runs on the runner, and a wheel's phonon has to come from the
image that links it. `rt.c` is the concurrency
spine (two SPSC rings, voice + sound tables, commit snapshot, generation handles, retire-ack)
and the whole `bwa_*` API forwards to it. Spatialization (the DBAP/SPCAP/VBAP gain solve,
layout load, per-speaker align), calibration (`bwa_calibrate`, the Zylia capsule survey),
validation (`bwa_validate`), and OptiTrack tracking (`natnet.c` + the seqlock pose handoff)
are all implemented and covered by the off-hardware test suite.

Four subsystems gate on `BWA_HAVE_STEAMAUDIO` (phonon built from the vendored submodule and
linked STATICALLY since 2026-09-15 — four archives under
`third_party/steam-audio-artifacts/lib/<platform>/`, nothing beside the engine at runtime):
`steam_decode.c` (the production HRTF monitor upgrade), `steam_scene.c` (automatic occlusion +
transmission EQ + directivity), `steam_reflect.c` (the reflection bed), and `steam_path.c`
(sound pathing). A no-SDK build is fully viable: the simple-pan binaural monitor, ISM early
reflections + the FDN late tail for reverb, and manual occlusion cover the same ground. What
it loses is automatic occlusion, pathing, and the real HRTF monitor.

Channel count is runtime. `BWA_MAX_CHANNELS` (64, `bw_audio.h`; `sink.h`'s `BWA_CHANNELS` is an
alias) is the CAPACITY, a transport fact (an ASIO/MADI/Dante endpoint carries 64), not a rig
detail. The layout's speaker count (4..BWA_MAX_CHANNELS) is the ACTIVE count, fixed per engine
instance and threaded through the rt core, sinks, monitor, and FDN. `bwa_get_channel_count()`
reads it back. `BWA_DEFAULT_GRID` (26) is the built-in 3x3x3-minus-center grid that runs with no
layout_path. A failed explicit layout load leaves `bwa_create` usable on that grid (reason via
`bwa_last_error`), but `bwa_start` refuses it with `BWA_ERR_LAYOUT` - only `layout_path = NULL`
runs the default grid. The capacity and the default grid are DIFFERENT numbers since the cap went
26 -> 64: never use one where you mean the other. Tests that build a core with no layout use
`BWA_DEFAULT_GRID`; fixed arrays use `BWA_MAX_CHANNELS`; loops use the active count. The rig
starts with 24 and may grow to 36, which is a layout file, not a recompile. The three playgrounds
(native, web, Godot) default to a generated 24-speaker DOME (`examples/dome_24.json`, from
`tools/layout/gen_dome.py`), not the default grid: argument > ./cave_layout.json > dome > grid.

The three GUI tools are on the imgui stack — `calib_view` on imgui + implot + implot3d (win32 +
d3d11, so WINDOWS-ONLY, and it links the ASIO capture shells too), `layout_tool` and `playground`
on rlImGui (a raylib 3D scene under imgui panels, and PORTABLE — they build and pass on Linux) —
and each has a `--tests` suite that drives the real UI under ctest.

For the feature-level catalog — every `bwa_*` call and what it does — see docs/api.md's
"Feature overview". NOTES.md holds the historical per-feature narration this section used to carry.

Remaining: the by-ear headphone check (HRTF quality), and live Motive verification of the
tracker path (parser + lifecycle are tested off-wire). See docs/hardware-validation.md.

## Traps

Regression-preventing gotchas. Each has bitten before or guards a real invariant:

- **Laterality checks must never drive DC.** A DC-driven `steam_decode` laterality assertion had
  inverted polarity (the default HRTF's per-ear DC gains oppose its audible ILD), mis-diagnosed a
  correct encode, and shipped a left/right mirror that only a by-ear report caught. Drive tones,
  never DC. See NOTES.md.
- **`isfinite()` is not a range check — finite-but-absurd is its own defect class.** `isfinite(3e38)`
  is TRUE, so the whole reject-non-finite guard family passes it, and then `bus * 3e38` OVERFLOWS to
  Inf. Gains are sticky, so every later block overflows too, and the Inf reaches the align delay line
  and the room-EQ biquads, whose IIR state holds it past any later correction. `test_fuzz_api` seed
  12648430 found this on master gain. Any new value that SCALES the bus needs a magnitude cap, not
  just a finite check. Note this is invisible to a reviewer scanning for missing guards — the guard
  is right there.
  The same defect then turned up on POSITION, which is worth stating separately because the
  overflow step is different: nothing multiplies a coordinate by the bus, but EVERY spatial solve
  begins by SQUARING a coordinate difference (`dbap.c`'s dist2, the spread frame's normalize, the
  ISM path length), so `dx*dx` on a 3e38 is +Inf before any downstream guard sees a number it could
  reject, and the Inf/Inf or Inf*0 of the normalize that follows is NaN — which the gain ramp
  `x + (t - x) * k` can never expel. `test_fuzz_api` seeds 126 / 142 / 185 found it on the listener
  pose. Corollary: a value DERIVED from a bounded one is not automatically bounded. An ISM room
  dimension is not a coordinate but `ism.c` mirrors the source across it (`2*plane - src`), so an
  absurd-but-finite room is an absurd IMAGE position that reaches the panner the same way.
  There are now THREE magnitude caps in this family, all in `rt.h`: `BWA_MAX_GAIN` (+80 dB, anything
  that scales the bus), `BWA_MAX_SAMPLE` (+30 dBFS, any sample entering from outside) and
  `BWA_MAX_COORD` (1e6 m, every room coordinate), plus `BWA_MAX_ROOM_DIM` derived from the last as
  the factor one image-source mirror multiplies through. `sane.h` spells the checks
  (`bwa_finite_clamp` / `bwa_finite_bounded` / `bwa_finite3_bounded`) with the bound as a REQUIRED
  argument, so "I checked finite and forgot the magnitude" stops being expressible — use them.
- **`powf(base, exp)` with a negative base and a NON-INTEGER exponent is NaN**, and one NaN
  poisons a whole normalized gain vector. `spcap.c`'s lobe `0.5 + 0.5*cos` rounds to ~-5e-8 for an
  antipodal speaker, which was harmless for as long as the focus exponent was the integer 12 and
  silenced *every* default-grid SPCAP solve the moment focus became geometry-derived (12.70). The
  lobe is clamped at 0 now. Any exponent that stops being a literal integer re-opens this class:
  clamp the base, do not trust the caller's range.
- **Publish-then-flag needs the reader to acquire the flag FIRST.** `rt_set_spcap_focus` stores
  focus/density and then release-bumps `RtCore.pan_gen`; `rt_render` must acquire `pan_gen` *before*
  loading the knobs. Read in the other order it is not a memory-model subtlety but a plain
  program-order interleaving (it bites on x86): a block pairs the NEW generation with the OLD value,
  stamps every voice current, and swallows the change until the next bump, so the last set of a
  slider drag silently never takes. Same rule for any future live knob that uses a generation
  counter, and note this is untestable single-threaded, so the ordering comment is the guard.
- **`/experimental:c11atomics`** is required on MSVC for every file that includes `stdatomic.h` —
  wired per-file in CMake, where source properties are directory-scoped so one entry covers every
  target compiling that file. The list is `rt.c`, `stream.c`, `assets.c`, `fdn.c`, `natnet.c`,
  `engine.c`, `null_sink.c`, `profile_self.c`, `steam_scene.c`, `steam_path.c`, `steam_reflect.c`,
  plus `test/os_test.c`, `test/natnet_test.c`, `test/audio_sink_test.c` and
  `test/rt_feature_test.c`. Miss one and the error is a confusing `<stdatomic.h> is not yet
  supported`, pointing at the header rather than at the missing flag. The list is deliberately
  MSVC-only, so a platform sink MSVC never compiles stays off it however many atomics it carries:
  `jack_sink.c`, `alsa_sink.c`, `aaudio_sink.c` and `worklet_sink.c` all use `stdatomic.h` and
  none belongs there.
- **`pose.h` is the one HEADER that carries `stdatomic.h`, so nothing else may include it
  casually.** Its seqlock moved off the Interlocked intrinsics onto C11 atomics (Boehm 2012: the
  payload fields are relaxed atomics, and fences carry the ordering), which means every translation
  unit that includes it needs the MSVC flag. `rt.h` and `natnet.h` therefore FORWARD-DECLARE
  `PoseSlot` — include `pose.h` only where `pose_read`/`pose_write` are actually called, or the
  flag spreads to twenty-odd files. Two consequences worth knowing: `_Atomic` is not C++, so
  `pose.h` gives C++ the type as opaque and `examples/validate.cpp` reads the pose through
  `natnet_read_pose` instead; and the payload is no longer memcpy-able, so a caller that used to
  seed `slot.p` directly must go through `pose_write` or stop seeding (rt.c's readback did the
  latter — `rt_read_pose` already falls back to the active fields).
- **Nothing in `src/` outside the `*_sink` files may call the OS directly.** `src/os/os.h` is the
  seam (threads, sleep, `os_sleep_until_ns`, the monotonic clock, mutex + rwlock, thread priority,
  `strdup`/`strcasecmp`, UDP sockets, `BWA_EXPORT`), with `os_win.c` and `os_posix.c` behind it.
  It must stay C++-includable, because `sink.h` includes it and the two C++ sinks include
  `sink.h`: no `<stdatomic.h>`, no `<windows.h>`, no `<winsock2.h>` in that header, ever. The .c
  files use C11 atomics directly instead. Tests may include `src/os/os.h`; examples are client code
  of the public ABI and use `examples/portable.h` instead.
- **A self-paced loop waits on an ABSOLUTE deadline, never a relative sleep.** `os_sleep_until_ns`
  exists because a relative sleep is computed from a clock reading that is already stale, so its
  error accumulates block after block. It also removes the reason the null sink used to call
  `timeBeginPeriod(1)`, which is SYSTEM-WIDE in effect: a visual-only tool that happened to open
  the offline sink held the whole machine at a 1 ms timer tick. Device-paced paths (the ASIO
  callback, the WASAPI event wait) keep their own wait — the device is the clock there.
- **An ALSA "macro" can be a library CALL.** `snd_pcm_hw_params_alloca(&hw)` looks like pure
  stack arithmetic and expands to `alloca(snd_pcm_hw_params_sizeof())` - a symbol like any other,
  and one of three (`_status`, `_hw_params`, `_sw_params`). `snd_strerror` is another, behind a
  name that reads like libc. Moving `alsa_sink.c` onto a dlopen'd libasound therefore could not be
  driven by grepping `snd_*(` alone: the three `_alloca` call sites carry no library name at all.
  What CATCHES the class is the link line, not the reading: with nothing linked, a missed symbol is
  an undefined reference at build time rather than a crash on a machine that has no alsa-lib. The
  CI assertion is the same shape - `nm -D --undefined-only | grep -cE ' (snd_|jack_)'` must be 0.
- **Do not link the NatNet SDK.** It is proprietary and conflicts with GPLv3 under distribution.
  `natnet.c` parses the wire format off-wire (reference only, never linked).
- **Proprietary VR-toolkit integrations (MiddleVR, Igloo) live OUTSIDE this repo**, in their own
  package consuming a released `com.brainworks.bw_audio`. Same reasoning as the NatNet rule: the
  Unity package is `GPL-3.0-only`, and an assembly referencing proprietary DLLs shipped inside it
  raises the same distribution conflict. Second, independent reason: CI has no license for either, so
  in-repo code would never be compiled by anything, giving sprawl AND silent drift. The sync cost is
  small because the contact patch is small (pose, registration, listener), and an integration SHOULD
  pin a released ABI rather than chase a moving one. What must track the ABI exactly (the rig-day
  harness) stays in-repo, where CI compiles it.
- **Assemblies point INWARD only.** `BwAudio` (core, no references) <- `BwAudio.RigDay` <-
  `BwAudio.RigDay.Editor`. The core must never reference a tool or an integration, and the asmdefs
  make that a compile error rather than a matter of discipline. The rig-day assemblies are
  `autoReferenced: false` so they stay out of a consumer's default reference set.
- **`tools/upm/gen-meta.ps1` must RECURSE.** An `.asmdef` governs its own folder, so assembly splits
  mean subfolders, and the original flat `Get-ChildItem -File` silently skipped every asset in one
  (folders included, which need a `.meta` as much as files do). A missing `.meta` regenerates that
  GUID per project and breaks every reference into it, which is the exact failure the script exists
  to prevent. Both existing asmdefs also carried `noBwAudioReferences`, a mangled `noEngineReferences`
  that Unity silently ignores.
- **Do not bake a backend's assumptions outside its own `*_sink` file.** ASIO is just the Windows
  ARRAY sink, WASAPI the Windows monitor sink, JACK and ALSA the Linux pair; the cross-platform
  move added files behind the same seam and changed nothing else. The two shared pieces are
  deliberate exceptions and carry no backend types: `sink_convert.h` and `sink_quant.c`. The one
  thing that is NOT an exception is `alsa_sink.c`'s `S24_LE` interleave: the shared header's int24
  is three PACKED bytes, and a 24-bit value in a 32-bit container is a different container, so the
  sink writes the container itself and still takes its clamp and NaN rules from `sink_to_i32`.
- **`bwa_desc.device` and `sink_flags` describe the PRIMARY device only.** `cave_both` opens two,
  and handing the array's device string to the monitor's open is not a harmless copy: on the rig
  `device` names the ASIO driver, rule 10 SKIPS a backend that has no device by that name, so the
  monitor's 2-channel AUTO request skips WASAPI, asks ASIO for a driver whose one process-wide slot
  the array already holds, and falls to the SILENT null sink. Under `BWA_SINK_ASIO` it fails the
  start outright. That is the defect WASAPI was added to fix, re-created one argument at a time, and
  it is invisible offline: with no device string there is nothing to inherit, so every null-sink test
  passes. The monitor opens AUTO with device NULL and flags 0. The exception reads the REQUESTED
  sink, not the resolved one: an explicit null or manual keeps the monitor device-free, while an
  AUTO array whose device is missing still gets a live monitor - reading the resolved sink there
  would give silence on the one configuration where hearing the sim is how you learn the array never
  opened. `test_cave_both` pins it against whatever stereo device AUTO finds.
- **A device-name copy TRUNCATES, it does not fail.** `bwa_get_device_name` promises "always
  NUL-terminated, truncated to cap-1", and `WideCharToMultiByte` straight into a short buffer does
  the opposite: it returns 0 with `ERROR_INSUFFICIENT_BUFFER` and writes NOTHING, so a picker that
  sized its buffer for the common case got an empty name and no error it could act on. The cut also
  has to land on a UTF-8 character boundary, because half a character is an invalid string rather
  than a shorter name - and it is a string the caller must be able to hand back as
  `bwa_desc.device`. `sink_copy_device_name` in `sink.h` is the one implementation; every backend
  uses it.
- **A device API that does not promise a fixed callback size must go through `sink_quant`.** ASIO
  is the only backend whose buffer size is fixed once buffers exist. WASAPI shared mode hands out
  `bufferFrameCount - GetCurrentPadding()`, which MOVES, and with the SDK the headphone decode is
  built for one frame size and SILENCES any other (`steam_decode.c`). A backend that passes the
  device's own count to render() therefore produces silence on some machines and not others,
  which is the worst shape a bug can take. Two rules travel with the adapter: the FIFO is a ring
  of block-sized SLOTS rather than of frames, because render()'s planar channel stride IS nframes
  and only a slot has that stride (a frame-indexed ring would need a copy per block); and two
  blocks rendered inside ONE device callback must not share `system_time_ns`, or the
  device-versus-host drift fit gets a zero-slope pair, which is worse than no pair. The corollary
  bit twice over: extrapolating those stamps FORWARD lets the NEXT callback land behind them when
  a device catches up after a fault, and a backward stamp is worse than a flat one, so the adapter
  also floors each stamp at the previous one plus a nominal block.
- **A device API's "obvious" fault signal may be structurally unreachable.** WASAPI shared mode
  looks like it reports underruns through `GetCurrentPadding() == 0` at a wake. It never does: the
  client refills the buffer to FULL every event, and a starve leaves the stream event already
  signalled, so the catch-up wake returns immediately holding the frames just written. A live
  starve produced zero such wakes. What works is the RELEASE INTERVAL exceeding the buffer depth,
  which is a measurement rather than an inference. Two lessons past the one fact: `measured` must
  follow whether the chosen rule has any HEADROOM (a one-period buffer leaves none, so it reports
  false rather than a zero it could not earn), and a fault detector is not believable until a real
  fault has been injected against it.
- **A DEVICE POSITION is not a wall clock, so a stall sized against the buffer can miss the
  dropout it injects.** The exclusive-mode rule is the queued depth: `written` against what
  IAudioClock reports the device consumed. But GetPosition counts frames READ FROM OUR BUFFER, and
  a starved device is not reading any, so during a stall the position advances by only about HALF
  the wall time. On a Realtek endpoint an exclusive buffer is one 144-frame period (3 ms), and the
  "two buffers plus slack" stall that is exactly right for the shared-mode release-interval rule
  came to 36 ms and tripped the depth rule at ONE of the three block sizes tried. 150 ms trips it
  at all three, every run. The shape to remember: when a fault detector reads a DEVICE-reported
  quantity, size the injection against that quantity's own behavior under fault, not against the
  buffer arithmetic that looks like it should govern. And a stall that fires sometimes is worse
  than one that never fires, because it reads as flakiness rather than as a finding.
- **A self-checking test that CANNOT FAIL is the default outcome, not a rare mistake.** It happened
  three times in the convenience-tier work alone. `examples/convenience.c` counted its failures and
  then returned 0 regardless, so it could only ever have caught a crash. `demo/api.gd` asserted
  `get_preset(KIND_UI)["atten_rolloff"] == 0.0`, which `BWA_SRC_DEFAULT` also satisfies (the
  sanitizer zeroes the whole attenuation triple when `atten_ref_dist <= 0`), so it would have passed
  against a completely unimplemented `BWA_SRC_UI`. And both async tests "covered" the held-play
  window while a fast decode meant the held path never ran. The rule that catches all three: BREAK
  THE THING ON PURPOSE and confirm the test goes red, before believing it green. Where a race is what
  hides the coverage, prefer ORDERING over a test hook: an async decode is adopted only at a pump
  point (acquire / is_ready / find / release / commit), and `bwa_source_play` is not one, so a play
  issued straight after `bwa_sound_acquire_async` is deterministically HELD. A `bwa_set_loader_stall`
  diagnostic was written for this and then deleted, because ordering buys the same guarantee without
  a public call that exists only for tests. Two corollaries: an assertion whose two sides both land
  near zero when the mechanism breaks is a COIN FLIP, so demand a margin; and `clock()` does not
  advance while a thread sleeps, so a test timing an async wait needs a wall clock (`GetTickCount64`)
  or it measures nothing. Where neither ordering nor a margin can settle it, SAY SO in the test
  rather than let it imply coverage it does not have.
- **A restored file can keep an OLD mtime, so the build skips it and you test a stale binary.** `mv`
  and `cp -p` preserve timestamps, so reverting an A/B edit from a backup can leave the object file
  newer than the source. Half an hour went into "the hook is broken" that was really an unrebuilt
  DLL. `touch` the file after any restore, and when a result contradicts the code you are reading,
  suspect the binary before the logic.
- **A build tree carries install rules it does not own, so an SDK install must name a
  COMPONENT.** `cmake --install <build> --prefix <sdk>` with no `--component` writes raylib's
  headers and import library, and the Python binding's wheel payload, into the same prefix as the
  engine SDK - and the mirror image is worse: scikit-build-core installs EVERY component by
  default, so a wheel built from a tree that also has the SDK rules grows a second copy of the
  engine at `bin/` plus the header and the CMake package files. The two halves are `bwa_sdk` and
  `bwa_python`, and `pyproject.toml` names the second in `install.components`. Both were seen
  before the split, in the same install listing.
- **An import library and the runtime file are siblings in a BUILD TREE and not in an INSTALL
  TREE.** `mkoctfile_mex.cmake` derived the DLL to stage from `$<TARGET_LINKER_FILE>`'s directory,
  which is right for `build/RelWithDebInfo/` and wrong for an SDK, where the .lib is under `lib/`
  and the .dll under `bin/`. It copies nothing and fails only later, on a machine with no engine on
  PATH. The runtime path is passed explicitly now (`BWA_ENGINE_DLL`).
- **`test/xval_data.h` is GENERATED** by `tools/xval/gen_reference.py` — don't hand-edit.
  Regenerating needs numpy + scipy; ctest stays hermetic on the committed header.
- **`-DBWA_ASAN=ON`** builds `test_sound` under AddressSanitizer — the control-side
  use-after-free check for the sound retire handshake.
- **A unit belongs in a NAME only when the quantity has two live units.** Time is the only one:
  frames (dsp clock, `play_at`/`stop_at`/`seek`, playheads, output latency) and seconds (fades,
  RT60, IR length, pose lead), so every time-valued name says which — `_frames` on a getter,
  `seconds`/`_s` on a parameter. Distances (meters), frequencies (Hz), angles (radians) and gains
  (linear) have no competitor, so they carry the unit on the VALUE (`radius_m`, `xover_hz`,
  `yaw_rad`) and never on the call. The one hard rule for new calls: a decibel value must say
  `_db`, because linear is the unmarked default everywhere. Full statement in docs/api.md →
  "Coordinates and units".
- **A binding call must not borrow a host-engine name with a different unit or meaning.**
  Godot's `AudioServer.get_output_latency()` is SECONDS and `AudioStreamPlayer3D.seek()` takes
  SECONDS; the binding's frame-valued twins are `get_output_latency_frames/_seconds` and
  `seek_frames/_seconds` for that reason. The 0.4.0 zip shipped the colliding spelling and it hid
  behind the null sink, which returns 0 — and 0 is 0 in any unit.
- **Runtime-printed strings are ASCII.** An en-dash in a `push_warning` becomes mojibake on a
  Windows console codepage. Comments, docs and inspector hint strings keep their punctuation;
  anything that can reach a console does not. The rule is REPO-WIDE, not a Godot rule: every
  `set_error` string reaches `bwa_last_error`, which every binding and every example prints. It was
  only ever *checked* on `bindings/godot/src`, and `src/` had accumulated 14 em-dashed `set_error`
  and `set_err` messages behind that gap (engine.c, layout.c, zylia.c) before anyone looked.
  `rg -P '"[^"]*[^\x00-\x7F][^"]*"' src bindings/godot/src` should stay empty (the one hit it can
  legitimately return is a quoted phrase inside a `//` comment, `asio_sink.cpp`).
- **wasm-ld's default shadow stack (64 KB) is too small for the engine, and the failure does not
  say so.** The shadow-stack pointer wraps past zero and the next write lands near 4 GB, so wasmtime
  reports `memory fault at wasm address 0xffff46fc` with a backtrace naming whatever libc function
  was running. It presented first as `FAIL: rt_create`, then as `test_golden` spinning for minutes
  on 0.25 s of audio. Bisected: 64 KB and 128 KB fault, 256 KB and up pass; the toolchain file asks
  for 1 MB (`BWA_WASM_STACK_SIZE`), and that one flag took the suite from 14/33 to 33/33. The
  other three load-bearing flags (`--import-memory --export-memory`, `--max-memory`,
  `-lwasi-emulated-process-clocks`) each have a comment in `tools/wasm/wasi-sdk.toolchain.cmake`.
  Also: `os` under wasmtime is the same sleep-lateness flake the Android emulator shows (32/33 then
  33/33 on back-to-back runs); it passes in isolation.
- **A `Layout` is never a stack local.** Raising the capacity to 64 grew it from 72 KB to 176 KB,
  almost all of it the 512-tap FIR each `Speaker` embeds. Four tests (`rt_feature`, `dsp`, `xval`,
  `valid`) then overflowed the 1 MB main-thread stack with exit `0xC00000FD` BEFORE PRINTING
  ANYTHING, so the log shows a bare SegFault and no failing check. The fix is residency, not a
  bigger stack: a binding can call in from a 512 KB thread (a macOS secondary thread), and wasm's
  shadow stack is 1 MB total. So a `Layout` lives in a heap struct (`RtCore`, `bwa_engine`), a
  calloc (the pure `bwa_*_batch` helpers), or a static in non-reentrant code (tests, tool mains).
  `layout_default(Layout*)` fills in place because a by-value return is a hidden stack temporary;
  do not bring the by-value form back. The note above the struct in `layout.h` says the same.
- **An AudioWorklet has NO monotonic clock, and an unchecked `clock_gettime` reads stack garbage.**
  An AudioWorkletGlobalScope has no `performance` object, so Emscripten's `clock_time_get` returns
  ENOSYS there (its `nowIsMonotonic` is `!!globalThis.performance?.now`), and `os_monotonic_ns`
  ignored the return value and read the never-written `timespec`: the constant 1069547520 every
  quantum. Every render time on the worklet read 0 except the first (1.07 s), so `peak_load` sat at
  40108% on the headset, `late_blocks` was dead on that sink, and the "330 to 480% start-up peak"
  docs/web.md once claimed was this bug. `os_monotonic_ns` now checks the return and falls back to
  `emscripten_get_now` (Date.now in that scope: 1 ms, wall time, can step BACKWARD, which is why
  `sink_quant` books a backward step as a zero-length render). Two rules: never present a web render
  time as a percentage of a 2.7 ms quantum from a 1 ms clock, and a health number needs a test that
  proves the clock behind it ticks (`test_sink_quant`'s injectable clock).
- **`git apply` INSIDE a repository is a silent no-op for paths outside the current directory.**
  It resolves the patch's paths against the repository root, and "patched paths outside the
  directory are ignored", so `git -C core/deps/flatbuffers apply` (a directory inside the
  steam-audio submodule's repo) matched nothing, exited 0 for `--check`, for `--reverse --check` AND
  for the apply, printed "applied", and left the header unpatched; phonon then died on the exact
  line the patch removes (Pages run 35716986768). It passed locally because the scratch copy had no
  `.git`, where git apply behaves like patch(1). `build-phonon.sh` now runs from the submodule root
  with `--directory=core/deps/flatbuffers` and GREPS for the patched line afterward. The fix then
  failed once more (run 35717926513) because the script had already `cd`'d into `core/build` and the
  submodule path was RELATIVE, so `git -C` died on "cannot change to" behind a `2>/dev/null` and read
  as "neither applies": a local proof run from the repo root could not see it. Absolute paths after
  the cd, and the failure path now prints git's own reason. General form: a check whose positive and
  negative BOTH pass is not a check, so after any patch step assert on the file's content, not on the
  tool's exit code, and never hide the stderr of the command whose exit code you branch on.
- **A global stage that scales "the bus" is not global once a profile bypasses the bus.**
  `bwa_set_master_gain` ramped the 26-channel speaker bus pre-align, and under
  `BWA_PROFILE_BINAURAL` point voices never touch that bus (they live in `ambi_direct` and, in mode
  2, the `dv_mono` point taps), so the knob was INERT on headphones and worked on the array sim. It
  shipped through every test because the tests checked it on the bus. Found from the web playground's
  slider, 2026-09-22; measured through the manual sink at 0.10 / 0.10 / 1.00 across cave, cave_sim
  and binaural before the fix. Rule: anything described as "over the whole mix" (master gain, global
  pause, a scene fade) must be applied to EVERY buffer a decoder reads, and its test must run in
  every profile, because the direct render is a second output path, not a second consumer of the
  first. `bindings/web/tests/master_gain.test.mjs` and the two `rt_feature` checks pin it.
- **A shipped artifact must not cite a doc it does not ship.** Both packs run
  `tools/dist/doc-pointers.ps1`, which rewrites repo-doc references in the staged tree to
  permalinks at the packed commit and then fails the pack on any relative `.md` reference the
  stage cannot satisfy. The 0.4.0 Godot zip shipped three dangling ones.

## What NOT to do

- Do not introduce FMOD/Wwise or route audio through the engine's mixer.
- Do not use Unity's built-in audio (8-channel cap) or the device's WDM/DirectSound
  driver (a consumer path: its own mixing, resampling, and no timing hooks).
  The array's channel count (24 on the CAVE) requires ASIO. This is settled.
- Do not pan via pure ambisonics for localized point sources — the listener moves
  across ~3×3 m and a single sweet spot fails. DBAP is recomputed per frame from
  tracked position. See `docs/spatialization.md`.
- Do not assume Steam Audio's Unity/FMOD *integration* limits apply to its C API.
  The C API supports custom speaker layouts; the integrations do not expose them.
- Do not run the Steam reflection bed AND the ISM early reflections together — the
  bed already contains early reflections, so they render twice (engine.c warns once).
- Do not model the CAVE room itself with the ISM. Its shoebox is the *virtual*
  environment; the physical room supplies its own reflections, and modeling it
  double-counts (same trap as matching the measured RT60 — docs/calibration.md).
- Do not let any `bwa_*` per-frame call block or allocate.

## Which acoustics path (the recommendation)

Three implementations now overlap here; they are complementary, not rivals (the full
comparison + rationale is `docs/materials.md` → "Choosing an acoustics path"):

- **Steam scene** (`steam_scene.c`) for occlusion + directivity + pathing — ray tracing
  earns its keep; the manual path needs the game to already know the answer.
- **ISM** (`ism.c`) for early reflections — the Steam bed is listener-CENTRIC (one field
  around one point, 30 Hz) so its reflections have no parallax; the ISM pans each bounce
  as a point source through the listener-relative panner, per block. That is the engine's
  own thesis applied to reflections. Cost: O(N) in sources vs the bed's O(1) — opt in on
  the few that matter.
- **FDN** (`fdn.c`) for the late tail — deterministic, infinite, designable.

That configuration never creates the Steam reflection bed. A **no-SDK build is fully
viable** (ISM + FDN + manual occlusion); what it loses is automatic occlusion, pathing,
and the real HRTF monitor — the last being a *developer-workstation* dependency, since
the production array render never uses HRTF.

## Docs style

**US English spelling, everywhere:** `docs/*.md`, headers, and code comments alike.
`center`/`color`/`behavior`/`optimize`/`meter`/`license`, not `centre`/`colour`/
`behaviour`/`optimise`/`metre`/`licence`. Catches the usual `-ise`/`-isation` family
(`normalize`, not `normalise`) too.

**Prose voice, `docs/*.md` and `README.md` only:** short, blunt prose (the
floooh/sokol voice: direct statements, second person, no throat-clearing), following
the Google developer documentation style guide beyond that. No em dashes, `e.g.`,
`i.e.`, or `&`; spell out `for example`, `that is`, `and` (a hyphen or a rewritten
sentence replaces the em dash). `vs` stays fine in an A/B-style label (`DBAP vs
SPCAP`), just not in flowing prose. Code comments keep their own punctuation and are
exempt from the voice rule.

CLAUDE.md itself is not user-facing (see "Repo layout" note on `docs/api.md` owning
the manual) and is exempt from the voice rule too; it predates it and mixes styles
freely. It still follows the US English rule above.

## Doc index

- `docs/architecture.md` — system overview, the bus seam, the full signal-flow diagram
  (every signal kind, source → device, with tap-order rationale), locked decisions + rationale.
  `docs/signal-flow.md` is the same diagram as a rendered Mermaid graph (ASCII is canonical).
- `docs/concurrency.md` — threading model, SPSC rings, commit snapshot, lifetimes. **Most load-bearing.**
- `docs/api.md` — C ABI reference and per-call threading semantics.
- `docs/spatialization.md` — DBAP, moving observer, binaural decode (3rd-order), speaker alignment.
- `docs/materials.md` — material/geometry model → Steam Audio occlusion + reflections → the bus.
- `docs/integration.md` — Unity + Godot bindings + the per-engine coordinate seams; Unreal notes.
- `docs/build.md` — platform, dependencies, licensing, Dante config.
- `docs/web.md` — the browser target: the settled two-thread decision, the measured toolchain
  verdicts (wasi-sdk, Emscripten, zig), the OS shim under wasm, the AudioWorklet sink and the JS
  binding as built, the "main" topology deviation and its ways out, phonon under wasm and its
  exception-model residue, GitHub Pages hosting through the service worker, the playground and XR
  pages, and the list of what a browser has never run. Live at aforren1.github.io/cave-audio.
- `docs/backends.md` — the sink contract + the backends beside ASIO. WASAPI (Windows), JACK + ALSA
  (Linux) are IMPLEMENTED; CoreAudio and AAudio are still spec.
  The 10-rule sink contract (fixed quantum, timestamp pair, health, exact-rate policy), the fixed-quantum
  adapter, the OS shim inventory, the ABI bump (enum, `bwa_desc.device`, device query), AUTO order, phases.
  Linux reaches the array via AES67 into the Dante net (or a multichannel card); JACK is the Linux production
  backend (runs on PipeWire via pipewire-jack), ALSA the no-server path. Goals per platform up top: Windows +
  Android = dev ease + reach for head-mounted VR games; Mac = dev ease; Linux = future rig alternative + seated
  headphone experiments (Psychtoolbox/PsychoPy, live or offline via the manual sink).
- `docs/profiling.md` — Tracy instrumentation (`BWA_TRACY`), the headless benches (`profile_bench`/`bench_situations`), real-time scheduling + memory notes.
- `docs/layout-schema.md` — `cave_layout.json` format: speaker geometry, per-speaker gain/delay, DBAP knobs.
- `docs/calibration.md` — `bwa_calibrate`: acoustic position survey, delay/gain trims, room report → `cave_layout.json`.
- `docs/validation.md` — `bwa_validate`: render a phantom, measure where it landed. The Zylia
  intensity/integrity/SRP/comb-depth estimators, the solve-position-versus-mic-position seam, the
  SPCAP focus sweep (and where it has power), simulate versus hardware, and what the measurements
  have said so far.
- `docs/hardware-validation.md` — the rig-day runbook: staged on-hardware checks (device → wiring → calibration → Motive → end-to-end → by-ear) with pass criteria.
- `docs/internal-types.md` — internal structs (`Voice`/`Sound`/`Layout`/`Listener`) + helper signatures. **Not ABI.**
- `docs/glossary.md` — the domain vocabulary across all of the above (panners + knobs, rE/rV metrics,
  ambisonic decoders, spread/decorrelation, acoustics paths, binaural, calibration + validation
  coinages), each entry short, formula-cited to `file:line`, and linked to the doc that owns it.
  Threading vocabulary is deliberately excluded (concurrency.md owns it).
