// RoomConstraints.cs — draw the PHYSICAL room in the scene view: the speaker truss you must stay
// inside, the CAVE screen cube you must stay out of, and the projectors hanging in the way.
//
// It reads the SAME constraints.json the C++ tools do (bwa_layout_tool, bwa_playground — see
// examples/constraints_view.h), so the room has one source of truth. Do not re-author these boxes as
// Unity geometry: the file comes from the survey, and a second copy of it is a second thing to be wrong.
//
// Purely a scene-view aid — it drives no audio, and needs no engine (it parses the JSON directly, so it
// works with the editor stopped). Colours match the raylib tools: green truss, red keep-out, orange
// obstacles. Room space is floor-based (y = 0 is the floor, x/z centred), so the boxes are drawn through
// the inverse of the coordinate seam — which means a wrong Room.UnityToRoom puts the whole CAVE visibly
// in the wrong place, exactly where you want to find out about it.
using System;
using System.IO;
using UnityEngine;

namespace BwAudio
{
    [DisallowMultipleComponent]
    public sealed class RoomConstraints : MonoBehaviour
    {
        [Header("Constraints file (under StreamingAssets)")]
        [Tooltip("The surveyed room: 'bounds' = the speaker truss (speakers must be INSIDE), 'nogo' = " +
                 "keep-out volumes (the screen cube + observer), 'obstacles' = solid objects (projectors). " +
                 "Room metres, floor-based origin. The same file bwa_layout_tool and bwa_playground read.")]
        [Clip(".json")] public string constraintsFile = "constraints.json";

        [Header("Draw")]
        public bool showBounds    = true;    // green wire  — the truss
        public bool showNogo      = true;    // red wire    — screens + observer
        public bool showObstacles = true;    // orange solid — projectors
        public bool showLabels    = true;
        [Tooltip("Draw even when this object isn't selected.")]
        public bool alwaysDraw    = true;

        // ---- the file (JsonUtility maps these names 1:1; "comment" is ignored) ----
        [Serializable] public class Box { public float[] min; public float[] max; public string label; }
        [Serializable] public class Data { public Box bounds; public Box[] nogo; public Box[] obstacles; }

        Data _data;
        string _loadedFrom;
        public string LoadError { get; private set; }
        public Data Constraints { get { Load(); return _data; } }

        void OnValidate() { _loadedFrom = null; }   // re-read when the path changes

        /// <summary>Parse the file (cached until the path changes). Safe to call from the editor.</summary>
        public void Load(bool force = false)
        {
            if (!force && _loadedFrom == constraintsFile) return;
            _loadedFrom = constraintsFile;
            _data = null; LoadError = null;

            if (string.IsNullOrEmpty(constraintsFile)) { LoadError = "no file set"; return; }
            string path = Path.Combine(Application.streamingAssetsPath, constraintsFile);
            if (!File.Exists(path)) { LoadError = "not found: " + path.Replace('\\', '/'); return; }
            try
            {
                _data = JsonUtility.FromJson<Data>(File.ReadAllText(path));
                if (_data == null) LoadError = "could not parse " + constraintsFile;
            }
            catch (Exception e) { LoadError = e.Message; }
        }

        void OnDrawGizmos()         { if (alwaysDraw) Draw(1f); }
        void OnDrawGizmosSelected() { if (!alwaysDraw) Draw(1f); }

        void Draw(float alpha)
        {
            Load();
            if (_data == null) return;

            var prev = Gizmos.matrix;
            Gizmos.matrix = Room.RoomToUnityMatrix();   // the boxes are in ROOM metres

            if (showBounds && _data.bounds != null)
                Wire(_data.bounds, new Color(90 / 255f, 200 / 255f, 120 / 255f, alpha));

            if (showNogo && _data.nogo != null)
                foreach (var b in _data.nogo) Wire(b, new Color(235 / 255f, 90 / 255f, 90 / 255f, alpha));

            if (showObstacles && _data.obstacles != null)
                foreach (var b in _data.obstacles)
                {
                    if (!Center(b, out var c, out var s)) continue;
                    Gizmos.color = new Color(235 / 255f, 150 / 255f, 60 / 255f, 0.28f * alpha);
                    Gizmos.DrawCube(c, s);              // solid: a projector is a thing, not a boundary
                    Gizmos.color = new Color(245 / 255f, 165 / 255f, 70 / 255f, alpha);
                    Gizmos.DrawWireCube(c, s);
                }

            Gizmos.matrix = prev;
#if UNITY_EDITOR
            if (showLabels) Labels();
#endif
        }

        void Wire(Box b, Color c)
        {
            if (!Center(b, out var ctr, out var size)) return;
            Gizmos.color = c;
            Gizmos.DrawWireCube(ctr, size);
        }

        // min/max are float[3] straight from the JSON — a malformed box is skipped, not thrown on.
        static bool Center(Box b, out Vector3 center, out Vector3 size)
        {
            center = default; size = default;
            if (b == null || b.min == null || b.max == null || b.min.Length < 3 || b.max.Length < 3) return false;
            var lo = new Vector3(b.min[0], b.min[1], b.min[2]);
            var hi = new Vector3(b.max[0], b.max[1], b.max[2]);
            center = (lo + hi) * 0.5f;
            size   = hi - lo;
            return true;
        }

#if UNITY_EDITOR
        // Labels go through Handles (editor-only), positioned in UNITY world — so unlike the boxes they
        // are placed with Room.FromRoom rather than by setting Gizmos.matrix.
        void Labels()
        {
            void Label(Box b)
            {
                if (b == null || string.IsNullOrEmpty(b.label) || !Center(b, out var c, out var s)) return;
                var top = new Vector3(c.x, c.y + s.y * 0.5f, c.z);
                UnityEditor.Handles.Label(Room.FromRoom(top), b.label);
            }
            if (showBounds) Label(_data.bounds);
            if (showNogo && _data.nogo != null)           foreach (var b in _data.nogo) Label(b);
            if (showObstacles && _data.obstacles != null) foreach (var b in _data.obstacles) Label(b);
        }
#endif
    }
}
