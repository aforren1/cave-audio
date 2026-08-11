// DynamicAcousticGeometry.cs — the MOVABLE counterpart to AcousticGeometry. Registers a low-poly
// acoustic mesh as a rigid INSTANCE in the engine's scene (Steam Audio IPLInstancedMesh) and pushes
// this transform's position + rotation every frame (throttled), so occlusion and REAL-TIME reflections
// track the object as it moves — a door swinging, a lift, a rotating panel. Moving it is a cheap
// scene-BVH refit, not a geometry rebuild.
//
// Same rules as AcousticGeometry: keep the acoustic mesh SIMPLE (tens–hundreds of triangles); use
// meshOverride for a low-poly proxy of a heavy render mesh. Differences: position + rotation are LIVE
// (SCALE is captured once at registration — rigid-body; to rescale, disable/enable). BAKED
// reflections/pathing do NOT track movement (the bake froze the geometry); real-time reflections and
// occlusion do. Needs the Steam Audio backend (a no-op otherwise).
using UnityEngine;
using System.Collections;

namespace BwAudio
{
    [DisallowMultipleComponent]
    public sealed class DynamicAcousticGeometry : MonoBehaviour
    {
        [Tooltip("Material for every triangle of this object. None = the engine's default material.")]
        public MaterialAsset material;

        [Tooltip("Low-poly acoustic mesh. If empty, the sibling MeshFilter's sharedMesh is used.")]
        public Mesh meshOverride;

        [Tooltip("Only push a new pose when it moved more than this many meters (0 = every frame).")]
        public float positionEpsilon = 0.002f;
        [Tooltip("Only push a new pose when it rotated more than this many degrees (0 = every frame).")]
        public float angleEpsilon = 0.25f;

        [Tooltip("Draw the acoustic mesh in the scene view even when not selected.")]
        public bool alwaysDrawGizmo = true;

        int _handle = -1;
        Engine _owner;                         // the Engine this mesh was registered under
        Vector3 _lastPos;
        Quaternion _lastRot;

        // Valid only while the REGISTERING Engine is still the live instance (SourceBase's guard,
        // same reason but sharper: the dynamic-mesh handle is a plain index with NO generation, so
        // on a destroyed+recreated Engine a stale handle silently addresses a FOREIGN mesh —
        // RemoveDynamicMesh(0) would remove whatever registered first under the successor). Owner
        // gone -> null, every op no-ops, and a disable/enable re-registers under the new engine.
        Engine Owner => _owner != null && ReferenceEquals(Engine.Instance, _owner) ? _owner : null;

        /// <summary>The mesh that will be contributed (override, else the MeshFilter), or null.</summary>
        public Mesh ResolveMesh()
        {
            if (meshOverride != null) return meshOverride;
            var mf = GetComponent<MeshFilter>();
            return mf != null ? mf.sharedMesh : null;
        }

        void OnEnable()
        {
            if (!TryRegister()) StartCoroutine(RegisterWhenReady());   // Engine may still be starting
        }

        IEnumerator RegisterWhenReady()
        {
            while (_handle < 0 && isActiveAndEnabled) { if (TryRegister()) yield break; yield return null; }
        }

        bool TryRegister()
        {
            var eng = Engine.Instance;
            if (eng == null || !eng.Ready) return false;
            var mesh = ResolveMesh();
            if (mesh == null) { Debug.LogWarning("[bw_audio] DynamicAcousticGeometry with no mesh: " + name); return true; }  // stop retrying
            _handle = eng.AddDynamicMesh(mesh, transform, material);
            if (_handle >= 0) { _owner = eng; _lastPos = transform.position; _lastRot = transform.rotation; }
            return true;
        }

        void Update()
        {
            if (_handle < 0) return;
            var eng = Owner;                     // stale owner -> no-op: never push into a successor
            if (eng == null) return;
            if (Vector3.Distance(transform.position, _lastPos) <= positionEpsilon &&
                Quaternion.Angle(transform.rotation, _lastRot) <= angleEpsilon) return;
            eng.SetDynamicTransform(_handle, transform);
            _lastPos = transform.position; _lastRot = transform.rotation;
        }

        void OnDisable()
        {
            var eng = Owner;                     // owner gone -> the mesh died with its engine:
            if (_handle >= 0 && eng != null)     // never remove a successor's mesh by index
                eng.RemoveDynamicMesh(_handle);
            _handle = -1;
            _owner = null;
        }

        void OnDrawGizmos()         { if (alwaysDrawGizmo) DrawGizmo(new Color(1f, 0.6f, 0.2f, 0.25f)); }
        void OnDrawGizmosSelected() { DrawGizmo(new Color(1f, 0.6f, 0.2f, 0.9f)); }   // orange: movable (cyan = static)

        void DrawGizmo(Color c)
        {
            var mesh = ResolveMesh();
            if (mesh == null) return;
            Gizmos.color = c;
            Gizmos.DrawWireMesh(mesh, transform.position, transform.rotation, transform.lossyScale);
        }
    }
}
