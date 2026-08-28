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
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"
#include "bwa_client.h"     /* BwaEngineClient: the non-source detach contract */
#include "bwa_material.h"   /* Ref<BwaMaterial>: the ground plane's surface */

namespace godot {

class BwaSource;
class BwaBed;

class BwaEngine : public Node {
	GDCLASS(BwaEngine, Node)

public:
	/* All mirror the C enums 1:1 — the values are ABI, so do not renumber. */
	/* Mirrors bwa_profile: BINAURAL is the direct per-source headphone render, CAVE_SIM the
	 * virtual-speaker array audition, CAVE_BOTH the rig plus that sim tap. */
	enum Profile { PROFILE_CAVE = 0, PROFILE_BINAURAL = 1, PROFILE_CAVE_SIM = 2, PROFILE_CAVE_BOTH = 3 };
	enum Sink { SINK_AUTO = 0, SINK_ASIO = 1, SINK_NULL = 2, SINK_MANUAL = 3 };
	/* Value 0 is RESERVED for default-init, mirroring the C enum: it means the engine's current
	 * default rather than a named algorithm, so the default can move without an ABI break. */
	enum BedDecoder { DECODE_DEFAULT = 0, DECODE_ALLRAD = 1, DECODE_EPAD = 2 };
	enum Setup { SETUP_DEFAULT = 0, SETUP_SEATED = 1, SETUP_ROAMING = 2 };
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
	/* Mirrors bwa_load_flags — a BITFIELD, because the core's cache key is (path, flags) and
	 * the flags pick the loader. LOAD_MEMORY is the zero (decode into RAM). The core refuses
	 * combinations no loader can express (AmbiX with FuMa, streaming with either). */
	enum LoadFlags {
		LOAD_MEMORY = 0, LOAD_STREAM = 1 << 0, LOAD_AMBIX = 1 << 1, LOAD_FUMA = 1 << 2,
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

	/* --- the ground / water-surface plane: the outdoor degenerate of the room box ---
	 * One horizontal mirror plane instead of a shoebox — the ground bounce. Applied between
	 * create and start (load-time, like a BwaRoomBox, which WINS when both exist: one room at
	 * a time). ground_height is a Godot-world height (the registration transform applies);
	 * pressure_release inverts the reflection — a water surface with the listener below it,
	 * the Lloyd's-mirror comb. */
	void set_ground_enabled(bool v) { ground_enabled = v; }
	bool get_ground_enabled() const { return ground_enabled; }
	void set_ground_height(float v) { ground_height = v; }
	float get_ground_height() const { return ground_height; }
	void set_ground_pressure_release(bool v) { ground_pressure_release = v; }
	bool get_ground_pressure_release() const { return ground_pressure_release; }
	void set_ground_material(const Ref<BwaMaterial> &m) { ground_material = m; }
	Ref<BwaMaterial> get_ground_material() const { return ground_material; }

	/* --- the coordinate seam --- */
	void set_registration(const Transform3D &t) { registration = t; }
	Transform3D get_registration() const { return registration; }
	Vector3 to_room_position(const Vector3 &v) const { return registration.xform(v); }
	Vector3 to_room_direction(const Vector3 &v) const { return registration.basis.xform(v); }
	Quaternion to_room_orientation(const Basis &b) const;
	float to_room_yaw(const Basis &b) const;
	Vector3 from_room_position(const Vector3 &v) const;
	Vector3 from_room_direction(const Vector3 &v) const {
		return registration.basis.inverse().xform(v);
	}
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

