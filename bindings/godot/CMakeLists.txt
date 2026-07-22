# ---- Godot GDExtension binding (bindings/godot) ----
#
# Builds addons/bw_audio/bin/bw_audio_gd.<platform>.<target>.<arch>.dll — the GDExtension
# Godot loads. It links bw_audio like any other client: control calls only, no rendered
# audio crosses the boundary (docs/integration.md).
#
# godot-cpp is pinned to a COMMIT, not a branch: its release branches lag Godot (4.5 is the
# newest cut) while master already ships extension_api.json for 4.7 — which is what the
# installed editor is. Bump the commit when the editor moves, and check the api json header
# (gdextension/extension_api.json) still matches.
#
# GODOTCPP_TARGET is a build-wide choice ("editor" | "template_debug" | "template_release"),
# so one configure tree yields one library flavour. "editor" is the one the editor itself
# loads, hence the default here; a shipping build configures a second tree with
# -DGODOTCPP_TARGET=template_release.

include(FetchContent)

set(BWA_GODOT_CPP_COMMIT "ba0edfed90512ec64aba51d4295a3e7e30112f86"  # master @ 2026-06-29, api 4.7.stable
    CACHE STRING "godot-cpp commit to build against")

# godot-cpp defaults GODOTCPP_TARGET to template_debug, which produces a library the EDITOR
# will not load — so a plain -DBWA_BUILD_GODOT=ON would build successfully and then appear
# broken when you opened the project. Default to editor instead; a shipping build configures
# a second tree with -DGODOTCPP_TARGET=template_release (see tools/godot/pack.ps1, which
# builds both).
if(NOT DEFINED GODOTCPP_TARGET)
  set(GODOTCPP_TARGET "editor" CACHE STRING "godot-cpp build profile: editor|template_debug|template_release")
endif()

FetchContent_Declare(godot_cpp
  GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
  GIT_TAG ${BWA_GODOT_CPP_COMMIT}
  GIT_SHALLOW FALSE)          # a bare commit is not fetchable shallow
FetchContent_MakeAvailable(godot_cpp)

add_library(bwa_gdextension SHARED
  src/register_types.cpp
  src/bwa_engine_node.cpp
  src/bwa_source_base.cpp
  src/bwa_emitter_node.cpp
  src/bwa_push_source.cpp
  src/bwa_bed_node.cpp
  src/bwa_material.cpp
  src/bwa_geometry.cpp
  src/bwa_speaker_view.cpp)

target_link_libraries(bwa_gdextension PRIVATE godot-cpp bw_audio)
target_compile_features(bwa_gdextension PRIVATE cxx_std_17)

# godot-cpp hands the platform/target/arch suffix out as a target property, so the built file
# name matches the keys in bw_audio.gdextension without us restating the triple.
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

set(BWA_GODOT_BIN "${CMAKE_CURRENT_SOURCE_DIR}/addons/bw_audio/bin")
set_target_properties(bwa_gdextension PROPERTIES
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
  POSITION_INDEPENDENT_CODE ON
  PREFIX ""
  OUTPUT_NAME "bw_audio_gd${GODOTCPP_SUFFIX}"
  # The generator expression keeps MSVC from appending a per-config subdirectory — the
  # .gdextension manifest names one fixed path.
  LIBRARY_OUTPUT_DIRECTORY "$<1:${BWA_GODOT_BIN}>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${BWA_GODOT_BIN}>"
  PDB_OUTPUT_DIRECTORY     "$<1:${BWA_GODOT_BIN}>")

