#include "bwa_emitter_node.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"
#include "bwa_guard.h"

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
 * is not an end, so drop the detector rather than let the silence read as one. `end_fired`
 * arms the one-shot suppression: a voice that was already stopping and whose clip runs out in
 * the SAME audio block still posts a real completion event (rt.c's mix seam), and that event
 * must not turn a scene change into a `finished`. */
void BwaEmitter::on_stopped_externally() {
	state = IDLE;
	pending_snd = 0;
	pending_async = false;
	end_fired = true;
}

/* The AUTHORITATIVE end: BwaEngine drained this handle out of bwa_poll_ended after the commit.
 * Nothing is guessed here - the core posted the completion for this exact play of this exact
 * generation - so a clip shorter than a frame reports correctly, which is what the old
 * grace-window guess could never do. */
void BwaEmitter::notify_ended() {
	/* The completion belongs to the play that ENDED, and that is never an async play still HELD: a
	 * held play has not bound, so it cannot have ended. Tearing the pending fields down for it would
	 * strand the play the caller is still waiting on - is_loading() would read false while it
	 * decodes, the decode-failed report could never fire, and post_commit's IDLE case ignores `now`,
	 * so the machine would never reach PLAYING and a later stop_at would emit no `finished`. */
	if (!pending_async) {
		state = IDLE;
		pending_snd = 0;
	}
	if (end_fired) { /* the halt path already accounted for this end - see the header */
		end_fired = false;
		return;
	}
	emit_signal("finished");
}

/* A loop WRAP. Deliberately no state change and no `end_fired` interaction: the voice is still
 * playing, and both feeds of `finished` are about a voice going quiet. One signal per wrap, so a
 * loop region shorter than a frame emits several times in one frame. */
void BwaEmitter::notify_looped() {
	emit_signal("looped");
}

void BwaEmitter::post_commit() {
	if (!LIVE) {
		return;
	}

	const bool now = bwa_source_is_playing(ENG, src);
	switch (state) {
		case PENDING:
			if (pending_async) {
				/* The HELD case is asked FIRST, because a held play has not bound and `now` therefore
				 * says nothing about it: the core does not stop the previous play to wait, so a true
				 * reading here is the PREVIOUS voice still running. Taking that as "the play landed"
				 * dropped the watch while the decode was still in flight, which made is_loading() lie
				 * and left a failure with nothing to report it. Ask the decode instead, and spend no
				 * grace on it: the window reopens, from zero, on the frame the data lands. */
				const int ready = owner->sound_ready_state(pending_snd);
				if (ready < 0) {
					UtilityFunctions::push_error(
							vformat("BwaEmitter (%s): the async load of \"%s\" failed: %s",
									get_name(), pending_path, owner->get_last_error()));
					state = IDLE;
					pending_async = false;
					pending_snd = 0;
				} else if (ready > 0) {
					/* The pump point that answered "ready" is the one that ADOPTED the decode and fired
					 * the held play, so this is the frame the play bound - and the only place a held
					 * play may void the previous end's duplicate suppression. See begin(). */
					pending_async = false;
					pending_frames = 0;
					end_fired = false;
				}
			} else if (now) {
				state = PLAYING; // the audio thread picked the play up; the halt edge is armed now
			} else if (++pending_frames > PENDING_GRACE) {
				/* Never observed playing and no completion event arrived either: a dropped play or
				 * a voice stolen at onset. Stop CLAIMING it plays, but emit nothing - see the
				 * header. A real sub-frame clip lands in notify_ended long before this. */
				state = IDLE;
			}
			break;
		case PLAYING:
			/* The explicit-HALT fallback, and only that: a natural end has already run through
			 * notify_ended above (the drain precedes this pass), which left state IDLE, so the
			 * silence it left behind is not an edge any more. What still reaches here is stop_at,
			 * which is documented to fire `finished`, and any other halt this node was not told
			 * about. `end_fired` keeps a straggling completion event from doubling it. */
			if (!now) {
				state = IDLE;
				end_fired = true;
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
	/* A streamed clip is never deferred (the ABI loads it synchronously here), and a cache HIT is
	 * already resident, so only ask about the ones that can actually still be decoding. Tri-state,
	 * because the question every line below turns on is "did this play BIND", and only `ready > 0`
	 * answers yes: 0 (still decoding) and -1 (the decode already failed) both mean the core is
	 * HOLDING the play, control-side, and it never reached rt.c's source_bind. Reading -1 as "not
	 * held" is also what used to let a failure that landed before this probe fall through to the
	 * PENDING_GRACE timeout, which drops the play silently and reports nothing. */
	const int ready = (async_load && !streaming) ? owner->sound_ready_state(snd) : 1;
	pending_async = ready <= 0;
	if (!pending_async) {
		/* Only a play that BINDS may void the previous end's duplicate suppression. A held play
		 * never reaches source_bind, so the core never bumps play_seq - and play_seq is the gate
		 * that would otherwise have dropped a completion straggling in from the previous play.
		 * Clear the latch for a held play and that straggler passes both gates and fires
		 * `finished` for a voice this node has already reported, or (after stop()) was told never
		 * to report at all. The held case clears it in post_commit instead, on the frame the core
		 * binds it; a decode that FAILS never binds, so the latch stays armed and eats the
		 * straggler, which is what should happen to it. */
		end_fired = false;
	}
	return true;
}

/* Asks the HANDLE, not the cached flag: post_commit is what clears `pending_async`, and a
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
	if (!bwa_guard_nonneg(this, "play_at", "start_sample", start_sample)) {
		return;
	}
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_source_play_at(ENG, src, snd, loop, (uint64_t)start_sample);
	}
}

void BwaEmitter::play_loop(const String &p, int64_t loop_beg, int64_t loop_end) {
	if (!bwa_guard_nonneg(this, "play_loop", "loop_beg", loop_beg) ||
			!bwa_guard_nonneg(this, "play_loop", "loop_end", loop_end)) {
		return;
	}
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_source_play_loop(ENG, src, snd, (uint64_t)loop_beg, (uint64_t)loop_end);
	}
}

void BwaEmitter::stop_at(int64_t stop_sample) {
	if (!bwa_guard_nonneg(this, "stop_at", "stop_sample", stop_sample)) {
		return;
	}
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
	if (!bwa_guard_nonneg(this, "seek_frames", "frame", frame)) {
		return;
	}
	if (LIVE) {
		bwa_source_seek(ENG, src, (uint64_t)frame);
	}
}

void BwaEmitter::seek_seconds(double seconds) {
	if (!bwa_guard_nonneg(this, "seek_seconds", "seconds", seconds) || !LIVE) {
		return;
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		seek_frames((int64_t)(seconds * (double)rate));
	}
}

void BwaEmitter::set_region_frames(int64_t start_frame, int64_t end_frame) {
	if (!bwa_guard_nonneg(this, "set_region_frames", "start_frame", start_frame) ||
			!bwa_guard_nonneg(this, "set_region_frames", "end_frame", end_frame) || !LIVE) {
		return;
	}
	bwa_source_set_region(ENG, src, (uint64_t)start_frame, (uint64_t)end_frame);
}

void BwaEmitter::set_region_seconds(double start_seconds, double end_seconds) {
	if (!bwa_guard_nonneg(this, "set_region_seconds", "start_seconds", start_seconds) ||
			!bwa_guard_nonneg(this, "set_region_seconds", "end_seconds", end_seconds) || !LIVE) {
		return;
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		/* 0 seconds means the clip end here too, and 0 * rate is 0, so the sentinel survives the
		 * conversion without a special case. */
		set_region_frames((int64_t)(start_seconds * (double)rate),
				(int64_t)(end_seconds * (double)rate));
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
	ClassDB::bind_method(D_METHOD("set_region_frames", "start_frame", "end_frame"),
			&BwaEmitter::set_region_frames);
	ClassDB::bind_method(D_METHOD("set_region_seconds", "start_seconds", "end_seconds"),
			&BwaEmitter::set_region_seconds);

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
	/* One per loop WRAP. A looping voice never finishes, so `finished` reports it never; this is
	 * what paces a trial or cues a visual off the loop. */
	ADD_SIGNAL(MethodInfo("looped"));
}
