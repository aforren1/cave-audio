/* BwaEmitter — a spatial source that plays a file. Everything spatial lives in BwaSource;
 * this adds the transport: play, scheduling, loop regions, gapless queueing, pitch, seek, the
 * play region, and the `finished` / `looped` signals.
 */
#pragma once

#include <godot_cpp/variant/string.hpp>

#include "bwa_source_base.h"

namespace godot {

class BwaEmitter : public BwaSource {
	GDCLASS(BwaEmitter, BwaSource)

public:
	BwaEmitter() = default;
	~BwaEmitter() override = default;

	void post_commit() override;
	void notify_ended() override;
	void notify_looped() override;

	void set_clip(const String &p) { clip = p; }
	String get_clip() const { return clip; }
	void set_loop(bool v) { loop = v; }
	bool get_loop() const { return loop; }
	void set_autoplay(bool v) { autoplay = v; }
	bool get_autoplay() const { return autoplay; }
	void set_streaming(bool v) { streaming = v; }
	bool get_streaming() const { return streaming; }
	/* Opt-in async load (bwa_sound_acquire_async): play returns at once and the voice stays
	 * SILENT until the decode lands, then starts from the top of the clip. For content that
	 * appears mid-session; the load-time path should stay synchronous, so this is OFF by
	 * default. Ignored by queue(), which the core refuses to resolve against a not-ready
	 * handle, and a streamed clip loads synchronously either way (the ABI says so: a stream
	 * open is cheap and already decodes off this thread). */
	void set_async_load(bool v) { async_load = v; }
	bool get_async_load() const { return async_load; }
	/* True between an async play and its data landing. The voice is bound and silent. */
	bool is_loading() const;
	void set_pitch(float rate);
	float get_pitch() const { return pitch; }

	void play();
	void play_clip(const String &p);
	/* Sample-accurate start against the engine's dsp clock: BwaEngine.get_dsp_time_frames() + n. */
	void play_at(const String &p, int64_t start_sample);
	/* Intro then looping body inside one file: [0, loop_beg) plays once, [loop_beg, loop_end)
	 * repeats. 0/0 loops the whole clip. */
	void play_loop(const String &p, int64_t loop_beg, int64_t loop_end);
	void stop_at(int64_t stop_sample);
	/* Gapless chaining. Queue AFTER play — a fresh play clears the queue. */
	void queue(const String &p, bool queue_loop);
	void clear_queue();
	/* Named for the unit, like get_output_latency_*: Godot's AudioStreamPlayer3D.seek() takes
	 * SECONDS as a float, so a bare seek() here taking frames is a silent unit trap — a caller
	 * passing 1.5 lands on frame 1 and the clip simply starts from the top. */
	/* Every time-valued argument below is refused, with a warning, if it is NEGATIVE. They all reach
	 * the ABI unsigned, so a negative becomes an enormous positive instead of an error: -1 frames is
	 * 1.8e19, which schedules for never and looks exactly like nothing happening. See bwa_guard.h. */
	void seek_frames(int64_t frame);
	void seek_seconds(double seconds);
	/* Bound playback to [start, end) of the clip; end 0 means the clip end. A looping clip wraps
	 * back to `start` (and emits `looped`); a one-shot ENDS at `end` exactly as it would at the
	 * clip end (and emits `finished`). So a loop region and a truncated one-shot are one call.
	 *
	 * Call it AFTER a play: the bounds resolve against the bound clip, and any play resets the
	 * region. It does not move the playhead, so a region set mid-play takes effect at the next
	 * boundary. In-memory clips only; a streamed clip ignores it. An `end` at or below `start`
	 * (and not 0) is refused.
	 *
	 * Named for the unit for the same reason seek_frames is: the seconds spelling differs from the
	 * frames one by a factor of the sample rate, and a bare set_region() would read as seconds to
	 * anyone arriving from AudioStreamPlayer3D. */
	void set_region_frames(int64_t start_frame, int64_t end_frame);
	void set_region_seconds(double start_seconds, double end_seconds);

	/* An explicit halt is not an end: `finished` means the sound RAN OUT (a non-loop end, a
	 * drained queue) — never that stop()/fade_out() was called. stop_at() deliberately DOES
	 * fire it: a scheduled stop is an arranged ending, and the caller wants to know when it
	 * landed. */
	void stop() override;
	void fade_out(float seconds) override;