# bw_audio.dll (and phonon.dll, when the Steam Audio build is on) must sit beside the
# extension: Godot loads the extension by absolute path, and Windows resolves its imports
# from that directory.
add_custom_command(TARGET bwa_gdextension POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:bw_audio> ${BWA_GODOT_BIN})
if(BWA_STEAMAUDIO_FOUND AND WIN32)
  add_custom_command(TARGET bwa_gdextension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" ${BWA_GODOT_BIN})
endif()

# The coordinate seam is pure float math in src/bwa_room.h with no godot-cpp dependency,
# so its property test builds and runs without the editor or the generated bindings.
if(BWA_BUILD_TESTS)
  add_executable(test_godot_room test/test_room.c)
  add_test(NAME godot_room COMMAND test_godot_room)

  # The demo scenes are self-checking (they print PASS/FAIL and exit 0/1), so they run
  # under ctest too — but only when an editor binary is pointed at. Godot is not a build
  # dependency and CI has none, so this stays an explicit opt-in rather than a find_program
  # that silently skips:
  #   cmake ... -DBWA_GODOT_EXE=".../Godot_v4.7.1-stable_mono_win64_console.exe"
  set(BWA_GODOT_EXE "" CACHE FILEPATH "Godot editor binary; set it to run the demo scenes under ctest")
  if(BWA_GODOT_EXE)
    # Every scene test rides an import FIXTURE (test/godot_import.cmake): a runtime scene
    # launch loads only the GDExtensions named in .godot/extension_list.cfg, and only the
    # editor's import scan writes that file. .godot/ is gitignored, so without this a fresh
    # checkout — every CI run — has no extension, no Bwa* classes, and each scene wedges for
    # its full timeout. ctest runs the fixture automatically for any selected dependent, so
    # -R filters keep working.
    add_test(NAME godot_import
      COMMAND ${CMAKE_COMMAND} -DGODOT_EXE=${BWA_GODOT_EXE}
              -DPROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/test/godot_import.cmake)
    set_tests_properties(godot_import PROPERTIES FIXTURES_SETUP godot_project TIMEOUT 300)

    foreach(scene smoke frame)
      add_test(NAME godot_${scene}
        COMMAND "${BWA_GODOT_EXE}" --headless --path "${CMAKE_CURRENT_SOURCE_DIR}"
                "res://demo/${scene}.tscn")
      # A scene that never reaches its own exit path would otherwise run forever.
      set_tests_properties(godot_${scene} PROPERTIES TIMEOUT 120
        FIXTURES_REQUIRED godot_project)
    endforeach()

    # The API surface runs on BOTH sinks, and neither run is redundant. MANUAL has no audio
    # thread, so blocks advance only when the scene pumps them and the assertions can be
    # exact — but nothing is concurrent there: one thread on the command ring, one thread on
    # both ends of the push ring, no voice retiring under a live mixer. NULL puts the audio
    # thread back (the shipping topology minus the device) and the scene loosens its timing
    # assertions to match.
    foreach(sink manual null)
      add_test(NAME godot_api_${sink}
        COMMAND "${BWA_GODOT_EXE}" --headless --path "${CMAKE_CURRENT_SOURCE_DIR}"
                "res://demo/api.tscn" -- "--sink=${sink}")
      set_tests_properties(godot_api_${sink} PROPERTIES TIMEOUT 120
        FIXTURES_REQUIRED godot_project)
    endforeach()

    # scene.tscn checks that the collected geometry actually reached the core, which is only
    # observable as the ABSENCE of the "no room" complaint. --no-room is its negative
    # control: drop the box and that same assertion must fail to find a room, which is what
    # keeps the positive run from passing vacuously.
    add_test(NAME godot_scene
      COMMAND "${BWA_GODOT_EXE}" --headless --path "${CMAKE_CURRENT_SOURCE_DIR}"
              "res://demo/scene.tscn")
    add_test(NAME godot_scene_no_room
      COMMAND "${BWA_GODOT_EXE}" --headless --path "${CMAKE_CURRENT_SOURCE_DIR}"
              "res://demo/scene.tscn" -- "--no-room")
    set_tests_properties(godot_scene godot_scene_no_room PROPERTIES TIMEOUT 120
      FIXTURES_REQUIRED godot_project)

    # The playground's own self-test: walk all seven scenes, including both crossings of the
    # reverb boundary (which tears the engine down and stands a new one up), and render each
    # scene's HUD. It cannot judge how anything SOUNDS — that is the point of the tool, not
    # of the test — but it does catch the scene machinery and the rebuild falling over, which
    # is what actually broke while porting.
    add_test(NAME godot_playground
      COMMAND "${BWA_GODOT_EXE}" --headless --path "${CMAKE_CURRENT_SOURCE_DIR}"
              "res://playground/playground.tscn" -- "--selftest")
    set_tests_properties(godot_playground PROPERTIES TIMEOUT 180
      FIXTURES_REQUIRED godot_project)
  endif()
endif()

message(STATUS "bw_audio: Godot GDExtension ENABLED (target ${GODOTCPP_TARGET}, suffix ${GODOTCPP_SUFFIX})")
