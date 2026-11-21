#include "bwa_engine_node.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_bed_node.h"
#include "bwa_geometry.h"
#include "bwa_material.h"
#include "bwa_room.h"
#include "bwa_source_base.h"

#include <algorithm>
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
	fallback_taken = false; /* before every return below: a re-entered node starts clean */
	if (Engine::get_singleton()->is_editor_hint()) {
		editor_watch = true; // ...but do keep the room_origin warnings current
		set_process(true);
		return; // never grab an audio device just because a scene was opened
	}
	if (singleton && singleton != this) {
		inert = true;
		UtilityFunctions::push_error(
				"BwaEngine: a second engine node entered the tree; only one may run. This one is "
				"inert and the first keeps running with its own layout and room_origin. If the "
				"rig is an Autoload, remove the BwaEngine from the scene.");
		return;
	}

	/* Before anything reads the registration: the static scene below bakes through it. */
	sync_room_origin();

	const uint32_t dll = bwa_get_version();
	if (dll != BWA_VERSION) {
		UtilityFunctions::push_warning(vformat(
				"BwaEngine: bw_audio.dll is version %d.%d.%d but the extension was built against "
				"%d.%d.%d - rebuild the binding against the DLL you are shipping.",
				(dll >> 16) & 0xff, (dll >> 8) & 0xff, dll & 0xff, BWA_VERSION_MAJOR,
				BWA_VERSION_MINOR, BWA_VERSION_PATCH));
	}

	/* The desk fallback, decided here: before bwa_create, from the device lists. Never after a
	 * failed bwa_start: a device that is present and will not open is a rig fault, and the rig
	 * must say so. */
	Profile use_profile = profile;
	Sink use_sink = sink;
	String use_device = device;
	int use_flags = sink_flags;
	/* The gate is the editor, not is_debug_build(): see the header. */
	const bool fallback_allowed = fallback_profile != FALLBACK_NONE
			&& (!fallback_only_in_editor || OS::get_singleton()->has_feature("editor"));
	if (fallback_allowed && !is_device_present(profile, sink, device)) {
		use_profile = (Profile)fallback_profile;
		use_sink = fallback_sink;
		use_device = fallback_device;
		use_flags = fallback_sink_flags;
		fallback_taken = true;
		active_profile = use_profile;
		UtilityFunctions::push_warning(vformat(
				"BwaEngine: DESK FALLBACK: the primary device is not on this machine (profile %d, "
				"sink %d, device %s). Running profile %d on sink %d, device %s instead. "
				"is_using_fallback() is true.",
				(int)profile, (int)sink,
				device.is_empty() ? String("(default)") : String("'") + device + "'",
				(int)use_profile, (int)use_sink,
				use_device.is_empty() ? String("(default)") : String("'") + use_device + "'"));
	}

	/* Binaural drives no speakers: the layout only places the virtual speakers the diffuse bed
	 * decodes through, and the default grid serves that. The refusal exists so the ARRAY never
	 * pans over the wrong room, so a desk scene must not go silent for want of a file it does not
	 * need, whether Binaural is the profile or the fallback. A file that exists still has to
	 * load. Cave, CaveSim and CaveBoth pan over the layout and keep the refusal. */
	bool pass_layout = !layout_path.is_empty();
	if (pass_layout && use_profile == PROFILE_BINAURAL && !FileAccess::file_exists(layout_path)) {
		UtilityFunctions::print(vformat(
				"BwaEngine: Binaural: no layout at \"%s\", so the diffuse bed decodes through the "
				"built-in default grid. The array profiles need the file.", layout_path));
		pass_layout = false;
	}

	const CharString layout_utf8 = to_os_path(layout_path).utf8();
	const CharString device_utf8 = use_device.utf8();

	bwa_desc cfg = {};
	cfg.profile = (bwa_profile)use_profile;
	cfg.layout_path = pass_layout ? layout_utf8.get_data() : nullptr;
	cfg.hrtf_path = nullptr;
	cfg.sample_rate = (uint32_t)sample_rate;
	cfg.block_size = (uint32_t)block_size;
	cfg.sink = (bwa_sink_type)use_sink;
	cfg.device = use_device.is_empty() ? nullptr : device_utf8.get_data();
	cfg.sink_flags = (uint32_t)use_flags;
	cfg.embree = embree;
	cfg.enable_pathing = enable_pathing;
	cfg.bed_decoder = (bwa_bed_decoder)bed_decoder;

	eng = bwa_create(&cfg);
	if (!eng) {
		UtilityFunctions::push_error("BwaEngine: bwa_create failed");
		fallback_taken = false; /* a fallback that did not start is not in use */
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
		fallback_taken = false;
		return;
	}

	singleton = this;

	/* The fallback was set and allowed, the device lists said the primary is here, and AUTO still
	 * ended on the offline sink: the device is listed and did not open. The check cannot see
	 * that, so the scene is silent, and that must not pass quietly. */
	if (fallback_allowed && !fallback_taken && sink != SINK_NULL && sink != SINK_MANUAL
			&& bwa_get_sink_type(eng) == BWA_SINK_NULL) {
		const char *why = bwa_last_error(eng);
		UtilityFunctions::push_error(vformat(
				"BwaEngine: NO AUDIO, and the desk fallback did NOT trigger: the primary device "
				"(profile %d, sink %d, device %s) is listed on this machine but did not open, so "
				"the engine runs the silent offline sink. Reason: %s. The check reads device "
				"lists: an installed driver with its hardware unplugged, too few channels, a "
				"busy device or a wrong rate all read as present. On the rig: fix the device. At "
				"a desk: set profile to %s by hand for this machine.",
				(int)profile, (int)sink,
				device.is_empty() ? String("(default)") : String("'") + device + "'",
				why ? String(why) : String("none given"),
				fallback_profile == FALLBACK_BINAURAL ? "Binaural" : "CaveSim"));
	}

	/* The other silent ending: the fallback was taken and IT did not open (AUTO with no output on
	 * this machine, or a fallback_device nobody has). The start line would say "null" and nothing
	 * else, after a warning that promised the fallback. */
	if (fallback_taken && use_sink != SINK_NULL && use_sink != SINK_MANUAL
			&& bwa_get_sink_type(eng) == BWA_SINK_NULL) {
		const char *why = bwa_last_error(eng);
		UtilityFunctions::push_error(vformat(
				"BwaEngine: DESK FALLBACK has NO AUDIO either: the fallback (profile %d, sink %d, "
				"device %s) did not open, so the engine runs the silent offline sink. Reason: %s. "
				"Connect headphones or another output, or set fallback_device to a device this "
				"machine has (BwaEngine.get_devices lists them; empty is the default output).",
				(int)use_profile, (int)use_sink,
				use_device.is_empty() ? String("(default)") : String("'") + use_device + "'",
				why ? String(why) : String("none given")));
	}

	/* Replay everything authored in the inspector before the engine existed. */
	bwa_set_master_gain(eng, master_gain);
	bwa_set_reverb_gain(eng, reverb_gain);
	bwa_set_early_reflections_gain(eng, er_gain);
	bwa_set_panner(eng, (bwa_panner)panner);
	bwa_set_dual_band(eng, dual_band);
	bwa_set_dual_band_cap(eng, dual_band_cap);
	bwa_set_spcap_focus(eng, spcap_focus, spcap_density);
	bwa_set_spread_mode(eng, (bwa_spread_mode)spread_mode);
	bwa_set_decorrelation(eng, decorrelation);
	bwa_set_near_spread(eng, near_spread);
	bwa_set_hole_spread(eng, hole_spread);
	bwa_set_speed_of_sound(eng, speed_of_sound);
	bwa_set_max_re(eng, max_re);
	bwa_set_max_re_split(eng, max_re_split);
	bwa_set_bed_renderer(eng, (bwa_bed_renderer)bed_renderer);
	bwa_set_tracked_room_eq(eng, tracked_room_eq);
	bwa_set_tracked_directivity(eng, tracked_directivity);
	bwa_set_tracked_align_guards(eng, tracked_align_dead_zone, tracked_align_slew_frames_per_s);
	bwa_set_tracked_align(eng, tracked_align);
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
		if (editor_watch) {
			editor_watch_origin();
		}
		return;
	}

	/* By index, not by iterator: push_frame can emit `finished`, and a user handler may add
	 * or free sources — freeing runs _exit_tree, which unregisters and mutates this vector.
	 * An index survives that (a freed source is removed BEFORE its memory dies, so it is
	 * never revisited; a same-frame addition is simply picked up or caught next frame). A
	 * range-for here is iterator invalidation; a copied snapshot is worse — it would keep a
	 * dangling pointer to anything the handler freed. */
	sync_room_origin(); // the frame every push below is expressed in
	refresh_static_scene(); // after the origin: a moved one re-bakes in this same frame

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

	/* ...and only THEN the event drains: bwa_commit runs the pass that FILLS both event rings, so
	 * draining before it reads a frame-old picture and delays every signal by a frame. */
	drain_events();

	/* The per-source POST-COMMIT pass, after both drains. An emitter's explicit-halt fallback reads
	 * bwa_source_is_playing here, and it has to read it AFTER the ended drain or one end reaches
	 * both feeds with nothing to say which arrived first. By index for the same reason the push
	 * loop above is: a `finished` handler is user code and may free a source. */
	for (size_t i = 0; i < sources.size(); i++) {
		sources[i]->post_commit();
	}
	/* Beds take the same pass, for the same reason: a bed's halt fallback reads
	 * bwa_bed_is_playing and must read it AFTER the ended drain. By index for the same reason
	 * too - a `finished` handler is user code and may free a bed. */
	for (size_t i = 0; i < beds.size(); i++) {
		beds[i]->post_commit();
	}
}

