// AmbisonicBed.cs — a world-locked AmbiX soundfield (diffuse/ambient), decoded straight to the 26
// speakers. NOT a positional source: there is no transform position (the field is fixed to the room;
// the listener moving through it is handled by the real speakers + the binaural monitor's head-track).
// Use for room tone, ambience, or a music bed. Wraps bwa_bed_*.
//
// The clip must be a MULTICHANNEL AmbiX file (4 / 9 / 16 channels = ambisonic order 1 / 2 / 3); a mono
// file is rejected by the engine. FLAC is the natural lossless container; MP3 can't carry ambisonics.
using System;
using UnityEngine;

namespace BwAudio
{
    public sealed class AmbisonicBed : MonoBehaviour
    {
        [Header("AmbiX clip (under StreamingAssets; 4 / 9 / 16-ch)")]
        [Clip] public string clip = "ambience.flac";
        public bool loop = true;
        public bool playOnEnable = true;
        [Range(0f, 1f)] public float gain = 1f;
        [Tooltip("Turn the soundfield about the vertical axis (degrees, Unity's sense) — line a recording " +
                 "up with the scene, or spin it slowly for effect. Glides to the target (~1 turn/s), so it " +
                 "is click-free live.")]
        [Range(-180f, 180f)] public float yawDegrees = 0f;

        uint _bed;
        bool _created;
        IntPtr Eng => Engine.Instance ? Engine.Instance.Handle : IntPtr.Zero;

        void OnEnable()
        {
            if (Eng == IntPtr.Zero) { enabled = false; return; }   // Engine not ready yet
            _bed = Bwa.bwa_bed_create(Eng);
            _created = true;
            Bwa.bwa_bed_set_gain(Eng, _bed, gain);
            if (yawDegrees != 0f) Bwa.bwa_bed_set_rotation(Eng, _bed, Room.YawRad(yawDegrees));
            if (playOnEnable) Play();
        }

        /// <summary>Play `clip` (or an override) as the soundfield, loading it on demand.</summary>
        public void Play(string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            uint snd = Engine.Instance.LoadAmbix(clipOverride ?? clip);   // rejects mono
            if (snd != 0) Bwa.bwa_bed_play(Eng, _bed, snd, loop);
        }

        public void Stop() { if (_created && Eng != IntPtr.Zero) Bwa.bwa_bed_stop(Eng, _bed); }

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
            set { yawDegrees = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_bed_set_rotation(Eng, _bed, Room.YawRad(value)); }
        }

        // Inspector edits are audible in Play mode (the engine glides to the new yaw).
        void OnValidate()
        {
            if (Application.isPlaying && _created && Eng != IntPtr.Zero)
            {
                Bwa.bwa_bed_set_gain(Eng, _bed, gain);
                Bwa.bwa_bed_set_rotation(Eng, _bed, Room.YawRad(yawDegrees));
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
