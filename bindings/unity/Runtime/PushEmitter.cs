// PushEmitter.cs — a positional source you FEED PCM instead of a clip (procedural audio: a synthesised
// engine, a software synth, a network voice stream). The full spatial path applies — position, gain,
// spread, occlusion, reflections, Doppler, groups, fades all work exactly like Emitter — but the engine
// REFUSES play/seek/pitch/queue on a push voice, so those simply don't exist here rather than sitting in
// the inspector doing nothing (the same reason Godot splits BwaPushSource off from BwaEmitter).
//
// The voice consumes from create: silence until your first Push, and an underrun renders silence without
// losing your place (the data-driven clock slips, it never drops). Feed mono float frames at the engine
// sample rate (Engine.SampleRate), from Unity's main thread like every other call, a frame or so ahead —
// pace against PushSpace. PushEnd ends the voice once the ring drains (one-way: a push voice is not
// restartable — disabling and re-enabling this component creates a FRESH one). The ring is fixed at 65536
// frames (~1.37 s at 48 kHz); there is no capacity argument (bwa_source_create_push takes none).
//
// Like Emitter, this does NOT push its own commit — Engine pulls its transform once per frame (before the
// listener + the single commit), so the audio thread never sees a half-moved frame.
using System;
using System.Collections;
using UnityEngine;

namespace BwAudio
{
    public sealed class PushEmitter : MonoBehaviour
    {
        [Header("Source")]
        [Range(0f, 1f)] public float gain = 1f;

        [Header("Mixing")]
        [Tooltip("Voice-steal priority: when the voice pool is full, the LOWEST-priority source is stolen " +
                 "to make room. 255 = protected (music/critical SFX).")]
        [Range(0, 255)] public int priority = 128;
        [Tooltip("Mix group (0..7). Engine.SetGroupGain/SetGroupPaused duck or pause the whole group.")]
        [Range(0, 7)] public int group = 0;

        [Header("Spatial")]
        public bool occlusion = false;                       // geometry between source + listener attenuates it
        [Tooltip("Image-source EARLY reflections: the six first-order wall bounces, each panned like any " +
                 "other point source (correct direction AND parallax as the listener walks). Needs a room " +
                 "box (Engine). No Steam Audio needed. Pairs with the FDN reverb (the late tail).")]
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
        [Tooltip("Angular width: 0 = a point, 1 = wide. For a procedural crowd/waterfall/wind that " +
                 "shouldn't collapse to one speaker.")]
        [Range(0f, 1f)] public float spread = 0f;
        [Tooltip("Physical radius in METRES (0 = point). The width becomes the angle the radius subtends " +
                 "from the listener, so the source stays the same PHYSICAL size as the listener walks. " +
                 "Floors `spread` — the larger of the two wins.")]
        public float sizeMetres = 0f;

        [Header("Propagation (opt-in; derived from source↔listener distance)")]
        [Tooltip("Render through the propagation delay (distance/c): pitch up approaching, down receding. " +
                 "Best for fast movers; adds the real propagation latency. (A push voice ignores Pitch, " +
                 "but Doppler is a distance effect, not a resample rate — so it DOES apply.)")]
        public bool doppler = false;
        [Tooltip("Distance-driven HF low-pass — far sources sound duller.")]
        public bool airAbsorption = false;
        [Tooltip("LF shelf that tracks the distance attenuation, so a far source reads far, not THIN. " +
                 "A perceptual stylization — leave off for strict realism.")]
        public bool loudnessComp = false;

        uint _src;
        bool _created;
        bool _paused;
        IntPtr Eng => Engine.Instance ? Engine.Instance.Handle : IntPtr.Zero;

        // ---- the push feed (the reason this component exists) ----------------------------------------

        /// <summary>Feed `pcm` (mono float frames at Engine.SampleRate) to the voice. Returns the count
        /// ACCEPTED — less than pcm.Length when the ring is full, so pace against <see cref="PushSpace"/>
        /// rather than assuming it all landed. Non-finite samples become 0. Call from the main thread, a
        /// frame or so ahead. Refused (returns 0) after PushEnd/Stop/FadeOut — the voice is one-way.</summary>
        public int Push(float[] pcm) => Push(pcm, pcm != null ? pcm.Length : 0);

