#include "bwa_speaker_view.h"

#include <cmath>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"

using namespace godot;

void BwaSpeakerView::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	owner = BwaEngine::get_singleton();
	if (!owner || !owner->is_running()) {
		UtilityFunctions::push_warning(
				"BwaSpeakerView: no running BwaEngine — nothing to draw.");
		return;
	}

	/* Sized by the layout the engine actually resolved, not by BWA_CHANNELS. */
	const PackedVector3Array speakers = owner->get_speakers();
	Ref<SphereMesh> mesh;
	mesh.instantiate();
	mesh->set_radius(gizmo_radius);
	mesh->set_height(gizmo_radius * 2.0f);
	mesh->set_radial_segments(12);
	mesh->set_rings(6);

	for (int i = 0; i < speakers.size(); i++) {
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mat->set_albedo(idle_color);
		mat->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
		mat->set_emission(idle_color);
		mat->set_emission_energy_multiplier(0.0f);

		MeshInstance3D *gz = memnew(MeshInstance3D);
		gz->set_mesh(mesh);
		gz->set_material_override(mat);
		gz->set_name(vformat("Speaker%d", i));
		add_child(gz);

		/* Speaker positions come back in ROOM space; the view lives in the scene, so they
		 * come back through the registration transform. */
		const Vector3 p = owner->from_room_position(speakers[i]);
		gz->set_position(p);

		gizmos.push_back(gz);
		materials.push_back(mat);
		positions.push_back(p);
	}
	set_process(true);
}

void BwaSpeakerView::_process(double delta) {
	(void)delta;
	if (!owner || !owner->is_running() || gizmos.empty()) {
		return;
	}
	const PackedFloat32Array peaks = owner->get_bus_levels();
	const int n = MIN(peaks.size(), (int)gizmos.size());
	for (int i = 0; i < n; i++) {
		/* Peaks are linear; ears and meters both want dB, and a linear ramp spends almost
		 * all its range on the loudest few dB. */
		const float db = peaks[i] > 0.0f ? 20.0f * std::log10(peaks[i]) : -120.0f;
		const float t = CLAMP((db - floor_db) / -floor_db, 0.0f, 1.0f);
		materials[i]->set_albedo(idle_color.lerp(active_color, t));
		materials[i]->set_emission(active_color);
		materials[i]->set_emission_energy_multiplier(t * 2.0f);
	}
}

int BwaSpeakerView::nearest_speaker(const Vector3 &godot_pos) const {
	int best = -1;
	float best_d = 0.0f;
	for (size_t i = 0; i < positions.size(); i++) {
		const float d = (float)positions[i].distance_squared_to(godot_pos);
		if (best < 0 || d < best_d) {
			best = (int)i;
			best_d = d;
		}
	}
	return best;
}

PackedStringArray BwaSpeakerView::_get_configuration_warnings() const {
	PackedStringArray w;
	w.push_back("Speaker positions are read back from the running engine, so this node draws "
				"nothing in the editor. It also draws the layout the engine RESOLVED — if that "
				"is the 26-speaker default grid, the surveyed layout did not load.");
	return w;
}

void BwaSpeakerView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_gizmo_radius", "r"), &BwaSpeakerView::set_gizmo_radius);
	ClassDB::bind_method(D_METHOD("get_gizmo_radius"), &BwaSpeakerView::get_gizmo_radius);
	ClassDB::bind_method(D_METHOD("set_idle_color", "color"), &BwaSpeakerView::set_idle_color);
	ClassDB::bind_method(D_METHOD("get_idle_color"), &BwaSpeakerView::get_idle_color);
	ClassDB::bind_method(D_METHOD("set_active_color", "color"), &BwaSpeakerView::set_active_color);
	ClassDB::bind_method(D_METHOD("get_active_color"), &BwaSpeakerView::get_active_color);
	ClassDB::bind_method(D_METHOD("set_floor_db", "db"), &BwaSpeakerView::set_floor_db);
	ClassDB::bind_method(D_METHOD("get_floor_db"), &BwaSpeakerView::get_floor_db);
	ClassDB::bind_method(D_METHOD("get_speaker_count"), &BwaSpeakerView::get_speaker_count);
	ClassDB::bind_method(D_METHOD("nearest_speaker", "position"), &BwaSpeakerView::nearest_speaker);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gizmo_radius", PROPERTY_HINT_RANGE, "0.01,1,0.01"),
			"set_gizmo_radius", "get_gizmo_radius");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "idle_color"), "set_idle_color", "get_idle_color");
	ADD_PROPERTY(
			PropertyInfo(Variant::COLOR, "active_color"), "set_active_color", "get_active_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_db", PROPERTY_HINT_RANGE, "-120,-6,1"),
			"set_floor_db", "get_floor_db");
}
