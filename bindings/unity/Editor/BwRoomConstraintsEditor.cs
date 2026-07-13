// BwRoomConstraintsEditor.cs — show what actually got loaded. A constraints file that is missing, or
// that parsed to nothing, otherwise just draws no gizmos, which looks identical to "the component is
// working and the room is empty".
using UnityEditor;
using UnityEngine;

namespace CaveAudio.EditorTools
{
    [CustomEditor(typeof(BwRoomConstraints))]
    public sealed class BwRoomConstraintsEditor : Editor
    {
        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();

            var c = (BwRoomConstraints)target;
            if (GUILayout.Button("Reload")) { c.Load(force: true); SceneView.RepaintAll(); }

            var data = c.Constraints;
            if (!string.IsNullOrEmpty(c.LoadError))
            {
                EditorGUILayout.HelpBox(
                    c.LoadError + "\n\nThis is the surveyed room (truss / screen cube / projectors), the " +
                    "same constraints.json bw_layout_tool and bw_playground read. Copy it into " +
                    "Assets/StreamingAssets/.", MessageType.Warning);
                return;
            }

            int nogo = data?.nogo?.Length ?? 0, obst = data?.obstacles?.Length ?? 0;
            EditorGUILayout.HelpBox(
                $"loaded: bounds {(data?.bounds != null ? "yes" : "no")}, {nogo} keep-out, {obst} obstacle(s)\n" +
                "Scene view: green = speaker truss (stay inside), red = keep-out (screens + observer), " +
                "orange = obstacles (projectors). Drawn in room metres through Room.UnityToRoom — if the " +
                "CAVE lands somewhere unexpected, that registration transform is wrong.",
                MessageType.Info);
        }
    }
}
