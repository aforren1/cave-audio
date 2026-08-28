/* BwaSource — everything a spatial voice can do, independent of where its audio comes from.
 *
 * Abstract: BwaEmitter plays files, BwaPushSource is fed PCM. They are separate classes
 * rather than one node with a mode flag because the core genuinely refuses play/seek/pitch
 * on a push source — a mode flag would leave those visible in the inspector and silently
 * inert, which is the class of quiet failure this binding tries to make unrepresentable.
 *
 * The node does NOT push its own position; BwaEngine pulls from every registered source in
 * one place, then the listener, then commits (invariant 6). See bwa_engine_node.h.
 */
#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"

namespace godot {

class BwaEngine;

class BwaSource : public Node3D {
	GDCLASS(BwaSource, Node3D)

public:
	enum Directivity { DIR_OMNI = 0, DIR_CARDIOID = 1, DIR_FIGURE8 = 2 };
	/* Mirrors bwa_source_kind: what the source IS, which is what a preset is named for. */
	enum Kind {
		KIND_DEFAULT = 0, KIND_PROP = 1, KIND_VOICE = 2, KIND_AMBIENCE = 3, KIND_UI = 4,
	};

	BwaSource() = default;
	~BwaSource() override = default;

	void _ready() override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	/* Called by BwaEngine once per frame, before the listener push and the commit. */
	virtual void push_frame();
	/* Called by BwaEngine once per frame, AFTER the commit and after both event drains. Anything
	 * that reasons about "did this voice stop" belongs here: the events are filled by the same
	 * pass bwa_commit runs, so a pre-commit reading is about the previous frame. */
	virtual void post_commit() {}
	/* One handle bwa_poll_ended / bwa_poll_looped reported, routed here by BwaEngine (the single
	 * owner of both drains - see BwaEngine::get_ended_this_frame). Base does nothing: only BwaEmitter has
	 * the signals. A wrap is NOT an end - the voice keeps playing - so the two never interact. */
	virtual void notify_ended() {}
	virtual void notify_looped() {}
	/* The native handle, for BwaEngine's event routing. 0 when the source was never created. */
	bwa_source native_handle() const { return src; }

	/* --- level / routing --- */
	void set_gain(float g);
	float get_gain() const { return gain; }
	void set_priority(int p);
	int get_priority() const { return priority; }
	void set_group(int g);
	int get_group() const { return group; }
	void fade_to(float target, float seconds);
	/* Virtual, with stop() below: BwaEmitter overrides both to mark the halt as EXPLICIT, so
	 * its end detector does not read the voice going silent as a natural end and fire
	 * `finished`. That reset lived in the emitter's own stop() before these moved down here,
	 * and losing it made every stop announce a finish. */
	virtual void fade_out(float seconds);
	void set_paused(bool p);
	bool get_paused() const { return paused; }

	/* --- direct output-channel route ---
	 * Send this voice out of exactly ONE output channel with no spatial processing: the
	 * psychophysics ground-truth condition (one real speaker, A/B'd against a phantom) and a
	 * wiring check you can run with real content. Valid range is 0 to
	 * BwaEngine.get_channel_count() - 1; CHANNEL_AUTO (-1) puts it back on the panner. An
	 * out-of-range channel is refused with a warning and the source keeps the channel it had, so
	 * get_channel() never reports a route the voice is not actually on. CHANNEL_AUTO is the only
	 * negative that means anything: every other one is refused too, and refused BEFORE the engine
	 * is live, because a negative needs no channel count to judge.
	 *
	 * NOT set_test_signal: that injects a built-in tone AFTER the align stage, which makes it a
	 * wiring tool and not level-comparable with a rendered source. This replaces the panner's gain
	 * vector, so the routed voice keeps the whole output stage a panned voice gets (align trims and
	 * delays, room EQ, master gain, limiter) and RAMPS in and out. Everything distance- or
	 * direction-derived is suppressed while it is on (attenuation, spread, occlusion, reverb sends,
	 * Doppler, air absorption) and takes effect again the moment you go back to CHANNEL_AUTO; pitch
	 * and pause still apply. Mono point sources only.
	 *
	 * No unit suffix: a channel is a bare index, not a quantity. Not an inspector property either -
	 * this is a run-time experimental condition, not authored configuration - but it IS replayed if
	 * the source is re-created, so a reference source keeps its channel across an engine rebuild. */
	static constexpr int CHANNEL_AUTO = BWA_CHANNEL_AUTO;
	void set_channel(int channel);
	int get_channel() const { return channel; }

