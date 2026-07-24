#include "bwa_engine_node.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_geometry.h"
#include "bwa_material.h"
#include "bwa_room.h"
#include "bwa_source_base.h"

#include <cmath>

using namespace godot;

BwaEngine *BwaEngine::singleton = nullptr;
int BwaEngine::next_generation = 0;

/* res:// -> an OS path the C core can fopen. The core loads assets itself (it never goes
 * through Godot's VFS), so anything it reads must be a real file on disk. In an exported
 * build res:// lives inside the .pck and globalize_path returns a path that does not
 * exist — ship those files beside the executable or stage them into user:// instead. Same
 * trap as Unity's StreamingAssets, one layer down. */
static String to_os_path(const String &p) {
	if (p.is_empty()) {
		return p;
	}
	if (p.begins_with("res://") || p.begins_with("user://")) {
		return ProjectSettings::get_singleton()->globalize_path(p);
	}
	return p;
}

void BwaEngine::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return; // never grab an audio device just because a scene was opened
	}
	if (singleton && singleton != this) {
		UtilityFunctions::push_error(
				"BwaEngine: a second engine node entered the tree; only one may run. This one is "
				"inert.");
		return;
	}

	const uint32_t dll = bwa_get_version();
	if (dll != BWA_VERSION) {
		UtilityFunctions::push_warning(vformat(
				"BwaEngine: bw_audio.dll is version %d.%d.%d but the extension was built against "
				"%d.%d.%d — rebuild the binding against the DLL you are shipping.",
				(dll >> 16) & 0xff, (dll >> 8) & 0xff, dll & 0xff, BWA_VERSION_MAJOR,
				BWA_VERSION_MINOR, BWA_VERSION_PATCH));
	}

	const CharString layout_utf8 = to_os_path(layout_path).utf8();
	const CharString driver_utf8 = asio_driver.utf8();

	bwa_desc cfg = {};
	cfg.profile = (bwa_profile)profile;
	cfg.layout_path = layout_path.is_empty() ? nullptr : layout_utf8.get_data();
	cfg.hrtf_path = nullptr;
	cfg.sample_rate = (uint32_t)sample_rate;
	cfg.block_size = (uint32_t)block_size;
	cfg.sink = (bwa_sink_type)sink;
	cfg.asio_driver = asio_driver.is_empty() ? nullptr : driver_utf8.get_data();
	cfg.embree = embree;
	cfg.enable_pathing = enable_pathing;
	cfg.bed_decoder = (bwa_bed_decoder)bed_decoder;

	eng = bwa_create(&cfg);
	if (!eng) {
		UtilityFunctions::push_error("BwaEngine: bwa_create failed");
		return;
	}
	/* A failed layout load is NOT fatal — the core falls back to the 26-speaker default
	 * grid and only records why. On a smaller rig that silently changes the channel count,
	 * so surface it; bwa_start refuses the fallback anyway when a path was given. */
	if (const char *err = bwa_last_error(eng)) {
		UtilityFunctions::push_warning(String("BwaEngine: ") + String(err));
	}

	/* Load-time config, between create and start. The two reverb beds share one tap, so
	 * only one may own it; the core would pick for us, but silently. */
	if (refl.enabled && fdn.enabled) {
		UtilityFunctions::push_warning(
				"BwaEngine: both the Steam reflection bed and the FDN are enabled, but they share "
				"one reverb tap. Enable exactly one.");
	}
	if (refl.enabled) {
		bwa_reflections_config(eng, &refl);
	}
	if (fdn.enabled) {
		bwa_fdn_config(eng, &fdn);
	}
	generation = ++next_generation; /* process-wide, so no two engine instances ever share one */
	build_static_scene();

	const bwa_result r = bwa_start(eng);
	if (r != BWA_OK) {
		const char *err = bwa_last_error(eng);
		UtilityFunctions::push_error(vformat("BwaEngine: bwa_start failed (%d): %s", (int)r,
				err ? String(err) : String("no detail")));
		bwa_destroy(eng);
		eng = nullptr;
		return;
	}

	singleton = this;

	/* Replay everything authored in the inspector before the engine existed. */
	bwa_set_master_gain(eng, master_gain);
	bwa_set_reverb_gain(eng, reverb_gain);
	bwa_set_early_reflections_gain(eng, er_gain);
	bwa_set_panner(eng, (bwa_panner)panner);
	bwa_set_dual_band(eng, dual_band);
	bwa_set_spread_mode(eng, (bwa_spread_mode)spread_mode);
	bwa_set_decorrelation(eng, decorrelation);
	bwa_set_near_spread(eng, near_spread);
	bwa_set_speed_of_sound(eng, speed_of_sound);
	bwa_set_max_re(eng, max_re);
	bwa_set_max_re_split(eng, max_re_split);
	bwa_set_bed_renderer(eng, (bwa_bed_renderer)bed_renderer);
	bwa_set_tracked_room_eq(eng, tracked_room_eq);
	bwa_set_limiter(eng, limiter);
	bwa_set_limiter_ceiling(eng, limiter_ceiling);
	bwa_set_headphone_eq(eng, headphone_eq_on);
	if (paused) {
		bwa_set_paused(eng, true);
	}

	set_process(true);
	UtilityFunctions::print(vformat("BwaEngine: %s, %d ch @ %d Hz / %d frames",
			get_audio_backend(), get_channel_count(), get_resolved_sample_rate(),
			get_resolved_block_size()));
}

