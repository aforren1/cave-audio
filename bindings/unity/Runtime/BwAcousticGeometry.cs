// BwAcousticGeometry.cs — marks a GameObject's mesh as occluding/reflecting geometry for the engine's
// acoustic scene, with a material. BwAudio collects every one of these at load time and bakes them
// (world -> room space) into a single mesh via bw_scene_set_mesh_mat.
//
// IMPORTANT: keep acoustic meshes SIMPLE (tens–hundreds of triangles). The engine ray-traces them at
// runtime; render meshes (thousands of tris) are far too heavy. Use a low-poly proxy via `meshOverride`
// (or a simple MeshFilter on a dedicated, renderer-less object). Geometry is STATIC (set before start).
using UnityEngine;

namespace CaveAudio
{
    [DisallowMultipleComponent]
    public sealed class BwAcousticGeometry : MonoBehaviour
    {
        [Tooltip("Material for every triangle of this object. None = the engine's default material.")]
        public BwMaterialAsset material;

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
