## Immediate-mode 3D overlay: lines and spheres, rebuilt every frame.
##
## The C++ playground gets DrawLine3D/DrawSphere from raylib. Godot has no immediate 3D
## drawing, so lines go into one ImmediateMesh (cheap, one surface, rebuilt per frame) and
## spheres come from a small recycled pool of MeshInstance3D — allocating a node per sphere
## per frame would churn the scene tree for no reason.
extends Node3D

const MAX_SPHERES := 64

var _lines: ImmediateMesh
var _line_mat: StandardMaterial3D
var _spheres: Array[MeshInstance3D] = []
var _sphere_mats: Array[StandardMaterial3D] = []
var _used := 0
var _open := false


func _ready() -> void:
	_lines = ImmediateMesh.new()
	_line_mat = StandardMaterial3D.new()
	_line_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_line_mat.vertex_color_use_as_albedo = true
	_line_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	var mi := MeshInstance3D.new()
	mi.mesh = _lines
	mi.material_override = _line_mat
	add_child(mi)

	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 12
	sphere.rings = 6
	for i in MAX_SPHERES:
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		var s := MeshInstance3D.new()
		s.mesh = sphere
		s.material_override = mat
		s.visible = false
		add_child(s)
		_spheres.append(s)
		_sphere_mats.append(mat)


## Begin a frame. Every line and sphere from the previous frame disappears unless redrawn,
## which is what makes the scene scripts able to describe their whole visual each frame.
func begin() -> void:
	_lines.clear_surfaces()
	_lines.surface_begin(Mesh.PRIMITIVE_LINES, _line_mat)
	_open = true
	_used = 0


func end() -> void:
	if _open:
		_lines.surface_end()
		_open = false
	for i in range(_used, _spheres.size()):
		_spheres[i].visible = false


func line(a: Vector3, b: Vector3, color: Color) -> void:
	if not _open:
		return
	_lines.surface_set_color(color)
	_lines.surface_add_vertex(a)
	_lines.surface_set_color(color)
	_lines.surface_add_vertex(b)


func sphere(p: Vector3, radius: float, color: Color) -> void:
	if _used >= _spheres.size():
		return   # silently capped rather than growing the pool mid-frame
	var s := _spheres[_used]
	s.position = p
	s.scale = Vector3.ONE * radius
	s.visible = true
	_sphere_mats[_used].albedo_color = color
	_used += 1


## A closed polyline through `points` — the lobe curves and orbit trails.
func polyline(points: PackedVector3Array, color: Color, closed: bool = false) -> void:
	for i in range(1, points.size()):
		line(points[i - 1], points[i], color)
	if closed and points.size() > 2:
		line(points[points.size() - 1], points[0], color)


## A wire quad, given centre and two half-extent axes: the occluder panels.
func quad(c: Vector3, u: Vector3, v: Vector3, color: Color) -> void:
	var p := PackedVector3Array([c - u - v, c + u - v, c + u + v, c - u + v])
	polyline(p, color, true)
	# Two diagonals so the panel reads as a surface rather than an empty frame.
	line(p[0], p[2], Color(color.r, color.g, color.b, color.a * 0.35))
	line(p[1], p[3], Color(color.r, color.g, color.b, color.a * 0.35))


## A ground grid on y = 0, with the room's own axes called out.
##
## The axis spurs are not decoration. Room space puts the listener's RIGHT at -X, which is
## the opposite of most people's reflex, and a scene with left and right swapped looks
## completely plausible. Labelling the floor makes the convention visible instead of
## something you have to already know.
func floor_grid(centre: Vector3, extent: float, step: float) -> void:
	var faint := Color(1, 1, 1, 0.07)
	var mid := Color(1, 1, 1, 0.14)
	var cx: float = roundf(centre.x / step) * step
	var cz: float = roundf(centre.z / step) * step
	var n := int(extent / step)
	for i in range(-n, n + 1):
		var col := mid if i % 5 == 0 else faint
		var x: float = cx + i * step
		var z: float = cz + i * step
		line(Vector3(x, 0, cz - extent), Vector3(x, 0, cz + extent), col)
		line(Vector3(cx - extent, 0, z), Vector3(cx + extent, 0, z), col)

	# +Z ahead (green), the listener's right (red) — same red-is-right as the head's ears.
	var ahead: Vector3 = BwaEngine.room_ahead()
	var right: Vector3 = BwaEngine.room_right()
	var o := Vector3(cx, 0.01, cz)
	line(o, o + ahead * (extent * 0.5), Color(0.35, 0.86, 0.35, 0.55))
	line(o, o + right * (extent * 0.5), Color(0.9, 0.3, 0.3, 0.55))
	# A tick at each arrow head so the direction reads without a label.
	var a_end := o + ahead * (extent * 0.5)
	var r_end := o + right * (extent * 0.5)
	line(a_end, a_end - ahead * 0.4 + right * 0.25, Color(0.35, 0.86, 0.35, 0.55))
	line(a_end, a_end - ahead * 0.4 - right * 0.25, Color(0.35, 0.86, 0.35, 0.55))
	line(r_end, r_end - right * 0.4 + ahead * 0.25, Color(0.9, 0.3, 0.3, 0.55))
	line(r_end, r_end - right * 0.4 - ahead * 0.25, Color(0.9, 0.3, 0.3, 0.55))


## An axis-aligned wire box, floor-based like the engine's own shoebox (y from 0 to size.y).
func room_box(centre_xz: Vector3, size: Vector3, color: Color) -> void:
	var hw := size.x * 0.5
	var hd := size.z * 0.5
	var lo := Vector3(centre_xz.x - hw, 0.0, centre_xz.z - hd)
	var hi := Vector3(centre_xz.x + hw, size.y, centre_xz.z + hd)
	var c := [
		Vector3(lo.x, lo.y, lo.z), Vector3(hi.x, lo.y, lo.z),
		Vector3(hi.x, lo.y, hi.z), Vector3(lo.x, lo.y, hi.z),
		Vector3(lo.x, hi.y, lo.z), Vector3(hi.x, hi.y, lo.z),
		Vector3(hi.x, hi.y, hi.z), Vector3(lo.x, hi.y, hi.z),
	]
	for i in 4:
		line(c[i], c[(i + 1) % 4], color)
		line(c[4 + i], c[4 + (i + 1) % 4], color)
		line(c[i], c[4 + i], color)
