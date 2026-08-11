/* BwaSpeakerView — one gizmo per speaker, lit by that channel's live output level.
 *
 * The array's own geometry, read back from the engine rather than authored: it draws the
 * speakers the engine is ACTUALLY panning with, so a layout that failed to load and fell
 * back to the default grid looks wrong immediately instead of sounding wrong later.
 *
 * The count comes from the layout, never from a constant. BWA_CHANNELS (26) is the
 * compile-time capacity; a 24-speaker rig loads into the same binary.
 */
#pragma once

#include <vector>

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/color.hpp>

#include "bwa_client.h"

namespace godot {

class BwaEngine;

class BwaSpeakerView : public Node3D, public BwaEngineClient {
	GDCLASS(BwaSpeakerView, Node3D)

public:
	BwaSpeakerView() = default;
	~BwaSpeakerView() override = default;

	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	/* Called by BwaEngine on ITS teardown while this view still lives (see bwa_client.h);
	 * without it the next _process tick dereferences the freed engine node. */
	void engine_gone() override;

	void set_gizmo_radius(float r) { gizmo_radius = r; }
	float get_gizmo_radius() const { return gizmo_radius; }
	void set_idle_color(const Color &c) { idle_color = c; }
	Color get_idle_color() const { return idle_color; }
	void set_active_color(const Color &c) { active_color = c; }
	Color get_active_color() const { return active_color; }
	/* Peak level, in dB below which a speaker reads as silent. */
	void set_floor_db(float db) { floor_db = db; }
	float get_floor_db() const { return floor_db; }

	int get_speaker_count() const { return (int)gizmos.size(); }
	/* Which speaker a point is nearest, in Godot space — the channel-walk check's question. */
	int nearest_speaker(const Vector3 &godot_pos) const;

protected:
	static void _bind_methods();

private:
	BwaEngine *owner = nullptr;
	std::vector<MeshInstance3D *> gizmos;
	std::vector<Ref<StandardMaterial3D>> materials;
	std::vector<Vector3> positions; /* Godot space, for nearest_speaker */

	float gizmo_radius = 0.12f;
	Color idle_color = Color(0.25f, 0.28f, 0.35f);
	Color active_color = Color(1.0f, 0.75f, 0.2f);
	float floor_db = -60.0f;
};

} // namespace godot
