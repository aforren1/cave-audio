#include "bwa_geometry.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "bwa_engine_node.h"
#include "bwa_material.h"

using namespace godot;

std::vector<BwaAcousticGeometry *> BwaAcousticGeometry::statics;
BwaRoomBox *BwaRoomBox::current = nullptr;

/* ---------------------------------------------------------------- static geometry --- */

Ref<Mesh> BwaAcousticGeometry::resolve_mesh() const {
	if (!source.is_empty()) {
		if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(get_node_or_null(source))) {
			return mi->get_mesh();
		}
	}
	return mesh;
}

PackedVector3Array BwaAcousticGeometry::world_faces() const {
	PackedVector3Array out;
	const Ref<Mesh> m = resolve_mesh();
	if (m.is_null()) {
		return out;
	}
	/* get_faces() hands back a flat triangle soup — three vertices per triangle, already
	 * tessellated — which is exactly the shape the core wants, so no index bookkeeping. */
	const PackedVector3Array local = m->get_faces();

	Transform3D xform = get_global_transform();
	if (!source.is_empty()) {
		if (Node3D *n = Object::cast_to<Node3D>(get_node_or_null(source))) {
			xform = n->get_global_transform(); // the mesh's own placement, not this node's
		}
	}

	out.resize(local.size());
	for (int i = 0; i < local.size(); i++) {
		out[i] = xform.xform(local[i]);
	}
	return out;
}

void BwaAcousticGeometry::_enter_tree() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	statics.push_back(this);
}

void BwaAcousticGeometry::_exit_tree() {
	for (size_t i = 0; i < statics.size(); i++) {
		if (statics[i] == this) {
			statics.erase(statics.begin() + (long)i);
			return;
		}
	}
}

PackedStringArray BwaAcousticGeometry::_get_configuration_warnings() const {
	PackedStringArray w;
	if (resolve_mesh().is_null()) {
		w.push_back("No mesh: set `mesh`, or point `source` at a MeshInstance3D. This node is "
					"contributing nothing to the acoustic scene.");
	}
	if (material.is_null()) {
		w.push_back("No material, so these surfaces use the generic default. That is a valid "
					"choice, but an easy one to make by accident.");
	}
	return w;
}

void BwaAcousticGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &BwaAcousticGeometry::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &BwaAcousticGeometry::get_mesh);
	ClassDB::bind_method(D_METHOD("set_source", "path"), &BwaAcousticGeometry::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &BwaAcousticGeometry::get_source);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &BwaAcousticGeometry::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &BwaAcousticGeometry::get_material);
	ClassDB::bind_method(D_METHOD("world_faces"), &BwaAcousticGeometry::world_faces);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"),
			"set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source", PROPERTY_HINT_NODE_PATH_VALID_TYPES,
						 "MeshInstance3D"),
			"set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE,
						 "BwaMaterial"),
			"set_material", "get_material");
}

/* --------------------------------------------------------------- dynamic geometry --- */

void BwaDynamicGeometry::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	owner = BwaEngine::get_singleton();
	if (!owner || !owner->is_running() || mesh.is_null()) {
		return;
	}

	/* Dynamic geometry lives in the MOVER's own space, so it takes no registration here —
	 * only the placement below crosses the seam. */
	const PackedVector3Array faces = mesh->get_faces();
	PackedVector3Array verts;
	PackedInt32Array tris;
	verts.resize(faces.size());
	tris.resize(faces.size());
	for (int i = 0; i < faces.size(); i++) {
		verts[i] = faces[i];
		tris[i] = i;
	}

	const bwa_material tok = material.is_valid() ? material->token(owner) : 0;
	handle = owner->scene_add_dynamic_mesh(verts, tris, (int)tok);
	if (handle >= 0) {
		pushed = get_global_transform();
		owner->scene_set_dynamic_transform(
				handle, pushed.origin, Quaternion(pushed.basis.orthonormalized()));
		set_process(true);
	}
}

void BwaDynamicGeometry::_process(double delta) {
	(void)delta;
	if (handle < 0 || !owner || !owner->is_running()) {
		return;
	}
	const Transform3D t = get_global_transform();
	if (t == pushed) {
		return; // a refit costs something; do not pay it for a stationary mover
	}
	pushed = t;
	owner->scene_set_dynamic_transform(handle, t.origin, Quaternion(t.basis.orthonormalized()));
}

void BwaDynamicGeometry::_exit_tree() {
	if (handle >= 0 && owner && owner->is_running()) {
		owner->scene_remove_dynamic_mesh(handle);
	}
	handle = -1;
	owner = nullptr;
}

PackedStringArray BwaDynamicGeometry::_get_configuration_warnings() const {
	PackedStringArray w;
	if (mesh.is_null()) {
		w.push_back("No mesh: this mover has no geometry and will occlude nothing.");
	}
	w.push_back("Dynamic geometry needs the Steam Audio build. Without it this node is inert, "
				"and BAKED reflections ignore it either way - the bake froze the scene.");
	return w;
}

void BwaDynamicGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &BwaDynamicGeometry::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &BwaDynamicGeometry::get_mesh);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &BwaDynamicGeometry::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &BwaDynamicGeometry::get_material);
	ClassDB::bind_method(D_METHOD("is_attached"), &BwaDynamicGeometry::is_attached);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"),
			"set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE,
						 "BwaMaterial"),
			"set_material", "get_material");
}

/* ----------------------------------------------------------------------- room box --- */

