/* BwaEngine — the Godot node that owns the engine handle and drives the frame.
 *
 * Lifecycle: create/start on _ready, stop/destroy on _exit_tree. Never in the editor —
 * _ready() bails on Engine::is_editor_hint(), so opening a scene does not grab a device.
 *
 * The frame: ONE node pushes everything. Each _process it pulls every registered source's
 * transform, pushes the listener pose, then calls bwa_commit once. Letting sources push
 * themselves would risk committing a frame where the listener had moved but some sources
 * had not — the commit is what defines frame coherence (invariant 6), so everything it
 * covers must be sampled together.
 *
 * Godot has no LateUpdate, so freshness is bought with process_priority instead: this node
 * defaults to a high value so it runs after ordinary gameplay nodes. Coherence does not
 * depend on that working — if a source moves after we sampled it, every source and the
 * listener are simply one frame old together, which is inaudible. Only the freshness is at
 * stake, never the consistency.
 *
 * Not bound, deliberately: bwa_set_output_capture. Its callback runs on the AUDIO thread,
 * where calling into GDScript would allocate and take the interpreter lock — exactly what
 * invariant 1 forbids. Use the MANUAL sink and render_block() for offline capture instead.
 */
#pragma once

#include <vector>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"

namespace godot {

class BwaSource;

class BwaEngine : public Node {
	GDCLASS(BwaEngine, Node)

public:
	/* All mirror the C enums 1:1 — the values are ABI, so do not renumber. */
	/* Mirrors bwa_profile: BINAURAL is the direct per-source headphone render, CAVE_SIM the
	 * virtual-speaker array audition, CAVE_BOTH the rig plus that sim tap. */
	enum Profile { PROFILE_CAVE = 0, PROFILE_BINAURAL = 1, PROFILE_CAVE_SIM = 2, PROFILE_CAVE_BOTH = 3 };
	enum Sink { SINK_AUTO = 0, SINK_ASIO = 1, SINK_NULL = 2, SINK_MANUAL = 3 };
	enum BedDecoder { DECODE_ALLRAD = 0, DECODE_EPAD = 1 };
	enum Panner { PAN_DBAP = 0, PAN_SPCAP = 1, PAN_VBAP = 2 };
	enum SpreadMode { SPREAD_LOBE = 0, SPREAD_MDAP = 1, SPREAD_SPECTRAL = 2 };
	enum BedRenderer { BED_MATRIX = 0, BED_PARAMETRIC = 1 };
	enum TestKind { TEST_OFF = 0, TEST_SINE = 1, TEST_NOISE = 2 };
	enum Material {
		MAT_GENERIC = 0, MAT_BRICK, MAT_CONCRETE, MAT_CERAMIC, MAT_GRAVEL, MAT_CARPET,
		MAT_GLASS, MAT_PLASTER, MAT_WOOD, MAT_METAL, MAT_ROCK,
	};
	enum TrackerState {
		TRACKER_DISCONNECTED = 0, TRACKER_NO_DATA = 1, TRACKER_NO_BODY = 2, TRACKER_LIVE = 3,
	};

	/* Godot runs _process in ascending process_priority, so a high default puts the push +
	 * commit after ordinary gameplay nodes. Overridable in the inspector like any node's. */
	BwaEngine() { set_process_priority(1000); }
	~BwaEngine() override = default;

	static BwaEngine *get_singleton() { return singleton; }

	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;

	/* --- device configuration (read at _ready) --- */
	void set_profile(Profile p) { profile = p; }
	Profile get_profile() const { return profile; }
	void set_sink(Sink s) { sink = s; }
	Sink get_sink() const { return sink; }
	void set_layout_path(const String &p) { layout_path = p; }
	String get_layout_path() const { return layout_path; }
	void set_asio_driver(const String &d) { asio_driver = d; }
	String get_asio_driver() const { return asio_driver; }
	void set_sample_rate(int r) { sample_rate = r; }
	int get_sample_rate() const { return sample_rate; }
	void set_block_size(int b) { block_size = b; }
	int get_block_size() const { return block_size; }
	void set_bed_decoder(BedDecoder d) { bed_decoder = d; }
	BedDecoder get_bed_decoder() const { return bed_decoder; }
	void set_embree(bool v) { embree = v; }
	bool get_embree() const { return embree; }
	void set_enable_pathing(bool v) { enable_pathing = v; }
	bool get_enable_pathing() const { return enable_pathing; }

