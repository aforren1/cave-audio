# Web (WebAssembly)

Status: **draft, and a spike stands behind it**. The engine core builds for wasm32 and the whole
offline suite passes there. Steam Audio builds for wasm too, the HRTF decode test passes against
it, and `tools/phonon/build-phonon.sh` builds and stages that phonon from one command. Nothing
about the browser side is built: there is no AudioWorklet sink, no JS binding, and no web CI job.
This note says what the spike measured, what was decided on top of it, and what a real web target
would still have to do.

Read [backends.md](./backends.md) for the sink contract first and
[concurrency.md](./concurrency.md) for the audio-thread rules, because the interesting question on
the web is which of those rules a browser lets you keep.

## The decision

Taken 2026-09-21. The rest of this note is written against it, not around it.

- **The browser design is the faithful two-thread model over `SharedArrayBuffer`, with the control
  thread on a Worker.** Not the page's main thread, and not the single-threaded shape that runs
  control calls on the audio thread. Every invariant in [concurrency.md](./concurrency.md) then
  holds unchanged, which is the point: the web gets this engine, not a second one that resembles
  it.
- **Cross-origin isolation is an accepted hosting requirement.** COOP plus COEP, or no
  `SharedArrayBuffer`. There is no fallback build, and a page that is not isolated must fail loudly
  instead of degrading.
- **Emscripten is the shipping toolchain for the browser. wasi-sdk is the CI and offline leg.**
  Both stay. Emscripten is the only one that reaches Web Audio, and wasi-sdk is the one that runs
  the suite the desktop jobs run.

What follows records what was measured, and what each of those three costs.

## What runs today

Two toolchains build the engine. Both give you a static `libbw_audio.a` on the null and manual
sinks, and nothing else: no device backend, no shared library.

