# The four optional BINDING subdirectories, in ONE place because the root CMakeLists has two
# modes and both have to include exactly the same set:
#
#   in-tree      - the engine is compiled here and `bwa::bw_audio` aliases it.
#   BWA_ENGINE_SDK - nothing in src/ is compiled; `bwa::bw_audio` is imported from a prebuilt SDK.
#
# A binding sees no difference: it links `bwa::bw_audio` and reads the engine file out of
# `$<TARGET_FILE:bwa::bw_audio>`. Splitting the two include sites would let one mode gain a binding
# the other does not have, which is exactly the drift this file prevents.
#
# A MACRO, not a function: option() and add_subdirectory both want the caller's directory scope.

macro(bwa_add_bindings)
  # ---- Optional: Python binding (nanobind) ----
  # Opt-in, because it needs a Python 3.9+ interpreter with nanobind importable and the default
  # build has no Python dependency at all:
  #   cmake -S . -B build -DBWA_BUILD_PYTHON=ON
  # It registers three more ctests (the pytest suite plus the two examples in --tests mode). This
  # is ALSO the path a wheel build takes: bindings/python/pyproject.toml points scikit-build-core
  # at the ROOT CMakeLists with the option on, rather than making bindings/python its own project,
  # because the ASIO and phonon lookups there resolve against ${CMAKE_SOURCE_DIR} and would miss
  # from anywhere else. See bindings/python/CMakeLists.txt and bindings/python/README.md.
  option(BWA_BUILD_PYTHON "Build the Python binding (nanobind; needs Python 3.9+ with nanobind)" OFF)
  if(BWA_BUILD_PYTHON)
    enable_language(CXX)
    add_subdirectory(bindings/python)
  endif()

  # ---- Optional: MATLAB and Octave binding (a classic C MEX gateway) ----
  # Opt-in, because it needs MATLAB or Octave installed and the default build needs neither:
  #   cmake -S . -B build -DBWA_BUILD_MATLAB=ON
  # It builds whichever of the two it finds, and both when both are there, from ONE source: the
  # classic C MEX API is what Octave implements, so the MATLAB-only C++ Data API is off the table.
  # Each half registers its own ctests (the suite plus the three examples in --tests mode).
  # See bindings/matlab/CMakeLists.txt and bindings/matlab/README.md.
  option(BWA_BUILD_MATLAB "Build the MATLAB and Octave binding (a MEX gateway; needs MATLAB or mkoctfile)" OFF)
  if(BWA_BUILD_MATLAB)
    add_subdirectory(bindings/matlab)
  endif()

  # ---- Optional: web binding (an ES module for a browser page) ----
  # Opt-in and Emscripten-only: it is an emcc link with its own JS glue, and there is nothing for
  # it to produce on a desktop toolchain. tools/wasm/build-web.sh is the one caller and turns
  # BWA_WITH_WORKLET on beside it, because a page with no AudioWorklet sink makes no sound.
  # See bindings/web/CMakeLists.txt and bindings/web/README.md.
  option(BWA_BUILD_WEB "Build the web binding (Emscripten only; an ES module + the worklet sink)" OFF)
  if(BWA_BUILD_WEB AND EMSCRIPTEN)
    add_subdirectory(bindings/web)
  elseif(BWA_BUILD_WEB)
    message(STATUS "bw_audio: web binding skipped (Emscripten only; run tools/wasm/build-web.sh)")
    set(BWA_BUILD_WEB OFF)
  endif()

  # ---- Optional: Godot GDExtension binding ----
  # Opt-in (it fetches godot-cpp, whose generated bindings are a multi-minute first build):
  # cmake -S . -B build -DBWA_BUILD_GODOT=ON. See bindings/godot/CMakeLists.txt.
  option(BWA_BUILD_GODOT "Build the Godot GDExtension binding (fetches godot-cpp)" OFF)
  if(BWA_BUILD_GODOT)
    enable_language(CXX)
    # godot-cpp puts CMAKE_MSVC_RUNTIME_LIBRARY in the CACHE on purpose (its cmake/windows.cmake
    # documents that it does so "for consumer target definitions") and picks the STATIC CRT. A
    # cache entry outlives the subdirectory, so without this dance every target declared BELOW
    # would be created with /MT while bwa_core above was built /MD, and linking the two together
    # fails with unresolved __imp_* CRT symbols (bwa_calib_view, bwa_calibrate and bwa_validate all
    # hit it). Save it here and put it back immediately AFTER the subdirectory, not around the
    # fetch inside bindings/godot/CMakeLists.txt: bwa_gdextension is declared after that fetch and
    # has to keep godot-cpp's choice, or the shipped GDExtension mismatches the static godot-cpp it
    # links.
    #
    # A build tree whose cache still holds godot-cpp's /MT entry from BEFORE this dance existed
    # hits the mismatch at the top of the file instead, where nothing here can save it: every
    # target is created /MT, and the static phonon (built /MD, third_party/README.md's CRT note)
    # then fails with one LNK2038 per archive member. A static archive CARRIES its CRT choice,
    # where the old import library did not, so what used to link silently is now a wall of errors.
    # The fix is to drop the stale entry: cmake -U CMAKE_MSVC_RUNTIME_LIBRARY -S . -B <tree>, or
    # reconfigure clean.
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
      set(BWA_MSVC_RUNTIME_SAVED "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    else()
      unset(BWA_MSVC_RUNTIME_SAVED)
    endif()

    add_subdirectory(bindings/godot)

    if(DEFINED BWA_MSVC_RUNTIME_SAVED)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "${BWA_MSVC_RUNTIME_SAVED}" CACHE STRING
          "Select the MSVC runtime library for use by compilers targeting the MSVC ABI." FORCE)
      unset(BWA_MSVC_RUNTIME_SAVED)
    else()
      # It was undefined before, so leave it undefined: an empty cache string is NOT the same thing
      # (CMake's own default under CMP0091 is the /MD selection, which is what the rest of the tree
      # is compiled with).
      unset(CMAKE_MSVC_RUNTIME_LIBRARY CACHE)
    endif()
  endif()
endmacro()
