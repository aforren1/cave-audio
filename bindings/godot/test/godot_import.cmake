# The ctest fixture behind every Godot scene test: run the editor's headless import so the
# project's .godot/ cache exists before any scene launches.
#
# This is load-bearing, not housekeeping. A RUNTIME launch (godot --path p scene.tscn) does
# not scan for GDExtensions — it loads whatever .godot/extension_list.cfg says, and only the
# EDITOR's import scan writes that file. .godot/ is gitignored, so on a fresh checkout (every
# CI run, every new clone) there is no list, no extension, no Bwa* classes: scripts fail to
# parse, the scene wedges with placeholder nodes, and the test burns its full timeout before
# failing. That is exactly the failure mode this fixture removes.
#
# Godot's --import exits NONZERO for incidental reasons (observed: exit 5 on a healthy
# project), so its exit code is deliberately ignored. What is checked is the postcondition
# the scene tests actually need: the extension list exists and names bw_audio.
#
# Invoked as:
#   cmake -DGODOT_EXE=<editor> -DPROJECT_DIR=<bindings/godot> -P godot_import.cmake

if(NOT GODOT_EXE OR NOT PROJECT_DIR)
  message(FATAL_ERROR "usage: cmake -DGODOT_EXE=... -DPROJECT_DIR=... -P godot_import.cmake")
endif()

execute_process(
  COMMAND "${GODOT_EXE}" --headless --path "${PROJECT_DIR}" --import
  TIMEOUT 240
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

set(listf "${PROJECT_DIR}/.godot/extension_list.cfg")
if(NOT EXISTS "${listf}")
  message(FATAL_ERROR "import wrote no ${listf} (godot exit: ${rc}) — GDExtensions will not "
                      "load and every scene test would hang.\nstderr:\n${err}")
endif()
file(READ "${listf}" content)
string(FIND "${content}" "bw_audio.gdextension" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "extension_list.cfg does not name bw_audio.gdextension (godot exit: "
                      "${rc}) — is addons/bw_audio/ intact?\ncontents:\n${content}")
endif()
message(STATUS "godot import ok (godot exit: ${rc}) — bw_audio extension registered")
