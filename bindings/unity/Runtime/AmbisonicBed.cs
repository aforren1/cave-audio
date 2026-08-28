// AmbisonicBed.cs — a world-locked AmbiX soundfield (diffuse/ambient), decoded straight to the 26
// speakers. NOT a positional source: there is no transform position (the field is fixed to the room;
// the listener moving through it is handled by the real speakers + the binaural monitor's head-track).
// Use for room tone, ambience, or a music bed. Wraps bwa_bed_*.
//
// The clip must be a MULTICHANNEL B-format file (4 / 9 / 16 channels = ambisonic order 1 / 2 / 3); a
// mono file is rejected by the engine. AmbiX by default; tick `fumaClip` for legacy FuMa recordings
// (converted at load). FLAC is the natural lossless container; MP3 can't carry ambisonics.
using System;
using System.Collections;
using UnityEngine;
using UnityEngine.Events;

namespace BwAudio
{
    public sealed class AmbisonicBed : MonoBehaviour
    {
        [Header("AmbiX clip (under StreamingAssets; 4 / 9 / 16-ch)")]
        [Clip] public string clip = "ambience.flac";
        [Tooltip("The clip is legacy FuMa B-format (.amb-style: WXYZ order, MaxN, W -3 dB) rather than " +
                 "AmbiX — converted at load, so everything downstream is identical.")]
        public bool fumaClip = false;
        public bool loop = true;
        public bool playOnEnable = true;
        [Tooltip("Decode the soundfield on the engine's loader thread instead of blocking this one. " +
                 "Play() returns immediately and the bed stays SILENT until the data lands, then starts " +
                 "from the top with nothing skipped. Beds are the biggest assets in a scene (4/9/16 " +
                 "channels, usually long), so this is the one that hurts most to load in-frame. For " +
                 "content that appears MID-SESSION (a streamed-in level, a downloaded ambience). Leave " +
                 "OFF for the CAVE's normal path, which is load-time and synchronous.")]
        public bool loadAsync = false;
        [Range(0f, 1f)] public float gain = 1f;
        [Tooltip("Turn the soundfield about the vertical axis (degrees, Unity's sense) — line a recording " +
                 "up with the scene, or spin it slowly for effect. Glides to the target (~1 turn/s), so it " +
                 "is click-free live.")]
        [Range(-180f, 180f)] public float yawDegrees = 0f;
        [Tooltip("Tilt the field's front upward (+) or downward (-) — LEVEL a capture that wasn't upright, " +
                 "or tilt it for effect. Same glide as yaw. Applied roll, then pitch, then yaw.")]
        [Range(-90f, 90f)] public float pitchDegrees = 0f;
        [Tooltip("Tilt the field's top toward the scene's right (+) or left (-). Same glide as yaw.")]
        [Range(-180f, 180f)] public float rollDegrees = 0f;

        [Header("Events")]
        [Tooltip("Fires when the bed stops producing audio — a non-looping soundfield finished, a play " +
                 "region ended, or Stop().")]
        public UnityEvent onFinished = new UnityEvent();
        [Tooltip("Fires each time a LOOPING bed wraps back to its loop start. A looping bed never " +
                 "finishes, so onFinished reports it never — this is the event to pace experimental " +
                 "trials or cue visuals from. One call per wrap, so a loop shorter than a frame fires " +
                 "several times in one frame.")]
        public UnityEvent onLoop = new UnityEvent();

        uint _bed;
        bool _created;
        uint _pending;                         // an async asset bound to this bed but not yet decoded (0 = none)
        bool _watching;                        // a WatchPendingLoad coroutine is running
        Engine _owner;                         // the Engine this bed was created under

        // onFinished has the same two feeds as Emitter's, because a bed IS a voice and the engine
        // reports only one of the two ways one goes quiet. A bed that RAN OUT (a non-looping
        // soundfield finished, a play region's end) posts an ended EVENT the block it happens, which
        // Engine drains with bwa_poll_ended and routes to NotifyEnded — that path cannot miss a
        // soundfield shorter than a frame. An explicit HALT (Stop, StopAt, FadeOut,
        // Engine.StopGroup/StopAll) posts no event at all, so the IsPlaying edge below covers it,
        // exactly as it does for an Emitter. `_endFired` keeps the two from both reporting one end.
        // `_endFired` is cleared by the next play that BINDS. A play held on an async decode has not
        // bound, so it does not clear it - see Resolve and WatchLoop.
        bool _wasPlaying;     // we have seen the CURRENT play as playing, so a fall to silence is an end
        bool _endFired;       // the edge fallback already reported this end; swallow the event if one follows

