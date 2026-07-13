// BwAudioEditor.cs — custom inspector for the manager.
//
// BwAudio carries the whole engine's configuration, which is a lot of fields, and most of them are
// irrelevant most of the time (the FDN decay controls mean nothing with the FDN off). So: hide what
// doesn't apply, and surface the things the component can only otherwise tell you by failing at run
// time — a layout file that isn't where the engine will look for it, both reverb beds fighting over
// one tap, a tracked-listener setup with nothing to track.
//
// Fields are drawn by ITERATING the serialized object rather than listing them by name, so a new field
// on BwAudio shows up here automatically; only the ones with a rule get hidden.
using System.IO;
using UnityEditor;
using UnityEngine;

namespace CaveAudio.EditorTools
{
    [CustomEditor(typeof(BwAudio))]
    public sealed class BwAudioEditor : Editor
    {
        // Play-mode readouts (backend, meters, voice count) are live values, not serialized state.
        public override bool RequiresConstantRepaint() => Application.isPlaying;

        public override void OnInspectorGUI()
        {
            var a = (BwAudio)target;
            serializedObject.Update();

            var it = serializedObject.GetIterator();
            for (bool enter = true; it.NextVisible(enter); enter = false)
            {
                if (it.propertyPath == "m_Script")
                {
                    using (new EditorGUI.DisabledScope(true)) EditorGUILayout.PropertyField(it);
                    continue;
                }
                if (IsHidden(it.propertyPath, a)) continue;
                EditorGUILayout.PropertyField(it, true);
            }

            serializedObject.ApplyModifiedProperties();   // fires BwAudio.OnValidate -> live re-push
            DrawProblems(a);
            DrawLiveStatus(a);
        }

        // The engine ignores these in the given state, so showing them just invites you to tune a knob
        // that does nothing.
        static bool IsHidden(string p, BwAudio a)
        {
            switch (p)
            {
                case "listener":          return !a.feedListener;      // the engine tracks itself
                case "posePredictionMs":  return a.feedListener;       // prediction is tracking-only
                case "limiterCeilingDb":  return !a.limiter;
                case "reverbSeconds":
                case "reflectionOrder":
                case "reverbGain":        return !a.enableReflections;
                case "fdnRt60LowSeconds":
                case "fdnRt60HighSeconds":
                case "fdnCrossoverHz":
                case "fdnDecayDirection":
                case "fdnDecayFactor":    return !a.enableFdnReverb;
                case "roomSizeMetres":
                case "roomMaterial":      return !a.enableRoomBox;
                case "speakerGizmoRadius":
                case "showSpeakerIndices": return !a.showSpeakers;
                default:                  return false;
            }
        }

        // Everything here is a mistake the engine survives — it logs and carries on — which is exactly
        // why it is worth catching in the inspector, where you can still see it.
        static void DrawProblems(BwAudio a)
        {
            if (!string.IsNullOrEmpty(a.layoutFile))
            {
                string full = Path.Combine(Application.streamingAssetsPath, a.layoutFile).Replace('\\', '/');
                if (!File.Exists(full))
                    EditorGUILayout.HelpBox(
                        "Speaker layout not found. The engine will look for it here:\n\n" + full +
                        "\n\nCreate the Assets/StreamingAssets folder and put the layout there (the field " +
                        "is a path relative to it). Without it the engine does NOT stop — it falls back " +
                        "to its built-in 26-speaker grid, so a smaller rig would be panned over geometry " +
                        "that isn't the one in your room.", MessageType.Warning);
            }

            if (a.enableReflections && a.enableFdnReverb)
                EditorGUILayout.HelpBox(
                    "Both reverb beds are enabled, and they share ONE reverb tap. The FDN takes it and " +
                    "the Steam Audio bed is ignored. Tick only one.", MessageType.Warning);

            if (a.feedListener && a.listener == null)
                EditorGUILayout.HelpBox(
                    "Feed Listener is on but no listener Transform is assigned — the listener will never " +
                    "move from the array centroid, and every source will be panned for a head that never " +
                    "moves. Assign the tracked head, or turn Feed Listener off to let the engine read " +
                    "NatNet itself.", MessageType.Warning);

            if (a.profile != BwProfile.Cave && a.enableRoomBox == false && a.enableReflections)
                EditorGUILayout.HelpBox(
                    "Reflections are on but there is no acoustic geometry: add a Room Box, or one or more " +
                    "BwAcousticGeometry objects. With nothing to reflect off, the bed stays silent.",
                    MessageType.Info);
        }

        // The questions you actually ask while the scene is running: did I get a real audio device, or
        // did it silently fall back to the null sink? How many voices am I burning? Is anything coming
        // out of the speakers at all?
        static void DrawLiveStatus(BwAudio a)
        {
            if (!Application.isPlaying || !a.Ready) return;

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Live", EditorStyles.boldLabel);

            string backend = Bw.Backend(a.Handle) ?? "?";
            var kind = backend.StartsWith("asio:") ? MessageType.Info : MessageType.Warning;
            EditorGUILayout.HelpBox(
                $"backend: {backend}{(backend == "null" ? "  (SILENT — no audio device)" : "")}\n" +
                $"channels: {a.ChannelCount}\n" +
                $"active voices: {a.ActiveVoices}", kind);

            var peaks = a.BusLevels();
            for (int i = 0; i < peaks.Length; i++)
            {
                var r = EditorGUILayout.GetControlRect(false, 10f);
                EditorGUI.ProgressBar(r, Mathf.Clamp01(peaks[i]), $"{i}");
            }
        }
    }
}
