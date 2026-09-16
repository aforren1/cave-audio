# bw_audio for Python

Python binding for the bw_audio spatial audio engine: a 26-speaker CAVE array over ASIO, and a
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
import bw_audio as bwa

with bwa.Engine(profile=bwa.Profile.BINAURAL) as engine:
    click = engine.load_sound("click.wav")
    source = engine.create_source()
    source.set_pos(1.0, 1.5, 2.0)
    engine.commit()
    source.play(click)
```

## Install

From a wheel:

```
uv pip install bw_audio-0.14.0-cp312-abi3-win_amd64.whl
```

Releases carry one wheel per platform. The wheel contains the engine library, so there is nothing
to install beside it. Steam Audio is linked statically into the engine, so there is no second
library either.

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
context manager, `Sound`, `Source`, `PushSource`, `Bed` and `Listener` objects, exceptions instead
of return codes, and the one-control-thread guard below. It adds no semantics of its own, so when
a method's behavior is in question, the header comment is the answer.

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

The raw layer has no guard. Two other rules travel with the contract:

- Every per-frame call is non-blocking. It encodes a command and returns.
- Position and pose are commit-gated. Push them every frame, then call `Engine.commit()` once,
  last. Forget the commit and everything renders at its last committed position: sounds play,
  nothing moves.

The GIL is released around the calls that block or do file I/O, which is the lifecycle, the loads,
the headphone EQ, the tracker connect, the scene rebuilds, and `render_block`. Another Python
thread runs while those are in flight. Per-frame calls keep the GIL, because they are non-blocking
ring writes and releasing would cost more than the call.

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
engine.commit()
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

## The live shape

The engine owns the device and you schedule onsets ahead of time. Wall time and the dsp-sample
clock are different clocks, so map one to the other with the driver-stamped pair, subtract the
device's render-to-DAC delay, and schedule:

```python
pair = engine.clock                      # (dsp_sample, host_time_ns), stamped inside the callback
heard = wall_to_dsp(t_event)             # your wall clock -> a dsp sample
latency = engine.output_latency_frames   # render -> DAC
source.play_at(sound, max(0, heard - latency))
```

The engine reports its own output chain. It cannot see your display delay: measure draw to photons
once, with a photodiode or an AV-sync clapper, and that one constant aligns the whole chain. The
full recipe, including the drift-tracking offset estimator, is
[docs/api.md](../../docs/api.md#land-a-sound-on-a-visual-event); a Python version of it is in
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

Two version streams, and they move independently.

- **The package version is the ABI version**, `BWA_VERSION_*` in `include/bw_audio.h`, read from the
  header at build time. A wheel can therefore never claim a version the library is not.
  `bw_audio.abi_version()` reports what the loaded library says, `bw_audio.header_abi_version()`
  what the extension was compiled against, and `bw_audio.check_abi()` raises when the two disagree
  on major.minor.
- **The release version is the git tag**, the same one the Unity package and the Godot addon carry.
  It moves on its own cadence. See [docs/build.md](../../docs/build.md#releasing).

## Platform notes

**Windows.** Windows does not search a `.pyd`'s own directory for its dependent DLLs, so
`__init__.py` calls `os.add_dll_directory` on the package directory before importing the extension.
Nothing for you to do, but do not move `bw_audio.dll` out of the package.

**Linux and macOS.** The extension carries an RPATH of `$ORIGIN` or `@loader_path`, so it finds the
engine library beside it.

**Linux wheels are tagged for the building machine's glibc, not manylinux.** CI builds them on the
`ubuntu-latest` image, so a wheel runs on that glibc or newer and not on an older distribution.
Building from a checkout always works. A manylinux build is a follow-up, not a limitation of the
binding.

**Stable ABI.** Built with Python 3.12 or later, the wheel is tagged `cp312-abi3` and runs on 3.12
and every later Python, one wheel per platform. Built with an older Python, it falls back to a
version-specific build and the configure step says so.

## Testing

```
cd bindings/python
uv run --extra test pytest
```

`uv run` builds and installs the project into its environment first, so pytest runs against the
installed package rather than the source tree.

The suite also runs under ctest as `python_bindings`, with the two examples as
`python_example_offline_render` and `python_example_live_onset`:

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
