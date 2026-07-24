#include "bwa_source_base.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

bwa_source BwaSource::create_source() { return bwa_source_create(ENG); }

void BwaSource::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	owner = BwaEngine::get_singleton();
	if (!owner || !owner->is_running()) {
		UtilityFunctions::push_warning(vformat(
				"%s (%s): no running BwaEngine in the scene — this source is silent.",
				get_class(), get_name()));
		owner = nullptr;
		return;
	}

	src = create_source();
	if (!src) {
		UtilityFunctions::push_error(vformat("%s (%s): could not create a source: %s", get_class(),
				get_name(), owner->get_last_error()));
		owner = nullptr;
		return;
	}

	apply_all();
	push_frame(); // place it before it can be heard
	owner->register_source(this);
	on_source_ready();
}

void BwaSource::_exit_tree() {
	if (!owner || !src) {
		return;
	}
	owner->unregister_source(this);
	if (owner->is_running()) {
		bwa_source_destroy(ENG, src);
	}
	src = 0;
	owner = nullptr;
}

/* The inspector is authored before the engine exists, so every setter caches into a field
 * AND pushes when live; this replays the cache onto a source that has just been minted. */
void BwaSource::apply_all() {
	bwa_source_set_gain(ENG, src, gain);
	bwa_source_set_priority(ENG, src, priority);
	bwa_source_set_group(ENG, src, (uint32_t)group);
	if (paused) {
		bwa_source_set_paused(ENG, src, true);
	}
	if (extent != Vector2(0.0f, 0.0f)) {
		bwa_source_set_extent(ENG, src, (float)extent.x, (float)extent.y);
	} else if (spread > 0.0f) {
		bwa_source_set_spread(ENG, src, spread);
	}
	if (size_m > 0.0f) {
		bwa_source_set_size(ENG, src, size_m);
	}
	if (atten_ref_dist > 0.0f) {
		bwa_source_set_attenuation_override(ENG, src, atten_ref_dist, atten_rolloff, atten_min_gain);
	}
	if (doppler) {
		bwa_source_set_doppler(ENG, src, true);
	}
	if (air_absorption) {
		bwa_source_set_air_absorption(ENG, src, true);
	}
	if (loudness_comp) {
		bwa_source_set_loudness_comp(ENG, src, true);
	}
	if (proximity) {
		bwa_source_set_proximity(ENG, src, true);
	}
	if (occlusion) {
		bwa_source_set_occlusion(ENG, src, true);
	}
	if (reverb) {
		bwa_source_set_reverb(ENG, src, true);
	}
	bwa_source_set_reverb_send(ENG, src, reverb_send);
	if (reverb_distance) {
		bwa_source_set_reverb_distance(ENG, src, true);
	}
	if (early_reflections) {
		bwa_source_set_early_reflections(ENG, src, true);
	}
	if (pathing) {
		bwa_source_set_pathing(ENG, src, true);
	}
}

PackedStringArray BwaSource::_get_configuration_warnings() const {
	PackedStringArray w;
	if (early_reflections) {
		/* The one combination the engine's own docs call out as a mistake: the Steam bed
		 * ALREADY contains early reflections, so running both renders them twice — which
		 * does not sound like a bug, only like a roomier room. */
		w.push_back("Image-source early reflections pair with the FDN, never with the Steam "
					"reflection bed — that bed already contains early reflections, so both "
					"together render them twice.");
		w.push_back("Early reflections need the room: set a BwaRoomBox in the scene, or this "
					"source renders dry.");
	}
	if (occlusion) {
		w.push_back("Ray-traced occlusion needs the Steam Audio build and scene geometry. "
					"Without the SDK use set_occlusion_manual() instead — and never drive one "
					"source through both, because the sim republishes every tick and wins.");
	}
	if (pathing) {
		w.push_back("Pathing also needs `enable_pathing` on the BwaEngine node; the per-source "
					"switch alone does nothing.");
	}
	return w;
}

void BwaSource::push_frame() {
	if (!LIVE) {
		return;
	}
	const Vector3 p = owner->to_room_position(get_global_position());
	bwa_source_set_pos(ENG, src, (float)p.x, (float)p.y, (float)p.z);

	if (orientation_follows_node) {
		const Quaternion q = owner->to_room_orientation(get_global_basis());
		bwa_source_set_orientation(ENG, src, (float)q.x, (float)q.y, (float)q.z, (float)q.w);
	}
}