        // Valid only while the CREATING Engine is still the live instance (SourceBase's guard, same
        // reason): bed handles are slot+generation like source handles, so a destroyed+recreated
        // Engine mints deterministically colliding ones (a fresh engine's first bed is slot 0, gen 1
        // again). A stale _bed must never reach a successor engine — it would drive, or in OnDisable
        // DESTROY, whatever occupies the colliding slot there. Once the owner is gone this reads
        // Zero, every op no-ops, and the next enable re-creates under the new engine.
        IntPtr Eng => _owner != null && ReferenceEquals(Engine.Instance, _owner) ? _owner.Handle : IntPtr.Zero;

        // Guard shorthand: the bed exists and its engine is still the live one.
        bool Live => _created && Eng != IntPtr.Zero;

        void OnEnable()
        {
            var engine = Engine.Instance;
            if (!engine || engine.Handle == IntPtr.Zero) { enabled = false; return; }   // Engine not ready yet
            _owner = engine;
            _bed = Bwa.bwa_bed_create(Eng);
            _created = true;
            _wasPlaying = false;                 // a recycled component never inherits a stale play edge
            _endFired = false;
            Bwa.bwa_bed_set_gain(Eng, _bed, gain);
            if (yawDegrees != 0f || pitchDegrees != 0f || rollDegrees != 0f) ApplyOrientation();
            engine.RegisterBed(this);            // the completion / loop event route
            if (playOnEnable) Play();
        }

        // ---- the event feed, driven by Engine (see Engine.RegisterBed) -------------------------------

        // The native handle, for Engine's handle -> component route. 0 when the create failed.
        // Deliberately NOT gated on _created: OnDisable clears that flag, and UnregisterBed still has
        // to find the map entry to remove it.
        internal uint NativeHandle => _bed;

        /// <summary>The engine reported this bed's voice as ENDED (Engine drained it from
        /// bwa_poll_ended after the commit). Fires onFinished unless the halt fallback already did.
        /// </summary>
        internal void NotifyEnded()
        {
            // The fallback runs a hair earlier in the frame than the event describing the same end can
            // be drained, so ONE end can reach both feeds. Whichever reports first wins; this latch
            // eats the straggler. Cleared by the next play, so a swallowed straggler can never eat a
            // LATER end — including the sub-frame soundfield this path exists to catch.
            if (_endFired) { _endFired = false; return; }
            _wasPlaying = false;
            onFinished.Invoke();
        }

        /// <summary>The engine reported this bed's voice as having WRAPPED at a loop point (Engine
        /// drained it from bwa_poll_looped after the commit). Called once per wrap.</summary>
        internal void NotifyLooped()
        {
            // No latch and no _wasPlaying touch: a wrap is not an end. The bed is still playing, both
            // feeds of onFinished are about a voice going quiet, and neither can produce a wrap.
            onLoop.Invoke();
        }

        /// <summary>Called once per frame by Engine, after the commit and both event drains.</summary>
        internal void PostCommit()
        {
            if (!Live) return;

            // The explicit-halt fallback. Reading AFTER the drain is what keeps a natural end from
            // being reported twice: NotifyEnded has already cleared _wasPlaying by the time this runs,
            // so the fall to silence it left behind is not an edge any more. A play held on an async
            // decode never arms this: it honestly reads "not playing" while it waits, and _wasPlaying
            // only latches on a bed actually observed playing.
            bool now = Bwa.bwa_bed_is_playing(Eng, _bed);
            if (now) _wasPlaying = true;
            else if (_wasPlaying) { _wasPlaying = false; _endFired = true; onFinished.Invoke(); }
        }

        // Unity degrees -> the engine's room-frame radians. Yaw goes through Room.YawRad (the X mirror
        // reverses its sense, and it folds any registration yaw); pitch and roll pass through with the
        // SAME sense — "front tilts up" never touches the mirrored axis, and Unity-right maps to
        // room-right, so both already mean what the engine means.
        void ApplyOrientation()
        {
            if (Live)
                Bwa.bwa_bed_set_orientation(Eng, _bed, Room.YawRad(yawDegrees),
                                            pitchDegrees * Mathf.Deg2Rad, rollDegrees * Mathf.Deg2Rad);
        }

        // The load flags ARE the bed's kind: the engine judges an async play against them (the asset
        // reports 0 channels until its decode lands, so the channel count cannot judge it yet), and a
        // handle acquired mono would be refused by bwa_bed_play. FuMa converts to AmbiX at load, so
        // past the acquire the two are the same asset kind.
        BwaLoadFlags LoadFlags => fumaClip ? BwaLoadFlags.Fuma : BwaLoadFlags.Ambix;