| leg | result |
|-----|--------|
| wasi-sdk 25, `wasm32-wasip1-threads` | builds the library and all 35 test and example binaries. **33 of 33 ctests pass** under wasmtime 27. |
| Emscripten 6.0.10, `-pthread` | builds the same tree. **31 of 33 pass** under node: `os` times out and `idle` fails. Both are the main thread blocking, see below. Needs `-sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=1048576`, which `tools/wasm/build-wasm.sh` passes; without them the same tree gives 13 of 33. |
| Emscripten 6.0.10, `-pthread`, phonon staged | **35 of 38 pass** through `tools/wasm/build-wasm.sh` verbatim in the emsdk container, 2026-09-22. All five SDK-gated tests pass, the four simulator ones included (`reflect`, `bake`, `path`, `dynmesh`, `steam_decode`). The three failures are the same `os` and `idle`, plus `fuzz_api`: see [Steam Audio under wasm](#steam-audio-phonon-under-wasm). |
| Emscripten 6.0.10, no `-pthread` | builds and links. **13 of 33 pass**: `rt_create` fails, because `stream_set_create` cannot start its refill thread. |
| zig cc 0.13 | compiles the DSP. No threads at all: see [Toolchains](#toolchains). |

Every row is a **no-SDK** build. A with-SDK wasm build is further along than the rest of this note
once implied and is not green yet, for a reason that has nothing to do with phonon: see
[Steam Audio (phonon) under wasm](#steam-audio-phonon-under-wasm).

The wasi run is the useful number, because it is the same suite the Linux and macOS jobs run at
default options, driven by the same `ctest`. It includes the threaded tests: `idle` (the loader and
stream wakeup counters), `cave_both` (two sinks), `golden` (a deterministic manual-sink render), and
`natnet` on the socket stubs. Build it with:

```
tools/wasm/build-wasm.sh          # BWA_WASM_TOOLCHAIN=wasi, WASI_SDK_PATH=<root>
```

Set `BWA_WASM_RUNTIME` to a `wasmtime` binary and the script runs the suite too. CMake does the
plumbing through `CMAKE_CROSSCOMPILING_EMULATOR`, so no test knows it is running under a wasm
runtime.

## Toolchains

The spike went looking for a way to avoid Emscripten, because Emscripten has been a fragile target
before. The short answer: **wasi-sdk carries the engine core and cannot carry the browser**, and
Emscripten carries both. That is why the decision keeps both rather than picking one: wasi-sdk is
the CI and offline leg, Emscripten is what ships a page.

### wasi-sdk (clang, wasi-libc)

Verdict: **the CI and offline leg.** It is a plain clang cross-compiler, the sysroot is
ordinary POSIX, and there is no framework between you and the module. `wasm32-wasip1-threads` has
real pthreads: a probe confirmed `pthread_create`, `pthread_join`, C11 atomics across threads,
`pthread_rwlock`, `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and
`pthread_condattr_setclock(CLOCK_MONOTONIC)` all work. Those five are exactly what
`src/os/os_posix.c` is built on, which is why the shim needed almost nothing (see
[The OS shim](#the-os-shim-under-wasm)).

What it cannot do is reach a browser. WASI is a system interface for a runtime like wasmtime, not
for a page. A WASI module in a browser needs a JS shim that implements `wasi_snapshot_preview1`
itself, and with threads it needs one that implements `wasi` `thread-spawn` on top of
`Worker` plus a shared `WebAssembly.Memory` as well. That shim is writable, and several exist, but
it is code you own, and it buys you nothing on the audio side: there is still no Web Audio binding
in it, so you write the AudioWorklet glue by hand either way.

Three flags have to be right or the module builds and then fails at run time in ways that read as
engine bugs. `tools/wasm/wasi-sdk.toolchain.cmake` sets all three and says why:

- `-Wl,--import-memory --export-memory`. wasi-threads requires an imported shared memory, and
  wasmtime's own WASI adapter requires the memory export. Miss the import and every
  `pthread_create` fails; miss the export and the first `printf` traps with `missing required
  memory export`.
- `-Wl,--max-memory=<N>`. A shared memory with no ceiling cannot grow, so the thread-stack
  allocation inside `pthread_create` fails with `ENOMEM` and wasi-libc reports `EAGAIN`. Measured:
  without the flag the threads probe returns `pthread_create=6`, with it `0`.
- `-Wl,-z,stack-size=<N>`. This one is the trap. wasm-ld defaults to 64 KB of shadow stack and the
  engine needs more than 128 KB. Below that the stack pointer wraps past zero, so what you get is
  not a stack-overflow message but `memory fault at wasm address 0xffff46fc in linear memory of
  size 0x170000`, with a backtrace naming whichever libc function happened to be running.
  Bisected against `rt_create`: 64 KB and 128 KB fault, 256 KB and up succeed. The toolchain file
  asks for 1 MB.

Two smaller ones: `-lwasi-emulated-process-clocks`, because wasi-libc has no `clock()` and two
tests use it, and `-ldl`, because wasi-libc's `dlopen` stub lives in its own archive.

### Emscripten

Verdict: **the shipping toolchain for the browser.** It is the only toolchain that already knows
about Web Audio, Workers and the module loader, and `-sAUDIO_WORKLET=1` with `-sWASM_WORKERS=1` is
the shortest path to an AudioWorklet that runs wasm.

With `-pthread` the tree builds clean and 31 of the 33 ctests pass under node. The two that do not
are the same finding twice, and it is the finding that shapes the browser design:

- `os` times out in its sleep section. `os_sleep_ms` on the main thread blocks the event loop.
- `idle` fails on `the async load completes (a lost wake would never finish)`. The asset loader's
  `os_event_wait` does not deliver inside half a second when the control side is the main thread.

Both say the same thing: **do not put the control side on the main thread.** Node tolerates
`Atomics.wait` there and a browser forbids it outright, so what is a slow test here is a hard error
in a page. Put the control thread on a Worker and both go away.

Without `-pthread` the tree links, after the shim change below, and then fails at run time: only 13
of 33 pass, and the rest die at `rt_create`, because `stream_set_create` calls `os_thread_create`
for its refill thread and there is no `pthread_create`. That is the honest measure of the
single-threaded shape: it is a different engine, not a build flag. See
[Threading model](#threading-model).

One shim change was needed, and the failure is worth recording because it is not the WASI one.
Emscripten **declares** `sched_get_priority_min`, `sched_get_priority_max` and
`pthread_setschedparam` in its headers and **defines** them only in the pthreads build, so a
single-threaded build fails at link:

```
wasm-ld: error: libbw_audio.a(os_posix.c.o): undefined symbol: sched_get_priority_min
```

`BWA_OS_NO_SCHED` in `src/os/os_posix.c` covers that case and the WASI one together.

Emscripten's own spellings of the three flags above are `-sSTACK_SIZE`, `-sALLOW_MEMORY_GROWTH`
and, for worker threads, `-sDEFAULT_PTHREAD_STACK_SIZE`. The stack default is the same 64 KB, so
the same wall is there; Emscripten at least calls it a stack overflow.

### zig cc

Verdict: **no.** Zig is an attractive single-binary clang frontend with a bundled wasi-libc, and it
compiles the pure DSP happily (`src/spatial/dbap.c` and `src/core/rt.c` both build for
`wasm32-wasi`, C11 atomics included). It has no threads. Zig 0.13 does not know the target at all:

```
error: unable to parse target query 'wasm32-wasi-threads': UnknownApplicationBinaryInterface
```

and `zig targets` lists only `wasm32-wasi-musl`. Compiling for plain `wasm32-wasi` with `-pthread`
gets through the headers and dies at link on `pthread_create`, `pthread_join`,
`pthread_condattr_init`, `pthread_condattr_setclock` and `pthread_rwlock_init`. Zig's bundled
wasi-libc is the no-threads build. Revisit when that changes; nothing else about zig is wrong here.

## The OS shim under wasm

`src/os/os_posix.c` takes wasm the way it takes Android: unchanged, with the parts the platform
refuses degraded and reported. Three areas, and only three.

**Thread scheduling.** Gone, behind `BWA_OS_NO_SCHED`. `os_thread_set_realtime` returns `ENOTSUP`,
`os_thread_realtime_available` returns false so a sink reports the degradation through the open's
`err`, and `os_thread_lower_priority` is a no-op. This costs nothing you had: a browser gives a
Worker no priority knob, and an AudioWorklet thread is already the highest-priority audio context
the page can get.

**Sockets.** Gone on WASI. wasi-libc has no `socket()`, and a browser has no UDP at all, so
`natnet.c` gets the same answer it gets on a machine with no tracker: the open fails and the
listener stays on whatever pose the control thread sets. `os_ipv4_valid` keeps working, because it
is string parsing and a config check must not depend on whether the platform could then connect.
The `natnet` ctest passes under wasmtime on the stubs. If a web build ever needs live tracking, it
arrives over WebSocket or WebTransport and that is host-side plumbing feeding
`bwa_set_listener_pose`, not a change to this shim.

**dlopen.** wasi-libc's stub returns NULL, which is already the "library not present" path the two
Linux backends handle. Nothing to do.

Everything else is intact: threads, the monotonic clock, `os_sleep_until_ns` on
`clock_nanosleep(TIMER_ABSTIME)`, the auto-reset event on a `CLOCK_MONOTONIC` condvar, mutexes,
rwlocks, and the whole UTF-8 file family.

## Threading model

Three shapes were on the table. They are not equivalent: the choice decides which invariants in
[concurrency.md](./concurrency.md) you still have. **The first is the decision**; the other two are
kept here because each is tempting and each costs something specific.

### Chosen: faithful two-thread, over SharedArrayBuffer

The control thread is a Worker; the audio thread is the AudioWorklet's own thread. Both run the
same wasm module against one shared `WebAssembly.Memory`, so the two SPSC rings, the voice table
and the commit snapshot are the same memory they are on a desktop, and every rule in
concurrency.md holds unchanged.

It needs `-pthread`, it needs `SharedArrayBuffer`, and `SharedArrayBuffer` needs cross-origin
isolation (see [Hosting](#hosting)). Those costs were accepted in exchange for one engine instead
of two. An AudioWorklet processor is a separate realm, so the module has to be instantiated there
against the shared memory rather than loaded a second time.

The one place the desktop model does not transfer is `Atomics.wait`, which is **forbidden on the
main thread**. Nothing in the engine's audio path waits: the rings are lock-free and the audio
thread never blocks. What does wait is `os_event_wait`, and its callers are the asset loader and the
stream refill, both already on their own threads. The rule is therefore free as long as the CONTROL
side is also on a Worker.

The Emscripten run measured what happens when it is not: `os` times out and `idle` loses the
loader's wake, both on the main thread, under node, which is the permissive case. A browser would
refuse outright. That measurement is why "control thread lives on a Worker" is part of the
decision and not a recommendation inside it.

### Considered and rejected: control on the audio thread (the kwaa shape)

One module in the AudioWorklet, no shared memory, no `-pthread`. Control calls arrive by
`postMessage`, are drained at the top of each render quantum, and run on the audio thread.

This works and it breaks invariant 1 by construction: `bwa_load_wav` allocates and does file I/O,
and now it does so on the audio thread. It is viable for a page that loads every asset before it
starts and then only moves sources, which is most of what a web demo does. It is not viable for
anything that streams or loads on the fly.

**The engine as it stands cannot take this shape.** The spike measured it: a no-`-pthread`
Emscripten build links, and then `rt_create` returns NULL on every call, because
`stream_set_create` needs a refill thread. Before a single-threaded web build is possible at all,
the streaming set has to become optional, and the asset loader with it. That is a core change, not
a sink, so cost it as one.

If you do take this shape, take it deliberately: the command ring stops being a ring you need, the
retire-ack handshake stops meaning anything, and the engine is running a model it was not designed
for. Say so at the seam rather than letting it look like the desktop build.

### Considered and rejected: control on the main thread, ring across SharedArrayBuffer

A middle option worth naming because it is tempting and does not actually help. You keep the two
threads and the rings, but put the control side on the page's main thread. Costs: the main thread
is where layout and garbage collection live, so a command burst can be delayed by a frame or more,
and `Atomics.wait` is unavailable there, so the asset loader cannot park. Prefer a Worker for the
control side and keep the main thread for input only.

## The SPSC rings over SharedArrayBuffer

There is nothing to port. The rings in `rt.c` are plain structs in linear memory with C11 atomic
head and tail indices. Compile with `-pthread` and a shared `WebAssembly.Memory`, and the atomics
lower to wasm's own `i32.atomic.*` instructions against that memory. Two threads sharing one
`WebAssembly.Memory` see one linear memory; the ring does not know or care that the other end is an
AudioWorklet.

Two things to watch:

- **The memory must be shared at creation**, `new WebAssembly.Memory({initial, maximum, shared:
  true})`, and `maximum` is mandatory for a shared memory. That is the same fact as the
  `--max-memory` flag above, seen from the JS side.
- **Non-atomic writes still need the release-acquire pairing the rings already use.** This is not
  new, but it is newly load-bearing: on x86 a missing fence often survives testing, and wasm's
  memory model is not x86's. The publish-then-flag trap in CLAUDE.md is the one to reread.

## The AudioWorklet sink

An AudioWorklet's `process()` gets **128 frames**, always, and that is not negotiable. The engine's
block is 256 by default. You could set `bwa_desc.block_size` to 128 and skip the adapter entirely,
and that is worth measuring, but do not design around it: 128 frames is 2.7 ms at 48 kHz, so every
per-block cost in the mixer is paid twice as often, and with the SDK the HRTF decode is built for
one frame size and silences any other.

`src/sink/sink_quant.c` is exactly the seam for this, and it is the seam by design: it renders whole
engine blocks into a ring of block-sized slots and serves the device any count. WASAPI shared mode,
AAudio and CoreAudio all go through it for the same reason. An AudioWorklet sink is the easiest
customer it will ever have, because 128 divides every sensible block size, so one rendered block
feeds exactly `block_size / 128` callbacks with no partial slot.

So the sink is small: open, allocate the adapter, and in `process()` pull 128 frames per channel out
of `sink_quant` into the output bus. It owns the timestamp pair like every other backend, and the
two rules that travel with the adapter apply unchanged: one `system_time_ns` per rendered block even
when several blocks land in one callback, and each stamp floored at the previous one plus a nominal
block.

What it reports for health is worth deciding up front rather than guessing. An AudioWorklet has no
device position, so `device_pos_valid` is false, the same answer AAudio gives. The honest dropout
signal is the render deadline: `currentTime` advancing by more than one quantum between callbacks.
`AudioContext.baseLatency` and `outputLatency` are the output-latency inputs.

Channel count: a page can ask for a wide `AudioContext`, but the array profiles have no transport
here. Web is a **stereo** target. Treat anything wider the way Android's AAudio sink does: AUTO
sends it to the offline sink and an explicit open says why.

## Which invariants survive

From CLAUDE.md's list:

1. **No allocation, locks, syscalls or file I/O on the audio thread.** Survives, and half of it is
   free: an AudioWorklet has no file system to do I/O against. What you must not do is take the
   control-on-audio-thread shape and then pretend otherwise.
2. **One control thread.** Survives, and needs a guard. JS makes it easy to call from the main
   thread and a Worker in the same page. The Python binding already carries a runtime guard for
   exactly this reason; a JS binding should carry the same one.
3. **Audio thread owns DSP state.** Survives with shared memory. Meaningless without it.
4. **Gains ramp, never jump.** Untouched. Pure DSP.
5. **Generation counts gate handle reuse.** Untouched.
6. **`CMD_COMMIT` defines frame coherence.** Untouched, and it matters more here: a page's control
   loop is `requestAnimationFrame`, whose cadence is nothing like the audio block's.

### Where wav decode runs

On the control side, as always, but the file does not come from a file. There is no synchronous
file system in a browser, so `bwa_load_wav`'s path argument has nothing to open.

Two answers, and the second is better:

- Preload into the wasm heap from JS (`fetch`, then copy the bytes in) and decode from memory.
  This needs a memory-buffer entry point beside the path-based one, which the engine does not have
  today.
- Decode in JS with `AudioContext.decodeAudioData`, which is the browser's own optimized decoder,
  and push the float samples in through the existing push-source feed. No new ABI, and it handles
  the formats a page actually gets served.

Disk streaming (`src/core/stream.c`) has no direct equivalent. The nearest shape is a JS-side
fetch that feeds a push source, which moves the refill cadence out of the engine.

## A JS/TS binding

Mirror `bindings/python`'s two layers, for the same reason it has two:

- a **raw layer** that is the C ABI one for one, C names minus the `bwa_` prefix, C field names, C
  units. It is what you debug against and what a generated binding can stay honest about.
- a **thin idiomatic layer** over it: an `Engine` that owns the context and the worklet, `Source`,
  `PushSource`, `Bed`, `Listener`, errors thrown from `bwa_result`.

Two rules the other bindings learned the hard way apply here as written. Do not borrow a host name
with a different unit: Web Audio speaks seconds everywhere, and the engine's frame-valued calls must
keep saying `Frames` (see the Godot `get_output_latency` note in CLAUDE.md). And exclude
`bwa_set_output_capture`: its callback runs on the audio thread, and a JS callback must never run
there. The manual sink plus `bwa_render_block` is the offline path, the same exclusion the Python
and MATLAB bindings make.

Async is the one genuinely new surface. Creating an `AudioContext` needs a user gesture, loading the
worklet module is a promise, and so is fetching the wasm. The idiomatic layer should own all of that
behind one `await Engine.create(...)` and leave the raw layer synchronous.

## Hosting

Cross-origin isolation is a **requirement of the chosen design**, and it was accepted as one rather
than designed around. The faithful two-thread model needs `SharedArrayBuffer`, and every current
browser gates that behind cross-origin isolation. The page must be served with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

and every cross-origin subresource must then opt in with `Cross-Origin-Resource-Policy` or be
fetched with CORS. Check `self.crossOriginIsolated` at startup and fail loudly: without it
`SharedArrayBuffer` is simply undefined, and a build that silently falls back to the
single-threaded shape is the worst outcome available.

This is a real deployment constraint, not a formality. It rules out the plain static hosts that do
not let you set headers, and it breaks embeds that are not CORP-clean. Both costs were weighed and
taken: the alternative was a second engine shape to maintain, which is more expensive than a
hosting rule.

There is deliberately **no non-isolated fallback build**. A page that silently drops to the
single-threaded shape looks like the real thing and is not, which is the one outcome worse than
refusing to start.

## What a no-SDK web build loses

The same thing a no-SDK desktop build loses, and on the web it is the only thing that matters:
**the real HRTF**. `src/binaural/steam_decode.c` is the production ambisonics-to-stereo decode and
it is the one SDK-gated file a headphone web target cares about. Occlusion, the reflection bed and
pathing are all SDK-gated too, and all three have viable substitutes already in the tree: manual
occlusion, the ISM early reflections in `src/acoustics/ism.c`, and the FDN late tail in
`src/acoustics/fdn.c`.

Without phonon you get `src/binaural/binaural.c`: a head-oriented constant-power pan, equivalent to
a first-order ambisonic encode with two opposed cardioid decoders. That is a real stereo image and
it responds to source position and head orientation, so routing, laterality and gross localization
are all verifiable. It is not an HRTF, so it has no elevation cue and no front-back disambiguation.

For a demo page that is a fair trade. For a listening experiment it is not.

## Steam Audio (phonon) under wasm

Upstream builds for wasm: the pinned submodule's CMake knows `IPL_OS_WASM`, builds it with
`-msimd128`, and `core/build/build.py` takes `--platform wasm` (it reads `EMSDK` and uses
Emscripten's toolchain file). `get_dependencies.py` knows the platform too.

**`tools/phonon/build-phonon.sh` now carries the whole wasm leg**, so this is one command from a
clean checkout, run from the repo root:

```sh
docker run --rm -v "$PWD:/ph" -w /ph \
    -e BWA_PLATFORM=wasm -e BWA_STAGE_DIR=wasm32 \
    emscripten/emsdk:latest tools/phonon/build-phonon.sh
```

It stages all four archives into `third_party/steam-audio-artifacts/lib/wasm32/`, which is the
name the root `CMakeLists.txt` already looks for. Measured on one 8-core desktop, about 20 minutes
from nothing: `libphonon.a` 6.85 MB, `libz.a` 103 kB, `libmysofa.a` 67 kB, `libpffft.a` 47 kB. Three things the script does for this platform
that it does for no other, all documented in its header and in
[../third_party/README.md](../third_party/README.md):

- it checks `EMSDK` and `emcc` up front, because `build.py` concatenates `EMSDK` into a toolchain
  path with no default and crashes inside their script when it is unset;
- it resolves the Python interpreter itself, because the emsdk image ships `python3` and no
  `python`;
- it builds **everything** `-pthread`, phonon and its three companions alike. See below for why
  that is not optional.

It also applies the one patch this needs, and it is not our patch. flatbuffers 1.12 has a private
`TableKeyComparator::operator=` that assigns to a reference member, which current clang rejects
outright:

```
flatbuffers.h:1875:12: error: overload resolution selected deleted operator '='
       buf_ = other.buf_;
```

The upstream flatbuffers fix is to declare that operator deleted rather than define it. This
matches the "one flatbuffers 1.12 patch" the kwaa/three-steam-audio build reports needing. It is a
compiler-version problem and not a wasm problem: the same dependency at the same pin builds on
MSVC, on gcc 13 and 14, and on the NDK's clang 18. It lives at
`third_party/patches/deps-flatbuffers-tablekeycomparator.patch` and it is the only patch in the
tree that lands on a DOWNLOADED dependency rather than on the submodule, which is why it is
applied after `get_dependencies.py` has run: before that, there is nothing to patch. Every
platform applies it, because gating it to wasm would only mean the next toolchain bump
rediscovers it.

**The with-SDK engine then links, and the HRTF decode works.** Point an Emscripten tree at that
stage, or just let the root CMakeLists auto-detect it, and it picks `wasm32` up as any other
platform: `bw_audio` builds, `test_steam_decode` builds, and it runs and prints real laterality
numbers (`mode2 +x, ident: L=327.9 R=51.8`, `mode2 +x, yaw180: L=56.68 R=314.4`), ending in
`steam_decode_test OK`. That is phonon's own HRTF convolving in wasm.

**Two things stood between that and a green with-SDK suite**, and neither was phonon's fault.
Both were measured against the staged archives, both are fixed, and the fixed tree measures
**35 of 38** through the script verbatim.

- **The C executables need the C++ link driver.** On desktop, `bw_audio` is a SHARED library and
  resolves phonon's C++ internally, so a C consumer never sees them. On wasm it is STATIC (see
  [What the spike changed](#what-the-spike-changed)), so phonon's archive reaches every consumer's
  own link, and every plain C target failed on `__cxa_throw`, `operator delete(void*, unsigned
  long)` and the libc++ shared-count symbols. `test_steam_decode` was the exception only because
  `bwa_add_steam_test` already sets `LINKER_LANGUAGE CXX` for exactly this reason. The root
  `CMakeLists.txt` now gives every executable the same property when `BWA_WASM` and the SDK are
  both on (the block just before the bindings). Patching it from the link line is NOT the fix,
  and both steps were tried: `-lc++ -lc++abi` alone gets the `-noexcept` variants of those
  libraries, so `__cxa_throw` stays undefined, and adding `-fexceptions` gets the real ones and
  then fails on `__cxa_begin_catch`. Let emcc pick the C++ link driver instead of arguing with it.
- **`tools/wasm/build-wasm.sh` was missing `-sSTACK_SIZE` on its emsdk branch.** Emscripten's
  default is 64 KB and the engine needs more than 128 KB, which [Toolchains](#toolchains) already
  records for the wasi side. Run verbatim without the flag, the script reported **13 of 33**, with
  the examples, `smoke`, `tools_api` and `golden` among the 20 failures, and every one read as a
  timeout rather than a stack fault. The script passes
  `-sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=1048576` now.

**What the with-SDK run says.** The simulator paths work: `reflect`, `bake`, `path` and `dynmesh`
all pass, so phonon's own thread pool runs under `-pthread` and kwaa's synchronous-pool patch is
not needed for any of the four subsystems. The one new failure is `fuzz_api`, and it is the
exception model, not the engine:

```
ERROR: Unable to load SOFA file: bwa_fuzz_garbage.bin. [10000]
Aborted(undefined)
```

On the desktops phonon catches that exception itself and `iplHRTFCreate` returns an error the
engine reports. The wasm phonon is compiled with emcc's default, which is exceptions OFF, so a
`throw` inside the archive is an `abort` of the whole module, and a test that hands the engine a
garbage SOFA on purpose takes the process down. The likely fix is `-fwasm-exceptions` on both
halves (the phonon recipe's `CFLAGS` and `CXXFLAGS`, and the engine's emsdk link), which is a
phonon rebuild and was not tried. Until it is, a web host must not pass a user-supplied file as
`bwa_desc.hrtf_path`: the built-in HRTF is the only one that cannot take the module down.

**And it works with `-pthread` too**, which was the open question. It does not work by accident:
a `-pthread` module needs every object in the link to carry the atomics and bulk-memory features,
so a default phonon against a `-pthread` engine fails with

```
wasm-ld: error: --shared-memory is disallowed by api_context.cpp.o
                because it was not compiled with 'atomics' or 'bulk-memory' features.
```

A `-pthread` phonon links and passes:

```
steam_decode_test OK (HRTF decode runs + preserves laterality: virtual-speaker, direct field, per-voice)
```

The script now does that without being asked, on both halves: `-DCMAKE_C_FLAGS=-pthread` and
`-DCMAKE_CXX_FLAGS=-pthread` on the phonon configure, and exported `CFLAGS` and `CXXFLAGS` for the
three companions, which `get_dependencies.py` gives no flag argument of their own. A caller's
`BWA_CMAKE_FLAGS` still wins, because CMake keeps the last of a repeated `-D`.

So the answer to "does phonon build under `-pthread`" is yes, and kwaa's synchronous-thread-pool
patch was not needed to get the decode working. Two caveats before treating this as done. The three
companion archives (zlib, pffft, mysofa) came out byte-identical with and without the environment
flags, so whether they needed the feature is untested rather than answered; they get it anyway.
And this exercised the HRTF decode only. The simulator paths (`steam_scene.c`, `steam_reflect.c`,
`steam_path.c`) run phonon's own threads and were not tried. That is where kwaa's patch would
matter.

Note what phonon costs here: `libphonon.a` alone is 6.9 MB, and on Android a static phonon took the
stripped library from 0.4 MB to 7.0 MB. A 7 MB wasm download is a different proposition from a 7 MB
`.so` on a headset.

## What would have to be built

In order, smallest first:

1. **An AudioWorklet sink** over `sink_quant`, stereo only, behind `BWA_HAVE_AUDIOWORKLET` and
   inside its own `*_sink.c` like every other backend. Nothing outside that file changes.
2. **A memory-buffer asset entry point**, or the decision to route every asset through the push
   feed instead. Pick one; do not leave both half-done.
3. **A JS/TS binding**, two layers, with the one-control-thread runtime guard.
4. **A CI job.** The wasi leg is the cheap one and already gives a real signal: it runs the same 33
   ctests the Linux job runs. Add it before the browser work, not after.
5. **phonon**, if the HRTF is wanted, and it is further along than expected: it builds, it links,
   and all five SDK-gated tests pass with `-pthread` on both sides, the simulator paths included.
   The recipe is wired into `tools/phonon/build-phonon.sh` and the flatbuffers patch is in
   `third_party/patches/`. What is left is the exception model (`fuzz_api`, above: a phonon
   rebuild with `-fwasm-exceptions` on both halves) and the CI side of it (a `wasm32` cache key,
   and a job that calls the script).

## What the spike changed

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
