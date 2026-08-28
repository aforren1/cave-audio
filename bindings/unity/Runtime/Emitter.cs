// Emitter.cs — a positional sound source. Attach to any GameObject; its transform drives the
// source position (and orientation, for directional sources) every frame via Engine's centralized
// push. Audio files live under StreamingAssets and are decoded by the engine (not Unity's AudioClip).
// The source-generic surface (lifecycle, gain/spread/occlusion/... knobs) lives on SourceBase —
// this class adds the clip/playback half: Play and its scheduled/looping/queued variants, pitch,
// seek, the play region, and the onFinished/onLoop events.
using System;
using UnityEngine;
using UnityEngine.Events;

namespace BwAudio
{
    public sealed class Emitter : SourceBase
    {
        [Header("Clip (under StreamingAssets)")]
        [Clip] public string clip = "sfx/footsteps.wav";
        public bool loop = true;
        public bool playOnEnable = true;
        [Tooltip("Decode the clip on the engine's loader thread instead of blocking this one. Play() " +
                 "returns immediately and the source stays SILENT until the data lands, then starts from " +
                 "the top with nothing skipped. For content that appears MID-SESSION (a streamed-in level, " +
                 "a downloaded line). Leave OFF for the CAVE's normal path, which is load-time and " +
                 "synchronous. Queue and PlayOneShot cannot be held, so they refuse a clip that is still " +
                 "decoding and say so.")]
        public bool loadAsync = false;
        [Tooltip("Playback rate (1 = native). In-memory clips only — streamed clips ignore it.")]
        [Range(0.25f, 4f)] public float pitch = 1f;

        [Header("Events")]
        [Tooltip("Fires when the source stops producing audio — a non-looping clip finished, or Stop().")]
        public UnityEvent onFinished = new UnityEvent();
        [Tooltip("Fires each time a LOOPING clip wraps back to its loop start. A looping voice never " +
                 "finishes, so onFinished reports it never — this is the event to pace experimental " +
                 "trials or cue visuals from. One call per wrap, so a loop shorter than a frame fires " +
                 "several times in one frame.")]
        public UnityEvent onLoop = new UnityEvent();

        // onFinished has two feeds, because the engine reports only one of the two ways a voice goes
        // quiet. A voice that RAN OUT (a non-loop clip finished, a queue drained, a push feed drained)
        // posts an ended EVENT the block it happens, which Engine drains with bwa_poll_ended and routes
        // here — that path cannot miss a clip shorter than a frame, which is the whole reason it exists.
        // An explicit HALT (Stop, StopAt, FadeOut, Engine.StopGroup/StopAll, a voice steal) posts no
        // event at all: the engine's position is that a halt is not a completion. Unity's onFinished has
        // always fired for those, so the IsPlaying edge below stays as the narrow fallback that covers
        // exactly that gap. `_endFired` keeps the two from both reporting the same end.
        bool _wasPlaying;     // we have seen the CURRENT play as playing, so a fall to silence is an end
        bool _endFired;       // the edge fallback already reported this end; swallow the event if one follows
        uint _pending;        // an async handle bound to this source but not yet decoded (0 = none)

        // _endFired is cleared by the next play that BINDS, and a play HELD on an async decode has not
        // bound. The engine only bumps its per-slot play counter at bind time, and that counter is the
        // gate that drops a completion straggling in from the PREVIOUS play; until it moves, the
        // straggler is still deliverable and this latch is the only thing standing in its way. Clearing
        // it on a held play therefore lets one end reach onFinished twice. Push() clears it instead, on
        // the frame the load lands — which is the frame the engine binds the play.

        // ---- SourceBase hooks ------------------------------------------------------------------------

        protected override uint CreateSource(IntPtr eng) => Bwa.bwa_source_create(eng);

        // Resets the completion state so a recycled component never inherits a stale play edge.
        protected override void ResetPlaybackState() { _wasPlaying = false; _endFired = false; _pending = 0; }

        // Pitch is a desc field, so the create-time bulk push carries it (SourceBase.TryInit) — there is
        // nothing left for this hook to do. The Pitch property is still the live, incremental path.
        protected override void WriteDesc(ref BwaSourceDesc d) { base.WriteDesc(ref d); d.pitch = pitch; }
        protected override void ReadDesc(in BwaSourceDesc d)   { base.ReadDesc(in d);    pitch = d.pitch; }

        protected override void OnSourceReady()
        {
            if (playOnEnable) Play();
        }

        internal override void FrameSync() => Push();

