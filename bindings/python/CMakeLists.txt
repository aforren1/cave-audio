# Python binding (nanobind) - included from the repo root by -DBWA_BUILD_PYTHON=ON.
#
# WHY IT IS INCLUDED RATHER THAN A PROJECT OF ITS OWN. A wheel build and a repo-root configure have
# to produce the same extension, and the root CMakeLists reads ${CMAKE_SOURCE_DIR} to find the ASIO
# SDK and the staged phonon. If this directory were the top-level project and pulled the root in
# with add_subdirectory, that variable would point HERE, both lookups would miss, and a wheel would
# silently ship a no-SDK, no-ASIO engine. So there is exactly one arrangement: the repo root is
# always the CMake source dir, and pyproject.toml points at it (`cmake.source-dir = "../.."`) with
# this option on. `uv build --wheel` from bindings/python and `cmake -S . -B build
# -DBWA_BUILD_PYTHON=ON` at the root run the same code.
#
# WHAT SHIPS. The extension `_bwa` plus the engine library itself, side by side inside the package
# directory, so a wheel is self-contained. phonon is linked statically (2026-09-15), so there is no
# third file. The loader finds the engine through an RPATH of $ORIGIN / @loader_path on Linux and
# macOS, and through os.add_dll_directory in __init__.py on Windows, which does not search a .pyd's
# own directory for its dependents.

# Interpreter first: the stable-ABI decision below needs its version, and the component set differs
# either side of it.
# 3.10 is the floor: the pinned nanobind (pyproject.toml, build-system.requires) declares
# `Requires-Python: >=3.10`, so an older interpreter cannot get as far as this file in a wheel
# build and would fail at find_package(nanobind) in a repo-root configure.
find_package(Python 3.10 COMPONENTS Interpreter REQUIRED)

if(Python_VERSION VERSION_GREATER_EQUAL 3.12)
  # Development.SABIModule is what links against python3.dll / libpython3.so rather than a
  # version-specific one. nanobind's STABLE_ABI then targets the 3.12 limited API, so ONE wheel per
  # platform (cp312-abi3) runs on 3.12 and every later Python.
  find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)
  set(BWA_PY_STABLE_ABI STABLE_ABI)
  message(STATUS "bw_audio: Python binding on the STABLE ABI (abi3, cp312+) with Python ${Python_VERSION}")
else()
  # 3.10 and 3.11 take this path, and a release builds a wheel for each of them: the limited API
  # nanobind's STABLE_ABI targets does not exist before 3.12, so there is no one file that covers
  # them. This is not a fallback to apologize for, it is the other half of the shipped matrix.
  find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
  set(BWA_PY_STABLE_ABI "")
  message(STATUS "bw_audio: Python binding is VERSION-SPECIFIC - Python ${Python_VERSION} predates "
                 "the 3.12 limited API nanobind's STABLE_ABI targets. The wheel will only run on "
                 "this Python. Build with 3.12 or later for the abi3 wheel that covers the rest.")
endif()

# nanobind ships its CMake config inside the package, so the interpreter that will import the
# extension is also what locates it. In a wheel build that interpreter is the isolated build
# environment's, where `nanobind` came from build-system.requires.
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE BWA_NANOBIND_DIR
  RESULT_VARIABLE BWA_NANOBIND_RC ERROR_VARIABLE BWA_NANOBIND_ERR)
if(NOT BWA_NANOBIND_RC EQUAL 0)
  message(FATAL_ERROR
    "bw_audio: BWA_BUILD_PYTHON=ON but ${Python_EXECUTABLE} cannot import nanobind "
    "(${BWA_NANOBIND_ERR}). Install it into the environment you are configuring against: "
    "`uv pip install nanobind`. A wheel build gets it from build-system.requires and never "
    "reaches this message.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${BWA_NANOBIND_DIR}")
find_package(nanobind CONFIG REQUIRED)

# NB_STATIC: nanobind's own runtime is linked into this one extension. There is only one extension
# here, so a shared libnanobind would be a second file to ship for no sharing.
nanobind_add_module(_bwa ${BWA_PY_STABLE_ABI} NB_STATIC src/bwa_module.cpp)
target_link_libraries(_bwa PRIVATE bwa::bw_audio)
target_include_directories(_bwa PRIVATE "${CMAKE_SOURCE_DIR}/include")