        /// <summary>Push only the first `count` frames of `pcm` — for a reusable buffer you fill partway
        /// each frame. (No offset overload: the marshalling pins the array from index 0, so a mid-array
        /// start would need a copy; slice into your own buffer if you need one.)</summary>
        public int Push(float[] pcm, int count)
        {
            if (!_created || Eng == IntPtr.Zero || pcm == null) return 0;
            if (count < 0) count = 0;
            if (count > pcm.Length) count = pcm.Length;
            return (int)Bwa.bwa_source_push(Eng, _src, pcm, (uint)count);
        }

        /// <summary>Frames of headroom in the ring right now (up to 65536) — push at most this many to
        /// avoid a full-ring rejection. 0 while not yet created.</summary>
        public int PushSpace => (_created && Eng != IntPtr.Zero) ? (int)Bwa.bwa_source_push_space(Eng, _src) : 0;

        /// <summary>Signal end of data: the voice ends once the ring drains, and further Push calls are
        /// refused. One-way — a push voice is not restartable (re-enable this component for a fresh one).
        /// Stop() and FadeOut() end it the same way; Paused just silences it.</summary>
        public void PushEnd() { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_push_end(Eng, _src); }

        /// <summary>Is the voice still producing audio? True from create until it ends (a drained PushEnd,
        /// Stop, or a finished FadeOut); a still-queued start counts as playing. False for a stale handle.</summary>
        public bool IsPlaying => _created && Eng != IntPtr.Zero && Bwa.bwa_source_is_playing(Eng, _src);

        // ---- lifecycle (mirrors Emitter) -------------------------------------------------------------

        void OnEnable()
        {
            if (!TryInit()) StartCoroutine(InitWhenReady());   // create now, or retry until Engine is ready
        }

        // Wait out an init-order race (this component enabled before Engine finished starting) instead of
        // permanently disabling. Unity stops the coroutine automatically when the component is disabled.
        IEnumerator InitWhenReady()
        {
            while (!_created) { yield return null; TryInit(); }
        }

        // Create the push voice + apply settings. Returns false (leaving _created false) if Engine isn't
        // ready yet, so the caller retries. bwa_source_create_push CAN fail (0 = a full voice pool /
        // command ring): we log it and stop retrying (a fresh create won't help), and every op below then
        // no-ops on the invalid handle.
        bool TryInit()
        {
            if (_created) return true;
            if (Eng == IntPtr.Zero) return false;          // Engine not ready -> caller retries
            _src = Bwa.bwa_source_create_push(Eng);
            _created = true;
            _paused = false;
            if (_src == 0)
            {
                Debug.LogWarning("[PushEmitter] bwa_source_create_push failed on " + name + ": " + Bwa.LastError(Eng));
                return true;                                // don't retry a genuine failure forever
            }

            Bwa.bwa_source_set_gain(Eng, _src, gain);
            Bwa.bwa_source_set_priority(Eng, _src, priority);
            if (group != 0) Bwa.bwa_source_set_group(Eng, _src, (uint)group);

            // The engine defaults every opt-in below to OFF/point/unity, so only push what differs — a
            // fresh source already IS the default (this runs again on every re-enable).
            if (occlusion)        Bwa.bwa_source_set_occlusion(Eng, _src, true);
            if (earlyReflections) Bwa.bwa_source_set_early_reflections(Eng, _src, true);
            if (reflections)
            {
                Bwa.bwa_source_set_reverb(Eng, _src, true);
                if (reflectionSend != 1f)  Bwa.bwa_source_set_reverb_send(Eng, _src, reflectionSend);
                if (reflectionDistance)    Bwa.bwa_source_set_reverb_distance(Eng, _src, true);
            }
            if (pathing)       Bwa.bwa_source_set_pathing(Eng, _src, true);
            if (spread > 0f)   Bwa.bwa_source_set_spread(Eng, _src, spread);
            if (_extent.x > 0f || _extent.y > 0f) Bwa.bwa_source_set_extent(Eng, _src, _extent.x, _extent.y);   // a script-set anisotropic extent, re-asserted after a re-enable
            if (sizeMetres > 0f) Bwa.bwa_source_set_size(Eng, _src, sizeMetres);
            if (doppler)       Bwa.bwa_source_set_doppler(Eng, _src, true);
            if (airAbsorption) Bwa.bwa_source_set_air_absorption(Eng, _src, true);
            if (loudnessComp)  Bwa.bwa_source_set_loudness_comp(Eng, _src, true);
            if (directivity != BwaDirectivity.Omni) ApplyDirectivity();   // fresh source defaults to omni, so skip the no-op
            SyncTransform();
            Engine.Instance.Register(this);
            return true;
        }