	/* --- reverb configuration (load-time; applied between create and start) --- */
	void set_enable_reflections(bool v) { refl.enabled = v; }
	bool get_enable_reflections() const { return refl.enabled != 0; }
	void set_reflections_ir_seconds(float v) { refl.ir_seconds = v; }
	float get_reflections_ir_seconds() const { return refl.ir_seconds; }
	void set_reflections_order(int v) { refl.order = (uint32_t)v; }
	int get_reflections_order() const { return (int)refl.order; }
	void set_reflections_rays(int v) { refl.num_rays = (uint32_t)v; }
	int get_reflections_rays() const { return (int)refl.num_rays; }
	void set_reflections_bounces(int v) { refl.num_bounces = (uint32_t)v; }
	int get_reflections_bounces() const { return (int)refl.num_bounces; }
	void set_reflections_bake(bool v) { refl.bake = v; }
	bool get_reflections_bake() const { return refl.bake != 0; }

	void set_enable_fdn(bool v) { fdn.enabled = v; }
	bool get_enable_fdn() const { return fdn.enabled != 0; }
	/* The three decay parameters are LIVE: they stage into the desc (a pre-start edit configures
	 * bwa_start) and forward to bwa_fdn_set_decay while running — the tail keeps ringing, only its
	 * slope changes. Structure (enable, anisotropy) stays load-time. */
	void set_fdn_rt60_low(float v);
	float get_fdn_rt60_low() const { return fdn.rt60_low_s; }
	void set_fdn_rt60_high(float v);
	float get_fdn_rt60_high() const { return fdn.rt60_high_s; }
	void set_fdn_xover_hz(float v);
	float get_fdn_xover_hz() const { return fdn.xover_hz; }
	/* Script-facing live retune (the room-transition knob): <= 0 keeps a parameter's value. */
	void fdn_set_decay(float rt60_low_s, float rt60_high_s, float xover_hz);
	void set_fdn_decay_dir(const Vector3 &v);
	Vector3 get_fdn_decay_dir() const;
	void set_fdn_decay_factor(float v) { fdn.decay_factor = v; }
	float get_fdn_decay_factor() const { return fdn.decay_factor; }

	/* --- the coordinate seam --- */
	void set_registration(const Transform3D &t) { registration = t; }
	Transform3D get_registration() const { return registration; }
	Vector3 to_room_position(const Vector3 &v) const { return registration.xform(v); }
	Vector3 to_room_direction(const Vector3 &v) const { return registration.basis.xform(v); }
	Quaternion to_room_orientation(const Basis &b) const;
	float to_room_yaw(const Basis &b) const;
	Vector3 from_room_position(const Vector3 &v) const;
	Quaternion from_room_orientation(const Quaternion &q) const;

	/* --- listener --- */
	void set_listener(const NodePath &p) { listener_path = p; }
	NodePath get_listener() const { return listener_path; }
	void set_feed_listener(bool v) { feed_listener = v; }
	bool get_feed_listener() const { return feed_listener; }
	void set_listener_pose(const Vector3 &pos, const Quaternion &rot);
	/* The pose the engine is actually rendering with, back in Godot space. */
	Transform3D get_listener_transform() const;
	/* Other occupants, for multi-listener compromise panning (up to 3). Godot space. */
	void set_extra_listeners(const PackedVector3Array &positions);
	void set_pose_prediction(float lead_s);

	/* --- tracker (OptiTrack / NatNet) --- */
	int tracker_connect(const String &server, const String &rigid_body_name, int rigid_body_id);
	void tracker_disconnect();
	TrackerState get_tracker_status() const;

	/* --- assets --- */
	bwa_sound load_sound(const String &path, bool streaming);
	bwa_sound load_ambisonic(const String &path, bool fuma);
	void unload_sound_path(const String &path);
	int64_t sound_get_frames(const String &path);
	int sound_get_channels(const String &path);
	void play_oneshot(const String &path, const Vector3 &godot_pos, float gain);

