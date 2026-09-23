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
| `files` | `{path: Uint8Array}` written into that file system before `bwa_create` runs |
| `module` | `"main"` topology only: an engine module this page already instantiated |

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
| `writeFile(path, bytes)` | promise | put a file where the engine can open it. See below |
| `module` | object | `"main"` topology: the instantiated module, to reuse with `create({module})` |
| `outputNode()` | sync | the sink's `AudioWorkletNode`, or null. See below |

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

### Files the engine opens

Two `bwa_desc` fields are PATHS: `layout_path` and `hrtf_path`. The engine opens them with an
ordinary `fopen` inside `bwa_create`, and a browser has no synchronous file system for that to
reach, so the only path that can ever resolve is one inside the module's own in-memory file system.
The module exports `FS` for that reason, and the binding wraps it:

```js
const bytes = new TextEncoder().encode(await file.text());
const engine = await BwaEngine.create({
  audioContext: ctx,
  files: { "/cave_layout.json": bytes },     // written BEFORE bwa_create
  layoutPath: "/cave_layout.json",
});
```

Use `files` for those two, not `engine.writeFile(path, bytes)`. `create` opens the layout inside its
own call, so a write afterwards is too late. `writeFile` is for anything a RUNNING engine opens
later, and it works in both topologies (the bytes travel to the control thread with the message).

The file lives in the module's heap and dies with the module, so mind what you write: a layout, a
stimulus loop, a sound effect. Not a sample library and not an hour of music, which is what
streaming exists for and what a browser has no answer to yet. See "Where the audio comes from".

A layout the engine rejects does not fail `create`. The engine stays usable on the default grid and
`bwa_start` then refuses with `BWA_ERR_LAYOUT`, carrying the reason through `bwa_last_error`, so the
throw a page has to catch and show comes from `start()` and not from `create()`.

### Tapping the output

`outputNode()` hands back the `AudioWorkletNode` the sink built, once its asynchronous setup chain
has, so a page can meter, record or route what it is really playing. Web Audio offers no tap on a
destination, and `bwa_set_output_capture` is not bound in any layer because its callback would run
on the audio thread. Connecting to the node is additive - the sink's own connection to the
destination stays - so a tap cannot mute the page:

```js
const node = engine.outputNode();            // null until the setup chain finishes
const split = ctx.createChannelSplitter(2);
node.connect(split);                         // beside the sink's own connection, not instead of it
```

It is `"main"` topology plus `SinkType.WORKLET` only. Everything else has no node to give.

## Where the audio comes from

There is no synchronous file system in a browser, so `bwa_load_sound` has nothing to open unless
you put a file in the wasm file system first. There are two routes, and the first one is the
default.

**A sound the engine owns.** Get float samples any way you like, wrap them in a wav header, write
the file with `engine.writeFile` and load it with `bwa_load_sound`. The engine then owns the
samples: it loops them sample accurately, and the page is not in the audio path at all.

```js
await engine.writeFile("/stim/click.wav", wavBytes);       // any time, not only before create
const snd = await engine.invoke("load_sound", "/stim/click.wav");
await src.play(snd, true);                                 // looped by the audio thread
```

The browser's own decoder gets you there from any format it serves: `decodeAudioData`, fold the
channels down to the mono a point source takes, re-wrap as a wav. `playground/wav.js` does the
wrapping, in one function; the engine reads wav, flac and mp3 through dr_wav and resamples at
load.

**A push source**, for audio the page makes or receives as it goes: a synthesizer, a microphone, a
network stream. The engine's one inbound exception, and a source feed rather than a render path.
`example/index.html` is that demo.

Prefer the first for anything you have in full before it starts, and that is not a style
preference. A push feed keeps the page in the audio path forever, and a page stalls: a garbage
collection, a window drag, a heavy frame on a weak GPU, a tab losing focus. Any stall longer than
what you queued ahead is a hole in the sound, and the engine counts it as a stream starve
(`bwa_health.streamStarves`). The playground fed its stimuli that way and the holes were audible
(2026-09-22); it loads them now, and the same 400 ms stall makes no sound at all.

If you do push, pace the feed on the ring's own space (`src.space()`), not on the animation frame:
a background tab gets fewer frames and the audio thread does not slow down with it. And pace it
rather than fill it: the ring holds 1.37 s, all of which has to play before anything new is heard.

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
change. The channel walk forces CAVE_SIM for itself, because the binaural profile has no bus to
walk.