BwaSource *BwaEngine::source_for_handle(uint32_t h) const {
	for (BwaSource *s : sources) {
		if (s->native_handle() == h) {
			return s;
		}
	}
	return nullptr; /* normal, not an error: freed between the event and this drain */
}

BwaBed *BwaEngine::bed_for_handle(uint32_t h) const {
	for (BwaBed *b : beds) {
		if (b->native_handle() == h) {
			return b;
		}
	}
	return nullptr; /* normal: a source's handle, or a bed freed since the event */
}

void BwaEngine::drain_events() {
	ended_this_frame.clear();
	looped_this_frame.clear();
	if (!eng) {
		return;
	}
	uint32_t buf[64];
	uint64_t dropped = 0;

	for (;;) {
		const uint32_t n = bwa_poll_ended(eng, buf, 64, &dropped);
		for (uint32_t i = 0; i < n; ++i) {
			ended_this_frame.push_back((int64_t)buf[i]);
			BwaSource *s = source_for_handle(buf[i]);
			if (s) {
				s->notify_ended();
			} else if (BwaBed *b = bed_for_handle(buf[i])) {
				b->notify_ended();
			}
		}
		if (n < 64) {
			break; /* drained */
		}
	}
	if (dropped != ended_dropped) {
		ended_dropped = dropped;
		if (!ended_drop_warned) {
			ended_drop_warned = true;
			UtilityFunctions::push_warning(vformat(
					"BwaEngine: %d voice-completion events were dropped before anything read "
					"them, so that many \"finished\" signals never fired. The engine's ended ring "
					"is bounded and drops the OLDEST, so something stalled the frame. "
					"get_ended_events_dropped() carries the running total (this warns once).",
					(int64_t)dropped));
		}
	}

	for (;;) {
		const uint32_t n = bwa_poll_looped(eng, buf, 64, &dropped);
		for (uint32_t i = 0; i < n; ++i) {
			looped_this_frame.push_back((int64_t)buf[i]);
			BwaSource *s = source_for_handle(buf[i]);
			if (s) {
				s->notify_looped();
			} else if (BwaBed *b = bed_for_handle(buf[i])) {
				b->notify_looped();
			}
		}
		if (n < 64) {
			break;
		}
	}
	if (dropped != looped_dropped) {
		looped_dropped = dropped;
		if (!loop_drop_warned) {
			loop_drop_warned = true;
			UtilityFunctions::push_warning(vformat(
					"BwaEngine: %d loop-boundary events were dropped before anything read them, "
					"so that many \"looped\" signals never fired. Either the frame stalled, or a "
					"loop region is short enough to wrap faster than the frame rate reads it. "
					"get_loop_events_dropped() carries the running total (this warns once).",
					(int64_t)dropped));
		}
	}
}

void BwaEngine::_exit_tree() {
	if (singleton == this) {
		singleton = nullptr;
	}
	fallback_taken = false; /* before the no-engine return: nothing runs on the fallback now */
	if (!eng) {
		return;
	}
	bwa_stop(eng);
	bwa_destroy(eng);
	eng = nullptr;
	sounds.clear(); /* no releases: bwa_destroy took the whole cache with it */
	static_mesh_up = false; /* the static mesh died with the engine */
	static_baked.clear();
	press_mask_set = false;
	/* Detach every source that outlives this engine, not just forget them: their `owner`
	 * back-pointers would otherwise dangle, and the next setter call on such a source (from
	 * a script, an animation, anywhere) would dereference a freed node. Exit order usually
	 * saves the shipped scenes — sources exit before a first-child engine — but nothing
	 * forces an engine to be the first child, or sources to share its subtree at all. */
	for (BwaSource *s : sources) {
		s->engine_gone();
	}
	sources.clear();
	/* Same protocol for the non-source clients (beds, speaker views, dynamic geometry) —
	 * a bed setter or a speaker view's next _process tick is the identical use-after-free. */
	for (BwaEngineClient *c : clients) {
		c->engine_gone();
	}
	clients.clear();
	/* The beds are in `clients` too and were just detached; drop the event route as well or the
	 * next drain scans freed nodes. */
	beds.clear();
}

/* --- static scene assembly --- */

void BwaEngine::build_static_scene() {
	BwaRoomBox *box = BwaRoomBox::active();

	/* The ground/surface plane: the outdoor degenerate of the box, applied at the same
	 * pre-start moment for the same reason (the image-source handoff is load-time). A
	 * BwaRoomBox wins when both exist — one room at a time, and the box is the more
	 * deliberate act of authoring. */
	ground_pushed = false;
	if (ground_enabled) {
		if (box) {
			UtilityFunctions::push_warning(
					"BwaEngine: both a BwaRoomBox and ground_enabled - the box wins (one room at "
					"a time); disable one.");
		} else {
			ground_pushed = true;
			push_ground();
		}
	}

	/* Capture the shoebox for the image-source early reflections. That half works with or without
	 * the Steam Audio SDK, so it matters even in a phonon-free build. Uses the ISM-ONLY call, so it
	 * does not clobber the static mesh. Its walls then join the merged mesh as ordinary geometry.
	 * Without that, the commonest Godot setup (a box and nothing else) had an empty ray-traced
	 * scene: no occlusion from the room's own walls. */
	box_faces.clear();
	box_tri_mats.clear();
	if (box) {
		PackedInt32Array face_tokens;
		for (int f = 0; f < 6; f++) {
			const Ref<BwaMaterial> m = box->get_face_material(f);
			face_tokens.push_back(m.is_valid() ? (int)m->token(this) : 0);
		}
		const Vector3 s = box->room_size();
		scene_set_ism_room((float)s.x, (float)s.y, (float)s.z, face_tokens);
		box_faces = box->wall_faces();
		for (int t = 0; t < 12; t++) {
			box_tri_mats.push_back(face_tokens[t / 2]);
		}
	}

	static_seen = BwaAcousticGeometry::revision();
	static_reg_dirty = false;
	commit_static_scene();
}

std::vector<uint64_t> BwaEngine::visible_statics() {
	std::vector<uint64_t> ids;
	for (BwaAcousticGeometry *g : BwaAcousticGeometry::registry()) {
		if (g->is_visible_in_tree()) {
			ids.push_back(g->get_instance_id());
		}
	}
	/* Instance ids, never pointers: a freed node's address can come back as a new node, and a
	 * pointer compare would then call a changed set unchanged. */
	std::sort(ids.begin(), ids.end());
	return ids;
}