void BwaEngine::_process(double delta) {
	(void)delta;
	if (!eng) {
		return;
	}

	/* By index, not by iterator: push_frame can emit `finished`, and a user handler may add
	 * or free sources — freeing runs _exit_tree, which unregisters and mutates this vector.
	 * An index survives that (a freed source is removed BEFORE its memory dies, so it is
	 * never revisited; a same-frame addition is simply picked up or caught next frame). A
	 * range-for here is iterator invalidation; a copied snapshot is worse — it would keep a
	 * dangling pointer to anything the handler freed. */
	for (size_t i = 0; i < sources.size(); i++) {
		sources[i]->push_frame(); // all sources first...
	}

	if (feed_listener && !listener_path.is_empty()) {
		Node3D *lis = Object::cast_to<Node3D>(get_node_or_null(listener_path));
		if (lis) {
			const Transform3D t = lis->get_global_transform();
			const Vector3 p = to_room_position(t.origin);
			const Quaternion q = to_room_orientation(t.basis);
			bwa_set_listener_pose(eng, p.x, p.y, p.z, q.x, q.y, q.z, q.w); // ...then the listener
		}
	}

	bwa_commit(eng); // ...and one atomic snapshot of the pair
}

void BwaEngine::_exit_tree() {
	if (singleton == this) {
		singleton = nullptr;
	}
	if (!eng) {
		return;
	}
	bwa_stop(eng);
	bwa_destroy(eng);
	eng = nullptr;
	sounds.clear();
	/* Detach every source that outlives this engine, not just forget them: their `owner`
	 * back-pointers would otherwise dangle, and the next setter call on such a source (from
	 * a script, an animation, anywhere) would dereference a freed node. Exit order usually
	 * saves the shipped scenes — sources exit before a first-child engine — but nothing
	 * forces an engine to be the first child, or sources to share its subtree at all. */
	for (BwaSource *s : sources) {
		s->engine_gone();
	}
	sources.clear();
}

/* --- static scene assembly --- */

void BwaEngine::build_static_scene() {
	BwaRoomBox *box = BwaRoomBox::active();
	const std::vector<BwaAcousticGeometry *> &geo = BwaAcousticGeometry::registry();
	if (!box && geo.empty()) {
		return;
	}

	/* The room box goes FIRST and on its own, because bwa_scene_set_box is the only way to
	 * capture the shoebox for the image-source early reflections — and that half works with
	 * or without the Steam Audio SDK, so it matters even in a phonon-free build. */
	PackedVector3Array faces;
	PackedInt32Array tri_materials;
	if (box) {
		PackedInt32Array face_tokens;
		for (int f = 0; f < 6; f++) {
			const Ref<BwaMaterial> m = box->get_face_material(f);
			face_tokens.push_back(m.is_valid() ? (int)m->token(this) : 0);
		}
		const Vector3 s = box->room_size();
		scene_set_box((float)s.x, (float)s.y, (float)s.z, face_tokens);

		/* ...and then its walls join the merged mesh, because that same call REPLACED the
		 * static mesh with just the box. Without this the walls would vanish the moment any
		 * other occluder exists. */
		faces.append_array(box->wall_faces());
		for (int t = 0; t < 12; t++) {
			tri_materials.push_back(face_tokens[t / 2]);
		}
	}

	for (BwaAcousticGeometry *g : geo) {
		const PackedVector3Array f = g->world_faces();
		if (f.is_empty()) {
			continue;
		}
		const Ref<BwaMaterial> m = g->material_ref();
		const int tok = m.is_valid() ? (int)m->token(this) : 0;
		faces.append_array(f);
		for (int t = 0; t < f.size() / 3; t++) {
			tri_materials.push_back(tok);
		}
	}

	/* With a box and nothing else, scene_set_box already committed exactly this mesh; a
	 * second identical call would only buy a redundant BVH rebuild. */
	if (faces.is_empty() || (box && geo.empty())) {
		return;
	}

	PackedInt32Array tris;
	tris.resize(faces.size());
	for (int i = 0; i < faces.size(); i++) {
		tris[i] = i; // get_faces() is already a triangle soup, so the index list is trivial
	}
	scene_set_mesh(faces, tris, tri_materials);
}

/* --- the coordinate seam --- */

Quaternion BwaEngine::to_room_orientation(const Basis &b) const {
	const Quaternion g = Quaternion((registration.basis * b).orthonormalized());
	const float in[4] = { (float)g.x, (float)g.y, (float)g.z, (float)g.w };
	float out[4];
	bwa_room_facing_quat(in, out);
	return Quaternion(out[0], out[1], out[2], out[3]);
}

float BwaEngine::to_room_yaw(const Basis &b) const {
	/* A node's facing is -Z of its basis; move that into room axes and read its yaw. No
	 * mirror means no sign flip — the Unity binding reverses here, this one must not. */
	const Vector3 ahead = to_room_direction(-(b.get_column(2)));
	return bwa_room_yaw_from_dir((float)ahead.x, (float)ahead.z);
}

Vector3 BwaEngine::from_room_position(const Vector3 &v) const {
	return registration.affine_inverse().xform(v);
}

Quaternion BwaEngine::from_room_orientation(const Quaternion &q) const {
	const float in[4] = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
	float out[4];
	bwa_godot_facing_quat(in, out);
	const Quaternion g(out[0], out[1], out[2], out[3]);
	return Quaternion(registration.basis.orthonormalized()).inverse() * g;
}

/* --- listener --- */