        protected override void OnDisable()
        {
            _wasPlaying = false;                 // never carry a stale play edge into a re-enable
            _endFired = false;
            base.OnDisable();
        }

        /// <summary>Called once per frame by Engine (before the listener + commit).</summary>
        public void Push()
        {
            if (!Live) return;
            SyncTransform();

            // While an async load is in flight the source is bound and silent, which looks exactly like
            // a decode that FAILED — a failure never becomes ready. Engine.WatchPendingLoad polls it
            // until it resolves one way or the other and returns 0 once it has, so the watch stops.
            _pending = Engine.WatchPendingLoad(_pending, this, "source", out bool landed);
            // The probe above is a pump point, so `landed` is the very call that adopted the decode and
            // released the held play into the engine — which makes this the moment the play BINDS, and
            // the only moment a held play may void the previous end's duplicate suppression. See Play().
            if (landed) _endFired = false;
        }

        /// <summary>The engine reported this source's voice as ENDED (Engine drained it from
        /// bwa_poll_ended after the commit). Fires onFinished unless the halt fallback already did.
        /// </summary>
        internal override void NotifyEnded()
        {
            // The fallback runs a hair earlier in the frame than the event that describes the same end
            // can be drained (the voice can end between this frame's drain and this frame's IsPlaying
            // read), so ONE end can reach both feeds. Whichever reports first wins; this latch eats the
            // straggler. It is cleared by the next play that BINDS, so a swallowed straggler can never
            // eat a LATER end — including the sub-frame clip this whole path exists to catch.
            if (_endFired) { _endFired = false; return; }
            _wasPlaying = false;
            onFinished.Invoke();
        }

        /// <summary>The engine reported this source's voice as having WRAPPED at a loop point (Engine
        /// drained it from bwa_poll_looped after the commit). Called once per wrap.</summary>
        internal override void NotifyLooped()
        {
            // Deliberately no latch and no _wasPlaying touch. A wrap is not an end: the voice is still
            // playing, both feeds of onFinished are about a voice going quiet, and neither can produce a
            // wrap — so there is nothing here to de-duplicate against and nothing to suppress.
            onLoop.Invoke();
        }

        /// <summary>Called once per frame by Engine, after the commit and both event drains.</summary>
        internal override void PostCommit()
        {
            if (!Live) return;

            // The explicit-halt fallback (see the field comments). Reading AFTER the drain is what keeps
            // a natural end from being reported twice: NotifyEnded has already cleared _wasPlaying by
            // the time this runs, so the fall to silence it left behind is not an edge any more.
            // A play held on an async decode never arms this: it honestly reads "not playing" while it
            // waits, and _wasPlaying only latches on a voice actually observed playing.
            bool now = Bwa.bwa_source_is_playing(Eng, _src);
            if (now) _wasPlaying = true;
            else if (_wasPlaying) { _wasPlaying = false; _endFired = true; onFinished.Invoke(); }
        }

        // The clip handle for a play: async when opted in, which returns immediately and lets the engine
        // hold the play until the data lands. `held` is true when the handle is not playable YET, which
        // is fine for the play calls (they are held) and refused by Queue/PlayOneShot (they are not).
        uint Resolve(string clipOverride, out bool held)
        {
            held = false;
            var engine = Engine.Instance;
            if (!engine) return 0;
            string c = clipOverride ?? clip;
            if (!loadAsync) return engine.Load(c);
            uint snd = engine.AcquireAsync(c);
            if (snd != 0) held = !engine.IsSoundReady(snd, out _);
            return snd;
        }

        // ---- the clip/playback surface ---------------------------------------------------------------

        /// <summary>Play `clip` (or an override), loading it on demand — AudioSource.Play equivalent.
        /// With `loadAsync` on this returns immediately and the sound starts, from its first frame, on the
        /// block its data lands.</summary>
        public void Play(string clipOverride = null)
        {
            if (!Live) return;
            uint snd = Resolve(clipOverride, out bool held);
            if (snd == 0) return;
            Bwa.bwa_source_play(Eng, _src, snd, loop);
            _paused = false;                                  // play restarts un-paused
            if (!held) _endFired = false;   // only a play that BINDS voids the previous end's suppression
            _pending = held ? snd : 0;      // a READY play supersedes a still-pending earlier one
        }