        /// <summary>Play `clip` (or an override) as the soundfield, loading it on demand.
        /// With `loadAsync` on this returns immediately and the field starts, from its first frame, on
        /// the block its data lands.</summary>
        public void Play(string clipOverride = null)
        {
            if (Resolve(clipOverride, out uint snd)) Bwa.bwa_bed_play(Eng, _bed, snd, loop);
        }

        /// <summary>Sample-accurate scheduled play — AudioSource.PlayScheduled equivalent, on the
        /// engine's dsp clock instead of AudioSettings.dspTime: the soundfield begins exactly when
        /// Engine.DspTimeFrames reaches `startSample`. Schedule with margin (at least a block); a start
        /// already in the past plays immediately.
        /// <para>With `loadAsync` on, the schedule is only as good as the decode: a start time that
        /// arrives before the data does plays as soon as the data lands, not at `startSample`.</para>
        /// </summary>
        public void PlayAt(ulong startSample, string clipOverride = null)
        {
            if (Resolve(clipOverride, out uint snd)) Bwa.bwa_bed_play_at(Eng, _bed, snd, loop, startSample);
        }

        /// <summary>Intro then looping body inside one soundfield file: FRAMES [0, loopBeg) play once,
        /// then [loopBeg, loopEnd) repeats. 0/0 loops the whole asset. The region IS the loop, so the
        /// `loop` field does not apply here.</summary>
        public void PlayLoop(ulong loopBeg, ulong loopEnd, string clipOverride = null)
        {
            if (Resolve(clipOverride, out uint snd)) Bwa.bwa_bed_play_loop(Eng, _bed, snd, loopBeg, loopEnd);
        }

        /// <summary>Scheduled stop on the same dsp clock as <see cref="PlayAt"/>: the bed takes the
        /// click-free stop path when Engine.DspTimeFrames reaches `stopSample`. Fires onFinished.</summary>
        public void StopAt(ulong stopSample) { if (Live) Bwa.bwa_bed_stop_at(Eng, _bed, stopSample); }

        /// <summary>Bound playback to FRAMES [startFrame, endFrame) of the soundfield; endFrame 0 means
        /// the asset end. A looping bed wraps back to `startFrame` (and fires onLoop); a one-shot ENDS
        /// at `endFrame` exactly as it would at the asset end (and fires onFinished).
        /// <para>Call it AFTER a play: the bounds resolve against the bound asset, and any play resets
        /// the region. It does not move the playhead, so a region set mid-play takes effect at the next
        /// boundary. An `endFrame` at or below `startFrame` (and not 0) is refused.</para>
        /// <para>Named for the unit, like <see cref="Emitter.SetRegionFrames"/>: the seconds spelling
        /// differs from the frames one by a factor of the sample rate, and a bare SetRegion would read
        /// as seconds to anyone arriving from AudioSource.time.</para></summary>
        public void SetRegionFrames(ulong startFrame, ulong endFrame)
        {
            if (Live) Bwa.bwa_bed_set_region(Eng, _bed, startFrame, endFrame);
        }

        /// <summary>The seconds twin of <see cref="SetRegionFrames"/> (converted at the engine's sample
        /// rate). 0 for `endSeconds` still means the asset end.</summary>
        public void SetRegionSeconds(double startSeconds, double endSeconds)
        {
            if (!Live || startSeconds < 0.0 || endSeconds < 0.0) return;
            double rate = Engine.Instance ? Engine.Instance.sampleRate : 0;
            if (rate <= 0.0) return;
            // 0 seconds means the asset end here too, and 0 * rate is 0, so the sentinel survives the
            // conversion without a special case.
            SetRegionFrames((ulong)(startSeconds * rate), (ulong)(endSeconds * rate));
        }

        // The one play path: resolve the clip to a soundfield handle, arm the event state, and start
        // (or renew) the async watch. Shared by all three play forms so they cannot drift on which one
        // clears the duplicate-end latch. False = nothing to play (Acquire has reported why).
        bool Resolve(string clipOverride, out uint snd)
        {
            snd = 0;
            if (!Live) return false;
            var engine = Engine.Instance;
            if (!engine) return false;
            var path = clipOverride ?? clip;
            bool held = false;
            if (loadAsync)
            {
                snd = engine.AcquireAsync(path, LoadFlags);            // returns at once; rt holds the play
                if (snd != 0) held = !engine.IsSoundReady(snd, out _);
            }
            else snd = engine.Acquire(path, LoadFlags);                // blocking decode; rejects mono
            if (snd == 0) return false;
            // Only a play that BINDS voids the previous end's duplicate suppression. A play HELD on an
            // async decode has not bound: the engine bumps its per-slot play counter at bind time, and
            // that counter is the gate that drops a completion straggling in from the PREVIOUS play, so
            // until it moves the straggler is still deliverable and this latch is all that stands in its
            // way. WatchLoop clears it instead, on the frame the data lands - which is the bind.
            if (!held) _endFired = false;
            // Always overwrite: a second play that resolved READY supersedes a still-pending first one,
            // and leaving the watch on the old handle would later warn that the bed "stays silent" while
            // the new clip is audibly playing.
            _pending = held ? snd : 0;          // a watch already running picks the new handle up
            if (held && !_watching) StartCoroutine(WatchLoop());
            return true;
        }

