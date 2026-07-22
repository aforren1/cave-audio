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
#include <godot_cpp/variant/vector3.hpp>

#include "bw_audio.h"

namespace godot {

class BwaEngine;

class BwaSource : public Node3D {
	GDCLASS(BwaSource, Node3D)

public:
	enum Directivity { DIR_OMNI = 0, DIR_CARDIOID = 1, DIR_FIGURE8 = 2 };

	BwaSource() = default;
	~BwaSource() override = default;

	void _ready() override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	/* Called by BwaEngine once per frame, before the listener push and the commit. */
	virtual void push_frame();

	/* --- level / routing --- */
	void set_gain(float g);
	float get_gain() const { return gain; }
	void set_priority(int p);
	int get_priority() const { return priority; }
	void set_group(int g);
	int get_group() const { return group; }
	void fade_to(float target, float seconds);
	void fade_out(float seconds);
	void set_paused(bool p);
	bool get_paused() const { return paused; }

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

	/* --- readbacks --- */
	virtual bool is_playing() const;
	int64_t get_playhead() const;
	double get_playhead_seconds() const;

	void stop();

protected:
	static void _bind_methods();

	/* Subclasses mint their own kind of source (bwa_source_create vs _create_push) and are
	 * told when it is live so they can apply their own state. */
	virtual bwa_source create_source();
	virtual void on_source_ready() {}

	BwaEngine *owner = nullptr;
	bwa_source src = 0;

private:
	void apply_all(); /* re-push authored state onto a freshly created source */

	float gain = 1.0f;
	int priority = 128;
	int group = 0;
	bool paused = false;

	float spread = 0.0f;
	Vector2 extent = Vector2(0.0f, 0.0f);
	float size_m = 0.0f;

	bool doppler = false;
	bool air_absorption = false;
	bool loudness_comp = false;

	bool occlusion = false;
	bool orientation_follows_node = false;

	bool reverb = false;
	float reverb_send = 1.0f;
	bool reverb_distance = false;
	bool early_reflections = false;
	bool pathing = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::BwaSource::Directivity);
