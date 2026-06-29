// BwClipDrawer.cs — editor picker for [BwClip] string fields. Lists the audio files actually present
// under StreamingAssets (so you don't type paths by hand), flags a missing file in red, and offers a
// browse button scoped to StreamingAssets. The stored value stays a StreamingAssets-relative path.
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace CaveAudio.EditorTools
{
    [CustomPropertyDrawer(typeof(BwClipAttribute))]
    public sealed class BwClipDrawer : PropertyDrawer
    {
        static string[] _files = Array.Empty<string>();
        static double _nextScan;
        static readonly string[] Exts = { ".wav", ".flac", ".mp3" };

        public override void OnGUI(Rect pos, SerializedProperty prop, GUIContent label)
        {
            if (prop.propertyType != SerializedPropertyType.String) { EditorGUI.PropertyField(pos, prop, label); return; }
            Scan();

            string cur = prop.stringValue ?? "";
            var opts = _files.ToList();
            int idx = opts.IndexOf(cur);
            bool missing = cur.Length > 0 && idx < 0;
            if (idx < 0) { opts.Insert(0, cur.Length == 0 ? "(none)" : cur + "  — not found"); idx = 0; }

            const float browseW = 24f, gap = 2f;
            var popupR  = new Rect(pos.x, pos.y, pos.width - browseW - gap, pos.height);
            var browseR = new Rect(pos.xMax - browseW, pos.y, browseW, pos.height);

            var content = new GUIContent(label.text, missing ? "File not found under StreamingAssets" : label.tooltip);
            var prev = GUI.color; if (missing) GUI.color = new Color(1f, 0.7f, 0.7f);
            EditorGUI.BeginChangeCheck();
            int sel = EditorGUI.Popup(popupR, content, idx, opts.Select(o => new GUIContent(o)).ToArray());
            GUI.color = prev;
            if (EditorGUI.EndChangeCheck() && sel >= 0 && sel < opts.Count && _files.Contains(opts[sel]))
                prop.stringValue = opts[sel];

            if (GUI.Button(browseR, "…"))
            {
                string root = Application.streamingAssetsPath;
                string picked = EditorUtility.OpenFilePanel("Audio file (under StreamingAssets)", root, "wav,flac,mp3");
                if (!string.IsNullOrEmpty(picked))
                {
                    string full = picked.Replace('\\', '/'), rootN = root.Replace('\\', '/');
                    if (full.StartsWith(rootN + "/")) { prop.stringValue = full.Substring(rootN.Length + 1); _nextScan = 0; }
                    else EditorUtility.DisplayDialog("BwAudio",
                        "Pick a file UNDER StreamingAssets — the engine loads it from there at runtime.", "OK");
                }
            }
        }

        static void Scan()
        {
            if (EditorApplication.timeSinceStartup < _nextScan) return;   // throttle the directory walk
            _nextScan = EditorApplication.timeSinceStartup + 2.0;
            string root = Application.streamingAssetsPath;
            if (!Directory.Exists(root)) { _files = Array.Empty<string>(); return; }
            _files = Directory.GetFiles(root, "*.*", SearchOption.AllDirectories)
                .Where(f => Exts.Contains(Path.GetExtension(f).ToLowerInvariant()))
                .Select(f => f.Substring(root.Length + 1).Replace('\\', '/'))
                .OrderBy(s => s, StringComparer.OrdinalIgnoreCase).ToArray();
        }
    }
}
