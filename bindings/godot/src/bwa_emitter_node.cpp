#include "bwa_emitter_node.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

/* No pitch push here: it is a bwa_source_desc field, so it rode in with the create desc. */
void BwaEmitter::on_source_ready() {
	if (autoplay) {
		play();
	}
}

void BwaEmitter::fill_desc(bwa_source_desc *d) const {
	BwaSource::fill_desc(d);
	d->pitch = pitch;
}

void BwaEmitter::mirror_desc(const bwa_source_desc &d) {
	BwaSource::mirror_desc(d);
	pitch = d.pitch;
}

/* BwaEngine::group_stop / stop_all stopped this voice. Same rule as stop(): an explicit halt
 * is not an end, so drop the detector rather than let the silence read as one. */
void BwaEmitter::on_stopped_externally() {
	state = IDLE;
	pending_snd = 0;
	pending_async = false;
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
				pending_async = false;
			} else if (pending_async) {
				/* An async play is HELD control-side, so "not playing" here means "not loaded
				 * yet", never "already over". Spend no grace on it: the window reopens, from
				 * zero, on the frame the data lands. */
				const int ready = owner->sound_ready_state(pending_snd);
				if (ready < 0) {
					UtilityFunctions::push_error(
							vformat("BwaEmitter (%s): the async load of \"%s\" failed: %s",
									get_name(), pending_path, owner->get_last_error()));
					state = IDLE;
					pending_async = false;
					pending_snd = 0;
				} else if (ready > 0) {
					pending_async = false;
					pending_frames = 0;
				}
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
	const bwa_sound snd = owner->load_sound(p, streaming, async_load);
	if (!snd) {
		return false;
	}
	*out = snd;
	state = PENDING;
	pending_frames = 0;
	pending_snd = snd;
	pending_path = p;
	/* A streamed clip is never deferred (the ABI loads it synchronously here), and a cache HIT
	 * is already resident, so only ask about the ones that can actually still be decoding. */
	pending_async = async_load && !streaming && owner->sound_ready_state(snd) == 0;
	return true;
}

/* Asks the HANDLE, not the cached flag: push_frame is what clears `pending_async`, and a
 * caller may look between two frames (or on the manual sink, where blocks advance without
 * one). The flag only keeps the probe off the synchronous path, where it is always true. */
bool BwaEmitter::is_loading() const {
	return LIVE && state == PENDING && pending_async && owner->sound_ready_state(pending_snd) == 0;
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

void BwaEmitter::seek_frames(int64_t frame) {
	if (LIVE) {
		bwa_source_seek(ENG, src, (uint64_t)frame);
	}
}

void BwaEmitter::seek_seconds(double seconds) {
	if (!LIVE || seconds < 0.0) {
		return;
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		seek_frames((int64_t)(seconds * (double)rate));
	}
}

void BwaEmitter::stop() {
	/* An explicit stop is not an end (see the header). It also cancels a play still waiting on
	 * an async decode: the core drops that hold on the same call, so the detector must too. */
	on_stopped_externally();
	BwaSource::stop();
}

void BwaEmitter::fade_out(float seconds) {
	on_stopped_externally(); // ends on the stop path once silent — same rule as stop()
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
	ClassDB::bind_method(D_METHOD("set_async_load", "enabled"), &BwaEmitter::set_async_load);
	ClassDB::bind_method(D_METHOD("get_async_load"), &BwaEmitter::get_async_load);
	ClassDB::bind_method(D_METHOD("is_loading"), &BwaEmitter::is_loading);
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
	ClassDB::bind_method(D_METHOD("seek_frames", "frame"), &BwaEmitter::seek_frames);
	ClassDB::bind_method(D_METHOD("seek_seconds", "seconds"), &BwaEmitter::seek_seconds);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "clip", PROPERTY_HINT_FILE, "*.wav,*.flac,*.mp3"),
			"set_clip", "get_clip");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autoplay"), "set_autoplay", "get_autoplay");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming", "get_streaming");
	/* Off by default on purpose: the load-time path stays synchronous, and this is the
	 * mid-session case. A play against a still-decoding clip binds the voice and starts it
	 * from the top when the data lands - is_loading() covers that window. */
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "async_load"), "set_async_load", "get_async_load");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch", PROPERTY_HINT_RANGE, "0.25,4,0.01"),
			"set_pitch", "get_pitch");

	ADD_SIGNAL(MethodInfo("finished"));
}
