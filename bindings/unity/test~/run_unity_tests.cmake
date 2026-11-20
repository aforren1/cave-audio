# run_unity_tests.cmake — the ctest driver behind the unity_playmode test.
#
# It stages a throwaway Unity project, runs the editor headless with -runTests, and judges the run
# from results.xml rather than from the exit code alone. The project is a scratch tree, and the
# package it references by `file:` path is a COPY of bindings/unity/ made fresh on every run.
#
# Two passes, each its own editor launch, because -testPlatform takes one value: EditMode (the
# editor tooling, test~/EditMode) and then PlayMode (the components against the native engine,
# test~/PlayMode). EditMode goes first because it is the quick one.
#
# Invoked as:
#   cmake -DUNITY_EXE=<editor> -DREPO=<repo root> -DPROJECT_DIR=<scratch project>
#         -DMIN_TESTS=<n> -P run_unity_tests.cmake
#
# MIN_TESTS is the PlayMode floor and comes from the root CMakeLists. The EditMode floor is
# MIN_EDIT_TESTS below: raise it whenever an EditMode test is added.

if(NOT UNITY_EXE OR NOT REPO OR NOT PROJECT_DIR)
  message(FATAL_ERROR "usage: cmake -DUNITY_EXE=... -DREPO=... -DPROJECT_DIR=... -P run_unity_tests.cmake")
endif()
if(NOT MIN_TESTS)
  set(MIN_TESTS 1)
endif()
if(NOT DEFINED MIN_EDIT_TESTS)        # DEFINED, so an explicit 0 is honored
  set(MIN_EDIT_TESTS 14)
endif()

# Forward slashes only: PROJECT_DIR goes into manifest.json as a `file:` path, and a backslash from
# %LOCALAPPDATA% is a bad JSON escape there. Unity then resolves no packages and exits 1.
file(TO_CMAKE_PATH "${PROJECT_DIR}" PROJECT_DIR)
file(TO_CMAKE_PATH "${REPO}" REPO)

set(pkg_live "${REPO}/bindings/unity")
set(pkg      "${PROJECT_DIR}/pkg/com.brainworks.bw_audio")
set(plugin   "${pkg_live}/Runtime/Plugins/x86_64/bw_audio.dll")

# The native engine reaches the test project through the package's Plugins folder, which the root
# CMakeLists stages on every bw_audio build (POST_BUILD copy_if_different). There is no manual step
# to forget, but there IS a build to forget, so say which one is missing.
if(NOT EXISTS "${plugin}")
  message(FATAL_ERROR "no ${plugin}\n"
                      "Build the engine first: the bw_audio target stages it there POST_BUILD.")
endif()

# ---- stage the project -----------------------------------------------------------------------------
#
# The package is COPIED, never referenced in place. A `file:` package is mutable to Unity, and the
# editor deletes any .meta whose asset is missing. The native libraries are build output, so on a
# machine that built one platform the other three .meta files are exactly that, and a run against
# the repo folder deleted Runtime/Plugins/macOS/libbw_audio.dylib.meta out of the working tree. The
# copy is remade every run (REMOVE_RECURSE first), so what compiles is still this run's sources.
file(REMOVE_RECURSE "${pkg}")
file(COPY "${pkg_live}/" DESTINATION "${pkg}"
     PATTERN "test~" EXCLUDE
     PATTERN "Samples~" EXCLUDE)

