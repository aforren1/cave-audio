#include "bwa_source_base.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

using namespace godot;

#define ENG (owner->handle())
#define LIVE (owner && src && owner->is_running())

/* ONE command instead of fifteen, and the desc is checked BEFORE a voice is allocated, so a
 * refused configuration cannot leak one. */
bwa_source BwaSource::create_source() {
	bwa_source_desc d;
	fill_desc(&d);
	return bwa_source_create_desc(ENG, &d);
}

void BwaSource::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	owner = BwaEngine::get_singleton();
	if (!owner || !owner->is_running()) {
		UtilityFunctions::push_warning(vformat(
				"%s (%s): no running BwaEngine in the scene - this source is silent.",
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

/* The inspector is authored before the engine exists, so every setter caches into a field AND
 * pushes when live; this replays the cache onto a source that has just been minted.
 *
 * The CONFIGURATION half of that replay rides the desc, which create_source already handed to
 * bwa_source_create_desc (BwaPushSource applies it right after bwa_source_create_push, the one
 * mint with no desc form). What is left here is exactly what bwa_source_desc deliberately
 * leaves out: playback state, the manual-occlusion measurement, and orientation. */
void BwaSource::apply_all() {
	if (paused) {
		bwa_source_set_paused(ENG, src, true);
	}
	/* Standing state that is not a desc field, replayed like the manual occlusion below, so an
	 * engine rebuild does not silently put a reference source back on the panner mid-experiment. */
	if (channel != CHANNEL_AUTO) {
		bwa_source_set_channel(ENG, src, (int32_t)channel);
	}
	if (occ_manual_set) {
		if (occ_manual_banded) {
			const float b[3] = { (float)occ_manual_bands.x, (float)occ_manual_bands.y,
				(float)occ_manual_bands.z };
			bwa_source_set_occlusion_manual(ENG, src, occ_manual_level, b);
		} else {
			bwa_source_set_occlusion_manual(ENG, src, occ_manual_level, nullptr);
		}
	}
	if (orientation_set) {
		push_orientation(orientation_q);
	}
}

/* --- configuration as one value (bwa_source_desc) --- */

/* The Dictionary keys are the C field names, so a printed desc reads against bw_audio.h
 * without a translation table. struct_size and reserved stay out: they are the ABI's
 * business, and the binding always fills them from bwa_source_preset. */
static Dictionary desc_to_dict(const bwa_source_desc &d) {
	Dictionary o;
	o["gain"] = d.gain;
	o["pitch"] = d.pitch;
	o["priority"] = d.priority;
	o["group"] = (int)d.group;
	o["spread"] = d.spread;
	o["extent_height"] = d.extent_height;
	o["size_m"] = d.size_m;
	o["reverb_send"] = d.reverb_send;
	o["atten_ref_dist"] = d.atten_ref_dist;
	o["atten_rolloff"] = d.atten_rolloff;
	o["atten_min_gain"] = d.atten_min_gain;
	o["directivity_weight"] = d.directivity_weight;
	o["directivity_power"] = d.directivity_power;
	o["doppler"] = d.doppler;
	o["air_absorption"] = d.air_absorption;
	o["loudness_comp"] = d.loudness_comp;
	o["proximity"] = d.proximity;
	o["occlusion"] = d.occlusion;
	o["early_reflections"] = d.early_reflections;
	o["reverb"] = d.reverb;
	o["reverb_distance"] = d.reverb_distance;
	o["pathing"] = d.pathing;
	return o;
}

/* Overlay, not replace: an absent key keeps the value already in `d`. An unknown key is
 * reported rather than dropped - a mistyped "gian" is a setting that silently never took,
 * which is the failure class this binding exists to make visible. */
static void dict_to_desc(const Dictionary &src, bwa_source_desc *d) {
	static const char *const KNOWN[] = { "gain", "pitch", "priority", "group", "spread",
		"extent_height", "size_m", "reverb_send", "atten_ref_dist", "atten_rolloff",
		"atten_min_gain", "directivity_weight", "directivity_power", "doppler",
		"air_absorption", "loudness_comp", "proximity", "occlusion", "early_reflections",
		"reverb", "reverb_distance", "pathing", nullptr };
	const Array keys = src.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String k = keys[i];
		bool known = false;
		for (int j = 0; KNOWN[j]; j++) {
			if (k == String(KNOWN[j])) {
				known = true;
				break;
			}
		}
		if (!known) {
			UtilityFunctions::push_warning(
					vformat("BwaSource.apply_desc: unknown key \"%s\" (see get_desc() for the "
							"field names); it was ignored.",
							k));
		}
	}

#define BWA_DESC_F(key, field) \
	if (src.has(key)) d->field = (float)(double)src.get(key, (double)d->field)
#define BWA_DESC_B(key, field) \
	if (src.has(key)) d->field = (bool)src.get(key, d->field)

	BWA_DESC_F("gain", gain);
	BWA_DESC_F("pitch", pitch);
	if (src.has("priority")) {
		d->priority = (int32_t)(int64_t)src.get("priority", (int64_t)d->priority);
	}
	if (src.has("group")) {
		d->group = (uint32_t)(int64_t)src.get("group", (int64_t)d->group);
	}
	BWA_DESC_F("spread", spread);
	BWA_DESC_F("extent_height", extent_height);
	BWA_DESC_F("size_m", size_m);
	BWA_DESC_F("reverb_send", reverb_send);
	BWA_DESC_F("atten_ref_dist", atten_ref_dist);
	BWA_DESC_F("atten_rolloff", atten_rolloff);
	BWA_DESC_F("atten_min_gain", atten_min_gain);
	BWA_DESC_F("directivity_weight", directivity_weight);
	BWA_DESC_F("directivity_power", directivity_power);
	BWA_DESC_B("doppler", doppler);
	BWA_DESC_B("air_absorption", air_absorption);
	BWA_DESC_B("loudness_comp", loudness_comp);
	BWA_DESC_B("proximity", proximity);
	BWA_DESC_B("occlusion", occlusion);
	BWA_DESC_B("early_reflections", early_reflections);
	BWA_DESC_B("reverb", reverb);
	BWA_DESC_B("reverb_distance", reverb_distance);
	BWA_DESC_B("pathing", pathing);
#undef BWA_DESC_F
#undef BWA_DESC_B
}

void BwaSource::fill_desc(bwa_source_desc *d) const {
	bwa_source_preset(BWA_SRC_DEFAULT, d); /* struct_size, and a base that is never a zero-fill */
	d->gain = gain;
	d->priority = priority;
	d->group = (uint32_t)group;
	/* set_spread mirrors into `extent`, so equal axes ARE the isotropic case - which is what
	 * extent_height < 0 means to the core. Keep that reduction, or a plain spread would ship
	 * as an anisotropic extent that merely happens to be square. */
	const float w = (float)extent.x, h = (float)extent.y;
	if (w == h) {
		d->spread = (w != 0.0f) ? w : spread;
		d->extent_height = -1.0f;
	} else {
		d->spread = w;
		d->extent_height = h;
	}
	d->size_m = size_m;
	d->reverb_send = reverb_send;
	d->atten_ref_dist = atten_ref_dist;
	d->atten_rolloff = atten_rolloff;
	d->atten_min_gain = atten_min_gain;
	/* A directivity PRESET is a named (weight, power) pair, so it reduces to the desc's two
	 * fields exactly (bwa_source_set_directivity_preset does the same arithmetic). */
	if (dir_mode == DIRSET_CUSTOM) {
		d->directivity_weight = dir_weight;
		d->directivity_power = dir_power;
	} else if (dir_mode == DIRSET_PRESET) {
		d->directivity_weight = dir_preset == DIR_CARDIOID ? 0.5f : (dir_preset == DIR_FIGURE8 ? 1.0f : 0.0f);
		d->directivity_power = 1.0f;
	}
	d->doppler = doppler;
	d->air_absorption = air_absorption;
	d->loudness_comp = loudness_comp;
	d->proximity = proximity;
	d->occlusion = occlusion;
	d->early_reflections = early_reflections;
	d->reverb = reverb;
	d->reverb_distance = reverb_distance;
	d->pathing = pathing;
}

void BwaSource::mirror_desc(const bwa_source_desc &d) {
	gain = d.gain;
	priority = d.priority;
	group = (int)d.group;
	spread = d.spread;
	extent = d.extent_height < 0.0f ? Vector2(d.spread, d.spread)
									: Vector2(d.spread, d.extent_height);
	size_m = d.size_m;
	reverb_send = d.reverb_send;
	atten_ref_dist = d.atten_ref_dist;
	atten_rolloff = d.atten_rolloff;
	atten_min_gain = d.atten_min_gain;
	/* CUSTOM, whatever spelling got us here: a desc names weight and power, and omni is just
	 * weight 0. Keeping PRESET would let apply_all re-push the old pattern over this one. */
	dir_mode = DIRSET_CUSTOM;
	dir_weight = d.directivity_weight;
	dir_power = d.directivity_power;
	doppler = d.doppler;
	air_absorption = d.air_absorption;
	loudness_comp = d.loudness_comp;
	proximity = d.proximity;
	occlusion = d.occlusion;
	early_reflections = d.early_reflections;
	reverb = d.reverb;
	reverb_distance = d.reverb_distance;
	pathing = d.pathing;
}

bool BwaSource::push_desc(const bwa_source_desc &d) {
	mirror_desc(d);
	if (!LIVE) {
		return true; /* authored before _ready; create_source carries it in */
	}
	if (bwa_source_apply(ENG, src, &d)) {
		return true;
	}
	UtilityFunctions::push_error(vformat("%s (%s): the source desc was refused: %s", get_class(),
			get_name(), owner->get_last_error()));
	return false;
}

Dictionary BwaSource::get_preset(Kind kind) {
	bwa_source_desc d;
	bwa_source_preset((bwa_source_kind)kind, &d);
	return desc_to_dict(d);
}

Dictionary BwaSource::get_desc() const {
	bwa_source_desc d;
	/* The engine reports what it was SET to (its readback shadow). With no engine yet, the
	 * node's own authored state is the honest answer, so the editor is not left with {}. */
	if (!LIVE || !bwa_source_get_desc(ENG, src, &d)) {
		fill_desc(&d);
	}
	return desc_to_dict(d);
}

bool BwaSource::apply_desc(const Dictionary &dict) {
	bwa_source_desc d;
	fill_desc(&d); /* the current configuration is the base the keys overlay onto */
	dict_to_desc(dict, &d);
	return push_desc(d);
}

bool BwaSource::reset_to_preset(Kind kind) {
	bwa_source_desc d;
	bwa_source_preset((bwa_source_kind)kind, &d);
	return push_desc(d);
}

PackedStringArray BwaSource::_get_configuration_warnings() const {
	PackedStringArray w;
	if (early_reflections) {
		/* The one combination the engine's own docs call out as a mistake: the Steam bed
		 * ALREADY contains early reflections, so running both renders them twice — which
		 * does not sound like a bug, only like a roomier room. */
		w.push_back("Image-source early reflections pair with the FDN, never with the Steam "
					"reflection bed - that bed already contains early reflections, so both "
					"together render them twice.");
		w.push_back("Early reflections need the room: set a BwaRoomBox in the scene, or this "
					"source renders dry.");
	}
	if (occlusion) {
		w.push_back("Ray-traced occlusion needs the Steam Audio build and scene geometry. "
					"Without the SDK use set_occlusion_manual() instead - and never drive one "
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

/* The one place a caller-supplied orientation reaches the ABI, so the FACING seam is applied
 * here and nowhere else. Callers only ever hold Godot-frame quaternions. */
void BwaSource::push_orientation(const Quaternion &q) {
	const Quaternion r = owner->to_room_orientation(Basis(q));
	bwa_source_set_orientation(ENG, src, (float)r.x, (float)r.y, (float)r.z, (float)r.w);
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

/* --- direct output-channel route --- */

void BwaSource::set_channel(int ch) {
	/* Range-check HERE when the answer is knowable, rather than let the core refuse it into
	 * bwa_last_error and leave get_channel() reporting a route the voice is not on.
	 *
	 * CHANNEL_AUTO is the ONE negative that means anything, and the core refuses every other one
	 * exactly as it refuses a too-large index (engine.c: folding them into AUTO would make a bad
	 * index look like it was TAKEN - the source keeps panning, nothing is reported, and the caller
	 * reads a phantom as a single-speaker reference). So this guard has to cover both ends, not just
	 * the upper one.
	 *
	 * The two ends are knowable at different times, which is why they are two checks. A negative is
	 * bad with NO channel count, so it is refused even before the engine is live. A too-large one
	 * needs the count, so that half waits for LIVE and apply_all replays the cached value for the
	 * core to judge at the moment it can. */
	if (ch != CHANNEL_AUTO && ch < 0) {
		UtilityFunctions::push_warning(vformat(
				"%s (%s): channel %d is negative; the only negative that means anything is "
				"BwaSource.CHANNEL_AUTO (%d), which restores spatial panning. "
				"The source stays on channel %d.",
				get_class(), get_name(), ch, CHANNEL_AUTO, channel));
		return;
	}
	if (LIVE && ch >= owner->get_channel_count()) {
		UtilityFunctions::push_warning(vformat(
				"%s (%s): channel %d is out of range (0 to %d, or BwaSource.CHANNEL_AUTO); "
				"the source stays on channel %d.",
				get_class(), get_name(), ch, owner->get_channel_count() - 1, channel));
		return;
	}
	channel = ch;
	if (LIVE) {
		bwa_source_set_channel(ENG, src, (int32_t)ch);
	}
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
	occ_manual_set = true;
	occ_manual_banded = false;
	occ_manual_level = level;
	if (LIVE) {
		bwa_source_set_occlusion_manual(ENG, src, level, nullptr);
	}
}

void BwaSource::set_occlusion_manual_bands(float level, const Vector3 &bands) {
	occ_manual_set = true;
	occ_manual_banded = true;
	occ_manual_level = level;
	occ_manual_bands = bands;
	if (LIVE) {
		const float b[3] = { (float)bands.x, (float)bands.y, (float)bands.z };
		bwa_source_set_occlusion_manual(ENG, src, level, b);
	}
}

float BwaSource::get_occlusion_factor() const {
	return LIVE ? bwa_source_get_occlusion(ENG, src) : 1.0f;
}

/* Cached like every standing knob (the set_attenuation_override rule), so a pre-ready call
 * is not silently dropped and apply_all() re-asserts it after a re-mint. The two spellings
 * are last-one-wins, so the cache remembers WHICH was used, not both. */
void BwaSource::set_directivity(float weight, float power) {
	dir_mode = DIRSET_CUSTOM;
	dir_weight = weight;
	dir_power = power;
	if (LIVE) {
		bwa_source_set_directivity(ENG, src, weight, power);
	}
}

void BwaSource::set_directivity_preset(Directivity pattern) {
	dir_mode = DIRSET_PRESET;
	dir_preset = pattern;
	if (LIVE) {
		bwa_source_set_directivity_preset(ENG, src, (bwa_directivity)pattern);
	}
}

float BwaSource::get_directivity_gain() const {
	return LIVE ? bwa_source_get_directivity(ENG, src) : 1.0f;
}

/* Takes a GODOT-frame orientation, like every geometric input on this node: it goes through
 * the facing seam (registration included), exactly as push_frame does for
 * orientation_follows_node. Passing it raw shipped once and aimed every dipole 180 degrees
 * from the node it was authored on. */
void BwaSource::set_orientation(const Quaternion &q) {
	orientation_set = true;
	orientation_q = q;
	if (LIVE) {
		push_orientation(q);
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

int64_t BwaSource::get_playhead_frames() const {
	return LIVE ? (int64_t)bwa_source_get_playhead_frames(ENG, src) : 0;
}

double BwaSource::get_playhead_seconds() const {
	if (!LIVE) {
		return 0.0;
	}
	const int rate = owner->get_resolved_sample_rate();
	return rate > 0 ? (double)bwa_source_get_playhead_frames(ENG, src) / (double)rate : 0.0;
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

	/* Methods, not a property: the direct route is a run-time experimental condition, so it should
	 * not be serialized into a scene the way gain or spread is. See the header. */
	ClassDB::bind_method(D_METHOD("set_channel", "channel"), &BwaSource::set_channel);
	ClassDB::bind_method(D_METHOD("get_channel"), &BwaSource::get_channel);

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

	ClassDB::bind_method(D_METHOD("get_desc"), &BwaSource::get_desc);
	ClassDB::bind_method(D_METHOD("apply_desc", "desc"), &BwaSource::apply_desc);
	ClassDB::bind_method(D_METHOD("reset_to_preset", "kind"), &BwaSource::reset_to_preset);
	/* Static: bwa_source_preset needs no engine, so a tool can print the table with nothing
	 * running - BwaSource.get_preset(BwaSource.KIND_PROP). */
	ClassDB::bind_static_method(
			"BwaSource", D_METHOD("get_preset", "kind"), &BwaSource::get_preset);

	ClassDB::bind_method(D_METHOD("is_playing"), &BwaSource::is_playing);
	ClassDB::bind_method(D_METHOD("get_playhead_frames"), &BwaSource::get_playhead_frames);
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

	/* BwaSource.CHANNEL_AUTO — the value set_channel takes to restore spatial panning. */
	BIND_CONSTANT(CHANNEL_AUTO);

	BIND_ENUM_CONSTANT(DIR_OMNI);
	BIND_ENUM_CONSTANT(DIR_CARDIOID);
	BIND_ENUM_CONSTANT(DIR_FIGURE8);

	BIND_ENUM_CONSTANT(KIND_DEFAULT);
	BIND_ENUM_CONSTANT(KIND_PROP);
	BIND_ENUM_CONSTANT(KIND_VOICE);
	BIND_ENUM_CONSTANT(KIND_AMBIENCE);
	BIND_ENUM_CONSTANT(KIND_UI);
}