void BwaEngine::commit_static_scene() {
	PackedVector3Array faces = box_faces;
	PackedInt32Array tri_materials = box_tri_mats;
	static_baked = visible_statics();
	for (BwaAcousticGeometry *g : BwaAcousticGeometry::registry()) {
		if (!g->is_visible_in_tree()) {
			continue;
		}
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

	if (faces.is_empty()) {
		if (!static_mesh_up) {
			return; /* nothing pushed, nothing to drop: an empty install pushes nothing */
		}
		/* The ground plane's own quad IS the static mesh when it is the room, so put it back.
		 * Otherwise the ABI's documented clear: NULL arrays, zero counts. */
		if (ground_pushed) {
			push_ground();
		} else {
			bwa_scene_set_mesh_mat(eng, nullptr, 0, nullptr, 0, nullptr);
		}
		static_mesh_up = false;
		static_rebuilds++;
		return;
	}

	PackedInt32Array tris;
	tris.resize(faces.size());
	for (int i = 0; i < faces.size(); i++) {
		tris[i] = i; // get_faces() is already a triangle soup, so the index list is trivial
	}
	scene_set_mesh(faces, tris, tri_materials);
	static_mesh_up = true;
	static_rebuilds++;
}

void BwaEngine::refresh_static_scene() {
	const uint32_t rev = BwaAcousticGeometry::revision();
	if (rev == static_seen && !static_reg_dirty) {
		return; /* the common frame: nothing entered, left, hid or moved the frame */
	}
	static_seen = rev;
	const bool moved = static_reg_dirty;
	static_reg_dirty = false;
	if (moved && ground_pushed) {
		push_ground(); /* replaces the merged mesh with its quad; the commit below restores it */
		static_mesh_up = false;
	}
	/* A toggle and its undo in one frame leave the set as baked, and cost nothing. */
	const bool baked_frame = !static_baked.empty() || !box_faces.is_empty();
	if (visible_statics() != static_baked || (moved && baked_frame)) {
		commit_static_scene();
	}
}

void BwaEngine::push_ground() {
	/* bwa_scene_set_ground rebuilds the image-source room from scratch, which drops a
	 * pressure-release mask set after start. Put it back. */
	const bool had_mask = press_mask_set;
	const int mask = press_mask;
	const int tok = ground_material.is_valid() ? (int)ground_material->token(this) : 0;
	scene_set_ground(ground_height, tok, ground_pressure_release);
	if (had_mask) {
		scene_set_pressure_release(mask);
	}
}

/* --- the coordinate seam --- */

/* Unity's Engine.roomOrigin warns past the same 1 degree: the ground plane takes the origin's
 * height only, so a tilted origin tilts every position against a floor that stayed level. */
static const double ROOM_ORIGIN_TILT_COS = 0.99984769515639127; /* cos(1 deg) */

/* Why an origin cannot give a registration. */
enum RoomOriginFault { ORIGIN_OK = 0, ORIGIN_NON_FINITE, ORIGIN_COLLAPSED, ORIGIN_MIRRORED };

/* Judged on the NORMALIZED axes, so a small uniform scale (0.001, a millimeter rig) is a
 * scale to drop and not a collapse. A negative determinant is a reflection: orthonormalizing
 * keeps it, every position would come out mirrored, and Quaternion(registration.basis * b)
 * would be handed a matrix that is not a rotation. Refused rather than repaired, because
 * which axis the user meant to flip is not ours to guess. */
static RoomOriginFault room_origin_fault(const Transform3D &t) {
	if (!t.is_finite()) {
		return ORIGIN_NON_FINITE;
	}
	Vector3 c[3];
	for (int i = 0; i < 3; i++) {
		c[i] = t.basis.get_column(i);
		const real_t len = c[i].length();
		if (!(len > 0)) {
			return ORIGIN_COLLAPSED;
		}
		c[i] /= len;
	}
	const double det = (double)c[0].cross(c[1]).dot(c[2]);
	if (std::fabs(det) < 1e-6) {
		return ORIGIN_COLLAPSED;
	}
	return det < 0.0 ? ORIGIN_MIRRORED : ORIGIN_OK;
}

static const char *room_origin_fault_text(RoomOriginFault f) {
	switch (f) {
		case ORIGIN_NON_FINITE:
			return "has a non-finite transform";
		case ORIGIN_COLLAPSED:
			return "has a zero-length or flattened axis";
		case ORIGIN_MIRRORED:
			return "is mirrored (a negative scale on it or on a parent)";
		default:
			return "is fine";
	}
}

static bool room_origin_scaled(const Basis &b) {
	for (int i = 0; i < 3; i++) {
		if (std::fabs((double)b.get_column(i).length() - 1.0) > 1e-3) {
			return true;
		}
	}
	return false;
}

static bool room_origin_tilted(const Basis &b) {
	const Vector3 up = b.get_column(1);
	const double len = (double)up.length();
	return len > 0.0 && (double)up.y / len < ROOM_ORIGIN_TILT_COS;
}

void BwaEngine::registration_changed() {
	reg_gen++;
	if (!eng) {
		return; /* before start nothing has been converted yet */
	}
	static_reg_dirty = true; /* the static mesh and the ground plane re-bake next _process */
	if (registration_stale) {
		return;
	}
	const bool fdn_dir = fdn.enabled
			&& (fdn.decay_dir[0] != 0.0f || fdn.decay_dir[1] != 0.0f || fdn.decay_dir[2] != 0.0f);
	/* The box's image-source room has no position to re-push: the ABI anchors it at the room
	 * origin by size alone. Its ray-traced walls re-bake, so after a change the two disagree. */
	const bool box = !box_faces.is_empty();
	if (!extra_listeners_set && !fdn_dir && !box) {
		return; /* everything else is pushed per frame or re-baked, and follows */
	}
	registration_stale = true;
	UtilityFunctions::push_warning(
			"BwaEngine: the registration changed after start. Three things keep the old frame: "
			"set_extra_listeners positions (call it again), the FDN decay direction (load-time) "
			"and the room box's image-source reflections, which the engine anchors at the room "
			"origin. The box's walls, the static geometry and the ground plane follow. "
			"is_registration_stale() now reads true.");
}

void BwaEngine::set_registration(const Transform3D &t) {
	if (inert) {
		if (!registration_refused_warned) {
			registration_refused_warned = true;
			UtilityFunctions::push_warning(
					"BwaEngine: this node is the refused second engine and runs nothing, so its "
					"registration is ignored. Set it on the BwaEngine that is running.");
		}
		return;
	}
	if (!room_origin_path.is_empty() && is_inside_tree()
			&& !Engine::get_singleton()->is_editor_hint()
			&& Object::cast_to<Node3D>(get_node_or_null(room_origin_path))) {
		if (!registration_refused_warned) {
			registration_refused_warned = true;
			UtilityFunctions::push_warning(
					"BwaEngine: registration is driven by room_origin, so this write is ignored. "
					"Move the room_origin node, or clear room_origin first.");
		}
		return;
	}
	if (t == registration) {
		return;
	}
	registration = t;
	registration_changed();
}

void BwaEngine::set_room_origin(const NodePath &p) {
	room_origin_path = p;
	room_origin_tracked = false; /* a new node at the old node's transform still recomputes */
	origin_missing_warned = false;
	origin_invalid_warned = false;
	editor_origin_state = -1;
	update_configuration_warnings();
	if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint()) {
		sync_room_origin();
	}
}

void BwaEngine::sync_room_origin() {
	if (inert || room_origin_path.is_empty()) {
		return; /* no origin: the registration is whatever code set, identity by default */
	}
	Node3D *origin = Object::cast_to<Node3D>(get_node_or_null(room_origin_path));
	if (!origin) {
		/* Freed with its scene, or a bad path. Keep the last registration: jumping the whole
		 * mix back to identity mid-run is worse than holding it. */
		if (!origin_missing_warned) {
			origin_missing_warned = true;
			UtilityFunctions::push_warning(vformat(
					"BwaEngine: room_origin \"%s\" is not a Node3D in the tree; the registration "
					"keeps its last value.",
					String(room_origin_path)));
		}
		return;
	}
	const Transform3D t = origin->get_global_transform();
	const RoomOriginFault fault = room_origin_fault(t);
	/* Non-finite is judged before the moved test: a NaN never compares equal, so it would
	 * read as "moved" on every frame. */
	const bool same = fault != ORIGIN_NON_FINITE && room_origin_tracked && t == room_origin_seen;
	if (same) {
		return;
	}
	if (fault != ORIGIN_OK) {
		/* All three faults hold the registration, like a freed origin. */
		if (!origin_invalid_warned) {
			origin_invalid_warned = true;
			UtilityFunctions::push_warning(vformat(
					"BwaEngine: room_origin \"%s\" %s, so it cannot set the registration. The "
					"registration keeps its last value.",
					String(room_origin_path), String(room_origin_fault_text(fault))));
		}
		if (fault != ORIGIN_NON_FINITE) {
			room_origin_seen = t;
			room_origin_tracked = true;
		}
		return;
	}
	room_origin_seen = t;
	room_origin_tracked = true;

	if (!origin_scale_warned && room_origin_scaled(t.basis)) {
		origin_scale_warned = true;
		UtilityFunctions::push_warning(
				"BwaEngine: room_origin is scaled (its own scale or a parent's). The scale is "
				"ignored: room meters are Godot meters.");
	}
	const Basis rot = t.basis.orthonormalized();
	if (!origin_tilt_warned && room_origin_tilted(rot)) {
		origin_tilt_warned = true;
		UtilityFunctions::push_warning(
				"BwaEngine: room_origin is tilted more than 1 degree. Keep it level: the room "
				"frame's +Y is up and the ground plane uses the origin's height only.");
	}
	/* A frame, not a facing: the origin's local axes are the room axes, so this takes the
	 * plain inverse and NOT the 180 degree facing swizzle of bwa_room.h. */
	const Transform3D reg = Transform3D(rot, t.origin).affine_inverse();
	if (reg == registration) {
		return; /* re-pointed at a node that says the same thing: nothing went stale */
	}
	registration = reg;
	registration_changed();
}

