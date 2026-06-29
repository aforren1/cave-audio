// BwClipAttribute.cs — marks a string field as a path to an audio file under StreamingAssets.
// The editor (BwClipDrawer) shows a picker of the available .wav/.flac/.mp3 files instead of a raw
// text box, and flags missing files. At runtime the value is just the StreamingAssets-relative path
// the engine loads (the engine decodes the file itself — Unity's AudioClip import is bypassed).
using UnityEngine;

namespace CaveAudio
{
    public sealed class BwClipAttribute : PropertyAttribute { }
}
