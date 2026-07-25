// SourceBase.cs — the source-generic half shared by Emitter (clip playback) and PushEmitter (PCM
// feed): the create / init-race-retry / destroy lifecycle, the per-frame transform push, live
// inspector re-push, and every bwa_source_* knob that applies to any voice. Mirrors the Godot
// binding's BwaSource base (bwa_source_base.h) — the same split, so a fix to the shared surface
// lands once instead of once per component. Subclasses mint their own kind of native source
// (CreateSource) and add only their feed: Emitter the clip/play surface, PushEmitter the PCM push.
using System;
using System.Collections;
using UnityEngine;

namespace BwAudio
{
    public abstract class SourceBase : MonoBehaviour
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
        [Tooltip("Near-field proximity boost: LF rises as the source closes inside ~1 m of the listener " +
                 "(up to +6 dB at the head), so 'at arm's length' reads as bass, not just level. " +
                 "Loudness comp's near mirror.")]
        public bool proximity = false;

        protected uint _src;
        protected bool _created;
        protected bool _paused;
        Vector2 _extent;
        float _attRef, _attRolloff, _attMin;   // SetAttenuationOverride mirror (standing state, like _extent)
        bool _attSet;
        Engine _owner;                         // the Engine this source was created under

        // Valid only while the CREATING Engine is still the live instance. A destroyed+recreated Engine
        // mints deterministically colliding native handles (a fresh engine's first source is slot 0,
        // gen 1 again), so a stale _src must never reach a successor engine: once the owner is gone this
        // reads Zero, every op (including OnDisable's destroy) no-ops, and the next enable re-creates
        // under the new engine.
        protected IntPtr Eng => _owner != null && ReferenceEquals(Engine.Instance, _owner) ? _owner.Handle : IntPtr.Zero;

        // Guard shorthand: the source exists and its engine is still the live one.
        protected bool Live => _created && Eng != IntPtr.Zero;

        /// <summary>Is this source still producing audio? (AudioSource.isPlaying equivalent. For a push
        /// voice: true from create until a drained PushEnd, Stop, or a finished FadeOut.)</summary>
        public bool IsPlaying => Live && Bwa.bwa_source_is_playing(Eng, _src);

        // ---- lifecycle -------------------------------------------------------------------------------

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

        /// <summary>Mint the native voice (bwa_source_create vs _create_push). 0 = failed.</summary>
        protected abstract uint CreateSource(IntPtr eng);

        /// <summary>Per-create playback-state reset, run before the settings push (even on a failed
        /// create) — a recycled component must not inherit a stale play edge.</summary>
        protected virtual void ResetPlaybackState() {}

        /// <summary>Subclass settings pushed alongside the shared ones (e.g. Emitter's pitch).</summary>
        protected virtual void ApplyExtraSettings() {}

        /// <summary>Runs once the fresh source is configured and positioned (e.g. play-on-enable).</summary>
        protected virtual void OnSourceReady() {}

