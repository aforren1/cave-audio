# Web binding

The engine as an ES module a browser page can load, with an AudioWorklet backend behind it.

Read [../../docs/web.md](../../docs/web.md) first. It holds the decisions this binding implements
and the measurements behind them.

## Build

```
tools/wasm/build-web.sh
```

That needs an Emscripten toolchain. On a host without one, the container image is the same one the
engine and phonon recipes use:

```
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src" -w /src emscripten/emsdk:latest \
    bash -c 'apt-get update -qq && apt-get install -y -qq ninja-build && \
             BWA_WEB_BUILD_DIR=/tmp/bw-web tools/wasm/build-web.sh'
```

Build inside the container and stage out to the bind mount. A bind-mounted build directory breaks
`FetchContent`'s file rename on a Windows host.

The script writes a self-contained `dist/`: `bw_audio.mjs`, `bw_audio.wasm`, the worklet bootstrap
emcc emits, and the binding's own JavaScript. There is no bundler and no server-side step.

## Run the example

```
node bindings/web/example/serve.mjs
```

Then open `http://localhost:8181/`. The server sets the two cross-origin isolation headers, which
are not optional: `SharedArrayBuffer` is gated behind them and the engine's two-thread model needs
it. A page served without them fails loudly. There is no single-threaded fallback, on purpose.

## Two layers

Same split as `bindings/python`, for the same reason.

- **`src/raw.js` is the raw layer.** One function per C entry point, named by its C name minus the
  `bwa_` prefix, with the header's arguments in the header's order and units. The table it is built
  from is generated out of `include/bw_audio.h` by `tools/wasm/gen-abi.mjs`, so it cannot drift.
- **`src/engine.js` is the idiomatic layer.** `Engine`, `Sound`, `Source`, `PushSource`, `Bed`,
  `Listener`, with the auto-commit model the Python layer has: a commit-gated write lands at once,
  unless you are inside `frame()`, which defers to one commit at the block's exit.

Both run on the control thread. A page reaches them through `src/client.js`.

`bwa_set_output_capture` is not bound, in either layer. Its callback runs on the audio thread and a
JavaScript callback must never run there. Use the manual sink and `renderBlock()` instead. That is
the same exclusion the Python and MATLAB bindings make.

## Two topologies

`BwaEngine.create({ topology })` picks where the module and the engine live.

| topology | control thread | AudioWorklet sink | use it for |
|----------|----------------|-------------------|------------|
| `"worker"` (default) | a dedicated module Worker | no | anything offline, measured or headless |
| `"main"` | the page's main thread | yes | a page that has to make sound |

`"worker"` is the shape `docs/web.md` decided on. The engine pointer exists only inside the Worker,
so the one-control-thread rule is structural rather than guarded, and the control side may block,
which it must: `bwa_destroy` joins threads and the asset loader parks on an `os_event`, and
`Atomics.wait` is forbidden on a browser's main thread.

`"main"` exists because of one Emscripten property, not because of a preference here.
`emscripten_create_audio_context` runs `new AudioContext()`, which needs a `Window` and throws in a
Worker (measured: `bwa_start` on `SinkType.WORKLET` from the control Worker fails with
`AudioContext is not defined`), and `libwebaudio.js` keeps its handle table in an ordinary per-scope JavaScript variable, so
a handle minted on the page means nothing in a Worker. None of it is proxied. So today an audible
page pays for its sound with the Worker control thread. `src/sink/worklet_sink.c` already proxies
its Web Audio work to the main runtime thread, so when that table becomes reachable across threads
the C does not change.

Pass an `audioContext` and you get `"main"` and the AudioWorklet sink together:

```js
const ctx = new AudioContext({ latencyHint: "interactive" });   // inside a click handler
await ctx.resume();
const engine = await BwaEngine.create({ audioContext: ctx, profile: Profile.BINAURAL });
```

The page keeps the context. The sink adopts it by handle through `bwa_desc.device`, so resuming it
stays the page's business, which is right: only a user gesture may do it.

## The per-frame path

A frame is one message, whatever the source count.

```js
engine.listener.setPose(px, py, pz, qx, qy, qz, qw);   // free: writes a shared buffer
src.setPosition(x, y, z);                              // free
engine.pushFrame();                                    // one message, one commit
```