A rebuild replaces the AudioContext too, and that is not tidiness. Emscripten's audio-worklet
bootstrap runs `audioWorklet.addModule()` and registers its processors by name in the context's own
worklet scope, so a second engine on the same context registers the same name twice, which throws.
The setup chain then never finishes, no node is ever connected, and the sink does what rule 4 says
for a device that is not there: it host-paces silence. The engine goes on rendering and the page
goes quiet. A fresh context is a fresh worklet scope. The wasm module is reused across the rebuild
(`create({ module })`), so the switch costs no second copy of the engine.

### The meters

Two, because the two profiles put the audio in different places.

The speaker cones and the bus strip are `bwa_get_bus_levels`, sampled on a 4 ms interval rather
than on the animation frame. That readback is the LAST BLOCK's peak, a block is 5.3 ms, and the
page used to read it every 120 ms: it saw one block in 22 and the default stimulus is a click train
whose burst is 2 ms in every 250 ms, so the meter read "silent" and the cones stayed dark while the
click was plainly audible. The tick holds the peak and the frame loop consumes it. The strip then
falls at the rate the cones and the output strips fall at, because a click's peak lands in about
one animation frame in fifteen and an instantaneous strip reads silent between them.

The two output strips are the stereo the AudioContext is playing, read from an AnalyserNode pair on
`engine.outputNode()`. In `BWA_PROFILE_BINAURAL` the point voices bypass the bus entirely, so the
bus meter there shows only the diffuse field and the panel says so; the output strips are the ones
with your ears' content in them.

### The stimuli are loaded sounds, not a feed

Every stimulus the page plays is a fixed buffer: the click train's 250 ms period, two seconds of
pink noise, the bursts, the tone (`playground/stimulus.js`). Each one is encoded as a float32 wav
once, written into the module's file system and loaded with `bwa_load_sound`, and one ordinary
source plays it with `loop = true`. A stimulus change is one `bwa_source_play`, which the engine
ramps click free over its first block. Your own clip goes the same way: the picker under the
stimulus menu decodes it with the browser, folds it to mono and loads it.

It used to feed a push source from a 20 ms timer on the main thread with 100 ms queued ahead of
the audio clock, and that was wrong in both directions. A stall longer than the queue was a hole
in the sound, which is what "the click train becomes inaudible for short periods" was (reported
2026-09-22; headless Chrome on a fast desktop collected 31 to 37 stream starves in one ordinary
check run before anyone stalled anything on purpose). And the queue was also the switch latency: a
menu change waited for the old signal to drain.

Both numbers moved. A stimulus switch is heard after 6 to 16 ms, where the feed took 97 to 170. A
deliberate 400 ms main-thread stall now produces zero starves, where the feed produced 45 to 59. The health table keeps
one row for it, `stream starves`, and it stays at zero because nothing on the page feeds the
engine any more. If you hear a dropout now, it is the audio thread, and the `late blocks` row is
where it shows. `run-playground.mjs` asserts both halves: the switch bound, and the stall making
no starve while the audio thread renders straight through it.

### The layout upload

The panel takes a `cave_layout.json` (the format is [docs/layout-schema.md](../../docs/layout-schema.md),
and `examples/cave_layout.json` is the reference) and rebuilds the array on it. The page checks the
three mistakes anyone makes first - not JSON, a count outside 4 to 26, a position that is not a
number - and the engine is the authority for the rest: a file it refuses is reported with its own
`bwa_last_error` text and the page falls back to the default grid, because an engine whose
`bwa_start` refused is an engine nobody can hear. The cones are placed from `bwa_get_speakers`
afterwards, so what is drawn is what the engine loaded. The schema carries no per-speaker
orientation, so the cones keep aiming at the array's nominal listening point.


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

### The default layout

With no file uploaded the engine runs its default grid, which IS the geometry
`examples/cave_layout.json` describes, speaker for speaker: a 3 by 3 by 3 boundary grid at plus or
minus 1.5 m with `y` at 0, 1.5 and 3, minus the center, 26 in all. The only thing the file adds is
the measured per-speaker delay trim, which nothing in a browser demo can hear. The page reads the
positions back with `bwa_get_speakers` rather than assuming them, so the gizmos are the engine's
layout whatever it turns out to be.

### three.js

Vendored, never linked from a CDN: under COEP `require-corp` a cross-origin script without
`Cross-Origin-Resource-Policy` is blocked, not slowed. `tools/wasm/fetch-web-vendor.sh` fetches a
pinned three.js (0.186.0) into `dist/vendor/three/` and checks each file against a sha256 recorded
in the script, and `tools/wasm/build-web.sh` calls it. `dist/` is a build output, so the repository
carries no 2 MB file and the Pages artifact still gets one. The script needs only curl, so a
developer with no Emscripten toolchain can still fill `vendor/`. There is no OrbitControls addon:
the orbit is 30 lines in `world.js`, which is one fewer file to pin.

## XR

