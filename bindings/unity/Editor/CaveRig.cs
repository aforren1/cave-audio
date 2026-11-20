// CaveRig.cs - GameObject > BwAudio > CAVE Rig: the scene setup in one step.
//
// A menu item, not a prefab. A prefab inside a package is read-only, so every project would live in
// overrides of it, and it adds GUIDs that must stay stable forever. A menu item writes plain scene
// objects the project owns.
//
// What it makes:
//
//   BwAudio                 Engine. Layout File pre-filled, Listener bound to the main camera.
//     Room Origin           assigned to Engine.roomOrigin. Move it to the CAVE floor center.
//     Speaker View          SpeakerView, component OFF.
//     Room                  RoomConstraints + RoomView (OFF), only when constraints.json exists.
//
// Room Origin is a CHILD of the Engine object. The Engine is DontDestroyOnLoad and a sibling origin
// is not, so a sibling dies on the first scene load and the Engine falls back to "keeping the last
// registration" with a warning. A child rides along. The other case is a second scene that carries
// its own rig: that Engine destroys itself in Awake, and Engine.AdoptRoomOrigin moves an origin
// parented under it to the survivor first, world pose kept. The cost of a child is that moving the
// BwAudio object moves the origin with it, so the root is created at the world origin and has no
// reason to move.
//
// The two views render in a BUILD, in front of a participant, so their components start disabled.
// RoomConstraints draws gizmos only and starts enabled.
//
// Create() is the whole of the work and shows no dialog, so a test or a project's own setup script
// can call it. The menu item adds the Unity-audio check, which may ask a question.
using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using Object = UnityEngine.Object;

namespace BwAudio.EditorTools
{
    public static class CaveRig
    {
        public const string RootName        = "BwAudio";
        public const string OriginName      = "Room Origin";
        public const string SpeakerViewName = "Speaker View";
        public const string RoomName        = "Room";

        // In preference order. trims.json is the calibrated layout (bwa_calibrate's trim step writes
        // it), cave_layout.json the surveyed one before trims.
        static readonly string[] LayoutCandidates = { "trims.json", "cave_layout.json" };
        const string ConstraintsFile = "constraints.json";

        // Priority 10 is what puts a GameObject/ item in the Hierarchy's right-click Create menu too.
        [MenuItem("GameObject/BwAudio/CAVE Rig", false, 10)]
        static void CreateFromMenu()
        {
            // The Hierarchy calls a GameObject/ item once per selected object. The second call finds
            // the first call's Engine and stops there, and the audio question is asked once.
            if (Create(out bool created) && created) ProjectCheck.CheckFromSetup();
        }

        // Greyed out in Play mode. Create refuses there too, for a caller that is not the menu.
        [MenuItem("GameObject/BwAudio/CAVE Rig", true)]
        static bool CreateFromMenu_Validate() => !Application.isPlaying;

        // The name a file under `dir` really has on disk, matched without regard to case, or null.
        // Windows and macOS open "trims.json" when the file is "Trims.json". A Linux or Android
        // player does not, so the field must carry the real spelling.
        static string OnDiskName(string dir, string name)
        {
            if (!Directory.Exists(dir)) return null;
            foreach (var path in Directory.GetFiles(dir))
            {
                var found = Path.GetFileName(path);
                if (string.Equals(found, name, StringComparison.OrdinalIgnoreCase)) return found;
            }
            return null;
        }

        static string SceneLabel(Scene s) => string.IsNullOrEmpty(s.name) ? "an untitled scene" : "scene '" + s.name + "'";

        /// <summary>The Layout File value for this project: the first candidate that exists at the
        /// top of `streamingAssetsDir`, as a path relative to it (how Engine.layoutFile is stored)
        /// and in the file's own on-disk case, or "" when there is none. "" is the Engine's spelling
        /// of "run the built-in default grid".</summary>
        public static string PickLayoutFile(string streamingAssetsDir)
        {
            foreach (var name in LayoutCandidates)
            {
                var found = OnDiskName(streamingAssetsDir, name);
                if (found != null) return found;
            }
            return "";
        }

        /// <summary>Create the rig in the active scene and select it. Returns the Engine.</summary>
        public static Engine Create() => Create(out _);