`setPose` and `setPosition` write a `SharedArrayBuffer` the control thread reads. `pushFrame()`
publishes them with one atomic write and one small `postMessage`, and the control side applies the
whole frame inside a single `engine.frame()`, which is a single `CMD_COMMIT`.

A message per call was the obvious alternative and it is worse in the way that matters: a frame
with a pose and 32 sources is 33 structured clones, each landing on the Worker's task queue in its
own turn, so one visual frame's writes reach the mixer spread across several audio blocks. That is
the incoherence `CMD_COMMIT` exists to prevent.

Only sources whose position changed are written, so a static scene costs nothing, and the sequence
number means a burst of frames collapses to its newest state rather than replaying stale ones.

## The whole public surface

```js
import { BwaEngine, Profile, SinkType, SinkFlags, BwaError } from "./dist/index.js";
```

### `BwaEngine.create(opts) -> Promise<BwaEngine>`

| option | meaning |
|--------|---------|
| `wasmUrl` | the engine module. Default `./bw_audio.mjs` beside `client.js` |
| `topology` | `"worker"` (default) or `"main"` |
| `audioContext` | an `AudioContext` the page created. Selects `"main"` and `SinkType.WORKLET` |
| `maxSources` | frame-slab capacity, default 256 |
| `profile` | `bwa_desc.profile` |
| `sampleRate` | Hz. 0 or absent takes the engine default (48000) |
| `blockSize` | frames. 0 or absent takes the engine default (256) |
| `sink` | `bwa_desc.sink` |
| `device` | `bwa_desc.device` |
| `sinkFlags` | `BWA_SINK_FLAG_*` |
| `layoutPath`, `hrtfPath` | paths inside the wasm file system |

### On the engine

| member | kind | notes |
|--------|------|-------|
| `info` | object | `sampleRate`, `blockSize`, `channelCount`, `backend`, `sinkType`, `outputLatencyFrames`, `lastError`, `abi` |
| `listener` | object | `setPose(px,py,pz,qx,qy,qz,qw)` staged; `setPosePredictionSeconds(s)` async |
| `start()` | promise | resolves to a fresh `info` |
| `stop()`, `destroy()` | promise | |
| `createSource()`, `createPushSource()`, `createBed()` | promise | |
| `pushFrame()` | sync | publishes the staged frame |
| `health()` | promise | `blocks`, `xruns`, `droppedFrames`, `driverResyncs`, `lateBlocks`, `streamStarves`, `peakLoad`, `deviceLost`, `measured` |
| `renderBlock()` | promise | manual sink only. `{channels, nframes, data}`, planar, a copy |
| `setMasterGain(linear)` | promise | |
| `invoke(name, ...args)` | promise | any raw call that takes the engine pointer |
| `invokeBuf(name, ...args)` | promise | the same, for raw calls whose arguments are pointers |
| `raw(name, ...args)` | promise | any raw call that does not |
| `refreshInfo()` | promise | |

### On a source

| member | kind | notes |
|--------|------|-------|
| `handle` | number | the C `bwa_source` |
| `setPosition(x,y,z)` | sync | staged for `pushFrame()` |
| `play(sound, loop)`, `stop()` | promise | |
| `setGain`, `setSpread`, `setSize`, `setPaused` | promise | |
| `isPlaying()` | promise | |
| `destroy()` | promise | |
| `push(Float32Array)` | promise | push sources. Mono frames. Resolves to the count accepted |
| `space()`, `pushEnd()` | promise | push sources |

`invoke` is the reason this list is short. Every one of the 167 calls in `include/bw_audio.h` is
reachable by its C name minus the prefix: `engine.invoke("source_set_reverb_send", src.handle, 0.3)`
is `bwa_source_set_reverb_send(e, src, 0.3f)`. Units are the C units. Frame-valued calls keep saying
frames, because Web Audio speaks seconds everywhere and mixing the two is how the Godot binding
shipped a bug.

### `invokeBuf`, for the calls that take pointers