`xr/index.html` is the playground with your head in it. Same engine, same scenes, same 26-channel
bus. What it adds is the one thing headphones and a mouse cannot show you: in an `immersive-vr`
session the viewer pose from every XR animation frame becomes the engine's listener pose, position
and orientation both, so the binaural render follows your head and a source stays where it is in
the room while you turn.

```
node bindings/web/example/serve.mjs      # then open http://localhost:8181/xr/
```

It has three states and says which one it is in.

| state | what you get |
|-------|--------------|
| no WebXR | a note saying so, a link to the flat playground, and a flat preview that still starts the engine |
| WebXR, no session | a normal three.js view, an Enter VR button, and the menu as DOM |
| in session | stereo through the headset, the menu as a panel in the room, and the head tracking |

The Enter VR click is also the user gesture that lets the page resume its `AudioContext`, so the
engine starts there. `rig.js` is the playground's, unchanged: the worklet sink, the source, the
looped stimuli, and `BWA_PROFILE_CAVE_SIM` by default with `BWA_PROFILE_BINAURAL` in the picker.

### The coordinate seam, again, and the other half of it

`playground/frame.js` owns the room to three.js map and finds it to be the identity.
`xr/frame_xr.js` owns the half the flat page never needs: the XR reference space to room map, for a
position and for an orientation.

Work it out from the two conventions. The room frame is `+y` up, `+z` ahead and `+x` LEFT. A WebXR
viewer is `+y` up, `-z` ahead and `+x` right. So the head-local basis map sends room ahead to `-z`,
room up to `+y`, and room `+x` to `-x`, which is `diag(-1, 1, -1)`.

**That is a rotation, not a mirror.** Its determinant is `+1`. It is 180 degrees about the up axis.
The tempting reading, that `+x` LEFT against `+x` right means a reflection across the YZ plane, is
wrong, and it is wrong in the direction a static test never catches: a reflection and a yaw agree
about where a source SITS and disagree about which way a head is FACING. The handedness flip a
reflection would introduce is already spent by the second sign, the one on the forward axis,
because the two conventions disagree about ahead as well as about left. Two sign flips compose to a
proper rotation.

The space map is the same rotation, by choice, so that the direction a user already faces when the
session starts becomes room ahead. With a `local-floor` reference space, XR `y = 0` is the physical
floor and room `y = 0` is the room floor, so the vertical axis needs no offset at all. Composing
the two gives `q_room = qY * q_xr * qY` with `qY = (0, 1, 0, 0)`, which multiplies out to
`(x, y, z, w) -> (x, -y, z, -w)`. An identity XR orientation is an identity room orientation.

`tests/xr_frame.test.mjs` checks all of that under node, against the ABI's own basis vectors rather
than against the algebra, so a mis-derivation fails whatever it wrote. `world_xr.js` applies the
same rotation to the three.js group that carries the XR camera, and the test pins the two together.
If they ever disagreed, the picture and the sound would disagree about where you are.

### The menu is a panel in the room

The WebXR DOM Overlay module is specified for handheld AR, and every shipping runtime grants it
only on an `immersive-ar` session. Ask for it on `immersive-vr` and either the feature is refused
or `session.domOverlayState` comes back undefined, which would leave a headset user looking at a
menu that exists only on a screen behind them. So the page asks for `dom-overlay` as an optional
feature, uses the DOM menu if the runtime says it granted one, and otherwise paints an in-world
panel: a plane at arm's length, facing you, with a canvas texture, selected by a controller ray.

Both renderers take the same control descriptors. `playground/ui.js` turns them into DOM and
`xr/panel.js` paints them, so the option set is the flat playground's option set and a scene's own
knobs reach XR with no XR code in the scene.

### Controllers

| input | what it does |
|-------|--------------|
| trigger, on the panel | press the control the ray is on |
| trigger or grip, elsewhere | grab the source with the nearer hand and carry it |
| thumbstick X | slide the source left and right, relative to where you are looking |
| thumbstick Y | push the source away, or pull it in |
| grip and thumbstick Y | raise and lower the source |
| thumbstick Y, on the panel | scroll the menu |

A press is resolved in that order, because a ray that lands on the panel is unambiguous and a grab
is not. The page prints the table beside the view.

**A phone viewer has no controller.** A Cardboard-style runtime reports one input whose
`targetRayMode` is `gaze`, with the ray starting at the eyes, and a tap on the screen as a
one-frame `screen` input. Both press. Neither is a hand, and the first build drew the gaze as
one, which put the grip sphere on the user's face. Now a gaze draws nothing but a 1.2 cm dot at
the end of its ray, and only while that ray is on the menu; a screen tap draws nothing.