        /// <summary>Called once per frame by Engine (before the listener + commit) — the position-push
        /// analogue of Emitter.Push()/Godot's push_frame. Named apart from <see cref="Push(float[])"/> so
        /// the PCM feed keeps the plain "Push" verb.</summary>
        public void SyncTransform()
        {
            if (!_created || Eng == IntPtr.Zero) return;
            var p = Room.Pos(transform.position);
            Bwa.bwa_source_set_pos(Eng, _src, p.x, p.y, p.z);
            if (directivity != BwaDirectivity.Omni)
            {
                var q = Room.Rot(transform.rotation);            // the source's forward axis drives the lobe
                Bwa.bwa_source_set_orientation(Eng, _src, q.x, q.y, q.z, q.w);
            }
        }

        void OnDisable()
        {
            StopAllCoroutines();                 // cancel a pending InitWhenReady
            if (_created && Eng != IntPtr.Zero)
            {
                if (Engine.Instance) Engine.Instance.Unregister(this);
                Bwa.bwa_source_destroy(Eng, _src);   // releases the ring, safe while playing
            }
            _created = false;
        }

        // ---- controls (the source-generic subset that applies to a push voice) -----------------------

        /// <summary>Stop the voice now — the click-free stop path (ends it exactly like a drained PushEnd).</summary>
        public void Stop() { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_stop(Eng, _src); }

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

        /// <summary>Fade to silence over `seconds`, then END the voice (the click-free stop path). One-way,
        /// like PushEnd — for a fresh push voice, re-enable the component.</summary>
        public void FadeOut(float seconds) { if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_fade_out(Eng, _src, seconds); }

        /// <summary>Angular width (0 = point .. 1 = wide). Floored by SizeMetres when that is set. Setting
        /// this resets Extent to isotropic (spread and extent are the same knob — last call wins).</summary>
        public float Spread
        {
            get => spread;
            set { spread = value; _extent = Vector2.zero; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_spread(Eng, _src, value); }
        }

        /// <summary>Anisotropic angular extent (x = width, y = height, each 0 = point .. 1 = wide). Equal
        /// values behave as the isotropic Spread; setting Spread resets this to isotropic. Scripting only —
        /// the inspector's single Spread slider is the isotropic knob.</summary>
        public Vector2 Extent
        {
            get => _extent;
            set { _extent = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_extent(Eng, _src, value.x, value.y); }
        }
        Vector2 _extent;

        /// <summary>Physical radius in metres (0 = point): the source holds its real-world size as the
        /// listener walks, where a fixed Spread would not.</summary>
        public float SizeMetres
        {
            get => sizeMetres;
            set { sizeMetres = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_size(Eng, _src, value); }
        }

        /// <summary>Override the layout's distance-attenuation curve for this source:
        /// <c>atten = clamp((refDist / max(d, refDist))^rolloff, minGain, 1)</c>. rolloff 0 = constant level
        /// at any distance; <c>refDist &lt;= 0</c> CLEARS the override (back to the layout curve).</summary>
        public void SetAttenuationOverride(float refDist, float rolloff, float minGain)
        {
            if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_attenuation_override(Eng, _src, refDist, rolloff, minGain);
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
            set { reflectionSend = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_reverb_send(Eng, _src, value); }
        }

        /// <summary>Pause in place — the ring keeps its data, the voice just goes silent (set_paused, not an
        /// end). Click-free; a paused voice still reads as IsPlaying. Keep pacing your pushes against
        /// PushSpace: the consumer is frozen, so the ring stops draining.</summary>
        public void Pause() { Paused = true; }

        /// <summary>Resume from silence — the ring continues from exactly where it froze.</summary>
        public void UnPause() { Paused = false; }