if(APPLE)
  set_target_properties(_bwa PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(_bwa PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# ---- what the wheel gets ----
# install(TARGETS) for the extension so CMake rewrites its RPATH at install time; install(FILES) for
# the engine, because TARGETS would also want a destination for the Windows import library and a
# wheel has no use for one.
#
# COMPONENT bwa_python on every rule, and pyproject.toml's `install.components` names it. The root
# CMakeLists also carries the engine SDK's install rules (component bwa_sdk), and scikit-build-core
# installs EVERY component unless told otherwise - so without this a wheel would carry a second
# copy of the engine at bin/ plus the header and the CMake package files.
install(TARGETS _bwa COMPONENT bwa_python LIBRARY DESTINATION bw_audio RUNTIME DESTINATION bw_audio)
install(FILES $<TARGET_FILE:bwa::bw_audio> DESTINATION bw_audio COMPONENT bwa_python)

# STRIP the installed engine library on the ELF and Mach-O platforms. It is not tidiness: a static
# phonon is built -g, so its DWARF is about 25 MB against the 7 MB of code, and a wheel carries the
# library whole where the CI dist trees split the symbols into a sidecar file. Measured on Linux,
# 30,369,016 bytes down to 7,493,568. Windows needs nothing here - its debug info was never in the
# DLL, it is in the .pdb the wheel does not carry. A developer who wants symbols builds from the
# checkout, where nothing is stripped.
if(NOT WIN32)
  if(APPLE)
    set(BWA_PY_STRIP_ARGS -S -x)   # -x on a dylib: local symbols only, the export table survives
  else()
    set(BWA_PY_STRIP_ARGS --strip-debug)
  endif()
  install(CODE "execute_process(COMMAND \"${CMAKE_STRIP}\" ${BWA_PY_STRIP_ARGS}
                  \"\${CMAKE_INSTALL_PREFIX}/bw_audio/$<TARGET_FILE_NAME:bwa::bw_audio>\")"
          COMPONENT bwa_python)
endif()

# GPLv3 travels with the binary, the same rule the Unity and Godot packs follow: the pack
# copies the repo's own LICENSE rather than keeping a second copy under the binding.
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY-NOTICES.md"
        DESTINATION bw_audio COMPONENT bwa_python)

# ---- the build tree's own importable copy ----
# ctest and a developer both want `import bw_audio` without building a wheel first, and the layout
# has to be the wheel's (the extension and the engine library inside the package directory) or the
# Windows DLL-directory rule would go untested until packaging. Multi-config generators put the
# binaries under a per-config subdirectory, so the stage is the one path that does not move.
set(BWA_PY_STAGE "${CMAKE_BINARY_DIR}/python-package/bw_audio")
add_custom_command(TARGET _bwa POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BWA_PY_STAGE}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:_bwa>      "${BWA_PY_STAGE}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bwa::bw_audio> "${BWA_PY_STAGE}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/src/bw_audio/__init__.py"             "${BWA_PY_STAGE}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/src/bw_audio/py.typed"                "${BWA_PY_STAGE}"
  COMMENT "Staging the importable bw_audio package at ${BWA_PY_STAGE}")

if(BWA_BUILD_TESTS)
  # pytest against the STAGED package, never the source tree: the source tree has no extension in
  # it, so an import there would find the directory and fail on `_bwa`.
  # run_ctest.py is the driver rather than `-m pytest` directly, for two reasons it states itself:
  # an environment with no pytest is a SKIP (77), and a run that collected NOTHING is a FAILURE
  # (pytest's own exit code 5 for that is not an error, and a suite that cannot fail is the default
  # outcome, not a rare mistake - CLAUDE.md, Traps).
  add_test(NAME python_bindings
           COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ctest.py")
  set_tests_properties(python_bindings PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python-package"
    TIMEOUT 300
    SKIP_RETURN_CODE 77)

  # The examples, in --tests mode, like the console examples the C build registers.
  foreach(ex minimal offline_render live_onset)
    add_test(NAME python_example_${ex}
             COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/${ex}.py" --tests)
    set_tests_properties(python_example_${ex} PROPERTIES
      ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python-package"
      TIMEOUT 120)
  endforeach()
endif()
