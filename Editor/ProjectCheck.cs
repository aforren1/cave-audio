// ProjectCheck.cs — editor-only guardrail.
//
// The bw_audio engine OWNS the audio device (ASIO/Dante + the binaural monitor). Unity's built-in
// audio pipeline must therefore be OFF — left enabled it opens its own output device (wasted CPU,
// possible contention for the monitor's headphones) and any stray AudioSource plays the WRONG path
// instead of the speaker array. The switch is Project Settings > Audio > "Disable Unity Audio"
// (the AudioManager's m_DisableAudio), which Unity reads at startup — so it cannot be flipped from a
// runtime script. This warns when it's on and offers one-click disable. It does NOT change the
// setting automatically (silently editing project settings on import is surprising).
using UnityEditor;
using UnityEngine;

namespace BwAudio.EditorTools
{
    [InitializeOnLoad]
    public static class ProjectCheck
    {
        const string AudioManagerPath = "ProjectSettings/AudioManager.asset";

        static ProjectCheck() => EditorApplication.delayCall += WarnIfEnabled;

        static void WarnIfEnabled()
        {
            if (!IsUnityAudioDisabled())
                Debug.LogWarning(
                    "[bw_audio] Unity's built-in audio is ENABLED. The bw_audio engine owns the audio " +
                    "device, so leaving Unity audio on wastes CPU, can contend for the monitor's output " +
                    "device, and plays stray AudioSources the wrong way (not through the CAVE array). " +
                    "Run Tools > BwAudio > Disable Unity Audio, or tick Project Settings > " +
                    "Audio > 'Disable Unity Audio'.");
        }

        // The same check, asked for by a setup step (CaveRig) instead of by the editor loading. It
        // may offer the fix because the user just asked for a working rig. A dialog would hang a
        // batchmode editor, so that case only warns.
        internal static void CheckFromSetup()
        {
            if (IsUnityAudioDisabled()) return;
            if (!Application.isBatchMode &&
                EditorUtility.DisplayDialog("BwAudio",
                    "Unity's built-in audio is enabled. The bw_audio engine owns the audio device, so " +
                    "Unity audio should be off.\n\nDisable it now? (Project Settings > Audio > " +
                    "Disable Unity Audio)", "Disable Unity Audio", "Not now"))
                DisableUnityAudio();
            else
                WarnIfEnabled();
        }

        [MenuItem("Tools/BwAudio/Disable Unity Audio")]
        static void DisableUnityAudio()
        {
            if (SetDisableAudio(true))
                Debug.Log("[bw_audio] Unity built-in audio disabled. Re-enter Play mode to apply.");
        }

        // gray out the menu item once it's already disabled
        [MenuItem("Tools/BwAudio/Disable Unity Audio", isValidateFunction: true)]
        static bool DisableUnityAudio_Validate() => !IsUnityAudioDisabled();

        static SerializedObject LoadAudioManager()
        {
            var objs = AssetDatabase.LoadAllAssetsAtPath(AudioManagerPath);
            return (objs != null && objs.Length > 0 && objs[0] != null) ? new SerializedObject(objs[0]) : null;
        }

        internal static bool IsUnityAudioDisabled()
        {
            var p = LoadAudioManager()?.FindProperty("m_DisableAudio");
            return p != null && p.boolValue;   // can't read it -> assume not disabled (warn, don't hide)
        }

        static bool SetDisableAudio(bool value)
        {
            var so = LoadAudioManager();
            var p = so?.FindProperty("m_DisableAudio");
            if (p == null) { Debug.LogError("[bw_audio] could not access AudioManager.m_DisableAudio."); return false; }
            p.boolValue = value;
            so.ApplyModifiedProperties();
            AssetDatabase.SaveAssets();
            return true;
        }
    }
}