        /// <summary>Paused state (set-driven).</summary>
        public bool Paused
        {
            get => _paused;
            set { _paused = value; if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_paused(Eng, _src, value); }
        }

        /// <summary>Drive occlusion from GAME LOGIC instead of the ray-traced sim (a door the gameplay
        /// knows about, muffled-by-menu). Works WITHOUT the Steam Audio build. `level` is broadband
        /// transmittance (1 = clear .. 0 = blocked); `bands` is an optional low/mid/high tilt in [0,1] (a
        /// wall MUFFLES, not just attenuates) — pass null for broadband only. Do NOT also tick
        /// `occlusion`: the sim republishes every tick and would overwrite this.</summary>
        public void SetOcclusionManual(float level, float[] bands = null)
        {
            if (occlusion) { Debug.LogWarning("[PushEmitter] SetOcclusionManual on a source with `occlusion` ticked — the sim overwrites it: " + name); return; }
            if (_created && Eng != IntPtr.Zero) Bwa.bwa_source_set_occlusion_manual(Eng, _src, level, bands);
        }

        /// <summary>Current occlusion factor (1 = clear .. 0 = fully blocked) for a HUD/debug readout.</summary>
        public float Occlusion => (_created && Eng != IntPtr.Zero) ? Bwa.bwa_source_get_occlusion(Eng, _src) : 1f;

        /// <summary>The voice's CONTENT playhead in engine-rate frames — for a push voice this counts the
        /// frames actually CONSUMED off the ring (an underrun slips it, like the audible clock). Freezes
        /// under pause; ~one audio block of lag; 0 before the first push. Unrelated to the spatial
        /// transform. seconds = Playhead / Engine.SampleRate.</summary>
        public ulong Playhead => _created && Eng != IntPtr.Zero ? Bwa.bwa_source_get_playhead(Eng, _src) : 0;

        /// <summary>Playhead in seconds (Playhead over the engine sample rate).</summary>
        public double PlayheadSeconds => Engine.Instance ? Playhead / (double)Engine.Instance.sampleRate : 0.0;

        // Push this source's directivity pattern + sharpness — shared by TryInit and OnValidate (both guard
        // _created/Eng first). Mirrors Emitter.ApplyDirectivity: the preset sets the pattern (Omni turns
        // directivity off), then a non-omni pattern re-issues with the cardioid/figure-8 weight + power.
        void ApplyDirectivity()
        {
            Bwa.bwa_source_set_directivity_preset(Eng, _src, directivity);
            if (directivity != BwaDirectivity.Omni)
            {
                float w = directivity == BwaDirectivity.Cardioid ? 0.5f : 1.0f;
                Bwa.bwa_source_set_directivity(Eng, _src, w, directivityPower);
            }
        }

        // Inspector edits take effect live in Play mode (same as Emitter). Play/seek/pitch/queue are
        // deliberately absent — the engine refuses them on a push voice.
        void OnValidate()
        {
            if (!Application.isPlaying || !_created || Eng == IntPtr.Zero) return;
            Bwa.bwa_source_set_gain(Eng, _src, gain);
            Bwa.bwa_source_set_priority(Eng, _src, priority);
            Bwa.bwa_source_set_group(Eng, _src, (uint)group);
            Bwa.bwa_source_set_spread(Eng, _src, spread);
            Bwa.bwa_source_set_size(Eng, _src, sizeMetres);
            Bwa.bwa_source_set_occlusion(Eng, _src, occlusion);
            Bwa.bwa_source_set_early_reflections(Eng, _src, earlyReflections);
            Bwa.bwa_source_set_reverb(Eng, _src, reflections);
            Bwa.bwa_source_set_reverb_send(Eng, _src, reflectionSend);
            Bwa.bwa_source_set_reverb_distance(Eng, _src, reflectionDistance);
            Bwa.bwa_source_set_pathing(Eng, _src, pathing);
            Bwa.bwa_source_set_doppler(Eng, _src, doppler);
            Bwa.bwa_source_set_air_absorption(Eng, _src, airAbsorption);
            Bwa.bwa_source_set_loudness_comp(Eng, _src, loudnessComp);
            ApplyDirectivity();   // always push the preset, so switching back to Omni disables it live
        }
    }
}