	/* --- global mix --- */
	void set_master_gain(float g);
	float get_master_gain() const { return master_gain; }
	void set_paused(bool p);
	bool get_paused() const { return paused; }
	void group_set_gain(int group, float linear);
	void group_set_paused(int group, bool p);
	void reverb_set_gain(float linear);
	float get_reverb_gain() const { return reverb_gain; }
	void early_reflections_set_gain(float linear);
	float get_early_reflections_gain() const { return er_gain; }

	/* --- rendering A/B (live; the inspector is the A/B tool) --- */
	void set_panner(Panner p);
	Panner get_panner() const { return panner; }
	void set_dual_band(bool on);
	bool get_dual_band() const { return dual_band; }
	void set_spread_mode(SpreadMode m);
	SpreadMode get_spread_mode() const { return spread_mode; }
	void set_decorrelation(bool on);
	bool get_decorrelation() const { return decorrelation; }
	void set_near_spread(float radius_m);
	float get_near_spread() const { return near_spread; }
	/* Engine-wide speed of sound (m/s; live): Doppler + reflection delays derive from it and
	 * glide to a change. 343 air, 1480 underwater; small values exaggerate Doppler (slow motion). */
	void set_speed_of_sound(float meters_per_sec);
	float get_speed_of_sound() const { return speed_of_sound; }
	void set_max_re(bool on);
	bool get_max_re() const { return max_re; }
	void set_max_re_split(bool on);
	bool get_max_re_split() const { return max_re_split; }
	void set_bed_renderer(BedRenderer r);
	BedRenderer get_bed_renderer() const { return bed_renderer; }
	void set_tracked_room_eq(bool on);
	bool get_tracked_room_eq() const { return tracked_room_eq; }
	void set_limiter(bool on);
	bool get_limiter() const { return limiter; }
	void set_limiter_ceiling(float linear);
	float get_limiter_ceiling() const { return limiter_ceiling; }
	/* Headphone correction EQ (an AutoEq ParametricEQ.txt on the headphone stereo): load after
	 * _ready (the engine must exist); returns the bwa_result (0 = OK). "" clears. The toggle is
	 * the ramped live A/B (default on; survives an engine restart via the replay). */
	int load_headphone_eq(const String &path);
	void set_headphone_eq(bool on);
	bool get_headphone_eq() const { return headphone_eq_on; }

	/* --- materials + scene geometry --- */
	int material_preset(Material preset);
	int material_define(const Vector3 &absorption, float scattering, const Vector3 &transmission);
	void material_release(int token);
	/* Shoebox room, floor-based, one material per face in order -x,+x,-y,+y,-z,+z. Captures
	 * the box for the image-source early reflections with OR without the Steam build, so
	 * this is the one scene call a no-SDK build still needs. Live-safe (a room change
	 * re-solves the reflections next block). */
	void scene_set_box(float w, float h, float d, const PackedInt32Array &faces);
	/* The outdoor degenerate of the box: one horizontal mirror plane at Godot-world height y —
	 * the ground bounce. Replaces the box (one room at a time); live-safe like it (reflections
	 * re-solve next block). Set
	 * pressure_release when the plane is a water surface with the listener below it. */
	void scene_set_ground(float y, int material, bool pressure_release);
	/* Flag box faces whose image-source reflection inverts (bit f = face f, -x,+x,-y,+y,-z,+z):
	 * an underwater room's ceiling-as-surface is 1 << 3. Call after scene_set_box/_set_ground. */
	void scene_set_pressure_release(int face_mask);
	void scene_set_mesh(const PackedVector3Array &verts, const PackedInt32Array &tris,
			const PackedInt32Array &tri_materials);
	int scene_add_dynamic_mesh(
			const PackedVector3Array &verts, const PackedInt32Array &tris, int material);
	void scene_set_dynamic_transform(int handle, const Vector3 &godot_pos, const Quaternion &rot);
	void scene_remove_dynamic_mesh(int handle);

	/* --- clock / scheduling --- */
	int64_t get_dsp_time() const;
	/* {valid: bool, dsp_sample: int, host_time_ns: int} — the jitter-free wall<->dsp pair. */
	Dictionary get_clock() const;
	int get_output_latency() const;