        // Create the source + apply settings. Returns false (leaving _created false) if Engine isn't
        // ready yet, so the caller retries. Create CAN fail (0 = a full voice pool / command ring): we
        // log it and stop retrying (a fresh create won't help), and every op below then no-ops on the
        // invalid handle until a re-enable.
        protected bool TryInit()
        {
            if (_created) return true;
            var engine = Engine.Instance;
            if (!engine || engine.Handle == IntPtr.Zero) return false;   // Engine not ready -> caller retries
            _owner = engine;
            _src = CreateSource(engine.Handle);
            _created = true;
            _paused = false;
            ResetPlaybackState();
            if (_src == 0)
            {
                Debug.LogWarning("[" + GetType().Name + "] source create failed on " + name + ": " + Bwa.LastError(engine.Handle));
                return true;                                // don't retry a genuine failure forever
            }

            var eng = engine.Handle;
            Bwa.bwa_source_set_gain(eng, _src, gain);
            Bwa.bwa_source_set_priority(eng, _src, priority);
            if (group != 0) Bwa.bwa_source_set_group(eng, _src, (uint)group);
            ApplyExtraSettings();

            // The engine defaults every opt-in below to OFF/point/unity, so only push what differs — a
            // fresh source already IS the default (this runs again on every re-enable).
            if (occlusion)        Bwa.bwa_source_set_occlusion(eng, _src, true);
            if (earlyReflections) Bwa.bwa_source_set_early_reflections(eng, _src, true);
            if (reflections)
            {
                Bwa.bwa_source_set_reverb(eng, _src, true);
                if (reflectionSend != 1f)  Bwa.bwa_source_set_reverb_send(eng, _src, reflectionSend);
                if (reflectionDistance)    Bwa.bwa_source_set_reverb_distance(eng, _src, true);
            }
            if (pathing)       Bwa.bwa_source_set_pathing(eng, _src, true);
            if (spread > 0f)   Bwa.bwa_source_set_spread(eng, _src, spread);
            if (_extent.x > 0f || _extent.y > 0f) Bwa.bwa_source_set_extent(eng, _src, _extent.x, _extent.y);   // a script-set anisotropic extent, re-asserted after a re-enable
            if (sizeMetres > 0f) Bwa.bwa_source_set_size(eng, _src, sizeMetres);
            if (_attSet)       Bwa.bwa_source_set_attenuation_override(eng, _src, _attRef, _attRolloff, _attMin);   // standing state, replayed like _extent
            if (doppler)       Bwa.bwa_source_set_doppler(eng, _src, true);
            if (airAbsorption) Bwa.bwa_source_set_air_absorption(eng, _src, true);
            if (loudnessComp)  Bwa.bwa_source_set_loudness_comp(eng, _src, true);
            if (proximity)     Bwa.bwa_source_set_proximity(eng, _src, true);
            if (directivity != BwaDirectivity.Omni) ApplyDirectivity();   // fresh source defaults to omni, so skip the no-op
            SyncTransform();
            OnSourceReady();
            engine.Register(this);
            return true;
        }

        /// <summary>Push this transform (and, for a directional source, its orientation) to the engine —
        /// called once per frame by Engine, before the listener + the single commit.</summary>
        public void SyncTransform()
        {
            if (!Live) return;
            var p = Room.Pos(transform.position);
            Bwa.bwa_source_set_pos(Eng, _src, p.x, p.y, p.z);
            if (directivity != BwaDirectivity.Omni)
            {
                var q = Room.Rot(transform.rotation);            // the source's forward axis drives the lobe
                Bwa.bwa_source_set_orientation(Eng, _src, q.x, q.y, q.z, q.w);
            }
        }

        // Engine's per-frame entry point (Emitter layers the onFinished edge on top of the transform
        // push). Every source kind runs through Engine's one snapshot loop, so a handler that disables
        // or unregisters sources mid-loop is safe regardless of which kind fires it.
        internal virtual void FrameSync() => SyncTransform();

        protected virtual void OnDisable()
        {
            StopAllCoroutines();                 // cancel a pending InitWhenReady
            if (Live)                            // owner gone (engine destroyed/replaced) -> Live is false:
            {                                    // the stale handle never reaches a successor engine
                _owner.Unregister(this);
                Bwa.bwa_source_destroy(Eng, _src);
            }
            _created = false;
            _owner = null;
        }

        // ---- the source-generic control surface ------------------------------------------------------

        /// <summary>Stop this source — AudioSource.Stop equivalent, the click-free stop path (a push
        /// voice ends exactly like a drained PushEnd).</summary>
        public void Stop() { if (Live) Bwa.bwa_source_stop(Eng, _src); }

        /// <summary>Linear gain — AudioSource.volume equivalent; applies immediately if live. Cancels a
        /// running FadeTo/FadeOut (an explicit gain wins over a fade).</summary>
        public float Gain
        {
            get => gain;
            set { gain = value; if (Live) Bwa.bwa_source_set_gain(Eng, _src, value); }
        }

        /// <summary>Glide the gain to `target` over `seconds`, on the audio thread — no per-frame
        /// scripting, no coroutine. A later Gain-set or fade replaces it.</summary>
        public void FadeTo(float target, float seconds)
        {
            gain = target;   // keep the inspector field truthful about where the fade lands
            if (Live) Bwa.bwa_source_fade_to(Eng, _src, target, seconds);
        }

        /// <summary>Fade to silence over `seconds`, then STOP the voice (the click-free stop path) — the
        /// one-call "fade this out and clean it up". One-way for a push voice, like PushEnd.</summary>
        public void FadeOut(float seconds) { if (Live) Bwa.bwa_source_fade_out(Eng, _src, seconds); }