The menu changes its follow policy for a gaze as well. The controller policy parks the panel
0.34 m to the left of the gaze line and moves it with every head turn, which a hand can reach
and a gaze never can (the panel is 0.52 m wide, so the gaze missed it by 8 cm, always). With a
gaze the panel is a thing in the room: centered ahead, a little below eye height, held there
while you look within 40 degrees of it, and re-parked in front of you over about half a second
once you look further away. The mode switches the moment the set of inputs changes, so a panel
a controller left to one side comes into view at once. `run-xr.mjs` pins all three states with
a fake gaze input: on the menu looking ahead, off it at 30 degrees, and on it again after a look
straight up.

### Pose prediction, and why the page does it

`bwa_set_pose_prediction` leads the pose the engine's OWN tracker publishes. `src/core/rt.c` reads
it inside the branch that follows a NatNet connection, so a pose pushed through
`bwa_set_listener_pose` never passes through it, and on this page that call is inert. The lead has
to happen where the poses are, which is the page. `xr/frame_xr.js` carries a `PoseLead` that copies
the engine's own shape: an exponential average over about 100 ms, a speed cap, and a reset across a
gap.

WebXR already returns a viewer pose predicted to the frame's DISPLAY time, so the eyes are taken
care of and the knob here is the extra distance to the EARS, which is the output latency plus one
block. The default is 30 ms and the slider runs to 80. Orientation is not led, because the engine
does not lead it either and an overshooting head rotation is far more audible than an overshooting
position.

### What it shares, and what it duplicates

It imports `playground/rig.js`, `playground/scenes/`, `playground/stimulus.js`, `playground/ui.js`
and `playground/world.js` (by subclass), so the audio, the scenes, the gizmos and the control
builder are one copy. Three things are duplicated, and each is marked in the file that carries it:

- the global control descriptors (scene, profile, stimulus, master gain) are re-spelled in
  `xr/options.js`, because `playground/main.js` builds them inline and exports nothing.
- `xr/probe.js` is `playground/probe.js` with a head ORIENTATION argument added. The playground's
  version hard-codes an identity listener quaternion, which is right for a page whose head never
  turns and useless here.
- the page's CSS, because neither page has an external stylesheet.

### Tests

```
ctest --test-dir <build> -R web_xr
node bindings/web/tests/run-xr.mjs                               # or directly
node bindings/web/tests/run-xr.mjs --site bindings/web/_site     # or against a staged site
```

It loads the page twice, because two of the three states are decided before any injected script
could change them. The first pass deletes `navigator.xr` and checks the no-WebXR state. The second
installs a minimal fake one, so an `immersive-vr` session opens against a synthetic `XRFrame` whose
viewer pose the check chooses.

What the fake proves: the seam, in both signs, on every axis; that the pose reaches the engine,
read back through `bwa_get_listener_pose`; that a source the listener looks at renders centered
while the same source with the head turned a quarter renders hard to the other ear, measured
through a second engine on the manual sink; the prediction lead, in meters; the in-world panel
including its ray hit test; the grab; and the thumbstick nudge. What it cannot prove: three.js
stereo rendering, the projection layer, a real runtime's controller poses or gamepad layout,
reprojection, and latency. Those need a headset, and nobody has worn one yet.

The seam itself also has a node test with no browser in it, which runs inside `web_bindings`:

```
node --test bindings/web/tests/xr_frame.test.mjs
```

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
bus channels read back from `bwa_get_speakers`, and the Start panel is gone with a status line in
its place; a source at room `+x` draws on the left of the screen AND is louder in the left ear,
measured by rendering the page's own click through a second engine on the manual sink
(`playground/probe.js`); the speaker cones and both meter strips MOVE while the default click train
plays; a stimulus change reaches the output inside 60 ms; the channel walk lights the channel it
drove and nothing else within 20 dB; a wall across the line of sight drops the occlusion factor and
moving it away restores it; a figure-of-eight source nulls at 90 degrees; a 400 ms main-thread
stall makes no stream starve and the output still carries the stimulus afterwards; the profile
rebuild lands
back on the worklet sink, keeps `device_lost` at 0 and still puts a `+x` source in the left ear of
the LIVE output, in both directions; and an uploaded layout rebuilds the array, reads back through
`bwa_get_speakers` with the position it was given, while a layout the engine refuses is reported and
falls back to the default grid. It skips when it finds no browser, no `dist/` or no vendored
three.js.

The live-output assertions read an AnalyserNode pair on `engine.outputNode()`, and they are there
because a health block cannot tell silence from sound: a sink whose worklet never came up
host-paces silence and counts blocks exactly like a working one. The offline probe cannot tell
either, because it renders its own engine.

The stimulus is the click, never DC. CLAUDE.md's first trap is a DC-driven laterality assertion that
shipped a left and right mirror.
