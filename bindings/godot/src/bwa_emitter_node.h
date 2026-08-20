/* BwaEmitter — a spatial source that plays a file. Everything spatial lives in BwaSource;
 * this adds the transport: play, scheduling, loop regions, gapless queueing, pitch, seek.
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

	void push_frame() override;

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
	/* Sample-accurate start against the engine's dsp clock: BwaEngine.get_dsp_time() + n. */
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
	void seek_frames(int64_t frame);
	void seek_seconds(double seconds);

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

	/* Edge-detecting the end needs three states, not two. bwa_source_play only ENQUEUES;
	 * bwa_source_is_playing is a per-block republish, so for the frame or two before the
	 * audio thread picks the command up the voice honestly reads "not playing". A plain
	 * was-playing/is-playing flag therefore fires `finished` immediately after every play.
	 * PENDING absorbs that window: only a voice we have actually SEEN playing can end.
	 *
	 * play_at needs no special case: the core sets the voice playing at the play command and
	 * the scheduled start only holds its OUTPUT (rt.c: start_sample gates rendering, while
	 * the play_pub publish keys on v->playing) — so a scheduled voice reads as playing for
	 * the whole hold and PENDING resolves immediately, however far out the start is. */
	enum State { IDLE, PENDING, PLAYING };
	State state = IDLE;
	int pending_frames = 0;
	/* The async play's handle, and whether the play took that route at all. An async play is
	 * HELD on the control thread until the data lands, so the voice honestly reads "not
	 * playing" for as long as the decode takes - which the grace window below would spend and
	 * then announce a `finished` for a clip that has not started. The grace only runs once the
	 * handle is READY, so a slow disk cannot manufacture an end. The flag keeps the readiness
	 * probe off the synchronous path, where it would clear bwa_last_error every frame for an
	 * answer that is always true. */
	bwa_sound pending_snd = 0;
	bool pending_async = false;
	String pending_path; /* what begin() was handed, for the failure message */
	/* ...and the window cannot be open-ended, because the ABI documents is_playing as
	 * best-effort: "a sound shorter than the caller's poll interval may never be observed
	 * as playing". Past this grace we take the readback at its word, so a very short clip
	 * still reports finished instead of hanging in PENDING forever. */
	static constexpr int PENDING_GRACE = 4;
};

} // namespace godot
