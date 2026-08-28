# Changelog

All notable changes to `com.brainworks.bw_audio`.

## [Unreleased]

### Changed (breaking): `BWA_VERSION` -> 0.13.0, and `bwa_get_dsp_time` is gone

The ABI version tracks compatibility, not releases, and this set of changes is not compatible with
0.12.0. `bwa_get_dsp_time` was renamed to `bwa_get_dsp_time_frames`, so a symbol was REMOVED, and
seven calls were added: `bwa_source_set_channel`, `bwa_source_set_region`, `bwa_poll_looped`, and
the four bed aliases `bwa_bed_play_at`, `bwa_bed_play_loop`, `bwa_bed_stop_at` and
`bwa_bed_set_region`.

0.12.0 was never tagged, so it was tempting to fold this into it. Do not. Binaries stamped 0.12.0
already exist with the old symbol set, and telling two different ABIs apart is the only thing
`bwa_get_version` is for. A stale GDExtension built before the rename is exactly the case it has to
catch, and it can only catch it if the numbers differ.

### Fixed: the engine could put a NaN on the bus, and a play region could stall the audio thread

Two real-time correctness bugs, both on calls documented as safe to make every frame.

`isfinite` is not a range check. A finite but absurd coordinate such as `3e38` passed every guard,
and then the first thing any spatial solve does is square a coordinate difference, which overflows
to infinity, and the normalize that follows produces a NaN. Gains are sticky, so once one voice was
poisoned every later block was too, and the value reached the align delay line and the room EQ
biquads, whose IIR state holds it past any later correction. Coordinates and room dimensions are now
bounded, not merely checked for finiteness, and the bound is a required argument to the check so the
mistake is no longer expressible. This accounted for five fuzz seeds.

`bwa_source_set_region` could make audio-thread work unbounded and caller-controlled. Setting a small
region on a voice that had been looping for a while made the wrap seam walk back one span at a time,
which for an hour-long loop re-regioned to one frame is on the order of a hundred million iterations
inside a single buffer callback. It is a modulo now, and one shared helper serves all three seam
sites so a bed and a source can no longer answer the same call differently.

Also fixed: a play region or a seek issued after an async play was silently lost, on exactly the call
order the documentation prescribes; destroyed handles reported stale playheads, directivity and
occlusion; triangle indices were handed to the ray tracer without being checked against the vertex
count, which is an out-of-bounds read rather than a wrong value; and a group stop left held plays to
start by themselves.

### Fixed: `Engine.StopGroup` left a still-decoding play to start after the stop

`StopAll` dropped the plays still waiting on an async decode. `StopGroup` did not, so a play issued
against a `LoadAsync` handle in that group started by itself the moment its decode landed. That is a
sound beginning after you told the group to stop, and only on async assets, which is the hardest
shape to catch by ear.

The engine now tracks each source's mix group on the control side as well, so a group stop can find
the pending plays that belong to it. `StopGroup` drops this group's held plays; `StopAll` still drops
every one. Both push the stop command before dropping anything, so a momentarily full command ring
leaves no half-effect. The group a still-decoding play belongs to is the one its source was in when
the stop ran.

### Fixed: `SourceBase.Channel` cached a route the engine had refused

The setter pushed whatever it was handed and cached it. The engine refuses an index outside
`0 .. ChannelCount - 1`, and refuses every negative but `Bwa.CHANNEL_AUTO`, but it refuses into
`bwa_last_error` where nothing reading the property would ever see it. So `Channel = 99` left the
source PANNED while `Channel` answered 99, which is exactly how a reference source gets read as a
single-speaker ground truth in an experiment that is really hearing a phantom.

It now refuses out of range with a warning and keeps the route the source had, matching Godot's
`BwaSource.set_channel`. The negative half is checked even before the source is live, because a
negative needs no channel count to judge. `ChannelRouteTests` pins both halves, warnings included
(`LogAssert.Expect`, so a silent refusal fails the test).

### Fixed: a play held on an async decode voided the duplicate-end suppression

`onFinished` has two feeds and a one-shot latch that keeps one end from reaching both. The latch was
cleared by the next play. A play HELD on an async decode is not a play the engine has bound: it
bumps a voice's play counter only at bind time, and that counter is the gate that drops a completion
straggling in from the PREVIOUS play. Until it moves the straggler is still deliverable, so clearing
the latch on a held play opened the way for one end to be reported twice. The case that produces the
straggler is a stop landing on the block a clip runs out in, which the engine posts a completion for
anyway.

The latch is now cleared by the next play that BINDS: at the call for a synchronous play, and on the
frame the data lands for a held one. `AmbisonicBed` had the same latch and the same defect and takes
the same fix. `Engine.WatchPendingLoad` gained an `out bool landed` so a component can tell "the
decode arrived" from "the decode failed", which is the same distinction as "the play bound".

`CompletionTests.HeldPlayVoidsSuppressionOnceItBinds` pins the half that is decidable by ordering:
a held play must void the suppression once it binds, or the clip it eventually plays reports no end
at all. The double-fire itself is NOT covered here and the test says so. Unity arms the latch only
inside `Emitter.PostCommit`, which `Engine` runs after the ended drain in the same `LateUpdate`, so
the window is the microseconds between those two calls and no ordering the harness can impose widens
it. Godot's `demo/api.gd` covers that half deterministically, because `BwaEmitter.stop()` arms its
latch synchronously.

### Added: a headless PlayMode suite, so the binding is EXECUTED and not only compiled

Every change below this line was compile-verified only. Nothing in the repo had ever run the Unity
binding's runtime behavior, and behavioral confidence was inference from the Godot binding driving
the same C ABI through equivalent machinery. `bindings/unity/test~/` closes that: 20 PlayMode tests
that drive the real components against the real `bw_audio.dll` on the offline sink, registered as the
`unity_playmode` ctest behind `-DBWA_UNITY_EXE=<editor>`. Details and the feasibility notes are in
`docs/integration.md` -> "Tests".

What it pins, in the order the risk sits: a sub-frame clip fires `onFinished` exactly once (the case
the drain replaced the `IsPlaying` edge for); a natural end fires once and not twice with both feeds
armed; `Stop()` still fires, which is Unity's contract and not Godot's; a stop scheduled onto the
clip's final block fires once; a looping source raises `onLoop` per wrap and `onFinished` never;
`SetRegionFrames` confines the playhead and truncates a one-shot; the seconds spellings of the region
and the seek agree with the frames ones exactly; a bed raises both events through Engine's second
handle map, and bed and source events do not cross; and `EndedEventsDropped` / `LoopEventsDropped`
rise when starved and stay at zero when pumped.

Two properties keep it from being a suite that cannot fail, which is the default outcome for a test
suite rather than a rare mistake:

- **Determinism comes from ordering, not from timing.** Several tests turn the `Engine` component
  off for a stretch. Only `bwa_commit` is frame-gated, so a play still reaches the audio thread and
  a voice starts, plays and ends while nothing commits, drains, or polls is-playing. Turning the
  pump back on gives exactly one pump against a known state. "Fires exactly once" is then decidable
  rather than a race the test happens to win.
- **Every test was falsified.** Each one was run against a deliberately broken binding and confirmed
  to go red: `NotifyEnded` made a no-op (the old `IsPlaying` edge) fails the sub-frame test;
  dropping `_wasPlaying = false` makes the natural end fire twice; removing the halt fallback loses
  `Stop()`; dropping the rate conversion in `SeekSeconds` / `SetRegionSeconds` lands the playhead on
  frame 1 instead of 48000; removing Engine's bed-map lookup loses every bed event; pinning either
  dropped counter at 0, or at "always one more", fails its own test.

The suite also carries its own vacuity guards. One test asserts the sub-frame clip really is shorter
than a frame at the measured frame interval (1.33 ms against 33 ms, 25x), because that claim is what
the headline test rests on. One asserts the bed fixture loads as 4-channel B-format through the
AmbiX loader, because the same file taken through the default mono loader reports one channel and
would have made the check vacuous. And the ctest driver judges `results.xml` rather than the exit
code, refusing a run that discovered fewer tests than expected or skipped any.

### Added: a convenience tier over the core ABI (`BWA_VERSION` -> 0.12.0)

Four additions in the same spirit, all of them control-thread sugar over calls that already existed.
The C ABI sat one tier below what a client actually writes, so every client rebuilt the same three
things by hand. `docs/api.md` gains an "API tiers" section that states the split and the guarantee
that goes with it: a convenience call writes the same command ring and lands in the same mixer, so
nothing new reaches the audio thread and there is no second render path.

- **Shared asset ownership: `bwa_sound_acquire` / `bwa_sound_release`.** A by-path, refcounted cache
  keyed on `(normalized path, flags)`, with the four loaders folded into one `bwa_load_flags`
  parameter (`BWA_LOAD_STREAM`, `BWA_LOAD_AMBIX`, `BWA_LOAD_FUMA`). Both bindings had already built
  this privately, and the Godot side had to carry a `unload_sound_path` fan-out precisely because one
  path can be cached under several keys. That is the engine's own key, so it is the engine's job now.
  The explicit-ownership loaders (`bwa_load_sound` and siblings) are unchanged; mixing the two tiers
  on one handle is refused with an error string rather than silently corrupting the refcount.
- **Async loading: `bwa_sound_acquire_async` / `bwa_sound_is_ready`.** Returns a usable handle
  immediately and decodes on a lazily started loader thread, for content that arrives mid-session.
  A play issued against a not-yet-ready handle is held on the control thread and re-issued as an
  ordinary `CMD_PLAY` once the data lands, so playback starts from frame 0 with nothing skipped. The
  audio thread never sees a reserved slot and gained no new branch. See `docs/concurrency.md`,
  "Async asset staging".
- **Source configuration: `bwa_source_desc` with `bwa_source_preset`, `bwa_source_create_desc`,
  `bwa_source_apply`, `bwa_source_get_desc`.** There were 24 per-source setters and readback for two
  of them, so configuring one prop took 15 or more calls, and nothing could print or reset a source.
  This is the same fill-then-apply shape as `bwa_tuning` and it carries the same `struct_size` guard
  for the same reason: this struct's zero is not its default (gain 0 is silence, pitch 0 is invalid),
  so a zero-initialized struct must fail loudly. Position, orientation, and playback state are
  deliberately excluded; they are per-frame, not configuration. `bwa_source_apply` is one ring
  command, and the payload packs inside the union's existing width, so no other command pays for it.
- **Scene transitions: `bwa_group_stop` and `bwa_stop_all`.** Mix groups had gain and pause but no
  stop. Both ride the existing one-block fade, so they are click-free (invariant 4), and both stop
  beds and drop pending play queues. Neither resets the mixer: group gains, pause gates, and master
  gain survive, so a re-played scene returns at the levels the client dialed.

`Bwa.BoundVersion` moves to 0.12.0 to match the header. The Unity binding rides all four additions
now, in the entry below, and `Engine.cs`'s `Dictionary<string, uint> _sounds` is gone with them.

The Godot migration turned out to delete less than the sentence here first claimed. Its
deduplication, its reference counting, and the four-prefix key fan-out ("m:", "s:", "a:", "f:") all
go, because that fan-out existed only for the want of a shared key. The path-to-handle RECORD stays,
but for a narrower job than it had: `unload_sound_path` releases the references this NODE holds and
no others, and `sound_is_ready` answers only for keys this node acquired. The metadata getters do
not use it at all any more. They go through `bwa_sound_find` (below), which the migration added for
exactly this and which answers for any resident path, not just the ones this node loaded.

A fourth console walkthrough, `examples/convenience.c` (`bwa_convenience`), demonstrates the whole
tier against a running engine: two systems acquiring one clip and getting one decode, the same file
resident in RAM and streamed at once, a probe that does not load, a handle handed out before its PCM
exists, a source configured and read back through one struct, and the two scene-transition stops.
Every part ends by naming the core calls it replaces, because the claim of this tier is that the two
spellings are the same audio. The three existing examples keep teaching the explicit tier and now
point at it. `examples/playground.cpp`'s per-source scene reset became one `bwa_source_apply`.

All four console examples now run under ctest as `example_*`. A new `--tests` flag forces the offline
sink, so the suite never depends on an ASIO driver being installed, and cuts the listening time
(17 seconds for all four). They exercise the real ABI end to end, which catches a change that still
compiles but misbehaves, and `bwa_convenience` verifies every claim it prints and exits nonzero on a
mismatch rather than only proving it did not crash.

### Added: the bed playback surface, and completion + loop events on beds

Both bindings exported `bwa_bed_play` but none of the four scheduled or region forms the C ABI
carries beside it, and neither could tell a client that a bed had ENDED or WRAPPED.

- **Unity** `AmbisonicBed` gains `PlayAt(startSample)`, `PlayLoop(loopBeg, loopEnd)`,
  `StopAt(stopSample)`, `SetRegionFrames(startFrame, endFrame)` and `SetRegionSeconds`, over the
  new `bwa_bed_play_at`, `bwa_bed_play_loop`, `bwa_bed_stop_at` and `bwa_bed_set_region` P/Invoke
  declarations. Every one of those quantities is FRAMES, so the region call carries its unit in its
  name, beside a seconds twin, exactly as `Emitter.SetRegionFrames` does.
- **Godot** `BwaBed` gains the same five as `play_at` / `play_loop` / `stop_at` /
  `set_region_frames` / `set_region_seconds`.
- **Events.** `AmbisonicBed` gains `onFinished` and `onLoop`, and `BwaBed` the `finished` and
  `looped` signals, with the emitter's contract on both sides. A bed IS a voice, so the core had
  been reporting bed handles through `bwa_poll_ended` and `bwa_poll_looped` all along. What was
  missing was the ROUTE: a bed is not a source component in either binding (it has no position, and
  the source registry exists to push one every frame), so the drain never found an owner for a bed
  handle and dropped it silently. Each binding now keeps a second handle map for beds, consulted
  when a handle belongs to no source. Bed and source handles come out of one pool, so a handle is a
  source's or a bed's and never both.
- The halt fallback comes with them: an explicit halt posts no completion event at all, so a bed
  keeps the same narrow is-playing edge an emitter does, read after the drain, with the same
  one-shot latch so a halt and a straggling completion describing one end cannot both report it.
  Godot's `BwaBed` needs the emitter's full three-state machine for this. A two-flag version looked
  equivalent and was not: a stop is enqueued, so the bed still reads as playing for a block
  afterward and re-armed the edge, which then fell to silence as a spurious `finished`. The Godot
  demo caught it.

### Added: loop events, play regions, and the single-speaker route

Three ABI additions, added because a collaborator's Max/MSP and Spat5 rig could do things this
engine could not: play arbitrary content out of exactly one speaker, take an event at a loop
boundary, and bound playback to a region inside a file.

- **`Emitter.onLoop`**, from `bwa_poll_looped`. A looping voice never ends, so `onFinished`
  reports it exactly never, and pacing an experimental trial or cueing a visual off a loop had no
  event at all. `Engine` drains this ring beside the ended one, under the same single-owner rule
  and at the same point (after `bwa_commit`, which is what fills both), and dispatches through the
  same handle-to-component map. It is NOT entangled with `onFinished`: a wrap does not mean the
  voice stopped, so none of the completion latching applies to it. One callback per WRAP, so a
  loop region shorter than a frame fires several times in one frame.
  `Engine.LoopEventsDropped` surfaces the ring's own dropped total, which unlike the ended one
  can rise without anything being wrong.