        /// <summary>Angular width (0 = point .. 1 = wide). Floored by SizeMetres when that is set. Setting
        /// this resets Extent to isotropic (spread and extent are the same knob — last call wins).</summary>
        public float Spread
        {
            get => spread;
            set { spread = value; _extent = Vector2.zero; if (Live) Bwa.bwa_source_set_spread(Eng, _src, value); }
        }

        /// <summary>Anisotropic angular extent (x = width, y = height, each 0 = point .. 1 = wide) — a
        /// shoreline is wide but not tall, rain tall but not wide. Equal values behave as the isotropic
        /// Spread; setting Spread resets this to isotropic (last call wins). Rides the spread mode, the
        /// size/near floors, and decorrelation. Scripting only — the inspector's single Spread slider is
        /// the isotropic knob.</summary>
        public Vector2 Extent
        {
            get => _extent;
            set { _extent = value; if (Live) Bwa.bwa_source_set_extent(Eng, _src, value.x, value.y); }
        }

        /// <summary>Override the layout's distance-attenuation curve for this source:
        /// <c>atten = clamp((refDist / max(d, refDist))^rolloff, minGain, 1)</c>. rolloff 0 = constant level
        /// at any distance (a direction-only cue that never fades); <c>refDist &lt;= 0</c> CLEARS the override
        /// (back to the layout curve). Composes with spread, dual-band, decorrelation, and loudness comp.
        /// Mono point sources only (a bed has no distance).</summary>
        public void SetAttenuationOverride(float refDist, float rolloff, float minGain)
        {
            // Mirrored (standing per-source state, like Extent — not a one-shot): a call that loses the
            // init race is replayed at create, and the override survives a disable/re-enable.
            _attSet = refDist > 0f;
            _attRef = refDist; _attRolloff = rolloff; _attMin = minGain;
            if (Live) Bwa.bwa_source_set_attenuation_override(Eng, _src, refDist, rolloff, minGain);
        }

        /// <summary>Physical radius in metres (0 = point): the source holds its real-world size as the
        /// listener walks, where a fixed Spread would not.</summary>
        public float SizeMetres
        {
            get => sizeMetres;
            set { sizeMetres = value; if (Live) Bwa.bwa_source_set_size(Eng, _src, value); }
        }

        /// <summary>Voice-steal priority (0 = expendable .. 255 = protected). A full voice pool steals the
        /// lowest-priority source rather than failing the new one.</summary>
        public int Priority
        {
            get => priority;
            set { priority = value; if (Live) Bwa.bwa_source_set_priority(Eng, _src, value); }
        }

        /// <summary>Mix group (0..7) — drive the whole group with Engine.SetGroupGain/SetGroupPaused.</summary>
        public int Group
        {
            get => group;
            set { group = value; if (Live) Bwa.bwa_source_set_group(Eng, _src, (uint)value); }
        }

        /// <summary>Wet-send level into the shared reverb bed (needs `reflections`).</summary>
        public float ReflectionSend
        {
            get => reflectionSend;
            set { reflectionSend = value; if (Live) Bwa.bwa_source_set_reverb_send(Eng, _src, value); }
        }

        /// <summary>Pause in place — AudioSource.Pause equivalent. Click-free (the engine ramps out over
        /// one block); a paused source still reads as IsPlaying. A push voice's ring keeps its data but
        /// stops draining — keep pacing pushes against PushSpace.</summary>
        public void Pause() { Paused = true; }

        /// <summary>Resume from exactly where Pause landed — AudioSource.UnPause equivalent.</summary>
        public void UnPause() { Paused = false; }

        /// <summary>Paused state (set-driven; Emitter.Play() always restarts un-paused).</summary>
        public bool Paused
        {
            get => _paused;
            set { _paused = value; if (Live) Bwa.bwa_source_set_paused(Eng, _src, value); }
        }

        /// <summary>Drive occlusion from GAME LOGIC instead of the ray-traced sim — a door the gameplay
        /// knows about, underwater, muffled-by-menu. Works WITHOUT the Steam Audio build. `level` is
        /// broadband transmittance (1 = clear .. 0 = blocked); `bands` is an optional low/mid/high tilt in
        /// [0,1] rendered as the same transmission EQ (so it MUFFLES, not just attenuates) — pass null for
        /// broadband only. Everything ramps. Do NOT also tick `occlusion`: the sim republishes every tick
        /// and would overwrite this.</summary>
        public void SetOcclusionManual(float level, float[] bands = null)
        {
            if (occlusion) { Debug.LogWarning("[" + GetType().Name + "] SetOcclusionManual on a source with `occlusion` ticked — the sim overwrites it: " + name); return; }
            if (Live) Bwa.bwa_source_set_occlusion_manual(Eng, _src, level, bands);
        }