/* --- level / routing --- */

void BwaSource::set_gain(float g) {
	gain = g;
	if (LIVE) {
		bwa_source_set_gain(ENG, src, g); // also cancels any running fade
	}
}

void BwaSource::set_priority(int p) {
	priority = p;
	if (LIVE) {
		bwa_source_set_priority(ENG, src, p);
	}
}

void BwaSource::set_group(int g) {
	group = g;
	if (LIVE) {
		bwa_source_set_group(ENG, src, (uint32_t)g);
	}
}

void BwaSource::fade_to(float target, float seconds) {
	gain = target;
	if (LIVE) {
		bwa_source_fade_to(ENG, src, target, seconds);
	}
}

void BwaSource::fade_out(float seconds) {
	if (LIVE) {
		bwa_source_fade_out(ENG, src, seconds); // fades to silence, then takes the stop path
	}
}

void BwaSource::set_paused(bool p) {
	paused = p;
	if (LIVE) {
		bwa_source_set_paused(ENG, src, p);
	}
}

void BwaSource::stop() {
	if (LIVE) {
		bwa_source_stop(ENG, src);
	}
}

void BwaSource::engine_gone() {
	/* No calls into the engine — it is mid-teardown. Just forget it. */
	owner = nullptr;
	src = 0;
}

/* --- extent --- */

void BwaSource::set_spread(float amount) {
	spread = amount;
	extent = Vector2(amount, amount); // spread resets to isotropic; keep the mirror honest
	if (LIVE) {
		bwa_source_set_spread(ENG, src, amount);
	}
}

void BwaSource::set_extent(const Vector2 &wh) {
	extent = wh;
	if (LIVE) {
		bwa_source_set_extent(ENG, src, (float)wh.x, (float)wh.y);
	}
}

void BwaSource::set_size(float radius_m) {
	size_m = radius_m;
	if (LIVE) {
		bwa_source_set_size(ENG, src, radius_m);
	}
}

/* --- propagation --- */

void BwaSource::set_doppler(bool on) {
	doppler = on;
	if (LIVE) {
		bwa_source_set_doppler(ENG, src, on);
	}
}

void BwaSource::set_air_absorption(bool on) {
	air_absorption = on;
	if (LIVE) {
		bwa_source_set_air_absorption(ENG, src, on);
	}
}

void BwaSource::set_loudness_comp(bool on) {
	loudness_comp = on;
	if (LIVE) {
		bwa_source_set_loudness_comp(ENG, src, on);
	}
}

/* Near-field proximity boost: LF rises as the source closes inside ~1 m of the listener, so
 * "at arm's length" reads as bass, not just level. Loudness comp's near mirror. */
void BwaSource::set_proximity(bool on) {
	proximity = on;
	if (LIVE) {
		bwa_source_set_proximity(ENG, src, on);
	}
}

void BwaSource::set_attenuation_override(float ref_dist, float rolloff, float min_gain) {
	/* Cached like every other standing knob (it is persistent per-source state, not a one-shot),
	 * so a pre-ready call isn't silently dropped and apply_all() re-asserts it after a re-mint.
	 * ref_dist <= 0 clears, mirroring the native contract. */
	atten_ref_dist = ref_dist;
	atten_rolloff = rolloff;
	atten_min_gain = min_gain;
	if (LIVE) {
		bwa_source_set_attenuation_override(ENG, src, ref_dist, rolloff, min_gain);
	}
}

/* --- occlusion / directivity --- */

void BwaSource::set_occlusion(bool on) {
	occlusion = on;
	if (LIVE) {
		bwa_source_set_occlusion(ENG, src, on);
	}
}

void BwaSource::set_occlusion_manual(float level) {
	if (LIVE) {
		bwa_source_set_occlusion_manual(ENG, src, level, nullptr);
	}
}

void BwaSource::set_occlusion_manual_bands(float level, const Vector3 &bands) {
	if (LIVE) {
		const float b[3] = { (float)bands.x, (float)bands.y, (float)bands.z };
		bwa_source_set_occlusion_manual(ENG, src, level, b);
	}
}

float BwaSource::get_occlusion_factor() const {
	return LIVE ? bwa_source_get_occlusion(ENG, src) : 1.0f;
}

void BwaSource::set_directivity(float weight, float power) {
	if (LIVE) {
		bwa_source_set_directivity(ENG, src, weight, power);
	}
}