void BwaEngine::set_listener_pose(const Vector3 &pos, const Quaternion &rot) {
	if (!eng) {
		return;
	}
	const Vector3 p = to_room_position(pos);
	const Quaternion q = to_room_orientation(Basis(rot));
	bwa_set_listener_pose(eng, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
}

Transform3D BwaEngine::get_listener_transform() const {
	if (!eng) {
		return Transform3D();
	}
	float p[3], q[4];
	bwa_get_listener_pose(eng, p, q);
	const Quaternion g = from_room_orientation(Quaternion(q[0], q[1], q[2], q[3]));
	return Transform3D(Basis(g), from_room_position(Vector3(p[0], p[1], p[2])));
}

void BwaEngine::set_extra_listeners(const PackedVector3Array &positions) {
	if (!eng) {
		return;
	}
	const int n = MIN(positions.size(), BWA_EXTRA_LIS);
	float xyz[BWA_EXTRA_LIS * 3];
	for (int i = 0; i < n; i++) {
		const Vector3 p = to_room_position(positions[i]);
		xyz[i * 3 + 0] = (float)p.x;
		xyz[i * 3 + 1] = (float)p.y;
		xyz[i * 3 + 2] = (float)p.z;
	}
	bwa_set_extra_listeners(eng, xyz, (uint32_t)n);
}

void BwaEngine::set_pose_prediction(float lead_s) {
	if (eng) {
		bwa_set_pose_prediction(eng, lead_s);
	}
}

/* --- tracker --- */

int BwaEngine::tracker_connect(const String &server, const String &rigid_body_name, int id) {
	if (!eng) {
		return (int)BWA_ERR_STATE;
	}
	const CharString server_utf8 = server.utf8();
	const CharString name_utf8 = rigid_body_name.utf8();
	bwa_tracker_desc d = {};
	d.server = server.is_empty() ? nullptr : server_utf8.get_data();
	d.rigid_body_name = rigid_body_name.is_empty() ? nullptr : name_utf8.get_data();
	d.rigid_body_id = id;
	const bwa_result r = bwa_tracker_connect(eng, &d);
	if (r != BWA_OK) {
		UtilityFunctions::push_error(
				vformat("BwaEngine: tracker connect failed: %s", get_last_error()));
	}
	return (int)r;
}

void BwaEngine::tracker_disconnect() {
	if (eng) {
		bwa_tracker_disconnect(eng);
	}
}

BwaEngine::TrackerState BwaEngine::get_tracker_status() const {
	return eng ? (TrackerState)bwa_tracker_status(eng) : TRACKER_DISCONNECTED;
}

/* --- assets (cached by path, so the same clip on many sources loads once) --- */

bwa_sound BwaEngine::load_sound(const String &path, bool stream) {
	if (!eng || path.is_empty()) {
		return 0;
	}
	const String key = (stream ? String("s:") : String("m:")) + path;
	if (bwa_sound *hit = sounds.getptr(key)) {
		return *hit;
	}
	const CharString os = to_os_path(path).utf8();
	const bwa_sound snd = stream ? bwa_load_sound_streaming(eng, os.get_data())
								 : bwa_load_sound(eng, os.get_data());
	if (!snd) {
		UtilityFunctions::push_error(
				vformat("BwaEngine: could not load \"%s\": %s", path, get_last_error()));
		return 0;
	}
	sounds.insert(key, snd);
	return snd;
}

bwa_sound BwaEngine::load_ambisonic(const String &path, bool fuma) {
	if (!eng || path.is_empty()) {
		return 0;
	}
	const String key = (fuma ? String("f:") : String("a:")) + path;
	if (bwa_sound *hit = sounds.getptr(key)) {
		return *hit;
	}
	const CharString os = to_os_path(path).utf8();
	const bwa_sound snd = fuma ? bwa_load_fuma(eng, os.get_data()) : bwa_load_ambix(eng, os.get_data());
	if (!snd) {
		UtilityFunctions::push_error(
				vformat("BwaEngine: could not load soundfield \"%s\": %s", path, get_last_error()));
		return 0;
	}
	sounds.insert(key, snd);
	return snd;
}

void BwaEngine::unload_sound_path(const String &path) {
	if (!eng) {
		return;
	}
	/* One path can be cached under several keys (memory vs streamed vs ambisonic); drop
	 * every one, so "unload this file" means what it says. */
	for (const String prefix : { String("m:"), String("s:"), String("a:"), String("f:") }) {
		const String key = prefix + path;
		if (bwa_sound *hit = sounds.getptr(key)) {
			bwa_unload_sound(eng, *hit);
			sounds.erase(key);
		}
	}
}

int64_t BwaEngine::sound_get_frames(const String &path) {
	const bwa_sound snd = load_sound(path, false);
	return snd ? (int64_t)bwa_sound_get_frames(eng, snd) : 0;
}

int BwaEngine::sound_get_channels(const String &path) {
	const bwa_sound snd = load_sound(path, false);
	return snd ? (int)bwa_sound_get_channels(eng, snd) : 0;
}

void BwaEngine::play_oneshot(const String &path, const Vector3 &godot_pos, float gain) {
	const bwa_sound snd = load_sound(path, false);
	if (!snd) {
		return;
	}
	const Vector3 p = to_room_position(godot_pos);
	bwa_play_oneshot(eng, snd, (float)p.x, (float)p.y, (float)p.z, gain);
}

/* --- global mix --- */

void BwaEngine::set_master_gain(float g) {
	master_gain = g;
	if (eng) {
		bwa_set_master_gain(eng, g);
	}
}

void BwaEngine::set_paused(bool p) {
	paused = p;
	if (eng) {
		bwa_set_paused(eng, p);
	}
}

void BwaEngine::group_set_gain(int group, float linear) {
	if (eng) {
		bwa_group_set_gain(eng, (uint32_t)group, linear);
	}
}

void BwaEngine::group_set_paused(int group, bool p) {
	if (eng) {
		bwa_group_set_paused(eng, (uint32_t)group, p);
	}
}

void BwaEngine::reverb_set_gain(float linear) {
	reverb_gain = linear;
	if (eng) {
		bwa_set_reverb_gain(eng, linear);
	}
}

void BwaEngine::early_reflections_set_gain(float linear) {
	er_gain = linear;
	if (eng) {
		bwa_set_early_reflections_gain(eng, linear);
	}
}

/* --- rendering A/B --- */

#define BWA_LIVE_SETTER(field, type, call)      \
	void BwaEngine::set_##field(type v) {       \
		field = v;                              \
		if (eng) {                              \
			call;                               \
		}                                       \
	}

