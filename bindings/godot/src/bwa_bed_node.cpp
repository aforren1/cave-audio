#include "bwa_bed_node.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

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
	if (!owner || !bed) {
		return;
	}
	if (owner->is_running()) {
		bwa_bed_destroy(ENG, bed);
	}
	bed = 0;
	owner = nullptr;
}

void BwaBed::play() { play_clip(clip); }

void BwaBed::play_clip(const String &p) {
	if (!LIVE) {
		return;
	}
	const bwa_sound snd = owner->load_ambisonic(p, format == FORMAT_FUMA);
	if (snd) {
		bwa_bed_play(ENG, bed, snd, loop);
	}
}

void BwaBed::stop() {
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
	if (LIVE) {
		bwa_bed_fade_out(ENG, bed, seconds);
	}
}

void BwaBed::seek_frames(int64_t frame) {
	if (LIVE) {
		bwa_bed_seek(ENG, bed, (uint64_t)frame);
	}
}

void BwaBed::seek_seconds(double seconds) {
	if (!LIVE || seconds < 0.0) {
		return;
	}
	const int rate = owner->get_resolved_sample_rate();
	if (rate > 0) {
		seek_frames((int64_t)(seconds * (double)rate));
	}
}

bool BwaBed::is_playing() const { return LIVE && bwa_bed_is_playing(ENG, bed); }

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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_gain",
			"get_gain");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "orientation"), "set_orientation",
			"get_orientation");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "0,255,1"),
			"set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "group", PROPERTY_HINT_RANGE, "0,7,1"), "set_group",
			"get_group");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "get_paused");

	BIND_ENUM_CONSTANT(FORMAT_AMBIX);
	BIND_ENUM_CONSTANT(FORMAT_FUMA);
}
