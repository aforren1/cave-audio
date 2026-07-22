// SpeakerView.cs — a LIVE, in-build view of speaker activity: one glowing marker per channel, sitting
// at the real speaker's position, brightening with that channel's output.
//
// This is the runtime counterpart to Engine's scene-view gizmos. Gizmos are an editor feature — they do
// not render in a build — so seeing the array light up while you are STANDING IN THE CAVE needs actual
// renderers, which is what this makes. Same data behind both (bwa_get_speakers + bwa_get_bus_levels).
//
// The markers are UNLIT on purpose: a CAVE is dark, and a lit material would need lights and would read
// as geometry. Unlit means the colour you compute IS the colour you see, so a hot channel glows.
//
// It reads levels and draws; it never writes audio state. Deleting it changes nothing you can hear.
using System;
using System.Collections;
using UnityEngine;

namespace BwAudio
{
    [DisallowMultipleComponent]
    public sealed class SpeakerView : MonoBehaviour
    {
        [Header("Markers")]
        [Tooltip("Marker radius in metres.")]
        [Range(0.01f, 0.5f)] public float radius = 0.08f;
        [Tooltip("Grow the marker with level as well as brightening it — easier to read from across the room.")]
        public bool scaleWithLevel = true;
        [Range(1f, 3f)] public float maxScale = 1.8f;

        [Header("Colour")]
        public Color idle = new Color(0.10f, 0.22f, 0.35f);   // dim, but visible: you can still see the array
        public Color hot  = new Color(1.00f, 0.60f, 0.15f);

        [Header("Ballistics")]
        [Tooltip("Peak-hold decay, in level-units per second. The engine reports a per-BLOCK peak, which " +
                 "flickers; instant attack + a slow release is what makes a meter readable.")]
        [Range(0.5f, 20f)] public float releasePerSecond = 3.0f;

        Transform[] _markers;
        Renderer[] _renderers;
        float[] _shown;
        Material _material;
        MaterialPropertyBlock _mpb;
        int _colorId = -1;

        // OnEnable, not Start: Start runs once for the lifetime of the component, so toggling this view
        // off and back on would tear the markers down and never rebuild them.
        void OnEnable() => StartCoroutine(BuildWhenReady());

        IEnumerator BuildWhenReady()
        {
            while (Engine.Instance == null || !Engine.Instance.Ready) yield return null;   // wait out init order
            Build();
        }

        void Build()
        {
            Teardown();
            var eng = Engine.Instance;
            float[] xyz = eng.SpeakerPositions();          // ROOM space, channel order — the engine's own truth
            int n = xyz.Length / 3;
            if (n == 0) return;

            _material = MakeUnlitMaterial();
            if (_material == null)
            {
                Debug.LogWarning("[SpeakerView] no unlit shader found for this render pipeline — no markers drawn.");
                return;
            }
            _mpb = new MaterialPropertyBlock();

            _markers   = new Transform[n];
            _renderers = new Renderer[n];
            _shown     = new float[n];
            for (int i = 0; i < n; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                go.name = "speaker " + i;                  // = the channel index
                go.hideFlags = HideFlags.DontSave;
                var col = go.GetComponent<Collider>();
                if (col) Destroy(col);                     // a marker must never be in the physics scene

                var t = go.transform;
                t.SetParent(transform, worldPositionStays: false);
                t.position   = Room.FromRoom(new Vector3(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]));
                t.localScale = Vector3.one * (radius * 2f);

                _markers[i]   = t;
                _renderers[i] = go.GetComponent<Renderer>();
                _renderers[i].sharedMaterial = _material;  // shared + a property block: no per-object material leak
            }
        }

        void Update()
        {
            if (_renderers == null) return;
            var eng = Engine.Instance;
            if (eng == null || !eng.Ready) return;

            float[] peaks = eng.BusLevels();
            float decay = releasePerSecond * Time.deltaTime;

            for (int i = 0; i < _renderers.Length; i++)
            {
                float peak = (i < peaks.Length) ? Mathf.Clamp01(peaks[i]) : 0f;
                // Instant attack, slow release: the raw per-block peak strobes too fast to read.
                _shown[i] = Mathf.Max(peak, _shown[i] - decay);

                float lvl = _shown[i];
                _mpb.SetColor(_colorId, Color.Lerp(idle, hot, lvl));
                _renderers[i].SetPropertyBlock(_mpb);

                // Recomputed every frame (not only when scaling with level) so dragging `radius` in the
                // inspector during a session resizes the markers instead of doing nothing.
                float grow = scaleWithLevel ? Mathf.Lerp(1f, maxScale, lvl) : 1f;
                _markers[i].localScale = Vector3.one * (radius * 2f * grow);
            }
        }

        // The colour property differs by pipeline (URP/HDRP use _BaseColor, built-in Unlit uses _Color),
        // so pick the shader first, then ask the material which one it actually has.
        Material MakeUnlitMaterial()
        {
            string[] shaders = { "Universal Render Pipeline/Unlit", "HDRP/Unlit", "Unlit/Color", "Sprites/Default" };
            foreach (var name in shaders)
            {
                var sh = Shader.Find(name);
                if (sh == null) continue;
                var m = new Material(sh) { hideFlags = HideFlags.DontSave };
                foreach (var prop in new[] { "_BaseColor", "_UnlitColor", "_Color" })
                {
                    if (!m.HasProperty(prop)) continue;
                    _colorId = Shader.PropertyToID(prop);
                    return m;
                }
                DestroyImmediate(m);
            }
            return null;
        }

        void OnDisable() => Teardown();

        void Teardown()
        {
            if (_markers != null)
                foreach (var t in _markers) if (t) Destroy(t.gameObject);
            if (_material) Destroy(_material);
            _markers = null; _renderers = null; _shown = null; _material = null;
        }
    }
}
