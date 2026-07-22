#include "bwa_emitter_node.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

void BwaEmitter::on_source_ready() {
	if (pitch != 1.0f) {
		bwa_source_set_pitch(ENG, src, pitch);
	}
	if (autoplay) {
		play();
	}
}

void BwaEmitter::push_frame() {
	BwaSource::push_frame();
	if (!LIVE) {
		return;
	}

	const bool now = bwa_source_is_playing(ENG, src);
	switch (state) {
		case PENDING:
			if (now) {
				state = PLAYING; // the audio thread picked the play up; the end is detectable now
			} else if (++pending_frames > PENDING_GRACE) {
				state = IDLE; // clip shorter than a frame — it came and went unobserved
				emit_signal("finished");
			}
			break;
		case PLAYING:
			if (!now) {
				state = IDLE;
				emit_signal("finished");
			}
			break;
		case IDLE:
			break;
	}
}

/* Resolve a clip to a loaded handle and arm the end detector. False = nothing to play
 * (load_sound has already reported why). */
bool BwaEmitter::begin(const String &p, bwa_sound *out) {
	if (!LIVE) {
		return false;
	}
	const bwa_sound snd = owner->load_sound(p, streaming);
	if (!snd) {
		return false;
	}
	*out = snd;
	state = PENDING;
	pending_frames = 0;
	return true;
}

void BwaEmitter::play() { play_clip(clip); }

void BwaEmitter::play_clip(const String &p) {
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_source_play(ENG, src, snd, loop);
	}
}

void BwaEmitter::play_at(const String &p, int64_t start_sample) {
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_source_play_at(ENG, src, snd, loop, (uint64_t)start_sample);
	}
}

void BwaEmitter::play_loop(const String &p, int64_t loop_beg, int64_t loop_end) {
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_source_play_loop(ENG, src, snd, (uint64_t)loop_beg, (uint64_t)loop_end);
	}
}

void BwaEmitter::stop_at(int64_t stop_sample) {
	if (LIVE) {
		bwa_source_stop_at(ENG, src, (uint64_t)stop_sample);
	}
}

void BwaEmitter::queue(const String &p, bool queue_loop) {
	if (!LIVE) {
		return;
	}
	/* Queued items are in-memory mono only — a streamed handle is rejected by the core, so
	 * never hand it one just because this emitter happens to be a streaming emitter. */
	const bwa_sound snd = owner->load_sound(p, false);
	if (snd) {
		bwa_source_queue(ENG, src, snd, queue_loop);
	}
}

void BwaEmitter::clear_queue() {
	if (LIVE) {
		bwa_source_clear_queue(ENG, src);
	}
}

void BwaEmitter::seek(int64_t frame) {
	if (LIVE) {
		bwa_source_seek(ENG, src, (uint64_t)frame);
	}
}

void BwaEmitter::stop() {
	state = IDLE; // an explicit stop is not an end; see the header
	BwaSource::stop();
}

void BwaEmitter::fade_out(float seconds) {
	state = IDLE; // ends on the stop path once silent — same rule as stop()
	BwaSource::fade_out(seconds);
}

void BwaEmitter::set_pitch(float rate) {
	pitch = rate;
	if (LIVE) {
		bwa_source_set_pitch(ENG, src, rate);
	}
}

/* A just-issued play counts as playing. The raw readback would say false until the audio
 * thread consumes the command, which makes the obvious `play(); if is_playing():` read
 * wrong for a frame — the same reason the core itself counts a still-queued play as
 * playing on push sources. */
bool BwaEmitter::is_playing() const {
	return state == PENDING || BwaSource::is_playing();
}

void BwaEmitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_clip", "path"), &BwaEmitter::set_clip);
	ClassDB::bind_method(D_METHOD("get_clip"), &BwaEmitter::get_clip);
	ClassDB::bind_method(D_METHOD("set_loop", "enabled"), &BwaEmitter::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &BwaEmitter::get_loop);
	ClassDB::bind_method(D_METHOD("set_autoplay", "enabled"), &BwaEmitter::set_autoplay);
	ClassDB::bind_method(D_METHOD("get_autoplay"), &BwaEmitter::get_autoplay);
	ClassDB::bind_method(D_METHOD("set_streaming", "enabled"), &BwaEmitter::set_streaming);
	ClassDB::bind_method(D_METHOD("get_streaming"), &BwaEmitter::get_streaming);
	ClassDB::bind_method(D_METHOD("set_pitch", "rate"), &BwaEmitter::set_pitch);
	ClassDB::bind_method(D_METHOD("get_pitch"), &BwaEmitter::get_pitch);

	ClassDB::bind_method(D_METHOD("play"), &BwaEmitter::play);
	ClassDB::bind_method(D_METHOD("play_clip", "path"), &BwaEmitter::play_clip);
	ClassDB::bind_method(D_METHOD("play_at", "path", "start_sample"), &BwaEmitter::play_at);
	ClassDB::bind_method(
			D_METHOD("play_loop", "path", "loop_beg", "loop_end"), &BwaEmitter::play_loop);
	ClassDB::bind_method(D_METHOD("stop_at", "stop_sample"), &BwaEmitter::stop_at);
	ClassDB::bind_method(D_METHOD("queue", "path", "loop"), &BwaEmitter::queue);
	ClassDB::bind_method(D_METHOD("clear_queue"), &BwaEmitter::clear_queue);
	ClassDB::bind_method(D_METHOD("seek", "frame"), &BwaEmitter::seek);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "clip", PROPERTY_HINT_FILE, "*.wav,*.flac,*.mp3"),
			"set_clip", "get_clip");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autoplay"), "set_autoplay", "get_autoplay");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming", "get_streaming");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch", PROPERTY_HINT_RANGE, "0.25,4,0.01"),
			"set_pitch", "get_pitch");

	ADD_SIGNAL(MethodInfo("finished"));
}
