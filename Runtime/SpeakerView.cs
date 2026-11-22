// SpeakerView.cs — a LIVE, in-build view of speaker activity: one glowing marker per channel, sitting
// at the real speaker's position, brightening with that channel's output.
//
// This is the runtime counterpart to Engine's scene-view gizmos. Gizmos are an editor feature — they do
// not render in a build — so seeing the array light up while you are STANDING IN THE CAVE needs actual
// renderers, which is what this makes. Same data behind both (bwa_get_speakers + bwa_get_bus_levels).
//
// The markers are UNLIT on purpose: a CAVE is dark, and a lit material would need lights and would read
// as geometry. Unlit means the color you compute IS the color you see, so a hot channel glows.
//
// It reads levels and draws; it never writes audio state. Deleting it changes nothing you can hear.
//
// The levels are the SPEAKER BUS's (Engine.BusLevels), so the view means what it says in the array
// profiles and in CaveSim, whose virtual speakers are that bus. In Binaural it does not: point
// sources and beds bypass the bus there, so the markers show only the reverb and diffuse taps and
// stay dark under a dry source that is plainly audible. That is the bus being empty, not a fault.
using System;
using System.Collections;
using UnityEngine;

namespace BwAudio
{
    [DisallowMultipleComponent]
    public sealed class SpeakerView : MonoBehaviour
    {
        [Header("Markers")]
        [Tooltip("Marker radius in meters.")]
        [Range(0.01f, 0.5f)] public float radius = 0.08f;
        [Tooltip("Grow the marker with level as well as brightening it — easier to read from across the room.")]
        public bool scaleWithLevel = true;
        [Range(1f, 3f)] public float maxScale = 1.8f;

        [Header("Color")]
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
        int _roomRev;

        // OnEnable, not Start: Start runs once for the lifetime of the component, so toggling this view
        // off and back on would tear the markers down and never rebuild them.
        void OnEnable() => StartCoroutine(BuildWhenReady());

        IEnumerator BuildWhenReady()
        {
            // Wait out init order: until the Engine's Start has decided, a not-Ready Engine is still
            // coming up, and the layout file is not what it will pan over. Once decided without a
            // running engine (a node that does not own audio, or a failed start) SpeakerPositions
            // falls back to the layout file, so the markers still stand where the speakers are, unlit.
            while (!Engine.Instance || !Engine.Instance.Decided) yield return null;
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
                Debug.LogWarning("[SpeakerView] no unlit shader found for this render pipeline, so no markers are drawn.");
                return;
            }
            _mpb = new MaterialPropertyBlock();

            _roomRev   = Room.Revision;
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
            if (eng == null) return;

            // The marker positions bake the registration, so a moved Engine.roomOrigin re-places them.
            if (_roomRev != Room.Revision)
            {
                _roomRev = Room.Revision;
                float[] xyz = eng.SpeakerPositions();
                for (int i = 0; i < _markers.Length && i * 3 + 2 < xyz.Length; i++)
                    _markers[i].position = Room.FromRoom(new Vector3(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]));
            }

            float[] peaks = eng.BusLevels();               // empty without an engine: the markers idle
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

        // Shared with RoomView: same pipeline-dependent shader and color-property search. See
        // UnlitMaterial.cs for why the property is discovered rather than assumed.
        Material MakeUnlitMaterial() => UnlitMaterial.Make(out _colorId);

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