`invoke` marshals scalars, which is most of the ABI. It cannot reach the calls that fill or read an
array, and those are the ones a page with a picture in it needs first: `bwa_get_bus_levels` and
`bwa_get_speakers` fill floats, `bwa_scene_set_mesh_mat` and `bwa_scene_add_dynamic_mesh` take two
arrays each, `bwa_source_set_occlusion_manual` takes a 3-band tilt. A page cannot allocate wasm heap
for itself, because the module lives on the control thread, which in the default topology is another
thread. So `invokeBuf` does the alloc, the copy in, the copy out and the free around the one call.

An argument is a plain number, or one of:

```js
{ f32: [...] } | { i32: [...] } | { u32: [...] }   // copied IN, passed as a pointer
{ out: "f32" | "i32" | "u32", len: n }             // zeroed, passed in, read back OUT
```

With any `out` the result is `{value, out: [TypedArray, ...]}`, one entry per `out` in argument
order. With none it is the call's own return.

```js
const r = await engine.invokeBuf("get_bus_levels", { out: "f32", len: 26 }, 26);
r.value;     // the count filled
r.out[0];    // a Float32Array of the last block's per-channel peaks

const mesh = await engine.invokeBuf("scene_add_dynamic_mesh",
                                    { f32: verts }, 4, { i32: tris }, 2, material);
```

It does **not** lay out structs, and that is deliberate. JavaScript has no `offsetof`, so a caller
building a `bwa_fdn_desc` by hand would hard-code field offsets a header change could move without
a word. That is the reason `src/bwa_web.c` exists at all. An array of one scalar type is a different
thing, because `float[3]` is three floats in every ABI. A struct-taking call needs a field by field
wrapper in `bwa_web.c`, not a byte buffer from here. Three calls are out of reach until one is
written: `bwa_fdn_config`, `bwa_reflections_config` and `bwa_apply_tuning`.

## Where the audio comes from

There is no synchronous file system in a browser, so `bwa_load_sound` has nothing to open unless
you put a file in the wasm file system first. The route this binding takes instead is the one
`docs/web.md` recommends: decode in JavaScript with `AudioContext.decodeAudioData`, which is the
browser's own optimized decoder, and feed the float samples to a **push source**. No new ABI, and
it handles the formats a page actually gets served.

Pace the feed on the ring's own space (`src.space()`), not on the animation frame: a background tab
gets fewer frames and the audio thread does not slow down with it.

## Playground

`playground/index.html` is the browser port of `examples/playground.cpp`: the engine's demo scenes,
with three.js visuals and the binaural AudioWorklet sink. It ships in the Pages artifact beside the
minimal example, at `/playground/`.

```
node bindings/web/example/serve.mjs      # then open http://localhost:8181/playground/
```

Six scenes, each self-contained and switchable from the panel:

| scene | what it shows |
|-------|---------------|
| Localization | a click orbiting your head, draggable, with the panner, spread, dual band, Doppler and air absorption knobs |
| Channel walk | `bwa_set_test_signal` on one bus channel at a time, with the 26 speaker gizmos lit from `bwa_get_bus_levels` |
| Occlusion and materials | a wall as a dynamic mesh with a material, and the ray tracer's own occlusion factor |
| Directivity | the weighted-dipole patterns, with `bwa_source_get_directivity` under the drawing of the lobe |
| Medium boundary | the underwater interface loss, the muffle, the speed of sound and the surface's inverted bounce |
| Blind A/B/X | two settings of one knob, a hidden X, and a one-sided binomial p-value |

The default profile is `BWA_PROFILE_CAVE_SIM`, because the playground is the CAVE auditioned: every
point source pans through the real DBAP, SPCAP or VBAP solve into the 26-channel bus, and the bus is
HRTF-decoded to stereo. So a lit cone is a channel the panner really solved for.
`BWA_PROFILE_BINAURAL` is in the picker beside it and rebuilds the engine, which is a create-time
change; your AudioContext survives it. The channel walk forces CAVE_SIM for itself, because the
binaural profile has no bus to walk.

### The coordinate seam

The room frame is `+z` ahead, `+y` up and therefore `+x` LEFT (`BWA_ROOM_*`). three.js uses the
same three axes, so the conversion is the identity and `playground/frame.js` is the one place that
says so. The half that is not free is the CAMERA: the default view stands BEHIND the listener, at
room `-z` looking toward `+z`, so that a source on the listener's left draws on the left of the
screen as well as sounding there. `tests/run-playground.mjs` asserts both halves at once.

