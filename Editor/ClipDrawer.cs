// ClipDrawer.cs — editor picker for [Clip] string fields. Lists the files actually present under
// StreamingAssets (so you don't type paths by hand), flags a missing file in red, and offers a browse
// button scoped to StreamingAssets. The stored value stays a StreamingAssets-relative path.
//
// The extension set comes from the attribute ([Clip] = audio, [Clip(".json")] = the speaker
// layout), so the file lists are cached per extension set rather than globally.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace BwAudio.EditorTools
{
    [CustomPropertyDrawer(typeof(ClipAttribute))]
    public sealed class ClipDrawer : PropertyDrawer
    {
        class Cache { public string[] Files = Array.Empty<string>(); public double NextScan; }
        static readonly Dictionary<string, Cache> _caches = new Dictionary<string, Cache>();

        public override void OnGUI(Rect pos, SerializedProperty prop, GUIContent label)
        {
            if (prop.propertyType != SerializedPropertyType.String) { EditorGUI.PropertyField(pos, prop, label); return; }
            var exts = ((ClipAttribute)attribute).Extensions;
            var files = Scan(exts);

            string cur = prop.stringValue ?? "";
            var opts = files.ToList();
            int idx = opts.IndexOf(cur);
            bool missing = cur.Length > 0 && idx < 0;
            if (idx < 0) { opts.Insert(0, cur.Length == 0 ? "(none)" : cur + "  — not found"); idx = 0; }

            const float browseW = 24f, gap = 2f;
            var popupR  = new Rect(pos.x, pos.y, pos.width - browseW - gap, pos.height);
            var browseR = new Rect(pos.xMax - browseW, pos.y, browseW, pos.height);

            // The tooltip carries the one thing the field can't show: WHERE the file has to live.
            string tip = missing
                ? "Not found under Assets/StreamingAssets/ — put the file there; the path is relative to it."
                : (string.IsNullOrEmpty(label.tooltip)
                    ? "A path RELATIVE to Assets/StreamingAssets/ (the engine loads the file itself)."
                    : label.tooltip);
            var content = new GUIContent(label.text, tip);
            var prev = GUI.color; if (missing) GUI.color = new Color(1f, 0.7f, 0.7f);
            EditorGUI.BeginChangeCheck();
            int sel = EditorGUI.Popup(popupR, content, idx, opts.Select(o => new GUIContent(o)).ToArray());
            GUI.color = prev;
            if (EditorGUI.EndChangeCheck() && sel >= 0 && sel < opts.Count && files.Contains(opts[sel]))
                prop.stringValue = opts[sel];

            if (GUI.Button(browseR, "…"))
            {
                string root = Application.streamingAssetsPath;
                string filter = string.Join(",", exts.Select(e => e.TrimStart('.')));
                string picked = EditorUtility.OpenFilePanel("File under StreamingAssets", root, filter);
                if (!string.IsNullOrEmpty(picked))
                {
                    string full = picked.Replace('\\', '/'), rootN = root.Replace('\\', '/');
                    if (full.StartsWith(rootN + "/"))
                    {
                        prop.stringValue = full.Substring(rootN.Length + 1);
                        Scan(exts, force: true);
                    }
                    else EditorUtility.DisplayDialog("BwAudio",
                        "Pick a file UNDER Assets/StreamingAssets — the engine loads it from there at runtime.", "OK");
                }
            }
        }

        static string[] Scan(string[] exts, bool force = false)
        {
            string key = string.Join("|", exts);
            if (!_caches.TryGetValue(key, out var c)) _caches[key] = c = new Cache();
            if (!force && EditorApplication.timeSinceStartup < c.NextScan) return c.Files;   // throttle the walk
            c.NextScan = EditorApplication.timeSinceStartup + 2.0;

            string root = Application.streamingAssetsPath;
            if (!Directory.Exists(root)) { c.Files = Array.Empty<string>(); return c.Files; }
            try
            {
                c.Files = Directory.GetFiles(root, "*.*", SearchOption.AllDirectories)
                    .Where(f => exts.Contains(Path.GetExtension(f).ToLowerInvariant()))
                    .Select(f => f.Substring(root.Length + 1).Replace('\\', '/'))
                    .OrderBy(s => s, StringComparer.OrdinalIgnoreCase).ToArray();
            }
            catch (Exception) { }   // an inaccessible subdir / reparse point shouldn't break the inspector
            return c.Files;         // (keep the last good list on failure)
        }
    }
}
