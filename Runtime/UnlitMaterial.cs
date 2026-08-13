// UnlitMaterial.cs — one unlit material, whichever render pipeline the project uses.
//
// Every in-build viewer here (SpeakerView, RoomView) wants the same thing: a material where the color
// you compute IS the color you see. A CAVE is dark and has no lighting rig, so a lit material would
// need lights set up and would still read as geometry rather than as an indicator.
//
// The shader name and the color property both differ by pipeline (URP wants _BaseColor, built-in Unlit
// wants _Color), so this picks a shader that exists and then asks the material which color property it
// actually has, rather than assuming either.
using UnityEngine;

namespace BwAudio
{
    internal static class UnlitMaterial
    {
        /// <summary>
        /// Create an unlit material for the active pipeline. Returns null when none of the known shaders
        /// is present, which is the caller's cue to warn and draw nothing rather than to draw magenta.
        /// `colorId` is the shader property id to use with a MaterialPropertyBlock.
        /// </summary>
        internal static Material Make(out int colorId)
        {
            colorId = -1;
            string[] shaders = { "Universal Render Pipeline/Unlit", "HDRP/Unlit", "Unlit/Color", "Sprites/Default" };
            foreach (var name in shaders)
            {
                var sh = Shader.Find(name);
                if (sh == null) continue;
                var m = new Material(sh) { hideFlags = HideFlags.DontSave };
                foreach (var prop in new[] { "_BaseColor", "_UnlitColor", "_Color" })
                {
                    if (!m.HasProperty(prop)) continue;
                    colorId = Shader.PropertyToID(prop);
                    return m;
                }
                Object.DestroyImmediate(m);
            }
            return null;
        }
    }
}
