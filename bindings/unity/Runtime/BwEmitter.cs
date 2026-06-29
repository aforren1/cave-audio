// BwEmitter.cs — a positional sound source. Attach to any GameObject; its transform drives the
// source position (and orientation, for directional sources) every frame via BwAudio's centralized
// push. Audio files live under StreamingAssets and are decoded by the engine (not Unity's AudioClip).
using System;
using System.Collections;
using UnityEngine;
using UnityEngine.Events;

namespace CaveAudio
{
    public sealed class BwEmitter : MonoBehaviour
    {
        [Header("Clip (under StreamingAssets)")]
        [BwClip] public string clip = "sfx/footsteps.wav";
        public bool loop = true;
        public bool playOnEnable = true;
        [Range(0f, 1f)] public float gain = 1f;

        [Header("Spatial")]
        public bool occlusion = false;                       // geometry between source + listener attenuates it
        public bool reflections = false;                     // contribute to the shared reverb bed
        public BwDirectivity directivity = BwDirectivity.Omni;
        [Range(1f, 8f)] public float directivityPower = 1f;  // sharpness of the lobe (cardioid/figure-8)

        [Header("Events")]
        [Tooltip("Fires when the source stops producing audio — a non-looping clip finished, or Stop().")]
        public UnityEvent onFinished = new UnityEvent();

        uint _src;
        bool _created;
        bool _wasPlaying;     // for the play->stop edge that drives onFinished
        IntPtr Eng => BwAudio.Instance ? BwAudio.Instance.Handle : IntPtr.Zero;

        /// <summary>Is this source still producing audio? (AudioSource.isPlaying equivalent.)</summary>
        public bool IsPlaying => _created && Eng != IntPtr.Zero && Bw.bw_source_is_playing(Eng, _src);

        void OnEnable()
        {
            if (!TryInit()) StartCoroutine(InitWhenReady());   // create now, or retry until BwAudio is ready
        }

        // Wait out an init-order race (this emitter enabled before BwAudio finished starting) instead of
        // permanently disabling. Unity stops the coroutine automatically when the component is disabled.
        IEnumerator InitWhenReady()
        {
            while (!_created) { yield return null; TryInit(); }
        }

        // Create the engine source + apply settings. Returns false (leaving _created false) if BwAudio
        // isn't ready yet. Resets _wasPlaying so a recycled component never inherits a stale play edge.
        bool TryInit()
        {
            if (_created) return true;
            if (Eng == IntPtr.Zero) return false;          // BwAudio not ready -> caller retries
            _src = Bw.bw_source_create(Eng);
            _created = true;
            _wasPlaying = false;
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
            if (playOnEnable) Play();
            BwAudio.Instance.Register(this);
            return true;
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

            // playback edge -> onFinished (poll the engine's per-source playing state). Best-effort: the
            // play is observed a frame or two after Play() (it's a queued command), and a clip shorter
            // than the frame interval may never read as playing, so onFinished can be missed for it.
            bool now = Bw.bw_source_is_playing(Eng, _src);
            if (now) _wasPlaying = true;
            else if (_wasPlaying) { _wasPlaying = false; onFinished.Invoke(); }
        }

        /// <summary>Play `clip` (or an override), loading it on demand — AudioSource.Play equivalent.</summary>
        public void Play(string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            uint snd = BwAudio.Instance.Load(clipOverride ?? clip);
            if (snd != 0) Bw.bw_source_play(Eng, _src, snd, loop);
        }

        /// <summary>Stop this source — AudioSource.Stop equivalent.</summary>
        public void Stop() { if (_created && Eng != IntPtr.Zero) Bw.bw_source_stop(Eng, _src); }

        /// <summary>Linear gain — AudioSource.volume equivalent; applies immediately if live.</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (_created && Eng != IntPtr.Zero) Bw.bw_source_set_gain(Eng, _src, value); }
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
            _wasPlaying = false;                 // never carry a stale play edge into a re-enable
            StopAllCoroutines();                 // cancel a pending InitWhenReady
            if (_created && Eng != IntPtr.Zero)
            {
                if (BwAudio.Instance) BwAudio.Instance.Unregister(this);
                Bw.bw_source_destroy(Eng, _src);
            }
            _created = false;
        }
    }
}
