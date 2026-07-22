// ClipAttribute.cs — marks a string field as a path to a file under StreamingAssets.
//
// The editor (ClipDrawer) shows a picker of the files actually present there, instead of a raw text
// box, and flags a missing one in red — the field is a StreamingAssets-RELATIVE path, which is not
// something an inspector can otherwise tell you. At runtime the value is just that path; the engine
// loads the file itself (its own decoder for audio, its own JSON parser for a layout — Unity's asset
// import is bypassed entirely, which is why these are loose files and not AudioClips/TextAssets).
//
// Defaults to the audio extensions; pass others for a different kind of file:
//   [Clip]                       // .wav / .flac / .mp3
//   [Clip(".json")]              // the speaker layout
using UnityEngine;

namespace BwAudio
{
    public sealed class ClipAttribute : PropertyAttribute
    {
        /// <summary>Extensions the picker lists, lowercase, dot-prefixed. Empty = the audio default.</summary>
        public readonly string[] Extensions;

        public ClipAttribute(params string[] extensions)
        {
            Extensions = (extensions != null && extensions.Length > 0)
                ? extensions
                : new[] { ".wav", ".flac", ".mp3" };
        }
    }
}