	/* --- assets ---------------------------------------------------------------------------
	 * The cache is the CORE's now (bwa_sound_acquire / bwa_sound_release, keyed on the same
	 * (path, flags) pair this binding used to spell as a "m:"/"s:"/"a:"/"f:" prefix). This
	 * node no longer dedups and no longer refcounts; it holds exactly one reference per key
	 * it acquired and records that key, because the three PATH-taking calls below have no
	 * ABI to answer them: nothing turns "res://ping.wav" back into a handle from outside.
	 * A player takes the two C++ forms; script takes the bound ones. */
	bwa_sound load_sound(const String &path, bool streaming, bool async = false);
	bwa_sound load_ambisonic(const String &path, bool fuma, bool async = false);
	/* Warm the cache before the first play. `flags` is the LoadFlags bitfield. The _async
	 * form returns as soon as the decode is queued; poll sound_is_ready for the landing. */
	bool preload_sound(const String &path, int flags);
	bool preload_sound_async(const String &path, int flags);
	/* False for a path this node never acquired, for a decode still running, and for one that
	 * FAILED (get_last_error says which of the last two). True is "playable now". */
	bool sound_is_ready(const String &path, int flags) const;
	/* Release this node's reference to EVERY form of `path`. One file can be held as more
	 * than one asset (memory, streamed, ambisonic), and "unload this file" means all of them.
	 * Other holders keep theirs; the core frees on the last release. */
	void unload_sound_path(const String &path);
	/* Metadata for a path already loaded by a player (or a load_* call); 0 for a path this
	 * engine never loaded. Deliberately CACHED-ONLY: the ABI has no metadata-only probe, so
	 * answering for an uncached path would mean a full hidden decode - and always a MONO one,
	 * which reports 1 channel for an ambisonic bed. */
	int64_t sound_get_frames(const String &path);
	int sound_get_channels(const String &path);
	/* Handle-level readiness for a caller that already holds one: 1 = playable, 0 = the async
	 * decode is still running, -1 = it failed (reason in get_last_error). Not bound - script
	 * holds paths, not handles; BwaEmitter/BwaBed hold handles. */
	int sound_ready_state(bwa_sound snd) const;
	/* false = nothing will be heard: the clip failed to load, or the voice pool / command ring
	 * was momentarily full and the transient was dropped. get_last_error() says which. */
	bool play_oneshot(const String &path, const Vector3 &godot_pos, float gain);

	/* --- global mix --- */
	void set_master_gain(float g);
	float get_master_gain() const { return master_gain; }
	void set_paused(bool p);
	bool get_paused() const { return paused; }
	void group_set_gain(int group, float linear);
	void group_set_paused(int group, bool p);
	/* Scene transitions. Both take the click-free one-block fade every stop takes, both stop
	 * beds (a bed is a voice), and both drop the stopped voices' pending queues. Neither
	 * touches group gains, the pause gates, or the master gain: a stop stops sound, it does
	 * not reset the mixer. Both also drop the plays still waiting on an async decode, which
	 * would otherwise start by themselves once their data landed: stop_all drops every one,
	 * group_stop the ones issued on sources in that group. That is what makes the
	 * on_stopped_externally() calls below correct rather than optimistic - clearing an
	 * emitter's pending_async matches a core that really did cancel the play. Both tell the
	 * emitters so a scene change never reads as a `finished`. */
	void group_stop(int group);
	void stop_all();
	void reverb_set_gain(float linear);
	float get_reverb_gain() const { return reverb_gain; }
	void early_reflections_set_gain(float linear);
	float get_early_reflections_gain() const { return er_gain; }