        /// <summary>Sample-accurate scheduled play — AudioSource.PlayScheduled equivalent, on the
        /// engine's dsp clock instead of AudioSettings.dspTime: output begins exactly when
        /// Engine.DspTimeFrames reaches `startSample`. Schedule with margin (at least a block; e.g.
        /// <c>PlayAt(engine.DspTimeFrames + engine.sampleRate / 2)</c> starts half a second out); a start
        /// already in the past plays immediately. Keep the startSample you passed —
        /// <c>DspTimeFrames - startSample</c> is the sync clock for driving visuals (or poll PlayheadFrames).
        /// <para>With `loadAsync` on, the schedule is only as good as the decode: a start time that
        /// arrives before the data does plays as soon as the data lands, not at `startSample`.</para></summary>
        public void PlayAt(ulong startSample, string clipOverride = null)
        {
            if (!Live) return;
            uint snd = Resolve(clipOverride, out bool held);
            if (snd == 0) return;
            Bwa.bwa_source_play_at(Eng, _src, snd, loop, startSample);
            _paused = false;
            if (!held) _endFired = false;   // only a play that BINDS voids the previous end's suppression
            _pending = held ? snd : 0;      // a READY play supersedes a still-pending earlier one
        }

        /// <summary>Play with an intro→loop region: the intro <c>[0, loopBeg)</c> plays once, then the
        /// body <c>[loopBeg, loopEnd)</c> loops forever (wraps at loopEnd back to loopBeg, not the clip
        /// end). Frames are engine-rate (seconds × Engine.SampleRate). loopEnd 0 = the clip end. Always
        /// loops (ignores this.loop). In-memory clips only. Author the loop points on matched endpoints —
        /// the seam is a hard wrap.</summary>
        public void PlayLoop(ulong loopBeg, ulong loopEnd, string clipOverride = null)
        {
            if (!Live) return;
            uint snd = Resolve(clipOverride, out bool held);
            if (snd == 0) return;
            Bwa.bwa_source_play_loop(Eng, _src, snd, loopBeg, loopEnd);
            _paused = false;
            if (!held) _endFired = false;   // only a play that BINDS voids the previous end's suppression
            _pending = held ? snd : 0;      // a READY play supersedes a still-pending earlier one
        }

        /// <summary>Schedule a click-free stop on the engine's dsp clock: when Engine.DspTimeFrames reaches
        /// stopSample the source fades out over one block and ends — never a hard cut, so it can't pop.
        /// Block-granular (silence lands within ~one block of stopSample); a stopSample in the past
        /// stops now; a later Play/PlayAt/PlayLoop clears it. Same time base as PlayAt.</summary>
        public void StopAt(ulong stopSample) { if (Live) Bwa.bwa_source_stop_at(Eng, _src, stopSample); }

        /// <summary>Stop, and drop any async load this source was waiting on. The engine cancels a held
        /// play on stop, so the decode can no longer start this source; watching it further would only
        /// warn later that the source "stays silent", which by then is what you asked for. Matches
        /// AmbisonicBed.Stop.</summary>
        public override void Stop() { _pending = 0; base.Stop(); }

        /// <summary>Gapless chaining: queue `clip` (or an override) to play the instant the current sound
        /// ends — no gap at the seam. Queue several for a sequence; a queued clip with loopTerminal = true
        /// is the looping tail (e.g. Play(intro, one-shot) then Queue(body, loopTerminal: true) for an
        /// intro→loop across two files). Up to 7 pending. Queue AFTER Play (Play restarts and clears the
        /// queue); nothing chains after a looping or stopped sound. In-memory mono clips only.
        /// <para>A queue entry resolves to its asset at bind time, so it cannot be held for an async
        /// decode: with `loadAsync` on, queueing a clip that is still decoding is refused (and warns)
        /// rather than silently dropping the chain.</para></summary>
        public void Queue(string clipOverride = null, bool loopTerminal = false)
        {
            if (!Live) return;
            uint snd = Resolve(clipOverride, out bool held);
            if (snd == 0) return;
            if (held)
            {
                Debug.LogWarning("[Emitter] Queue on " + name + " skipped: '" + (clipOverride ?? clip) +
                                 "' is still decoding, and a queue entry cannot be held. Wait for the " +
                                 "load (Engine.IsSoundReady) or turn Load Async off.");
                return;
            }
            Bwa.bwa_source_queue(Eng, _src, snd, loopTerminal);
        }

        /// <summary>Drop the pending gapless chain queued with Queue.</summary>
        public void ClearQueue() { if (Live) Bwa.bwa_source_clear_queue(Eng, _src); }