### What is not in it

The reverb bed scene the native playground has. Enabling a late reverb is `bwa_fdn_config` or
`bwa_reflections_config`, both struct-taking calls this binding does not marshal (see `invokeBuf`
above). A scene picker must not offer something that does nothing, so it is absent rather than
empty. The medium boundary scene says the same thing in its own panel: it has the boundary and the
medium, and no room tail.

### No layout file, and why none is needed

There is no synchronous file system in a browser and this binding cannot write into the wasm one:
`FS` is not in the module's `EXPORTED_RUNTIME_METHODS`, so `bwa_desc.layout_path` has nothing to
open. It does not matter here. The engine's default grid IS the geometry `examples/cave_layout.json`
describes, speaker for speaker: a 3 by 3 by 3 boundary grid at plus or minus 1.5 m with `y` at 0,
1.5 and 3, minus the center, 26 in all. The only thing the file adds is the measured per-speaker
delay trim, which nothing in a browser demo can hear. The page reads the positions back with
`bwa_get_speakers` rather than assuming them, so the gizmos are the engine's layout whatever it
turns out to be.

### three.js

Vendored, never linked from a CDN: under COEP `require-corp` a cross-origin script without
`Cross-Origin-Resource-Policy` is blocked, not slowed. `tools/wasm/fetch-web-vendor.sh` fetches a
pinned three.js (0.186.0) into `dist/vendor/three/` and checks each file against a sha256 recorded
in the script, and `tools/wasm/build-web.sh` calls it. `dist/` is a build output, so the repository
carries no 2 MB file and the Pages artifact still gets one. The script needs only curl, so a
developer with no Emscripten toolchain can still fill `vendor/`. There is no OrbitControls addon:
the orbit is 30 lines in `world.js`, which is one fewer file to pin.

## Tests

```
ctest --test-dir <build> -R web_bindings
```

or directly:

```
node --test bindings/web/tests
```

They run against the module that was just built, under node, with the null and manual sinks: the
generated table against the module's real exports, engine lifecycle and the commit model on the null
sink, and a manual-sink render that pins laterality and the frame slab against real samples.

The AudioWorklet sink needs a browser, so it has its own check:

```
ctest --test-dir <build> -R web_browser
node bindings/web/tests/run-browser.mjs            # or directly
```

It serves the repo cross-origin isolated, drives `tests/browser.html` in headless Chromium, and
waits for the page to post its verdict back. It skips when it finds no browser. What it proves: the
async setup chain, the node connect, `process()` pulling the adapter on the AudioWorklet thread, the
render paced by the audio clock, and the suspended-to-running handoff in both directions. It also
drives the `"worker"` topology on the null sink, which is the only place that path runs at all:
node has no `Worker`, so the module Worker, the postMessage protocol and the frame slab across a
thread boundary are browser-only. What it cannot prove is that it sounds right; nobody has listened
yet.

The playground has its own, in the same shape:

```
ctest --test-dir <build> -R web_playground
node bindings/web/tests/run-playground.mjs         # or directly
```

`web_browser` proves the sink. This proves the demo on top of it. The runner's server appends one
script tag to the playground's HTML for a request carrying `?__drive=1`, so the page a visitor loads
carries no test code and the check still drives the real page through `window.__bwaPlayground`, the
hook `playground/main.js` documents. What it asserts: the page starts on the worklet sink with 26
bus channels read back from `bwa_get_speakers`; a source at room `+x` draws on the left of the
screen AND is louder in the left ear, measured by rendering the page's own click through a second
engine on the manual sink (`playground/probe.js`); the channel walk lights the channel it drove and
nothing else within 20 dB; a wall across the line of sight drops the occlusion factor and moving it
away restores it; a figure-of-eight source nulls at 90 degrees; and the profile rebuild lands back
on the worklet sink and renders. It skips when it finds no browser, no `dist/` or no vendored
three.js.

The stimulus is the click, never DC. CLAUDE.md's first trap is a DC-driven laterality assertion that
shipped a left and right mirror.
