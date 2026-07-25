// PushEmitter.cs — a positional source you FEED PCM instead of a clip (procedural audio: a synthesised
// engine, a software synth, a network voice stream). The full spatial path applies — position, gain,
// spread, occlusion, reflections, Doppler, groups, fades all work exactly like Emitter (they live on
// the shared SourceBase) — but the engine REFUSES play/seek/pitch/queue on a push voice, so those
// simply don't exist here rather than sitting in the inspector doing nothing (the same reason Godot
// splits BwaPushSource off from BwaEmitter — and, like Godot, both derive from one source base).
//
// The voice consumes from create: silence until your first Push, and an underrun renders silence without
// losing your place (the data-driven clock slips, it never drops). Feed mono float frames at the engine
// sample rate (Engine.SampleRate), from Unity's main thread like every other call, a frame or so ahead —
// pace against PushSpace. PushEnd ends the voice once the ring drains (one-way: a push voice is not
// restartable — disabling and re-enabling this component creates a FRESH one). The ring is fixed at 65536
// frames (~1.37 s at 48 kHz); there is no capacity argument (bwa_source_create_push takes none). While
// Paused the consumer freezes, so the ring stops draining — keep pacing pushes against PushSpace.
//
// Like Emitter, this does NOT push its own commit — Engine pulls its transform once per frame (before the
// listener + the single commit), so the audio thread never sees a half-moved frame.
using System;
using UnityEngine;

namespace BwAudio
{
    public sealed class PushEmitter : SourceBase
    {
        // ---- SourceBase hook: mint a push voice instead of a clip voice ------------------------------

        protected override uint CreateSource(IntPtr eng) => Bwa.bwa_source_create_push(eng);

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
            if (!Live || pcm == null) return 0;
            if (count < 0) count = 0;
            if (count > pcm.Length) count = pcm.Length;
            return (int)Bwa.bwa_source_push(Eng, _src, pcm, (uint)count);
        }

        /// <summary>Frames of headroom in the ring right now (up to 65536) — push at most this many to
        /// avoid a full-ring rejection. 0 while not yet created.</summary>
        public int PushSpace => Live ? (int)Bwa.bwa_source_push_space(Eng, _src) : 0;

        /// <summary>Signal end of data: the voice ends once the ring drains, and further Push calls are
        /// refused. One-way — a push voice is not restartable (re-enable this component for a fresh one).
        /// Stop() and FadeOut() end it the same way; Paused just silences it.</summary>
        public void PushEnd() { if (Live) Bwa.bwa_source_push_end(Eng, _src); }
    }
}
