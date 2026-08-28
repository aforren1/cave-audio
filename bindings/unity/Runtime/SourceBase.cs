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
        [Tooltip("Physical radius in METERS (0 = point). The width becomes the angle the radius subtends " +
                 "from the listener, so the source stays the same PHYSICAL size as the listener walks. " +
                 "Floors `spread` — the larger of the two wins.")]
        public float sizeMeters = 0f;

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
        int _channel = Bwa.CHANNEL_AUTO;       // direct output-channel route (standing state; see Channel)
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

            // ONE bwa_source_apply for the whole field set instead of the fifteen-or-so setters this used
            // to issue. The engine packs the audio-thread knobs into a SINGLE ring command, which is what
            // makes this worth doing: bwa_play_oneshot already documents dropping when the ring is
            // momentarily full, so spawning a prefab that issued fifteen commands was real pressure. The
            // standing script-set state (Extent, SetAttenuationOverride) rides along, so it is replayed on
            // a re-enable exactly as before.
            ApplyDesc();
            // Standing script-set state that is not a desc field, replayed like Extent so a re-enable
            // does not silently put a reference source back on the panner mid-experiment.
            if (_channel != Bwa.CHANNEL_AUTO) Bwa.bwa_source_set_channel(engine.Handle, _src, _channel);
            ApplyExtraSettings();
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

        // Engine's per-frame entry point, BEFORE the commit (Emitter layers its async-load watch on top
        // of the transform push). Every source kind runs through Engine's one snapshot loop, so a
        // handler that disables or unregisters sources mid-loop is safe regardless of which kind fires it.
        internal virtual void FrameSync() => SyncTransform();

        // Engine's second per-frame entry point, AFTER the commit and after the bwa_poll_ended drain.
        // Completion work belongs here and not in FrameSync: the ended events are filled by the same
        // pass bwa_commit runs, so anything that reasons about "did this end" must read the engine
        // after the commit, or it reasons about the previous frame.
        internal virtual void PostCommit() {}

        // One handle bwa_poll_ended reported as ENDED, routed here by Engine (the single owner of that
        // drain). Base does nothing: only Emitter has a completion event.
        internal virtual void NotifyEnded() {}

        // One handle bwa_poll_looped reported as WRAPPED, routed here by Engine (the single owner of that
        // drain too). A wrap is not an end — the voice keeps playing — so nothing here touches the
        // completion state. Base does nothing: only Emitter has a loop event.
        internal virtual void NotifyLooped() {}

        // The native source handle, for Engine's handle -> component route. 0 when the create failed.
        // Deliberately NOT gated on _created: OnDisable clears that flag, and Unregister still has to
        // find the map entry to remove it.
        internal uint NativeHandle => _src;

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
        public virtual void Stop() { if (Live) Bwa.bwa_source_stop(Eng, _src); }

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

        /// <summary>Angular width (0 = point .. 1 = wide). Floored by SizeMeters when that is set. Setting
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

        /// <summary>Physical radius in meters (0 = point): the source holds its real-world size as the
        /// listener walks, where a fixed Spread would not.</summary>
        public float SizeMeters
        {
            get => sizeMeters;
            set { sizeMeters = value; if (Live) Bwa.bwa_source_set_size(Eng, _src, value); }
        }

        /// <summary>Distance-driven HF low-pass (far sources sound duller) — the `airAbsorption` field
        /// as a live property, pushed like Spread/SizeMeters; setting the field alone would change
        /// nothing until the next re-enable.</summary>
        public bool AirAbsorption
        {
            get => airAbsorption;
            set { airAbsorption = value; if (Live) Bwa.bwa_source_set_air_absorption(Eng, _src, value); }
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

        /// <summary>Send this source out of exactly ONE output channel, with no spatial processing —
        /// the psychophysics ground-truth condition (one real speaker, A/B'd against a phantom) and a
        /// wiring check you can run with real content. Valid range is 0 to
        /// <c>Engine.ChannelCount - 1</c>; <see cref="Bwa.CHANNEL_AUTO"/> (-1) restores normal panning,
        /// and an out-of-range value is refused with a warning, leaving the source on the channel it
        /// had - so this property never reports a route the voice is not on.
        /// <para>Unlike a test signal, the routed voice keeps the whole output stage a panned voice gets
        /// (align trims and delays, room EQ, master gain, limiter) and RAMPS in and out, so it is
        /// level-comparable with the phantom. Everything distance- or direction-derived is suppressed
        /// while it is on (attenuation, spread, occlusion, reverb sends, Doppler, air absorption) and
        /// comes back the moment you go to CHANNEL_AUTO. Pitch and Paused still apply. Mono point
        /// sources only. No unit suffix: a channel is a bare index, not a quantity.</para>
        /// <para>Script-only, deliberately: this is a run-time experimental condition, not authored
        /// configuration, so it is not serialized in the scene. It IS replayed across a re-enable.</para>
        /// </summary>
        public int Channel
        {
            get => _channel;
            // Range-check HERE, and keep the old route on a refusal, so the getter can never report a
            // channel the voice is not on: the engine refuses out of range too, but into Bwa.LastError,
            // where nothing reading this property would see it. That is the whole failure mode — a
            // reference source that quietly stays panned while the caller reads back a speaker index.
            // Godot's BwaSource.set_channel refuses identically, on purpose: the two bindings must not
            // disagree about what a route means.
            //
            // The two ends are knowable at different times, hence two checks. A negative is bad with NO
            // channel count (CHANNEL_AUTO is the one negative that means anything), so it is refused
            // even before this source is live. A too-large one needs the count, so it waits for Live
            // and TryInit replays the cached value for the engine to judge when it can.
            set
            {
                if (value != Bwa.CHANNEL_AUTO && value < 0)
                {
                    Debug.LogWarning("[" + GetType().Name + "] Channel " + value + " on " + name +
                                     " refused: the only negative that means anything is " +
                                     "Bwa.CHANNEL_AUTO (" + Bwa.CHANNEL_AUTO + "), which restores " +
                                     "spatial panning. The source stays on channel " + _channel + ".");
                    return;
                }
                if (Live && Engine.Instance && value >= (int)Engine.Instance.ChannelCount)
                {
                    Debug.LogWarning("[" + GetType().Name + "] Channel " + value + " on " + name +
                                     " is out of range (0 to " + (Engine.Instance.ChannelCount - 1) +
                                     ", or Bwa.CHANNEL_AUTO). The source stays on channel " +
                                     _channel + ".");
                    return;
                }
                _channel = value;
                if (Live) Bwa.bwa_source_set_channel(Eng, _src, value);
            }
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
        /// DspTimeFrames breaks: it freezes while Paused, lands where SeekFrames lands, follows Pitch at the
        /// actual rate, and counts frames actually consumed for streamed clips and push voices.
        /// (The CONTENT position — unrelated to the source's spatial transform.)</summary>
        public ulong PlayheadFrames => Live ? Bwa.bwa_source_get_playhead_frames(Eng, _src) : 0;

        /// <summary>PlayheadFrames in seconds — AudioSource.time-get equivalent (PlayheadFrames over the engine
        /// sample rate).</summary>
        public double PlayheadSeconds => Engine.Instance ? PlayheadFrames / (double)Engine.Instance.sampleRate : 0.0;

        // ---- bulk configuration (bwa_source_desc) ----------------------------------------------------
        // The per-property setters above stay the live, incremental path. This is the WHOLE field set in
        // one call, which is what the create-time push uses and what a preset needs. The inspector fields
        // remain the source of truth in both directions: BuildDesc reads them, ApplyPreset writes them.

        /// <summary>This component's whole configuration as a BwaSourceDesc, starting from the engine's
        /// own defaults so every field is set (never build one from default(BwaSourceDesc) — its zero is
        /// not its default). Position, orientation and playback state are deliberately not in it.</summary>
        public BwaSourceDesc BuildDesc()
        {
            var d = Bwa.SourcePreset(BwaSourceKind.Default);
            WriteDesc(ref d);
            return d;
        }

        /// <summary>Push this component's whole field set to the engine in ONE call. Safe any time (it
        /// no-ops before the source exists, and the create-time push replays the fields anyway). False
        /// means the source isn't live, or the engine refused the desc (a NaN in a field).</summary>
        public bool ApplyDesc() => ApplyDesc(BuildDesc());

        /// <summary>Push an explicit desc — read one back with <see cref="TryGetDesc"/>, edit it, hand it
        /// back. This does NOT update the inspector fields, so a later per-property edit or re-enable
        /// pushes the fields again; use <see cref="ApplyPreset"/> when you want both to move.</summary>
        public bool ApplyDesc(in BwaSourceDesc d)
        {
            if (!Live) return false;
            if (Bwa.bwa_source_apply(Eng, _src, in d)) return true;
            Debug.LogWarning("[" + GetType().Name + "] source apply refused on " + name + ": " + Bwa.LastError(Eng));
            return false;
        }

        /// <summary>Read back what the ENGINE has this source configured at (what you set, not what the
        /// sim is currently doing — that is <see cref="Occlusion"/>). False for a source that isn't
        /// live.</summary>
        public bool TryGetDesc(out BwaSourceDesc d)
        {
            if (Live) return Bwa.bwa_source_get_desc(Eng, _src, out d);
            d = default;
            return false;
        }

        /// <summary>Configure this source as a KIND: fill the engine's preset for it, mirror it into the
        /// inspector fields, and push the lot in one call. This is also the source RESET the API had no
        /// way to express before — <c>ApplyPreset(BwaSourceKind.Default)</c>.
        /// <para>Nothing in the preset table is measured. A kind differs from Default only where a doc
        /// already argues the case (docs/api.md, "What each preset rests on"), so treat it as a starting
        /// point and edit what you disagree with.</para></summary>
        public void ApplyPreset(BwaSourceKind kind)
        {
            var d = Bwa.SourcePreset(kind);
            ReadDesc(in d);      // keep the inspector truthful about what the engine is running
            ApplyDesc(in d);
        }

        /// <summary>Fill `d` from this component's fields. Override to add a subclass's own knobs (Emitter
        /// adds pitch); call base first.</summary>
        protected virtual void WriteDesc(ref BwaSourceDesc d)
        {
            d.gain     = gain;
            d.priority = priority;
            d.group    = (uint)group;
            // A script-set anisotropic extent wins over the isotropic slider, which is the engine's own
            // last-call-wins rule between set_spread and set_extent (and the reason TryInit used to
            // re-assert the extent right after the spread).
            if (_extent.x > 0f || _extent.y > 0f) { d.spread = _extent.x; d.extentHeight = _extent.y; }
            else                                  { d.spread = spread;    d.extentHeight = -1f; }
            d.sizeMeters = sizeMeters;
            d.reverbSend = reflectionSend;
            if (_attSet) { d.attenRefDist = _attRef; d.attenRolloff = _attRolloff; d.attenMinGain = _attMin; }
            // Same mapping bwa_source_set_directivity_preset uses: omni = weight 0 (off, power 1),
            // cardioid = 0.5, figure-8 = 1, with the component's sharpness on the non-omni patterns.
            d.directivityWeight = directivity == BwaDirectivity.Omni     ? 0f
                                : directivity == BwaDirectivity.Cardioid ? 0.5f : 1f;
            d.directivityPower  = directivity == BwaDirectivity.Omni ? 1f : directivityPower;
            d.doppler          = doppler;
            d.airAbsorption    = airAbsorption;
            d.loudnessComp     = loudnessComp;
            d.proximity        = proximity;
            d.occlusion        = occlusion;
            d.earlyReflections = earlyReflections;
            d.reverb           = reflections;
            d.reverbDistance   = reflectionDistance;
            d.pathing          = pathing;
        }

        /// <summary>Write `d` into this component's fields (the inverse of <see cref="WriteDesc"/>).
        /// Override alongside it; call base first.</summary>
        protected virtual void ReadDesc(in BwaSourceDesc d)
        {
            gain     = d.gain;
            priority = d.priority;
            group    = (int)d.group;
            spread   = d.spread;
            _extent  = d.extentHeight >= 0f ? new Vector2(d.spread, d.extentHeight) : Vector2.zero;
            sizeMeters     = d.sizeMeters;
            reflectionSend = d.reverbSend;
            _attSet   = d.attenRefDist > 0f;
            _attRef   = d.attenRefDist; _attRolloff = d.attenRolloff; _attMin = d.attenMinGain;
            directivity = d.directivityWeight <= 0f    ? BwaDirectivity.Omni
                        : d.directivityWeight >= 0.99f ? BwaDirectivity.Figure8 : BwaDirectivity.Cardioid;
            if (directivity != BwaDirectivity.Omni) directivityPower = d.directivityPower;
            doppler            = d.doppler;
            airAbsorption      = d.airAbsorption;
            loudnessComp       = d.loudnessComp;
            proximity          = d.proximity;
            occlusion          = d.occlusion;
            earlyReflections   = d.earlyReflections;
            reflections        = d.reverb;
            reflectionDistance = d.reverbDistance;
            pathing            = d.pathing;
        }

        // Unity's editor Reset (the component's context menu, and adding it to a GameObject). Unity has
        // already restored the field initializers by the time this runs; re-filling from the engine's own
        // BWA_SRC_DEFAULT keeps the two in step if a default ever moves. bwa_source_preset is PURE, so it
        // needs no running engine — but it IS a P/Invoke, and a project that has not staged bw_audio.dll
        // yet would throw here, where the field initializers are already the right answer.
        protected virtual void Reset()
        {
            try { ApplyPreset(BwaSourceKind.Default); }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
        }

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
            Bwa.bwa_source_set_size(eng, _src, sizeMeters);
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