	/* --- extent --- */
	void set_spread(float amount);
	float get_spread() const { return spread; }
	void set_extent(const Vector2 &wh);
	Vector2 get_extent() const { return extent; }
	void set_size(float radius_m);
	float get_size() const { return size_m; }

	/* --- propagation --- */
	void set_doppler(bool on);
	bool get_doppler() const { return doppler; }
	void set_air_absorption(bool on);
	bool get_air_absorption() const { return air_absorption; }
	void set_loudness_comp(bool on);
	bool get_loudness_comp() const { return loudness_comp; }
	void set_proximity(bool on);
	bool get_proximity() const { return proximity; }
	/* ref_dist <= 0 clears the override and returns the source to the layout's curve. */
	void set_attenuation_override(float ref_dist, float rolloff, float min_gain);

	/* --- occlusion / directivity (ray-traced paths need the Steam Audio build) --- */
	void set_occlusion(bool on);
	bool get_occlusion() const { return occlusion; }
	void set_occlusion_manual(float level);
	void set_occlusion_manual_bands(float level, const Vector3 &bands);
	float get_occlusion_factor() const;
	void set_directivity(float weight, float power);
	void set_directivity_preset(Directivity pattern);
	float get_directivity_gain() const;
	/* Godot-frame, converted through the facing seam before it reaches the ABI. */
	void set_orientation(const Quaternion &q);
	/* Push this node's own rotation as the directivity axis every frame — the reason a
	 * source is a Node3D rather than a position. */
	void set_orientation_follows_node(bool v) { orientation_follows_node = v; }
	bool get_orientation_follows_node() const { return orientation_follows_node; }

	/* --- reverb / reflections --- */
	void set_reverb(bool on);
	bool get_reverb() const { return reverb; }
	void set_reverb_send(float g);
	float get_reverb_send() const { return reverb_send; }
	void set_reverb_distance(bool on);
	bool get_reverb_distance() const { return reverb_distance; }
	void set_early_reflections(bool on);
	bool get_early_reflections() const { return early_reflections; }
	void set_pathing(bool on);
	bool get_pathing() const { return pathing; }

	/* --- configuration as one value (bwa_source_desc) --------------------------------------
	 * The same shape BwaEngine::get_setup_tuning uses for the engine knobs, and for the same
	 * reason: a Dictionary can be PRINTED and diffed, where a struct behind 24 setters cannot.
	 *
	 * WHAT IS IN: configuration, the settings that describe the source. WHAT IS OUT: position
	 * and orientation (per-frame, pushed by BwaEngine every commit), playback state (what the
	 * source is DOING, not what it is - an apply must never restart a sound), and the manual
	 * occlusion LEVEL, which is a per-frame measurement the game publishes.
	 *
	 * get_preset is pure, so a tool can print the table with no engine in the scene. */
	static Dictionary get_preset(Kind kind);
	/* What this source is configured at. Reads the engine back when live, the node's authored
	 * state otherwise, so it answers in the editor too. Round-trips through apply_desc. */
	Dictionary get_desc() const;
	/* Overlay: only the keys PRESENT are changed, the rest keep their current value. So
	 * apply_desc({"gain": 0.5}) is a one-field edit and get_desc() -> apply_desc() is a no-op.
	 * An unknown key is a warning, not a silent miss. False = the engine refused the desc. */
	bool apply_desc(const Dictionary &d);
	/* The reset the API had no way to express: fill the preset for `kind` and push the lot.
	 * Every configured field goes back to that preset, authored inspector values included. */
	bool reset_to_preset(Kind kind);

