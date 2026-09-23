# bw_audio for Python

Python binding for the bw_audio spatial audio engine: a speaker array over ASIO (up to 64 channels; the CAVE starts with 24), and a
binaural headphone render. Built with [nanobind](https://github.com/wjakob/nanobind) on the C ABI in
[`include/bw_audio.h`](../../include/bw_audio.h).

Links below are relative to this file in the repository,
[github.com/aforren1/cave-audio](https://github.com/aforren1/cave-audio), under
`bindings/python/`. Resolve them there if you are reading this from a wheel's metadata rather than
from a checkout.

It exists for two experiment shapes, described in
[docs/backends.md](../../docs/backends.md#experiments-from-psychtoolbox-or-psychopy). PsychoPy is
Python, so it uses this package directly. Psychtoolbox is MATLAB and gets its own MEX binding later.

```python
import time
import bw_audio as bwa

with bwa.Engine(profile=bwa.Profile.BINAURAL) as engine:
    click = engine.load_sound("click.wav")
    source = engine.create_source()
    source.set_pos(1.0, 1.5, 2.0)
    source.play(click)
    while source.is_playing():   # leaving the block closes the engine, which cuts the sound
        time.sleep(0.01)
```

## Install

From a wheel:

```
uv pip install bw_audio-0.15.0-cp312-abi3-win_amd64.whl
```

Wheels cover **Python 3.10 and later** on all three desktops. A release carries nine of them: three
per platform, named by the Python they serve.

| Python | Tag |
| --- | --- |
| 3.10 | `cp310-cp310-<platform>` |
| 3.11 | `cp311-cp311-<platform>` |
| 3.12 and every later version | `cp312-abi3-<platform>` |

The 3.12 wheel is built on the stable ABI, so one file serves 3.12, 3.13, 3.14 and what comes next.
3.10 and 3.11 predate that limited API and each needs its own file. Pick the file that matches your
interpreter and your platform.

Each platform tag is built for more than the machine that built it:

| Platform | Tag | Runs on |
| --- | --- | --- |
| Windows | `win_amd64` | Windows x64 |
| Linux | `manylinux_2_28_x86_64` | glibc 2.28 or newer: RHEL 8, Debian 10, Ubuntu 18.10 and later |
| macOS | `macosx_10_13_universal2` | Intel and Apple silicon, one file |

The wheel contains the engine library, so there is nothing to install beside it. Steam Audio is
linked statically into the engine, so there is no second library either. The Linux filename can
name more than one tag (`manylinux_2_27_x86_64.manylinux_2_28_x86_64`): auditwheel records every
tag the wheel qualifies for, and the lowest one is what it needs. The Linux wheel imports with
neither `libasound.so.2` nor `libjack.so.0` on the machine: the engine loads each of them at run
time, so the JACK or ALSA backend is available when its library is installed, and unavailable with
the library named when it is not. Both come with any Linux desktop that plays audio (`libjack` also
arrives with `pipewire-jack`).

CI also builds a wheel on each runner with plain `uv build --wheel`, one per job. Those are the
**fast gate**, not the release: they are tagged for the runner image's own glibc and architecture,
and for the one interpreter the step pins. The wheels a release ships are built with cibuildwheel,
once per Python, in a manylinux container on Linux and for both architectures at once on macOS.
See [docs/build.md](../../docs/build.md#continuous-integration).

From source, in a checkout of this repository:

```
cd bindings/python
uv build --wheel
uv pip install dist/bw_audio-*.whl
```

A source build needs CMake 3.20 or later and a C and C++ compiler. It builds the engine too, so it
picks up whatever the checkout has: the ASIO SDK at `third_party/asiosdk/` and the staged Steam
Audio at `third_party/steam-audio-artifacts/`. See [docs/build.md](../../docs/build.md).

A source distribution is not published. The build needs the whole repository, not just this
directory, so build a wheel or work from a checkout.

## The two layers

**`bw_audio._bwa` is the raw layer.** One function for each C entry point, named by its C name
without the `bwa_` prefix, so `bwa_source_play` is `_bwa.source_play`. Enums keep their C spelling
without the type prefix, so `BWA_PROFILE_CAVE` is `_bwa.profile.CAVE`. Structs keep their C field
names. Nothing is renamed and nothing changes units. Read
[`include/bw_audio.h`](../../include/bw_audio.h) or [docs/api.md](../../docs/api.md) and you are
reading this layer.

Three places where a literal binding cannot express the C:

- Out parameters become return values. A call that returns a boolean plus an out struct returns the
  value on true and `None` on false, so `get_health()` returns `None` when the counters cannot mean
  anything. A pair of out pointers returns a tuple, so `get_clock()` returns
  `(dsp_sample, host_time_ns)`.
- Buffers are numpy arrays. `render_block` hands back a read-only view of the engine's own memory,
  and `source_push` takes a float32 array.
- The engine pointer lives in a wrapper object that `destroy` empties, so a call after destroy
  raises instead of reading freed memory.

**`bw_audio` is the Pythonic layer.** Pure Python over the raw layer. It gives you an `Engine`
context manager, `Sound`, `Source`, `PushSource`, `Bed`, `Listener` and `ClockBridge` objects,
exceptions instead of return codes, and the one-control-thread guard below. It adds no semantics of its own, so when
a method's behavior is in question, the header comment is the answer. It adds one semantic and
only one: it commits for you. See "Commit model" below.

Mix the layers freely. `Engine.raw` is the raw handle and `Source.handle` is the raw source, so
anything the Pythonic layer does not wrap you can still call:

```python
from bw_audio import _bwa

_bwa.set_hole_spread(engine.raw, 1.0)
```

### What is not bound

`bwa_set_output_capture` is excluded on purpose. Its callback runs on the **audio thread**, and an
interpreter must never run there: taking the GIL inside a device callback allocates, blocks on a
lock, and runs arbitrary bytecode, which breaks the engine's first invariant. The offline path is
the manual sink and `Engine.render_block()`, which hands you the same post-limiter samples on your
own thread. This is the rule [docs/backends.md](../../docs/backends.md) states for this binding by
name.

Everything else in the ABI is bound. A test parses the header and fails if a new call arrives
unbound.

## The threading contract

All `bwa_*` calls must come from **one control thread**. The command ring between the control
thread and the audio thread is single-producer, so a second producer breaks it. That is the
engine's contract, not Python's, and no amount of locking on the Python side fixes it.

`Engine` records the thread that created it and raises `BwaError` from any other thread:

```python
engine = bwa.Engine()          # guarded, the default
engine = bwa.Engine(thread_guard=False)   # you funnel your own calls through one thread
```

The raw layer has no guard. One other rule travels with the contract: every per-frame call is
non-blocking. It encodes a command and returns.

The GIL is released around the calls that block or do file I/O, which is the lifecycle, the loads,
the headphone EQ, the tracker connect, the scene rebuilds, and `render_block`. Another Python
thread runs while those are in flight. Per-frame calls keep the GIL, because they are non-blocking
ring writes and releasing would cost more than the call.

## Commit model

Position and pose are **commit-gated** in the ABI. They write pending fields, and only
`bwa_commit` promotes them as one coherent snapshot. Forget the commit and everything renders at
its last committed position: sounds play, nothing moves.

Unity and Godot never make you think about it, because both have a frame: their bindings push
every position, then commit once per frame, and your code never calls commit at all. Python has no
frame to hang that on, so this binding gives you three modes.

**Autocommit, the default.** Every commit-gated write commits itself. Nothing to remember, and a
script that sets a position and plays a sound is correct with no ceremony.

```python
source.set_pos(1.0, 1.5, 2.0)      # committed for you
source.play(click)
```

**A frame block, for a loop.** `Engine.frame()` groups a frame's writes into one commit, taken at
the block's exit. This is the coherence path, and the one to use in a game-style loop or around
any multi-source update whose intermediate state must not be rendered: two sources that move
together are then rendered as having moved together, rather than one per commit.

```python
while running:
    with engine.frame():
        for source, position in scene:
            source.set_pos(*position)
        engine.listener.set_pose(*head)
```

Blocks nest and commit once, at the outermost exit. An exception leaving the block still commits:
a half-pending scene outlives the exception and renders at mixed positions from then on, which is
a worse failure and a harder one to read. The exception propagates unchanged.

A play call inside a block flushes a pending write FIRST, even in a block, so "set the position,
play it" always sounds where you put it. `Source.play`, `play_at`, `play_loop` and `queue`, the
three `Bed` play calls, and `Engine.play_oneshot` are the flush points. `PushSource.push` is not:
a push is a per-frame feed rather than a play, and a loop that pushes every source would commit on
the first push and land the rest of the frame's positions late.

**Autocommit off, for your own frame loop.** `Engine(autocommit=False)`, or the `autocommit`
property, gives the C semantics back: nothing commits but `Engine.commit()`, a frame block's exit
included. Use it when you drive the loop and want exactly one commit per iteration.

Three things are worth knowing whichever mode you use. `Engine.commit()` is always there and
always `bwa_commit`. Only position and pose are gated, so gain, spread, orientation and the rest
land on the next audio block by themselves and get no commit. And commit is what fills the event
rings, so `poll_ended()` and `poll_looped()` read a frame-old picture without one ahead of them.

**The raw layer does none of this.** `bw_audio._bwa` is the C semantics exactly: no pending flag,
no autocommit, no flush. Mixing the layers is fine, but a raw `_bwa.source_set_pos` stays pending
until you commit.

## The offline shape

The manual sink creates no device and no audio thread. You pump one block at a time on your own
thread, the clock is a pure sample counter, and a fixed input with a fixed call sequence renders
bit-identically every run. That is what makes it the basis for stimuli you pre-render and for
golden tests.

```python
import numpy as np
import bw_audio as bwa

engine = bwa.Engine(profile=bwa.Profile.BINAURAL, sink=bwa.SinkType.MANUAL)
source = engine.create_push_source()
source.set_pos(2.0, 1.5, 0.0)
engine.start()

blocks = []
for b in range(200):
    source.push(my_stimulus[b * 256:(b + 1) * 256])
    blocks.append(engine.render_block(copy=True))   # copy=True: the view dies at the next call
engine.close()

audio = np.concatenate(blocks, axis=1)              # (channels, frames), float32
```

`render_block()` returns a **read-only view of the engine's own buffer**, valid until the next
`render_block()` or `stop()`. Pass `copy=True` when you keep it.

The async Steam Audio sims (occlusion, reflections, pathing) are wall-clock timed and are **not**
reproducible. Keep a golden render on the synchronous DSP, or on the manual occlusion path.

[`examples/offline_render.py`](examples/offline_render.py) is this shape end to end, including
writing a wav with numpy alone.

Before either shape, run [`examples/minimal.py`](examples/minimal.py). It orbits a click around the
listener's head for six seconds and quits, which is the smallest realistic client and the fastest
way to hear that the binding works. Every other binding of this engine ships the same demo with the
same stimulus, so the four are comparable by ear. See `docs/integration.md`, "The minimal example".

## The live shape

The engine owns the device and you schedule onsets ahead of time. Wall time and the dsp-sample
clock are different clocks, so `Engine.bridge()` measures the offset between them and maps one to
the other:

```python
clock = engine.bridge()                  # measures now; default caller clock is time.perf_counter
print(clock.error_ns)                    # the bound on that measurement, in nanoseconds

clock.play_at(source, sound, t_event)    # heard at t_event: it takes off the output latency
dsp = clock.dsp_at(t_event)              # or do it yourself
t = clock.time_at(dsp)                   # the inverse
```

The bridge sandwiches `bw_audio.host_time_ns()`, which reads the engine's own host clock, between
two reads of yours. The error is bounded by half that round trip, reported as `error_ns`, and there
is no convergence period: one `refresh()` is a measurement, not an estimate. `drift_ppm` is the
device clock against the host clock, for a session long enough to care.

Pass your own clock when the experiment already has one: `engine.bridge(clock=psychopy_timer)`.
The default, `time.perf_counter`, IS the engine's clock on every platform, so its offset never goes
stale. `time.monotonic` is a different clock on Windows with about 15.6 ms of granularity, so a
sandwich around it measures that granularity: do not use it. Psychtoolbox `GetSecs` matches on
Windows and macOS but is `CLOCK_REALTIME` on Linux, which NTP steps, so re-measure per trial there.
The bridge does not police your clock, and any clock works. The per-platform table is in
[docs/api.md](../../docs/api.md#land-a-sound-on-a-visual-event).

`play_at` subtracts the ENGINE's render-to-DAC delay only. It cannot see your display delay, or any
delay past the DAC in an amplifier or a wireless headphone: measure draw to photons once, with a
photodiode or an AV-sync clapper, and fold that constant into `t_event`. A worked version is in
[`examples/live_onset.py`](examples/live_onset.py).

When onset precision is the measurement, open the card directly rather than through a system mixer.
On Linux that means ALSA with a raw `hw:` device; on Windows it means ASIO, or WASAPI with
`SinkFlags.EXCLUSIVE`. `SinkFlags.EXACT_RATE` refuses a device that cannot run at the rate you
asked for, instead of accepting the OS resampler.

```python
for name, ident in bwa.list_devices(bwa.SinkType.WASAPI):
    print(name, ident)

engine = bwa.Engine(sink=bwa.SinkType.WASAPI, device="Headphones (Realtek Audio)",
                    sink_flags=bwa.SinkFlags.EXCLUSIVE | bwa.SinkFlags.EXACT_RATE)
```

Check `Engine.health` after a session. It returns `None` when this configuration cannot observe a
dropout at all, which is not a clean bill of health: a zero xrun count on its own cannot be told
apart from never having measured.

## Coordinates and units

Room space is right-handed, meters, +y up, +z forward, with the origin on the floor at the center
of the working area. Gains are linear. Angles are radians.

Time is the one quantity with two live units, so every time-valued name says which: a getter ends
`_frames` or `_seconds`, and a parameter is named `seconds` or ends `_s`. Distances, frequencies,
angles and gains have no competitor, so they carry the unit on the value (`radius_m`, `xover_hz`,
`yaw_rad`) and never on the call. A decibel value always says `_db`.

Positions take each axis in the range -1e6 to 1e6 meters. Outside that, or non-finite, the call is
a no-op and the previous position stands, so a bad frame reads as a dropped frame.

Full statement: [docs/api.md](../../docs/api.md#coordinates-and-units).

## Versions

One number. The package version is `BWA_VERSION_*` in `include/bw_audio.h`, read from the header
at build time, so a wheel can never claim a version the library is not. The release script writes
the git tag into that header before tagging, and CI refuses a tag that disagrees, so a wheel from
release `vX.Y.Z` is `bw_audio-X.Y.Z` and the Unity package and the Godot addon carry the same
number. `bw_audio.abi_version()` reports what the loaded library says,
`bw_audio.header_abi_version()` what the extension was compiled against, and `bw_audio.check_abi()`
raises when the two disagree on major.minor. See [docs/build.md](../../docs/build.md#releasing).

## Platform notes

**Windows.** Windows does not search a `.pyd`'s own directory for its dependent DLLs, so
`__init__.py` calls `os.add_dll_directory` on the package directory before importing the extension.
Nothing for you to do, but do not move `bw_audio.dll` out of the package. The release wheels are
not repaired for the same reason: a repair tool renames the libraries it vendors, and this package
needs the engine to keep its own name beside the extension.

**Linux and macOS.** The extension carries an RPATH of `$ORIGIN` or `@loader_path`, so it finds the
engine library beside it. The release wheels keep it: every library the wheel does not already
carry is a system one, so the repair step (auditwheel or delocate) bundles nothing and never
rewrites that path.

**Stable ABI.** Built with Python 3.12 or later, the wheel is tagged `cp312-abi3` and runs on 3.12
and every later Python, one file per platform. Built with 3.10 or 3.11, it is version-specific and
the configure step says so. That is not a fallback to apologize for: it is why a release ships
three wheels per platform instead of one.

**The floor is Python 3.10.** The pinned nanobind declares `Requires-Python >=3.10`, so a source
build on 3.9 cannot get its build dependency, and no wheel is built for it.

## Testing

```
cd bindings/python
uv run --extra test pytest
```

`uv run` builds and installs the project into its environment first, so pytest runs against the
installed package rather than the source tree.

The suite also runs under ctest as `python_bindings`, with the three examples as
`python_example_minimal`, `python_example_offline_render` and `python_example_live_onset`:

```
cmake -S . -B build -DBWA_BUILD_PYTHON=ON
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo -R python
```

That path needs nanobind and pytest importable by the interpreter CMake finds. It reports SKIPPED
rather than failing when pytest is missing, because a C developer should not need a Python test
framework.

Nothing in the suite touches a real device: every engine is the null or the manual sink. The golden
test drives the same scenario and asserts the same committed constants as `test/golden_test.c`, so
a binding bug that quietly changed a gain or a position cannot pass.

## Building the extension

`pyproject.toml` points scikit-build-core at the **repository root**, not at this directory, with
`BWA_BUILD_PYTHON=ON`. The root `CMakeLists.txt` resolves the ASIO SDK and the staged Steam Audio
against its own source directory, so anything that makes `bindings/python` the top-level project
would silently produce a wheel with no ASIO and no Steam Audio. There is one arrangement, and a
repository-root configure with `-DBWA_BUILD_PYTHON=ON` runs exactly the same code a wheel build
does.

### Against a prebuilt engine

A plain `uv build --wheel` compiles the whole engine inside its own isolated build directory. If
you already have an engine you want the wheel to carry, install it as an
[engine SDK](../../docs/build.md#the-engine-sdk) and point the build at it:

```
cmake -S ../.. -B ../../build -A x64
cmake --build ../../build --config RelWithDebInfo
cmake --install ../../build --prefix /path/to/sdk --component bwa_sdk --config RelWithDebInfo

uv build --wheel --python 3.12 -C cmake.define.BWA_ENGINE_SDK=/path/to/sdk
```

`uv build` forwards `-C` to scikit-build-core, which puts `cmake.define.*` on the CMake command
line. The root `CMakeLists.txt` then compiles nothing under `src/` and the wheel carries the
library out of that SDK. This is how CI builds every wheel it ships, so the file a user installs
is the file that platform's ctest run tested.

The package checks at import that the engine it loaded reports the same `BWA_VERSION` the
extension was compiled against, and raises `ImportError` naming both versions when it does not. A
wheel built from one tree cannot trip it; the guard is there for the SDK path.
