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

        uint _bed;
        bool _created;
        uint _pending;                         // an async asset bound to this bed but not yet decoded (0 = none)
        bool _watching;                        // a WatchPendingLoad coroutine is running
        Engine _owner;                         // the Engine this bed was created under

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
            Bwa.bwa_bed_set_gain(Eng, _bed, gain);
            if (yawDegrees != 0f || pitchDegrees != 0f || rollDegrees != 0f) ApplyOrientation();
            if (playOnEnable) Play();
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
            if (!Live) return;
            var engine = Engine.Instance;
            if (!engine) return;
            var path = clipOverride ?? clip;
            bool held = false;
            uint snd;
            if (loadAsync)
            {
                snd = engine.AcquireAsync(path, LoadFlags);            // returns at once; rt holds the play
                if (snd != 0) held = !engine.IsSoundReady(snd, out _);
            }
            else snd = engine.Acquire(path, LoadFlags);                // blocking decode; rejects mono
            if (snd == 0) return;
            Bwa.bwa_bed_play(Eng, _bed, snd, loop);
            // Always overwrite: a second Play that resolved READY supersedes a still-pending first one,
            // and leaving the watch on the old handle would later warn that the bed "stays silent" while
            // the new clip is audibly playing.
            _pending = held ? snd : 0;          // a watch already running picks the new handle up
            if (held && !_watching) StartCoroutine(WatchLoop());
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
                _pending = Engine.WatchPendingLoad(_pending, this, "bed");
            }
            _watching = false;
        }

        public void Stop()
        {
            _pending = 0;                        // the engine cancels a held play on stop; stop watching it
            if (Live) Bwa.bwa_bed_stop(Eng, _bed);
        }

        /// <summary>Current playhead into the soundfield clip, in engine-rate frames (latest-wins
        /// readback, ~one audio block of lag; freezes while the bed is paused). 0 while idle;
        /// seconds = Playhead / Engine.Instance.sampleRate.</summary>
        public ulong Playhead => Live ? Bwa.bwa_bed_get_playhead_frames(Eng, _bed) : 0;

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
            if (Live)                            // owner gone (engine destroyed/replaced) -> Live is false:
                Bwa.bwa_bed_destroy(Eng, _bed);  // the stale handle never destroys a successor's bed
            _created = false;
            _owner = null;
        }
    }
}