        /// <summary>Current occlusion factor (1 = clear .. 0 = fully blocked) for a HUD/debug readout.</summary>
        public float Occlusion => Live ? Bwa.bwa_source_get_occlusion(Eng, _src) : 1f;

        /// <summary>Current playhead in engine-rate frames — AudioSource.timeSamples-get equivalent
        /// (latest-wins readback, ~one audio block of lag). Engine-owned truth where deriving it from
        /// DspTime breaks: it freezes while Paused, lands where Seek lands, follows Pitch at the
        /// actual rate, and counts frames actually consumed for streamed clips and push voices.
        /// (The CONTENT position — unrelated to the source's spatial transform.)</summary>
        public ulong Playhead => Live ? Bwa.bwa_source_get_playhead(Eng, _src) : 0;

        /// <summary>Playhead in seconds — AudioSource.time-get equivalent (Playhead over the engine
        /// sample rate).</summary>
        public double PlayheadSeconds => Engine.Instance ? Playhead / (double)Engine.Instance.sampleRate : 0.0;

        // Push this source's directivity pattern + sharpness. The preset call sets the pattern (and,
        // for Omni, turns directivity off); a non-omni pattern then re-issues with directivityPower, since
        // the preset only sets the weight — mapping the pattern to its cardioid/figure-8 weight. Shared by
        // TryInit and OnValidate; both call sites guard Live first.
        protected void ApplyDirectivity()
        {
            Bwa.bwa_source_set_directivity_preset(Eng, _src, directivity);
            if (directivity != BwaDirectivity.Omni)
            {
                float w = directivity == BwaDirectivity.Cardioid ? 0.5f : 1.0f;
                Bwa.bwa_source_set_directivity(Eng, _src, w, directivityPower);
            }
        }

        // Inspector edits take effect live in Play mode. Without this, dragging Gain during Play changes
        // the FIELD and nothing else — the value is only read once, at source creation — which reads as
        // "the slider is broken". Everything here is per-frame-safe and ramped engine-side.
        // (Position/orientation are pushed every frame anyway; the load-time-ish opt-ins are re-pushed
        // too, so ticking `doppler` mid-play does what you'd expect.)
        protected virtual void OnValidate()
        {
            if (!Application.isPlaying || !Live) return;
            var eng = Eng;
            Bwa.bwa_source_set_gain(eng, _src, gain);
            Bwa.bwa_source_set_priority(eng, _src, priority);
            Bwa.bwa_source_set_group(eng, _src, (uint)group);
            Bwa.bwa_source_set_spread(eng, _src, spread);
            // set_spread resets a script-set anisotropic extent (last call wins in the engine), so
            // re-assert it exactly like TryInit does — otherwise any inspector edit in Play mode
            // silently collapses the extent while the Extent getter keeps reporting it.
            if (_extent.x > 0f || _extent.y > 0f) Bwa.bwa_source_set_extent(eng, _src, _extent.x, _extent.y);
            Bwa.bwa_source_set_size(eng, _src, sizeMetres);
            Bwa.bwa_source_set_occlusion(eng, _src, occlusion);
            Bwa.bwa_source_set_early_reflections(eng, _src, earlyReflections);
            Bwa.bwa_source_set_reverb(eng, _src, reflections);
            Bwa.bwa_source_set_reverb_send(eng, _src, reflectionSend);
            Bwa.bwa_source_set_reverb_distance(eng, _src, reflectionDistance);
            Bwa.bwa_source_set_pathing(eng, _src, pathing);
            Bwa.bwa_source_set_doppler(eng, _src, doppler);
            Bwa.bwa_source_set_air_absorption(eng, _src, airAbsorption);
            Bwa.bwa_source_set_loudness_comp(eng, _src, loudnessComp);
            Bwa.bwa_source_set_proximity(eng, _src, proximity);
            ApplyDirectivity();   // always push the preset, so switching back to Omni disables it live
        }
    }
}
