#include "bwa_bed_node.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"
#include "bwa_guard.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && bed && owner->is_running())

void BwaBed::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	owner = BwaEngine::get_singleton();
	if (!owner || !owner->is_running()) {
		UtilityFunctions::push_warning(vformat(
				"BwaBed (%s): no running BwaEngine in the scene - this bed is silent.", get_name()));
		owner = nullptr;
		return;
	}

	bed = bwa_bed_create(ENG);
	if (!bed) {
		UtilityFunctions::push_error(vformat(
				"BwaBed (%s): bwa_bed_create failed: %s", get_name(), owner->get_last_error()));
		owner = nullptr;
		return;
	}

	bwa_bed_set_gain(ENG, bed, gain);
	bwa_bed_set_priority(ENG, bed, priority);
	bwa_bed_set_group(ENG, bed, (uint32_t)group);
	if (paused) {
		bwa_bed_set_paused(ENG, bed, true);
	}
	if (orientation != Vector3()) {
		bwa_bed_set_orientation(
				ENG, bed, (float)orientation.x, (float)orientation.y, (float)orientation.z);
		pushed_orientation = orientation;
	}

	owner->register_client(this); // for the engine-freed-first teardown order
	owner->register_bed(this);   // ...and for the completion / loop event route
	set_process(true);
	if (autoplay) {
		play();
	}
}

/* The orientation setter can be called from anywhere (an animation, a tween); pushing it
 * from _process instead of the setter keeps it to one call per frame however often it was
 * written, and only when it actually changed. */
void BwaBed::_process(double delta) {
	(void)delta;
	if (LIVE && orientation != pushed_orientation) {
		bwa_bed_set_orientation(
				ENG, bed, (float)orientation.x, (float)orientation.y, (float)orientation.z);
		pushed_orientation = orientation;
	}
}

void BwaBed::_exit_tree() {
	if (!owner) {
		return;
	}
	owner->unregister_client(this); // the child-exits-first order: drop us from its lists
	owner->unregister_bed(this);
	if (bed && owner->is_running()) {
		bwa_bed_destroy(ENG, bed);
	}
	bed = 0;
	owner = nullptr;
}

void BwaBed::engine_gone() {
	/* No calls into the engine — it is mid-teardown. Just forget it. */
	owner = nullptr;
	bed = 0;
}

/* Resolve a clip to a loaded soundfield handle and arm the event state. False = nothing to
 * play (load_ambisonic has already reported why). Shared by all three play forms so they
 * cannot drift on which one clears the duplicate-end latch. */
bool BwaBed::begin(const String &p, bwa_sound *out) {
	if (!LIVE) {
		return false;
	}
	const bwa_sound snd = owner->load_ambisonic(p, format == FORMAT_FUMA, async_load);
	if (!snd) {
		return false;
	}
	*out = snd;
	playing_snd = snd;
	pending_path = p;
	state = PENDING;
	pending_frames = 0;
	/* A cache HIT is already resident, so only ask about a handle that can still be decoding.
	 * Tri-state, because the question the latch below turns on is "did this play BIND", and only
	 * `ready > 0` answers yes: 0 (still decoding) and -1 (the decode already failed) both mean the
	 * core is HOLDING the play, control-side, and it never reached rt.c's source_bind. Reading -1 as
	 * "not held" is also what used to let a failure that landed before this probe fall through to the
	 * PENDING_GRACE timeout, which drops the play silently and reports nothing. */
	const int ready = async_load ? owner->sound_ready_state(snd) : 1;
	pending_async = ready <= 0;
	if (!pending_async) {
		/* Only a play that BINDS may void the previous end's duplicate suppression. A held play
		 * never reaches source_bind, so the core never bumps play_seq - and play_seq is the gate
		 * that would otherwise have dropped a completion straggling in from the previous play.
		 * Clear the latch for a held play and that straggler passes both gates and fires
		 * `finished` for a bed this node has already reported, or (after stop()) was told never to
		 * report at all. The held case clears it in post_commit instead, on the frame the core
		 * binds it. */
		end_fired = false;
	}
	return true;
}

void BwaBed::play() { play_clip(clip); }

void BwaBed::play_clip(const String &p) {
	bwa_sound snd;
	if (begin(p, &snd)) {
		/* A not-ready handle is fine here: the core holds the play control-side and starts the
		 * field from the top on the block its data lands. It also skips the mono guard for a
		 * still-loading asset, which reports 0 channels because nothing is decoded yet. */
		bwa_bed_play(ENG, bed, snd, loop);
	}
}