	/* --- readbacks --- */
	virtual bool is_playing() const;
	int64_t get_playhead_frames() const;
	double get_playhead_seconds() const;

	virtual void stop(); /* see fade_out */

	/* Called by BwaEngine on ITS teardown for every still-registered source: the engine node
	 * can be freed while sources live elsewhere in the tree, and without this their `owner`
	 * back-pointer dangles — the next setter on the source is then a use-after-free. */
	void engine_gone();
	/* Something outside this node stopped the voice (BwaEngine::group_stop / stop_all), so it
	 * is called BY the engine like push_frame. An emitter uses it to keep its end detector
	 * from reading a scene change as a finish. */
	virtual void on_stopped_externally() {}

protected:
	static void _bind_methods();

	/* Subclasses mint their own kind of source (bwa_source_create_desc vs _create_push) and
	 * are told when it is live so they can apply their own state. */
	virtual bwa_source create_source();
	virtual void on_source_ready() {}
	/* The node's authored configuration as a desc, and the way back. Virtual because `pitch`
	 * belongs to the desc but only BwaEmitter owns a pitch (the core refuses it on a push
	 * voice), so the emitter extends both halves. Always start from bwa_source_preset: this
	 * struct's zero is not its default. */
	virtual void fill_desc(bwa_source_desc *d) const;
	virtual void mirror_desc(const bwa_source_desc &d);

	BwaEngine *owner = nullptr;
	bwa_source src = 0;

private:
	void apply_all(); /* re-push authored state onto a freshly created source */
	/* Mirror `d` into the node's caches (so the inspector does not lie) and push it if the
	 * source is live. Not live = authored early, and apply_all replays it at _ready. */
	bool push_desc(const bwa_source_desc &d);
	void push_orientation(const Quaternion &q); /* the facing seam, applied in one place */

	float gain = 1.0f;
	int priority = 128;
	int group = 0;
	bool paused = false;

	int channel = CHANNEL_AUTO; /* direct output-channel route; see set_channel */
	float spread = 0.0f;
	Vector2 extent = Vector2(0.0f, 0.0f);
	float size_m = 0.0f;
	/* attenuation override cache: ref_dist <= 0 = none, mirroring the native clear semantic */
	float atten_ref_dist = 0.0f, atten_rolloff = 0.0f, atten_min_gain = 0.0f;

	bool doppler = false;
	bool air_absorption = false;
	bool loudness_comp = false;
	bool proximity = false;

	bool occlusion = false;
	bool orientation_follows_node = false;
	/* Standing-knob caches with has-been-set flags: each "unset" state is a live core default
	 * the binding must not restate, so only an authored value replays through apply_all(). */
	bool occ_manual_set = false, occ_manual_banded = false;
	float occ_manual_level = 1.0f;
	Vector3 occ_manual_bands = Vector3(1.0f, 1.0f, 1.0f);
	enum DirSet { DIRSET_NONE = 0, DIRSET_CUSTOM, DIRSET_PRESET };
	DirSet dir_mode = DIRSET_NONE; /* remembers WHICH spelling was used (last one wins) */
	float dir_weight = 0.0f, dir_power = 1.0f;
	Directivity dir_preset = DIR_OMNI;
	bool orientation_set = false;
	Quaternion orientation_q; /* Godot frame; converted at push time, when `owner` exists */

	bool reverb = false;
	float reverb_send = 1.0f;
	bool reverb_distance = false;
	bool early_reflections = false;
	bool pathing = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaSource::Directivity);
VARIANT_ENUM_CAST(godot::BwaSource::Kind);