	/* --- rendering A/B (live; the inspector is the A/B tool) --- */
	void set_panner(Panner p);
	Panner get_panner() const { return panner; }
	void set_dual_band(bool on);
	bool get_dual_band() const { return dual_band; }
	/* Compensated amplitude panning on the dual-band low band. INERT unless dual_band is on.
	 * Constrains the interaural component of the summed field to a real source's, using the
	 * tracked head ORIENTATION, so an image holds still as the listener turns. A no-op facing
	 * the source, and it fades out with source spread. */
	/* Situation tuning. get_setup_tuning returns the preset as a Dictionary so it can be PRINTED and
	 * diffed before you commit to it; apply_setup pushes one straight through. */
	godot::Dictionary get_setup_tuning(Setup setup) const;
	/* The engine's CURRENT tuning, so the inspector can be reconciled with live state instead of
	 * shadowing every field by hand. */
	godot::Dictionary get_live_tuning() const;
	/* Handles whose voices ENDED, and handles whose voices WRAPPED at a loop point, as of this
	 * frame's drain. Completion and loop boundaries as EVENTS: BwaEmitter no longer has to guess an
	 * end from is_playing, which cannot see a clip shorter than a frame, and a looping voice never
	 * ends at all so nothing but the wrap can pace a trial off it.
	 *
	 * These return THIS FRAME'S BATCH, not a fresh drain. They are NAMED for that: an earlier
	 * poll_ended() drained destructively, and reusing that name for a non-destructive batch would
	 * have turned a `while poll_ended().size() > 0` loop into a hang with no error. A rename fails
	 * loudly instead. The ABI's bwa_poll_ended/_looped are
	 * destructive and engine-wide, so exactly ONE caller may make them or the readers silently eat
	 * each other's events - and BwaEngine has to be that caller, because it is what routes each
	 * handle to the node that owns it. So the drain happens once per _process (right after the
	 * commit that fills the rings) and both batches are kept here for the frame. Reading them twice
	 * gives the same answer twice; not reading them loses nothing. A script whose process_priority
	 * puts it BEFORE this node sees the previous frame's batch, which is a frame of lag, never a
	 * miss. Most callers want the `finished` / `looped` signals instead. */
	godot::PackedInt64Array get_ended_this_frame() const { return ended_this_frame; }
	godot::PackedInt64Array get_looped_this_frame() const { return looped_this_frame; }
	/* Events the ENGINE dropped because nothing drained them in time (running totals; both rings are
	 * bounded and drop the OLDEST). A rising ended total means that many `finished` signals never
	 * fired and something stalled the frame. A rising loop total can also just mean a loop region
	 * short enough to wrap faster than the frame rate reads it - pace off the wraps you receive. */
	int64_t get_ended_events_dropped() const { return (int64_t)ended_dropped; }
	int64_t get_loop_events_dropped() const { return (int64_t)looped_dropped; }
	bool apply_setup(Setup setup);
	void set_dual_band_cap(bool on);
	bool get_dual_band_cap() const { return dual_band_cap; }
	/* SPCAP's two tuning exponents (inert under DBAP/VBAP; live, and every source re-solves next
	 * block, static ones included). focus = lobe sharpness: higher concentrates a source on fewer
	 * speakers, lower spreads it. density = the placement-correction kernel exponent that de-biases
	 * a clustered array; 2 is the default and is rarely worth moving. 0 or less on EITHER reverts
	 * that one to its default - focus to a value derived from the array geometry (about 12.7 on the
	 * 26-speaker grid, lower on a sparser array), density to 2.0. */
	void set_spcap_focus(float focus);
	float get_spcap_focus() const { return spcap_focus; }
	void set_spcap_density(float density);
	float get_spcap_density() const { return spcap_density; }
	void set_spread_mode(SpreadMode m);
	SpreadMode get_spread_mode() const { return spread_mode; }
	void set_decorrelation(bool on);
	bool get_decorrelation() const { return decorrelation; }
	void set_near_spread(float radius_m);
	float get_near_spread() const { return near_spread; }
	/* Hole-aware spread floor (0 = off). Floors a source's spread by how far its bearing sits from
	 * the NEAREST speaker, so a source aimed where the array has no speaker - the CAVE array is a
	 * barrel, open at both poles - renders as an honest WIDE source instead of a split image across
	 * the two distant speakers that close the hole. The floor stays 0 until the gap exceeds the
	 * array's own mean speaker spacing, so an array that surrounds the listener is inert by
	 * construction. 1 = the honest width, below that a partial widening, above it an exaggeration
	 * (clamped at 2). Live: every source re-solves next block, static ones included. */
	void set_hole_spread(float strength);
	float get_hole_spread() const { return hole_spread; }
	/* Engine-wide speed of sound (m/s; live): Doppler + reflection delays derive from it and
	 * glide to a change. 343 air, 1480 underwater; small values exaggerate Doppler (slow motion).
	 * NOT the layout file's reference.speed_of_sound_mps, which is the ROOM AIR TEMPERATURE the
	 * acoustic survey was measured at (docs/calibration.md, "Air temperature"). That one is
	 * tool-side provenance the engine never reads; this one is the propagation medium and is
	 * yours to drive. Same units, different quantities: setting either does nothing to the other. */
	void set_speed_of_sound(float meters_per_s);
	float get_speed_of_sound() const { return speed_of_sound; }
	void set_max_re(bool on);
	bool get_max_re() const { return max_re; }
	void set_max_re_split(bool on);
	bool get_max_re_split() const { return max_re_split; }
	void set_bed_renderer(BedRenderer r);
	BedRenderer get_bed_renderer() const { return bed_renderer; }
	void set_tracked_room_eq(bool on);
	bool get_tracked_room_eq() const { return tracked_room_eq; }
	/* Tracked listener alignment: re-reference the per-speaker delay/gain trims from the fixed array
	 * centroid onto the TRACKED listener. OFF by default (every delay change resamples). One C call
	 * carries all three, so each setter re-sends the trio, like the SPCAP pair. */
	void set_tracked_align(bool on);
	bool get_tracked_align() const { return tracked_align; }
	void set_tracked_align_dead_zone(float meters);
	float get_tracked_align_dead_zone() const { return tracked_align_dead_zone; }
	void set_tracked_align_slew_frames_per_s(float frames_per_s);
	float get_tracked_align_slew_frames_per_s() const { return tracked_align_slew_frames_per_s; }
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
	/* the ISM shoebox only: leaves the ray-traced static mesh alone, so the box composes */
	void scene_set_ism_room(float w, float h, float d, const PackedInt32Array &faces);
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
	/* Named for the UNIT, like the latency and seek pairs below and for the same reason: the engine
	 * clock is FRAMES, and every host's dsp-time call is seconds (Unity's AudioSettings.dspTime is
	 * a double of seconds; Godot's own AudioServer times are seconds too). Both units are exposed;
	 * neither is spelled the colliding way. get_dsp_time_seconds is a convenience derived from the
	 * frame count and the resolved rate - schedule with the FRAME value, which is exact. */
	int64_t get_dsp_time_frames() const;
	float get_dsp_time_seconds() const;
	/* {valid: bool, dsp_sample: int, host_time_ns: int} — the jitter-free wall<->dsp pair. */
	Dictionary get_clock() const;
	/* Named for the UNIT, deliberately. Godot's own AudioServer.get_output_latency() returns
	 * SECONDS, so a bare get_output_latency() here returning frames reads as seconds to anyone
	 * who knows that call — and the null sink reporting 0 hides the mistake, because 0 is 0 in
	 * either unit. Both units are exposed; neither is spelled the colliding way. */
	int get_output_latency_frames() const;
	float get_output_latency_seconds() const;
	/* {valid: bool, ppm: float, ppm_sigma: float, rate_hz: float, span_s: float, jitter_ns: float,
	 * stamps: int} — the fitted device-vs-host drift, for a timeline you don't own or a rig log. */
	Dictionary get_clock_model() const;

