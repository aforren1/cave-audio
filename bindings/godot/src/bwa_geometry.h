/* Acoustic geometry authored in the scene tree.
 *
 * Three nodes, and one constraint that shapes all of them: bwa_scene_set_mesh_mat REPLACES
 * the whole static mesh, and bwa_scene_set_box is a convenience that calls it. So per-node
 * calls would silently clobber each other, last writer winning, and a scene with a room box
 * plus a pillar would lose one of them with no error anywhere.
 *
 * The binding therefore COLLECTS. Every static piece registers itself in _enter_tree — which
 * Godot runs top-down, before any _ready — and BwaEngine merges the lot into ONE call before
 * bwa_start. The room box is applied first (it is the only way to capture the shoebox for
 * the image-source reflections, which works with or without the Steam Audio SDK) and then
 * contributes its own walls to that merged mesh.
 */
#pragma once

#include <vector>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "bw_audio.h"
#include "bwa_client.h"

namespace godot {

class BwaEngine;
class BwaMaterial;

/* Static occluding / reflecting geometry. Committed once, before the engine starts. */
class BwaAcousticGeometry : public Node3D {
	GDCLASS(BwaAcousticGeometry, Node3D)

public:
	BwaAcousticGeometry() = default;
	~BwaAcousticGeometry() override = default;

	void _enter_tree() override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	void set_mesh(const Ref<Mesh> &m) { mesh = m; }
	Ref<Mesh> get_mesh() const { return mesh; }
	void set_source(const NodePath &p) { source = p; }
	NodePath get_source() const { return source; }
	void set_material(const Ref<BwaMaterial> &m) { material = m; }
	Ref<BwaMaterial> get_material() const { return material; }

	/* World-space triangle soup (three vertices per triangle), in Godot coordinates.
	 * BwaEngine converts through the registration transform when it merges. */
	PackedVector3Array world_faces() const;
	Ref<BwaMaterial> material_ref() const { return material; }

	static const std::vector<BwaAcousticGeometry *> &registry() { return statics; }

protected:
	static void _bind_methods();

private:
	Ref<Mesh> resolve_mesh() const;

	static std::vector<BwaAcousticGeometry *> statics;

	Ref<Mesh> mesh;
	NodePath source; /* a MeshInstance3D to take the mesh AND transform from; overrides `mesh` */
	Ref<BwaMaterial> material;
};

/* A movable occluder: its own sub-scene instance, so moving it is a cheap BVH refit rather
 * than a geometry rebuild. Needs the Steam Audio build; without it the handle stays -1 and
 * the node is inert (which the configuration warning says out loud). */
class BwaDynamicGeometry : public Node3D, public BwaEngineClient {
	GDCLASS(BwaDynamicGeometry, Node3D)

public:
	BwaDynamicGeometry() = default;
	~BwaDynamicGeometry() override = default;

	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	/* Called by BwaEngine on ITS teardown while this mover still lives (see bwa_client.h). */
	void engine_gone() override;

	void set_mesh(const Ref<Mesh> &m) { mesh = m; }
	Ref<Mesh> get_mesh() const { return mesh; }
	void set_material(const Ref<BwaMaterial> &m) { material = m; }
	Ref<BwaMaterial> get_material() const { return material; }
	bool is_attached() const { return handle >= 0; }

protected:
	static void _bind_methods();

private:
	BwaEngine *owner = nullptr;
	int handle = -1;
	Transform3D pushed;

	Ref<Mesh> mesh;
	Ref<BwaMaterial> material;
};

/* The shoebox the virtual scene lives in: floor-based, centered on this node.
 *
 * Do NOT model the physical CAVE with this. Its box is the VIRTUAL environment; the real
 * room supplies its own reflections, and modeling it double-counts.
 */
class BwaRoomBox : public Node3D {
	GDCLASS(BwaRoomBox, Node3D)

public:
	BwaRoomBox() = default;
	~BwaRoomBox() override = default;

	void _enter_tree() override;
	void _exit_tree() override;
	PackedStringArray _get_configuration_warnings() const override;

	void set_size(const Vector3 &s) { size = s; }
	Vector3 get_size() const { return size; }
	/* Face order is -x, +x, -y (floor), +y (ceiling), -z, +z. */
	void set_face_material(int face, const Ref<BwaMaterial> &m);
	Ref<BwaMaterial> get_face_material(int face) const;
	void set_all_materials(const Ref<BwaMaterial> &m);
	Ref<BwaMaterial> get_all_materials() const { return all; }

	Vector3 room_size() const { return size; }
	/* The same 12 inward-facing triangles the core builds for its own box, so the walls can
	 * join the merged static mesh instead of being replaced by it. */
	PackedVector3Array wall_faces() const;
	Ref<BwaMaterial> face_material_for_triangle(int tri) const;

	static BwaRoomBox *active() { return current; }

protected:
	static void _bind_methods();

private:
	static BwaRoomBox *current;

	Vector3 size = Vector3(6.0f, 3.0f, 8.0f);
	Ref<BwaMaterial> all;              /* used for any face without its own material */
	Ref<BwaMaterial> faces[6];
};

} // namespace godot
