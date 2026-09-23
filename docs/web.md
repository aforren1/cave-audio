# Web (WebAssembly)

Status: **the runtime is built and a browser runs it**. The engine core builds for wasm32 and the
offline suite passes there. Steam Audio builds for wasm too and the HRTF decode test passes against
it. On top of that there is now an AudioWorklet sink (`src/sink/worklet_sink.c`), a two-layer
JavaScript binding (`bindings/web`) and a demo page. A headless Chromium drives the sink end to end
and reports `worklet:1 (steam HRTF direct)` rendering at the audio clock, including the suspended
and resumed handoff. What has NOT happened is a person listening to it: nothing here says the image
is right, only that the path runs. Read
[What is verified and what is not](#what-is-verified-and-what-is-not) for the exact line.

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
| Emscripten 6.0.10, `-pthread`, phonon staged, **`-sPROXY_TO_PTHREAD`** | **36 of 38**, 2026-09-22, through `BWA_WASM_PROXY=1 tools/wasm/build-wasm.sh`. `idle` goes GREEN (1.63 s), which is the finding: main() on a pthread leaves the main thread's event loop running, and the asset loader's wake arrives. `os` still times out and `fuzz_api` still fails. |
| Emscripten 6.0.10, `-pthread`, phonon staged, the WEB build | `bindings/web` links (`bw_audio.mjs` 120 kB, `bw_audio.wasm` 6.4 MB with a static phonon) and its **29 node tests pass**, 2026-09-22, through `tools/wasm/build-web.sh`. Null sink, manual sink, the generated raw layer against the module's real exports, and a manual-sink render that pins laterality. No browser was involved. |
| Emscripten 6.0.10, no `-pthread` | builds and links. **13 of 33 pass**: `rt_create` fails, because `stream_set_create` cannot start its refill thread. |
| zig cc 0.13 | compiles the DSP. No threads at all: see [Toolchains](#toolchains). |

The first two rows are **no-SDK** builds; the rest carry the wasm phonon. The one with-SDK red
that is phonon's is the exception model: see
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

**`-sPROXY_TO_PTHREAD` is the C-side proof of the same point, and it is measured.** It runs `main()`
on a pthread and leaves the browser (or node) main thread free to run its event loop, which is
exactly the shape the decision asks for. `tools/wasm/build-wasm.sh` takes `BWA_WASM_PROXY=1` for it.
Numbers from one machine, same tree, same commit, both through the script verbatim in the
`emscripten/emsdk:latest` container on 2026-09-22:

| link | result |
|------|--------|
| without `-sPROXY_TO_PTHREAD` | 35 of 38. Failures: `os` (timeout), `idle`, `fuzz_api` |
| with `-sPROXY_TO_PTHREAD`    | **36 of 38**. Failures: `os` (timeout), `fuzz_api` |

So `idle` goes green and `os` does not. `idle` is the one that mattered: it is the asset loader's
wake, the thing a page's asset path depends on, and it now arrives. `os` still times out, and the
cause is NOT yet pinned - it is a hang rather than a lateness bound, and the per-test output was
not captured on the run that produced these numbers. `fuzz_api` is unrelated and already explained
under [Steam Audio (phonon) under wasm](#steam-audio-phonon-under-wasm): the wasm phonon is built
with exceptions off, so a deliberately garbage SOFA file aborts the module. Do not hand a web host
a user-supplied `bwa_desc.hrtf_path`.

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

Built: `src/sink/worklet_sink.c`, `BWA_SINK_WORKLET`, behind the `BWA_WITH_WORKLET` CMake option.
[backends.md](./backends.md) carries it in the contract's tables like every other backend; this
section is the part that is specific to a browser.

An AudioWorklet's `process()` gets **128 frames**, always. `src/sink/sink_quant.c` is the seam for
that, and this is the easiest customer it will ever have: 128 divides every sensible block size, so
one rendered engine block feeds exactly `block / 128` callbacks with no partial slot. The output
buffer is planar float already (`AudioSampleFrame.data` is `data[channel * samplesPerChannel + i]`),
which is the bus layout, so the adapter's runs copy straight out with no conversion and no
interleave. The sink is small for that reason.

Four decisions in it are worth reading before changing anything.

**Where the Web Audio calls run.** On the browser main thread, always, and two facts force it:
`new AudioContext()` needs a `Window`, which a Worker global scope is not, and Emscripten's handle
table (`emAudio` in `libwebaudio.js`) is an ordinary per-thread JavaScript variable, so a handle
minted on one thread means nothing on another. Emscripten proxies none of it: its own audio-worklet
tests all call `emscripten_create_audio_context` from `main()` on the main thread, and there is no
`PROXY_TO_PTHREAD` variant among them. The sink therefore proxies its setup with
`emscripten_proxy_sync` and calls through directly when it is already there. This is also the
reason the binding has two topologies; see [A JavaScript binding](#a-javascript-binding).

**The suspended context.** A browser starts an AudioContext suspended and only a user gesture may
resume it. A suspended context renders no quanta at all, so `process()` is never called, so nothing
pulls the adapter and the engine's dsp clock, playheads and fades would stop. That is not a device
fault and must not read as one. Rule 4 already has the answer for a device that went away and the
sink gives the same one: a host-paced thread paces silence so the clocks keep advancing, and
`device_lost` says so. On this platform that is the NORMAL state until the button is pressed.

The handoff between that thread and the worklet has no precedent in the other backends, because
no other backend has two pullers. Both pull the same `SinkQuant`, which is single-threaded by
contract, so they share a **wait-free try-lock**: whoever finds it taken bails immediately. The
worklet writes one quantum of silence, the host thread skips one period. It is not a mutex and the
audio thread never blocks, which is what keeps invariant 1. It is contended only in the single
quantum where a context resumes or suspends. The host thread decides the worklet is live from a
heartbeat `process()` bumps, and stands down after eight quiet block periods.

**What it cannot measure, which is everything the device would have told you.** Web Audio exposes
no xrun counter, no device position and no underrun signal. `currentFrame` in the processor scope
advances by exactly one quantum per callback whether or not the output starved, and a browser
renders several quanta back to back inside one system audio callback, so the "`currentTime`
advanced by more than a quantum" rule this note proposed before the sink existed would
false-positive on every ordinary batch. So `measured` is **false** and `device_pos_valid` is false:
a zero dropout count means "cannot know". `late_blocks` and `render_ns_peak` come from the adapter
and are real, on a 1 ms clock (below). `AudioContext.baseLatency + outputLatency` is the output latency, read once at open
off the context object, because `webaudio.h` has no accessor for either.

**The clock, and a trap that is invisible.** `sample_pos` is the adapter's own stream position, not
`currentFrame`: rule 3 defines it as the frames handed to the device before this block, which is
what the adapter counts, and while the node is connected `currentFrame` minus its value at the
first callback IS that number. Reading it would cost a wasm-to-JS transition per quantum for a
value the sink holds. The host half is `os_monotonic_ns` like every other backend, and here is the
trap: `AudioWorkletGlobalScope` has no `performance` (checked in Chrome 153: `typeof performance`
is `"undefined"` inside a processor), so Emscripten's `CLOCK_MONOTONIC` returns `ENOSYS` on the
audio thread and writes nothing. Until 2026-09-23 `os_monotonic_ns` did not check, and returned
whatever the stack held. Measured through a debug export in headless Chrome: `clock_gettime`
returned -1 with errno 52 on every quantum, and the value read back was the constant 1069547520.
So every render time read 0 except the first, which read 1.07 s and pinned `peak_load` at 401
(40108 %), which is the "percentages with many digits" a Galaxy XR showed. Now it falls back to
`emscripten_get_now`, which Emscripten itself resolves to `Date.now` in that scope: the only clock
the scope has. Same epoch as the control thread's `performance.timeOrigin + performance.now`, a
thousand times coarser, and not monotonic. There is no better clock to find: Emscripten 6.0.10
has no worklet time source past this, `currentTime` and `currentFrame` are the audio clock (they do
not move during a `process()` call), and a Worker spinning `performance.now()` into shared memory
would burn a core to fix a readout. So on this backend a render time is whole milliseconds against
a 2.67 ms quantum, `late_blocks` can be off by one near the budget either way, and both of the
adapter's backward-step rules are load-bearing: a stamp never steps back, and a render time that
would go negative books 0 instead of wrapping to 1.8e19 ns.

**Channel count**: stereo, said at the open. AUTO sends anything wider to the offline sink, the way
Android's AAudio sink does.

**Teardown** has no thread to join. Returning `false` from `process()` is the only "this processor
is done" the API offers, so `close()` sets a flag, destroys the node on the main thread, and waits
for the heartbeat to go quiet before freeing anything the callback touches. The wait is bounded,
because a suspended context never calls `process()` and would otherwise hang the close forever.

## Which invariants survive

From CLAUDE.md's list:

1. **No allocation, locks, syscalls or file I/O on the audio thread.** Survives, and half of it is
   free: an AudioWorklet has no file system to do I/O against. What you must not do is take the
   control-on-audio-thread shape and then pretend otherwise.
2. **One control thread.** Survives, and in the `"worker"` topology it needs no guard: the engine
   pointer exists only inside the control Worker, so a second caller has nothing to call with. The
   Python binding needed a runtime guard because Python makes threads cheap inside one address
   space; JavaScript's threads are separate realms, which turns the same rule into a structural
   one. The `"main"` topology has no second thread to be wrong from either.
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

A third answer beat both and is what shipped: write the wav into the module's own file system and
let `bwa_load_sound` open it there. See [Where the audio comes from](#where-the-audio-comes-from)
below, which records what the push feed cost when it was the default.

Disk streaming (`src/core/stream.c`) has no direct equivalent. The nearest shape is a JS-side
fetch that feeds a push source, which moves the refill cadence out of the engine.

## A JavaScript binding

Built: `bindings/web`. Two layers, mirroring `bindings/python` for the reason it has two.

- **`src/raw.js` is the raw layer**: the C ABI one for one, C names minus the `bwa_` prefix, C
  argument order, C units. The table it is built from is GENERATED out of `include/bw_audio.h` by
  `tools/wasm/gen-abi.mjs` (167 entries), so a call cannot drift from the header, and
  `tests/raw_layer.test.mjs` checks the generated table against the module's real wasm exports, so
  the table cannot claim a symbol the engine does not carry. The same generator writes the
  `-sEXPORTED_FUNCTIONS` list, which is also what forces the archive members into a static link.
- **`src/engine.js` is the idiomatic layer**: `Engine`, `Sound`, `Source`, `PushSource`, `Bed`,
  `Listener`, with the Python layer's auto-commit model. A commit-gated write lands at once unless
  you are inside `frame()`, which defers to one commit at the block's exit.

`bwa_set_output_capture` is excluded from BOTH layers, so it is absent rather than
present-and-broken. Its callback runs on the audio thread. The manual sink plus `renderBlock()` is
the offline path, the same exclusion Python and MATLAB make.

Units follow the rule the Godot binding learned the hard way: Web Audio speaks seconds everywhere
and the frame-valued calls keep saying frames (`seekFrames` beside `seekSeconds`).

### The page talks to a Worker, and the frame is batched

`src/client.js` is what a page imports. The idiomatic layer lives on the control thread and the
page reaches it over `postMessage`, which was a choice between two options and is the cheaper one
per frame.

The per-frame path is a **SharedArrayBuffer frame slab plus one message**. `setPose` and
`setPosition` write the slab in place and cost nothing; `pushFrame()` publishes with one atomic
store and one small message, and the control side applies the whole frame inside a single
`engine.frame()`, which is a single `CMD_COMMIT`. The alternative, proxying each raw call, puts one
structured clone per call on that path: a frame with a pose and 32 sources is 33 messages, each
landing on the Worker's task queue in its own turn, so one visual frame's writes reach the mixer
spread across several audio blocks. That is the incoherence invariant 6 exists to prevent. Only
sources that moved are written, and a sequence number makes a burst collapse to its newest state
rather than replaying stale ones.

Everything that is not per frame is a promise. `invoke(name, ...args)` reaches any of the 167 calls
with the engine pointer supplied, which is why the ergonomic surface stays small.

### Two topologies, and why the second one exists

| topology | control thread | AudioWorklet sink |
|----------|----------------|-------------------|
| `"worker"` (default) | a dedicated module Worker | no |
| `"main"` | the page's main thread | yes |

`"worker"` is this note's decision, implemented: the engine pointer exists only inside the Worker,
so invariant 2 is structural rather than guarded, and the control side may block, which it must.

`"main"` exists because of the Emscripten property the sink section records: the AudioContext can
only be created on the thread that owns the module's `Window`, and the handle table is per-scope.
So the module instance that owns the sink must be the page's main-thread one, and today an audible
page pays for its sound with the Worker control thread. What that costs: `destroy()` joins threads
on the main thread (Emscripten busy-waits and pumps its queue, so it survives, but it is a stall),
and a page that used async assets or streaming would meet the `idle` failure measured above.

This is a deviation from the decision at the top of this note, taken knowingly and scoped to the
one topology that needs it. Three ways out, none of them tried:

1. Emscripten proxies its `webaudio.h` entry points to the main runtime thread. `worklet_sink.c`
   already calls through `emscripten_proxy_sync`, so nothing in the C would change.
2. `-sPROXY_TO_PTHREAD` with the control layer driven from the main-pthread, which needs the
   binding's JavaScript to run in that pthread's Worker scope (`--pre-js` puts it there) and a C
   trampoline to reach it. Measured to be the right shape for the engine, untried for the binding.
3. Web Audio grows an AudioContext a Worker can create, which is a specification change.

### One AudioContext, one engine

Measured 2026-09-22, on the playground's profile switch, and it is a platform property rather than
a binding bug: **a second engine cannot open on an AudioContext that has already carried one.**

Emscripten's worklet bootstrap calls `audioWorklet.addModule(bw_audio.mjs)`, and that script runs
`registerProcessor("em-bootstrap", ...)` in the context's own `AudioWorkletGlobalScope`. A scope is
per AudioContext and survives the node, so the second `addModule` re-runs the same registration in
the same scope. It throws, the promise the glue never catches rejects, the setup chain stops, no
node is ever created, and the sink does what rule 4 says for a device that is not there: it
host-paces silence with `device_lost` set. The engine keeps rendering, `bwa_health` keeps counting
blocks, and the page is silent. Every later rebuild on that context fails the same way.

So a create-time change - the profile, a layout file - replaces the AudioContext. The page owns it
and can, and the wasm module is reused across the rebuild (`BwaEngine.create({ module })`), because
instantiating a second 6 MB module per switch leaks both it and its pthread pool.

Two lessons past the one fact. A health block cannot tell silence from sound on this platform: the
host-paced fallback counts blocks exactly like a working sink, so "the rebuilt engine renders" was
a green check over a silent page. And a page has no tap on `AudioContext.destination`, so
`BwaEngine.outputNode()` now hands back the sink's own node for an AnalyserNode to sit beside -
which is what the playground's check measures the rebuild with.

### Where the audio comes from

Both of the two options this note weighed are built, and the one it ranked second turned out to be
the fallback rather than the default.

**A loaded sound, through MEMFS.** The third option, which nothing above predicted because the file
system work had not happened yet: write a wav into the module's own file system with
`engine.writeFile` and load it with `bwa_load_sound`. No memory-buffer entry point, no ABI change,
and the engine owns the samples, so it loops them sample accurately and the page is out of the
audio path. The playground does this with every stimulus it plays (`playground/wav.js` wraps a
`Float32Array` in a 44-byte header) and with a clip the visitor drops on it, decoded by
`decodeAudioData` and folded to mono first.

**A push source**, for audio a page makes or receives as it goes. `example/index.html` is that
demo, and it falls back to a synthesized click when no file is supplied so the page works with no
asset served.

The push feed was the playground's default until 2026-09-22 and it was the wrong one, for a reason
worth keeping: a push feed puts the PAGE in the audio path, and a page's main thread stalls. A
garbage collection, a window drag, a heavy frame on a weak GPU, a tab losing focus. Any stall
longer than what is queued ahead is a hole in the sound. It was reported by ear ("the click train
becomes inaudible for short periods"), and the engine had been counting it all along as a stream
starve: 31 to 37 in one ordinary headless check run, and 45 to 59 more from a single deliberate
400 ms stall. Loading the same stimuli instead took both to zero, and took a stimulus switch from
97 to 170 ms down to 6 to 16 ms, because the queue was also the switch latency.

So the rule is the shape of the audio, not the platform: a buffer you have in full before it starts
belongs to the engine, and a push source is for the stream you do not have yet. If you do push,
pace the feed on the ring's own space (`bwa_source_push_space`), not on the animation frame: a
background tab gets fewer frames and the audio thread does not slow down with it. Pace it, do not
FILL it: the ring holds 65536 frames, which is 1.37 s of audio that has to play before anything new
can be heard.

### Files the engine opens

`bwa_desc.layout_path` and `bwa_desc.hrtf_path` are paths, opened with an ordinary `fopen` inside
`bwa_create`. The module exports `FS`, so the only file system those paths can name is the module's
own MEMFS, and the binding writes into it: `create({ files })` for the two paths create itself
opens, `engine.writeFile(path, bytes)` for anything later. The playground's layout upload was the
first user; its stimuli are the second, and they are why audio no longer has to go the other way
through a push source.

### Build and test

```
tools/wasm/build-web.sh
```

It regenerates the raw layer from the header, configures with `BWA_WITH_WORKLET=ON
BWA_BUILD_WEB=ON`, and stages a self-contained `bindings/web/dist/`: `bw_audio.mjs` (120 kB),
`bw_audio.wasm` (6.4 MB with a static phonon), and the binding's JavaScript. No bundler and no
server-side step. `node bindings/web/example/serve.mjs` serves it with the two isolation headers.

The module is linked `-sENVIRONMENT=web,worker,node` so the SHIPPED artifact is the TESTED one:
`ctest -R web_bindings` runs 29 node tests against it. `bindings/web/README.md` has the whole
public surface.

## What is verified and what is not

Worth stating plainly, because the sink is the part a reader will most want to trust.

**Verified**, on 2026-09-22, in the `emscripten/emsdk:latest` container (emcc 6.0.10) with the
wasm32 phonon staged:

- `worklet_sink.c` compiles clean at `-Wall -Wextra` and links into `bw_audio.mjs`.
- the web module builds and its 29 node tests pass: the generated raw layer against the module's
  own exports, engine create/start/stop/destroy on the null sink, the commit model, handle
  generation gating, a push source's ring, a manual-sink render that pins laterality with the
  frame slab driving the position, and the file-system seam (a layout written through `FS` is the
  layout `bwa_create` loads, and one the loader rejects refuses at `bwa_start`). The laterality
  test was BROKEN ON PURPOSE (the frame apply disabled) and confirmed red before being trusted
  green; so was each layout test.
- the engine suite: 35 of 38 without `-sPROXY_TO_PTHREAD`, 36 of 38 with it.

- **the AudioWorklet sink in a real browser**, through `bindings/web/tests/run-browser.mjs`, which
  serves the page cross-origin isolated and drives it in headless Chrome 153. One run's notes:

  ```
  ok   cross-origin isolated
  ok   AudioContext running at 48000 Hz
  ok   backend "worklet:1 (steam HRTF direct)", sink type 9
  ok   output latency 736 frames (context baseLatency 0.01, outputLatency 0)
  ok   451 blocks in 2.43 s of wall time (2.41 s of audio)
  ok   the render is paced by the audio clock
  ok   device_lost is 0: the AudioWorklet is driving, not the host-paced fallback
  ok   health.measured is false, as the backend promises
  ok   suspended: 107 blocks in 600 ms, device_lost 1
  ok   resumed:   112 blocks in 600 ms, device_lost 0
  ok   destroy returned
  ok   worker topology backend "null (steam HRTF direct)"
  ok   worker topology rendered 111 blocks in 600 ms
  ok   the worklet sink is not reachable from a Worker, as expected: AudioContext is not defined
  ```

  That covers the async setup chain, the node connect, `process()` on the AudioWorklet thread
  pulling the adapter, the audio clock pacing the render, and the suspended-to-running handoff in
  BOTH directions, which is the try-lock and the heartbeat. The last two lines are also what makes
  the `device_lost is 0` assertion discriminating rather than decorative: the same page shows it
  going to 1 under a real condition. It runs as `ctest -R web_browser` and SKIPS when it finds no
  Chromium. The 736-frame latency is the context's own 10 ms plus the one block the adapter can
  hold, which is also the check that the `baseLatency` read works at all: `sink.h` says 0 means
  unknown. The last two lines are the DEFAULT topology, which node cannot reach: a real module
  Worker holding the engine, the postMessage protocol, the frame slab across a thread boundary,
  the engine's own threads as NESTED workers, and a `destroy()` that joins them from a Worker
  rather than from the main thread. The last line is the two-topology split itself, MEASURED
  rather than reasoned: opening `BWA_SINK_WORKLET` from the control Worker fails with
  `AudioContext is not defined`. Note that the check has to START the engine, not just create it:
  no sink is opened until `bwa_start`, so a create that resolves proves nothing.
- **the example page loads**, served by `example/serve.mjs` and dumped from headless Chrome: the
  page is cross-origin isolated, its module script imports `dist/index.js` without throwing, and
  the start button is there. The click path itself is the same code the check above drives.
- **the XR page's head-pose path**, through `bindings/web/tests/run-xr.mjs`, which loads
  `bindings/web/xr/` twice in headless Chrome: once with `navigator.xr` deleted, which is the
  no-WebXR state the page has to report and recover from, and once with a minimal fake one that
  opens an `immersive-vr` session against a synthetic `XRFrame` whose viewer pose the check
  chooses. That runs the whole chain: the XR reference space to room seam in both signs on every
  axis, the per-frame `bwa_set_listener_pose` and commit, the page-side prediction lead in meters,
  the in-world menu and its ray hit test, the controller grab and the thumbstick nudge. Two
  assertions close the loop on the audio rather than on the arithmetic. `bwa_get_listener_pose`
  reads the turned head back out of the page's own engine, and a second engine on the manual sink
  renders the page's live pose, so a source the listener looks at comes out 0.9 dB off center
  while the same source with the head turned a quarter comes out 11 dB into one ear, both ways
  round. The seam has a browser-free node test beside it, `tests/xr_frame.test.mjs`, which checks
  it against `BWA_ROOM_AHEAD`, `BWA_ROOM_UP` and `BWA_ROOM_RIGHT` rather than against the algebra
  the module derives. Every assertion in both was broken on purpose and confirmed red first.
  Since 2026-09-23 it also pins the Galaxy XR fixes: the head gizmo is hidden in session and back
  in the flat preview, the engine opens at block 128, the status table reads `128 / 128 = 1`,
  the session asks for `hand-tracking`, a gamepad-less hand input grabs on a pinch and carries the
  source in height and depth, the panel's move buttons move it, and a block-256 rebuild and a
  `playback` latency-hint rebuild both come back driven by the worklet. The health readout is
  pinned too: the status table leads with the audio clock slip over a full 5 s window, the render
  peak row reads whole milliseconds and says "(1 ms clock)", the percentage rows are gone, and the
  in-world health line leads with the slip and wraps to at most two lines at the panel's own font
  and width. `tests/xr_slip.test.mjs` pins the slip's sign, window, suspended state and
  per-context reset under node. Each new pin was broken once and went red.

**Not verified** on the XR page: that three.js renders it. Headless Chrome has no XR device, so
the fake session cannot be bound to a framebuffer, and the page is deliberately built so the pose
path does not care. Stereo rendering, the projection layer, a real runtime's controller poses and
gamepad layout, reprojection, and every latency number are headset work. So is the question the
page exists to ask, which is whether the image stays put when you turn your head.

**Not verified**: that it sounds right. Nobody has listened. A headless browser has no speaker, so
what the check proves is that the path runs and is paced by the audio clock, not that the HRTF
image is correct - and the wasm build uses phonon's real HRTF, so a by-ear pass is worth having.
Also unverified: the `baseLatency` read (headless Chrome reports one, but not one anybody has
compared against a stopwatch), the `Date.now` clock's effect on `bwa_get_clock_model`, and
anything on a browser that is not Chromium.

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

The demo is hosted on GitHub Pages, which cannot set either header, so the site carries a service
worker that adds them to every response it serves (coi-serviceworker, vendored under
`bindings/web/deploy/`). Every page registers it and reloads once, and `crossOriginIsolated` is
true from the second load on: measured in headless Edge against a server that sends no headers,
including a cold deep link into `example/`. `.github/workflows/pages.yml` builds the wasm engine
and phonon in the `emscripten/emsdk` container on every push to main, stages the artifact with
`bindings/web/deploy/stage.sh` (the site root mirrors `bindings/web`: `dist/` and `example/` side
by side, the shell and the worker at the root) and deploys it to
`https://aforren1.github.io/cave-audio/`.

The worker does not soften the CORP rule, it moves it. Every asset the pages load has to be
same-origin or send `Cross-Origin-Resource-Policy`, so the artifact vendors what it needs and
loads nothing from a CDN. A three.js page vendors three.js. `bindings/web/deploy/README.md` states
the rule and `stage.sh` warns when the staged tree breaks it.

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

## What is left

In order, and the first three are no longer on this list because they are built.

1. **Listen to it.** The headless check proves the path runs; it cannot say the image is right,
   and the wasm build carries phonon's real HRTF, so a by-ear pass on a page is worth having. It
   is the same open item `docs/hardware-validation.md` holds for the desktop headphone path.
2. **The control thread for the audible topology.** Today `SinkType.WORKLET` needs the module on
   the page's main thread, which is a deviation from the decision at the top of this note. The
   three ways out are named under
   [Two topologies](#two-topologies-and-why-the-second-one-exists).
3. **A CI job.** The wasi leg is the cheap one and already gives a real signal: it runs the same
   suite the Linux job runs. The web leg is `tools/wasm/build-web.sh` plus `ctest -R web_bindings`,
   which needs only node.
4. **`os` under `-sPROXY_TO_PTHREAD`.** It times out rather than failing a bound, and the cause is
   not pinned. It is the one red that is not explained.
5. **The phonon exception model.** `fuzz_api` aborts the module on a garbage SOFA file because the
   wasm phonon is built with exceptions off. The likely fix is `-fwasm-exceptions` on both halves,
   which is a phonon rebuild and was not tried. Until it is, a web host must not pass a
   user-supplied file as `bwa_desc.hrtf_path`.
6. ~~A memory-buffer asset entry point, or the decision that the push feed is the answer.~~
   **Answered**, 2026-09-22, and by neither of those: a page writes a wav into MEMFS and calls
   `bwa_load_sound`, so the engine owns the samples and loops them itself. The push feed stays for
   audio a page generates or receives as it goes. What is genuinely left here is STREAMING: a
   sound too long to sit in the module's heap has no answer, because `bwa_load_sound_streaming`
   reads a real file system as it plays.
7. ~~A way to put a file in the wasm file system.~~ **Built**, 2026-09-22: `FS` is in
   `EXPORTED_RUNTIME_METHODS`, the binding writes through it (`create({ files })` and
   `engine.writeFile`), and the playground uploads a `cave_layout.json` with it. It cost one link
   flag and one method, as predicted. A SOFA file is still not safe to accept from a user: see 5.
8. **A struct-taking wrapper in `bwa_web.c`.** `invokeBuf` marshals arrays, deliberately not
   structs, so three calls are out of reach: `bwa_fdn_config`, `bwa_reflections_config` and
   `bwa_apply_tuning`. That is why the playground has no reverb bed scene. Each needs a field by
   field wrapper beside `bwaw_create`, in the shape that file already establishes.

## What the runtime added

On top of [What the spike changed](#what-the-spike-changed) below.

- `src/sink/worklet_sink.c`: the backend, and the only new file in `src/`.
- `src/sink/sink.h`, `src/sink/sink.c`: the declarations, the dispatch, the device query and the
  browser AUTO entry. One `#elif defined(__EMSCRIPTEN__)` branch, placed BEFORE the `__linux__` and
  `__APPLE__` ones because Emscripten's sysroot defines `__unix__` and a plain chain would offer a
  backend no browser carries.
- `include/bw_audio.h`: `BWA_SINK_WORKLET = 9`, appended.
- `CMakeLists.txt`: the `BWA_WITH_WORKLET` block. OFF by default even on Emscripten, because
  `-sAUDIO_WORKLET` pulls in `-sWASM_WORKERS` and changes `emscripten_get_now` for every target
  that links the library, and the CI leg must not pay for that.
- `cmake/bwa_bindings.cmake`, `bindings/web/CMakeLists.txt`: a fourth binding, opt-in and
  Emscripten-only.
- `bindings/web/`: the two layers, the transport, the example page, the node suite, the README.
- `bindings/web/playground/`: the browser port of `examples/playground.cpp`, with three.js visuals
  and six scenes, on the generated 24-speaker dome (`examples/dome_24.json`, staged into `dist/`
  by `build-web.sh`) until you upload a layout, plus `tests/run-playground.mjs` that drives it in headless Chromium. Its check
  measures the AUDIBLE output now, through an AnalyserNode pair on `engine.outputNode()`: the
  speaker cones and the meter strips have to move while the default click train plays, a stimulus
  change has to reach the output inside 150 ms, and a profile rebuild has to keep `device_lost` at
  0 and still put a `+x` source in the left ear. Each of those replaced a check that a silent page
  passed.
- `bindings/web/xr/`: the head-tracked sibling of that page. In an `immersive-vr` session the
  viewer pose becomes the engine's listener pose every XR animation frame, which is the one thing
  headphones and a mouse cannot demonstrate. It imports the playground's rig, scenes, gizmos and
  control builder, and adds four things of its own: `frame_xr.js`, the XR reference space to room
  seam (a 180 degree yaw, derived and tested, NOT the mirror the two "+x" conventions suggest);
  `session.js`, whose pose path deliberately does not depend on the renderer binding; `panel.js`,
  the in-world menu, because DOM Overlay is an AR-only feature in practice; and a page-side
  prediction lead, because `bwa_set_pose_prediction` only leads the engine's own tracker and is
  inert for a pushed pose. `tests/run-xr.mjs` and `tests/xr_frame.test.mjs` are its checks, and
  `bindings/web/README.md` has the derivation in full.

  The XR page opens the engine at block 128, one Web Audio render quantum, and not the rig's
  default 256. At 256 the worklet sink's fixed-quantum adapter renders a whole 256-frame block
  inside every second `process()` call and only copies out in the call between, so the render
  cost lands in half the callbacks at twice the size. At 128 every call renders one block
  straight into its slot and copies it out, which is the adapter's pass-through case, and the
  adapter holds no extra block of latency. Nothing in the engine refuses 128: phonon's frame size
  is whatever the engine block is, and the scratch buffers are sized to the 8192-frame ceiling.
  This came from a Galaxy XR report of crackle under `cave_sim`. It is a hypothesis about a
  mobile SoC, not a measurement, and per-block overhead is higher at 128, so the panel offers
  128, 256 and 512 and an `interactive` or `playback` latency hint. Either one rebuilds the engine
  and the AudioContext. No new gesture is needed, because the Enter VR press already unlocked
  audio for the document.

  What to read on the headset: the panel's "audio health" line, two lines, numbers first. The
  first number is the **audio clock slip**: over the last 5 s, wall time from `performance.now()`
  minus audio time from `AudioContext.currentTime`, both read on the main thread every animation
  frame (`xr/slip.js`). It is the one dropout signal this platform has. When the render thread
  misses a deadline Chrome plays fallback silence and the graph does not advance, so the audio
  clock falls behind: 0 is healthy, positive is the audio thread not keeping up. Beside it is the
  count of animation frames over 30 ms in the same window, which tells a stalled main thread (long
  frames, no slip; the main thread samples both clocks late, not apart) from a stalled audio thread
  (slip, no long frames). Then late blocks in the window, then the render peak in milliseconds
  against the block budget, marked "(1 ms clock)" because on the worklet that is what it is. There
  is no percentage and no "per process() call" figure any more: both multiplied a 1 ms reading.
  Headless desktop Chrome, measured by `run-xr.mjs`: in the first 5 s after a start the slip reads
  2 to 3 percent (the context's clock sits at 0 for its first few hundred ms) and the render peak
  7 to 13 ms (the first blocks pay for wasm compilation); once the start is out of the window it
  reads 0.1 percent (3 ms) and 1 ms. A plain oscillator page reads the same 0.1 percent, so that is
  the resolution of `currentTime` on the main thread, not a loss. The direction and the scale
  were checked with a worklet that busy-waits on purpose, headless Chrome 153, 5 s windows: no
  stall reads -0.1 percent, a 20 ms stall once a second reads 0.1 percent (the device buffer
  absorbs it), a 100 ms stall once a second reads +2.4 percent (119 ms). So the slip counts the part
  of a stall that outlasted the buffer, which is the silence you hear, not the stall itself. The 330 to 480 percent peaks this
  section used to quote were not start-up cost at all: they were the worklet's uninitialized clock
  (see "The clock, and a trap that is invisible" above).

  **Why a bigger block gives fewer but longer glitches here.** The adapter renders a whole engine
  block synchronously inside ONE `process()` call, and the browser cannot hand the device that
  quantum until the call returns. So a render that overruns stalls the output for the whole render,
  not for the part that overran. At 512 a block renders once every four quanta and costs about four
  times as much, so an overrun happens a quarter as often and stalls about four times longer: fewer,
  longer bursts, which is what the Galaxy XR showed. At 128 the same cost is spread over every
  call. Block size moves the glitches around; it does not remove them. The cure is render cost
  (fewer voices, `binaural` instead of `cave_sim`, a cheaper scene), or a device buffer with more
  headroom (`playback`). If the slip stays at 0 while the audio crackles, the render is keeping up
  and the problem is downstream of it.

  The page also asks for `hand-tracking` as an optional feature. A pinch is `select`, so
  pinch-and-hold grabs the source and carries it on all three axes. A hand has no thumbstick, so
  the panel has move buttons (left, right, up, down, ahead, back, 0.25 m each, relative to where
  you look). Hand joints are not drawn. The playground's head gizmo is hidden in session, because
  it sits at the tracked pose and its nose cone floated 22 cm in front of the eyes.
- `bindings/web/src/host.js`, `client.js`: `invokeBuf`, which reaches the raw calls whose arguments
  are pointers. A page cannot allocate wasm heap of its own, so the alloc, the copy and the free
  happen around the one call on the control thread. Plus `writeFile` and `create({ files })` for
  the two `bwa_desc` fields that are PATHS, `create({ module })` so a rebuild costs no second
  module, and `outputNode()` so a page can tap what it is playing.
- `tools/wasm/gen-abi.mjs`: the raw layer and the export list, generated from the header.
- `tools/wasm/build-web.sh`: the one recipe for a shippable `dist/`.
- `tools/wasm/build-wasm.sh`: `BWA_WASM_PROXY` for the `-sPROXY_TO_PTHREAD` measurement.

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
