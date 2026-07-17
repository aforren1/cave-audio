// Emitter.cs — a positional sound source. Attach to any GameObject; its transform drives the
// source position (and orientation, for directional sources) every frame via Engine's centralized
// push. Audio files live under StreamingAssets and are decoded by the engine (not Unity's AudioClip).
using System;
using System.Collections;
using UnityEngine;
using UnityEngine.Events;

namespace BwAudio
{
    public sealed class Emitter : MonoBehaviour
    {
        [Header("Clip (under StreamingAssets)")]
        [Clip] public string clip = "sfx/footsteps.wav";
        public bool loop = true;
        public bool playOnEnable = true;
        [Range(0f, 1f)] public float gain = 1f;
        [Tooltip("Playback rate (1 = native). In-memory clips only — streamed clips ignore it.")]
        [Range(0.25f, 4f)] public float pitch = 1f;

        [Header("Mixing")]
        [Tooltip("Voice-steal priority: when the voice pool is full, the LOWEST-priority source is stolen " +
                 "to make room. 255 = protected (music/critical SFX).")]
        [Range(0, 255)] public int priority = 128;
        [Tooltip("Mix group (0..7). Engine.SetGroupGain/SetGroupPaused duck or pause the whole group.")]
        [Range(0, 7)] public int group = 0;

        [Header("Spatial")]
        public bool occlusion = false;                       // geometry between source + listener attenuates it
        [Tooltip("Image-source EARLY reflections: the six first-order wall bounces, each rendered as a " +
                 "real point source at its mirrored position and panned like any other — so they keep " +
                 "correct direction AND parallax as the listener walks, which a shared reverb bed can't " +
                 "do. Needs a room box (Engine). No Steam Audio needed. Pairs with the FDN reverb, " +
                 "which renders the late tail.")]
        public bool earlyReflections = false;
        public bool reflections = false;                     // contribute to the shared reverb bed
        [Tooltip("Wet-send level into the shared reverb bed (needs Reflections).")]
        [Range(0f, 2f)] public float reflectionSend = 1f;
        [Tooltip("Scale the reverb send by distance to the listener: near = drier, far = wetter.")]
        public bool reflectionDistance = false;
        [Tooltip("When the direct line is blocked, route the sound around occluders / through openings. " +
                 "Needs scene geometry + Engine's 'Enable Pathing'.")]
        public bool pathing = false;
        public BwaDirectivity directivity = BwaDirectivity.Omni;
        [Range(1f, 8f)] public float directivityPower = 1f;  // sharpness of the lobe (cardioid/figure-8)

        [Header("Width")]
        [Tooltip("Angular width: 0 = a point, 1 = wide. For a crowd/waterfall/ambience that shouldn't " +
                 "collapse to one speaker.")]
        [Range(0f, 1f)] public float spread = 0f;
        [Tooltip("Physical radius in METRES (0 = point). The width becomes the angle the radius subtends " +
                 "from the listener, so the source stays the same PHYSICAL size as the listener walks. " +
                 "Floors `spread` — the larger of the two wins.")]
        public float sizeMetres = 0f;

        [Header("Propagation (opt-in; derived from source↔listener distance)")]
        [Tooltip("Render through the propagation delay (distance/c): pitch up approaching, down receding. " +
                 "Best for fast movers; adds the real propagation latency.")]
        public bool doppler = false;
        [Tooltip("Distance-driven HF low-pass — far sources sound duller.")]
        public bool airAbsorption = false;
        [Tooltip("LF shelf that tracks the distance attenuation, so a far source reads far, not THIN. " +
                 "A perceptual stylization — leave off for strict realism.")]
        public bool loudnessComp = false;

        [Header("Events")]
        [Tooltip("Fires when the source stops producing audio — a non-looping clip finished, or Stop().")]
        public UnityEvent onFinished = new UnityEvent();

        uint _src;
        bool _created;
        bool _wasPlaying;     // for the play->stop edge that drives onFinished
        IntPtr Eng => Engine.Instance ? Engine.Instance.Handle : IntPtr.Zero;

        /// <summary>Is this source still producing audio? (AudioSource.isPlaying equivalent.)</summary>
        public bool IsPlaying => _created && Eng != IntPtr.Zero && Bwa.bwa_source_is_playing(Eng, _src);

        void OnEnable()
        {
            if (!TryInit()) StartCoroutine(InitWhenReady());   // create now, or retry until Engine is ready
        }