void BwaBed::play_at(const String &p, int64_t start_sample) {
	if (!bwa_guard_nonneg(this, "play_at", "start_sample", start_sample)) {
		return;
	}
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_bed_play_at(ENG, bed, snd, loop, (uint64_t)start_sample);
	}
}

void BwaBed::play_loop(const String &p, int64_t loop_beg, int64_t loop_end) {
	if (!bwa_guard_nonneg(this, "play_loop", "loop_beg", loop_beg) ||
			!bwa_guard_nonneg(this, "play_loop", "loop_end", loop_end)) {
		return;
	}
	bwa_sound snd;
	if (begin(p, &snd)) {
		bwa_bed_play_loop(ENG, bed, snd, (uint64_t)loop_beg, (uint64_t)loop_end);
	}
}

void BwaBed::stop_at(int64_t stop_sample) {
	/* Deliberately NOT on_stopped_externally(): a scheduled stop is an arranged ending and
	 * fires `finished`. It takes the same click-free path as stop(), which posts no completion
	 * event, so the is-playing edge in post_commit() is what reports it. */
	if (!bwa_guard_nonneg(this, "stop_at", "stop_sample", stop_sample)) {
		return;
	}
	if (LIVE) {
		bwa_bed_stop_at(ENG, bed, (uint64_t)stop_sample);
	}
}

void BwaBed::set_region_frames(int64_t start_frame, int64_t end_frame) {
	if (!bwa_guard_nonneg(this, "set_region_frames", "start_frame", start_frame) ||
			!bwa_guard_nonneg(this, "set_region_frames", "end_frame", end_frame) || !LIVE) {
		return;
	}
	bwa_bed_set_region(ENG, bed, (uint64_t)start_frame, (uint64_t)end_frame);
}

void BwaBed::set_region_seconds(double start_seconds, double end_seconds) {
	if (!bwa_guard_nonneg(this, "set_region_seconds", "start_seconds", start_seconds) ||
			!bwa_guard_nonneg(this, "set_region_seconds", "end_seconds", end_seconds) || !LIVE) {
		return;
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		/* 0 seconds means the asset end here too, and 0 * rate is 0, so the sentinel survives
		 * the conversion without a special case. */
		set_region_frames((int64_t)(start_seconds * (double)rate),
				(int64_t)(end_seconds * (double)rate));
	}
}

/* The AUTHORITATIVE end: BwaEngine drained this handle out of bwa_poll_ended after the commit.
 * Nothing is guessed - the core posted the completion for this exact play of this exact
 * generation - so a soundfield shorter than a frame reports correctly. */
void BwaBed::notify_ended() {
	/* The completion belongs to the play that ENDED, and that is never an async play still HELD: a
	 * held play has not bound, so it cannot have ended. Tearing the pending fields down for it would
	 * strand the play the caller is still waiting on - is_loading() would read false while it
	 * decodes, the decode-failed report could never fire, and post_commit's IDLE case ignores `now`,
	 * so the machine would never reach PLAYING and a later stop_at would emit no `finished`. */
	if (!pending_async) {
		state = IDLE;
		playing_snd = 0;
	}
	if (end_fired) { /* the halt path already accounted for this end - see the header */
		end_fired = false;
		return;
	}
	emit_signal("finished");
}

/* A loop WRAP. No state change and no `end_fired` interaction: the bed is still playing, and
 * both feeds of `finished` are about a voice going quiet. One signal per wrap, so a loop
 * region shorter than a frame emits several times in one frame. */
void BwaBed::notify_looped() { emit_signal("looped"); }

