# bw_audio for MATLAB and Octave

A MEX binding for the bw_audio spatial audio engine, for Psychtoolbox experiments.

It is one MEX gateway plus a small class layer, and the same source builds for both interpreters.
The gateway uses the classic C MEX API, which Octave implements, and not the MATLAB-only C++ Data
API, so nothing here is MATLAB-only by construction.

## Install

### From a release

Download `bw_audio-matlab-<tag>.zip` from a release, unzip it anywhere, and in MATLAB or Octave:

```matlab
addpath('<where-you-unzipped>/bw_audio-matlab')
```

That is the whole install. The folder carries `+bwa`, the examples, and `bin/win64`,
`bin/glnxa64` and `bin/maca64`, each holding that platform's **two** MEX files, MATLAB's
(`.mexw64`, `.mexa64`, `.mexmaca64`) and Octave's (`.mex`), plus the one engine library both of
them load. `bwa.setup` picks yours, and `bwa.Engine` calls `bwa.setup` for you.

One thing to know. The macOS binaries are **unsigned and un-notarized**, like every other macOS
binary this project ships, so clear the quarantine flag after unzipping:

```
xattr -dr com.apple.quarantine <folder>
```

### From the checkout

You need the engine built first. Both halves are opt-in, and CMake builds whichever toolchain it
finds:

```
cmake -S . -B build -DBWA_BUILD_MATLAB=ON
cmake --build build --config RelWithDebInfo
```

The MATLAB half needs MATLAB with a configured C compiler. Check with `mex.getCompilerConfigurations('C','Selected')`
and run `mex -setup C` once if it is empty. The Octave half needs `mkoctfile`, which comes with
Octave. On Windows the installer does not put it on `PATH`; CMake looks in
`C:\Program Files\GNU Octave\Octave-*\mingw64\bin` for you, or set `OCTAVE_HOME` to the `mingw64`
folder itself. That is Octave's own variable and its own meaning, the install prefix, and
`mkoctfile` derives its include path from it: point it one level up and the MEX build stops at
`mex.h: No such file or directory`. A Chocolatey `octave.portable` install is somewhere else
again (`<ChocolateyInstall>\lib\octave.portable\tools\octave\mingw64`), which is what CI points
`OCTAVE_HOME` at.

The build stages each MEX with the engine library beside it, under
`bindings/matlab/bin/<matlab|octave>/<platform>/`. Nothing there is committed. That layout is the
build tree's, not a release's: a build stages two interpreters side by side and a release ships
one per architecture. `bwa.setup` looks in both.

Then, from either interpreter:

```matlab
addpath('<repo>/bindings/matlab');   % the directory that CONTAINS +bwa
bwa.setup();                          % finds the MEX for THIS interpreter and checks the ABI
```

`bwa.Engine` calls `bwa.setup` for you, so adding the one directory is the whole install. Set
`BWA_MATLAB_BIN` to point somewhere else, for instance at a copied release directory.

### Against a prebuilt engine