	bool is_playing() const override;

protected:
	static void _bind_methods();
	void on_source_ready() override;
	void on_stopped_externally() override;
	/* `pitch` is a desc field, and this is the only source class that owns one. */
	void fill_desc(bwa_source_desc *d) const override;
	void mirror_desc(const bwa_source_desc &d) override;

private:
	bool begin(const String &p, bwa_sound *out);

	String clip;
	bool loop = true;
	bool autoplay = true;
	bool streaming = false; /* long file: stream from disk instead of decoding into RAM */
	bool async_load = false; /* opt-in: decode on the loader thread, play when it lands */
	float pitch = 1.0f;

	/* `finished` has TWO feeds, because the core reports only one of the two ways a voice goes
	 * quiet.
	 *
	 * A voice that RAN OUT (a non-loop clip finished, a queue drained, a play region's end) posts
	 * a completion EVENT the block it happens. BwaEngine drains that with bwa_poll_ended and calls
	 * notify_ended() here. THAT is the authoritative feed, and it is the reason this class no
	 * longer guesses: the old three-state machine edge-detected bwa_source_is_playing and, when a
	 * clip never once read as playing, assumed after a few frames that it "came and went
	 * unobserved" and emitted `finished`. A guess cannot tell a sub-frame clip that really finished
	 * from a play the command ring dropped or a voice stolen at onset, so it could announce the end
	 * of a sound that never played. The event knows the difference.
	 *
	 * An explicit HALT posts NO event at all (rt.c: stop / stop_at / fade_out / group_stop /
	 * stop_all all take the click-free stop path, which finalizes in pause_gate with playing=false
	 * and no completion; a STEAL posts only the ownership ack, which the drain deliberately does
	 * not report). stop_at() is documented to fire `finished` - a scheduled stop is an arranged
	 * ending - so the is-playing EDGE survives as the narrow fallback that covers exactly that.
	 *
	 * The three states are still needed for the play WINDOW, not for the end: bwa_source_play only
	 * ENQUEUES, and bwa_source_is_playing is a per-block republish, so for a frame or two after a
	 * play the voice honestly reads "not playing". A two-state flag would read that as an end.
	 * PENDING absorbs the window; only a voice actually SEEN playing can take the fallback.
	 *
	 * play_at needs no special case: the core sets the voice playing at the play command and the
	 * scheduled start only holds its OUTPUT (rt.c: start_sample gates rendering, while the play_pub
	 * publish keys on v->playing) - so a scheduled voice reads as playing for the whole hold and
	 * PENDING resolves immediately, however far out the start is. */
	enum State { IDLE, PENDING, PLAYING };
	State state = IDLE;
	int pending_frames = 0;
	/* ONE end can still reach both feeds. rt.c's mix seam takes `ended = true` for a voice that is
	 * already `stopping` and whose clip runs out in the same block (queue_pop_valid is skipped, not
	 * the end), so a halt can be followed by a genuine completion event for the same play. This
	 * latch makes whichever feed reports first the only one that does. It is cleared by the next
	 * play that BINDS, so a swallowed straggler can never eat a LATER end - including the sub-frame
	 * clip the event feed exists to catch. A play still HELD on an async decode has NOT bound, so it
	 * does not clear it: the core only bumps play_seq at source_bind, so until then the previous
	 * play's completion still passes the drain's gate and this latch is the only thing standing in
	 * its way. on_stopped_externally() arms it too: `finished` must not fire for a scene change, and
	 * that interleaving is exactly how it could. */
	bool end_fired = false;
	/* The async play's handle, and whether the core is HOLDING that play. A held play did not
	 * bind, so the voice honestly reads "not playing" until the decode lands. Both non-binding
	 * answers set the flag - still decoding, and a decode that already failed - because
	 * everything that turns on it (the duplicate-end latch, the failure report, is_loading) is
	 * really asking "has this play bound yet", and a failed one never will. It stays off on the
	 * synchronous path, where the readiness probe would clear bwa_last_error every frame for an
	 * answer that is always true. */
	bwa_sound pending_snd = 0;
	bool pending_async = false;
	String pending_path; /* what begin() was handed, for the failure message */
	/* A play can be DROPPED (a full command ring) or its voice STOLEN at onset, and neither posts
	 * a completion. Such a voice would sit in PENDING forever and is_playing() would keep claiming
	 * it plays. Past this many frames the readback is taken at its word and the claim is dropped -
	 * silently. It emits nothing: not knowing whether a sound ended is not the same as knowing it
	 * did, and the event feed is what answers that. A genuinely sub-frame clip's completion event
	 * arrives within a frame or two, well inside this window. */
	static constexpr int PENDING_GRACE = 4;
};

} // namespace godot
