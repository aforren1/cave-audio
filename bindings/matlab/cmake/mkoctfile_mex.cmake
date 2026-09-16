# Driver for the Octave MEX build, run as `cmake -P` from bindings/matlab/CMakeLists.txt.
#
# It exists so the link line survives the GENERATOR. The Linux rpath is literally $ORIGIN, and a
# Makefile generator would read $O as a make variable before anything else saw it. Inside a -P
# script the string is data. (mkoctfile then runs its own inner shell, which is a separate
# problem with its own fix below.)
#
# Expects: BWA_MKOCTFILE, BWA_SRC, BWA_INCLUDE, BWA_ENGINE_LIB, BWA_ENGINE_DLL, BWA_OUT,
# BWA_STAMP. BWA_ENGINE_LIB is what the MEX LINKS against (the import library on Windows);
# BWA_ENGINE_DLL is what it LOADS. Off Windows they are one file.

get_filename_component(_outdir "${BWA_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}")

# -L plus -l rather than the library's own path: Octave 6's mkoctfile (Ubuntu 22.04) rejects a
# bare .so path with "unrecognized argument", where 10.1 accepts one. The -l form works in both.
get_filename_component(_engdir "${BWA_ENGINE_LIB}" DIRECTORY)
get_filename_component(_engstem "${BWA_ENGINE_LIB}" NAME_WE)
string(REGEX REPLACE "^lib" "" _engname "${_engstem}")

set(_args --mex "${BWA_SRC}" "-I${BWA_INCLUDE}" "-L${_engdir}" "-l${_engname}" -o "${BWA_OUT}")

if(WIN32)
  # MinGW's ld (binutils 2.4x as shipped with Octave 10.1 for Windows) hits an INTERNAL ERROR on
  # this object, "aborting at ldlang.c:527 in compare_section", with no sort order requested.
  # Asking for one explicitly is the whole fix and costs nothing. Without it the link fails
  # outright, so this is not a tidiness flag.
  list(APPEND _args -Wl,--sort-section=name)
elseif(APPLE)
  list(APPEND _args "-Wl,-rpath,@loader_path")
else()
  # The engine library is staged beside the MEX, and that is where the MEX must find it: a user
  # copies one directory, not a directory plus an LD_LIBRARY_PATH.
  #
  # The backslash is load-bearing. mkoctfile hands its link line to a SHELL, which expands a bare
  # $ORIGIN to nothing and writes an EMPTY runpath - a MEX that builds, stages and then refuses to
  # load with "cannot open shared object file". Escaped, the shell passes it through and ld writes
  # the literal string the loader wants. @loader_path above has no $ and needs none of this.
  list(APPEND _args "-Wl,-rpath,\\$ORIGIN")
endif()

# mkoctfile is a driver: it shells out to the compiler it was built with, and the Windows Octave
# installer leaves neither on PATH. Its own directory is where that gcc lives, so put it there for
# the length of this call rather than asking every user to edit their environment.
get_filename_component(_mkdir "${BWA_MKOCTFILE}" DIRECTORY)
if(WIN32)
  set(ENV{PATH} "${_mkdir};$ENV{PATH}")
else()
  set(ENV{PATH} "${_mkdir}:$ENV{PATH}")
endif()

execute_process(COMMAND "${BWA_MKOCTFILE}" ${_args}
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "mkoctfile failed (${_rc}):\n${_out}\n${_err}")
endif()
if(_err)
  message(STATUS "mkoctfile: ${_err}")
endif()

# The engine library goes beside the MEX, so the staged directory is self-contained.
if(WIN32)
  # The DLL is named explicitly rather than derived from the import library's directory: an engine
  # SDK install puts the two in SEPARATE directories (lib/ and bin/), so the old sibling assumption
  # copied nothing and the staged MEX then failed to load on a machine with no engine on PATH.
  file(COPY "${BWA_ENGINE_DLL}" DESTINATION "${_outdir}")
else()
  file(COPY "${BWA_ENGINE_LIB}" DESTINATION "${_outdir}")
endif()

file(WRITE "${BWA_STAMP}" "ok\n")
