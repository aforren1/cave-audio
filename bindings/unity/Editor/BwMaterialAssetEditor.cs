// BwMaterialAssetEditor.cs — show either the preset dropdown or the custom coefficients, never both.
// A material is one or the other, and leaving the unused half on screen makes it look like the numbers
// under a preset are what you'd get (they aren't — the engine's own table is).
using UnityEditor;
using UnityEngine;

namespace CaveAudio.EditorTools
{
    [CustomEditor(typeof(BwMaterialAsset))]
    public sealed class BwMaterialAssetEditor : Editor
    {
        public override void OnInspectorGUI()
        {
            var m = (BwMaterialAsset)target;
            serializedObject.Update();

            var it = serializedObject.GetIterator();
            for (bool enter = true; it.NextVisible(enter); enter = false)
            {
                if (it.propertyPath == "m_Script")
                {
                    using (new EditorGUI.DisabledScope(true)) EditorGUILayout.PropertyField(it);
                    continue;
                }
                bool custom = m.source == BwMaterialAsset.Source.Custom;
                switch (it.propertyPath)
                {
                    case "preset":       if (custom)  continue; break;
                    case "absorption":
                    case "scattering":
                    case "transmission": if (!custom) continue; break;
                }
                EditorGUILayout.PropertyField(it, true);
            }

            serializedObject.ApplyModifiedProperties();

            EditorGUILayout.HelpBox(
                m.source == BwMaterialAsset.Source.Preset
                    ? "Uses the engine's built-in coefficients for this material."
                    : "Bands are low / mid / high. Absorption = what a reflection loses. Transmission = " +
                      "what passes THROUGH (the spectral tilt of occluded sound — this is what makes a " +
                      "wall muffle rather than just quieten).",
                MessageType.Info);
        }
    }
}
