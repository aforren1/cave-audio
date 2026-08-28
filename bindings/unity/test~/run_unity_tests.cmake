# run_unity_tests.cmake — the ctest driver behind the unity_playmode test.
#
# It stages a throwaway Unity project, runs the editor headless with -runTests, and judges the run
# from results.xml rather than from the exit code alone. The project is a scratch tree: the package
# is referenced by `file:` path, so what compiles is the LIVE bindings/unity/, never a copy.
#
# Invoked as:
#   cmake -DUNITY_EXE=<editor> -DREPO=<repo root> -DPROJECT_DIR=<scratch project>
#         -DMIN_TESTS=<n> -P run_unity_tests.cmake

if(NOT UNITY_EXE OR NOT REPO OR NOT PROJECT_DIR)
  message(FATAL_ERROR "usage: cmake -DUNITY_EXE=... -DREPO=... -DPROJECT_DIR=... -P run_unity_tests.cmake")
endif()
if(NOT MIN_TESTS)
  set(MIN_TESTS 1)
endif()

set(pkg     "${REPO}/bindings/unity")
set(plugin  "${pkg}/Runtime/Plugins/x86_64/bw_audio.dll")

# The native engine reaches the test project through the package's Plugins folder, which the root
# CMakeLists stages on every bw_audio build (POST_BUILD copy_if_different). There is no manual step
# to forget, but there IS a build to forget, so say which one is missing.
if(NOT EXISTS "${plugin}")
  message(FATAL_ERROR "no ${plugin}\n"
                      "Build the engine first: the bw_audio target stages it there POST_BUILD.")
endif()

# ---- stage the project -----------------------------------------------------------------------------
#
# -projectPath refuses a folder holding only Packages/manifest.json, so the skeleton comes first.
file(MAKE_DIRECTORY "${PROJECT_DIR}/Assets/Tests")
file(MAKE_DIRECTORY "${PROJECT_DIR}/Packages")
file(MAKE_DIRECTORY "${PROJECT_DIR}/ProjectSettings")

# The editor version the project claims. Read it off the editor being used rather than hard-coding
# one, so pointing BWA_UNITY_EXE at a different install does not open the project as an upgrade.
get_filename_component(editor_dir "${UNITY_EXE}" DIRECTORY)
get_filename_component(editor_ver "${editor_dir}" DIRECTORY)
get_filename_component(editor_ver "${editor_ver}" NAME)
file(WRITE "${PROJECT_DIR}/ProjectSettings/ProjectVersion.txt" "m_EditorVersion: ${editor_ver}\n")

# The built-in module set is pinned in full ON PURPOSE. A manifest listing only the package resolves
# on a fresh project and then writes a packages-lock.json pinning exactly that set; every later run
# resolves strictly from the lock, drops the built-in module assemblies, and reports errors that look
# like repo defects (unresolved JsonUtility, GUIContent wanting IMGUIModule) but are harness
# artifacts. file(WRITE) emits no BOM, which matters: Unity's JSON parser rejects one outright.
file(WRITE "${PROJECT_DIR}/Packages/manifest.json"
"{
  \"dependencies\": {
    \"com.brainworks.bw_audio\": \"file:${pkg}\",
    \"com.unity.test-framework\": \"1.4.5\",
    \"com.unity.modules.ai\": \"1.0.0\",
    \"com.unity.modules.androidjni\": \"1.0.0\",
    \"com.unity.modules.animation\": \"1.0.0\",
    \"com.unity.modules.assetbundle\": \"1.0.0\",
    \"com.unity.modules.audio\": \"1.0.0\",
    \"com.unity.modules.cloth\": \"1.0.0\",
    \"com.unity.modules.director\": \"1.0.0\",
    \"com.unity.modules.imageconversion\": \"1.0.0\",
    \"com.unity.modules.imgui\": \"1.0.0\",
    \"com.unity.modules.jsonserialize\": \"1.0.0\",
    \"com.unity.modules.particlesystem\": \"1.0.0\",
    \"com.unity.modules.physics\": \"1.0.0\",
    \"com.unity.modules.physics2d\": \"1.0.0\",
    \"com.unity.modules.screencapture\": \"1.0.0\",
    \"com.unity.modules.terrain\": \"1.0.0\",
    \"com.unity.modules.terrainphysics\": \"1.0.0\",
    \"com.unity.modules.tilemap\": \"1.0.0\",
    \"com.unity.modules.ui\": \"1.0.0\",
    \"com.unity.modules.uielements\": \"1.0.0\",
    \"com.unity.modules.umbra\": \"1.0.0\",
    \"com.unity.modules.unityanalytics\": \"1.0.0\",
    \"com.unity.modules.unitywebrequest\": \"1.0.0\",
    \"com.unity.modules.unitywebrequestassetbundle\": \"1.0.0\",
    \"com.unity.modules.unitywebrequestaudio\": \"1.0.0\",
    \"com.unity.modules.unitywebrequesttexture\": \"1.0.0\",
    \"com.unity.modules.unitywebrequestwww\": \"1.0.0\",
    \"com.unity.modules.vehicles\": \"1.0.0\",
    \"com.unity.modules.video\": \"1.0.0\",
    \"com.unity.modules.vr\": \"1.0.0\",
    \"com.unity.modules.wind\": \"1.0.0\",
    \"com.unity.modules.xr\": \"1.0.0\"
  }
}
")

# The tests themselves. Copied rather than symlinked so the scratch project stays a plain tree, and
# copied EVERY run (copy_directory overwrites) so a scratch project kept from an earlier run cannot
# hold a stale copy of a test.
file(GLOB test_sources "${pkg}/test~/PlayMode/*.cs" "${pkg}/test~/PlayMode/*.asmdef")
file(COPY ${test_sources} DESTINATION "${PROJECT_DIR}/Assets/Tests")