- **`Emitter.SetRegionFrames` / `SetRegionSeconds`**, from `bwa_source_set_region`. Bound the
  clip to `[start, end)`: a looping voice wraps back to `start`, a one-shot ENDS at `end` exactly
  as it would at the clip end, so a loop region and a truncated one-shot are one call. Both
  spellings, and no bare `SetRegion`, for the reason the repo already spells `seek_frames` and
  `get_output_latency_frames` in Godot: the two differ by a factor of the sample rate, and a
  caller arriving from `AudioSource.time` guesses seconds.
- **`SourceBase.Channel`** (with `Bwa.CHANNEL_AUTO`), from `bwa_source_set_channel`. Sends one
  source out of one speaker with no spatial processing: the psychophysics ground-truth condition,
  a real speaker to A/B a phantom against. Not `bwa_set_test_signal`, which injects a built-in
  tone after the per-speaker align stage and is therefore not level-comparable with a rendered
  source. Script-only rather than a serialized inspector field, because a route is a run-time
  experimental condition and not authored configuration; it is replayed across a re-enable.

### Changed (breaking): `Emitter.Seek` is `SeekFrames`, beside a new `SeekSeconds`

`Seek(ulong)` took engine-rate FRAMES under a bare name. `AudioSource.time` is seconds, so a caller
arriving from Unity's own API reads the bare spelling as seconds, and the two differ by a factor of
the sample rate. That is the same defect the Godot binding already spells around with
`seek_frames` / `seek_seconds`, and the same reason `SetRegionFrames` / `SetRegionSeconds` landed
above with no bare `SetRegion`. `Seek` was the last call in the Emitter surface still taking the
bare form.

`Emitter.Seek(ulong samples)` becomes `Emitter.SeekFrames(ulong frame)`, and `SeekSeconds(double
seconds)` converts at the engine's sample rate exactly as `SetRegionSeconds` does (a negative
position is ignored, and it is inert with no engine). There is no alias and no `[Obsolete]`
forwarder: nothing ships against this yet, so the correct spelling is worth more than the
compatibility.

Those three names are settled in the next entry, as a set.

### Changed (breaking): the time-unit sweep, and the 0.5.0 decision reversed

**0.5.0 decided to KEEP `SourceBase.Playhead` / `PlayheadSeconds`, `AmbisonicBed.Playhead` and
`Engine.OutputLatency`** (see "Changed - units in names, where they earn it" below), on the argument
that `AudioSource.timeSamples` versus `time` is Unity's own spelling of the frames-versus-seconds
pairing and that there was no host collision to fix. **That decision is reversed.** Two things
changed under it:

- The argument covered `Seek` equally well, and `Seek` was renamed to `SeekFrames` above, beside a
  new `SetRegionFrames` / `SetRegionSeconds` pair that never had a bare spelling. So the 0.5.0
  reasoning was left half-applied: the same surface now spelled the unit in some places and leaned
  on Unity's precedent in others, which is worse than either rule applied consistently.
- The "no host collision" half is simply false for the clock. **Unity's own `AudioSettings.dspTime`
  is a `double` of SECONDS**, and `Engine.DspTime` returned a `ulong` of FRAMES. That is the exact
  defect the repo already spells around in Godot (`AudioServer.get_output_latency()` is seconds), in
  the opposite unit and under a nearly identical name. It is also the hardest one to notice, because
  the two are wrong by a factor of the sample rate, which reads as a plausible-but-early cue.

Renamed, with no alias and no `[Obsolete]` forwarder (nothing ships against this yet):

| Old | New |
| --- | --- |
| `SourceBase.Playhead` | `SourceBase.PlayheadFrames` |
| `AmbisonicBed.Playhead` | `AmbisonicBed.PlayheadFrames` |
| `Engine.OutputLatency` | `Engine.OutputLatencyFrames` |
| `Engine.DspTime` | `Engine.DspTimeFrames` |
| `Engine.DspTimeAt(realtime)` | `Engine.DspTimeFramesAt(realtime)` |

Added beside them, so every frames-valued reading has its seconds twin: `AmbisonicBed.PlayheadSeconds`
(a bed and a source now read alike, and match Godot's `BwaBed`), `Engine.OutputLatencySeconds` (the
unit AV-alignment arithmetic wants, since a measured display delay is seconds too), and
`Engine.DspTimeSeconds` (the like-for-like comparison against `AudioSettings.dspTime`; only
DIFFERENCES are comparable, since the two clocks have different epochs).

`Engine.RealtimeAt(dspSample)` is deliberately NOT renamed: `Realtime` is Unity's own word for that
clock and it is seconds there too, so the name already agrees with its unit, and the parameter
carries its own.

The C ABI and the Godot binding move with it, so all three layers say the same thing:

| Layer | Old | New |
| --- | --- | --- |
| C | `bwa_get_dsp_time` | `bwa_get_dsp_time_frames` |
| Godot | `BwaEngine.get_dsp_time()` | `get_dsp_time_frames()` + new `get_dsp_time_seconds()` |

That was the last bare time-valued name in the C ABI. `start_sample` / `stop_sample` /
`dsp_sample` keep the `sample` spelling of the same unit, as 0.5.0 decided.

### Fixed: `Emitter.onFinished` missed any clip shorter than a frame

The event was driven by edge-detecting `bwa_source_is_playing` once per frame, which is exactly the
mechanism the header tells clients not to use for completion: "you may never observe a sound shorter
than your poll interval as playing. That is exactly why `bwa_poll_ended` exists." A footstep, a UI
click, a short impact could come and go entirely between two `LateUpdate` calls and `onFinished`
would never fire. The code comment admitted the miss rather than fixing it.

`Engine` now drains `bwa_poll_ended` and routes each handle to the source component that owns it.

- **`Engine` is the single owner of the drain.** The drain is engine-wide and destructive, so a
  second caller would consume other components' completions and those would silently never fire.
  `Engine.Register`/`Unregister` keep a handle to component map for the dispatch, removed with the
  source so it neither leaks nor outlives a handle. A generation is bumped before a slot is reissued,
  so a later source cannot mint a key this map still holds.
- **Polled after `bwa_commit`, in the same `LateUpdate`.** The ended ring is filled by the pass
  `bwa_commit` runs, so polling before it would read a frame-old picture.
- **The `IsPlaying` edge survives as a narrow fallback, because an explicit HALT posts no event.**
  `bwa_source_stop`, `stop_at`, `fade_out`, `bwa_group_stop` and `bwa_stop_all` all take the
  click-free stop path, which sets the voice not-playing without posting a completion (the engine's
  position is that a halt is not a completion, and a stolen voice is not one either). Unity's
  `onFinished` has always fired for those, so it still does. The fallback reads AFTER the drain, so a
  natural end is reported by its event and not twice; a latch absorbs the one interleaving where a
  voice ends between the drain and the read.
- **`Engine.EndedEventsDropped` surfaces `dropped_out`.** The ended ring is bounded and drops the
  oldest, so a non-zero total means that many `onFinished` callbacks never fired. A warning names it
  once, on the first increase.

### Fixed: the async tests could not tell whether they had covered the held-play window

Review turned up a test that could not fail. Both the C asset test and the Godot demo fixture claimed
to cover the window in which a play waits on a decode, and neither could know whether it had: a small
asset usually decodes before the first render, so the play binds immediately, the held path goes
untouched, and every assertion still passes.

The fix is ordering, not machinery. A finished decode is adopted only at a pump point
(`bwa_sound_acquire`, `bwa_sound_is_ready`, `bwa_sound_find`, `bwa_sound_release`, `bwa_commit`), and
`bwa_source_play` is not one of them, so a play issued straight after `bwa_sound_acquire_async` with
nothing pumping in between is guaranteed to be held. `test/assets_test.c` now does exactly that and
asserts it, using `bwa_sound_get_channels` (0 while the slot is reserved, and it does not pump) as
the witness. Checked in both directions: inserting a readiness check before the play makes those
assertions fail, which is the silent way the coverage was being lost.

A `bwa_set_loader_stall` diagnostic was built for this first and then removed. It worked, but it put
a call in the public ABI that existed only for testing, and ordering gets the same guarantee for
nothing. Two cases genuinely cannot be pinned this way and now say so instead of implying otherwise:
cancelling an in-flight load (`bwa_sound_release` pumps on entry, so the decode may already be
adopted) and the Godot fixture (the binding checks readiness on its way to playing). Both assert the
outcome, which holds either way, and neither claims to have exercised the hold.

### Added: `bwa_sound_find`, a by-path probe that does not load

Migrating both bindings onto the shared asset tier turned up one gap, and both hit it independently:
there was no way to ask "which handle does this path have" without `bwa_sound_acquire`, whose miss
path *loads* the file. Godot has three public methods that are by path (`unload_sound_path`,
`sound_get_frames`, `sound_get_channels`), and probing them with an acquire would have reinstated
exactly the hidden-decode bug those getters were fixed for in an earlier release. It would also have
decoded mono, so an ambisonic bed would report 1 channel forever.

`bwa_sound_find(engine, path, flags)` returns the handle for a key the cache already holds, or 0. It
never loads, never touches the disk, and never takes a reference, so the handle it returns is
borrowed: read metadata from it, do not release against it. A still-loading async entry answers with
its handle (it is resident; ask `bwa_sound_is_ready` about the data), while a failed one answers 0
because it holds nothing to answer about.

Godot's `find_loaded_sound` now asks the engine instead of scanning its own record, so it answers for
any resident path rather than only the ones loaded through the node. Unity's `SoundFrames` and
`SoundChannels` probe first, so asking a resident clip its length no longer takes a reference.

Failure stays deliberately uncached on the synchronous path: a failed `bwa_sound_acquire` records
nothing and the next call retries the file, which is what a late-appearing file wants. A client that
asks in a loop should remember its own failures, which is why `Engine` keeps a small set of failed
keys rather than re-hitting the disk every frame.

### Added: the Unity binding rides the convenience tier

`Bwa.cs` binds the eleven new entry points: `bwa_sound_acquire`, `bwa_sound_acquire_async`,
`bwa_sound_is_ready`, `bwa_sound_find`, `bwa_sound_release`, `bwa_group_stop`, `bwa_stop_all`,
`bwa_source_preset`, `bwa_source_create_desc`, `bwa_source_apply`, and `bwa_source_get_desc`,
with `BwaLoadFlags`,
`BwaSourceKind`, and the `BwaSourceDesc` struct. That file claims to bind every `BWA_API` function
except `bwa_set_output_capture` and `bwa_render_block`, a claim that has been false before, so it was
re-checked by diffing the header's `BWA_API` list against the file's `DllImport` list: those two are
the only difference.

`BwaSourceDesc` follows the `BwaTuning` precedent field for field, including the `structSize` guard
and `UnmanagedType.I1` for the C `bool`. `Engine.Awake` now proves that layout rather than trusting
it: `bwa_source_preset` is pure and fills `struct_size` with the engine's own `sizeof`, so comparing
it against `Marshal.SizeOf<BwaSourceDesc>()` catches a field the binding got wrong. A mismatch there
is not a crash, it is the marshaller handing the engine a short buffer to read past, so the check
refuses to start for the same reason the version check does.

**`Engine`'s asset dictionary is gone.** The `"ambix:"` and `"fuma:"` key prefixes existed only
because the ABI had four separate loaders; the engine keys on `(path, flags)` itself now, so
`Engine.Acquire(clip, flags)` is the one path and `Load` / `LoadAmbix` / `LoadFuma` are one-liners
over it. `LoadStreaming` comes free with the flag. Behavior is unchanged: the same clip returns the
same handle, assets live for the engine's lifetime, and `bwa_destroy` frees them whatever the
refcount, so there is nothing to release at teardown. One small map survives, and it holds no
handles: a **negative** cache of clips that failed to load. A synchronous acquire that fails inserts
no cache entry, so without it a missing clip would re-open the file and re-log the warning on every
`Play`.

**Async loading is opt-in per emitter.** `Emitter.loadAsync` routes `Play` through
`Engine.AcquireAsync`: the call returns at once and the source stays silent until the data lands,
then starts from the clip's first frame. It is off by default because the CAVE's normal path is
load-time and synchronous. The not-ready window is handled where the ABI says it must be:
`Emitter.Queue` and `Emitter.PlayOneShot` refuse a still-decoding clip and say so, because neither
can be held, and the emitter polls `bwa_sound_is_ready` while a load is in flight so a decode that
FAILS gets reported instead of leaving a silent source and no explanation.

**Sources configure in one call.** `SourceBase` gains `BuildDesc`, `ApplyDesc`, `TryGetDesc`,
`ApplyPreset(kind)`, and the editor `Reset`, and the create-time push is now a single
`bwa_source_apply` instead of the seventeen setters it used to issue. That matters when a prefab
spawns: the engine packs the audio-thread knobs into one ring command, and `bwa_play_oneshot`
already documents dropping when the ring is momentarily full. The per-property setters and
`OnValidate`'s live re-push are untouched, so an inspector drag still behaves exactly as before. The
inspector fields stay the source of truth in both directions, which is why `ApplyPreset` writes them
rather than configuring the source behind their back, and why the new preset picker in the source
inspector is undoable. `bwa_source_preset` is pure, but it is still a P/Invoke, so both entry points
into it from the editor (the picker's Apply button and the component's `Reset`) catch
`DllNotFoundException`: a project that has not staged `bw_audio.dll` yet would otherwise throw from
inside `OnInspectorGUI` and lose the whole inspector on every repaint.

`Engine.StopGroup(group)` and `Engine.StopAll()` sit beside the existing group gain and pause
wrappers. Both are click-free, both stop beds, and neither resets the mixer.

### Added: `AmbisonicBed` loads asynchronously too

`AmbisonicBed.loadAsync` is the opt-in `Emitter` already had, on the component that gains most from
it: a bed is a 4, 9, or 16 channel file and usually a long one, so it is the biggest in-frame decode
a scene does. `Play` routes through `Engine.AcquireAsync`, returns at once, and the field starts from
its first frame on the block its data lands. It is off by default for the same reason the emitter's
is, that the CAVE's normal path is load-time and synchronous.

The acquire flags carry the kind, and that is what makes an async bed play safe. `fumaClip` picks
`BWA_LOAD_AMBIX` or `BWA_LOAD_FUMA`, and the engine judges the play against those flags rather than
against a channel count, because a still-decoding asset reports 0 channels and has no count to judge.
A handle acquired as mono is refused at `bwa_bed_play` itself, and as a backstop a held play whose
asset lands as the other kind is dropped rather than bound, with the `bwa_commit` that dropped it
reporting so. The window between the acquire and the decode is guarded at both ends: the bed either
plays as a soundfield or does not play at all.

A bed is world-locked, so it has no per-frame push to hang a readiness poll on the way an emitter
does. The watch is a coroutine that exists only while a load is in flight, which is why a bed still
costs nothing per frame when it is not loading. The poll itself is now `Engine.WatchPendingLoad`,
shared by both components: a decode that FAILS never becomes ready, so silence alone cannot separate
"still decoding" from "failed", and the one warning that separates them has a single implementation
instead of two that drift. `Emitter` keeps its behavior and the exact wording of its warning.

### Added: the Godot binding rides the convenience tier

**`BwaEngine`'s asset cache is the engine's now.** The `"m:"` / `"s:"` / `"a:"` / `"f:"` key
prefixes are gone, and with them the fan-out `unload_sound_path` had to do over all four. They
existed only because the ABI had four separate loaders with no shared key. `load_sound` and
`load_ambisonic` are one-liners over one `acquire_sound(path, flags, async)`, and the binding no
longer deduplicates or counts references.