	/* --- diagnostics --- */
	void set_test_signal(int channel, TestKind kind, float gain);
	bool is_running() const { return eng != nullptr; }
	String get_audio_backend() const;
	String get_last_error() const;
	int get_channel_count() const;
	int get_resolved_sample_rate() const;
	int get_resolved_block_size() const;
	int get_active_voices() const;
	PackedFloat32Array get_bus_levels() const;
	PackedVector3Array get_speakers() const;
	/* MANUAL sink only: render one block and return it PLANAR (channel-major). Empty
	 * otherwise. The deterministic path — golden renders, offline capture. */
	PackedFloat32Array render_block();

	/* --- static helpers (no engine needed) --- */
	static int get_asio_driver_count();
	static String get_asio_driver_name(int index);
	static int get_version();
	/* The room frame's identity basis, straight from the ABI's own BWA_ROOM_* data. Derive
	 * "move the source to the listener's right" from these rather than writing a sign: room
	 * RIGHT is -X, which is the opposite of the reflex, and a hardcoded guess reads as a
	 * plausible scene that simply has left and right swapped. */
	static Vector3 room_ahead();
	static Vector3 room_up();
	static Vector3 room_right();

	bwa_engine *handle() const { return eng; }
	void register_source(BwaSource *s);
	void unregister_source(BwaSource *s);
	/* A PROCESS-WIDE monotonic id, taken from a static counter on every successful start.
	 * Material tokens belong to the engine instance that issued them, so BwaMaterial keys
	 * its cache on this. It must be process-wide, not per-instance: a per-instance counter
	 * reads 1 on every fresh engine node, so a material cached against a torn-down engine
	 * would match the REBUILT one and hand back a stale token — which the core clamps to
	 * the default material, silently. The playground's rig rebuilds hit exactly that. */
	int get_generation() const { return generation; }
	/* Public because godot-cpp dispatches the virtual from Node, not from the subclass. */
	PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	static BwaEngine *singleton;

	/* Commit the scene tree's acoustic geometry as ONE static mesh, before bwa_start. See
	 * bwa_geometry.h: the core replaces the whole static mesh per call, so collecting is the
	 * only way a room box and separate occluders can coexist. */
	void build_static_scene();

	static int next_generation; /* process-wide; see get_generation */

	bwa_engine *eng = nullptr;
	int generation = 0; /* 0 = never started; assigned from next_generation in _ready */
	std::vector<BwaSource *> sources;
	HashMap<String, bwa_sound> sounds;

	Profile profile = PROFILE_BINAURAL;
	Sink sink = SINK_AUTO;
	BedDecoder bed_decoder = DECODE_ALLRAD;
	String layout_path;
	String asio_driver;
	int sample_rate = 48000;
	int block_size = 256;
	bool embree = false;
	bool enable_pathing = false;

	bwa_reflections_desc refl = {};
	bwa_fdn_desc fdn = {};

	Transform3D registration; /* Godot world -> room/Motive origin; identity until surveyed */
	NodePath listener_path;
	bool feed_listener = true; /* false => the core reads NatNet itself (cave/both) */

	float master_gain = 1.0f;
	bool paused = false;
	float reverb_gain = 1.0f;
	float er_gain = 1.0f;
	Panner panner = PAN_DBAP;
	bool dual_band = false;
	SpreadMode spread_mode = SPREAD_LOBE;
	bool decorrelation = false;
	float near_spread = 0.0f;
	float speed_of_sound = 343.0f;
	bool max_re = false;
	bool max_re_split = false;
	BedRenderer bed_renderer = BED_MATRIX;
	bool tracked_room_eq = true;
	bool limiter = true;
	float limiter_ceiling = 0.891251f;   /* -1 dBFS, linear */
	bool headphone_eq_on = true;         /* the headphone-EQ A/B (the loaded file dies with the
	                                      * engine; re-load after a restart, the toggle replays) */
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaEngine::Profile);
VARIANT_ENUM_CAST(godot::BwaEngine::Sink);
VARIANT_ENUM_CAST(godot::BwaEngine::BedDecoder);
VARIANT_ENUM_CAST(godot::BwaEngine::Panner);
VARIANT_ENUM_CAST(godot::BwaEngine::SpreadMode);
VARIANT_ENUM_CAST(godot::BwaEngine::BedRenderer);
VARIANT_ENUM_CAST(godot::BwaEngine::TestKind);
VARIANT_ENUM_CAST(godot::BwaEngine::Material);
VARIANT_ENUM_CAST(godot::BwaEngine::TrackerState);