BWA_LIVE_SETTER(panner, Panner, bwa_set_panner(eng, (bwa_panner)v))
BWA_LIVE_SETTER(dual_band, bool, bwa_set_dual_band(eng, v))
BWA_LIVE_SETTER(spread_mode, SpreadMode, bwa_set_spread_mode(eng, (bwa_spread_mode)v))
BWA_LIVE_SETTER(decorrelation, bool, bwa_set_decorrelation(eng, v))
BWA_LIVE_SETTER(near_spread, float, bwa_set_near_spread(eng, v))
BWA_LIVE_SETTER(speed_of_sound, float, bwa_set_speed_of_sound(eng, v))
BWA_LIVE_SETTER(max_re, bool, bwa_set_max_re(eng, v))
BWA_LIVE_SETTER(max_re_split, bool, bwa_set_max_re_split(eng, v))
BWA_LIVE_SETTER(bed_renderer, BedRenderer, bwa_set_bed_renderer(eng, (bwa_bed_renderer)v))
BWA_LIVE_SETTER(tracked_room_eq, bool, bwa_set_tracked_room_eq(eng, v))
BWA_LIVE_SETTER(limiter, bool, bwa_set_limiter(eng, v))

#undef BWA_LIVE_SETTER

/* Hand-written (not BWA_LIVE_SETTER): the property kept its name across the 0.10 dB -> linear unit
 * change, so a pre-0.10 scene replays its authored dB value (always <= 0) here — which the engine
 * contract silently ignores, leaving the cached getter claiming a ceiling the engine isn't running.
 * Convert instead and warn, so old scenes migrate loudly and re-save in linear; -60 dB (the old
 * clamp floor) maps onto the new 0.001 hint floor. */
void BwaEngine::set_limiter_ceiling(float v) {
	if (v <= 0.f) {
		const float lin = powf(10.f, v / 20.f);
		UtilityFunctions::push_warning(vformat(
				"BwaEngine: limiter_ceiling %.2f reads as a pre-0.10 dB value; converting to linear %.3f (re-save the scene)",
				v, lin));
		v = lin < 0.001f ? 0.001f : lin;
	}
	limiter_ceiling = v;
	if (eng) {
		bwa_set_limiter_ceiling(eng, v);
	}
}

/* Headphone correction EQ (bwa_load_headphone_eq): an AutoEq ParametricEQ.txt applied to the
 * final headphone stereo (binaural/cave_sim/cave_both's tap; inert in cave). Load-class — file
 * I/O, so call it from a load path, not per frame. Empty path clears. Returns the bwa_result
 * (0 = OK; on failure get_last_error has the reason and the previous EQ is kept). The engine is
 * created at _ready, so load AFTER the node is in the tree; a rebuild-free live toggle rides
 * set_headphone_eq (ramped, default on). */
int BwaEngine::load_headphone_eq(const String &path) {
	if (!eng) {
		return (int)BWA_ERR_STATE;
	}
	if (path.is_empty()) {
		return (int)bwa_load_headphone_eq(eng, nullptr);
	}
	return (int)bwa_load_headphone_eq(eng, path.utf8().get_data());
}

void BwaEngine::set_headphone_eq(bool on) {
	headphone_eq_on = on;
	if (eng) {
		bwa_set_headphone_eq(eng, on);
	}
}

/* The three FDN decay parameters are live: stage into the desc (a pre-start edit rides
 * bwa_fdn_config at start) and forward the full triple to bwa_fdn_set_decay while running — the
 * tail keeps ringing, only its slope changes. */
void BwaEngine::set_fdn_rt60_low(float v) {
	fdn.rt60_low_s = v;
	if (eng) {
		bwa_fdn_set_decay(eng, fdn.rt60_low_s, fdn.rt60_high_s, fdn.xover_hz);
	}
}

void BwaEngine::set_fdn_rt60_high(float v) {
	fdn.rt60_high_s = v;
	if (eng) {
		bwa_fdn_set_decay(eng, fdn.rt60_low_s, fdn.rt60_high_s, fdn.xover_hz);
	}
}

void BwaEngine::set_fdn_xover_hz(float v) {
	fdn.xover_hz = v;
	if (eng) {
		bwa_fdn_set_decay(eng, fdn.rt60_low_s, fdn.rt60_high_s, fdn.xover_hz);
	}
}

void BwaEngine::fdn_set_decay(float rt60_low_s, float rt60_high_s, float xover_hz) {
	if (rt60_low_s > 0.0f) {
		fdn.rt60_low_s = rt60_low_s;
	}
	if (rt60_high_s > 0.0f) {
		fdn.rt60_high_s = rt60_high_s;
	}
	if (xover_hz > 0.0f) {
		fdn.xover_hz = xover_hz;
	}
	if (eng) {
		bwa_fdn_set_decay(eng, fdn.rt60_low_s, fdn.rt60_high_s, fdn.xover_hz);
	}
}

void BwaEngine::set_fdn_decay_dir(const Vector3 &v) {
	/* A DIRECTION, so it takes the registration basis only — no translation. */
	const Vector3 d = to_room_direction(v);
	fdn.decay_dir[0] = (float)d.x;
	fdn.decay_dir[1] = (float)d.y;
	fdn.decay_dir[2] = (float)d.z;
}

Vector3 BwaEngine::get_fdn_decay_dir() const {
	return Vector3(fdn.decay_dir[0], fdn.decay_dir[1], fdn.decay_dir[2]);
}

/* --- materials + scene geometry --- */

int BwaEngine::material_preset(Material preset) {
	return eng ? (int)bwa_material_preset(eng, (bwa_material_type)preset) : 0;
}

int BwaEngine::material_define(
		const Vector3 &absorption, float scattering, const Vector3 &transmission) {
	if (!eng) {
		return 0;
	}
	const float a[3] = { (float)absorption.x, (float)absorption.y, (float)absorption.z };
	const float t[3] = { (float)transmission.x, (float)transmission.y, (float)transmission.z };
	return (int)bwa_material_define(eng, a, scattering, t);
}

void BwaEngine::material_release(int token) {
	if (eng) {
		bwa_material_release(eng, (bwa_material)token);
	}
}