What could NOT be deleted, and why: `unload_sound_path(path)` and `sound_is_ready(path, flags)` are
questions about the references this NODE owns, and the ABI cannot answer those. So `BwaEngine` keeps
a flat record of the `(path, flags)` keys it acquired, holding one reference each. It is an ownership
ledger now, not a cache. `sound_get_frames(path)` and `sound_get_channels(path)` do not consult it:
they go through `bwa_sound_find` (added for this, above), which never loads on a miss and so
cannot restore the hidden-decode bug those getters were fixed for.

**Three new by-path calls on `BwaEngine`:** `preload_sound(path, flags)` warms the cache before the
first play, `preload_sound_async(path, flags)` starts the decode without a player, and
`sound_is_ready(path, flags)` reports the landing. `flags` is a bound bitfield, `LOAD_MEMORY` /
`LOAD_STREAM` / `LOAD_AMBIX` / `LOAD_FUMA`, so the four loaders are one argument here too.
`group_stop(group)` and `stop_all()` join the group gain and pause wrappers.

**Async loading is opt-in per player.** `async_load` on `BwaEmitter` and on `BwaBed` is off by
default, for the same reason as Unity's: the CAVE's normal path is load-time and synchronous. A bed
gets the switch as well because a soundfield is the case that most wants it. The not-ready window is
handled where it bites: an emitter's end detector reads the voice as "not playing" for the whole
hold, so it would spend its four-frame grace and announce a `finished` for a sound that had not
started. The grace now only runs once the handle is READY, and a decode that FAILS is reported and
clears the detector instead of leaving it pending forever. `is_loading()` exposes the window, and
`queue` stays synchronous because the core refuses to resolve a queue entry against a not-ready
handle.

**Sources configure in one value.** `BwaSource` gains `get_desc()`, `apply_desc(dict)`,
`reset_to_preset(kind)`, and the static, engine-free `BwaSource.get_preset(kind)`, mirroring
`get_setup_tuning` / `apply_setup` for the engine knobs, and for the same stated reason: a
Dictionary can be printed and diffed. `apply_desc` OVERLAYS, so only the keys present change and a
`get_desc` round trip is a no-op. Creation now goes through `bwa_source_create_desc`, so a source
arrives configured in one ring command instead of fifteen, and the desc is validated before a voice
is allocated. `BwaPushSource` creates first and applies second, since a push voice has no
`create_desc` form. Applying a desc writes the node's own properties too, so the inspector cannot
disagree with what the engine is rendering.

`demo/api.tscn` covers the lot on both sinks: the preset table, the desc round trip and overlay, the
preload and unload path, the async landing with a check that no `finished` fires during the hold,
and both scene stops.

### Fixed: an async asset could be bound as the wrong KIND, silently

Found in review of the async tier, before it shipped.

The engine tells a mono point source from an ambisonic bed by channel count, and both play calls
guard on it: `bwa_source_play` refuses a multichannel asset, `bwa_bed_play` requires one. A
still-decoding `bwa_sound_acquire_async` handle reports 0 channels, so it passed both guards. Play a
pending AmbiX handle on a point source and the play was accepted, held, and bound to a bed when the
decode landed. The mixer dispatches on the asset, so it then rendered as a soundfield, and the
spread, directivity, and panner settings the caller had dialed in did nothing. Nothing reported
this, at the call or afterwards. A pending mono asset sailed through `bwa_bed_play` the same way.
`bwa_play_oneshot` and `bwa_source_queue` were never affected, because they refuse a not-ready
handle outright.

The load flags already fix the kind at acquire time, and the asset cache keeps them per entry, so
the mismatch is now refused at the play call itself with the reason in `bwa_last_error`. That is
where a caller can still do something about it. Behind that, a held play carries the kind it was
issued as (one byte), and `rt_sound_publish` re-checks it against the real channel count once the
data lands: a mismatch is dropped rather than bound, and the `bwa_commit` that dropped it says so
through `bwa_last_error`, so a client that never polls readiness is not left with silence and no
trace. Beds reach the core through their own `rt_bed_play` entry point now, because a voice cannot
be asked afterwards which kind the caller meant.

That backstop notice is the one exception to the documented `bwa_last_error` rule, which says a
per-frame call that merely enqueues never sets it and that you should read it right after the call
you are checking. The drop belongs to a play call that returned successfully several frames earlier,
so there is no call to attribute it to. Both `bwa_last_error` and `bwa_commit` now say this at their
declarations rather than leaving the header contradicting itself.

### Fixed: a play could cancel a voice steal, and stale-handle directivity could poison a live source

Both are pre-existing holes that the convenience tier made easier to reach, found in review of it.

`CMD_PLAY` cleared a voice's `stopping` flag unconditionally, which silently downgraded a
steal-in-progress. `CMD_STOP` right beside it already knew not to do that. A steal has already handed
the caller a replacement handle on a reserve slot and is waiting on the victim to fade, free, and
acknowledge; resurrecting the victim cancels that acknowledgement, so its `stealing` flag stays set
and the source can never be stolen again for as long as it lives, leaving the pool a slot short. A
client playing a mid-steal handle could always reach this. What changed is that `rt_sound_publish`
can now re-issue a held play by itself, so the engine could do it to itself at the timing of a
decode landing. `CMD_PLAY` now refuses to downgrade a steal.

`bwa_source_set_directivity` wrote its control-side cache with only a bounds check, no generation
gate, even though the comment on that cache block claims every per-source setter is gated. A stale
handle could therefore scribble a value that the slot's NEXT occupant inherited. That used to be
harmless, because the rt and sim calls drop a stale handle on their own. It stopped being harmless
when `bwa_source_apply` began reading that cache to skip a directivity change that already matches,
and `bwa_source_get_desc` began reporting from it: the apply would conclude "already matches" and
skip a real change, so the source rendered omni while the readback claimed figure-8. Now gated like
every other setter.

Two smaller hardening fixes alongside: `rt_unload_sound` now refuses a reserved (async, not yet
published) sound slot, which is the invariant the staging comment states and the function it names
(unreachable through the public ABI today, so this guards the next internal caller); and
`rt_stop_all` pushes its command before dropping held plays, so a momentarily full ring no longer
leaves the half-effect of voices still playing with the pending plays silently gone.

### Fixed: a stopped one-shot leaked its voice slot forever

`pause_gate`'s stop-finalize freed the voice slot only for a steal. A one-shot's handle is
engine-internal, so nothing else ever recycles it, and the natural-end path had always done this
free. The bug was unreachable until now because no public call could stop a one-shot mid-play; the
new voice-table sweeps reach it, and without the fix a scene transition would burn a slot per
one-shot until the pool ran dry. Found while testing `bwa_stop_all`, with a regression test that
fails without the fix.

### Fixed: a recycled source slot inherited the previous occupant's directivity

`bwa_source_create` did not reset the control-side `src_fwd` / `src_dirw` / `src_dirp` / `src_pos`
caches for a reused slot, while rt had already cleared the corresponding `Voice`. The two now agree.

## [0.6.0]

### Fixed: the default speaker grid was unreachable, and the binding claimed a failed layout was survivable

`Engine` always passed an explicit `layout_path`, so there was no way to express `layout_path = NULL`,
the ABI's only route to the built-in default 26-speaker grid. An empty Layout File field produced the
StreamingAssets *directory* as the path, which fails to load — and since the `BWA_ERR_LAYOUT` change
`bwa_start` REFUSES a failed explicit layout. Concrete failure: a fresh project on the default
Binaural profile without `StreamingAssets/cave_layout.json` got no audio while three pieces of binding
text (the inspector tooltip, the startup log, the editor HelpBox) still described the old
falls-back-and-carries-on contract.

- **An empty Layout File now maps to `layout_path = NULL`** — the deliberate opt-in to the default
  grid, exactly like the sibling `asioDriver` field's empty-to-null mapping.
- All three texts now state the real contract: an explicit path must load, or `bwa_start` refuses
  with `BWA_ERR_LAYOUT`. The startup error for a failed explicit load says what will happen (start
  will refuse) and both fixes (fix the file, or clear the field); the editor HelpBox for a missing
  layout file is an Error now, not a Warning, because the scene will start with no audio.

### Added: startup ABI version check (`bwa_get_version`)

`Bwa.cs` bound `bwa_get_version` and promised verification that never happened; only `bwa_tuning`
had a `struct_size` guard. The exposure is the in-repo dev loop: a stale staged `bw_audio.dll` after
an ABI break runs with mismatched enums and struct layouts — silent corruption, not a crash.
`Bwa.BoundVersion` now records the header revision the bindings were written against (0.11.0), and
`Engine.Awake` compares it against the DLL's before `bwa_create`: a **major.minor** mismatch logs
both versions and refuses to start (the header guarantees enum values and struct layouts only within
a major.minor); a patch difference is compatible and passes silently.

### Fixed: stale handles could reach a successor engine (`AmbisonicBed`, `DynamicAcousticGeometry`)