# -projectPath refuses a folder holding only Packages/manifest.json, so the skeleton comes first.
file(MAKE_DIRECTORY "${PROJECT_DIR}/Assets/Tests")
# What the tests write at run time. A run that crashed leaves it behind, and the EditMode suite
# refuses to start over a leftover trims.json. The PlayMode fixtures are rewritten every run anyway.
file(REMOVE_RECURSE "${PROJECT_DIR}/Assets/StreamingAssets" "${PROJECT_DIR}/Assets/StreamingAssets.meta"
                    "${PROJECT_DIR}/Assets/RigTestScenes"   "${PROJECT_DIR}/Assets/RigTestScenes.meta")
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
# into folders emptied first, so a scratch project kept from an earlier run can hold neither a stale
# copy of a test nor a test that has since been deleted (which would still run, and count).
file(REMOVE_RECURSE "${PROJECT_DIR}/Assets/Tests" "${PROJECT_DIR}/Assets/EditModeTests")
file(MAKE_DIRECTORY "${PROJECT_DIR}/Assets/Tests")
file(GLOB test_sources "${pkg_live}/test~/PlayMode/*.cs" "${pkg_live}/test~/PlayMode/*.asmdef")
file(COPY ${test_sources} DESTINATION "${PROJECT_DIR}/Assets/Tests")
# The EditMode assembly gets its own folder: an .asmdef governs its folder, and the two assemblies
# target different platforms.
file(GLOB edit_sources "${pkg_live}/test~/EditMode/*.cs" "${pkg_live}/test~/EditMode/*.asmdef")
file(COPY ${edit_sources} DESTINATION "${PROJECT_DIR}/Assets/EditModeTests")

# Unity's Tundra content-hashes its inputs, and a scratch project kept between runs can report
# "0 items updated" and reuse the cached assemblies. Deleting them forces a real compile of the
# package sources, which is the point of running this at all.
file(GLOB stale "${PROJECT_DIR}/Library/ScriptAssemblies/BwAudio*.dll")
if(stale)
  file(REMOVE ${stale})
endif()

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

# One pass: run `platform`, then judge it against the floor `min_tests`. A function, so the two
# passes cannot drift apart in what they accept. TEST_PLATFORM (EditMode or PlayMode) narrows a
# working run to one pass, the same way TEST_FILTER narrows it to one test.
#
# The two timeouts must sum to less than the ctest entry's TIMEOUT (1800 in the root CMakeLists),
# or ctest kills the run before the driver can say which pass hung.
function(run_pass platform min_tests timeout_s)
if(TEST_PLATFORM AND NOT TEST_PLATFORM STREQUAL platform)
  return()
endif()
set(MIN_TESTS ${min_tests})
set(results "${PROJECT_DIR}/results-${platform}.xml")
set(logfile "${PROJECT_DIR}/editor-${platform}.log")
file(REMOVE "${results}" "${logfile}")
execute_process(
  COMMAND "${UNITY_EXE}" -runTests -batchmode -nographics
          -projectPath "${PROJECT_DIR}"
          -testPlatform ${platform}
          -testResults "${results}"
          -logFile "${logfile}"
          ${filter_arg}
  TIMEOUT ${timeout_s}
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

message(STATUS "unity ${platform}: result=${result} total=${total} passed=${passed} failed=${failed} "
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
  message(FATAL_ERROR "unity ${platform} FAILED (exit ${rc}, result ${result}, ${failed} failed):\n"
                      "${names}see ${results} and ${logfile}")
endif()

# A run that discovered nothing is the failure mode this whole entry exists to avoid: it would
# otherwise report success forever while testing nothing. MIN_TESTS comes from the CMakeLists and
# has to be raised whenever a test is added.
# A filtered run is a working run and finds fewer on purpose, but it must still find something.
if(TEST_FILTER)
  set(MIN_TESTS 1)
endif()
if(total LESS MIN_TESTS)
  message(FATAL_ERROR "unity ${platform} discovered only ${total} tests, expected at least ${MIN_TESTS} "
                      "- the suite did not run. See ${logfile}")
endif()
if(NOT skipped STREQUAL "0" OR NOT inconclusive STREQUAL "0")
  message(FATAL_ERROR "unity ${platform}: ${skipped} skipped, ${inconclusive} inconclusive - a test that "
                      "did not run must not read as a pass. See ${results}")
endif()
if(NOT rc STREQUAL "0")
  message(FATAL_ERROR "unity exited ${rc} despite a clean results.xml - treat the run as failed")
endif()

message(STATUS "unity ${platform} ok: ${passed}/${total} passed")
endfunction()

run_pass(EditMode ${MIN_EDIT_TESTS} 600)
run_pass(PlayMode ${MIN_TESTS} 1100)
