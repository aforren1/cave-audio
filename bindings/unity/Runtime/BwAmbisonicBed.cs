// BwAmbisonicBed.cs — a world-locked AmbiX soundfield (diffuse/ambient), decoded straight to the 26
// speakers. NOT a positional source: there is no transform position (the field is fixed to the room;
// the listener moving through it is handled by the real speakers + the binaural monitor's head-track).
// Use for room tone, ambience, or a music bed. Wraps bw_bed_*.
//
// The clip must be a MULTICHANNEL AmbiX file (4 / 9 / 16 channels = ambisonic order 1 / 2 / 3); a mono
// file is rejected by the engine. FLAC is the natural lossless container; MP3 can't carry ambisonics.
using System;
using UnityEngine;

namespace CaveAudio
{
    public sealed class BwAmbisonicBed : MonoBehaviour
    {
        [Header("AmbiX clip (under StreamingAssets; 4 / 9 / 16-ch)")]
        [BwClip] public string clip = "ambience.flac";
        public bool loop = true;
        public bool playOnEnable = true;
        [Range(0f, 1f)] public float gain = 1f;

        uint _bed;
        bool _created;
        IntPtr Eng => BwAudio.Instance ? BwAudio.Instance.Handle : IntPtr.Zero;

        void OnEnable()
        {
            if (Eng == IntPtr.Zero) { enabled = false; return; }   // BwAudio not ready yet
            _bed = Bw.bw_bed_create(Eng);
            _created = true;
            Bw.bw_bed_set_gain(Eng, _bed, gain);
            if (playOnEnable) Play();
        }

        /// <summary>Play `clip` (or an override) as the soundfield, loading it on demand.</summary>
        public void Play(string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            uint snd = BwAudio.Instance.LoadAmbix(clipOverride ?? clip);   // rejects mono
            if (snd != 0) Bw.bw_bed_play(Eng, _bed, snd, loop);
        }

        public void Stop() { if (_created && Eng != IntPtr.Zero) Bw.bw_bed_stop(Eng, _bed); }

        /// <summary>Master gain of the bed (ramped); applies immediately if live.</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (_created && Eng != IntPtr.Zero) Bw.bw_bed_set_gain(Eng, _bed, value); }
        }

        void OnDisable()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            Bw.bw_bed_destroy(Eng, _bed);
            _created = false;
        }
    }
}
