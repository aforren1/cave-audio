// AcousticGeometry.cs — marks a GameObject's mesh as occluding/reflecting geometry for the engine's
// acoustic scene, with a material. Engine collects every enabled one and bakes them (world -> room
// space) into a single mesh via bwa_scene_set_mesh_mat: at start, and again in the LateUpdate after
// any of them is enabled or disabled (spawned, toggled, or its scene loaded or unloaded), once per
// frame however many change.
//
// IMPORTANT: keep acoustic meshes SIMPLE (tens–hundreds of triangles). The engine ray-traces them at
// runtime; render meshes (thousands of tris) are far too heavy. Use a low-poly proxy via `meshOverride`
// (or a simple MeshFilter on a dedicated, renderer-less object). Geometry is STATIC: moving one does
// nothing until the next rebuild. Use DynamicAcousticGeometry for anything that moves.
using UnityEngine;

namespace BwAudio
{
    [DisallowMultipleComponent]
    public sealed class AcousticGeometry : MonoBehaviour
    {
        [Tooltip("Material for every triangle of this object. None = the engine's default material.")]
        public MaterialAsset material;

        [Tooltip("Low-poly acoustic mesh. If empty, the sibling MeshFilter's sharedMesh is used.")]
        public Mesh meshOverride;

        [Tooltip("Draw the acoustic mesh in the scene view even when not selected.")]
        public bool alwaysDrawGizmo = true;

        /// <summary>The mesh that will be contributed (override, else the MeshFilter), or null.</summary>
        public Mesh ResolveMesh()
        {
            if (meshOverride != null) return meshOverride;
            var mf = GetComponent<MeshFilter>();
            return mf != null ? mf.sharedMesh : null;
        }

        // Tell the Engine; it compares against what it last baked and rebuilds once, in LateUpdate.
        void OnEnable()  { var e = Engine.Instance; if (e) e.StaticGeometryChanged(this, true); }
        void OnDisable() { var e = Engine.Instance; if (e) e.StaticGeometryChanged(this, false); }

        void OnDrawGizmos()        { if (alwaysDrawGizmo) DrawGizmo(new Color(0.2f, 0.8f, 1f, 0.25f)); }
        void OnDrawGizmosSelected() { DrawGizmo(new Color(0.2f, 0.8f, 1f, 0.9f)); }

        void DrawGizmo(Color c)
        {
            var mesh = ResolveMesh();
            if (mesh == null) return;
            Gizmos.color = c;
            Gizmos.DrawWireMesh(mesh, transform.position, transform.rotation, transform.lossyScale);
        }
    }
}