	/* --- device health: was the callback starved, and by whom ---
	 * {measured, blocks, xruns, dropped_frames, driver_resyncs, late_blocks, stream_starves,
	 * peak_load}. `measured` is false when this configuration cannot observe a dropout at all
	 * (no engine, or a sink with no deadline), in which case a 0 xrun count proves nothing. */
	Dictionary get_health() const;
	int64_t get_xruns() const;

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
	/* The focus value spcap_focus = 0 reverts to, derived from the active layout's geometry. Read
	 * it to show what the array itself implies before you override the knob. 0 with no engine. */
	float get_spcap_focus_default() const;
	/* MANUAL sink only: render one block and return it PLANAR (channel-major). Empty
	 * otherwise. The deterministic path — golden renders, offline capture. */
	PackedFloat32Array render_block();

	/* --- static helpers (no engine needed) --- */
	/* Every installed ASIO driver's name, in the order the ABI enumerates them — the values
	 * asio_driver accepts. One call, no index loop; the count/name pair remains for a caller
	 * that wants a single name. */
	static PackedStringArray get_asio_drivers();
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
	/* Non-source nodes holding a back-pointer (beds, speaker views, dynamic geometry): same
	 * detach protocol as sources, minus the per-frame pull. See bwa_client.h. */
	void register_client(BwaEngineClient *c);
	void unregister_client(BwaEngineClient *c);
	/* Beds register a SECOND time, here. A bed is a voice - the core reports its handle through
	 * bwa_poll_ended and bwa_poll_looped like any other - but BwaBed is a Node, not a BwaSource,
	 * because a bed has no position. Rather than force it into the source registry (which exists
	 * to PULL a transform every frame, the one thing a bed has none of), the event drain and the
	 * post-commit pass consult this list too. */
	void register_bed(BwaBed *b);
	void unregister_bed(BwaBed *b);
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

	/* The ONE call site of bwa_poll_ended / bwa_poll_looped (see poll_ended above for why it has
	 * to be exactly one), run right after the commit that fills the rings. Routes each handle to
	 * the registered source that owns it and keeps the batch for the frame. */
	void drain_events();
	/* handle -> registered source, by linear scan. A parallel map would have to be kept in step
	 * with `sources` on every register/unregister/engine_gone, and a map that falls out of step
	 * routes an event to a freed node; the vector IS the registry, so scanning it cannot. Source
	 * counts here are small enough that this is not worth trading for that risk. */
	BwaSource *source_for_handle(uint32_t h) const;
	/* The same lookup over the bed registry. Bed and source handles come out of ONE pool
	 * (bwa_bed_create IS bwa_source_create), so a handle is either a source's or a bed's and
	 * never both - which is what makes consulting the two lists in turn correct. */
	BwaBed *bed_for_handle(uint32_t h) const;