`SourceBase` already refused to let a handle minted under a destroyed `Engine` act on a replacement
(a fresh engine's first handle is slot 0, gen 1 again — deterministic collision), but the bed and
dynamic-mesh components lacked the guard: a stale bed handle would drive (or in `OnDisable`
*destroy*) whatever occupied the colliding slot in a successor engine, and the dynamic-mesh handle
is a plain index with no generation at all, so `RemoveDynamicMesh(0)` removed a foreign mesh. Both
components now carry `SourceBase`'s `_owner` pattern: once the creating engine is gone, every
operation (the destroy paths included) no-ops, and the next enable re-creates under the new engine.

### Fixed: `Bwa.cs` is 1:1 with the header again (two bindings were missing)

The completeness claim ("every `BWA_API` function except `bwa_set_output_capture` and
`bwa_render_block`") had drifted false by two: `bwa_spcap_focus_default` and `bwa_bed_gains_batch`
were unbound. Both are pure, engine-free evaluators with the same marshaling shape as the already
bound `bwa_panner_gains_batch`, so they are bound now and the claim is true as written.

### Added: `SourceBase.AirAbsorption` live property

The `airAbsorption` field as a live property, pushed like `Spread`/`SizeMeters`; setting the field
alone changed nothing until the next re-enable.

### Added: compensated amplitude panning (`bwa_set_dual_band_cap`)

Dual-band's low band aims the velocity vector at the source and takes whatever `|rV| < 1` the speaker
geometry gives. The shortfall is direction-dependent, so the rendered interaural time difference falls
short of a real source's by a varying amount and the image **shifts when you turn your head**. CAP
(Menzies and Fazi) constrains the one quantity the ear reads below the crossover instead: the
interaural component of the summed field, `rV . e == u_s . e`. Matching one scalar is satisfiable
where matching a 3-vector is not, so the ITD comes out exact and stays exact as the head turns.

- **`bwa_set_dual_band_cap(e, on)`** is off by default, live-toggleable, and it **requires `bwa_set_dual_band`**
  since the low band is the only thing it touches. Applied as a projection on top of the selected
  panner rather than as a fourth panner, so it inherits DBAP's or VBAP's image and only corrects ITD:
  facing the source it is a no-op and reduces to the seed panner, and it fades out with
  `bwa_source_set_spread` (an engulfing source has no single bearing to fix).
  Renamed from `bwa_set_cap` before release: `cap` already means CAPACITY in three calls in this same
  header, and carrying the parent toggle's name matches `bwa_set_max_re` -> `bwa_set_max_re_split`.
  Unity: a `dualBandCap` inspector toggle plus `SetDualBandCap(bool)`. Godot: the `dual_band_cap`
  property and `BwaEngine.set_dual_band_cap`.
- **This is the first engine feature that reads head orientation into the speaker path.** Everywhere
  else orientation enters only at the binaural decode, so `CMD_COMMIT` now dirties voices on a
  quaternion change, gated on CAP being on so a tracked head does not re-solve every voice every
  block for a rotation nothing downstream reads. Wants a real tracked pose; aimed at the seated case,
  where it rebuilds no panner cache at all (everything is computed in room space, so a head that only
  turns invalidates neither the SPCAP nor the VBAP direction cache).
- Measured over a full head-yaw sweep on the default grid, worst `|rV.e - u_s.e|` is **0.017 with CAP
  against 0.404 without**. CAP's entire residual is the 2 of 24 yaw angles where the target is not
  achievable: `rV` is a convex combination of speaker directions, so no non-negative gain vector can
  render an ITD **more lateral than the most lateral speaker the panner lit**. CAP saturates at that
  bound rather than diverging. It is an array-density limit, not a defect.
- Not implemented: the published method's near-field ILD arm (one first-order filter per image). It
  needs per-speaker frequency-dependent gain, which would make this a render mode rather than a
  gain-vector modifier. The near-field proximity shelf and near-listener widening cover adjacent
  ground. See docs/spatialization.md for how this differs from VISR's own CAP, which minimizes energy
  and permits negative gains where this minimizes change from the seed and does not.

### Added: situation tuning (`bwa_tuning_preset` / `bwa_apply_tuning`)

Fourteen rendering knobs is a lot to get right, and the right answer depends on whether the listener
sits or roams. `bwa_setup` names that, `bwa_tuning_preset` fills a complete `bwa_tuning` for it, and
`bwa_apply_tuning` pushes every live knob in one call.

- **Fill-then-apply, not apply-a-preset.** You can print what the preset chose, which matters because
  most of these values are contested; preset then override then apply composes where
  apply-then-re-override is order dependent; `bwa_tuning_preset` is pure so a tool can show the table
  off-hardware; and two setups diff field by field, which is the natural rig-day question. Unity
  marshals the struct directly. Godot gets `get_setup_tuning` (a Dictionary, so it is inspectable
  there too) plus `apply_setup`, which also mirrors into the node properties so the inspector cannot
  lie about the live state.
- **Orthogonal to `bwa_profile`.** Profile is what you render to, setup is how the listener uses it.
  Seated-CAVE, roaming-CAVE and seated-binaural are all real combinations.
- **This struct's zero is NOT its default**, unlike `bwa_desc`. A zero-filled `bwa_tuning` would force
  max-rE off, which stopped being the engine default. `struct_size` makes that fail loudly:
  `bwa_apply_tuning` refuses a wrong-sized struct rather than silently misconfiguring the render.
- **Seated and roaming differ in exactly three fields** today (`panner`, `dual_band`,
  `dual_band_cap`), and `smoke` asserts that count. That is not an oversight, it is what the evidence
  supports: most knobs are still rig-day questions and are left at the engine default rather than
  guessed. docs/api.md carries a per-field evidence table marking each value as measured, design
  intent, or unmeasured, so a preset cannot quietly become folklore.

### Fixed: a finite but un-normalized listener quaternion poisoned the render

Found by the new seeded fuzzer (`test_fuzz_api`), and a direct follow-on to the NaN sweep below: the
finite guard added to `bwa_set_listener_pose` was NOT enough. Every consumer of that quaternion assumes
a unit (`frame_qrot` says so outright). A finite quaternion of large magnitude, with a component around
1e6 from an uninitialized or un-normalized caller pose, overflowed the rotation math downstream. It
drove the render non-finite exactly as a NaN would. `rt_set_listener` normalizes now, with a degenerate or zero
quaternion falling back to identity rather than being dropped, so a caller who never set an orientation
still gets a listener facing room ahead. Doing it once at the edge means no consumer has to.

The fuzzer had clamped the value behind a comment naming the defect and its reproducing seed rather
than hiding it; that clamp is gone and the un-normalized case is fuzzed for real now. Seeds 104, 106
and 119 reproduced it; all three pass, plus 10 fresh seeds at 8000 operations each.

Doc correction from the same run: `bwa_get_health` claimed `out` is zeroed when the numbers are not
measurable, but `stream_starves` is deliberately filled either way, because the stream ring is the
engine's rather than the device's. The header said one thing and the implementation another.

### Fixed: NaN could reach the device through twelve entry points

A deliberate-misuse test suite (`test_abuse`, new) fed non-finite values into every float parameter on
the ABI and asserted the device-bound output stays finite. Twelve assertions failed, all one bug class:
a two-sided clamp written `x < lo ? lo : (x > hi ? hi : x)` passes NaN straight through, because every
comparison against NaN is false. The engine already had the right idiom in places (`isfinite` guards
at the ABI edge, "keep NaN/Inf off the audio thread"), so this was an asymmetry rather than an
oversight, which is exactly the kind of thing a systematic sweep finds and spot checks do not.

**Eight of the twelve reached the rendered output**: `bwa_set_master_gain` (NaN and Inf, which NaNs the
whole bus), `bwa_group_set_gain`, `bwa_source_fade_to` in both the target and the DURATION (a NaN
duration slips `seconds <= 0` and makes the fade rate NaN), `bwa_source_set_pitch`,
`bwa_set_test_signal`, `bwa_source_set_occlusion_manual`, and `bwa_set_listener_pose`. That last one
is the worst: it had no finite guard at all while `bwa_source_set_pos` did, a NaN listener NaNs every
panner solve, and once NaN is in a gain ramp the interpolation `x + (t - x) * k` can never leave it,
so it survived until the engine restarted.

**Four poisoned a readback**: the listener pose, `near_spread`, `hole_spread`, and the SPCAP knobs,
where `bwa_get_tuning` reported the raw argument although rt clamps focus to 64. That one broke the
readback's own stated rule, so `rt_get_spcap_sanitized` was added alongside the existing sanitized
reads and `bwa_get_tuning` now reports post-clamp values for every field it covers.

All twelve are fixed, and the suite pins them. Two idioms are now used deliberately: `isfinite` to
REJECT at the ABI edge where a bad value has no sensible interpretation, and `!(x > lo)` where the
value should clamp, because that reads false for NaN and lands on the floor.

Also new: **`test_fuzz_api`**, seeded random API call sequences (deterministic per seed, so a failure
replays from its seed alone), and `-DBWA_ASAN=ON` now instruments `test_sound`, `test_abuse` and
`test_fuzz_api` rather than `test_sound` alone. AddressSanitizer reported **zero diagnostics** across
the misuse suite: no heap overflow, use-after-free or out-of-bounds anywhere in those paths, including
the NaN-pitch cursor. Note the ASAN build needs `clang_rt.asan_dynamic-x86_64.dll` on PATH (a VS dev
prompt provides it).

### Added: the four API-ergonomics fixes (completion events, scene composition, tuning readback)

An API-usability review looked at the real call sequences rather than the declarations, and found the
friction concentrated where BOTH bindings had independently invented the same workaround. That is the
tell, so all four are fixed at the C level and the workarounds can go.

- **`bwa_poll_ended(e, out, cap, dropped_out)`: completion as an EVENT.** Godot had built a
  three-state machine with a 4-frame grace timeout that FABRICATED a `finished` signal for clips it
  never observed, Unity a `_wasPlaying` edge detector that admits it can miss short clips, and
  `minimal.c` a literal `Sleep(50)`. All three existed because completion was poll-only. Drain the
  handles instead. Handles come back as you knew them (before the generation bump) so a compare
  works; unpolled events are bounded and drop OLDEST, and `dropped_out` tells you which happened.
  - This needed a **new event**, not the existing one. `EVT_VOICE_ENDED` fires only for one-shots and
    steals, because it means "recycle this transient handle" and not "this voice finished". A plain
    source finishing posted nothing at all. `EVT_VOICE_DONE` is the notification: same ring, handle
    stays yours, nothing is recycled.
- **`bwa_source_is_playing` no longer lies about a RE-play.** The published word carries a play
  SEQUENCE now, because it alone cannot tell "not playing, before your play" from "after it": same
  generation, same 0 bit. A re-play on a handle whose voice already ended used to read false until
  the next rendered block, while `docs/api.md` claimed the opposite in as many words. Prefer
  `bwa_poll_ended` for completion regardless; is_playing still cannot see a clip shorter than your
  poll interval, and no sequence fixes that.
- **Scene composition no longer needs a lie or a re-derivation.** `bwa_scene_set_mesh_mat` **clears**
  the static mesh when passed NULL geometry, so removing it does not mean replacing it with something
  harmless (Unity was pushing a degenerate 2 cm triangle parked at (1000, 1000, 1000)). And
  `bwa_scene_set_box` is now the two calls it was always made of: **`bwa_scene_set_ism_room`** for the
  shoebox alone, and **`bwa_box_mesh`** (pure, no engine) for its 12 inward-facing triangles, so the
  box composes with your own geometry instead of replacing it. Godot was re-appending the box after
  every call; Unity was hand-rolling the triangles, flip-toward-center subtlety included. `set_box`
  itself is unchanged for the box-IS-the-scene case, and neither of the new calls is order-dependent.
- **Two reverb beds no longer resolve silently.** Configuring both `bwa_reflections_config` and
  `bwa_fdn_config` shares one reverb tap; the FDN won and the Steam bed was skipped without a word,
  so both bindings had added their own warning. The engine now says which one took the tap, once,
  the same way the ISM-plus-Steam double-render is reported.
- **`bwa_get_tuning(e, out)`: the readback half of `bwa_apply_tuning`.** Godot's `apply_setup` was
  hand-mirroring 16 fields to stop its inspector lying about live state, and every new knob had to be
  added in four places. The engine now shadows the whole struct, updated by the individual setters
  AND by `bwa_apply_tuning`, so the two writer paths cannot drift. Godot also gets `get_live_tuning`
  and `poll_ended`. This is not per-knob getters, which the live A/B surface still deliberately lacks.
- Doc correction while in the area: the stated rule that per-frame calls never touch
  `bwa_last_error` was false. Per-frame calls that **reject** set it; ones that merely enqueue do not.
  A successful call that DEGRADED something also sets it now (the two-beds notice), which the contract
  says explicitly rather than leaving as an exception readers have to discover.

**A review of this work found eight defects in it, all fixed before this entry was written.** Recording
them because two were the kind that pass every test:

- **`bwa_start` could hang forever.** `steam_scene_flush` claimed a staged mesh change but never read
  the new clear flag, so a staged clear left it waiting on a generation the sim thread could no longer
  apply. `pend_clear` is part of the staged tuple now, taken under the same lock as the buffers by both
  consumers, and a real set supersedes a staged clear instead of poisoning the next one.
- **The event ring could overflow.** `EVT_VOICE_DONE` broke the sizing argument the ring rests on: a
  plain voice re-arms with just a play, needing no drain, so completions are unbounded where the
  ownership acks are not. A full ring would then have dropped an `EVT_VOICE_ENDED` (leaking a voice
  slot) or an `EVT_SOUND_RETIRED` (never freeing the buffer), which is the exact failure the
  retire-ack handshake exists to prevent. Completions now yield `voice_cap + sound_cap` slots of
  headroom and count their own refusals into `dropped_out`.
- A play the command ring refused left `play_seq` permanently ahead, so `bwa_source_is_playing`
  answered true forever with no voice. The bump is rolled back when the push fails.
- A finished one-shot reported **twice**, once with a handle the caller never held. Only the
  notification reports now; the recycle event does not, and a steal is not a completion at all.
- A completion could be attributed to the **wrong play** of the same handle. Notices carry their play
  sequence and a superseded one is dropped, so a client cannot free or re-trigger the wrong play.
- `bwa_get_tuning` reported raw arguments where rt clamps them, so the readback whose job is to stop
  an inspector lying could itself lie. It reads the post-clamp values now.
- Godot lost the room's walls from the ray-traced scene entirely for the commonest setup (a room box
  and nothing else), because an early-out still assumed `scene_set_box` had committed the mesh.
- Plus contract corrections: `bwa_poll_ended` does not hand back recycled handles for plain sources,
  and an error message named a function the caller never called.

### Changed (ABI): enum value 0 reserved for default-init, and every public enum is width-pinned

Two related changes to the enum surface, taken while the C ABI for 0.11.0 is still unreleased.

> **MIGRATION, and the one place "unreleased" does not cover it.** The C ABI is unreleased; the
> **bindings' serialized-scene contract is not**. The 0.4.0 packs shipped `BwaBedDecoder` /
> `BedDecoder`, and both Godot `.tscn` and Unity YAML store an inspector enum as a bare integer. A
> scene saved before this change with **EPAD** stored `1`, which now reads back as `DECODE_ALLRAD`.
> Nothing warns, because 1 is still a valid value. **Re-pick the bed decoder in any scene or prefab
> saved before 0.11.0.** Scenes that used AllRAD stored `0` and now read `DECODE_DEFAULT`, which
> resolves to AllRAD, so those are correct by luck rather than by design. The renumbering was kept
> anyway rather than mapping at the binding seam, because a binding whose enum numbering permanently
> disagrees with the C header is a worse long-term trap than a one-time re-pick.

- **`bwa_bed_decoder` reserves value 0.** It is now `BWA_DECODE_DEFAULT = 0`, `BWA_DECODE_ALLRAD = 1`,
  `BWA_DECODE_EPAD = 2`. A zero-filled `bwa_desc` asks for the engine's current default rather than
  naming an algorithm, which is what the struct's own "every field's zero is its default" contract
  always claimed. Behavior today is unchanged, since the default resolves to AllRAD. The point is that
  the default can move later without an ABI break, and without silently re-pointing a caller who did
  name a decoder. `engine.c`'s `resolve_bed_decoder` is the one line that says what the default is.
  This is the sokol convention (`_SG_PIXELFORMAT_DEFAULT` and 21 siblings, "value 0 reserved for
  default-init"), and it is the mechanism the max-rE flip showed was missing: that knob was a live
  setter so its default moved in one line, while the decoder's was welded to an enum value.
  **Update call sites that passed a literal `0` or `1`** rather than the named constants. Unity's
  `BwaBedDecoder` and Godot's `BedDecoder` mirror the new numbering, and both inspector defaults are
  now `Default` so a fresh scene tracks the engine instead of pinning AllRAD.
- **Every public enum gained `*_FORCE_U32 = 0x7FFFFFFF`.** C leaves an enum's underlying type
  implementation-defined; this DLL is consumed by C# P/Invoke and a GDExtension, both of which marshal
  enums as 4-byte ints. MSVC would not shrink these today, so this is defensive, but a width mismatch
  at that boundary is silent corruption rather than a compile error. Never pass it. Only enums are
  affected, not the `bwa_*_desc` structs.

Only `bwa_desc.bed_decoder` needed the reserved zero. The setter enums (`bwa_set_panner`,
`bwa_set_spread_mode`, `bwa_set_bed_renderer`, `bwa_source_set_directivity`, `bwa_set_test_signal`)
do not, because no zero-initialized struct carries them: the caller always passes an explicit value
and the engine's own default lives in `rt_create`. `bwa_sink_type` already worked this way, since
`BWA_SINK_AUTO = 0` is a default sentinel under another name.

### Changed: max-rE bed weighting now defaults to ON

`bwa_set_max_re` defaulted to OFF through the bake-off. The offline evidence flipped it: the layout
tool's bed metric, which scores through the engine's own AllRAD and EPAD builds, has the taper winning
**every axis on this array, under both decoders and both observer models, including at the sweet
spot** where classical theory says the plain decode should win. An irregular 26-speaker array's decode
sidelobes bend rE even at center, and the taper suppresses them.

- This is the **diffuse layer only**. Point-source panning is untouched, and so is `BWA_PROFILE_BINAURAL`,
  where the taper is gated off anyway. What changes is ambisonic beds, the reflection bed and the FDN
  reverb's line render.
- The rig trial in `docs/hardware-validation.md` now **confirms rather than gates**. If the rig
  disagrees, revert the default. Turn it off with `bwa_set_max_re(e, false)`.
- `bwa_set_max_re_split` stays OFF. Nothing in the evidence speaks to the band split, so the broadband
  taper remains the incumbent.
- The layout tool's `--score` default moved with it: it grades AllRAD **with** max-rE now, because a
  scorer whose default disagrees with the engine grades a render nobody ships. Pass `maxre` only if
  you turned the taper off some other way.
- Unity's `maxRe` and Godot's `max_re` inspector defaults moved too, so a fresh scene matches the
  engine rather than quietly overriding it on the first push.

**Three tests were measuring nothing** and this exposed them: they read their A-side from the engine
default instead of setting it, so with the taper on by default the max-rE comparison read
`rear share 0.056 -> 0.056` and passed. Set explicitly, the same test now reads **0.210 -> 0.056**. The
limiter's own tests had the same shape and were made explicit as well. A sweep of the remaining
defaults found no other test leaning on one.

### Changed: `bwa_validate` renders through the engine, and sweeps the render knobs

`bwa_validate` built its speaker feeds by calling the panner solves directly and applying only the
layout's static gain and delay. That is a partial reimplementation of the render path, so the tool
could not see dual-band, CAP, the spread modes, decorrelation, the hole-aware floor or tracked
alignment: everything that lives downstream of the gain solve. The phantom arm now places a voice in a
real engine core and pumps blocks through `rt_render` into the 26-channel bus, after `align.c`. What
it measures is what the array would emit.

- Engine-rendered feeds reproduce the old builder to **1.1e-7 of peak** with every knob off, which is
  one float ULP: the engine multiplies stimulus by gain by trim in floats where the old builder folded
  gain and trim into one double. Same solve, same trim, same delay, different rounding. Pinned.
- New sweep axes with matching flags: `--dual-band`, `--cap`, `--tracked-align`, `--decorrelation`,
  `--spread-mode`, `--hole-spread`, `--near-spread`, plus `--spread` for the source width the width
  knobs act on. Default is **one knob at a time** against a baseline (N extra passes); `--factorial`
  takes the cross product. The condition table and cell count print before anything is measured.
  `--cap on` with `--dual-band off` is rejected rather than silently rendering nothing.
- The **physical reference arm is unchanged** and still uses the direct builder. It is the measurement
  floor, it involves no panner and no knob, and it must not acquire new dependencies.
- Measured, and worth reading before rig day: tracked alignment is the largest effect (comb 8.54 to
  0.80 dB off-center on a calibrated layout, with a clean null at the reference) but **only on a
  calibrated layout**. On an unaligned one it can measure worse. The hole-aware floor's benefit is
  invisible to these estimators while its angular cost is not, so it cannot be A/B'd on these numbers
  alone. Dual-band and CAP barely move: they act below 700 Hz and the analysis band is 400 to 1200 Hz.
- Cost: the tool got about 3x slower (`--simulate` 12 s to 37 s), because per-capsule propagation
  replaced an analytic collapse. Still off-hardware and still deterministic, bit-identical run to run.

### Added: re-align the array to the tracked listener (`bwa_set_tracked_align`)

The per-speaker delay and gain trims align arrival times at ONE fixed point, `Layout.ref`, so the
array is time-coherent there and progressively less so as the listener walks away. This re-references
that alignment onto the tracked head, in the output stage, so coherence follows the listener. Same
idea as VISR's `librcl/listener_compensation`.

- **`bwa_set_tracked_align(e, on)`** plus **`bwa_set_tracked_align_guards(e, dead_zone_m,
  slew_frames_per_s)`**, off by default. The enable and its tuning are separate calls, matching
  `bwa_set_limiter` / `bwa_set_limiter_ceiling`, so A/B-ing the toggle never resets a dialed guard.
  Per speaker the correction is pure geometry against `Layout.ref`: `extra delay = (dref - dlis) * rate / c` and
  `extra gain = dlis / dref`, so walking toward a speaker delays it further and turns it down. The
  delay set is shifted to a minimum of zero, which keeps it purely relative and makes a listener
  standing at `ref` **bit-exact** identity rather than approximately so. Both `<= 0` arguments revert
  that one knob to its default, the same sentinel `bwa_set_spcap_focus` uses. Unity: `trackedAlign` /
  `trackedAlignDeadZone` / `trackedAlignSlewFramesPerSecond` plus `SetTrackedAlign(bool)` and
  `SetTrackedAlignGuards(float, float)`. Godot: the matching
  properties and `BwaEngine.set_tracked_align`.
- **Off by default because every delay change is a resampling event.** A walking listener means 26
  delay lines gliding at once. Two guards, both tunable: a **dead zone** (default 0.05 m, since
  tracker jitter would otherwise keep the array permanently gliding) and a **rate limit** (default
  about 63 frames/s at 48 kHz, stated internally as a 0.45 m/s closing speed so it means the same
  thing at any sample rate or speed of sound). That rate over the sample rate IS the resampling
  ratio, so the default bounds the pitch shift at 0.13%, about 2.3 cents. A brisk walk outruns it on
  purpose: stale alignment is the cheap failure, warble is not.
- Nothing here touches the gain solve, so it composes with every panner and re-solves nothing, and
  it needs no `pan_gen` bump. It reads the active listener position, so it fires on **both** listener
  paths, the committed pose and the internal tracker that writes that field directly.
- **Two limits worth knowing before rig day.** The level trim is clamped to +/-6 dB, and that clamp
  binds inside the working area: on the shipped grid a listener 1 m off center already asks for
  -9.5 dB on the nearest speaker, and at 1.5 m is standing on one. Treat the level half as partial
  (the delay half stays exact). And the min-subtraction adds position-dependent latency, about 280
  frames (5.8 ms) at 1 m off center, which `bwa_get_output_latency_frames` does **not** report, so a
  client syncing video against the DSP clock will drift as the listener walks.
- Untested on hardware. Whether the coherence gain beats the warble the rate limit still lets through
  is a by-ear rig call.

### Added: hole-aware spread floor (`bwa_set_hole_spread`)

The real array is a barrel, open at both poles, so a source aimed into a pole has no speaker anywhere
near it. The panner's hull closes the hole with big triangles of distant speakers: the rendered
direction stays about right, but the energy is carried by speakers up to 113 degrees apart, which is
a split image rather than a phantom. Imaginary pole speakers were tried and rejected (see below).
This is the other fix. A source with no speaker near it is genuinely not a point, so stop asking the
array to pretend, and widen it into an honest diffuse source instead.

- **`bwa_set_hole_spread(e, strength)`** is off by default (`strength` 0), live, and clamped at 2.
  Per voice the engine measures one angle, the **gap** from the source bearing to the nearest speaker
  bearing seen from the tracked listener, and floors the voice's effective spread at
  `strength * clamp((gap - knee) / (90 deg - knee), 0, 1)`. Unity: a `holeSpread` inspector slider
  plus `SetHoleSpread(float)`. Godot: the `hole_spread` property and `BwaEngine.set_hole_spread`.
- **`knee` is the array's own mean nearest-neighbor speaker angle**, the same geometry SPCAP derives
  its lobe width from. That measurement moved into a shared `layout_mean_speaker_spacing()` and the
  SPCAP focus derivation now sits on top of it, so the two features read one measurement of the
  geometry instead of mirroring it. The cube grid still derives focus 12.70, unchanged.
- **Inert on a covering array by construction, not by tuning.** No direction on an array that
  surrounds the listener is ever a full speaker spacing from a speaker, so the floor is exactly 0
  there. Measured: the default grid's worst gap over a 4096-direction sweep is 27.3 degrees against a
  37.5 degree knee, and the test pins the worst per-bearing energy change at **exactly 0.00e+00** with
  the knob at full strength. On the barrel the same sweep reaches a 57.8 degree gap and a 0.428 floor.
- Composes with the metric-size and near-listener floors as a **max**, feeds the ordinary spread
  machinery (spread mode, decorrelation, gain ramps), and does nothing under `BWA_PROFILE_BINAURAL`,
  which has no speakers and so no holes.
- **It weakens CAP on the sources it widens**, because CAP's strength is `1 - spread`. That is correct
  (a deliberately wide source has no single bearing whose ITD is worth fixing) but worth knowing
  before A/B-ing the two knobs together, since raising one quietly lowers the other.
- The mapping is a defensible first cut, not a measured optimum. `strength` is the rig-day A/B knob.
  Two known gaps: the offline scoring path (`bwa_panner_gains_batch`) does not run the spread solve,
  so this cannot be swept the way `--focus` sweeps SPCAP, and ISM reflection images bypass it.

### Investigated and rejected: imaginary speakers in the point-source panner

The real array does not surround the listener (speakers mount between the CAVE screen cube and the
truss, so it is a barrel open at both poles). Closing those holes with an imaginary pole speaker, as
`allrad.c` already does for the diffuse bed and as VISR's `<virtualspeaker>` does with explicit
routes, **was tried and made point-source localization worse**: rE direction error against the
intended bearing rose from 14 degrees to 26 (routed to the rim) or 30 (share discarded) at 60 degrees
below the horizon, and only the exact pole improved. An imaginary speaker is a triangulation vertex,
so it claims a share of every direction in the hole and drags it poleward. No code change; the
measurements and the reasoning are recorded in docs/spatialization.md so nobody repeats it.

### Added: SPCAP focus/density tuning, and an ABI break to score it (`BWA_VERSION` → 0.11.0)

SPCAP's lobe sharpness and placement-correction exponent were compile-time `#define`s (12.0 / 2.0).
The 12 was only ever right for the 26-speaker cave, so `focus` now **derives from the array**: the
mean nearest-neighbor angle between speaker directions, then the exponent that puts the lobe 6 dB
down in energy there. The cube grid lands on 12.70; a 6-speaker cross derives 2.0, a 12-speaker ring
20.0. `density` keeps a plain 2.0 (nothing measurable maps onto it).

- **`bwa_set_spcap_focus(e, focus, density)`** retunes SPCAP live, per-frame-safe, and the change
  reaches **every** source on the next block including sources that never move. Pass `0` or less for
  either argument to revert *that one* to its default. Inert under DBAP and VBAP. Neither value
  lives in the layout file: like `bwa_set_near_spread` and `bwa_set_dual_band`, persisting a dialed
  value is your application's business. Unity: `spcapFocus` / `spcapDensity` inspector fields plus
  `SetSpcapFocus(focus, density)`. Godot: `BwaEngine.set_spcap_focus` and
  `BwaEngine.get_spcap_focus_default`.
- **`bwa_spcap_focus_default(positions, n)`** is the pure companion (no engine handle, same contract
  as `bwa_panner_gains_batch`), so a tool can print what an in-progress array implies before you
  override it. Returns 0 on bad arguments.
- **ABI break: `bwa_panner_gains_batch` gained `float focus, float density`** immediately before
  `out`, honoring the same `<= 0` sentinel: either one at or below zero means "the default for THIS
  array", so `focus` falls back to the value derived from the `positions` you passed, `density` to
  2.0. Both are inert under `BWA_PAN_DBAP` and `BWA_PAN_VBAP`. Update call sites: the old
  `(panner, positions, n, lis, srcs, nsrc, out)` becomes
  `(panner, positions, n, lis, srcs, nsrc, 0f, 0f, out)` for identical behavior. This is what lets a
  layout be **scored** at the tuning it ships with. `bwa_layout_tool` now runs its Score board, its
  rE coverage overlay, its badness map and its optimizer cost through the dialed value, with focus
  and density sliders on the analyze panel beside the preview panel's live pair, and a headless
  `--score <file> focus=<n> density=<n>` for sweeping the knob offline.
- Both GUI tools drive the knobs by ear too: the playground's localization scene gained a panner
  combo, focus and density sliders and a "default" button; the layout tool's preview panel the same
  beside its `B` panner A/B. Each prints what the loaded array derives.

### Added: layout-tool conditions, stages, and pinned speaker allocation

What a layout is optimized FOR is now a first-class artifact in `bwa_layout_tool`,
because collaborators disagree about it. A named **condition** bundles the objective
knobs with a scoring-shell restriction: `3d` (the full sphere, the old behavior),
`horizontal` (only source directions within 15 deg of the ear plane, for planar
localization), and `visual` (a front wedge, azimuth and elevation within 30 deg of
+z). The GUI gets a condition combo plus band/azi sliders; the Score board follows
the active condition. Headless, conditions chain as **stages**
(`--optimize file dbap horizontal,3d`), each stage re-anchoring the leash at the
previous result and reporting before/after scores plus ear-plane occupancy.

- Staging alone cannot protect an allocation: objectives compete, and a `3d` stage
  pulls plane speakers back toward elevation (measured on the default dome: 10
  in-slab speakers eroded to 5). Allocation is therefore a CONSTRAINT:
  **`"pin": "plane"`** per speaker (plus top-level `pin_slab_m`) holds a speaker to
  the ear-plane slab through optimizer trials, snap, and optimizer start. Both are
  authoring-only fields; the engine's loader ignores them. With 12 of 26 pinned the
  plane score lands at its ceiling for about 0.2 deg of full-sphere mean.
- `--score [file] [condition] [fixed|moving] [ears=<m>]` scores under a named
  condition, for a seated install (sweet spot only) instead of the roaming grid,
  and at the install's actual ear height. `--optimize` takes the same tokens.
  `ears=` moves everything plane-shaped together: the band, the pin slab, and the
  delay-alignment point written on save.
- `--optimize ... radial` moves speakers only along the ear ray: directions frozen,
  radii free. The cross-panner pass; optimize for VBAP (direction structure), then
  a radial DBAP pass refits distances without disturbing the triangulation.
- `--optimize ... guard=<panner>[:tol]` is the constrained form: while climbing the
  target panner, moves that let the guard panner's cost slip more than tol above
  its stage-start value are rejected. "The best VBAP layout that never lets DBAP
  slip" is DBAP-optimize first, then a guarded VBAP climb.
- The search itself grew past greedy: best-so-far is always tracked and shipped
  (stopping, preview, and headless all restore it), `anneal` adds Metropolis
  acceptance with per-trial cooling, and `restarts=<n>` basin-hops from the best
  layout plus a kick. Motivated by measured multimodality: identical inputs landed
  26.8 vs 31.2 deg worst on different random paths.
- The GUI climb now stops itself at the same step floor the headless runs use
  (it previously ran until stopped by hand), restores the best layout, and reports
  convergence in the HUD.
- `--optimize ... leash=<m>` sets the per-stage displacement cap from the CLI
  (previously GUI-only), and every optimize start projects the incoming layout into
  the room constraints and pin slabs, so an infeasible generated file starts
  feasible instead of leaking through a run.
- New ABI: **`bwa_bed_gains_batch`**, the bed counterpart of `bwa_panner_gains_batch`:
  the per-speaker gains the diffuse-bed decode (AllRAD/EPAD, optional max-rE, the
  engine's real builds) produces for plane-wave directions over a candidate layout.
  The layout tool uses it for an **AMBI** scoreboard/--score row and a
  `bed=<wt>` objective term, so a layout can be optimized for what ambisonic
  content wants (spherical uniformity), not only for the point-source panners.
  `epad` / `allrad` / `maxre` tokens (GUI: bed decode combo + max-rE checkbox)
  grade the decode the install actually ships.
- The measured tradeoffs live in docs/layout-schema.md (pin-count trend, the
  visual-wedge verdict per panner, the leash-matching caveat). Headline: narrow
  conditions express requirements, they are not accuracy shortcuts; DBAP gained
  nothing in the wedge from wedge-only optimization, VBAP's fixed solve did.
- The layout-tool suite grew 15 -> 17 tests: band/wedge scoring on synthetic
  layouts, condition-preset wiring, pin enforcement (including the regression where
  a file-authored pin starting outside its slab was never projected in), and the
  fixed-observer scoring model.

## [0.5.0]

### Added - device health (`bwa_get_health` / `bwa_get_xruns`)

The third dogfooding gap, and the one that cost three probes: every readback described the RENDER -
voices, meters, the clock - and nothing counted blocks the device asked for and didn't get. You could
prove the render path was clean and still not know whether the callback missed its deadline.

- **`bwa_get_xruns(e)`** answers "is the device being starved?" in one line. **`bwa_get_health(e, &h)`**
  fills a `bwa_health` when you need the diagnosis: `blocks` (the denominator), `xruns` and
  `dropped_frames` (the device ran on without us), `driver_resyncs` (the driver reporting a
  discontinuity itself), `late_blocks` and `peak_load` (OUR render overrunning the block period -
  the cause, not the symptom), and `stream_starves` (a streamed voice's ring ran dry; the device kept
  its deadline, we had nothing to give it).
- Most of this was already being computed and thrown away. The ASIO callback predicts the next
  sample position for its fallback clock; comparing the driver's actual position against that
  prediction IS the dropout, in one comparison. `kAsioResyncRequest` was already handled and its
  information discarded.
- **`bwa_get_health` returns a bool, and that is the load-bearing part.** False = this configuration
  cannot observe a dropout at all: not started, the manual sink (no clock, no deadline - an offline
  render cannot miss one by construction), or a driver that never flags a valid sample position. In
  every one of those `xruns` is 0, and reading that as a clean bill of health is how a starved device
  goes unnoticed. Same trap as the latency-0 bug two entries down; this time it is designed out.
- **Tested off-hardware, honestly.** A real missed deadline needs a real device, but the arithmetic
  that turns a position jump into a count is ordinary code: the gap rule is factored into
  `sink_position_gap` and unit-tested (including its refusals - a backward or absurd jump is a driver
  reset, not a dropout), and a null-sink injection hook makes the sink report a position skip it did
  not render, so the whole path from detection to readback runs under ctest. The null sink's
  `late_blocks` are real, not simulated: that thread has a genuine deadline.
- Unity: `Engine.GetHealth(out BwaHealth)` and `Engine.Xruns`. Godot: `get_health()` returning a
  Dictionary (`measured` included) and `get_xruns()`.

### Changed - units in names, where they earn it

The narrow generalization of the `get_output_latency` trap below, applied once and then written
down. The rule: **a unit belongs in a name when the quantity has two live units.** Time is the only
one in this ABI - frames and seconds are both real - so every time-valued name now says which.
Distances (meters), frequencies (Hz), angles (radians) and gains (linear) have no competitor and
stay unmarked, carrying the unit on the value (`radius_m`, `xover_hz`, `yaw_rad`) where it helps.
A decibel value must say `_db`, since linear is the unmarked default. Stated in
docs/api.md → "Coordinates and units".

- **Three C symbols renamed** (the only frames-valued names that did not say so):
  `bwa_get_output_latency` → `bwa_get_output_latency_frames`, `bwa_source_get_playhead` →
  `bwa_source_get_playhead_frames`, `bwa_bed_get_playhead` → `bwa_bed_get_playhead_frames`.
  Signatures and semantics are unchanged. This also closes a layer mismatch: the Godot binding
  says `_frames`, so C now agrees with it.
- **Godot: `get_playhead()` → `get_playhead_frames()`** on `BwaSource` and `BwaBed`, beside the
  existing `get_playhead_seconds()`. That surface is now uniform: `seek_frames`/`seek_seconds`,
  `get_playhead_frames`/`_seconds`, `get_output_latency_frames`/`_seconds`.
- **Unity is unaffected at the C# level** - only the P/Invoke declarations follow the C symbols.
  `Playhead` / `PlayheadSeconds` and `OutputLatency` keep their names: `AudioSource.timeSamples`
  vs `time` is Unity's own spelling of the same pairing, and there is no host collision to fix.
  **REVERSED in [Unreleased]** ("the time-unit sweep, and the 0.5.0 decision reversed"). The
  no-collision half was wrong for the clock (`AudioSettings.dspTime` is a seconds `double`), and the
  precedent half stopped applying once `Seek` became `SeekFrames`.
- Deliberately NOT renamed: the `sample`/`frame` synonym (`start_sample`, `dsp_sample`), which
  denotes the same thing for mono voices and has never misled anyone, and `ir_seconds` → `ir_s`
  for consistency with its `_s` siblings. Both are churn against a frozen-soon ABI.

### Fixed - a dogfooding pass on the Godot addon

All six from an outside install of `bw_audio-godot-0.4.0.zip`. The two expensive ones are the same
failure in different clothes: a call that could not be checked, and a number that could not be
interpreted. Both stayed invisible off-hardware, because the null sink returns the benign value
(nothing to drop, zero latency).

- **`bwa_play_oneshot` now returns `bool`** - whether the one-shot was ACCEPTED. It was `void`, so
  a caller could not tell "played" from "clip missing" from "dropped because the voice pool or
  command ring was full", and the only workaround was probing `sound_get_channels` as a proxy for
  load success. False sets `bwa_last_error` to which of the three it was. The drop itself is
  unchanged and still correct: one-shots never steal, so spam cannot evict named sources. There is
  still no handle to poll - use `bwa_source_create` + `bwa_source_play` if you need to track the
  voice. Unity: `Emitter.PlayOneShot()` returns `bool`. Godot: `BwaEngine.play_oneshot()` returns
  `bool`. Adding a return to a `void` C function is caller-compatible, so existing C call sites
  that ignore it keep working.
- **Godot: `get_output_latency()` is now `get_output_latency_frames()`**, with a new
  `get_output_latency_seconds()` beside it. Godot's own `AudioServer.get_output_latency()` returns
  SECONDS, so the identical name returning frames read as seconds to anyone who knew that call - a
  normal 960-frame ASIO latency displayed as `960000.0 ms`. It hid indefinitely because the null
  sink returns 0, and 0 is 0 in any unit.
- **Godot: `seek()` is now `seek_frames()`**, with a new `seek_seconds()`, on both `BwaEmitter` and
  `BwaBed` - the same collision found by sweeping for it (`AudioStreamPlayer3D.seek()` takes
  seconds, so `seek(1.5)` silently truncated to frame 1). The engine's own unit stays frames
  everywhere, because that is what the dsp clock counts.
- **Godot: the `profile` property now spells out what each value does** in the inspector
  (`"Binaural - headphones, direct render"` and friends) instead of naming the enum, and a new
  configuration warning fires when `profile` is Cave on a machine with no ASIO driver - the case
  that renders 26 channels into nothing and is silent by design. It is the highest-stakes property
  on the node and the inspector is where the choice is made.
- **A one-call driver list**: `BwaEngine.get_asio_drivers() -> PackedStringArray` and Unity's
  `Engine.AsioDrivers`. The count/name pair remains, but it was undocumented and had to be found by
  grepping the DLL's strings.
- **The release artifacts no longer ship dangling doc pointers.** The 0.4.0 zip contained
  `playground.gd` and `scenes.gd` citing `api.md` and `THIRD_PARTY-NOTICES.md` citing
  `docs/build.md`, none of which are in the zip. Both packs now run `tools/dist/doc-pointers.ps1`,
  which rewrites every repo-doc reference in the staged tree to a permalink at the packed commit
  and then FAILS the pack if any relative `.md` reference is left that the stage cannot satisfy.
  The Unity tarball had the same bug and is fixed the same way.
- **ASCII in every runtime-printed string** in the Godot binding: an en-dash in
  `"no running BwaEngine in the scene"` came out as mojibake on a Windows console codepage. Comments
  and docs keep their punctuation; only strings that reach a console changed.

## [0.4.0]

### Added - physical emulation batch

- **`SourceBase.proximity`** (inspector toggle + `bwa_source_set_proximity`): near-field LF boost -
  the shelf rises as the source closes inside ~1 m, so "at arm's length" reads as bass, not just
  level. Loudness comp's near mirror; pushed on init and live from OnValidate like its siblings.
- **`Engine.SpeedOfSound`** (inspector field + live property, `bwa_set_speed_of_sound`): Doppler
  and reflection delays derive from it and glide to a change. 343 air, 1480 underwater; small
  values exaggerate Doppler for slow motion.
- **`Engine.FdnSetDecay(low, high, xover)`** (`bwa_fdn_set_decay`): LIVE FDN decay retune - the
  room-transition knob; the tail keeps ringing, only its slope changes. `<= 0` keeps a parameter.
- **`Engine.SceneSetGround(worldY, material, pressureRelease)`** (`bwa_scene_set_ground`): the
  outdoor degenerate of the room box - one mirror plane, the ground bounce. Replaces the box.
- **`Engine.SceneSetPressureRelease(faceMask)`** (`bwa_scene_set_pressure_release`): flag box faces
  whose image-source reflection inverts - an underwater room's ceiling-as-surface (`1u << 3`), the
  Lloyd's-mirror comb.
- **Directivity note**: `directivity`/`directivityPower` now work in every build - without a Steam
  scene the engine evaluates the same weighted dipole on the audio thread (no binding change).
- **Headphone correction EQ** (`bwa_load_headphone_eq` / `bwa_set_headphone_eq`; Unity raw externs,
  Godot `load_headphone_eq(path)` + the `set_headphone_eq` ramped A/B): an AutoEq ParametricEQ.txt
  for your headphone model, applied to the final stereo of every headphone profile after the HRTF
  decode - the headphone-side align stage (corrects the transducer, not the render; inert in
  `cave`). The Preamp line is honored; a bad file fails with `ErrConfig` and keeps the previous EQ;
  loading and toggling both crossfade. Reload after an engine rebuild (the correction dies with the
  engine; the Godot toggle itself replays on restart).

### Added - clock drift

- **`Engine.GetClockModel(out BwaClockModel)`** → new engine ABI `bwa_get_clock_model`: how fast the
  device clock actually runs against the host clock. `bwa_get_clock`'s pair fixes an exact *instant*;
  this fits the *slope* by exponentially weighted least squares over the same per-block stamps
  (~2 min window, on the audio thread), and reports it as `ppm` with its own `ppmSigma`, plus
  `rateHz`, `spanS`, `jitterNs` and the effective stamp count. `DspTimeAt` re-anchors every frame and
  needs none of it - reach for this when something else owns the timeline (a video file, timecode,
  another render node), when a minutes-long extrapolation has to hold (use `rateHz` in place of the
  nominal rate), or to log the rig's drift. False until the fit has ~1 s of stamps, and again for
  ~1 s after a restart re-bases the device sample position. `ppmSigma` assumes independent stamp
  noise, so read it as a lower bound; `jitterNs` grades the *driver's* stamps, and a driver without
  `kSystemTimeValid` reads worse because the QPC fallback adds dispatch noise.

### Changed - ABI breaks (pre-freeze cleanup, `BWA_VERSION` → 0.10.0)

Deliberate, one-time ABI breaks before the pre-hardware freeze. Update call sites:

- **Removed the bed yaw-shorthand binding** (the `Bwa` P/Invoke was bound but never called). It was
  the pure subset of `bwa_bed_set_orientation(yaw, 0, 0)` - call that with `pitch = roll = 0`
  instead (bit-identical path: yaw-only stays on the exact phasor rotation).
- **Renamed the two engine-global reverb setters** to the `bwa_set_<x>` form, matching the other
  engine-global setters: now `bwa_set_reverb_gain` and `bwa_set_early_reflections_gain` (previously
  the noun-first `..._set_gain` spelling). The Godot method names (`reverb_set_gain` /
  `early_reflections_set_gain`) and the Unity properties (`ReverbGain` / `EarlyReflectionGain`) are
  unchanged - only the underlying C symbol moved.
- **`bwa_set_limiter_ceiling` now takes a LINEAR peak amplitude** in `(0..1]`, like every other gain
  in the ABI - no longer decibels. The default is `0.891251f` (still -1 dBFS). The Unity inspector
  field is now `limiterCeiling` (linear; was `limiterCeilingDb`) with `SetLimiterCeiling(float
  linear)`; the Godot `limiter_ceiling` property range is now `0..1`.
- **`bwa_set_pose_prediction` lead is now in SECONDS**, not milliseconds (matching the fade-time
  calls). The Unity field is `posePredictionS` (was `posePredictionMs`, range `0..0.2`); the Godot
  `set_pose_prediction` argument is seconds.
- **The profile enum was renamed and renumbered** around the new first-class binaural render:
  `BWA_PROFILE_BINAURAL` (still 1) is now the DIRECT per-source headphone render (point sources
  SH-encode at their true listener-relative directions and HRTF-decode - no speaker-array
  simulation in the direct path); the old virtual-speaker array audition is
  `BWA_PROFILE_CAVE_SIM` (2), and the old `BWA_PROFILE_BOTH` is `BWA_PROFILE_CAVE_BOTH` (3, rig +
  the sim tap). Unity: `BwaProfile.{Cave, Binaural, CaveSim, CaveBoth}`; Godot:
  `PROFILE_{CAVE, BINAURAL, CAVE_SIM, CAVE_BOTH}` (the Godot playground now uses CAVE_SIM, since
  its speaker meters visualize the array bus). If you were using `Binaural` to audition the
  ARRAY, switch to `CaveSim`; if you wanted the best headphone render, `Binaural` just got better:
  with the Steam Audio build every point source gets its own true HRTF convolution (one
  `IPLBinauralEffect` per voice), ambisonic beds pass SH→SH, and pathing's indirect field joins
  the binaural decode directly - no speaker-array round trip anywhere in the direct render. No
  API surface changed for this; it's all behind the profile.

### Added - ABI parity (the seven calls the binding had missed)

`Bwa.cs` is **1:1 with `include/bw_audio.h`** again: it now binds every `BWA_API` function except
`bwa_set_output_capture` (an audio-thread callback) and `bwa_render_block` (the manual-sink
golden-render path), both deliberately unbound - Unity uses neither. Seven declarations were added,
with the ones a scene actually authors surfaced on the components:

- **`Emitter.Extent` (Vector2)** → `bwa_source_set_extent`: anisotropic angular width/height (a
  shoreline is wide but not tall, rain tall but not wide). Equal values behave as the isotropic
  `Spread`; setting `Spread` resets it to isotropic (last call wins).
- **`Emitter.SetAttenuationOverride(refDist, rolloff, minGain)`** →
  `bwa_source_set_attenuation_override`: a per-source distance-attenuation curve (rolloff 0 = a
  direction-only cue that never fades; `refDist <= 0` clears it back to the layout curve).
- **`Engine.maxReSplit` / `SetMaxReSplit`** → `bwa_set_max_re_split`: band-split max-rE (taper only
  above ~700 Hz, plain decode below; needs `maxRe` on). Live A/B; the inspector hides it while
  max-rE is off.
- **`Engine.SoundFrames(clip)` / `SoundChannels(clip)`** → `bwa_sound_get_frames` /
  `bwa_sound_get_channels`: asset length (engine-rate frames) and channel count, loaded on demand
  through the same cache as `Load`.
- **`Engine.AsioDriverCount` / `AsioDriverName(index)`** (static, engine-free) →
  `bwa_get_asio_driver_count` / `bwa_get_asio_driver_name`: enumerate the registered ASIO drivers
  before an `Engine` exists, to populate a picker for `asioDriver`.

### Added - `PushEmitter` component

- **`PushEmitter`** - a MonoBehaviour wrapper for procedural (push) sources, filling the one gap
  where the raw `bwa_source_*` push calls (`bwa_source_create_push` / `_push` / `_push_space` /
  `_push_end`) had no component like every other source concept. A positional source you FEED mono
  float PCM at `Engine.SampleRate` instead of a clip (a synth, an engine model, a voice stream):
  `Push(float[])` / `Push(float[], count)` (returns the count accepted), `PushSpace`, `PushEnd()`,
  `IsPlaying`, plus the source-generic surface `Emitter` has that applies to a push voice - `Gain` /
  `FadeTo` / `FadeOut`, `Spread` / `Extent` / `SizeMeters`, `Priority`, `Group`, `Pause` / `UnPause`,
  `SetAttenuationOverride`, `SetOcclusionManual`, `Occlusion`, `Playhead` / `PlayheadSeconds`, `Stop`,
  and the inspector spatial toggles (occlusion, early reflections, reverb send/distance, pathing,
  directivity, doppler, air absorption, loudness comp). It registers with `Engine` and its transform
  rides the same centralized per-frame push, through the one source registry and snapshot loop
  (see `SourceBase` under Changed). Deliberately OMITTED - the engine refuses them
  on a push voice: `Play` / `PlayAt` / `PlayLoop`, `Seek`, `Pitch`, `Queue` / `ClearQueue`,
  `PlayOneShot`, and the file `clip` / `loop` / `playOnEnable` machinery. Mirrors Godot's
  `BwaPushSource` (which splits off `BwaEmitter` for the same reason) adapted to `Emitter`'s Unity
  idioms (lazy `TryInit` with the init-order-race coroutine, generation-safe handle, live `OnValidate`
  re-push). One-way: `PushEnd`/`Stop`/`FadeOut` end the voice, so re-enable the component for a fresh one.

### Removed

- **`Emitter.Position` / `Emitter.PositionSeconds` and `AmbisonicBed.Position`** - the `[Obsolete]`
  forwarders left over from the 0.3.0 `Position → Playhead` rename are gone. Use `Playhead` /
  `PlayheadSeconds` (the content playhead, unrelated to the spatial transform).
- **`Bwa.MaterialPreset(engine, preset)`** - the thin static alias over the already-typed
  `bwa_material_preset` extern is gone; call `Bwa.bwa_material_preset` directly (the two internal
  callers, `Engine.ResolvePreset` and `MaterialAsset.Resolve`, were migrated). The engine-level
  `Engine.MaterialPreset(preset)` convenience (the cached mint) is unaffected.

### Changed

- **`SourceBase`** - the source-generic surface `Emitter` and `PushEmitter` had duplicated
  member-for-member (lifecycle, the per-frame transform push, the live `OnValidate` re-push, and the
  whole knob surface) now lives on one abstract `SourceBase` MonoBehaviour, mirroring Godot's
  `BwaSource` base: a subclass overrides only the create call and adds its own feed. `Engine` keeps
  ONE source registry (the `Register`/`Unregister(PushEmitter)` overloads and the second per-frame
  loop are gone - every source kind runs through the same mutation-safe snapshot loop), and the
  custom inspector now registers on `SourceBase` with `editorForChildClasses`, so `PushEmitter` gets
  the conditional hides + live occlusion bar too. Public API and serialized field names are
  unchanged - existing scenes and scripts migrate untouched.
- **`Emitter` directivity** - the cardioid-weight mapping + `set_directivity` call, duplicated in
  `TryInit` and `OnValidate`, moved into one `ApplyDirectivity()` helper (no behavior change).

### Fixed

- **Play-mode inspector edits no longer wipe a script-set `Extent`** - `OnValidate` re-pushes
  `spread`, which the engine defines as resetting extent (last call wins), and now re-asserts the
  extent after it, exactly like `TryInit` always did. Previously ANY inspector nudge on a live
  source collapsed a script-set anisotropic extent to a point while the `Extent` getter kept
  reporting the stale vector.
- **`SetAttenuationOverride` is mirrored + replayed** (Unity and Godot): it is standing per-source
  state, so a call that loses the init-order race now lands at create, and the override survives a
  disable/re-enable instead of silently reverting to the layout curve.
- **A stale source handle can no longer cross an `Engine` destroy+recreate** - a source component
  records its owning `Engine`; if that engine is replaced while the component stays enabled, every
  call (including `OnDisable`'s destroy) no-ops instead of aliasing the successor engine's
  deterministically-recycled first handles, and the next enable re-creates cleanly.
- **Limiter ceiling can't silently diverge from the engine** - the inspector slider floors at 0.001
  and `SetLimiterCeiling` clamps into `(0..1]` before caching (the engine ignores `<= 0`); the
  Godot hint floors the same way, and its setter converts a replayed pre-0.10 dB scene value to
  linear with a warning instead of silently dropping the authored ceiling.
- **ASIO driver names are UTF-8 across the ABI** - the native enumeration converts the registry's
  ANSI bytes to UTF-8 (and converts an explicit `asioDriver` back before the SDK's byte-exact
  match), so a non-ASCII driver name survives the picker round trip; Godot now decodes with
  `String::utf8`.
- **A failed source create logs on `Emitter` too** (previously only `PushEmitter` checked the
  handle) - unified in `SourceBase.TryInit`.
- **`ProjectCheck` warning** pointed at the wrong menu: "Tools → Engine → Disable Unity Audio" now
  reads "Tools → BwAudio → Disable Unity Audio", matching the actual `MenuItem` path.
- **Docs**: the README and `docs/integration.md` "1:1" claims now name the two deliberately-unbound
  calls instead of overclaiming. The README's live-A/B list gains SPECTRAL spread and max-rE, and
  its load-time list names the bed decoder as AllRAD / EPAD (sampling is no longer selectable).

## [0.3.2-rc3]

- CI tests

## [0.3.2-rc2]

- CI tests

## [0.3.2-rc1]

- CI tests

## [0.3.1]

- Updated build docs.

## [0.3.0]

### Changed - release versioning

- **The git `v*` tag is now the single source of truth for the package version.** `pack.ps1` stamps
  the tag into the packaged `package.json` at build time, replacing the 0.2.0 version/tag guard. The
  committed manifest carries a `0.0.0-dev` placeholder that never needs bumping - cutting a release is
  pushing a tag (plus this CHANGELOG), and `tools/release.ps1` does both in one step. A non-tag pack
  derives a SemVer dev version from `git describe` (`0.2.0-dev.<n>.g<hash>`) so dev tarballs stay
  traceable. No consumer-visible change; the released tarball still carries its real version.

### Added - gapless chaining

- **`Emitter.Queue(clip, loopTerminal)` / `Emitter.ClearQueue()`** → new engine ABI
  `bwa_source_queue` / `bwa_source_clear_queue`: queue a clip to play the instant the current one
  ends, with no gap at the seam (the engine swaps mid-block if the boundary falls there). Queue
  several for a sequence; a `loopTerminal: true` entry is the looping tail - `Play(intro)` then
  `Queue(body, loopTerminal: true)` is an intro→loop across two files. Up to 7 pending; queue *after*
  Play (Play restarts and clears the queue). In-memory mono clips only.

### Added - loop regions + scheduled stop

- **`Emitter.PlayLoop(loopBeg, loopEnd)`** → new engine ABI `bwa_source_play_loop`: the intro→loop
  pattern. Playback starts at 0, plays the intro `[0, loopBeg)` once, then loops the body
  `[loopBeg, loopEnd)` forever (wraps at loopEnd back to loopBeg, not the clip end). Frames are
  engine-rate; loopEnd 0 = the clip end. In-memory clips; a stream loops its whole file. The seam is
  a hard wrap, so author the loop points on matched endpoints.
- **`Emitter.StopAt(stopSample)`** → new engine ABI `bwa_source_stop_at`: a click-free stop on the
  dsp clock (same time base as `PlayAt`). When `Engine.DspTime` reaches stopSample the source fades
  out over one block and ends - never a hard cut, so it can't pop. Block-granular; a later
  Play/PlayAt/PlayLoop clears a pending stop.

### Changed - engine ABI clarity renames (native 0.9.0)

The engine renamed seven symbols for clarity; the binding follows. C#-visible changes:

- **`Emitter.Position`/`PositionSeconds` → `Playhead`/`PlayheadSeconds`**, **`AmbisonicBed.Position`
  → `Playhead`** - "Position" collided with the spatial transform; the readback is the CONTENT
  playhead. The old properties remain as `[Obsolete]` forwarders for now.
- Raw `Bwa` layer follows the C renames: `bwa_source_get_playhead` / `bwa_bed_get_playhead`
  (was `_get_position`), `bwa_source_create_push` (was `_create_stream`), and the reverb-send
  family `bwa_set_reverb_gain` / `bwa_source_set_reverb` / `bwa_source_set_reverb_send` /
  `bwa_source_set_reverb_distance` (was `bwa_reflections_set_gain` / `bwa_source_set_reflections`
  / `..._reflection_send` / `..._reflection_distance`) - "reflections" now always means the Steam
  reflection-bed config or the image-source earlies, "reverb" the shared send/tap.
- New imports: `bwa_get_version` (the DLL's packed version - check it against the header rev at
  startup), `bwa_get_sample_rate` / `bwa_get_block_size` (resolved config - divide frames by THIS,
  not by what you put in the desc), `bwa_get_sink_type` (enum-typed backend readback; `BwaSinkType`
  gains `Manual`).
- A failed **explicit** `layoutPath` now fails `bwa_start` with `BwaResult.ErrLayout` instead of
  silently running the 26-grid default at the wrong channel count (`layoutPath = null` still
  means the default grid deliberately).

### Added - AV sync surface (scheduled play + playhead readback)

- **`Emitter.PlayAt(startSample)`** → the already-bound `bwa_source_play_at`: sample-accurate
  scheduled play on the engine's dsp clock (`Engine.DspTime`) - the `AudioSource.PlayScheduled`
  equivalent, previously reachable only through the raw `Bwa` layer. Keep the startSample you
  passed: `DspTime - startSample` is the sync clock for beat-cued visuals.
- **`Emitter.Position` / `PositionSeconds`** and **`AmbisonicBed.Position`** → new engine ABI
  `bwa_source_get_position` / `bwa_bed_get_position` (latest-wins per-voice playhead readback,
  like `is_playing`): the content playhead in engine-rate frames, correct where client-side
  `DspTime` arithmetic breaks - it freezes under pause, lands where a seek lands, follows pitch
  at the actual rate, and for streamed clips counts frames actually consumed (an underrun slips
  it, exactly like the audible clock). ~One audio block of lag; for tighter-than-a-block
  scheduling keep using `DspTime` arithmetic.
- **`Engine.DspTimeAt(realtime)` / `RealtimeAt(dspSample)`** → new engine ABI `bwa_get_clock`:
  the wall↔dsp bridge. The engine now publishes the device's own (sample position, host time)
  stamp from each audio callback - `ASIOTime`'s pair, previously captured at the sink and
  discarded - so mapping a `Time.realtimeSinceStartupAsDouble` moment to a dsp sample no longer
  carries a block of jitter: `emitter.PlayAt(engine.DspTimeAt(tEvent))` lands a sound on a visual
  event to well under a millisecond. The helpers maintain the epoch offset between the driver's
  clock and Unity's (decaying-max estimator, refreshed per frame from `LateUpdate`), self-correct
  ppm clock drift, and fall back to block-granular `DspTime` pairing when the backend has no host
  stamp. `Engine.GetClock` exposes the raw pair.
- **`Engine.OutputLatency`** → new engine ABI `bwa_get_output_latency`: the device's self-reported
  render→DAC latency in frames (`ASIOGetLatencies` - the Digiface includes its Dante buffering;
  0 on the null-sink fallback). A sound scheduled for dsp time T is *heard* at T + OutputLatency:
  the audio half of AV-latency alignment, so only the display delay is left to measure by hand.

### Added - multi-scene support

- The `Engine` (a `DontDestroyOnLoad` singleton = the physical CAVE, not a level) now **follows Unity's
  loaded scenes**: it subscribes to `SceneManager.sceneLoaded`/`sceneUnloaded` and re-bakes the static
  `AcousticGeometry` whenever scenes change (deferred one frame so additive loads coalesce into a single
  BVH rebuild; `sceneUnloaded` fires after teardown, so an unloaded scene's geometry drops naturally).
  Made possible by the runtime-safe geometry work - no engine rebuild, no audio gap. Additive scenes
  compose (a re-bake spans all loaded scenes); emitters were already per-scene via `OnEnable`/`OnDisable`.
- **Persistent material cache** (`Engine.ResolveMaterial`): material tokens are minted into a fixed
  64-slot engine table, so each `MaterialAsset`/preset is now minted **once** and reused across every
  scene load. Re-minting per load (the old per-`SetupScene` cache, and `AddDynamicMesh`) would exhaust
  the table in a multi-scene game - both now route through the shared cache.
- **`Engine.ReleaseMaterial`** → `bwa_material_release`: frees a material's table slot for reuse and
  evicts it from the cache (a later `ResolveMaterial` re-mints). Caller-managed - only release a
  material no live mesh/occluder references. The mint-once cache covers the common case; this is the
  escape hatch for apps that churn many *distinct* materials over a long session.
- Recommended pattern: put the `Engine` in a **persistent bootstrap scene**, load levels on top
  (single or additive). Sources, dynamic occluders, and static geometry all track the loaded scenes;
  the reflection-bed *config* (IR/order, room box) and the speaker layout stay engine-level (rebuild
  the engine only if those must change - rare for a fixed install).

### Added - dynamic (movable) acoustic geometry

- **`DynamicAcousticGeometry`** component + **`Engine.AddDynamicMesh` / `SetDynamicTransform` /
  `RemoveDynamicMesh`** → `bwa_scene_add_dynamic_mesh` & co.: mark a MOVING object (door, lift,
  rotating panel) as an occluder/reflector. It registers a low-poly acoustic mesh as a rigid
  instance (Steam Audio `IPLInstancedMesh`) and pushes its pose each frame (throttled by
  `positionEpsilon`/`angleEpsilon`), so occlusion and REAL-TIME reflections track it - moving it is a
  cheap scene-BVH refit, not a geometry rebuild. Same "keep it simple / use `meshOverride`" rules as
  `AcousticGeometry`; scale is captured at registration (rigid-body). Coordinate seam handled: the
  mesh bakes into room handedness once (X-flip + scale, winding reversed) and the per-frame pose goes
  through `Room.Pos`/`Room.Rot`. Baked reflections/pathing do NOT track movement (real-time does).
  Needs the Steam Audio backend (a no-op otherwise). Static geometry stays on `AcousticGeometry`,
  which is now also safe to re-push at runtime (a full scene rebuild - prefer dynamic meshes for
  movers).

### Added - parity with the engine's A/B round (max-rE · spectral spread · FuMa · bed orientation)

- **`Engine.maxRe` / `SetMaxRe`** → `bwa_set_max_re`: max-rE weighting on the bed decode and the
  FDN's render (live A/B, crossfaded, level-fair) - fewer decode sidelobes, better localization
  away from the sweet spot. Sits under the *Diffuse beds* header; off by default like the engine.
- **`BwaSpreadMode.Spectral`**: the third spread render - frequency-dependent panning (6 bands,
  each from its own direction inside the cone; width with no coherent copies to collapse or
  comb-filter - the decorrelation alternative). The existing `spreadMode` field/`SetSpreadMode`
  pass it through unchanged.
- **`Engine.LoadFuma`** + **`AmbisonicBed.fumaClip`** → `bwa_load_fuma`: legacy FuMa B-format
  clips (WXYZ order, MaxN, the W -3 dB) convert to AmbiX at load - downstream they are AmbiX
  assets, cached under a separate `fuma:` key so the same path can be loaded both ways.
- **`AmbisonicBed.pitchDegrees` / `rollDegrees`** (+ `PitchDegrees`/`RollDegrees` properties) →
  `bwa_bed_set_orientation`: level or tilt a capture, glided and click-free like yaw. Coordinate
  seam: yaw still converts through `Room.YawRad` (the X mirror reverses its sense), while pitch
  and roll pass through with the **same** sense - "front tilts up" never touches the mirrored
  axis, and Unity-right maps to room-right. All orientation paths (inspector, properties,
  enable) now go through one `ApplyOrientation()`.

**Breaking** - the native ABI was reshaped for consistency (nothing had shipped against it, so no
migration window): load-time configuration lives in config structs, live control lives in setters,
one door per knob.

- **The whole API is renamed**, sokol/miniaudio-style: the native prefix is `bwa_` (header
  `bw_audio.h`; `bw` stays free as the family namespace), C types are lowercase snake_case with
  `_desc` config structs (`bwa_desc`, `bwa_reflections_desc`, `bwa_fdn_desc`), constants are
  `BWA_*`, env vars are `BWA_*` (were `BWAUDIO_*`), and the native library is `bw_audio.dll`
  (was `bwaudio.dll`) - one product string everywhere.
- **The C# namespace is `BwAudio`** (was `CaveAudio`) **and the components lost their `Bw`
  prefix** - the namespace is the prefix now: `BwAudio.Engine` (the manager, previously the
  `BwAudio` class), `BwAudio.Emitter`, `AmbisonicBed`, `SpeakerView`, `AcousticGeometry`, `MaterialAsset`,
  `RoomConstraints`, and the `[Clip]` attribute. The raw P/Invoke layer keeps its C shape on
  purpose: `Bwa.bwa_*` with `Bwa*` mirror types (`BwaDesc`, `BwaPanner`, …). Scene/prefab
  references survive (every rename moved the `.meta` with its file, so GUIDs are unchanged);
  the package id is now `com.brainworks.bw_audio`.
- **`BwaDesc`** gained `enablePathing` (replaces the `BWAUDIO_PATHING` env var), `bedDecoder`
  (replaces the removed `bwa_set_bed_decoder`), and reserved fields so future growth won't break
  the ABI again.
- **`BwaReflectionsDesc.wetGain` is gone** - `bwa_reflections_set_gain` is the one wet-level
  control (live; a value pushed before `bwa_start` seeds whichever reverb bed starts). New `bake`
  field (replaces the `BWAUDIO_BAKE` env var).
- **The FDN setters** (`bwa_reverb_fdn`, `bwa_fdn_set_decay`, `bwa_fdn_set_decay_direction`)
  collapsed into one `bwa_fdn_config(in BwaFdnDesc)` - same shape as the reflection config.
- **`bwa_scene_set_mesh` (single-material) removed** - use `bwa_scene_set_mesh_mat` with one
  material token.
- **The `bwa_bed_*` facade is complete**: `bwa_bed_fade_to` / `fade_out` / `set_paused` / `seek` /
  `set_priority` / `set_group` / `is_playing` - a bed is a voice; bed code never needs the
  `bwa_source_*` prefix. Note a bed CAN be voice-stolen at default priority; protect a music bed
  with priority 255.
- **`Engine` inspector**: new `enablePathing` and `bakeReflections` toggles; `reverbGain` now
  rides the live setter (re-applied from `OnValidate` like the other live knobs).
- **Typed results**: the documented error codes are now a real enum - `bwa_result` in C,
  `BwaResult` here - and `bwa_start`/`bwa_stop`/`bwa_tracker_connect` return it.
- **The tracker is a runtime API, not env vars**: `bwa_tracker_connect(in BwaTrackerDesc)` /
  `bwa_tracker_disconnect` replace the `BWA_NATNET_*` environment variables AND the
  `BwaDesc.trackInternal` flag (connect/reconnect/disconnect any time, like every other NatNet
  client; the pose-source swap is glitch-free). `Engine` gained `natnetServer` / `natnetRigidBody`
  inspector fields and connects after start when Feed Listener is off.
- **Material presets are typed end to end**: `bwa_material_preset` takes `bwa_material_type`
  (mirrored by the existing `BwaMaterialPreset`) instead of a name string - the misspelled-name
  footgun is gone, and the C# `PresetName` shim with it. Custom materials are unchanged:
  `bwa_material_define` returns the same kind of `bwa_material` token.
- **Readback naming unified**: `bwa_get_channel_count`, `bwa_get_dsp_time_frames`,
  `bwa_get_audio_backend` (were `bwa_channel_count`/`bwa_dsp_time`/`bwa_audio_backend`), and the
  test tone is a setter like its siblings: `bwa_set_test_signal` (was `bwa_test_signal`).
- **The last env vars moved into `BwaDesc`** - there are now NO environment variables:
  `sink` (`BwaSinkType`: Auto = try ASIO then fall back to the silent null sink; Asio = demand a
  device, fail loudly; Null = force offline - replaces `BWA_SINK`), `asioDriver` (replaces
  `BWA_ASIO_DRIVER`; empty = auto-pick by channel count), and `embree` (replaces `BWA_EMBREE`;
  silently falls back to the default ray tracer when the phonon build lacks Embree). `Engine`
  gained `sink` + `asioDriver` inspector fields.

## [0.2.0]

First **published** release: the package now ships as an installable UPM tarball with the native engine
inside it (`bw_audio.dll` + `phonon.dll`, import settings pre-configured), so a Unity project consumes it
without building any C++.

- **Renamed to `com.brainworks.bw_audio`** (was `com.cave.bw_audio`). Done before anything shipped, so
  there is nothing to migrate: the scope you add to `manifest.json` is now `com.brainworks`. The C#
  namespace is `BwAudio` - this is the package identity, not the code.
- **Distribution** - `tools/upm/pack.ps1` packs `com.brainworks.bw_audio-<version>.tgz` (uses `tar`, no
  Node anywhere; CI packs on *every* run, so a broken package fails the build rather than the release).
  A `v*` tag cuts a GitHub Release with that tarball attached, and **the Release is the distribution** -
  no registry, no token, nothing to keep in sync. Install it with Package Manager → `+` → *Install
  package from tarball…*. See the README.
- **`.meta` files are now committed** (`tools/upm/gen-meta.ps1`). An installed package is
  immutable, so assets arriving without a `.meta` get a fresh random GUID in every project: a scene
  referencing `Emitter` on one machine would deserialize as *"Missing (Mono Script)"* on another. The
  native plugins' import settings (Windows x64, Editor enabled) ship the same way - in an immutable
  package the user cannot fix them in the Inspector.
- **Version/tag guard** - the pack fails if the git tag and `package.json` disagree, so a tarball can't
  claim a version it isn't.

### Usability pass

The theme: the binding had several settings that failed *silently* - the engine survives them, logs
something, and carries on sounding subtly wrong. Those are now impossible to express, or caught in the
inspector where you can still see them.

- **Materials are a dropdown, not a string.** `BwaMaterialPreset` (mirrors the engine's table) replaces
  the free-text preset name on `Engine.roomMaterial` and `MaterialAsset.preset`. An unrecognized name
  was never an error - `bwa_material_preset` quietly returns material 0 (generic) and leaves the reason in
  `bwa_last_error` - so a typo'd `"concreet"` wall just *sounded* wrong. **Breaking:** `MaterialAsset`
  assets whose preset wasn't `concrete` will reset to Concrete; re-pick it from the dropdown.
- **The layout file says where it goes.** `layoutFile` uses the `[Clip(".json")]` picker (the same one
  audio clips use), so it lists the JSON files actually present under `StreamingAssets`, flags a missing
  one in red, and tooltips that the path is *relative to `Assets/StreamingAssets/`*. The inspector also
  shows the **absolute path the engine will look in** when the file isn't there - paired with the
  runtime error, both ends of that trap are now closed.
- **Inspector sliders work in Play mode.** `Emitter` gained the `OnValidate` re-push that `Engine` and
  `AmbisonicBed` already had. Dragging Gain/Pitch/Spread during Play used to change the field and
  nothing else (the value is only read at source creation), which reads as a broken slider.
- **Custom inspectors** for `Engine`, `Emitter` and `MaterialAsset`: settings that don't apply are
  hidden (FDN decay with the FDN off, pose prediction when Unity feeds the pose, custom coefficients
  under a preset material…), and the mistakes the engine merely *warns* about are surfaced as inspector
  warnings - a missing layout, both reverb beds fighting over the one tap, reflections with no geometry
  to reflect off. In Play mode `Engine` shows the live backend (flagging a **silent** fallback to the
  null sink), channel count, active voices and per-channel output meters; `Emitter` shows its
  ray-traced occlusion, which is otherwise invisible.
- **The room box is visible.** It draws as a wireframe gizmo (`Room.RoomToUnityMatrix` - the inverse of
  the coordinate seam, so a wrong `Room.UnityToRoom` makes the box land visibly in the wrong place).
- **The speaker array is visible.** `Engine` draws each speaker as a gizmo, labeled with its channel
  index. Stopped, the positions come from the layout **file**; in Play mode they come from the **engine**
  (`bwa_get_speakers`) and each one lights up with that channel's live output level, the same way the
  playground's gizmos do - so a dead or mis-wired speaker is visible at a glance. The Play-mode source
  matters: it's the geometry the engine is *actually* panning over, which means a failed layout load
  shows up as the built-in 26-grid sitting where your room isn't.
- **`SpeakerView` - live speaker activity you can see from inside the CAVE.** The gizmos above are an
  *editor* feature and don't render in a build, so this is the runtime counterpart: one unlit marker per
  channel, placed at the real speaker's position, brightening (and growing) with that channel's output.
  Unlit on purpose - a CAVE is dark, so the color computed is the color seen, with no lights to set up.
  Instant attack + slow release, because the engine reports a per-*block* peak that strobes too fast to
  read raw. Uses the same `bwa_get_speakers` + `bwa_get_bus_levels` readbacks as everything else, writes no
  audio state, and picks its shader across URP / HDRP / built-in. Optional: delete it and nothing changes
  audibly.
- **`RoomConstraints` - the physical room, in the scene view.** Reads the surveyed `constraints.json`
  (from StreamingAssets) and draws it: green = the speaker truss, red = the CAVE screen cube / observer
  keep-out, orange = the projectors. It's the **same file** `bwa_layout_tool` and `bwa_playground` read,
  so the room has one source of truth and doesn't get re-authored as Unity geometry that can drift from
  the survey. Purely a scene-view aid - no engine needed (it parses the JSON directly, so it works with
  the editor stopped) and nothing audible depends on it.
- **Minimum Unity is now 6000.0 (Unity 6)**, up from 2021.3. Nothing had ever been tested below it, and
  the old floor was already forcing compatibility shims.
- **No deprecation warnings.** The scene bake used `FindObjectsOfType`, deprecated in favor of
  `FindObjectsByType`, which forces you to say whether you need the results sorted. We don't - every
  geometry is baked into one mesh - so it passes `FindObjectsSortMode.None` and skips a pointless
  InstanceID sort.
- **Packing no longer dies on a locked DLL.** An open Unity Editor holds `bw_audio.dll` loaded out of
  `Runtime/Plugins/x86_64/`, so it can't be overwritten - `pack.ps1` now hashes first and skips the copy
  when the binary is already identical, and explains itself instead of throwing a raw IOException when
  it genuinely has to write. (A CMake rebuild hits the same lock; close the editor first.)
- **A tracked listener with nothing to track now warns** - `feedListener` on with no `listener` Transform
  silently left the listener parked at the array centroid, panning every source for a head that never
  moves.

## [0.1.0] - unreleased

Initial Unity binding (M7).

- Room convention change (engine-wide): room space is now **+Z forward** (identity head faces +z,
  matching Motive's default streamed frame). `Room`'s baseline handedness flip moved from the Z
  axis to the X axis; since Unity is also +Z-forward, identity rotations now map to identity.
- Room origin is now canonically **on the floor** (Motive ground plane; y = height above the
  floor), and the room box (`AddBox` / `bwa_scene_set_box`) is floor-based: x/z centered, y from 0
  up to the box height. The engine references the array centroid, not the origin, for its
  world-locked decodes, so surveys with other origins keep working.

- Pause/seek + the output protection limiter (engine `9d60c6e`): `Emitter.Pause()/UnPause()/Paused`
  (AudioSource.Pause/UnPause equivalents; click-free, the playhead freezes, paused still reads as
  IsPlaying) and `Emitter.Seek(samples)` (a timeSamples-set equivalent; in-memory clips only -
  streamed clips ignore it); `Engine.SetLimiter(bool)` / `SetLimiterCeiling(dB)` over the
  engine-default ON at -1 dBFS.

- `Bwa` - P/Invoke layer, 1:1 with the bw_audio C ABI (`include/bw_audio.h`): lifecycle, assets,
  sources, ambisonic beds, materials/occlusion, directivity, reflection bed, listener, commit.
  Verified against the real `bw_audio.dll` (struct layout, calling convention, string/array/bool
  marshalling).
- `Room` - the room-space (RH) ↔ Unity (LH) coordinate seam.
- `Engine` - scene manager singleton: owns the engine handle, loads assets, configures reflections +
  an optional room box at load time, and runs the centralized per-frame push (sources → listener →
  one commit).
- `Emitter` - positional source: transform-driven position/orientation, with `occlusion`,
  `reflections`, and `directivity` toggles, plus a one-shot helper.
- Editor guardrail (`ProjectCheck`): warns if Unity's built-in audio is still enabled (the
  engine owns the device) and offers one-click **Tools → Engine → Disable Unity Audio**.
- Acoustic-scene authoring: `MaterialAsset` (Create → Engine → Acoustic Material; preset or custom
  3-band) and `AcousticGeometry` (mark a mesh as occluding/reflecting, assign a material, scene-view
  gizmo). `Engine` bakes all geometry (+ the optional room box) world→room into one mesh at load.
- Audio-file authoring: `[Clip]` attribute + `ClipDrawer` - an editor picker that lists the
  `.wav`/`.flac`/`.mp3` files under StreamingAssets (with a browse button and a missing-file flag)
  instead of a hand-typed path. `Emitter` gains AudioSource-style `Play()`/`Stop()`/`Gain`. README
  has a "Replacing Unity audio" mapping table.
- `AmbisonicBed` - world-locked AmbiX soundfield component (wraps `bwa_bed_*`): play/stop/gain for
  diffuse ambience/music, decoded straight to all 26 speakers.
- Reverb wet level: `Engine.reverbGain` (inspector) + a live `ReverbGain` property, backed by the
  new engine config field `wet_gain` + `bwa_reflections_set_gain`.
- `Emitter.IsPlaying` + an `onFinished` UnityEvent, backed by a new engine ABI call
  `bwa_source_is_playing` (latest-wins per-source playback readback).

### Caught up to the engine ABI

The engine had grown 23 `BWA_API` calls the binding never got. `Bwa` is **1:1 with `bw_audio.h`** again
(verified by diffing the exported symbols), and the components expose the ones a scene actually
authors:

- **Mixing** - `Emitter.FadeTo()` / `FadeOut()` (the engine runs the fade on the audio thread; no
  coroutine, and `FadeOut` lands on the click-free stop path), `Emitter.Pitch` (glides - a change
  bends the pitch rather than stepping it), `Emitter.Priority` (voice-steal), mix groups
  (`Emitter.group` + `Engine.SetGroupGain` / `SetGroupPaused` - duck the SFX, keep the dialog),
  `Engine.MasterGain`, and `Engine.Paused` (global freeze; resume continues exactly).
- **Width** - `Emitter.spread` was bound but had no inspector field; `sizeMeters` (a physical
  radius: the source keeps its real-world size as the listener walks, where a fixed angular spread
  would not) is new, as are the engine-wide `spreadMode` (LOBE / MDAP), `decorrelation` (wide sources
  stop collapsing to phantom images as you walk), and `nearSpreadRadius`.
- **Propagation** - `Emitter.loudnessComp` (an LF shelf tracking the distance attenuation: far, not
  thin) joins `doppler` and `airAbsorption`.
- **Reverb without the SDK** - `Engine.enableFdnReverb` + the decay controls: a directional FDN bed
  that takes the reverb tap **instead of** the Steam bed (the manager warns if both are ticked) and is
  fed by the same per-emitter sends, so reverb works in a build with no phonon. Likewise
  `Emitter.SetOcclusionManual()` - game-driven occlusion (a door the gameplay knows about,
  underwater) through the sim's own ramped, band-tilted publish path, no SDK required.
- **Beds** - `AmbisonicBed.YawDegrees` (turn a recorded soundfield to line up with the scene) and
  `Engine.bedRenderer`: MATRIX, or **PARAMETRIC**, which re-pans the directional part of the field
  through the listener-relative panner - a recorded soundfield becomes *walkable*.
- **Listener** - `Engine.extraListeners` (up to 3 other occupants; panning becomes the energy mean of
  everyone's solve, instead of exact for one head and wrong for the rest), pushed in the same frame
  block as the primary pose since it is commit-gated the same way; and `posePredictionMs`, which leads
  the *tracked* pose by your measured motion-to-ears latency.
- **Diagnostics** - `Engine.ChannelCount` / `BusLevels()` / `SpeakerPositions()` / `ActiveVoices` /
  `TestSignal()` / `DspTime`.
- **Live A/B by ear** - `Engine.OnValidate` re-pushes every knob the engine makes atomic or
  crossfaded (panner, dual-band, spread mode, decorrelation, near-spread, bed renderer, tracked room
  EQ, master gain, limiter), so inspector tweaks are audible in Play mode instead of needing a restart.

Two coordinate seams the new calls exposed, both now in `Room` so nothing re-derives them:
`Room.YawRad` (the X mirror **reverses the sense of rotation** - a Unity euler angle passed straight
to the bed's yaw (`bwa_bed_set_orientation`) spins the soundfield the wrong way) and `Room.Dir` (a *direction*, e.g. the
FDN's decay axis, must not pick up the registration transform's translation the way `Room.Pos` does).

`Engine` now also **reports a failed layout load** (`bwa_last_error` right after `bwa_create`): it is
non-fatal - the engine falls back to its 26-speaker default grid - which on a smaller rig silently
changes the channel count and pans every source over geometry that isn't the one in the room.

### Hardened (adversarial review)

- `Engine` claims the `Instance` singleton only after a successful `bwa_start` (a failed init no longer
  leaves a dead, un-replaceable manager), and iterates a snapshot of the emitter list during the
  per-frame push (an `onFinished` handler that disables its emitter can no longer mutate the list
  mid-loop and drop the frame's commit).
- `Emitter` lazily creates its source (a coroutine retries until `Engine` is ready) instead of
  permanently disabling on an init-order race, and resets its play-edge state on disable (no spurious
  `onFinished` on re-enable).
- Mesh baking computes the winding flip from the full transform determinant, so a negative/mirrored
  object scale no longer bakes backward-facing reflectors. `Room.UnityToRoom` is documented as
  rigid-only.
- `Bwa.GetListenerPose` allocates correctly-sized arrays (the raw call writes fixed slots); the editor
  clip scan tolerates an inaccessible StreamingAssets subdir.