# Unity's Tundra content-hashes its inputs, and a scratch project kept between runs can report
# "0 items updated" and reuse the cached assemblies. Deleting them forces a real compile of the
# package sources, which is the point of running this at all.
file(GLOB stale "${PROJECT_DIR}/Library/ScriptAssemblies/BwAudio*.dll")
if(stale)
  file(REMOVE ${stale})
endif()

set(results "${PROJECT_DIR}/results.xml")
set(logfile "${PROJECT_DIR}/editor.log")
file(REMOVE "${results}" "${logfile}")

# ---- run -------------------------------------------------------------------------------------------
#
# TEST_FILTER is for working ON the suite, not for ctest: it narrows the run to one test while you
# break the production code on purpose and check that the test goes red. ctest never passes it, so a
# filtered run can never be what the registered entry reports.
set(filter_arg "")
if(TEST_FILTER)
  set(filter_arg -testFilter "${TEST_FILTER}")
  message(STATUS "test filter: ${TEST_FILTER}")
endif()
message(STATUS "unity: ${UNITY_EXE}")
message(STATUS "project: ${PROJECT_DIR}")
execute_process(
  COMMAND "${UNITY_EXE}" -runTests -batchmode -nographics
          -projectPath "${PROJECT_DIR}"
          -testPlatform PlayMode
          -testResults "${results}"
          -logFile "${logfile}"
          ${filter_arg}
  TIMEOUT 1500
  RESULT_VARIABLE rc)

# ---- judge -----------------------------------------------------------------------------------------
#
# The exit code alone is not enough: a run that compiled nothing, discovered nothing, or died before
# the test runner started can still leave a tidy status behind. What is checked is the postcondition:
# a results file that reports a specific number of tests, all of them passed.
if(NOT EXISTS "${results}")
  if(EXISTS "${logfile}")
    file(READ "${logfile}" log)
    string(LENGTH "${log}" loglen)
    if(loglen GREATER 8000)
      math(EXPR tailat "${loglen} - 8000")
      string(SUBSTRING "${log}" ${tailat} -1 log)
    endif()
  else()
    set(log "(no editor log)")
  endif()
  message(FATAL_ERROR "Unity wrote no ${results} (exit: ${rc}).\nlog tail:\n${log}")
endif()

file(READ "${results}" xml)
string(REGEX MATCH "<test-run[^>]*>" run "${xml}")
if(NOT run)
  message(FATAL_ERROR "results.xml has no <test-run> element (exit: ${rc})")
endif()

foreach(attr total passed failed inconclusive skipped)
  set(${attr} "")
  string(REGEX MATCH " ${attr}=\"([0-9]+)\"" m "${run}")
  if(m)
    set(${attr} "${CMAKE_MATCH_1}")
  endif()
endforeach()
string(REGEX MATCH " result=\"([^\"]+)\"" m "${run}")
set(result "${CMAKE_MATCH_1}")

message(STATUS "unity playmode: result=${result} total=${total} passed=${passed} failed=${failed} "
               "inconclusive=${inconclusive} skipped=${skipped}")

# Name every failing case, then quote every assertion message in the file, so the ctest output says
# what broke without opening the xml. The two lists are gathered separately rather than paired:
# CMake's regex has no non-greedy repeat, so there is no way to carve one test-case element out of
# the document. A passing case carries no <message>, so what this prints is the failures.
if(NOT failed STREQUAL "0" OR NOT result STREQUAL "Passed")
  string(REGEX MATCHALL "<test-case [^>]*result=\"Failed\"[^>]*>" bad "${xml}")
  set(names "")
  foreach(b ${bad})
    string(REGEX MATCH "fullname=\"([^\"]*)\"" n "${b}")
    string(APPEND names "  - ${CMAKE_MATCH_1}\n")
  endforeach()
  string(REPLACE "<![CDATA[" "" flat "${xml}")
  string(REPLACE "]]>" "" flat "${flat}")
  string(REGEX MATCHALL "<message>[^<]*" msgs "${flat}")
  foreach(m ${msgs})
    string(REPLACE "<message>" "" m "${m}")
    string(STRIP "${m}" m)
    # The run- and suite-level rollups say only that a child failed, which the list above already did.
    if(NOT m STREQUAL "One or more child tests had errors")
      string(REPLACE "\n" "\n      " m "${m}")
      string(APPEND names "      ${m}\n")
    endif()
  endforeach()
  message(FATAL_ERROR "unity playmode FAILED (exit ${rc}, result ${result}, ${failed} failed):\n"
                      "${names}see ${results} and ${logfile}")
endif()

# A run that discovered nothing is the failure mode this whole entry exists to avoid: it would
# otherwise report success forever while testing nothing. MIN_TESTS comes from the CMakeLists and
# has to be raised whenever a test is added.
if(total LESS MIN_TESTS)
  message(FATAL_ERROR "unity playmode discovered only ${total} tests, expected at least ${MIN_TESTS} "
                      "- the suite did not run. See ${logfile}")
endif()
if(NOT skipped STREQUAL "0" OR NOT inconclusive STREQUAL "0")
  message(FATAL_ERROR "unity playmode: ${skipped} skipped, ${inconclusive} inconclusive - a test that "
                      "did not run must not read as a pass. See ${results}")
endif()
if(NOT rc STREQUAL "0")
  message(FATAL_ERROR "unity exited ${rc} despite a clean results.xml - treat the run as failed")
endif()

message(STATUS "unity playmode ok: ${passed}/${total} passed")