You do not have to compile the engine to build a MEX. Install an
[engine SDK](../../docs/build.md#the-engine-sdk) once, from a checkout or from a release engine
artifact, and configure the binding against it:

```
cmake -S . -B build-bind -DBWA_ENGINE_SDK=/path/to/sdk -DBWA_BUILD_MATLAB=ON
cmake --build build-bind --config RelWithDebInfo
```

Nothing under `src/` compiles, no test or tool is registered, and the staging is the same: each MEX
lands under `bindings/matlab/bin/<matlab|octave>/<platform>/` with the SDK's engine library beside
it. This is how CI builds both gateways, so the library a MEX loads is the one that platform's
ctest run tested.

`bwa.setup` already guards the seam this opens: it compares the gateway's compiled `BWA_VERSION`
against `bwa_mex('get_version')` and errors with `bwa:abiMismatch` when the two are from different
builds.

## Two layers

`bwa_mex` is the raw layer: one subcommand per `bw_audio.h` entry point, named by its C name minus
the `bwa_` prefix, with the header's own argument order and units. Read `include/bw_audio.h` or
`docs/api.md` and you are reading this layer.

```matlab
h = bwa_mex('create', struct('profile', 0, 'sink', 3, 'sample_rate', 48000));
bwa_mex('source_set_pos', h, src, 1.0, 1.5, 0.0);
bwa_mex('commit', h);
[dsp, host] = bwa_mex('get_clock', h);
bwa_mex('destroy', h);
```

`bwa_mex('commands')` lists every subcommand. The shape is PsychPortAudio's, which is what this
audience already reads.

The `+bwa` package is the class layer over it: an `Engine` and small handle objects, plus the one
convenience with semantics, an automatic commit.

```matlab
e = bwa.Engine('profile', bwa.Const.PROFILE_BINAURAL, 'sink', bwa.Const.SINK_MANUAL);
snd = e.loadSound('click.wav');
src = e.createSource();
src.setPos(1.0, 1.5, 0.0);        % committed for you
src.play(snd);
block = e.render_block();
e.close();
```

Mix the layers freely. `e.raw('set_spcap_focus', 12.7, 2.0)` is `bwa_mex('set_spcap_focus', h,
12.7, 2.0)`, and `e.handle()` gives you the raw handle for a call you would rather write out.

## What is not bound

`bwa_set_output_capture`. Its callback runs on the audio thread, and an interpreter must never run
there: entering MATLAB or Octave inside a device callback allocates, takes locks and runs arbitrary
code, which breaks the engine's first invariant. The subcommand exists and refuses, with that
reason, so you find the explanation where you looked for the call.

The offline path is the manual sink and `render_block`, which hands back the same post-limiter
samples on your own thread.

## Values and shapes

Handles (engine, sound, source, bed) are `uint64` scalars. An engine handle that is not live raises
`bwa:deadEngine` rather than dereferencing freed memory; the gateway keeps a registry for exactly
that.

Positions and directions are `(n,3)` arrays, one xyz per ROW. That is what `get_speakers` and
`box_mesh` hand back, so a round trip works. It matters: MATLAB is column-major, so an `(n,3)`
matrix is `x1..xn y1..yn z1..zn` in memory, and a call that read it as xyz triples would garble
every position after the first without erroring.

`render_block` returns an `[nframes, channels]` `single` matrix. It is a COPY. The Python binding
hands back a zero-copy view of the engine's own buffer; an `mxArray` cannot alias memory the engine
overwrites on the next call, so this one memcpy's. The copy is one `memcpy` and no transpose,
because the engine's planar block is channel-major with a channel stride of `nframes`, which is
exactly the column-major layout of `[nframes, channels]`.

`push` accepts `single` or `double`, as a mono column or row.

Out-parameters become extra outputs: `[dsp, host] = bwa_mex('get_clock', h)`. A `bool` plus
out-struct call returns `[]` on false and a struct on true, rather than a separate `ok` flag, which
is what the Python raw layer does with the same calls. So `e.health` is `[]` when the configuration
cannot observe a dropout at all. That is not a clean bill of health.

Strings are char rows. `[]` or `''` means NULL, which is how you ask for the engine's default.

Enums are `bwa.Const.PROFILE_BINAURAL` and friends. `bwa.constants()` returns the live values the
gateway compiled, and the test suite pins the class against it, so a literal cannot drift.

## Commit model

Position and pose are commit-gated in the ABI: they write to pending fields, and only `bwa_commit`
promotes them. A client that forgets it plays sounds that never move. This layer commits for you.
Three modes:

```matlab
src.setPos(1, 1.5, 0);            % autocommit (the default): lands at once

c = e.frame();                     % a frame block: one commit, at the end
src1.setPos(...);
src2.setPos(...);
e.listener.setPose(...);
clear c                            % the commit lands here

e.autocommit = false;              % the C semantics: nothing commits but commit()
src.setPos(1, 1.5, 0);
e.commit();
```

`e.beginFrame()` and `e.endFrame()` are the explicit form of the middle one; nesting is allowed and
commits once, at the outermost exit. A play call flushes a pending write first, so "set the
position, play it" never renders the first block at the old position.

Use a frame block whenever several writes must land as one snapshot. Two sources that move together
have to be rendered as having moved together.

## Threading

Every `bwa_*` call must come from ONE control thread. Here that is satisfied by construction: a MEX
call runs on the interpreter's main thread. Parallel-pool workers are separate processes with their
own address space, so an engine handle does not travel to one. Do not try.

The gateway is `mexLock`ed while any engine is live, so `clear mex` cannot unload it out from under
a running audio thread, and a `mexAtExit` handler stops and destroys whatever is still live, so
`clear all` or quitting the interpreter never leaves a device open.

## Timing: landing a sound on a visual event

Wall time and the engine's dsp-sample clock are different clocks on different epochs. The engine
stamps a `(sample, host time)` pair INSIDE the device callback, which is exact, so the only unknown
is the constant offset between the engine's host clock and yours. `bwa.ClockBridge` measures it
with a sandwich, two reads of your clock around one of the engine's:

```matlab
b = e.bridge();                    % or bwa.ClockBridge(e)
b.refresh();                        % once per trial
sample = b.dsp_at(t_seconds);       % your clock, as a dsp sample
t      = b.time_at(sample);         % and back
b.play_at(src, snd, t_seconds);     % scheduled so it is HEARD at t_seconds
```

`b.sandwich_seconds` reports the width of the last measurement, which is the error bound on the
offset. It measures tens of microseconds in the steady state on the machines this was written on.

`play_at` subtracts the device's render-to-DAC delay (`e.output_latency_frames`) because that is
the part the engine knows. YOUR display delay is not: measure draw-to-photons once with a
photodiode or an AV-sync clapper, and add that one constant to `t_seconds` yourself.

This is the same arithmetic `PsychPortAudio('Start', pahandle, 1, when)` does against its own clock.

**Which clock.** With Psychtoolbox usable, `ClockBridge` uses `GetSecs`, because that is what the
rest of a Psychtoolbox experiment times against. `GetSecs` is QPC on Windows and
`mach_absolute_time` on macOS, which ARE the engine's clock, so the offset is stable for a session.
On LINUX `GetSecs` is `CLOCK_REALTIME`, a wall clock NTP can slew or step, and the engine's clock is
`CLOCK_MONOTONIC`: the offset drifts and can jump, so call `refresh()` before each trial there.
Without Psychtoolbox the bridge uses `tic`/`toc` against a stored `tic`, which is monotonic in both
interpreters. Do not reach for `clock()` or `now()`: both are calendar time. Pass your own function
handle as the second argument to override.

## Coexisting with PsychPortAudio

They must not open the same device exclusively. PsychPortAudio takes its device in exclusive mode by
default at its higher latency classes, and `BWA_SINK_FLAG_EXCLUSIVE` does the same here, so two
exclusive claims on one endpoint means one of them fails to open.

Pick one:

- Let the engine own the device and use PsychPortAudio for nothing, or for a different device.
- Let PsychPortAudio own the device and pre-render your stimuli through the engine's manual sink.
  That is the offline shape, and it is the one to start with for any experiment that can pre-render.
- Share, with both in shared mode. You give up the lowest latency and a fixed callback size.

The engine reports what it actually opened through `e.backend`, so check rather than assume.

## Examples

Each runs as a function, and each takes `--tests` for a self-check that forces an offline sink and
exits nonzero on a failure. ctest runs all three under both interpreters.

- `examples/offline_render.m` renders a moving source to a matrix and writes a wav, with no device.
  The manual sink is bit-identical run to run, which is what makes a pre-rendered stimulus set
  reproducible.
- `examples/live_onset.m` schedules onsets against the dsp clock through `bwa.ClockBridge`. The
  live shape.
- `examples/AudioTunnel3DDemo_bwa.m` walks you through a tunnel of looping sources that drift past
  you and wrap around: a re-implementation of the idea in Psychtoolbox-3's `AudioTunnel3DDemo2`
  against this engine. It differs from the OpenAL original in four ways worth knowing. Doppler is
  the engine's, derived from the position changes the loop is already making, rather than a
  velocity vector you maintain by hand. Reverb is image-source early reflections plus an FDN late
  tail, which run on every platform, rather than the macOS-only OpenAL reverb; the shoebox is the
  VIRTUAL tunnel, never the room you are sitting in. Every source moves inside ONE frame block per
  iteration, so no rendered block sees half an update. And Psychtoolbox is optional: with it you
  get the original's Escape and Space controls, without it the demo synthesizes its own looping
  sounds and runs for a fixed time, because neither interpreter has a portable non-blocking key
  read on its own.

## Tests

```
ctest --test-dir build -C RelWithDebInfo -R "matlab_|octave_"
```

Or directly:

```
matlab -batch "exit(run_tests())"                 # from bindings/matlab/tests
octave-cli --no-gui --quiet --eval "exit(run_tests())"
```

Plain assert-based scripts, not `matlab.unittest`, because Octave has no `matlab.unittest` and a
suite that only runs in one interpreter lets the other drift. `run_tests` returns 77 when the MEX
has not been built, which ctest maps to SKIPPED.

The suite includes the golden from `test/golden_test.c`, driven from here against that test's own
committed constants. It proves this binding reaches the same DSP the C suite pins, so a binding bug
that quietly changed a gain, a position or a channel order cannot pass.

## CI

Both MEX files are built inside each desktop CI job, after that job's engine build, so each links
that platform's engine with its own backends intact. The `windows` job then assembles all six into
the one toolbox folder a release ships.

Every desktop job installs Octave itself: `octave` and `octave-dev` from apt on Linux,
`octave.portable` from Chocolatey on Windows, `octave` from Homebrew on macOS. Each configures with
`BWA_BUILD_MATLAB=ON` before the engine build, so the Octave MEX is an ordinary `ALL` target and the
four `octave_*` ctests run with the rest of the suite. MATLAB's half waits for the `matlab-actions`
steps, because only MATLAB needs an action to install it and a licensed `run-command` step to drive
it.

## Platform notes

The Linux MEX finds the engine library beside it through an `$ORIGIN` runpath, so a copied
directory works with no `LD_LIBRARY_PATH`. On Windows `bwa.setup` prepends the staged directory to
`PATH`, because Windows searches `PATH` for a MEX file's dependent DLLs and not the MEX file's own
directory.

The Octave MEX on Windows is built by MinGW `gcc` and links against the MSVC-built
`bw_audio.dll` through its import library directly. No `dlltool` step is needed: MinGW's `ld`
reads an MSVC `.lib`.