        // Wait out an init-order race (this emitter enabled before Engine finished starting) instead of
        // permanently disabling. Unity stops the coroutine automatically when the component is disabled.
        IEnumerator InitWhenReady()
        {
            while (!_created) { yield return null; TryInit(); }
        }

        // Create the engine source + apply settings. Returns false (leaving _created false) if Engine
        // isn't ready yet. Resets _wasPlaying so a recycled component never inherits a stale play edge.
        bool TryInit()
        {
            if (_created) return true;
            if (Eng == IntPtr.Zero) return false;          // Engine not ready -> caller retries
            _src = Bwa.bwa_source_create(Eng);
            _created = true;
            _wasPlaying = false;
            _paused = false;
            Bwa.bwa_source_set_gain(Eng, _src, gain);
            Bwa.bwa_source_set_priority(Eng, _src, priority);
            if (group != 0) Bwa.bwa_source_set_group(Eng, _src, (uint)group);
            if (pitch != 1f) Bwa.bwa_source_set_pitch(Eng, _src, pitch);

            // The engine defaults every opt-in below to OFF/point/unity, so only push what differs — a
            // fresh source already IS the default (this runs again on every re-enable).
            if (occlusion)        Bwa.bwa_source_set_occlusion(Eng, _src, true);
            if (earlyReflections) Bwa.bwa_source_set_early_reflections(Eng, _src, true);
            if (reflections)
            {
                Bwa.bwa_source_set_reflections(Eng, _src, true);
                if (reflectionSend != 1f)  Bwa.bwa_source_set_reflection_send(Eng, _src, reflectionSend);
                if (reflectionDistance)    Bwa.bwa_source_set_reflection_distance(Eng, _src, true);
            }
            if (pathing)       Bwa.bwa_source_set_pathing(Eng, _src, true);
            if (spread > 0f)   Bwa.bwa_source_set_spread(Eng, _src, spread);
            if (sizeMetres > 0f) Bwa.bwa_source_set_size(Eng, _src, sizeMetres);
            if (doppler)       Bwa.bwa_source_set_doppler(Eng, _src, true);
            if (airAbsorption) Bwa.bwa_source_set_air_absorption(Eng, _src, true);
            if (loudnessComp)  Bwa.bwa_source_set_loudness_comp(Eng, _src, true);
            if (directivity != BwaDirectivity.Omni)
            {
                Bwa.bwa_source_set_directivity_preset(Eng, _src, directivity);
                if (directivityPower != 1f)
                {
                    // preset sets weight; re-issue with the chosen power (weight from the preset table)
                    float w = directivity == BwaDirectivity.Cardioid ? 0.5f : 1.0f;
                    Bwa.bwa_source_set_directivity(Eng, _src, w, directivityPower);
                }
            }
            Push();
            if (playOnEnable) Play();
            Engine.Instance.Register(this);
            return true;
        }

        /// <summary>Called once per frame by Engine (before the listener + commit).</summary>
        public void Push()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            var p = Room.Pos(transform.position);
            Bwa.bwa_source_set_pos(Eng, _src, p.x, p.y, p.z);
            if (directivity != BwaDirectivity.Omni)
            {
                var q = Room.Rot(transform.rotation);            // the source's forward axis drives the lobe
                Bwa.bwa_source_set_orientation(Eng, _src, q.x, q.y, q.z, q.w);
            }

            // playback edge -> onFinished (poll the engine's per-source playing state). Best-effort: the
            // play is observed a frame or two after Play() (it's a queued command), and a clip shorter
            // than the frame interval may never read as playing, so onFinished can be missed for it.
            bool now = Bwa.bwa_source_is_playing(Eng, _src);
            if (now) _wasPlaying = true;
            else if (_wasPlaying) { _wasPlaying = false; onFinished.Invoke(); }
        }

        /// <summary>Play `clip` (or an override), loading it on demand — AudioSource.Play equivalent.</summary>
        public void Play(string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            uint snd = Engine.Instance.Load(clipOverride ?? clip);
            if (snd != 0) { Bwa.bwa_source_play(Eng, _src, snd, loop); _paused = false; }   // play restarts un-paused
        }

