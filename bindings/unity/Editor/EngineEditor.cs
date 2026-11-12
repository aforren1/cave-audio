// EngineEditor.cs — custom inspector for the manager.
//
// Engine carries the whole engine's configuration, which is a lot of fields, and most of them are
// irrelevant most of the time (the FDN decay controls mean nothing with the FDN off). So: hide what
// doesn't apply, and surface the things the component can only otherwise tell you by failing at run
// time — a layout file that isn't where the engine will look for it, both reverb beds fighting over
// one tap, a tracked-listener setup with nothing to track.
//
// Fields are drawn by ITERATING the serialized object rather than listing them by name, so a new field
// on Engine shows up here automatically; only the ones with a rule get hidden.
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace BwAudio.EditorTools
{
    [CustomEditor(typeof(Engine))]
    public sealed class EngineEditor : Editor
    {
        // Play-mode readouts (backend, meters, voice count) are live values, not serialized state.
        public override bool RequiresConstantRepaint() => Application.isPlaying;

        public override void OnInspectorGUI()
        {
            var a = (Engine)target;
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
                if (it.propertyPath == "sink") { DrawSinkField(it); continue; }
                EditorGUILayout.PropertyField(it, true);
            }

            serializedObject.ApplyModifiedProperties();   // fires Engine.OnValidate -> live re-push
            DrawProblems(a);
            DrawLiveStatus(a);
        }

        // The device backends each player platform's engine library carries. BwaSinkType mirrors the
        // ABI, so it names every backend on every platform, and naming one a target lacks is a DEMAND
        // that fails startup there. Left out everywhere: CoreAudio (not built yet), Worklet (the
        // browser build, which Unity never links) and Manual (it needs bwa_render_block pumped, and
        // Unity binds no pump).
        static BwaSinkType[] BackendsFor(BuildTarget t)
        {
            switch (t)
            {
                case BuildTarget.StandaloneWindows:
                case BuildTarget.StandaloneWindows64: return new[] { BwaSinkType.Asio, BwaSinkType.Wasapi };
                case BuildTarget.StandaloneLinux64:   return new[] { BwaSinkType.Jack, BwaSinkType.Alsa };
                case BuildTarget.Android:             return new[] { BwaSinkType.AAudio };
                default:                              return new BwaSinkType[0];
            }
        }

        // Play mode runs the EDITOR's library, not the build target's, so a Quest project played on a
        // Windows desk opens WASAPI. Those entries stay pickable, marked, for desk testing.
        static BwaSinkType[] EditorBackends()
        {
            switch (Application.platform)
            {
                case RuntimePlatform.WindowsEditor: return BackendsFor(BuildTarget.StandaloneWindows64);
                case RuntimePlatform.LinuxEditor:   return BackendsFor(BuildTarget.StandaloneLinux64);
                default:                            return new BwaSinkType[0];
            }
        }

        static bool SinkFitsTarget(BwaSinkType s) =>
            s == BwaSinkType.Auto || s == BwaSinkType.Null ||
            System.Array.IndexOf(BackendsFor(EditorUserBuildSettings.activeBuildTarget), s) >= 0;

        // A popup instead of the enum's own: only what the active build target (and the editor, for
        // Play mode) can open. A saved value outside that set stays selected and labeled rather than
        // being rewritten, because switching the build target must not edit the scene.
        static void DrawSinkField(SerializedProperty p)
        {
            var target = EditorUserBuildSettings.activeBuildTarget;
            var cur = (BwaSinkType)p.intValue;
            var opts = new List<BwaSinkType> { BwaSinkType.Auto };
            var labels = new List<GUIContent> { new GUIContent("Auto") };
            foreach (var b in BackendsFor(target)) { opts.Add(b); labels.Add(new GUIContent(b.ToString())); }
            foreach (var b in EditorBackends())
                if (!opts.Contains(b)) { opts.Add(b); labels.Add(new GUIContent(b + " (editor only)")); }
            opts.Add(BwaSinkType.Null); labels.Add(new GUIContent("Null (silent)"));
            if (!opts.Contains(cur)) { opts.Add(cur); labels.Add(new GUIContent(cur + " (not on " + target + ")")); }

            var rect = EditorGUILayout.GetControlRect();
            var label = EditorGUI.BeginProperty(rect, new GUIContent(p.displayName, p.tooltip), p);
            EditorGUI.BeginChangeCheck();
            int pick = EditorGUI.Popup(rect, label, opts.IndexOf(cur), labels.ToArray());
            if (EditorGUI.EndChangeCheck()) p.intValue = (int)opts[pick];
            EditorGUI.EndProperty();
        }

        // The engine ignores these in the given state, so showing them just invites you to tune a knob
        // that does nothing.
        static bool IsHidden(string p, Engine a)
        {
            switch (p)
            {
                case "maxReSplit":        return !a.maxRe;            // the band split only means anything with max-rE on
                case "listener":          return !a.feedListener;      // the engine tracks itself
                case "natnetServer":                                   // NatNet connection: tracking-only
                case "natnetRigidBody":
                case "posePredictionS":   return a.feedListener;       // prediction is tracking-only
                case "limiterCeiling":    return !a.limiter;
                case "reverbSeconds":
                case "reflectionOrder":
                case "reverbGain":        return !a.enableReflections;
                case "fdnRt60LowSeconds":
                case "fdnRt60HighSeconds":
                case "fdnCrossoverHz":
                case "fdnDecayDirection":
                case "fdnDecayFactor":    return !a.enableFdnReverb;
                case "roomSizeMeters":
                case "roomMaterial":      return !a.enableRoomBox;
                case "speakerGizmoRadius":
                case "showSpeakerIndices": return !a.showSpeakers;
                default:                  return false;
            }
        }

        // Mistakes worth catching in the inspector, where you can still see them. Most the engine
        // survives with a log; the missing layout is the exception — bwa_start refuses it outright
        // (BWA_ERR_LAYOUT), so that one draws as an Error.
        static void DrawProblems(Engine a)
        {
            if (!string.IsNullOrEmpty(a.layoutFile))
            {
                string full = Path.Combine(Application.streamingAssetsPath, a.layoutFile).Replace('\\', '/');
                if (!File.Exists(full) && a.profile == BwaProfile.Binaural)
                    EditorGUILayout.HelpBox(
                        "No speaker layout at:\n\n" + full + "\n\nBinaural drives no speakers, so the " +
                        "engine runs its built-in default grid for the diffuse bed. Cave, CaveSim and " +
                        "CaveBoth need the file, and refuse to start without it.", MessageType.Info);
                else if (!File.Exists(full))
                    EditorGUILayout.HelpBox(
                        "Speaker layout not found. The engine will look for it here:\n\n" + full +
                        "\n\nAn explicit layout MUST load: bwa_start refuses a failed load with " +
                        "BWA_ERR_LAYOUT, so this scene will start with NO AUDIO. Create the " +
                        "Assets/StreamingAssets folder and put the layout there (the field is a path " +
                        "relative to it) — or clear the field to run the engine's built-in default " +
                        "26-speaker grid deliberately.", MessageType.Error);
            }

            if (!SinkFitsTarget(a.sink))
                EditorGUILayout.HelpBox(
                    "Sink " + a.sink + " is not available on " + EditorUserBuildSettings.activeBuildTarget +
                    ". A named backend is a demand, so a build for that target fails to start. Use Auto " +
                    "for a project that ships to more than one platform.", MessageType.Warning);

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

            if (a.profile != BwaProfile.Cave && a.enableRoomBox == false && a.enableReflections)
                EditorGUILayout.HelpBox(
                    "Reflections are on but there is no acoustic geometry: add a Room Box, or one or more " +
                    "AcousticGeometry objects. With nothing to reflect off, the bed stays silent.",
                    MessageType.Info);
        }

        // The questions you actually ask while the scene is running: did I get a real audio device, or
        // did it silently fall back to the null sink? How many voices am I burning? Is anything coming
        // out of the speakers at all?
        static void DrawLiveStatus(Engine a)
        {
            if (!Application.isPlaying || !a.Ready) return;

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Live", EditorStyles.boldLabel);

            string backend = Bwa.Backend(a.Handle) ?? "?";
            // A real device is anything that is not the silent offline sink or the caller-pumped one.
            // Checking for the "asio:" prefix stopped being the right test the moment a headphone
            // profile could legitimately open a WASAPI endpoint instead.
            bool live = !(backend == "null" || backend.StartsWith("null ") ||
                          backend == "manual" || backend.StartsWith("manual ") || backend == "none");
            var kind = live ? MessageType.Info : MessageType.Warning;
            EditorGUILayout.HelpBox(
                $"backend: {backend}{(live ? "" : "  (SILENT — no audio device)")}\n" +
                $"channels: {a.ChannelCount}\n" +
                $"active voices: {a.ActiveVoices}", kind);

            // Internal tracking (NatNet): only meaningful when the engine reads the pose itself. A connect
            // that "succeeded" only opened the socket — it can still be dead on the wire, and if the stream
            // drops mid-session nothing tears it down. This is the live truth, so you catch it here instead
            // of by noticing the listener has stopped moving.
            if (!a.feedListener)
            {
                string msg; MessageType tk;
                switch (a.TrackerStatus)
                {
                    case BwaTrackerState.Live:
                        msg = "tracker: LIVE — head pose is arriving and current."; tk = MessageType.Info; break;
                    case BwaTrackerState.NoData:
                        msg = "tracker: NO DATA — no frames arriving. Check Motive is streaming, the network/" +
                              "interface, and the data port or multicast group. The listener is frozen at its " +
                              "last pose.";
                        tk = MessageType.Warning; break;
                    case BwaTrackerState.NoBody:
                        msg = "tracker: NO BODY — frames are arriving but the followed rigid body has no pose. " +
                              "Check the rigid body id/name, or the head is occluded right now. The listener is " +
                              "frozen at its last pose.";
                        tk = MessageType.Warning; break;
                    default:
                        msg = "tracker: DISCONNECTED — the NatNet connect did not succeed (see the console). The " +
                              "listener stays at the committed/default pose.";
                        tk = MessageType.Warning; break;
                }
                EditorGUILayout.HelpBox(msg, tk);
            }

            var peaks = a.BusLevels();
            for (int i = 0; i < peaks.Length; i++)
            {
                var r = EditorGUILayout.GetControlRect(false, 10f);
                EditorGUI.ProgressBar(r, Mathf.Clamp01(peaks[i]), $"{i}");
            }
        }
    }
}