/* Editor only. The engine never runs there, so this is the whole cost of keeping the scale,
 * tilt and mirror warnings honest while someone drags the origin around. */
void BwaEngine::editor_watch_origin() {
	if (room_origin_path.is_empty()) {
		return;
	}
	const Node3D *origin = Object::cast_to<Node3D>(get_node_or_null(room_origin_path));
	Transform3D t;
	int state = 0; /* 0 = unresolved, 1 = non-finite, 2 = a transform worth comparing */
	if (origin) {
		t = origin->get_global_transform();
		state = t.is_finite() ? 2 : 1;
	}
	if (state == editor_origin_state && (state != 2 || t == room_origin_seen)) {
		return;
	}
	editor_origin_state = state;
	room_origin_seen = t;
	update_configuration_warnings();
}

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
	extra_listeners_set = n > 0; /* room positions as of NOW; see registration_changed */
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

/* --- assets ---------------------------------------------------------------------------
 *
 * The by-path cache moved INTO the core (bwa_sound_acquire / bwa_sound_release, keyed on
 * (path, flags)), so this node no longer deduplicates and no longer counts references. What
 * stays is a RECORD of the keys it acquired, and only because the public by-path calls have
 * no ABI to lean on: bwa_sound_acquire cannot be used as a probe, because on a miss it
 * LOADS - which is the hidden-decode bug the metadata getters were fixed for once already.
 *
 * The node holds exactly one reference per key for its own lifetime, which is the same
 * ownership the old cache had: acquired on first use, released by unload_sound_path or with
 * the engine. */

bwa_sound BwaEngine::acquire_sound(const String &path, uint32_t flags, bool async) {
	if (!eng || path.is_empty()) {
		return 0;
	}
	for (const HeldSound &h : sounds) {
		if (h.flags == flags && h.path == path) {
			return h.snd; /* our reference, already taken */
		}
	}
	const CharString os = to_os_path(path).utf8();
	const bwa_sound snd = async ? bwa_sound_acquire_async(eng, os.get_data(), flags)
								: bwa_sound_acquire(eng, os.get_data(), flags);
	if (!snd) {
		UtilityFunctions::push_error(
				vformat("BwaEngine: could not load \"%s\": %s", path, get_last_error()));
		return 0;
	}
	sounds.push_back({ path, flags, snd });
	return snd;
}

bwa_sound BwaEngine::load_sound(const String &path, bool stream, bool async) {
	return acquire_sound(path, stream ? (uint32_t)BWA_LOAD_STREAM : 0u, async);
}

bwa_sound BwaEngine::load_ambisonic(const String &path, bool fuma, bool async) {
	return acquire_sound(path, fuma ? (uint32_t)BWA_LOAD_FUMA : (uint32_t)BWA_LOAD_AMBIX, async);
}

bool BwaEngine::preload_sound(const String &path, int flags) {
	return acquire_sound(path, (uint32_t)flags, false) != 0;
}

bool BwaEngine::preload_sound_async(const String &path, int flags) {
	return acquire_sound(path, (uint32_t)flags, true) != 0;
}

bool BwaEngine::sound_is_ready(const String &path, int flags) const {
	if (!eng) {
		return false;
	}
	for (const HeldSound &h : sounds) {
		if (h.flags == (uint32_t)flags && h.path == path) {
			return bwa_sound_is_ready(eng, h.snd);
		}
	}
	return false; /* never acquired: nothing is on its way, so nothing is ready */
}

/* Still-decoding is NOT an error, so the ABI leaves bwa_last_error clear for it and sets it
 * only when the load failed (or the handle is not the cache's). That is the whole difference
 * between "wait" and "give up", and it is why bwa_sound_is_ready clears the error first. */
int BwaEngine::sound_ready_state(bwa_sound snd) const {
	if (!eng || !snd) {
		return 1; /* nothing to wait for */
	}
	if (bwa_sound_is_ready(eng, snd)) {
		return 1;
	}
	return bwa_last_error(eng) ? -1 : 0;
}

void BwaEngine::unload_sound_path(const String &path) {
	if (!eng) {
		return;
	}
	/* One path can be held under several flag sets (memory vs streamed vs ambisonic), so drop
	 * every one and "unload this file" means what it says. The engine frees on the LAST
	 * reference, so another holder keeps the asset alive - which is the point of the tier. */
	for (size_t i = sounds.size(); i-- > 0;) {
		if (sounds[i].path == path) {
			bwa_sound_release(eng, sounds[i].snd);
			sounds.erase(sounds.begin() + (ptrdiff_t)i);
		}
	}
}

/* Metadata must answer for whichever form was actually loaded, in the order a multi-role path
 * would want: the point-source copies first, then the bed ones. */
/* Asks the ENGINE's cache, not our own record: bwa_sound_find is a pure lookup that never loads
 * and never takes a reference, so it answers for anything resident, including a path acquired
 * through the C ABI directly rather than through this node. Probing with bwa_sound_acquire would
 * LOAD on a miss, which is the hidden decode the getters below exist to avoid. The order is the
 * one a multi-role path wants: the point-source forms first, then the bed ones. */
bwa_sound BwaEngine::find_loaded_sound(const String &path) const {
	if (!eng || path.is_empty()) {
		return 0;
	}
	const CharString os = to_os_path(path).utf8();
	const uint32_t order[4] = { 0u, (uint32_t)BWA_LOAD_STREAM, (uint32_t)BWA_LOAD_AMBIX,
		(uint32_t)BWA_LOAD_FUMA };
	for (uint32_t flags : order) {
		if (const bwa_sound snd = bwa_sound_find(eng, os.get_data(), flags)) {
			return snd;
		}
	}
	return 0;
}

/* CACHED-ONLY, deliberately (see the header): the old fallback decoded an uncached path as a
 * hidden side effect - and decoded it MONO, so an ambisonic bed answered channels=1 forever
 * and the ABI's 4/9/16 answers were unreachable through this getter. */
int64_t BwaEngine::sound_get_frames(const String &path) {
	if (!eng) {
		return 0;
	}
	const bwa_sound snd = find_loaded_sound(path);
	return snd ? (int64_t)bwa_sound_get_frames(eng, snd) : 0;
}

int BwaEngine::sound_get_channels(const String &path) {
	if (!eng) {
		return 0;
	}
	const bwa_sound snd = find_loaded_sound(path);
	return snd ? (int)bwa_sound_get_channels(eng, snd) : 0;
}

/* Returns whether the oneshot was ACCEPTED. A oneshot holds no handle, so this boolean is the
 * only thing a caller can check — without it, "clip missing" and "dropped under load" and
 * "played" all look identical from GDScript. The load failure already pushes an error (see
 * load_sound); the DROP deliberately does not, because the case that produces it is oneshot
 * spam and a per-drop message would bury the console at exactly the wrong moment. */