        /// <summary>Sample-accurate scheduled play — AudioSource.PlayScheduled equivalent, on the
        /// engine's dsp clock instead of AudioSettings.dspTime: output begins exactly when
        /// Engine.DspTime reaches `startSample`. Schedule with margin (at least a block; e.g.
        /// <c>PlayAt(engine.DspTime + engine.sampleRate / 2)</c> starts half a second out); a start
        /// already in the past plays immediately. Keep the startSample you passed —
        /// <c>DspTime - startSample</c> is the sync clock for driving visuals (or poll Position).</summary>
        public void PlayAt(ulong startSample, string clipOverride = null)
        {
            if (!_created || Eng == IntPtr.Zero) return;
            uint snd = Engine.Instance.Load(clipOverride ?? clip);
            if (snd != 0) { Bwa.bwa_source_play_at(Eng, _src, snd, loop, startSample); _paused = false; }
        }

        /// <summary>Stop this source — AudioSource.Stop equivalent.</summary>
        public void Stop() { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_stop(Eng, _src); }

        /// <summary>Pause in place — AudioSource.Pause equivalent. Click-free (the engine ramps out
        /// over one block and freezes the playhead); a paused source still reads as IsPlaying.</summary>
        public void Pause() { Paused = true; }

        /// <summary>Resume from exactly where Pause landed — AudioSource.UnPause equivalent.</summary>
        public void UnPause() { Paused = false; }

        /// <summary>Paused state (set-driven; Play() always restarts un-paused).</summary>
        public bool Paused
        {
            get => _paused;
            set { _paused = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_paused(Eng, _src, value); }
        }
        bool _paused;

        /// <summary>Jump to `samples` (engine-rate frames) into the clip — AudioSource.timeSamples-set
        /// equivalent, click-free (ramp-out → jump → ramp-in). In-memory clips only; streamed clips
        /// ignore it. Past-the-end wraps a looping clip and ends a one-shot.</summary>
        public void Seek(ulong samples) { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_seek(Eng, _src, samples); }

        /// <summary>Current playhead in engine-rate frames — AudioSource.timeSamples-get equivalent
        /// (latest-wins readback, ~one audio block of lag). Engine-owned truth where deriving it from
        /// DspTime breaks: it freezes while Paused, lands where Seek lands, follows Pitch at the
        /// actual rate, and counts frames actually consumed for streamed clips. 0 while idle or
        /// before a scheduled PlayAt starts; a finished one-shot keeps its final position.</summary>
        public ulong Position => _created && Eng != IntPtr.Zero ? Bwa.bwa_source_get_position(Eng, _src) : 0;

        /// <summary>Playhead in seconds — AudioSource.time-get equivalent (Position over the engine
        /// sample rate).</summary>
        public double PositionSeconds => Engine.Instance ? Position / (double)Engine.Instance.sampleRate : 0.0;