void BwaEngine::scene_set_box(float w, float h, float d, const PackedInt32Array &faces) {
	if (!eng) {
		return;
	}
	bwa_material f[6] = {};
	for (int i = 0; i < MIN(faces.size(), 6); i++) {
		f[i] = (bwa_material)faces[i];
	}
	bwa_scene_set_box(eng, w, h, d, f);
}

void BwaEngine::scene_set_ground(float y, int material, bool pressure_release) {
	if (!eng) {
		return;
	}
	/* y is a Godot-world height: the plane sits AT a position, so the registration transform
	 * applies (the box escapes this only because it takes sizes, not positions). */
	const float room_y = (float)to_room_position(Vector3(0, y, 0)).y;
	bwa_scene_set_ground(eng, room_y, (bwa_material)material, pressure_release);
}

void BwaEngine::scene_set_pressure_release(int face_mask) {
	if (eng) {
		bwa_scene_set_pressure_release(eng, (uint32_t)face_mask);
	}
}

void BwaEngine::scene_set_mesh(const PackedVector3Array &verts, const PackedInt32Array &tris,
		const PackedInt32Array &tri_materials) {
	if (!eng || verts.is_empty() || tris.is_empty()) {
		return;
	}
	const int ntris = tris.size() / 3;
	std::vector<float> v(verts.size() * 3);
	for (int i = 0; i < verts.size(); i++) {
		const Vector3 p = to_room_position(verts[i]);
		v[i * 3 + 0] = (float)p.x;
		v[i * 3 + 1] = (float)p.y;
		v[i * 3 + 2] = (float)p.z;
	}
	/* One token per TRIANGLE. A short array is padded with the default rather than read
	 * past its end — a mesh with no per-face materials is a normal thing to pass. */
	std::vector<bwa_material> mats((size_t)ntris, 0);
	for (int i = 0; i < MIN(tri_materials.size(), ntris); i++) {
		mats[(size_t)i] = (bwa_material)tri_materials[i];
	}
	bwa_scene_set_mesh_mat(eng, v.data(), verts.size(), tris.ptr(), ntris, mats.data());
}

int BwaEngine::scene_add_dynamic_mesh(
		const PackedVector3Array &verts, const PackedInt32Array &tris, int material) {
	if (!eng || verts.is_empty() || tris.is_empty()) {
		return -1;
	}
	/* Dynamic geometry is in the MOVER's local space, so it takes no registration — only
	 * the placement (scene_set_dynamic_transform) crosses the seam. */
	std::vector<float> v(verts.size() * 3);
	for (int i = 0; i < verts.size(); i++) {
		v[i * 3 + 0] = (float)verts[i].x;
		v[i * 3 + 1] = (float)verts[i].y;
		v[i * 3 + 2] = (float)verts[i].z;
	}
	return bwa_scene_add_dynamic_mesh(
			eng, v.data(), verts.size(), tris.ptr(), tris.size() / 3, (bwa_material)material);
}

void BwaEngine::scene_set_dynamic_transform(
		int handle, const Vector3 &godot_pos, const Quaternion &rot) {
	if (!eng) {
		return;
	}
	const Vector3 p = to_room_position(godot_pos);
	/* Registration ONLY — deliberately not to_room_orientation. That helper is the FACING
	 * conversion (its rotY(pi) makes a node's -Z forward read as room +Z), which is correct
	 * for things that face: the listener, a directivity axis. A mesh transform is not a
	 * facing: its local vertices go to the core untouched, so the placement must satisfy
	 * p_room = Reg * T_godot * p_local exactly. Routing it through the facing helper spins
	 * every dynamic mesh 180 degrees about Y relative to its visual node — invisible on the
	 * demos' centred quads (symmetric under that turn), wrong for anything asymmetric. */
	const Quaternion q = Quaternion((registration.basis * Basis(rot)).orthonormalized());
	bwa_scene_set_dynamic_transform(
			eng, handle, (float)p.x, (float)p.y, (float)p.z, (float)q.x, (float)q.y, (float)q.z,
			(float)q.w);
}

void BwaEngine::scene_remove_dynamic_mesh(int handle) {
	if (eng) {
		bwa_scene_remove_dynamic_mesh(eng, handle);
	}
}

/* --- clock / scheduling --- */

int64_t BwaEngine::get_dsp_time() const { return eng ? (int64_t)bwa_get_dsp_time(eng) : 0; }

Dictionary BwaEngine::get_clock() const {
	Dictionary d;
	uint64_t sample = 0, host = 0;
	const bool ok = eng && bwa_get_clock(eng, &sample, &host);
	d["valid"] = ok;
	d["dsp_sample"] = (int64_t)sample;
	d["host_time_ns"] = (int64_t)host;
	return d;
}

int BwaEngine::get_output_latency() const {
	return eng ? (int)bwa_get_output_latency(eng) : 0;
}

/* --- diagnostics --- */

void BwaEngine::set_test_signal(int channel, TestKind kind, float gain) {
	if (eng) {
		bwa_set_test_signal(eng, (uint32_t)channel, (bwa_test_kind)kind, gain);
	}
}

String BwaEngine::get_audio_backend() const {
	if (!eng) {
		return String("none");
	}
	const char *b = bwa_get_audio_backend(eng);
	return b ? String(b) : String("none");
}

String BwaEngine::get_last_error() const {
	if (!eng) {
		return String();
	}
	const char *e = bwa_last_error(eng);
	return e ? String(e) : String();
}

int BwaEngine::get_channel_count() const { return eng ? (int)bwa_get_channel_count(eng) : 0; }
int BwaEngine::get_resolved_sample_rate() const { return eng ? (int)bwa_get_sample_rate(eng) : 0; }
int BwaEngine::get_resolved_block_size() const { return eng ? (int)bwa_get_block_size(eng) : 0; }
int BwaEngine::get_active_voices() const { return eng ? (int)bwa_get_active_voices(eng) : 0; }

