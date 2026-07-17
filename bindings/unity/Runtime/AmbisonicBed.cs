// AmbisonicBed.cs — a world-locked AmbiX soundfield (diffuse/ambient), decoded straight to the 26
// speakers. NOT a positional source: there is no transform position (the field is fixed to the room;
// the listener moving through it is handled by the real speakers + the binaural monitor's head-track).
// Use for room tone, ambience, or a music bed. Wraps bwa_bed_*.
//
// The clip must be a MULTICHANNEL B-format file (4 / 9 / 16 channels = ambisonic order 1 / 2 / 3); a
// mono file is rejected by the engine. AmbiX by default; tick `fumaClip` for legacy FuMa recordings
// (converted at load). FLAC is the natural lossless container; MP3 can't carry ambisonics.
using System;
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
        IntPtr Eng => Engine.Instance ? Engine.Instance.Handle : IntPtr.Zero;

        void OnEnable()
        {
            if (Eng == IntPtr.Zero) { enabled = false; return; }   // Engine not ready yet
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
            if (_created && Eng != IntPtr.Zero)
                Bwa.bwa_bed_set_orientation(Eng, _bed, Room.YawRad(yawDegrees),
                                            pitchDegrees * Mathf.Deg2Rad, rollDegrees * Mathf.Deg2Rad);
        }

        /// <summary>Play `clip` (or an override) as the soundfield, loading it on demand.</summary>
        public void Play(string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            var path = clipOverride ?? clip;
            uint snd = fumaClip ? Engine.Instance.LoadFuma(path)      // FuMa converts at load
                                : Engine.Instance.LoadAmbix(path);    // rejects mono
            if (snd != 0) Bwa.bwa_bed_play(Eng, _bed, snd, loop);
        }

        public void Stop() { if (_created && Eng != IntPtr.Zero) Bwa.bwa_bed_stop(Eng, _bed); }

        /// <summary>Current playhead into the soundfield clip, in engine-rate frames (latest-wins
        /// readback, ~one audio block of lag; freezes while the bed is paused). 0 while idle;
        /// seconds = Playhead / Engine.Instance.sampleRate.</summary>
        public ulong Playhead => _created && Eng != IntPtr.Zero ? Bwa.bwa_bed_get_playhead(Eng, _bed) : 0;

        [System.Obsolete("Renamed to Playhead.")]
        public ulong Position => Playhead;

        /// <summary>Master gain of the bed (ramped); applies immediately if live.</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_bed_set_gain(Eng, _bed, value); }
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
            if (Application.isPlaying && _created && Eng != IntPtr.Zero)
            {
                Bwa.bwa_bed_set_gain(Eng, _bed, gain);
                ApplyOrientation();
            }
        }

        void OnDisable()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            Bwa.bwa_bed_destroy(Eng, _bed);
            _created = false;
        }
    }
}