        // While an async load is in flight the bed is bound and silent, which looks exactly like a decode
        // that FAILED — a failure never becomes ready. A bed is world-locked, so unlike a source it has
        // no per-frame push to poll from; the watch is a coroutine that exists only while a load is in
        // flight, and Unity stops it when the component is disabled (OnDisable clears _pending, so a
        // re-enable starts a fresh one rather than leaving a dead handle behind).
        IEnumerator WatchLoop()
        {
            _watching = true;                   // set before StartCoroutine returns (the body runs to the
            while (_pending != 0)               // first yield inline), so Play never starts a second watch
            {
                yield return null;
                // Live-gated like Emitter's poll: the engine can be destroyed and replaced mid-decode,
                // and the helper resolves Engine.Instance fresh, so an ungated probe would ask the
                // SUCCESSOR engine about this engine's handle. Drop the watch instead.
                if (!Live) { _pending = 0; break; }
                _pending = Engine.WatchPendingLoad(_pending, this, "bed", out bool landed);
                // That probe is a pump point, so it is the call that adopted the decode and released the
                // held play into the engine: the play binds HERE, and only here may a held play void the
                // previous end's suppression. See Resolve.
                if (landed) _endFired = false;
            }
            _watching = false;
        }

        public void Stop()
        {
            _pending = 0;                        // the engine cancels a held play on stop; stop watching it
            if (Live) Bwa.bwa_bed_stop(Eng, _bed);
        }

        /// <summary>Current playhead into the soundfield clip, in engine-rate FRAMES (latest-wins
        /// readback, ~one audio block of lag; freezes while the bed is paused). 0 while idle.
        /// Named for its unit, like SourceBase.PlayheadFrames and the SetRegionFrames/Seconds pair
        /// beside it.</summary>
        public ulong PlayheadFrames => Live ? Bwa.bwa_bed_get_playhead_frames(Eng, _bed) : 0;

        /// <summary>The seconds twin of <see cref="PlayheadFrames"/> (over the engine sample rate) —
        /// the same pair SourceBase carries, so a bed and a source read alike.</summary>
        public double PlayheadSeconds => Engine.Instance ? PlayheadFrames / (double)Engine.Instance.sampleRate : 0.0;

        /// <summary>Master gain of the bed (ramped); applies immediately if live.</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (Live) Bwa.bwa_bed_set_gain(Eng, _bed, value); }
        }

        /// <summary>Yaw of the soundfield in DEGREES, Unity's sense (positive = turn it to the right).
        /// Room.YawRad converts to the engine's frame — the X mirror reverses the sense of rotation, so a
        /// Unity angle handed straight to the engine would spin the field the wrong way.</summary>
        public float YawDegrees
        {
            get => yawDegrees;
            set { yawDegrees = value; ApplyOrientation(); }
        }

        /// <summary>Pitch in DEGREES: positive tilts the field's front upward (level a capture that
        /// wasn't upright). Glided like yaw; applied roll -> pitch -> yaw.</summary>
        public float PitchDegrees
        {
            get => pitchDegrees;
            set { pitchDegrees = value; ApplyOrientation(); }
        }

        /// <summary>Roll in DEGREES: positive tilts the field's top toward the scene's right.</summary>
        public float RollDegrees
        {
            get => rollDegrees;
            set { rollDegrees = value; ApplyOrientation(); }
        }

        // Inspector edits are audible in Play mode (the engine glides to the new orientation).
        void OnValidate()
        {
            if (Application.isPlaying && Live)
            {
                Bwa.bwa_bed_set_gain(Eng, _bed, gain);
                ApplyOrientation();
            }
        }

        void OnDisable()
        {
            StopAllCoroutines();                 // cancel a running WatchPendingLoad
            _pending = 0; _watching = false;    // StopAllCoroutines never runs its exit line
            _wasPlaying = false;                 // never carry a stale play edge into a re-enable
            _endFired = false;
            if (Live)                            // owner gone (engine destroyed/replaced) -> Live is false:
            {                                    // the stale handle never destroys a successor's bed
                _owner.UnregisterBed(this);
                Bwa.bwa_bed_destroy(Eng, _bed);
            }
            _created = false;
            _owner = null;
        }
    }
}