        /// <summary>Jump to <c>frame</c> (engine-rate frames) into the clip — AudioSource.timeSamples-set
        /// equivalent, click-free (ramp-out → jump → ramp-in). In-memory clips only; streamed clips
        /// ignore it. Past-the-end wraps a looping clip and ends a one-shot.
        /// <para>Named for its unit: <c>AudioSource.time</c> is SECONDS, so a bare Seek would read as
        /// seconds to anyone coming from Unity's own API, and the two spellings differ by a factor of
        /// the sample rate. Same reason as <see cref="SetRegionFrames"/> below.</para></summary>
        public void SeekFrames(ulong frame) { if (Live) Bwa.bwa_source_seek(Eng, _src, frame); }

        /// <summary>The seconds twin of <see cref="SeekFrames"/> (converted at the engine's sample
        /// rate). A negative position is ignored.</summary>
        public void SeekSeconds(double seconds)
        {
            if (!Live || seconds < 0.0) return;
            double rate = Engine.Instance ? Engine.Instance.sampleRate : 0;
            if (rate <= 0.0) return;
            SeekFrames((ulong)(seconds * rate));
        }

        /// <summary>Bound playback to the region <c>[startFrame, endFrame)</c> of the clip, in engine-rate
        /// frames; <c>endFrame</c> 0 means the clip end. A looping clip wraps back to <c>startFrame</c>
        /// (and raises <see cref="onLoop"/>); a one-shot ENDS at <c>endFrame</c> exactly as it would at
        /// the clip end (and raises <see cref="onFinished"/>). So a loop region and a truncated one-shot
        /// are one call.
        /// <para>Call it AFTER a Play: the bounds resolve against the bound clip, and any
        /// Play/PlayAt/PlayLoop resets the region. It does not move the playhead, so a region set
        /// mid-play takes effect at the next boundary. In-memory clips only; streamed clips ignore it.
        /// An <c>endFrame</c> at or below <c>startFrame</c> (and not 0) is refused.</para>
        /// <para>Named for its unit, like <see cref="PlayLoop"/>'s frames and the frames/seconds pair
        /// below: a bare SetRegion would read as seconds to anyone coming from AudioSource.time, and the
        /// two spellings differ by a factor of the sample rate.</para></summary>
        public void SetRegionFrames(ulong startFrame, ulong endFrame)
        {
            if (Live) Bwa.bwa_source_set_region(Eng, _src, startFrame, endFrame);
        }

        /// <summary>The seconds twin of <see cref="SetRegionFrames"/> (converted at the engine's sample
        /// rate). <c>endSeconds</c> 0 means the clip end, matching the frames form; a negative bound is
        /// ignored.</summary>
        public void SetRegionSeconds(double startSeconds, double endSeconds)
        {
            if (!Live || startSeconds < 0.0 || endSeconds < 0.0) return;
            double rate = Engine.Instance ? Engine.Instance.sampleRate : 0;
            if (rate <= 0.0) return;
            SetRegionFrames((ulong)(startSeconds * rate), (ulong)(endSeconds * rate));
        }

        /// <summary>Playback rate (1 = native, clamped [0.25, 4]); the rate GLIDES, so a change bends the
        /// pitch rather than stepping it. In-memory clips only — streamed clips ignore it.</summary>
        public float Pitch
        {
            get => pitch;
            set { pitch = value; if (Live) Bwa.bwa_source_set_pitch(Eng, _src, value); }
        }

        /// <summary>Fire a one-shot at this transform (transient voice; no handle held).
        /// Returns whether it was ACCEPTED — false means the clip never loaded, or the voice pool
        /// or command ring was momentarily full and it was dropped. There is no handle to poll,
        /// so this is the only signal; Bwa.LastError says which it was.</summary>
        public bool PlayOneShot(string oneShotClip = null)
        {
            if (Eng == IntPtr.Zero) return false;
            uint snd = Resolve(oneShotClip, out bool held);
            if (snd == 0) return false;
            // A one-shot owns no handle you could start later, so the engine refuses a not-ready one
            // rather than holding it. Report it here, where the caller's false has a reason.
            if (held)
            {
                Debug.LogWarning("[Emitter] PlayOneShot on " + name + " skipped: '" + (oneShotClip ?? clip) +
                                 "' is still decoding, and a one-shot cannot be held. Wait for the load " +
                                 "(Engine.IsSoundReady) or turn Load Async off.");
                return false;
            }
            var p = Room.Pos(transform.position);
            return Bwa.bwa_play_oneshot(Eng, snd, p.x, p.y, p.z, gain);
        }

        // The shared OnValidate re-pushes every source-generic knob; pitch is the clip-only extra.
        protected override void OnValidate()
        {
            base.OnValidate();
            if (!Application.isPlaying || !Live) return;
            Bwa.bwa_source_set_pitch(Eng, _src, pitch);
        }
    }
}