PackedFloat32Array BwaEngine::get_bus_levels() const {
	PackedFloat32Array out;
	if (!eng) {
		return out;
	}
	/* Sized from the layout's speaker count, never a hard-coded 26. */
	const int n = (int)bwa_get_channel_count(eng);
	out.resize(n);
	bwa_get_bus_levels(eng, out.ptrw(), (uint32_t)n);
	return out;
}

PackedVector3Array BwaEngine::get_speakers() const {
	PackedVector3Array out;
	if (!eng) {
		return out;
	}
	const uint32_t n = bwa_get_speakers(eng, nullptr, 0);
	std::vector<float> xyz(n * 3);
	bwa_get_speakers(eng, xyz.data(), n);
	out.resize((int)n);
	for (uint32_t i = 0; i < n; i++) {
		out[(int)i] = Vector3(xyz[i * 3 + 0], xyz[i * 3 + 1], xyz[i * 3 + 2]);
	}
	return out;
}

PackedFloat32Array BwaEngine::render_block() {
	PackedFloat32Array out;
	if (!eng) {
		return out;
	}
	uint32_t channels = 0, nframes = 0;
	const float *p = bwa_render_block(eng, &channels, &nframes);
	if (!p) {
		return out; // not started, or the sink is not MANUAL
	}
	out.resize((int)(channels * nframes));
	memcpy(out.ptrw(), p, (size_t)channels * nframes * sizeof(float));
	return out;
}

/* --- static helpers --- */

int BwaEngine::get_asio_driver_count() { return (int)bwa_get_asio_driver_count(); }

String BwaEngine::get_asio_driver_name(int index) {
	char buf[256];
	if (!bwa_get_asio_driver_name((uint32_t)index, buf, sizeof buf)) {
		return String();
	}
	return String::utf8(buf);    /* the ABI speaks UTF-8 (asio_sink converts from the registry's ACP) */
}

int BwaEngine::get_version() { return (int)bwa_get_version(); }

Vector3 BwaEngine::room_ahead() {
	return Vector3(BWA_ROOM_AHEAD[0], BWA_ROOM_AHEAD[1], BWA_ROOM_AHEAD[2]);
}
Vector3 BwaEngine::room_up() { return Vector3(BWA_ROOM_UP[0], BWA_ROOM_UP[1], BWA_ROOM_UP[2]); }
Vector3 BwaEngine::room_right() {
	return Vector3(BWA_ROOM_RIGHT[0], BWA_ROOM_RIGHT[1], BWA_ROOM_RIGHT[2]);
}

/* --- source registry --- */

void BwaEngine::register_source(BwaSource *s) {
	for (BwaSource *x : sources) {
		if (x == s) {
			return;
		}
	}
	sources.push_back(s);
}

void BwaEngine::unregister_source(BwaSource *s) {
	for (size_t i = 0; i < sources.size(); i++) {
		if (sources[i] == s) {
			sources.erase(sources.begin() + (long)i);
			return;
		}
	}
}

/* The settings that fail QUIETLY are the ones worth surfacing. None of these are errors to
 * the core — it starts, it renders, it just sounds wrong — so the scene tree's own warning
 * marker is the right place for them. */
PackedStringArray BwaEngine::_get_configuration_warnings() const {
	PackedStringArray w;

	if (refl.enabled && fdn.enabled) {
		w.push_back("The Steam reflection bed and the FDN share ONE reverb tap; enable exactly "
					"one. With both on, only one takes it.");
	}
	if (refl.enabled) {
		/* The engine warns once at runtime too, but by then the double-rendered reflections
		 * are already in the mix and sound merely "roomy". */
		w.push_back("The Steam reflection bed already contains early reflections. Do not also "
					"enable early_reflections on sources — they render twice.");
	}
	if (feed_listener && listener_path.is_empty()) {
		w.push_back("Feed Listener is on but no listener node is set, so the listener never "
					"moves from the origin. Point it at the head/camera, or turn it off to let "
					"the engine read NatNet itself.");
	}
	if (!feed_listener && profile != PROFILE_CAVE) {
		w.push_back("The headphone profiles (Binaural/CaveSim/CaveBoth) need a head POSE, but "
					"Feed Listener is off — so the pose comes from a tracker, which must be "
					"connected for the render to follow the head.");
	}
	if (!layout_path.is_empty()) {
		/* A failed layout load is not fatal at create: the core falls back to the 26-speaker
		 * grid, quietly changing the channel count on a smaller rig. bwa_start does refuse
		 * the fallback, so this is about catching it before a run rather than after. */
		if (!FileAccess::file_exists(layout_path)) {
			w.push_back(vformat("Layout file not found: \"%s\". bwa_start will refuse to run "
								"rather than fall back to the 26-speaker default grid.",
					layout_path));
		} else if (layout_path.begins_with("res://")) {
			w.push_back("The core opens files by OS path, not through Godot's virtual "
						"filesystem. A res:// layout works in the editor but will not exist "
						"inside an exported .pck — ship it beside the executable or stage it "
						"into user://.");
		}
	}
	if (enable_pathing && !BwaRoomBox::active() && BwaAcousticGeometry::registry().empty()) {
		w.push_back("Pathing is enabled but the scene has no acoustic geometry, so there is "
					"nothing for sound to path around.");
	}
	if (sink == SINK_MANUAL) {
		w.push_back("The MANUAL sink creates no device and no audio thread: nothing is heard "
					"until you call render_block() yourself. That is the offline/golden-render "
					"path, not a playable one.");
	}
	return w;
}