	PackedInt64Array ended_this_frame;   /* this frame's drained batches, kept for            */
	PackedInt64Array looped_this_frame;  /* get_ended/looped_this_frame (see their decls) */
	uint64_t ended_dropped = 0;
	uint64_t looped_dropped = 0;
	bool ended_drop_warned = false;      /* the dropped warnings fire once, not once per frame */
	bool loop_drop_warned = false;

	static int next_generation; /* process-wide; see get_generation */

	bwa_engine *eng = nullptr;
	int generation = 0; /* 0 = never started; assigned from next_generation in _ready */
	std::vector<BwaSource *> sources;
	std::vector<BwaEngineClient *> clients;
	std::vector<BwaBed *> beds; /* the event route; see register_bed */

	/* One record per (path, flags) key this node acquired, holding exactly ONE reference.
	 * A flat vector, not a map: the core does the deduplication and the refcounting now, so
	 * this is a lookup table for the by-path calls and nothing else. Both of those want
	 * every entry for a path anyway, and an asset list is small. */
	struct HeldSound {
		String path;
		uint32_t flags;
		bwa_sound snd;
	};
	std::vector<HeldSound> sounds;

	/* The one acquire path: hit the record, else bwa_sound_acquire(_async) and record it. */
	bwa_sound acquire_sound(const String &path, uint32_t flags, bool async);
	/* Metadata lookup, preferring the point-source forms over the bed ones (a multi-role path
	 * would want the mono answer first); 0 when the path was never acquired. */
	bwa_sound find_loaded_sound(const String &path) const;

	Profile profile = PROFILE_BINAURAL;
	Sink sink = SINK_AUTO;
	BedDecoder bed_decoder = DECODE_DEFAULT;
	String layout_path;
	String asio_driver;
	int sample_rate = 48000;
	int block_size = 256;
	bool embree = false;
	bool enable_pathing = false;

	bwa_reflections_desc refl = {};
	bwa_fdn_desc fdn = {};

	bool ground_enabled = false;            /* the ground/surface plane (see the setters above) */
	float ground_height = 0.0f;
	bool ground_pressure_release = false;
	Ref<BwaMaterial> ground_material;

	Transform3D registration; /* Godot world -> room/Motive origin; identity until surveyed */
	NodePath listener_path;
	bool feed_listener = true; /* false => the core reads NatNet itself (cave/both) */

	float master_gain = 1.0f;
	bool paused = false;
	float reverb_gain = 1.0f;
	float er_gain = 1.0f;
	Panner panner = PAN_DBAP;
	bool dual_band = false;
	bool dual_band_cap = false;
	float spcap_focus = 0.0f;   /* 0 = the geometry-derived default */
	float spcap_density = 0.0f; /* 0 = the 2.0 constant default */
	SpreadMode spread_mode = SPREAD_LOBE;
	bool decorrelation = false;
	float near_spread = 0.0f;
	float hole_spread = 0.0f;   /* 0 = off; only arrays with holes are affected at all */
	float speed_of_sound = 343.0f;
	bool max_re = true;
	bool max_re_split = false;
	BedRenderer bed_renderer = BED_MATRIX;
	bool tracked_room_eq = true;
	bool tracked_align = false;             /* opt-in: a moving delay line resamples the whole array */
	float tracked_align_dead_zone = 0.0f;   /* 0 = the 5 cm default */
	float tracked_align_slew_frames_per_s = 0.0f;        /* 0 = the default (~63 audio frames/s at 48 kHz) */
	bool limiter = true;
	float limiter_ceiling = 0.891251f;   /* -1 dBFS, linear */
	bool headphone_eq_on = true;         /* the headphone-EQ A/B (the loaded file dies with the
	                                      * engine; re-load after a restart, the toggle replays) */
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaEngine::Profile);
VARIANT_ENUM_CAST(godot::BwaEngine::Sink);
VARIANT_ENUM_CAST(godot::BwaEngine::BedDecoder);
VARIANT_ENUM_CAST(godot::BwaEngine::Setup);
VARIANT_ENUM_CAST(godot::BwaEngine::Panner);
VARIANT_ENUM_CAST(godot::BwaEngine::SpreadMode);
VARIANT_ENUM_CAST(godot::BwaEngine::BedRenderer);
VARIANT_ENUM_CAST(godot::BwaEngine::TestKind);
VARIANT_ENUM_CAST(godot::BwaEngine::Material);
VARIANT_ENUM_CAST(godot::BwaEngine::TrackerState);
VARIANT_BITFIELD_CAST(godot::BwaEngine::LoadFlags);
