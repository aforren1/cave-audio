// RoomView.cs — a LIVE, in-build view of the surveyed room: the speaker truss, the CAVE screen cube,
// and the projectors hanging in the way.
//
// This is the runtime counterpart to RoomConstraints' scene-view gizmos, exactly as SpeakerView is the
// runtime counterpart to Engine's speaker gizmos. Gizmos are an editor feature and do not render in a
// build, so standing inside the CAVE and seeing where the truss is takes actual renderers.
//
// It does not re-read or re-author anything: it asks the RoomConstraints on the same GameObject for its
// already-parsed boxes, and it honors that component's show* toggles. One file, one parse, one set of
// switches. RoomConstraints needs no engine (it reads the JSON directly), so this view still draws when
// the engine failed to start, which is the moment you most want to see the room.
//
// WIREFRAME for all three kinds, including the obstacles the gizmo draws solid. A solid translucent box
// needs blend state and culling configured per render pipeline (URP, HDRP and built-in disagree), while
// line topology needs neither and looks the same everywhere. For "where is the truss, where are the
// projectors" in a dark room, edges are what you actually read.
using UnityEngine;

namespace BwAudio
{
    [DisallowMultipleComponent]
    [RequireComponent(typeof(RoomConstraints))]
    public sealed class RoomView : MonoBehaviour
    {
        [Header("Color")]
        [Tooltip("Matches the gizmo and the raylib tools: green truss, red keep-out, orange obstacles.")]
        public Color bounds    = new Color(90 / 255f, 200 / 255f, 120 / 255f);
        public Color nogo      = new Color(235 / 255f, 90 / 255f, 90 / 255f);
        public Color obstacles = new Color(245 / 255f, 165 / 255f, 70 / 255f);

        RoomConstraints _rc;
        Material _material;
        MaterialPropertyBlock _mpb;
        int _colorId = -1;
        GameObject _root;
        bool _warned;

        // OnEnable, not Start: Start runs once per component lifetime, so toggling the view off and back
        // on would tear the boxes down and never rebuild them (the same reason SpeakerView does this).
        void OnEnable() => Rebuild();
        void OnDisable() => Teardown();

        /// <summary>
        /// Rebuild the boxes. Call this after changing `Room.UnityToRoom` or the constraints file: the
        /// vertices bake the room-to-Unity transform at build time, so a later registration change does
        /// not move them by itself.
        /// </summary>
        public void Rebuild()
        {
            Teardown();
            if (_rc == null) _rc = GetComponent<RoomConstraints>();
            var data = _rc.Constraints;                       // parses on first access
            if (data == null)
            {
                if (!_warned)
                {
                    _warned = true;
                    Debug.LogWarning("[RoomView] no constraints drawn: " + (_rc.LoadError ?? "no data"), this);
                }
                return;
            }

            _material = UnlitMaterial.Make(out _colorId);
            if (_material == null)
            {
                if (!_warned)
                {
                    _warned = true;
                    Debug.LogWarning("[RoomView] no unlit shader found for this render pipeline - no boxes drawn.", this);
                }
                return;
            }
            _mpb = new MaterialPropertyBlock();

            _root = new GameObject("room boxes") { hideFlags = HideFlags.DontSave };
            _root.transform.SetParent(transform, worldPositionStays: false);

            if (_rc.showBounds && data.bounds != null) Box(data.bounds, bounds);
            if (_rc.showNogo && data.nogo != null)
                foreach (var b in data.nogo) Box(b, nogo);
            if (_rc.showObstacles && data.obstacles != null)
                foreach (var b in data.obstacles) Box(b, obstacles);
        }

        // The 12 edges of a box, as index pairs into the 8 corners built below.
        static readonly int[] EDGES =
        {
            0, 1,  1, 2,  2, 3,  3, 0,      // bottom face
            4, 5,  5, 6,  6, 7,  7, 4,      // top face
            0, 4,  1, 5,  2, 6,  3, 7,      // the verticals
        };

        void Box(RoomConstraints.Box b, Color color)
        {
            if (!Corners(b, out var c)) return;              // a malformed box is skipped, not thrown on

            var mesh = new Mesh { name = b?.label ?? "box", hideFlags = HideFlags.DontSave };
            mesh.SetVertices(c);
            mesh.SetIndices(EDGES, MeshTopology.Lines, 0);
            mesh.RecalculateBounds();                        // without this the mesh can be culled at once

            var go = new GameObject(string.IsNullOrEmpty(b.label) ? "box" : b.label) { hideFlags = HideFlags.DontSave };
            go.transform.SetParent(_root.transform, worldPositionStays: false);
            go.AddComponent<MeshFilter>().sharedMesh = mesh;
            var r = go.AddComponent<MeshRenderer>();
            r.sharedMaterial = _material;                    // shared + a property block: no per-object material leak
            _mpb.SetColor(_colorId, color);
            r.SetPropertyBlock(_mpb);
        }

        // The corners are transformed to Unity space ON THE CPU, one point at a time, rather than by
        // putting Room.RoomToUnityMatrix() on a Transform. That matrix carries the handedness mirror, so
        // decomposing it into a Transform means a negative scale and the winding questions that follow.
        // Line topology has no winding, so mapping the points and leaving the object at identity is both
        // simpler and correct for any registration matrix.
        static bool Corners(RoomConstraints.Box b, out Vector3[] corners)
        {
            corners = null;
            if (b == null || b.min == null || b.max == null || b.min.Length < 3 || b.max.Length < 3) return false;
            float x0 = b.min[0], y0 = b.min[1], z0 = b.min[2];
            float x1 = b.max[0], y1 = b.max[1], z1 = b.max[2];
            corners = new[]
            {
                Room.FromRoom(new Vector3(x0, y0, z0)), Room.FromRoom(new Vector3(x1, y0, z0)),
                Room.FromRoom(new Vector3(x1, y0, z1)), Room.FromRoom(new Vector3(x0, y0, z1)),
                Room.FromRoom(new Vector3(x0, y1, z0)), Room.FromRoom(new Vector3(x1, y1, z0)),
                Room.FromRoom(new Vector3(x1, y1, z1)), Room.FromRoom(new Vector3(x0, y1, z1)),
            };
            return true;
        }

        void Teardown()
        {
            if (_root != null)
            {
                foreach (var mf in _root.GetComponentsInChildren<MeshFilter>())
                    if (mf.sharedMesh) Destroy(mf.sharedMesh);   // the meshes are built here, so they are ours to free
                Destroy(_root);
            }
            if (_material) Destroy(_material);
            _root = null; _material = null; _mpb = null;
        }
    }
}