        /// <summary>Create the rig in the active scene and select it. When any loaded scene already
        /// has an Engine, nothing is created: that Engine is selected and returned, and `created` is
        /// false. In Play mode and in a prefab stage it refuses with a log line and returns null.
        /// One undo step removes everything this made.</summary>
        public static Engine Create(out bool created)
        {
            created = false;
            // AddComponent in Play mode runs Engine.Awake at once, before any field below is set:
            // the Engine would register the room with no origin, and the whole rig would vanish on
            // Stop.
            if (Application.isPlaying)
            {
                Debug.Log("[bw_audio] CAVE Rig: not in Play mode. The Engine would start before it " +
                          "is configured, and the rig would be gone on Stop. Stop, then run it again.");
                return null;
            }
            // In a prefab stage the rig would land in the stage or in the hidden main scene,
            // depending on what is selected. The Engine is a scene singleton, not prefab content.
            if (PrefabStageUtility.GetCurrentPrefabStage() != null)
            {
                Debug.Log("[bw_audio] CAVE Rig: a prefab is open for editing. The rig belongs in a " +
                          "scene. Leave the prefab stage, then run it again.");
                return null;
            }
            // Inactive ones count: a disabled rig is still the scene's rig, and a second Engine would
            // destroy itself in Awake the moment the first is switched on.
            var existing = Object.FindObjectsByType<Engine>(FindObjectsInactive.Include, FindObjectsSortMode.None);
            if (existing.Length > 0)
            {
                Selection.activeGameObject = existing[0].gameObject;
                Debug.Log("[bw_audio] CAVE Rig: there is already an Engine on '" + existing[0].name +
                          "' in " + SceneLabel(existing[0].gameObject.scene) + ". Engine is a " +
                          "singleton, so nothing was created. The existing one is selected.",
                          existing[0]);
                return existing[0];
            }

            Undo.IncrementCurrentGroup();
            Undo.SetCurrentGroupName("Create CAVE Rig");

            // Always a scene root, whatever was right-clicked: DontDestroyOnLoad needs a root object.
            var root = new GameObject(RootName);
            // Registered before anything is added, so the children and components below are part of
            // the same created object and one undo removes the lot.
            Undo.RegisterCreatedObjectUndo(root, "Create CAVE Rig");
            var engine = root.AddComponent<Engine>();

            string dir = Application.streamingAssetsPath;
            engine.layoutFile = PickLayoutFile(dir);
            if (engine.layoutFile.Length == 0)
                Debug.LogWarning("[bw_audio] CAVE Rig: no trims.json or cave_layout.json at the top " +
                                 "of Assets/StreamingAssets (subfolders are not searched), so Layout " +
                                 "File is empty and the engine runs its built-in default grid, not " +
                                 "your room. Put the calibrated layout there, or pick a file in a " +
                                 "subfolder with the Engine's Layout File.", engine);

            var origin = Child(root, OriginName);
            engine.roomOrigin = origin.transform;

            // The Engine has one way to get the head pose from Unity: its `listener` Transform. The
            // main camera is the right guess at a desk and in a headset. A tracked CAVE replaces it
            // with the head rigid body, or turns Feed Listener off.
            // Only a camera in the rig's OWN scene. Unity does not save a reference across scenes:
            // it would read as assigned now and come back null after a save and reload.
            var cam = MainCameraFor(root.scene, out var elsewhere);
            if (cam) engine.listener = cam.transform;
            else if (elsewhere)
                Debug.LogWarning("[bw_audio] CAVE Rig: the camera tagged MainCamera ('" + elsewhere.name +
                                 "') is in " + SceneLabel(elsewhere.gameObject.scene) + ", and the rig " +
                                 "is in " + SceneLabel(root.scene) + ". A reference across scenes is " +
                                 "lost on save, so the Engine's Listener is not assigned. Move the rig " +
                                 "into the camera's scene, or assign the Listener from a script at " +
                                 "run time.", engine);
            else
                Debug.LogWarning("[bw_audio] CAVE Rig: no camera is tagged MainCamera, so the Engine's " +
                                 "Listener is not assigned and the listener will not move. Assign the " +
                                 "tracked head or the XR camera, or turn Feed Listener off to let the " +
                                 "engine read NatNet itself.", engine);

            Child(root, SpeakerViewName).AddComponent<SpeakerView>().enabled = false;

            var constraints = OnDiskName(dir, ConstraintsFile);
            if (constraints != null)
            {
                var room = Child(root, RoomName);
                room.AddComponent<RoomConstraints>().constraintsFile = constraints;   // the on-disk case
                room.AddComponent<RoomView>().enabled = false;
            }

            Selection.activeGameObject = root;
            Debug.Log("[bw_audio] CAVE Rig created. Next: move '" + OriginName + "' to the CAVE floor " +
                      "center, on the floor. Its blue arrow is the layout's front (+z). If the layout's " +
                      "front is the CAVE's back wall, rotate it 180 degrees about Y. Speaker View and " +
                      "Room View are off: turn them on to see the array and the room in a build.", origin);
            created = true;
            return engine;
        }

        // The enabled MainCamera in `scene`, or null. `elsewhere` is one in another loaded scene.
        static Camera MainCameraFor(Scene scene, out Camera elsewhere)
        {
            elsewhere = null;
            foreach (var cam in Object.FindObjectsByType<Camera>(FindObjectsSortMode.None))
            {
                if (!cam.enabled || !cam.CompareTag("MainCamera")) continue;
                if (cam.gameObject.scene == scene) return cam;
                if (!elsewhere) elsewhere = cam;
            }
            return null;
        }

        static GameObject Child(GameObject parent, string name)
        {
            var go = new GameObject(name);
            go.transform.SetParent(parent.transform, worldPositionStays: false);
            return go;
        }
    }
}