/* The core's own box: floor-based (x/z centred, y from 0 up to h), face order -x, +x, -y,
 * +y, -z, +z, wound so the normals face INWARD because the listener is inside. Mirrored
 * here so the walls can join the merged static mesh — see the header for why they cannot
 * simply be left to bwa_scene_set_box. */
static const int BWA_BOX_QUAD[6][4] = {
	{ 0, 4, 7, 3 }, { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 3, 2, 1 }, { 4, 5, 6, 7 }
};

PackedVector3Array BwaRoomBox::wall_faces() const {
	const float hw = (float)size.x * 0.5f, h = (float)size.y, hd = (float)size.z * 0.5f;
	const Vector3 v[8] = {
		Vector3(-hw, 0, -hd), Vector3(hw, 0, -hd), Vector3(hw, h, -hd), Vector3(-hw, h, -hd),
		Vector3(-hw, 0, hd), Vector3(hw, 0, hd), Vector3(hw, h, hd), Vector3(-hw, h, hd)
	};
	const Vector3 centre(0, h * 0.5f, 0);
	const Transform3D xform = get_global_transform();

	PackedVector3Array out;
	for (int f = 0; f < 6; f++) {
		const int a = BWA_BOX_QUAD[f][0], b = BWA_BOX_QUAD[f][1];
		const int c = BWA_BOX_QUAD[f][2], d = BWA_BOX_QUAD[f][3];
		const int quad[2][3] = { { a, b, c }, { a, c, d } };
		for (int t = 0; t < 2; t++) {
			Vector3 p0 = v[quad[t][0]], p1 = v[quad[t][1]], p2 = v[quad[t][2]];
			/* Flip any triangle whose normal points away from the room centre, exactly as
			 * the core's emit_inward does — the winding is what makes it an enclosure. */
			if ((p1 - p0).cross(p2 - p0).dot(centre - p0) < 0.0f) {
				const Vector3 tmp = p1;
				p1 = p2;
				p2 = tmp;
			}
			out.push_back(xform.xform(p0));
			out.push_back(xform.xform(p1));
			out.push_back(xform.xform(p2));
		}
	}
	return out;
}

Ref<BwaMaterial> BwaRoomBox::face_material_for_triangle(int tri) const {
	return get_face_material(tri / 2); // two triangles per face, in face order
}

void BwaRoomBox::set_face_material(int face, const Ref<BwaMaterial> &m) {
	if (face >= 0 && face < 6) {
		faces[face] = m;
	}
}

Ref<BwaMaterial> BwaRoomBox::get_face_material(int face) const {
	if (face < 0 || face >= 6) {
		return all;
	}
	return faces[face].is_valid() ? faces[face] : all;
}

void BwaRoomBox::set_all_materials(const Ref<BwaMaterial> &m) { all = m; }

void BwaRoomBox::_enter_tree() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (current && current != this) {
		UtilityFunctions::push_error(
				"BwaRoomBox: a second room box entered the tree. There is one shoebox; this one is "
				"ignored.");
		return;
	}
	current = this;
	if (BwaEngine::get_singleton() && BwaEngine::get_singleton()->is_running()) {
		/* The engine collects room boxes before bwa_start because the image-source handoff
		 * assumes the audio thread is not yet running. Arriving late is not recoverable. */
		UtilityFunctions::push_error(
				"BwaRoomBox: added after the engine started. The room box is load-time only - put "
				"it in the scene alongside BwaEngine.");
	}
}

void BwaRoomBox::_exit_tree() {
	if (current == this) {
		current = nullptr;
	}
}

PackedStringArray BwaRoomBox::_get_configuration_warnings() const {
	PackedStringArray w;
	if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f) {
		w.push_back("Every dimension must be positive; the engine rejects a degenerate box.");
	}
	if (!get_transform().is_equal_approx(Transform3D())) {
		/* The split failure: wall_faces() bakes this node's transform into the merged
		 * ray-traced mesh, but the core's ISM shoebox is origin-anchored by design
		 * (bwa_scene_set_box takes only w/h/d) — so a moved box leaves early reflections
		 * bouncing off a DIFFERENT room than occlusion sees. */
		w.push_back("This node's transform moves the ray-traced walls but NOT the image-source "
					"shoebox, which the engine anchors at the room origin (floor-based). The two "
					"acoustics paths will disagree about where the room is. Keep this node at "
					"the identity transform and size the box instead.");
	}
	w.push_back("This box is the VIRTUAL room, not the physical CAVE. Modelling the real room "
				"double-counts its reflections, which already reach the listener acoustically.");
	return w;
}

void BwaRoomBox::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &BwaRoomBox::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &BwaRoomBox::get_size);
	ClassDB::bind_method(
			D_METHOD("set_face_material", "face", "material"), &BwaRoomBox::set_face_material);
	ClassDB::bind_method(D_METHOD("get_face_material", "face"), &BwaRoomBox::get_face_material);
	ClassDB::bind_method(
			D_METHOD("set_all_materials", "material"), &BwaRoomBox::set_all_materials);
	ClassDB::bind_method(D_METHOD("get_all_materials"), &BwaRoomBox::get_all_materials);
	ClassDB::bind_method(D_METHOD("wall_faces"), &BwaRoomBox::wall_faces);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "all_materials", PROPERTY_HINT_RESOURCE_TYPE,
						 "BwaMaterial"),
			"set_all_materials", "get_all_materials");
}
