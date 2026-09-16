# MATLAB and Octave binding (a classic C MEX gateway) - included from the repo root by
# -DBWA_BUILD_MATLAB=ON.
#
# WHY IT IS INCLUDED RATHER THAN A PROJECT OF ITS OWN. The same argument the Python binding makes:
# the root CMakeLists reads ${CMAKE_SOURCE_DIR} to find the ASIO SDK and the staged phonon, so a
# top-level project here would miss both and quietly build a no-SDK engine for the MEX to link.
#
# WHAT IT BUILDS. One source, two binaries, because MATLAB and Octave need different ones: a
# different MEX extension (.mexw64/.mexa64 against .mex), a different compiler, a different set of
# MEX libraries. Either half builds on its own, and both build in one configure when both
# toolchains are installed. Each lands in bin/<matlab|octave>/<platform>/ with the engine library
# beside it, and +bwa/setup.m picks the right one for the interpreter it is running in.

set(BWA_MATLAB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# The engine as a BUILD DEPENDENCY, which only exists in-tree: under -DBWA_ENGINE_SDK the engine is
# an imported target and there is nothing here to rebuild when it changes. The MATLAB MEX gets this
# for free through target_link_libraries; the Octave one is a custom command and has to say it.
if(TARGET bw_audio)
  set(BWA_ENGINE_DEP bw_audio)
else()
  set(BWA_ENGINE_DEP "")
endif()

# The same platform string +bwa/platform_dir.m computes. It is deliberately not computer('arch'):
# that answers 'win64' in MATLAB and 'mingw32-x86_64' in Octave for one machine.
if(WIN32)
  set(BWA_MATLAB_PLATFORM "win-x64")
elseif(APPLE)
  set(BWA_MATLAB_PLATFORM "macos-universal")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(BWA_MATLAB_PLATFORM "linux-arm64")
else()
  set(BWA_MATLAB_PLATFORM "linux-x64")
endif()

set(BWA_MATLAB_ANY OFF)

# ---- MATLAB --------------------------------------------------------------------------------
# MAIN_PROGRAM is beyond what the MEX itself needs: it is how ctest runs the suite. A MATLAB with
# no launcher still builds the MEX, it just registers no tests.
find_package(Matlab COMPONENTS MX_LIBRARY MEX_COMPILER MAIN_PROGRAM QUIET)

if(Matlab_FOUND AND Matlab_MEX_COMPILER)
  set(BWA_MATLAB_OUT "${BWA_MATLAB_DIR}/bin/matlab/${BWA_MATLAB_PLATFORM}")
  # R2017b pins the CLASSIC (separate-complex) API. It is what Octave implements, so the one
  # source compiles in both places; -R2018a would give this file mxGetDoubles semantics Octave has
  # no twin for. Nothing here touches complex data, so the pin costs nothing and buys portability.
  matlab_add_mex(NAME bwa_mex_matlab SRC "${BWA_MATLAB_DIR}/src/bwa_mex.c"
                 OUTPUT_NAME bwa_mex LINK_TO bwa::bw_audio R2017b)
  target_include_directories(bwa_mex_matlab PRIVATE "${CMAKE_SOURCE_DIR}/include")
  if(UNIX AND NOT APPLE)
    set_target_properties(bwa_mex_matlab PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  elseif(APPLE)
    # HOST arch only, whatever CMAKE_OSX_ARCHITECTURES says. CI builds the engine UNIVERSAL
    # (x86_64;arm64) and inherited that here, but MATLAB ships libmx/libmex for the ONE
    # architecture it runs as, so the x86_64 half of a universal link finds no _mexAtExit and
    # the link fails. A MEX only ever runs inside this machine's MATLAB, so its arch is the
    # host's; the universal engine dylib beside it loads in either.
    set_target_properties(bwa_mex_matlab PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path"
                                                    OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    # Drop MATLAB's C++ MEX export map. FindMatlab hands the linker BOTH maps on macOS whenever
    # the install has the C++ API (R2018a and later), and cppMexFunction.map names the three C++
    # entry points (mexCreateMexFunction, mexDestroyMexFunction, mexFunctionAdapter) that a C
    # MEX never defines. FindMatlab papers over that with -Wl,-U for the three, which the
    # classic ld64 honored; the Xcode 15+ linker treats every name in an -exported_symbols_list
    # as a required definition and fails the link with "referenced from <initial-undefines>".
    # The C map (c_exportsmexfileversion.map: mexFunction + mexfilerequiredapiversion) stays,
    # and it is all this gateway exports. Editing the flag string is the only handle: FindMatlab
    # offers no option to choose one map, and the property is a single string, not a list.
    get_target_property(_bwa_mex_lf bwa_mex_matlab LINK_FLAGS)
    if(_bwa_mex_lf)
      string(REGEX REPLACE " -Wl,-exported_symbols_list,[^ ]*cppMexFunction\\.map" "" _bwa_mex_lf "${_bwa_mex_lf}")
      string(REGEX REPLACE " -Wl,-U,_mex(Create|Destroy)MexFunction| -Wl,-U,_mexFunctionAdapter" "" _bwa_mex_lf "${_bwa_mex_lf}")
      set_target_properties(bwa_mex_matlab PROPERTIES LINK_FLAGS "${_bwa_mex_lf}")
    endif()
  endif()
  add_custom_command(TARGET bwa_mex_matlab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BWA_MATLAB_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bwa_mex_matlab> "${BWA_MATLAB_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bwa::bw_audio>   "${BWA_MATLAB_OUT}"
    COMMENT "Staging the MATLAB MEX at ${BWA_MATLAB_OUT}")
  set(BWA_MATLAB_ANY ON)
  message(STATUS "bw_audio: MATLAB MEX enabled (${Matlab_ROOT_DIR})")
else()
  message(STATUS "bw_audio: MATLAB not found - skipping the MATLAB MEX (the Octave one is separate)")
endif()

# ---- Octave --------------------------------------------------------------------------------
# mkoctfile is a driver script, not a CMake-visible compiler, so this is a custom command rather
# than a target. It is still a dependency-tracked build: touching the source or rebuilding the
# engine re-runs it.
# The Windows installer does not put Octave on PATH, so look where it puts itself. Everything
# else (Linux, macOS, and a Windows user who did add it) is found by the plain search.
set(BWA_OCTAVE_HINTS "")
if(WIN32)
  # OCTAVE_HOME arrives from the environment, so on Windows it usually carries BACKSLASHES, which a
  # glob pattern reads as escape characters rather than as separators. CMake 3.29 happens to match
  # the mixed-separator string anyway; do not lean on that. Normalize, and the hint is then a plain
  # CMake path like the two literals below. Those two cover the vendor installer's own locations; a
  # Chocolatey octave.portable install (what CI uses) lands under its tools/ directory instead and
  # is reached through OCTAVE_HOME.
  file(TO_CMAKE_PATH "$ENV{OCTAVE_HOME}" BWA_OCTAVE_HOME)
  file(GLOB BWA_OCTAVE_HINTS "${BWA_OCTAVE_HOME}/mingw64/bin"
                             "C:/Program Files/GNU Octave/Octave-*/mingw64/bin"
                             "C:/Octave/Octave-*/mingw64/bin")
endif()
find_program(BWA_MKOCTFILE NAMES mkoctfile HINTS ${BWA_OCTAVE_HINTS})
find_program(BWA_OCTAVE_CLI NAMES octave-cli octave HINTS ${BWA_OCTAVE_HINTS})

if(BWA_MKOCTFILE)
  set(BWA_OCTAVE_OUT "${BWA_MATLAB_DIR}/bin/octave/${BWA_MATLAB_PLATFORM}")
  set(BWA_OCTAVE_MEX "${BWA_OCTAVE_OUT}/bwa_mex.mex")
  set(BWA_OCTAVE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/octave_mex.stamp")
  add_custom_command(OUTPUT "${BWA_OCTAVE_STAMP}"
    COMMAND ${CMAKE_COMMAND}
            -DBWA_MKOCTFILE=${BWA_MKOCTFILE}
            -DBWA_SRC=${BWA_MATLAB_DIR}/src/bwa_mex.c
            -DBWA_INCLUDE=${CMAKE_SOURCE_DIR}/include
            -DBWA_ENGINE_LIB=$<TARGET_LINKER_FILE:bwa::bw_audio>
            # The RUNTIME file as well as the link file, because the two are not in one directory
            # in every mode: an SDK install puts the Windows import library under lib/ and the DLL
            # under bin/, where the build tree had them side by side. Off Windows they are the
            # same file and the driver script ignores this one.
            -DBWA_ENGINE_DLL=$<TARGET_FILE:bwa::bw_audio>
            -DBWA_OUT=${BWA_OCTAVE_MEX}
            -DBWA_STAMP=${BWA_OCTAVE_STAMP}
            -P "${BWA_MATLAB_DIR}/cmake/mkoctfile_mex.cmake"
    # The driver script is a dependency too: it carries the link flags, so changing one has to
    # relink. Nothing else here would notice.
    DEPENDS "${BWA_MATLAB_DIR}/src/bwa_mex.c" "${BWA_MATLAB_DIR}/cmake/mkoctfile_mex.cmake"
            ${BWA_ENGINE_DEP}
    COMMENT "Building the Octave MEX at ${BWA_OCTAVE_MEX}"
    VERBATIM)
  add_custom_target(bwa_mex_octave ALL DEPENDS "${BWA_OCTAVE_STAMP}")
  set(BWA_MATLAB_ANY ON)
  message(STATUS "bw_audio: Octave MEX enabled (${BWA_MKOCTFILE})")
else()
  message(STATUS "bw_audio: mkoctfile not found - skipping the Octave MEX")
endif()

if(NOT BWA_MATLAB_ANY)
  message(WARNING
    "bw_audio: BWA_BUILD_MATLAB=ON but neither MATLAB nor mkoctfile was found. Nothing to build. "
    "Install one, or point CMake at MATLAB with -DMatlab_ROOT_DIR=<matlabroot>.")
endif()

# ---- tests ---------------------------------------------------------------------------------
# Registered only where the interpreter that would run them exists: ctest cannot drive a MATLAB
# test with no MATLAB, so there is nothing to skip. What SKIP_RETURN_CODE is for is the case that
# survives configure and fails at RUN time - `run_tests` returns 77 when the MEX for the running
# interpreter was never staged, and a MATLAB that cannot check out a license can do the same.
# The examples do not skip: they are only registered when the MEX target exists, so a missing
# gateway there is a real failure and should read as one.
if(BWA_BUILD_TESTS)
  set(BWA_M_TESTS_DIR "${BWA_MATLAB_DIR}/tests")
  set(BWA_M_EXAMPLES_DIR "${BWA_MATLAB_DIR}/examples")

  if(Matlab_FOUND AND Matlab_MAIN_PROGRAM AND TARGET bwa_mex_matlab)
    add_test(NAME matlab_tests
             COMMAND "${Matlab_MAIN_PROGRAM}" -batch "exit(run_tests())")
    set_tests_properties(matlab_tests PROPERTIES
      WORKING_DIRECTORY "${BWA_M_TESTS_DIR}" TIMEOUT 900 SKIP_RETURN_CODE 77)
    foreach(ex offline_render live_onset AudioTunnel3DDemo_bwa)
      add_test(NAME matlab_example_${ex}
               COMMAND "${Matlab_MAIN_PROGRAM}" -batch "exit(${ex}('--tests'))")
      set_tests_properties(matlab_example_${ex} PROPERTIES
        WORKING_DIRECTORY "${BWA_M_EXAMPLES_DIR}" TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
  endif()

  if(BWA_OCTAVE_CLI AND BWA_MKOCTFILE)
    add_test(NAME octave_tests
             COMMAND "${BWA_OCTAVE_CLI}" --no-gui --quiet --eval "exit(run_tests())")
    set_tests_properties(octave_tests PROPERTIES
      WORKING_DIRECTORY "${BWA_M_TESTS_DIR}" TIMEOUT 900 SKIP_RETURN_CODE 77)
    foreach(ex offline_render live_onset AudioTunnel3DDemo_bwa)
      add_test(NAME octave_example_${ex}
               COMMAND "${BWA_OCTAVE_CLI}" --no-gui --quiet --eval "exit(${ex}('--tests'))")
      set_tests_properties(octave_example_${ex} PROPERTIES
        WORKING_DIRECTORY "${BWA_M_EXAMPLES_DIR}" TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
  endif()
endif()
