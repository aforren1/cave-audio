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
	void seek(int64_t frame);

	bool is_playing() const override;

protected:
	static void _bind_methods();
	void on_source_ready() override;

private:
	bool begin(const String &p, bwa_sound *out);

	String clip;
	bool loop = true;
	bool autoplay = true;
	bool streaming = false; /* long file: stream from disk instead of decoding into RAM */
	float pitch = 1.0f;

	/* Edge-detecting the end needs three states, not two. bwa_source_play only ENQUEUES;
	 * bwa_source_is_playing is a per-block republish, so for the frame or two before the
	 * audio thread picks the command up the voice honestly reads "not playing". A plain
	 * was-playing/is-playing flag therefore fires `finished` immediately after every play.
	 * PENDING absorbs that window: only a voice we have actually SEEN playing can end. */
	enum State { IDLE, PENDING, PLAYING };
	State state = IDLE;
	int pending_frames = 0;
	/* ...and the window cannot be open-ended, because the ABI documents is_playing as
	 * best-effort: "a sound shorter than the caller's poll interval may never be observed
	 * as playing". Past this grace we take the readback at its word, so a very short clip
	 * still reports finished instead of hanging in PENDING forever. */
	static constexpr int PENDING_GRACE = 4;
};

} // namespace godot