        /// <summary>Linear gain — AudioSource.volume equivalent; applies immediately if live. Cancels a
        /// running FadeTo/FadeOut (an explicit gain wins over a fade).</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_gain(Eng, _src, value); }
        }

        /// <summary>Glide the gain to `target` over `seconds`, on the audio thread — no per-frame
        /// scripting, no coroutine. A later Gain-set or fade replaces it.</summary>
        public void FadeTo(float target, float seconds)
        {
            gain = target;   // keep the inspector field truthful about where the fade lands
            if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_fade_to(Eng, _src, target, seconds);
        }

        /// <summary>Fade to silence over `seconds`, then STOP the voice (the click-free stop path) — the
        /// one-call "fade this out and clean it up". Fires onFinished when the voice ends.</summary>
        public void FadeOut(float seconds) { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_fade_out(Eng, _src, seconds); }

        /// <summary>Playback rate (1 = native, clamped [0.25, 4]); the rate GLIDES, so a change bends the
        /// pitch rather than stepping it. In-memory clips only — streamed clips ignore it.</summary>
        public float Pitch
        {
            get => pitch;
            set { pitch = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_pitch(Eng, _src, value); }
        }

        /// <summary>Angular width (0 = point .. 1 = wide). Floored by SizeMetres when that is set.</summary>
        public float Spread
        {
            get => spread;
            set { spread = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_spread(Eng, _src, value); }
        }

        /// <summary>Physical radius in metres (0 = point): the source holds its real-world size as the
        /// listener walks, where a fixed Spread would not.</summary>
        public float SizeMetres
        {
            get => sizeMetres;
            set { sizeMetres = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_size(Eng, _src, value); }
        }

        /// <summary>Voice-steal priority (0 = expendable .. 255 = protected). A full voice pool steals the
        /// lowest-priority source rather than failing the new one.</summary>
        public int Priority
        {
            get => priority;
            set { priority = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_priority(Eng, _src, value); }
        }

        /// <summary>Mix group (0..7) — drive the whole group with Engine.SetGroupGain/SetGroupPaused.</summary>
        public int Group
        {
            get => group;
            set { group = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_group(Eng, _src, (uint)value); }
        }

        /// <summary>Wet-send level into the shared reverb bed (needs `reflections`).</summary>
        public float ReflectionSend
        {
            get => reflectionSend;
            set { reflectionSend = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_reflection_send(Eng, _src, value); }
        }

        /// <summary>Drive occlusion from GAME LOGIC instead of the ray-traced sim — a door the gameplay
        /// knows about, underwater, muffled-by-menu. Works WITHOUT the Steam Audio build. `level` is
        /// broadband transmittance (1 = clear .. 0 = blocked); `bands` is an optional low/mid/high tilt in
        /// [0,1] rendered as the same transmission EQ (so it MUFFLES, not just attenuates) — pass null for
        /// broadband only. Everything ramps. Do NOT also tick `occlusion`: the sim republishes every tick
        /// and would overwrite this.</summary>
        public void SetOcclusionManual(float level, float[] bands = null)
        {
            if (occlusion) { Debug.LogWarning("[Emitter] SetOcclusionManual on a source with `occlusion` ticked — the sim overwrites it: " + name); return; }
            if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_occlusion_manual(Eng, _src, level, bands);
        }

        /// <summary>Fire a one-shot at this transform (transient voice; no handle held).</summary>
        public void PlayOneShot(string oneShotClip = null)
        {
            if (Eng == IntPtr.Zero) return;
            uint snd = Engine.Instance.Load(oneShotClip ?? clip);
            if (snd == 0) return;
            var p = Room.Pos(transform.position);
            Bwa.bwa_play_oneshot(Eng, snd, p.x, p.y, p.z, gain);
        }

        /// <summary>Current occlusion factor (1 = clear .. 0 = fully blocked) for a HUD/debug readout.</summary>
        public float Occlusion => (_created && Eng != IntPtr.Zero) ? Bwa.bwa_source_get_occlusion(Eng, _src) : 1f;

        // Inspector edits take effect live in Play mode. Without this, dragging Gain or Pitch during Play
        // changes the FIELD and nothing else — the value is only read once, at source creation — which
        // reads as "the slider is broken". Everything here is per-frame-safe and ramped engine-side.
        // (Position/orientation are pushed every frame anyway; the load-time-ish opt-ins are re-pushed
        // too, so ticking `doppler` mid-play does what you'd expect.)
        void OnValidate()
        {
            if (!Application.isPlaying || !_created || Eng == IntPtr.Zero) return;
            Bwa.bwa_source_set_gain(Eng, _src, gain);
            Bwa.bwa_source_set_pitch(Eng, _src, pitch);
            Bwa.bwa_source_set_priority(Eng, _src, priority);
            Bwa.bwa_source_set_group(Eng, _src, (uint)group);
            Bwa.bwa_source_set_spread(Eng, _src, spread);
            Bwa.bwa_source_set_size(Eng, _src, sizeMetres);
            Bwa.bwa_source_set_occlusion(Eng, _src, occlusion);
            Bwa.bwa_source_set_early_reflections(Eng, _src, earlyReflections);
            Bwa.bwa_source_set_reflections(Eng, _src, reflections);
            Bwa.bwa_source_set_reflection_send(Eng, _src, reflectionSend);
            Bwa.bwa_source_set_reflection_distance(Eng, _src, reflectionDistance);
            Bwa.bwa_source_set_pathing(Eng, _src, pathing);
            Bwa.bwa_source_set_doppler(Eng, _src, doppler);
            Bwa.bwa_source_set_air_absorption(Eng, _src, airAbsorption);
            Bwa.bwa_source_set_loudness_comp(Eng, _src, loudnessComp);
            Bwa.bwa_source_set_directivity_preset(Eng, _src, directivity);
            if (directivity != BwaDirectivity.Omni)
            {
                float w = directivity == BwaDirectivity.Cardioid ? 0.5f : 1.0f;
                Bwa.bwa_source_set_directivity(Eng, _src, w, directivityPower);
            }
        }

        void OnDisable()
        {
            _wasPlaying = false;                 // never carry a stale play edge into a re-enable
            StopAllCoroutines();                 // cancel a pending InitWhenReady
            if (_created && Eng != IntPtr.Zero)
            {
                if (Engine.Instance) Engine.Instance.Unregister(this);
                Bwa.bwa_source_destroy(Eng, _src);
            }
            _created = false;
        }
    }
}
