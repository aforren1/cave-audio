// BwEmitter.cs — a positional sound source. Attach to any GameObject; its transform drives the
// source position (and orientation, for directional sources) every frame via BwAudio's centralized
// push. Audio files live under StreamingAssets and are decoded by the engine (not Unity's AudioClip).
using System;
using UnityEngine;

namespace CaveAudio
{
    public sealed class BwEmitter : MonoBehaviour
    {
        [Header("Clip (under StreamingAssets)")]
        public string clip = "sfx/footsteps.wav";
        public bool loop = true;
        public bool playOnEnable = true;
        [Range(0f, 1f)] public float gain = 1f;

        [Header("Spatial")]
        public bool occlusion = false;                       // geometry between source + listener attenuates it
        public bool reflections = false;                     // contribute to the shared reverb bed
        public BwDirectivity directivity = BwDirectivity.Omni;
        [Range(1f, 8f)] public float directivityPower = 1f;  // sharpness of the lobe (cardioid/figure-8)

        uint _src;
        bool _created;
        IntPtr Eng => BwAudio.Instance ? BwAudio.Instance.Handle : IntPtr.Zero;

        void OnEnable()
        {
            if (Eng == IntPtr.Zero) { enabled = false; return; }   // BwAudio not ready yet
            _src = Bw.bw_source_create(Eng);
            _created = true;
            Bw.bw_source_set_gain(Eng, _src, gain);
            if (occlusion)   Bw.bw_source_set_occlusion(Eng, _src, true);
            if (reflections) Bw.bw_source_set_reflections(Eng, _src, true);
            if (directivity != BwDirectivity.Omni)
            {
                Bw.bw_source_set_directivity_preset(Eng, _src, directivity);
                if (directivityPower != 1f)
                {
                    // preset sets weight; re-issue with the chosen power (weight from the preset table)
                    float w = directivity == BwDirectivity.Cardioid ? 0.5f : 1.0f;
                    Bw.bw_source_set_directivity(Eng, _src, w, directivityPower);
                }
            }
            Push();
            if (playOnEnable)
            {
                uint snd = BwAudio.Instance.Load(clip);
                if (snd != 0) Bw.bw_source_play(Eng, _src, snd, loop);
            }
            BwAudio.Instance.Register(this);
        }

        /// <summary>Called once per frame by BwAudio (before the listener + commit).</summary>
        public void Push()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            var p = Room.Pos(transform.position);
            Bw.bw_source_set_pos(Eng, _src, p.x, p.y, p.z);
            if (directivity != BwDirectivity.Omni)
            {
                var q = Room.Rot(transform.rotation);            // the source's forward axis drives the lobe
                Bw.bw_source_set_orientation(Eng, _src, q.x, q.y, q.z, q.w);
            }
        }

        /// <summary>Fire a one-shot at this transform (transient voice; no handle held).</summary>
        public void PlayOneShot(string oneShotClip = null)
        {
            if (Eng == IntPtr.Zero) return;
            uint snd = BwAudio.Instance.Load(oneShotClip ?? clip);
            if (snd == 0) return;
            var p = Room.Pos(transform.position);
            Bw.bw_play_oneshot(Eng, snd, p.x, p.y, p.z, gain);
        }

        /// <summary>Current occlusion factor (1 = clear .. 0 = fully blocked) for a HUD/debug readout.</summary>
        public float Occlusion => (_created && Eng != IntPtr.Zero) ? Bw.bw_source_get_occlusion(Eng, _src) : 1f;

        void OnDisable()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            if (BwAudio.Instance) BwAudio.Instance.Unregister(this);
            Bw.bw_source_destroy(Eng, _src);
            _created = false;
        }
    }
}