void BwaBed::post_commit() {
	if (!LIVE) {
		return;
	}
	const bool now = bwa_bed_is_playing(ENG, bed);
	switch (state) {
		case PENDING:
			if (pending_async) {
				/* The HELD case is asked FIRST, because a held play has not bound and `now` therefore
				 * says nothing about it: the core does not stop the previous play to wait, so a true
				 * reading here is the PREVIOUS soundfield still running. Taking that as "the play
				 * landed" dropped the watch while the decode was still in flight, which made
				 * is_loading() lie and left a failure with nothing to report it. Ask the decode
				 * instead, and spend no grace on it: the window reopens, from zero, on the frame the
				 * data lands. */
				const int ready = owner->sound_ready_state(playing_snd);
				if (ready < 0) {
					UtilityFunctions::push_error(
							vformat("BwaBed (%s): the async load of \"%s\" failed: %s", get_name(),
									pending_path, owner->get_last_error()));
					state = IDLE;
					pending_async = false;
					playing_snd = 0;
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
				/* Never observed playing and no completion event arrived either: a dropped play
				 * or a voice stolen at onset. Stop CLAIMING it, but emit nothing - a soundfield
				 * shorter than a frame lands in notify_ended long before this. */
				state = IDLE;
			}
			break;
		case PLAYING:
			/* The explicit-HALT fallback, and only that: a natural end has already run through
			 * notify_ended above (the drain precedes this pass), which left state IDLE, so the
			 * silence it left behind is not an edge any more. What still reaches here is
			 * stop_at. `end_fired` keeps a straggling completion event from doubling it. */
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

void BwaBed::on_stopped_externally() {
	state = IDLE;
	playing_snd = 0;
	pending_async = false;
	end_fired = true;
}

bool BwaBed::is_loading() const {
	return LIVE && playing_snd && owner->sound_ready_state(playing_snd) == 0;
}

void BwaBed::stop() {
	/* An explicit stop is not an end (see the header). It also cancels a play still waiting on
	 * an async decode: the core drops that hold on the same call, so the detector must too. */
	on_stopped_externally();
	if (LIVE) {
		bwa_bed_stop(ENG, bed);
	}
}

void BwaBed::set_gain(float g) {
	gain = g;
	if (LIVE) {
		bwa_bed_set_gain(ENG, bed, g);
	}
}

void BwaBed::set_priority(int p) {
	priority = p;
	if (LIVE) {
		bwa_bed_set_priority(ENG, bed, p);
	}
}

void BwaBed::set_group(int g) {
	group = g;
	if (LIVE) {
		bwa_bed_set_group(ENG, bed, (uint32_t)g);
	}
}

void BwaBed::set_paused(bool p) {
	paused = p;
	if (LIVE) {
		bwa_bed_set_paused(ENG, bed, p);
	}
}

void BwaBed::set_orientation(const Vector3 &ypr) { orientation = ypr; }

void BwaBed::set_yaw_from_basis(const Basis &b) {
	if (owner) {
		orientation = Vector3(owner->to_room_yaw(b), 0.0f, 0.0f);
	}
}

void BwaBed::fade_to(float target, float seconds) {
	gain = target;
	if (LIVE) {
		bwa_bed_fade_to(ENG, bed, target, seconds);
	}
}

void BwaBed::fade_out(float seconds) {
	on_stopped_externally(); // ends on the stop path once silent - same rule as stop()
	if (LIVE) {
		bwa_bed_fade_out(ENG, bed, seconds);
	}
}

void BwaBed::seek_frames(int64_t frame) {
	if (!bwa_guard_nonneg(this, "seek_frames", "frame", frame)) {
		return;   /* same refusal as BwaEmitter::seek_frames: an unguarded -1 reaches the ABI as 1.8e19 */
	}
	if (LIVE) {
		bwa_bed_seek(ENG, bed, (uint64_t)frame);
	}
}

void BwaBed::seek_seconds(double seconds) {
	if (!bwa_guard_nonneg(this, "seek_seconds", "seconds", seconds) || !LIVE) {
		return;   /* warns rather than returning in silence, like every other transport refusal */
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		seek_frames((int64_t)(seconds * (double)rate));
	}
}

/* A just-issued play counts as playing, exactly as BwaEmitter::is_playing does and for the same
 * reason: bwa_bed_play only ENQUEUES, so the raw readback says false until the audio thread
 * consumes the command and the obvious `play(); if is_playing():` reads wrong for a frame. The two
 * classes must not disagree about what "playing" means, and a play the core is still HOLDING on an
 * async decode is outstanding in exactly the same sense. */
bool BwaBed::is_playing() const { return state == PENDING || (LIVE && bwa_bed_is_playing(ENG, bed)); }

int64_t BwaBed::get_playhead_frames() const {
	return LIVE ? (int64_t)bwa_bed_get_playhead_frames(ENG, bed) : 0;
}

double BwaBed::get_playhead_seconds() const {
	if (!LIVE) {
		return 0.0;
	}
	const int rate = owner->get_resolved_sample_rate();
	return rate > 0 ? (double)bwa_bed_get_playhead_frames(ENG, bed) / (double)rate : 0.0;
}

void BwaBed::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_clip", "path"), &BwaBed::set_clip);
	ClassDB::bind_method(D_METHOD("get_clip"), &BwaBed::get_clip);
	ClassDB::bind_method(D_METHOD("set_format", "format"), &BwaBed::set_format);
	ClassDB::bind_method(D_METHOD("get_format"), &BwaBed::get_format);
	ClassDB::bind_method(D_METHOD("set_loop", "enabled"), &BwaBed::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &BwaBed::get_loop);
	ClassDB::bind_method(D_METHOD("set_autoplay", "enabled"), &BwaBed::set_autoplay);
	ClassDB::bind_method(D_METHOD("get_autoplay"), &BwaBed::get_autoplay);
	ClassDB::bind_method(D_METHOD("set_async_load", "enabled"), &BwaBed::set_async_load);
	ClassDB::bind_method(D_METHOD("get_async_load"), &BwaBed::get_async_load);
	ClassDB::bind_method(D_METHOD("is_loading"), &BwaBed::is_loading);
	ClassDB::bind_method(D_METHOD("set_gain", "linear"), &BwaBed::set_gain);
	ClassDB::bind_method(D_METHOD("get_gain"), &BwaBed::get_gain);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &BwaBed::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &BwaBed::get_priority);
	ClassDB::bind_method(D_METHOD("set_group", "group"), &BwaBed::set_group);
	ClassDB::bind_method(D_METHOD("get_group"), &BwaBed::get_group);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &BwaBed::set_paused);
	ClassDB::bind_method(D_METHOD("get_paused"), &BwaBed::get_paused);
	ClassDB::bind_method(D_METHOD("set_orientation", "yaw_pitch_roll"), &BwaBed::set_orientation);
	ClassDB::bind_method(D_METHOD("get_orientation"), &BwaBed::get_orientation);
	ClassDB::bind_method(D_METHOD("set_yaw_from_basis", "basis"), &BwaBed::set_yaw_from_basis);

	ClassDB::bind_method(D_METHOD("play"), &BwaBed::play);
	ClassDB::bind_method(D_METHOD("play_clip", "path"), &BwaBed::play_clip);
	ClassDB::bind_method(D_METHOD("play_at", "path", "start_sample"), &BwaBed::play_at);
	ClassDB::bind_method(
			D_METHOD("play_loop", "path", "loop_beg", "loop_end"), &BwaBed::play_loop);
	ClassDB::bind_method(D_METHOD("stop_at", "stop_sample"), &BwaBed::stop_at);
	ClassDB::bind_method(D_METHOD("set_region_frames", "start_frame", "end_frame"),
			&BwaBed::set_region_frames);
	ClassDB::bind_method(D_METHOD("set_region_seconds", "start_seconds", "end_seconds"),
			&BwaBed::set_region_seconds);
	ClassDB::bind_method(D_METHOD("stop"), &BwaBed::stop);
	ClassDB::bind_method(D_METHOD("fade_to", "gain", "seconds"), &BwaBed::fade_to);
	ClassDB::bind_method(D_METHOD("fade_out", "seconds"), &BwaBed::fade_out);
	ClassDB::bind_method(D_METHOD("seek_frames", "frame"), &BwaBed::seek_frames);
	ClassDB::bind_method(D_METHOD("seek_seconds", "seconds"), &BwaBed::seek_seconds);
	ClassDB::bind_method(D_METHOD("is_playing"), &BwaBed::is_playing);
	ClassDB::bind_method(D_METHOD("get_playhead_frames"), &BwaBed::get_playhead_frames);
	ClassDB::bind_method(D_METHOD("get_playhead_seconds"), &BwaBed::get_playhead_seconds);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "clip", PROPERTY_HINT_FILE, "*.wav,*.flac,*.amb"),
			"set_clip", "get_clip");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "format", PROPERTY_HINT_ENUM, "AmbiX,FuMa"),
			"set_format", "get_format");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autoplay"), "set_autoplay", "get_autoplay");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "async_load"), "set_async_load", "get_async_load");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_gain",
			"get_gain");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "orientation"), "set_orientation",
			"get_orientation");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "0,255,1"),
			"set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "group", PROPERTY_HINT_RANGE, "0,7,1"), "set_group",
			"get_group");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "get_paused");

	/* The same pair BwaEmitter carries, and the same contract: `finished` means the soundfield
	 * RAN OUT (a non-loop end, a region end) or that stop_at() landed - never that stop() or
	 * fade_out() was called. */
	ADD_SIGNAL(MethodInfo("finished"));
	/* One per loop WRAP. A looping bed never finishes, so `finished` reports it never; this is
	 * what paces a trial or cues a visual off an ambience loop. */
	ADD_SIGNAL(MethodInfo("looped"));

	BIND_ENUM_CONSTANT(FORMAT_AMBIX);
	BIND_ENUM_CONSTANT(FORMAT_FUMA);
}
