// EmitterEditor.cs — custom inspector for a source. Hides the settings that only matter once their
// feature is switched on, and shows what the engine is doing to this source while it runs (occlusion is
// ray-traced off-thread, so the number is the only way to see whether the wall you built is working).
// Registered on SourceBase with editorForChildClasses, so Emitter and PushEmitter get the same
// conditional-hide and live readouts.
using UnityEditor;
using UnityEngine;

namespace BwAudio.EditorTools
{
    [CustomEditor(typeof(SourceBase), true)]
    [CanEditMultipleObjects]
    public sealed class EmitterEditor : Editor
    {
        public override bool RequiresConstantRepaint() => Application.isPlaying;

        public override void OnInspectorGUI()
        {
            var e = (SourceBase)target;
            serializedObject.Update();

            var it = serializedObject.GetIterator();
            for (bool enter = true; it.NextVisible(enter); enter = false)
            {
                if (it.propertyPath == "m_Script")
                {
                    using (new EditorGUI.DisabledScope(true)) EditorGUILayout.PropertyField(it);
                    continue;
                }
                if (IsHidden(it.propertyPath, e)) continue;
                EditorGUILayout.PropertyField(it, true);
            }

            serializedObject.ApplyModifiedProperties();   // fires Emitter.OnValidate -> live re-push

            if (e.spread > 0f && e.sizeMeters > 0f)
                EditorGUILayout.HelpBox(
                    "Spread and Size both set. They don't add up — the engine takes the WIDER of the two, " +
                    "and Size (a physical radius) usually wins as the listener gets close. Pick one.",
                    MessageType.Info);

            if (Application.isPlaying && e.isActiveAndEnabled)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Live", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("playing", e.IsPlaying ? "yes" : "no");
                var r = EditorGUILayout.GetControlRect();
                // 1 = clear, 0 = fully blocked. Shown as "openness" so a full bar means an unobstructed path.
                EditorGUI.ProgressBar(r, e.Occlusion, $"unoccluded  {e.Occlusion:0.00}");
            }
        }

        static bool IsHidden(string p, SourceBase e)
        {
            switch (p)
            {
                case "directivityPower":    return e.directivity == BwaDirectivity.Omni;
                case "reflectionSend":
                case "reflectionDistance":  return !e.reflections;
                default:                    return false;
            }
        }
    }
}