void BwaEngine::_bind_methods() {
	/* Two macros rather than one variadic: MSVC's default preprocessor has no __VA_OPT__,
	 * and forcing /Zc:preprocessor across the target to save a character is not a trade
	 * worth making. M0 takes no arguments, M takes at least one. */
#define M0(name) ClassDB::bind_method(D_METHOD(#name), &BwaEngine::name)
#define M(name, ...) ClassDB::bind_method(D_METHOD(#name, __VA_ARGS__), &BwaEngine::name)

	M(set_profile, "profile"); M0(get_profile);
	M(set_sink, "sink"); M0(get_sink);
	M(set_layout_path, "path"); M0(get_layout_path);
	M(set_asio_driver, "name"); M0(get_asio_driver);
	M(set_sample_rate, "hz"); M0(get_sample_rate);
	M(set_block_size, "frames"); M0(get_block_size);
	M(set_bed_decoder, "decoder"); M0(get_bed_decoder);
	M(set_embree, "on"); M0(get_embree);
	M(set_enable_pathing, "on"); M0(get_enable_pathing);

	M(set_enable_reflections, "on"); M0(get_enable_reflections);
	M(set_reflections_ir_seconds, "seconds"); M0(get_reflections_ir_seconds);
	M(set_reflections_order, "order"); M0(get_reflections_order);
	M(set_reflections_rays, "rays"); M0(get_reflections_rays);
	M(set_reflections_bounces, "bounces"); M0(get_reflections_bounces);
	M(set_reflections_bake, "on"); M0(get_reflections_bake);
	M(set_enable_fdn, "on"); M0(get_enable_fdn);
	M(set_fdn_rt60_low, "seconds"); M0(get_fdn_rt60_low);
	M(set_fdn_rt60_high, "seconds"); M0(get_fdn_rt60_high);
	M(set_fdn_xover_hz, "hz"); M0(get_fdn_xover_hz);
	M(set_fdn_decay_dir, "direction"); M0(get_fdn_decay_dir);
	M(set_fdn_decay_factor, "factor"); M0(get_fdn_decay_factor);

	M(set_registration, "xform"); M0(get_registration);
	M(to_room_position, "v"); M(to_room_direction, "v");
	M(to_room_orientation, "basis"); M(to_room_yaw, "basis");
	M(from_room_position, "v"); M(from_room_orientation, "quaternion");

	M(set_listener, "path"); M0(get_listener);
	M(set_feed_listener, "enabled"); M0(get_feed_listener);
	M(set_listener_pose, "position", "rotation");
	M0(get_listener_transform);
	M(set_extra_listeners, "positions");
	M(set_pose_prediction, "lead_s");

	M(tracker_connect, "server", "rigid_body_name", "rigid_body_id");
	M0(tracker_disconnect);
	M0(get_tracker_status);

	M(unload_sound_path, "path");
	M(sound_get_frames, "path");
	M(sound_get_channels, "path");
	M(play_oneshot, "path", "position", "gain");

	M(set_master_gain, "linear"); M0(get_master_gain);
	M(set_paused, "paused"); M0(get_paused);
	M(group_set_gain, "group", "linear");
	M(group_set_paused, "group", "paused");
	M(reverb_set_gain, "linear"); M0(get_reverb_gain);
	M(early_reflections_set_gain, "linear"); M0(get_early_reflections_gain);
	M(fdn_set_decay, "rt60_low_s", "rt60_high_s", "xover_hz");

	M(set_panner, "panner"); M0(get_panner);
	M(set_dual_band, "on"); M0(get_dual_band);
	M(set_spread_mode, "mode"); M0(get_spread_mode);
	M(set_decorrelation, "on"); M0(get_decorrelation);
	M(set_near_spread, "radius_m"); M0(get_near_spread);
	M(set_speed_of_sound, "meters_per_sec"); M0(get_speed_of_sound);
	M(set_max_re, "on"); M0(get_max_re);
	M(set_max_re_split, "on"); M0(get_max_re_split);
	M(set_bed_renderer, "renderer"); M0(get_bed_renderer);
	M(set_tracked_room_eq, "on"); M0(get_tracked_room_eq);
	M(set_limiter, "on"); M0(get_limiter);
	M(set_limiter_ceiling, "linear"); M0(get_limiter_ceiling);
	M(load_headphone_eq, "path");
	M(set_headphone_eq, "on"); M0(get_headphone_eq);

	M(material_preset, "preset");
	M(material_define, "absorption", "scattering", "transmission");
	M(material_release, "token");
	M(scene_set_box, "width", "height", "depth", "faces");
	M(scene_set_ground, "y", "material", "pressure_release");
	M(scene_set_pressure_release, "face_mask");
	M(scene_set_mesh, "verts", "tris", "tri_materials");
	M(scene_add_dynamic_mesh, "verts", "tris", "material");
	M(scene_set_dynamic_transform, "handle", "position", "rotation");
	M(scene_remove_dynamic_mesh, "handle");

	M0(get_dsp_time); M0(get_clock); M0(get_output_latency);
	M(set_test_signal, "channel", "kind", "gain");
	M0(is_running); M0(get_audio_backend); M0(get_last_error);
	M0(get_channel_count); M0(get_resolved_sample_rate); M0(get_resolved_block_size);
	M0(get_active_voices); M0(get_bus_levels); M0(get_speakers); M0(render_block);
	M0(get_generation);
#undef M
#undef M0

	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_asio_driver_count"), &BwaEngine::get_asio_driver_count);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_asio_driver_name", "index"),
			&BwaEngine::get_asio_driver_name);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_version"), &BwaEngine::get_version);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_ahead"), &BwaEngine::room_ahead);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_up"), &BwaEngine::room_up);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_right"), &BwaEngine::room_right);

	ADD_GROUP("Device", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "profile", PROPERTY_HINT_ENUM, "Cave,Binaural,CaveSim,CaveBoth"),
			"set_profile", "get_profile");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sink", PROPERTY_HINT_ENUM, "Auto,ASIO,Null,Manual"),
			"set_sink", "get_sink");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "asio_driver"), "set_asio_driver", "get_asio_driver");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sample_rate"), "set_sample_rate", "get_sample_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "block_size"), "set_block_size", "get_block_size");

	ADD_GROUP("Room", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layout_path", PROPERTY_HINT_FILE, "*.json"),
			"set_layout_path", "get_layout_path");
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "registration"), "set_registration",
			"get_registration");

	ADD_GROUP("Listener", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "listener", PROPERTY_HINT_NODE_PATH_VALID_TYPES,
						 "Node3D"),
			"set_listener", "get_listener");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "feed_listener"), "set_feed_listener",
			"get_feed_listener");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "panner", PROPERTY_HINT_ENUM, "DBAP,SPCAP,VBAP"),
			"set_panner", "get_panner");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dual_band"), "set_dual_band", "get_dual_band");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spread_mode", PROPERTY_HINT_ENUM, "Lobe,MDAP,Spectral"),
			"set_spread_mode", "get_spread_mode");
	ADD_PROPERTY(
			PropertyInfo(Variant::BOOL, "decorrelation"), "set_decorrelation", "get_decorrelation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_spread", PROPERTY_HINT_RANGE, "0,4,0.05"),
			"set_near_spread", "get_near_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed_of_sound", PROPERTY_HINT_RANGE,
						 "30,20000,1,or_greater"),
			"set_speed_of_sound", "get_speed_of_sound");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bed_decoder", PROPERTY_HINT_ENUM, "AllRAD,EPAD"),
			"set_bed_decoder", "get_bed_decoder");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bed_renderer", PROPERTY_HINT_ENUM, "Matrix,Parametric"),
			"set_bed_renderer", "get_bed_renderer");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "max_re"), "set_max_re", "get_max_re");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "max_re_split"), "set_max_re_split",
			"get_max_re_split");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tracked_room_eq"), "set_tracked_room_eq",
			"get_tracked_room_eq");

	ADD_GROUP("Output", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "master_gain", PROPERTY_HINT_RANGE, "0,2,0.01"),
			"set_master_gain", "get_master_gain");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "limiter"), "set_limiter", "get_limiter");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "limiter_ceiling", PROPERTY_HINT_RANGE, "0.001,1,0.001"),
			"set_limiter_ceiling", "get_limiter_ceiling");

	ADD_GROUP("Reverb", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enable_reflections"), "set_enable_reflections",
			"get_enable_reflections");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflections_ir_seconds"),
			"set_reflections_ir_seconds", "get_reflections_ir_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflections_order"), "set_reflections_order",
			"get_reflections_order");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflections_rays"), "set_reflections_rays",
			"get_reflections_rays");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflections_bounces"), "set_reflections_bounces",
			"get_reflections_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reflections_bake"), "set_reflections_bake",
			"get_reflections_bake");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enable_fdn"), "set_enable_fdn", "get_enable_fdn");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fdn_rt60_low"), "set_fdn_rt60_low",
			"get_fdn_rt60_low");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fdn_rt60_high"), "set_fdn_rt60_high",
			"get_fdn_rt60_high");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fdn_xover_hz"), "set_fdn_xover_hz",
			"get_fdn_xover_hz");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "fdn_decay_dir"), "set_fdn_decay_dir",
			"get_fdn_decay_dir");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fdn_decay_factor"), "set_fdn_decay_factor",
			"get_fdn_decay_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reverb_gain", PROPERTY_HINT_RANGE, "0,4,0.01"),
			"reverb_set_gain", "get_reverb_gain");
	ADD_PROPERTY(
			PropertyInfo(Variant::FLOAT, "early_reflections_gain", PROPERTY_HINT_RANGE, "0,4,0.01"),
			"early_reflections_set_gain", "get_early_reflections_gain");

	ADD_GROUP("Acoustics", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "embree"), "set_embree", "get_embree");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enable_pathing"), "set_enable_pathing",
			"get_enable_pathing");

	BIND_ENUM_CONSTANT(PROFILE_CAVE); BIND_ENUM_CONSTANT(PROFILE_BINAURAL);
	BIND_ENUM_CONSTANT(PROFILE_CAVE_SIM); BIND_ENUM_CONSTANT(PROFILE_CAVE_BOTH);
	BIND_ENUM_CONSTANT(SINK_AUTO); BIND_ENUM_CONSTANT(SINK_ASIO);
	BIND_ENUM_CONSTANT(SINK_NULL); BIND_ENUM_CONSTANT(SINK_MANUAL);
	BIND_ENUM_CONSTANT(DECODE_ALLRAD); BIND_ENUM_CONSTANT(DECODE_EPAD);
	BIND_ENUM_CONSTANT(PAN_DBAP); BIND_ENUM_CONSTANT(PAN_SPCAP); BIND_ENUM_CONSTANT(PAN_VBAP);
	BIND_ENUM_CONSTANT(SPREAD_LOBE); BIND_ENUM_CONSTANT(SPREAD_MDAP);
	BIND_ENUM_CONSTANT(SPREAD_SPECTRAL);
	BIND_ENUM_CONSTANT(BED_MATRIX); BIND_ENUM_CONSTANT(BED_PARAMETRIC);
	BIND_ENUM_CONSTANT(TEST_OFF); BIND_ENUM_CONSTANT(TEST_SINE); BIND_ENUM_CONSTANT(TEST_NOISE);
	BIND_ENUM_CONSTANT(MAT_GENERIC); BIND_ENUM_CONSTANT(MAT_BRICK); BIND_ENUM_CONSTANT(MAT_CONCRETE);
	BIND_ENUM_CONSTANT(MAT_CERAMIC); BIND_ENUM_CONSTANT(MAT_GRAVEL); BIND_ENUM_CONSTANT(MAT_CARPET);
	BIND_ENUM_CONSTANT(MAT_GLASS); BIND_ENUM_CONSTANT(MAT_PLASTER); BIND_ENUM_CONSTANT(MAT_WOOD);
	BIND_ENUM_CONSTANT(MAT_METAL); BIND_ENUM_CONSTANT(MAT_ROCK);
	BIND_ENUM_CONSTANT(TRACKER_DISCONNECTED); BIND_ENUM_CONSTANT(TRACKER_NO_DATA);
	BIND_ENUM_CONSTANT(TRACKER_NO_BODY); BIND_ENUM_CONSTANT(TRACKER_LIVE);
}