void BwaSource::set_directivity_preset(Directivity pattern) {
	if (LIVE) {
		bwa_source_set_directivity_preset(ENG, src, (bwa_directivity)pattern);
	}
}

float BwaSource::get_directivity_gain() const {
	return LIVE ? bwa_source_get_directivity(ENG, src) : 1.0f;
}

void BwaSource::set_orientation(const Quaternion &q) {
	if (LIVE) {
		bwa_source_set_orientation(ENG, src, (float)q.x, (float)q.y, (float)q.z, (float)q.w);
	}
}

/* --- reverb / reflections --- */

void BwaSource::set_reverb(bool on) {
	reverb = on;
	if (LIVE) {
		bwa_source_set_reverb(ENG, src, on);
	}
}

void BwaSource::set_reverb_send(float g) {
	reverb_send = g;
	if (LIVE) {
		bwa_source_set_reverb_send(ENG, src, g);
	}
}

void BwaSource::set_reverb_distance(bool on) {
	reverb_distance = on;
	if (LIVE) {
		bwa_source_set_reverb_distance(ENG, src, on);
	}
}

void BwaSource::set_early_reflections(bool on) {
	early_reflections = on;
	if (LIVE) {
		bwa_source_set_early_reflections(ENG, src, on);
	}
}

void BwaSource::set_pathing(bool on) {
	pathing = on;
	if (LIVE) {
		bwa_source_set_pathing(ENG, src, on);
	}
}

/* --- readbacks --- */

bool BwaSource::is_playing() const { return LIVE && bwa_source_is_playing(ENG, src); }

int64_t BwaSource::get_playhead() const {
	return LIVE ? (int64_t)bwa_source_get_playhead(ENG, src) : 0;
}

double BwaSource::get_playhead_seconds() const {
	if (!LIVE) {
		return 0.0;
	}
	const int rate = owner->get_resolved_sample_rate();
	return rate > 0 ? (double)bwa_source_get_playhead(ENG, src) / (double)rate : 0.0;
}

void BwaSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_gain", "linear"), &BwaSource::set_gain);
	ClassDB::bind_method(D_METHOD("get_gain"), &BwaSource::get_gain);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &BwaSource::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &BwaSource::get_priority);
	ClassDB::bind_method(D_METHOD("set_group", "group"), &BwaSource::set_group);
	ClassDB::bind_method(D_METHOD("get_group"), &BwaSource::get_group);
	ClassDB::bind_method(D_METHOD("fade_to", "gain", "seconds"), &BwaSource::fade_to);
	ClassDB::bind_method(D_METHOD("fade_out", "seconds"), &BwaSource::fade_out);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &BwaSource::set_paused);
	ClassDB::bind_method(D_METHOD("get_paused"), &BwaSource::get_paused);
	ClassDB::bind_method(D_METHOD("stop"), &BwaSource::stop);

	ClassDB::bind_method(D_METHOD("set_spread", "amount"), &BwaSource::set_spread);
	ClassDB::bind_method(D_METHOD("get_spread"), &BwaSource::get_spread);
	ClassDB::bind_method(D_METHOD("set_extent", "width_height"), &BwaSource::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &BwaSource::get_extent);
	ClassDB::bind_method(D_METHOD("set_size", "radius_m"), &BwaSource::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &BwaSource::get_size);

	ClassDB::bind_method(D_METHOD("set_doppler", "on"), &BwaSource::set_doppler);
	ClassDB::bind_method(D_METHOD("get_doppler"), &BwaSource::get_doppler);
	ClassDB::bind_method(D_METHOD("set_air_absorption", "on"), &BwaSource::set_air_absorption);
	ClassDB::bind_method(D_METHOD("get_air_absorption"), &BwaSource::get_air_absorption);
	ClassDB::bind_method(D_METHOD("set_loudness_comp", "on"), &BwaSource::set_loudness_comp);
	ClassDB::bind_method(D_METHOD("get_loudness_comp"), &BwaSource::get_loudness_comp);
	ClassDB::bind_method(D_METHOD("set_proximity", "on"), &BwaSource::set_proximity);
	ClassDB::bind_method(D_METHOD("get_proximity"), &BwaSource::get_proximity);
	ClassDB::bind_method(D_METHOD("set_attenuation_override", "ref_dist", "rolloff", "min_gain"),
			&BwaSource::set_attenuation_override);

	ClassDB::bind_method(D_METHOD("set_occlusion", "on"), &BwaSource::set_occlusion);
	ClassDB::bind_method(D_METHOD("get_occlusion"), &BwaSource::get_occlusion);
	ClassDB::bind_method(D_METHOD("set_occlusion_manual", "level"), &BwaSource::set_occlusion_manual);
	ClassDB::bind_method(D_METHOD("set_occlusion_manual_bands", "level", "bands"),
			&BwaSource::set_occlusion_manual_bands);
	ClassDB::bind_method(D_METHOD("get_occlusion_factor"), &BwaSource::get_occlusion_factor);
	ClassDB::bind_method(
			D_METHOD("set_directivity", "weight", "power"), &BwaSource::set_directivity);
	ClassDB::bind_method(
			D_METHOD("set_directivity_preset", "pattern"), &BwaSource::set_directivity_preset);
	ClassDB::bind_method(D_METHOD("get_directivity_gain"), &BwaSource::get_directivity_gain);
	ClassDB::bind_method(D_METHOD("set_orientation", "quaternion"), &BwaSource::set_orientation);
	ClassDB::bind_method(D_METHOD("set_orientation_follows_node", "enabled"),
			&BwaSource::set_orientation_follows_node);
	ClassDB::bind_method(
			D_METHOD("get_orientation_follows_node"), &BwaSource::get_orientation_follows_node);

	ClassDB::bind_method(D_METHOD("set_reverb", "on"), &BwaSource::set_reverb);
	ClassDB::bind_method(D_METHOD("get_reverb"), &BwaSource::get_reverb);
	ClassDB::bind_method(D_METHOD("set_reverb_send", "gain"), &BwaSource::set_reverb_send);
	ClassDB::bind_method(D_METHOD("get_reverb_send"), &BwaSource::get_reverb_send);
	ClassDB::bind_method(D_METHOD("set_reverb_distance", "on"), &BwaSource::set_reverb_distance);
	ClassDB::bind_method(D_METHOD("get_reverb_distance"), &BwaSource::get_reverb_distance);
	ClassDB::bind_method(
			D_METHOD("set_early_reflections", "on"), &BwaSource::set_early_reflections);
	ClassDB::bind_method(D_METHOD("get_early_reflections"), &BwaSource::get_early_reflections);
	ClassDB::bind_method(D_METHOD("set_pathing", "on"), &BwaSource::set_pathing);
	ClassDB::bind_method(D_METHOD("get_pathing"), &BwaSource::get_pathing);

	ClassDB::bind_method(D_METHOD("is_playing"), &BwaSource::is_playing);
	ClassDB::bind_method(D_METHOD("get_playhead"), &BwaSource::get_playhead);
	ClassDB::bind_method(D_METHOD("get_playhead_seconds"), &BwaSource::get_playhead_seconds);

	ADD_GROUP("Mix", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_gain",
			"get_gain");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "0,255,1"),
			"set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "group", PROPERTY_HINT_RANGE, "0,7,1"), "set_group",
			"get_group");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "get_paused");

	ADD_GROUP("Extent", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread", PROPERTY_HINT_RANGE, "0,1,0.01"),
			"set_spread", "get_spread");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "extent"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater"),
			"set_size", "get_size");

	ADD_GROUP("Propagation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "doppler"), "set_doppler", "get_doppler");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "air_absorption"), "set_air_absorption",
			"get_air_absorption");
	ADD_PROPERTY(
			PropertyInfo(Variant::BOOL, "loudness_comp"), "set_loudness_comp", "get_loudness_comp");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "proximity"), "set_proximity", "get_proximity");

	ADD_GROUP("Occlusion", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion"), "set_occlusion", "get_occlusion");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "orientation_follows_node"),
			"set_orientation_follows_node", "get_orientation_follows_node");

	ADD_GROUP("Reverb", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverb"), "set_reverb", "get_reverb");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reverb_send", PROPERTY_HINT_RANGE, "0,2,0.01"),
			"set_reverb_send", "get_reverb_send");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverb_distance"), "set_reverb_distance",
			"get_reverb_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "early_reflections"), "set_early_reflections",
			"get_early_reflections");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pathing"), "set_pathing", "get_pathing");

	BIND_ENUM_CONSTANT(DIR_OMNI);
	BIND_ENUM_CONSTANT(DIR_CARDIOID);
	BIND_ENUM_CONSTANT(DIR_FIGURE8);
}