bool BwaEngine::play_oneshot(const String &path, const Vector3 &godot_pos, float gain) {
	const bwa_sound snd = load_sound(path, false);
	if (!snd) {
		return false;
	}
	const Vector3 p = to_room_position(godot_pos);
	return bwa_play_oneshot(eng, snd, (float)p.x, (float)p.y, (float)p.z, gain);
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

/* The scene transition. The core does the stopping; the loop is only there to tell the
 * sources, because an emitter that is not told reads the voice going quiet as a natural end
 * and announces `finished` - and a scene change is the one moment every emitter would do it
 * at once. Same rule as BwaEmitter::stop(): an explicit halt is not an end. */
void BwaEngine::group_stop(int group) {
	if (!eng) {
		return;
	}
	bwa_group_stop(eng, (uint32_t)group);
	for (BwaSource *s : sources) {
		if (s->get_group() == group) {
			s->on_stopped_externally();
		}
	}
	/* Beds carry a mix group too, and a bed that is not told reads the silence as a natural end
	 * exactly as an emitter would. */
	for (BwaBed *b : beds) {
		if (b->get_group() == group) {
			b->on_stopped_externally();
		}
	}
}

void BwaEngine::stop_all() {
	if (!eng) {
		return;
	}
	bwa_stop_all(eng);
	for (BwaSource *s : sources) {
		s->on_stopped_externally();
	}
	for (BwaBed *b : beds) {
		b->on_stopped_externally();
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
BWA_LIVE_SETTER(dual_band_cap, bool, bwa_set_dual_band_cap(eng, v))

/* Situation tuning. The Dictionary form exists so the preset can be printed and diffed rather than
 * trusted blind, which is the whole reason the C API fills a struct instead of applying one. */
static godot::Dictionary tuning_to_dict(const bwa_tuning& t) {
	godot::Dictionary d;
	d["panner"] = (int)t.panner;
	d["spcap_focus"] = t.spcap_focus;
	d["spcap_density"] = t.spcap_density;
	d["dual_band"] = t.dual_band;
	d["dual_band_cap"] = t.dual_band_cap;
	d["spread_mode"] = (int)t.spread_mode;
	d["decorrelation"] = t.decorrelation;
	d["near_spread"] = t.near_spread;
	d["hole_spread"] = t.hole_spread;
	d["max_re"] = t.max_re;
	d["max_re_split"] = t.max_re_split;
	d["bed_renderer"] = (int)t.bed_renderer;
	d["tracked_room_eq"] = t.tracked_room_eq;
	d["tracked_directivity"] = t.tracked_directivity;
	d["tracked_align"] = t.tracked_align;
	d["align_dead_zone_m"] = t.align_dead_zone_m;
	d["align_slew_frames_per_s"] = t.align_slew_frames_per_s;
	return d;
}

godot::Dictionary BwaEngine::get_live_tuning() const {
	bwa_tuning t;
	if (!eng || !bwa_get_tuning(eng, &t)) return godot::Dictionary();
	return tuning_to_dict(t);
}

godot::Dictionary BwaEngine::get_setup_tuning(Setup setup) const {
	bwa_tuning t;
	bwa_tuning_preset((bwa_setup)setup, &t);
	return tuning_to_dict(t);
}

bool BwaEngine::apply_setup(Setup setup) {
	if (!eng) return false;
	bwa_tuning t;
	bwa_tuning_preset((bwa_setup)setup, &t);
	if (!bwa_apply_tuning(eng, &t)) return false;
	/* mirror into the node's own properties so the inspector does not lie about the live state */
	panner = (Panner)t.panner;
	spcap_focus = t.spcap_focus; spcap_density = t.spcap_density;
	dual_band = t.dual_band; dual_band_cap = t.dual_band_cap;
	spread_mode = (SpreadMode)t.spread_mode;
	decorrelation = t.decorrelation;
	near_spread = t.near_spread; hole_spread = t.hole_spread;
	max_re = t.max_re; max_re_split = t.max_re_split;
	bed_renderer = (BedRenderer)t.bed_renderer;
	tracked_room_eq = t.tracked_room_eq;
	tracked_directivity = t.tracked_directivity;
	tracked_align = t.tracked_align;
	tracked_align_dead_zone = t.align_dead_zone_m;
	tracked_align_slew_frames_per_s = t.align_slew_frames_per_s;
	return true;
}
/* one C call carries both SPCAP exponents, so each property setter re-sends the pair */
BWA_LIVE_SETTER(spcap_focus, float, bwa_set_spcap_focus(eng, spcap_focus, spcap_density))
BWA_LIVE_SETTER(spcap_density, float, bwa_set_spcap_focus(eng, spcap_focus, spcap_density))
BWA_LIVE_SETTER(spread_mode, SpreadMode, bwa_set_spread_mode(eng, (bwa_spread_mode)v))
BWA_LIVE_SETTER(decorrelation, bool, bwa_set_decorrelation(eng, v))
BWA_LIVE_SETTER(near_spread, float, bwa_set_near_spread(eng, v))
BWA_LIVE_SETTER(hole_spread, float, bwa_set_hole_spread(eng, v))
BWA_LIVE_SETTER(speed_of_sound, float, bwa_set_speed_of_sound(eng, v))
BWA_LIVE_SETTER(max_re, bool, bwa_set_max_re(eng, v))
BWA_LIVE_SETTER(max_re_split, bool, bwa_set_max_re_split(eng, v))
BWA_LIVE_SETTER(bed_renderer, BedRenderer, bwa_set_bed_renderer(eng, (bwa_bed_renderer)v))
BWA_LIVE_SETTER(tracked_room_eq, bool, bwa_set_tracked_room_eq(eng, v))
BWA_LIVE_SETTER(tracked_directivity, bool, bwa_set_tracked_directivity(eng, v))
/* one C call carries the toggle and both knobs, so each property setter re-sends the trio */
BWA_LIVE_SETTER(tracked_align, bool, bwa_set_tracked_align(eng, v))
BWA_LIVE_SETTER(tracked_align_dead_zone, float, bwa_set_tracked_align_guards(eng, tracked_align_dead_zone, tracked_align_slew_frames_per_s))
BWA_LIVE_SETTER(tracked_align_slew_frames_per_s, float, bwa_set_tracked_align_guards(eng, tracked_align_dead_zone, tracked_align_slew_frames_per_s))
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
	/* Inverted back through the registration so the PROPERTY round-trips exactly. The desc
	 * stores room space; returning that raw meant every save/load under a rotated
	 * registration re-applied the rotation and the direction drifted one step per cycle. */
	return from_room_direction(Vector3(fdn.decay_dir[0], fdn.decay_dir[1], fdn.decay_dir[2]));
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

/* The ISM shoebox only: does not touch the ray-traced static mesh, so the box composes with other
 * occluders instead of replacing them. scene_set_box remains for the box-IS-the-scene case. */
void BwaEngine::scene_set_ism_room(float w, float h, float d, const PackedInt32Array &faces) {
	if (!eng) {
		return;
	}
	bwa_material f[6] = {};
	for (int i = 0; i < MIN(faces.size(), 6); i++) {
		f[i] = (bwa_material)faces[i];
	}
	bwa_scene_set_ism_room(eng, w, h, d, f);
	press_mask_set = false; /* a new room resets every face */
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
	press_mask_set = false;
}

void BwaEngine::scene_set_ground(float y, int material, bool pressure_release) {
	if (!eng) {
		return;
	}
	/* y is a Godot-world height: the plane sits AT a position, so the registration transform
	 * applies (the box escapes this only because it takes sizes, not positions). */
	const float room_y = (float)to_room_position(Vector3(0, y, 0)).y;
	bwa_scene_set_ground(eng, room_y, (bwa_material)material, pressure_release);
	press_mask_set = false;
}

void BwaEngine::scene_set_pressure_release(int face_mask) {
	if (eng) {
		bwa_scene_set_pressure_release(eng, (uint32_t)face_mask);
		press_mask = face_mask; /* kept so a re-pushed ground can restore it */
		press_mask_set = true;
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
	 * demos' centered quads (symmetric under that turn), wrong for anything asymmetric. */
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

int64_t BwaEngine::get_dsp_time_frames() const { return eng ? (int64_t)bwa_get_dsp_time_frames(eng) : 0; }

float BwaEngine::get_dsp_time_seconds() const {
	const int rate = get_resolved_sample_rate();
	return rate > 0 ? (float)((double)get_dsp_time_frames() / (double)rate) : 0.0f;
}

Dictionary BwaEngine::get_clock() const {
	Dictionary d;
	uint64_t sample = 0, host = 0;
	const bool ok = eng && bwa_get_clock(eng, &sample, &host);
	d["valid"] = ok;
	d["dsp_sample"] = (int64_t)sample;
	d["host_time_ns"] = (int64_t)host;
	return d;
}

int BwaEngine::get_output_latency_frames() const {
	return eng ? (int)bwa_get_output_latency_frames(eng) : 0;
}

float BwaEngine::get_output_latency_seconds() const {
	const int rate = get_resolved_sample_rate();
	return rate > 0 ? (float)get_output_latency_frames() / (float)rate : 0.0f;
}

/* {measured, blocks, xruns, dropped_frames, driver_resyncs, late_blocks, stream_starves, peak_load,
 * device_lost}
 * — a Dictionary rather than a fistful of getters, because these numbers only mean anything
 * TOGETHER: a count without `blocks` under it has no scale, and any of them without `measured` may
 * be zero simply because this configuration cannot see a dropout. */
Dictionary BwaEngine::get_health() const {
	Dictionary d;
	bwa_health h = {};
	const bool measured = eng ? bwa_get_health(eng, &h) : false;
	d["measured"] = measured;
	d["blocks"] = (int64_t)h.blocks;
	d["xruns"] = (int64_t)h.xruns;
	d["dropped_frames"] = (int64_t)h.dropped_frames;
	d["driver_resyncs"] = (int64_t)h.driver_resyncs;
	d["late_blocks"] = (int64_t)h.late_blocks;
	d["stream_starves"] = (int64_t)h.stream_starves;
	d["peak_load"] = h.peak_load;
	d["device_lost"] = h.device_lost != 0;
	return d;
}

int64_t BwaEngine::get_xruns() const {
	return eng ? (int64_t)bwa_get_xruns(eng) : 0;
}

Dictionary BwaEngine::get_clock_model() const {
	Dictionary d;
	bwa_clock_model m = {};
	const bool ok = eng && bwa_get_clock_model(eng, &m);
	d["valid"] = ok;
	d["ppm"] = m.ppm;
	d["ppm_sigma"] = m.ppm_sigma;
	d["rate_hz"] = m.rate_hz;
	d["span_s"] = m.span_s;
	d["jitter_ns"] = m.jitter_ns;
	d["stamps"] = (int)m.stamps;
	return d;
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

/* What spcap_focus falls back to on the engine's ACTIVE layout: the speakers come back in engine
 * (room) space, which is what the derivation wants - it is centroid-relative and angle-only. */
float BwaEngine::get_spcap_focus_default() const {
	if (!eng) {
		return 0.0f;
	}
	const uint32_t n = bwa_get_speakers(eng, nullptr, 0);
	if (n < 2) {
		return 0.0f;
	}
	std::vector<float> xyz(n * 3);
	bwa_get_speakers(eng, xyz.data(), n);
	return bwa_spcap_focus_default(xyz.data(), n);
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

int BwaEngine::get_device_count(Sink backend) { return (int)bwa_get_device_count((bwa_sink_type)backend); }

/* int64_t rather than the ABI's uint64_t because Godot's Variant has no unsigned int: the clock is
 * a monotonic counter of nanoseconds, which does not reach the sign bit for ~292 years of uptime. */
int64_t BwaEngine::get_host_time_ns() { return (int64_t)bwa_host_time_ns(); }

String BwaEngine::get_device_name(Sink backend, int index) {
	char buf[256];
	if (index < 0 || !bwa_get_device_name((bwa_sink_type)backend, (uint32_t)index, buf, sizeof buf)) {
		return String();
	}
	return String::utf8(buf);    /* the ABI speaks UTF-8 at every seam */
}

String BwaEngine::get_device_id(Sink backend, int index) {
	char buf[256];
	if (index < 0 || !bwa_get_device_id((bwa_sink_type)backend, (uint32_t)index, buf, sizeof buf)) {
		return String();
	}
	return String::utf8(buf);
}

/* The engine's own answer (bwa_device_present): the rule, the AUTO order for this platform and
 * the rule-10 match all live in src/sink/sink.c, next to the open they describe. */
bool BwaEngine::is_device_present(Profile p_profile, Sink p_sink, const String &p_device) {
	const CharString dev = p_device.utf8();
	return bwa_device_present((bwa_profile)p_profile, (bwa_sink_type)p_sink,
			p_device.is_empty() ? nullptr : dev.get_data());
}

PackedStringArray BwaEngine::get_devices(Sink backend) {
	const int n = get_device_count(backend);
	PackedStringArray out;
	out.resize(n);
	for (int i = 0; i < n; ++i) {
		out.set(i, get_device_name(backend, i));
	}
	return out;
}

int BwaEngine::get_asio_driver_count() { return (int)bwa_get_asio_driver_count(); }

/* The one-call form: what you actually want before filling a dropdown or setting asio_driver.
 * The count/name pair below is still there for a caller that wants one name without the array. */
PackedStringArray BwaEngine::get_asio_drivers() {
	const int n = get_asio_driver_count();
	PackedStringArray out;
	out.resize(n);
	for (int i = 0; i < n; ++i) {
		out.set(i, get_asio_driver_name(i));
	}
	return out;
}

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

void BwaEngine::register_bed(BwaBed *b) {
	for (BwaBed *x : beds) {
		if (x == b) {
			return;
		}
	}
	beds.push_back(b);
}

void BwaEngine::unregister_bed(BwaBed *b) {
	for (size_t i = 0; i < beds.size(); i++) {
		if (beds[i] == b) {
			beds.erase(beds.begin() + (long)i);
			return;
		}
	}
}

void BwaEngine::register_client(BwaEngineClient *c) {
	for (BwaEngineClient *x : clients) {
		if (x == c) {
			return;
		}
	}
	clients.push_back(c);
}

void BwaEngine::unregister_client(BwaEngineClient *c) {
	for (size_t i = 0; i < clients.size(); i++) {
		if (clients[i] == c) {
			clients.erase(clients.begin() + (long)i);
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
					"enable early_reflections on sources - they render twice.");
	}
	/* The desk case, and the one that costs the most time: profile Cave renders the speaker
	 * array and nothing else, so on a machine with no rig it is silent by design. get_audio_backend()
	 * reports it after the fact — this says it before the run. CaveSim is the profile that
	 * auditions the array on headphones; Binaural is the direct headphone render. */
	/* Cave is the ONLY profile this warning still applies to. The headphone profiles reach a
	 * WASAPI endpoint now, so "no ASIO driver" stopped meaning "silence" for them; the array is
	 * the one path whose transport is fixed to ASIO.
	 * WINDOWS ONLY: elsewhere the array has no ASIO to miss. It opens through JACK, ALSA or
	 * CoreAudio on any device with enough outputs, and the binding cannot count those from here. */
	/* With a fallback set, the same machine is not silent: say what a run here will do. */
	const bool has_fallback = fallback_profile != FALLBACK_NONE;
	bool primary_here = true;
	if (has_fallback) {
		/* Asked again only when a property it depends on is set, never on a timer: the query
		 * reads the registry (and opens a JACK probe client on Linux), and this function runs on
		 * every step of a room_origin drag in the editor. A device plugged in later shows when
		 * the scene is reopened or one of those properties is set. */
		if (!warn_present_valid) {
			warn_present_valid = true;
			warn_present = is_device_present(profile, sink, device);
		}
		primary_here = warn_present;
	}
	if (has_fallback && !primary_here) {
		w.push_back(vformat("The primary device%s is not listed on this machine, so a run from "
							"the editor here takes the desk fallback (%s).%s",
				device.is_empty() ? String() : String(" (\"") + device + "\")",
				fallback_profile == FALLBACK_BINAURAL ? "Binaural" : "CaveSim",
				fallback_only_in_editor ? " An export does not."
										: " So does an export: a typo in Device on the rig then "
										  "plays to headphones and does not fail the start."));
	}
	if (has_fallback && device.is_empty() && sink != SINK_NULL && sink != SINK_MANUAL
			&& (profile == PROFILE_CAVE || profile == PROFILE_CAVE_BOTH)) {
		w.push_back("A fallback is set, but Device is empty on an array profile. The check then "
					"only asks whether the backend lists ANY device (any installed ASIO driver on "
					"Windows), which cannot tell a desk from the rig: a desk with a driver "
					"installed starts the array, fails to open it, and is silent. Put the rig's "
					"device name in Device.");
	}
#ifdef _WIN32
	if (!has_fallback && profile == PROFILE_CAVE && sink != SINK_NULL && sink != SINK_MANUAL
			&& BwaEngine::get_asio_driver_count() == 0) {
		w.push_back("Profile is Cave (the speaker-array rig) but no ASIO driver is installed, so "
					"this machine will render the array to nothing and you will hear silence. "
					"Use CaveSim to audition the array on headphones, or Binaural for the "
					"direct headphone render - both open an ordinary Windows output device.");
	}
#endif
	if (feed_listener && listener_path.is_empty()) {
		w.push_back("Feed Listener is on but no listener node is set, so the listener never "
					"moves from the origin. Point it at the head/camera, or turn it off to let "
					"the engine read NatNet itself.");
	}
	if (!feed_listener && profile != PROFILE_CAVE) {
		w.push_back("The headphone profiles (Binaural/CaveSim/CaveBoth) need a head POSE, but "
					"Feed Listener is off - so the pose comes from a tracker, which must be "
					"connected for the render to follow the head.");
	}
	if (layout_path.is_empty() && profile != PROFILE_BINAURAL) {
		/* Binaural is exempt: no physical array, so the default grid costs nothing there and a
		 * warning on every correct desk scene would teach people to ignore the marker. */
		w.push_back("No layout: this array profile will render the built-in 26-speaker grid, "
					"not your room. Copy the rig's trims.json or cave_layout.json into the "
					"project (under res://) and set Layout Path to it.");
	}
	if (!room_origin_path.is_empty() && is_inside_tree()) {
		const Node3D *origin = Object::cast_to<Node3D>(get_node_or_null(room_origin_path));
		if (!origin) {
			w.push_back("Room Origin does not point at a Node3D, so the registration never "
						"follows it.");
		} else {
			const Transform3D t = origin->get_global_transform();
			const RoomOriginFault fault = room_origin_fault(t);
			if (fault != ORIGIN_OK) {
				w.push_back(vformat("Room Origin %s, so it cannot set the registration and the "
									"engine will ignore it.",
						String(room_origin_fault_text(fault))));
			} else {
				if (room_origin_scaled(t.basis)) {
					w.push_back("Room Origin is scaled (its own scale or a parent's). The scale "
								"is ignored: room meters are Godot meters. Set it back to 1.");
				}
				if (room_origin_tilted(t.basis.orthonormalized())) {
					w.push_back("Room Origin is tilted more than 1 degree. Keep it level and "
								"turn it about Y only.");
				}
			}
		}
	}
	if (!layout_path.is_empty()) {
		/* A failed layout load is not fatal at create: the core falls back to the 26-speaker
		 * grid, quietly changing the channel count on a smaller rig. bwa_start does refuse
		 * the fallback, so this is about catching it before a run rather than after. */
		if (!FileAccess::file_exists(layout_path) && profile == PROFILE_BINAURAL) {
			w.push_back(vformat("Layout file not found: \"%s\". Binaural drives no speakers, so "
								"the engine runs the built-in default grid for the diffuse bed. "
								"Cave, CaveSim and CaveBoth need the file and refuse to start "
								"without it.",
					layout_path));
		} else if (!FileAccess::file_exists(layout_path)) {
			w.push_back(vformat("Layout file not found: \"%s\". bwa_start will refuse to run "
								"rather than fall back to the 26-speaker default grid.%s",
					layout_path,
					fallback_profile == FALLBACK_BINAURAL
							? " The Binaural desk fallback runs without it." : ""));
		} else if (layout_path.begins_with("res://")) {
			w.push_back("The core opens files by OS path, not through Godot's virtual "
						"filesystem. A res:// layout works in the editor but will not exist "
						"inside an exported .pck - ship it beside the executable or stage it "
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
	M(set_device, "name"); M0(get_device);
	M(set_asio_driver, "name"); M0(get_asio_driver);   /* the old spelling of set_device */
	M(set_sink_flags, "flags"); M0(get_sink_flags);
	M(set_fallback_profile, "profile"); M0(get_fallback_profile);
	M(set_fallback_sink, "sink"); M0(get_fallback_sink);
	M(set_fallback_device, "name"); M0(get_fallback_device);
	M(set_fallback_sink_flags, "flags"); M0(get_fallback_sink_flags);
	M(set_fallback_only_in_editor, "on"); M0(get_fallback_only_in_editor);
	M0(is_using_fallback); M0(get_active_profile);
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
	M(set_room_origin, "path"); M0(get_room_origin);
	M0(is_registration_stale);
	M(to_room_position, "v"); M(to_room_direction, "v");
	M(to_room_orientation, "basis"); M(to_room_yaw, "basis");
	M(from_room_position, "v"); M(from_room_direction, "v");
	M(from_room_orientation, "quaternion");

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
	/* flags defaults to LOAD_MEMORY, which is the case nearly every caller wants. */
	ClassDB::bind_method(D_METHOD("preload_sound", "path", "flags"), &BwaEngine::preload_sound,
			DEFVAL(0));
	ClassDB::bind_method(D_METHOD("preload_sound_async", "path", "flags"),
			&BwaEngine::preload_sound_async, DEFVAL(0));
	ClassDB::bind_method(
			D_METHOD("sound_is_ready", "path", "flags"), &BwaEngine::sound_is_ready, DEFVAL(0));

	M(set_master_gain, "linear"); M0(get_master_gain);
	M(set_paused, "paused"); M0(get_paused);
	M(group_set_gain, "group", "linear");
	M(group_set_paused, "group", "paused");
	M(group_stop, "group"); M0(stop_all);
	M(reverb_set_gain, "linear"); M0(get_reverb_gain);
	M(early_reflections_set_gain, "linear"); M0(get_early_reflections_gain);
	M(fdn_set_decay, "rt60_low_s", "rt60_high_s", "xover_hz");

	M(set_panner, "panner"); M0(get_panner);
	M(set_dual_band, "on"); M0(get_dual_band);
	M(set_dual_band_cap, "on"); M0(get_dual_band_cap);
	M(get_setup_tuning, "setup"); M(apply_setup, "setup");
	M0(get_live_tuning); M0(get_ended_this_frame); M0(get_looped_this_frame);
	M0(get_ended_events_dropped); M0(get_loop_events_dropped);
	M(set_spcap_focus, "focus"); M0(get_spcap_focus);
	M(set_spcap_density, "density"); M0(get_spcap_density);
	M(set_spread_mode, "mode"); M0(get_spread_mode);
	M(set_decorrelation, "on"); M0(get_decorrelation);
	M(set_near_spread, "radius_m"); M0(get_near_spread);
	M(set_hole_spread, "strength"); M0(get_hole_spread);
	M(set_speed_of_sound, "meters_per_s"); M0(get_speed_of_sound);
	M(set_max_re, "on"); M0(get_max_re);
	M(set_max_re_split, "on"); M0(get_max_re_split);
	M(set_bed_renderer, "renderer"); M0(get_bed_renderer);
	M(set_tracked_room_eq, "on"); M0(get_tracked_room_eq);
	M(set_tracked_directivity, "on"); M0(get_tracked_directivity);
	M(set_tracked_align, "on"); M0(get_tracked_align);
	M(set_tracked_align_dead_zone, "dead_zone_m"); M0(get_tracked_align_dead_zone);
	M(set_tracked_align_slew_frames_per_s, "frames_per_s"); M0(get_tracked_align_slew_frames_per_s);
	M(set_limiter, "on"); M0(get_limiter);
	M(set_limiter_ceiling, "linear"); M0(get_limiter_ceiling);
	M(load_headphone_eq, "path");
	M(set_headphone_eq, "on"); M0(get_headphone_eq);

	M(material_preset, "preset");
	M(material_define, "absorption", "scattering", "transmission");
	M(material_release, "token");
	M(scene_set_box, "width", "height", "depth", "faces");
	M(scene_set_ism_room, "width", "height", "depth", "faces");
	M(scene_set_ground, "y", "material", "pressure_release");
	M(scene_set_pressure_release, "face_mask");
	M(set_ground_enabled, "on"); M0(get_ground_enabled);
	M(set_ground_height, "y"); M0(get_ground_height);
	M(set_ground_pressure_release, "on"); M0(get_ground_pressure_release);
	M(set_ground_material, "material"); M0(get_ground_material);
	M(scene_set_mesh, "verts", "tris", "tri_materials");
	M0(get_static_rebuilds);
	M(scene_add_dynamic_mesh, "verts", "tris", "material");
	M(scene_set_dynamic_transform, "handle", "position", "rotation");
	M(scene_remove_dynamic_mesh, "handle");

	M0(get_dsp_time_frames); M0(get_dsp_time_seconds); M0(get_clock); M0(get_clock_model);
	M0(get_health); M0(get_xruns);
	M0(get_output_latency_frames); M0(get_output_latency_seconds);
	M(set_test_signal, "channel", "kind", "gain");
	M0(is_running); M0(get_audio_backend); M0(get_last_error);
	M0(get_channel_count); M0(get_resolved_sample_rate); M0(get_resolved_block_size);
	M0(get_active_voices); M0(get_bus_levels); M0(get_speakers); M0(get_spcap_focus_default);
	M0(render_block);
	M0(get_generation);
#undef M
#undef M0

	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_devices", "backend"), &BwaEngine::get_devices);
	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_device_count", "backend"), &BwaEngine::get_device_count);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_device_name", "backend", "index"),
			&BwaEngine::get_device_name);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_device_id", "backend", "index"),
			&BwaEngine::get_device_id);
	ClassDB::bind_static_method("BwaEngine",
			D_METHOD("is_device_present", "profile", "sink", "device"),
			&BwaEngine::is_device_present);
	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_asio_drivers"), &BwaEngine::get_asio_drivers);
	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_asio_driver_count"), &BwaEngine::get_asio_driver_count);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_asio_driver_name", "index"),
			&BwaEngine::get_asio_driver_name);
	ClassDB::bind_static_method(
			"BwaEngine", D_METHOD("get_host_time_ns"), &BwaEngine::get_host_time_ns);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("get_version"), &BwaEngine::get_version);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_ahead"), &BwaEngine::room_ahead);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_up"), &BwaEngine::room_up);
	ClassDB::bind_static_method("BwaEngine", D_METHOD("room_right"), &BwaEngine::room_right);

	ADD_GROUP("Device", "");
	/* The hint labels carry the DECISION, not just the enum spelling. This is the highest-stakes
	 * property on the node — it decides whether someone at a desk hears anything at all — and the
	 * inspector is where the choice is made, so the answer has to be legible there rather than in
	 * docs/api.md. Cave renders the speaker array to the rig and is inaudible on headphones; CaveSim is
	 * the one that auditions the ARRAY at a desk; Binaural is the direct headphone render. */
	ADD_PROPERTY(PropertyInfo(Variant::INT, "profile", PROPERTY_HINT_ENUM,
						 "Cave - speaker-array rig only:0,"
						 "Binaural - headphones, direct render:1,"
						 "CaveSim - headphones, auditions the array:2,"
						 "CaveBoth - rig + the sim tap:3"),
			"set_profile", "get_profile");
	/* Every backend an export of this addon can open, on every platform: the list is one string
	 * and a scene travels between platforms, so it cannot follow the editor's own. Naming one the
	 * running platform lacks fails the start with a message that says so. WORKLET is left out:
	 * it is the browser build, which this extension never links. */
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sink", PROPERTY_HINT_ENUM,
						 "Auto:0,ASIO - the rig:1,Null - silent/offline:2,Manual - render_block:3,"
						 "WASAPI - Windows endpoint:4,CoreAudio - macOS device:5,"
						 "ALSA - Linux PCM:6,AAudio - Android stereo:7,JACK - Linux ports:8"),
			"set_sink", "get_sink");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "device"), "set_device", "get_device");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sink_flags", PROPERTY_HINT_FLAGS,
						 "Exclusive - take the device:1,"
						 "ExactRate - fail rather than resample:2,"
						 "TightBuffer - smallest device buffer:4,"
						 "DeepBuffer - browser sink only:8"),
			"set_sink_flags", "get_sink_flags");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sample_rate"), "set_sample_rate", "get_sample_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "block_size"), "set_block_size", "get_block_size");

	/* The desk fallback: what a rig scene runs on a machine without the rig's device. Its own
	 * group, so the rig's settings above read as the rig's and stay untouched. */
	ADD_GROUP("Device Fallback", "fallback_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback_profile", PROPERTY_HINT_ENUM,
						 "None - off:0,"
						 "Binaural - headphones, direct render:1,"
						 "CaveSim - headphones, auditions the array:2"),
			"set_fallback_profile", "get_fallback_profile");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback_sink", PROPERTY_HINT_ENUM,
						 "Auto:0,ASIO - the rig:1,Null - silent/offline:2,Manual - render_block:3,"
						 "WASAPI - Windows endpoint:4,CoreAudio - macOS device:5,"
						 "ALSA - Linux PCM:6,AAudio - Android stereo:7,JACK - Linux ports:8"),
			"set_fallback_sink", "get_fallback_sink");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "fallback_device"), "set_fallback_device",
			"get_fallback_device");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback_sink_flags", PROPERTY_HINT_FLAGS,
						 "Exclusive - take the device:1,"
						 "ExactRate - fail rather than resample:2,"
						 "TightBuffer - smallest device buffer:4,"
						 "DeepBuffer - browser sink only:8"),
			"set_fallback_sink_flags", "get_fallback_sink_flags");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fallback_only_in_editor"),
			"set_fallback_only_in_editor", "get_fallback_only_in_editor");

	ADD_GROUP("Room", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layout_path", PROPERTY_HINT_FILE, "*.json"),
			"set_layout_path", "get_layout_path");
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "registration"), "set_registration",
			"get_registration");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "room_origin",
						 PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"),
			"set_room_origin", "get_room_origin");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_enabled"), "set_ground_enabled",
			"get_ground_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_height"), "set_ground_height",
			"get_ground_height");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ground_material", PROPERTY_HINT_RESOURCE_TYPE,
						 "BwaMaterial"),
			"set_ground_material", "get_ground_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_pressure_release"),
			"set_ground_pressure_release", "get_ground_pressure_release");

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
	/* Needs dual_band: it corrects that low band only. */
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dual_band_cap"), "set_dual_band_cap", "get_dual_band_cap");
	/* SPCAP only. 0 = the default: focus derived from the array geometry, density 2.0. */
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spcap_focus", PROPERTY_HINT_RANGE, "0,64,0.1"),
			"set_spcap_focus", "get_spcap_focus");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spcap_density", PROPERTY_HINT_RANGE, "0,16,0.1"),
			"set_spcap_density", "get_spcap_density");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spread_mode", PROPERTY_HINT_ENUM, "Lobe,MDAP,Spectral"),
			"set_spread_mode", "get_spread_mode");
	ADD_PROPERTY(
			PropertyInfo(Variant::BOOL, "decorrelation"), "set_decorrelation", "get_decorrelation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_spread", PROPERTY_HINT_RANGE, "0,4,0.05"),
			"set_near_spread", "get_near_spread");
	/* Only arrays with HOLES are affected: a surrounding array never derives a floor. */
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hole_spread", PROPERTY_HINT_RANGE, "0,2,0.05"),
			"set_hole_spread", "get_hole_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed_of_sound", PROPERTY_HINT_RANGE,
						 "30,20000,1,or_greater"),
			"set_speed_of_sound", "get_speed_of_sound");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bed_decoder", PROPERTY_HINT_ENUM, "Default,AllRAD,EPAD,Layout"),
			"set_bed_decoder", "get_bed_decoder");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bed_renderer", PROPERTY_HINT_ENUM, "Matrix,Parametric"),
			"set_bed_renderer", "get_bed_renderer");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "max_re"), "set_max_re", "get_max_re");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "max_re_split"), "set_max_re_split",
			"get_max_re_split");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tracked_room_eq"), "set_tracked_room_eq",
			"get_tracked_room_eq");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tracked_directivity"), "set_tracked_directivity",
			"get_tracked_directivity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tracked_align"), "set_tracked_align",
			"get_tracked_align");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tracked_align_dead_zone", PROPERTY_HINT_RANGE,
						"0,0.5,0.005"),
			"set_tracked_align_dead_zone", "get_tracked_align_dead_zone");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tracked_align_slew_frames_per_s", PROPERTY_HINT_RANGE,
						"0,1024,1"),
			"set_tracked_align_slew_frames_per_s", "get_tracked_align_slew_frames_per_s");

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
	BIND_ENUM_CONSTANT(FALLBACK_NONE); BIND_ENUM_CONSTANT(FALLBACK_BINAURAL);
	BIND_ENUM_CONSTANT(FALLBACK_CAVE_SIM);
	BIND_ENUM_CONSTANT(SINK_AUTO); BIND_ENUM_CONSTANT(SINK_ASIO);
	BIND_ENUM_CONSTANT(SINK_NULL); BIND_ENUM_CONSTANT(SINK_MANUAL);
	BIND_ENUM_CONSTANT(SINK_WASAPI); BIND_ENUM_CONSTANT(SINK_COREAUDIO);
	BIND_ENUM_CONSTANT(SINK_ALSA); BIND_ENUM_CONSTANT(SINK_AAUDIO); BIND_ENUM_CONSTANT(SINK_JACK);
	BIND_ENUM_CONSTANT(SINK_WORKLET);
	BIND_ENUM_CONSTANT(SINK_FLAG_NONE); BIND_ENUM_CONSTANT(SINK_FLAG_EXCLUSIVE);
	BIND_ENUM_CONSTANT(SINK_FLAG_EXACT_RATE); BIND_ENUM_CONSTANT(SINK_FLAG_TIGHT_BUFFER);
	BIND_ENUM_CONSTANT(SINK_FLAG_DEEP_BUFFER);
	BIND_ENUM_CONSTANT(DECODE_DEFAULT); BIND_ENUM_CONSTANT(DECODE_ALLRAD); BIND_ENUM_CONSTANT(DECODE_EPAD); BIND_ENUM_CONSTANT(DECODE_LAYOUT);
	BIND_ENUM_CONSTANT(SETUP_DEFAULT); BIND_ENUM_CONSTANT(SETUP_SEATED); BIND_ENUM_CONSTANT(SETUP_ROAMING);
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
	BIND_BITFIELD_FLAG(LOAD_MEMORY); BIND_BITFIELD_FLAG(LOAD_STREAM);
	BIND_BITFIELD_FLAG(LOAD_AMBIX); BIND_BITFIELD_FLAG(LOAD_FUMA);
}
