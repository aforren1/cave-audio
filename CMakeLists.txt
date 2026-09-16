# bw_audio — self-hosted spatial audio engine for a 26-speaker CAVE array.
# See docs/ for the design; CLAUDE.md for invariants.
#
# What this builds:
#   - bw_audio.dll — the public C ABI (include/bw_audio.h) over the shared core (bwa_core) plus the
#     null/manual sinks. Optional ASIO and Steam Audio backends auto-enable when their SDKs are
#     vendored under third_party/ (see third_party/README.md).
#   - console examples (minimal/ambisonic/streaming/track_monitor) — built every build so
#     they can't drift from the ABI.
#   - profiling benches (profile_bench + bench_situations) — opt-in behind -DBWA_BUILD_BENCH=ON.
#   - the ctest suite (BWA_BUILD_TESTS, ON by default).
#   - opt-in tools: bwa_calibrate (-DBWA_BUILD_CALIBRATE), bwa_calib_view (-DBWA_BUILD_CALIBVIEW),
#     bwa_playground + bwa_layout_tool (-DBWA_BUILD_PLAYGROUND), Godot binding (-DBWA_BUILD_GODOT).
#
# TWO MODES. By default this file compiles the engine and everything above. With
# -DBWA_ENGINE_SDK=<dir> it compiles NOTHING from src/: it imports a prebuilt engine out of that
# install tree and builds only the bindings. One engine binary per platform then serves every
# package that ships on it. See docs/build.md, "The engine SDK".
cmake_minimum_required(VERSION 3.20)
# No project VERSION here: nothing consumes ${PROJECT_VERSION}, and two version streams own the real
# numbers - the ABI version lives in include/bw_audio.h (BWA_VERSION*, what bwa_get_version returns),
# and the distribution/release version is the git tag (stamped into the Unity package at pack time).
project(bw_audio LANGUAGES C)

set(CMAKE_C_STANDARD 11)          # C11 atomics: src/os/os.h's shim leaves the shared state to them
set(CMAKE_C_STANDARD_REQUIRED ON)

# Platform: Windows is the production target (ASIO -> Digiface -> Dante) and the only one with
# device backends so far. Everything else builds the library, the tests and the console examples on
# the null and manual sinks, which is what the Linux and macOS CI jobs run. See docs/backends.md.
if(WIN32)
  message(STATUS "bw_audio: Windows - ASIO and WASAPI backends available")
elseif(ANDROID)
  message(STATUS "bw_audio: Android (API ${ANDROID_PLATFORM_LEVEL}, ${ANDROID_ABI}) - AAudio backend available")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "bw_audio: Linux - JACK and ALSA backends available")
else()
  message(STATUS "bw_audio: ${CMAKE_SYSTEM_NAME} - null and manual sinks only (no device backend yet)")
endif()

# The codebase uses standard C string functions (strncpy etc.); silence MSVC's
# nonstandard *_s deprecation warnings rather than fork the code for one compiler.
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(bwa_bindings)

# ---- SDK MODE: bindings only, against a prebuilt engine ----------------------------------------
# -DBWA_ENGINE_SDK=<dir> points at the install tree `cmake --install` writes from an ordinary build
# of this same file. Nothing under src/ is compiled here, no test or tool is registered, and the
# engine the bindings link is the file that tree already holds - so the wheel, the MEX pair and the
# GDExtension on one platform all ship the ONE binary that platform's ctest run tested.
#
# The repo root stays the CMake source dir either way, which is what lets pyproject.toml keep
# `cmake.source-dir = "../.."` and a wheel build reach SDK mode through one -D.
#
# `return()` rather than wrapping the rest of the file in an if(): the engine build below is ~1100
# lines and re-indenting it would hide the one thing this block does.
set(BWA_ENGINE_SDK "" CACHE PATH
    "Build ONLY the bindings, against the prebuilt engine SDK installed at this prefix")
if(BWA_ENGINE_SDK)
  if(NOT IS_DIRECTORY "${BWA_ENGINE_SDK}")
    message(FATAL_ERROR "BWA_ENGINE_SDK=${BWA_ENGINE_SDK} is not a directory. It must be the "
                        "prefix a `cmake --install` of this project wrote (docs/build.md).")
  endif()
  # NO_DEFAULT_PATH: a bw_audio installed somewhere else on the machine must never answer instead
  # of the tree that was named. A wrong engine here is the exact failure this mode exists to avoid.
  find_package(bw_audio CONFIG REQUIRED PATHS "${BWA_ENGINE_SDK}" NO_DEFAULT_PATH)
  message(STATUS "bw_audio: SDK MODE - bindings only, against ${BWA_ENGINE_SDK}")
  # The configurations the SDK actually carries. An SDK installed from a RelWithDebInfo build and
  # consumed from a Debug tree works (CMake falls back to whatever the imported target has), but it
  # is worth SAYING which binary is about to be shipped rather than leaving it to be inferred.
  get_target_property(BWA_SDK_CONFIGS bwa::bw_audio IMPORTED_CONFIGURATIONS)
  message(STATUS "bw_audio: SDK ABI version ${bw_audio_VERSION}, configurations: ${BWA_SDK_CONFIGS}")

  # Same option and the same enable_testing() the in-tree path has, so each binding registers its
  # ctests here exactly as it does there.
  option(BWA_BUILD_TESTS "Build the bw_audio tests" ON)
  if(BWA_BUILD_TESTS)
    enable_testing()
  endif()

  # Nothing under src/ is compiled here, so every engine-side option is inert. Say so: a flag that
  # is quietly dropped is how someone spends an afternoon looking for a tool this tree was never
  # going to build.
  foreach(opt BWA_BUILD_PLAYGROUND BWA_BUILD_CALIBVIEW BWA_BUILD_CALIBRATE BWA_BUILD_BENCH
              BWA_TRACY BWA_PROFILE_SELF BWA_ASAN)
    if(${opt})
      message(WARNING "bw_audio: ${opt} is IGNORED under BWA_ENGINE_SDK - this tree builds bindings "
                      "only. Build it in an ordinary (engine) tree instead.")
    endif()
  endforeach()

  bwa_add_bindings()
  return()
endif()

# The gcc/clang counterpart of the /W4 the MSVC targets carry. Applied per target below, not
# globally, so the fetched third-party sources keep their own warning levels.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(BWA_C_WARNINGS -Wall -Wextra)
else()
  set(BWA_C_WARNINGS)
endif()

# ---- third-party single-header / single-file deps, fetched + pinned (not vendored) ----
# Offline builds can override with -DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>. See third_party/README.md.
include(FetchContent)
# dr_libs (dr_wav / dr_flac / dr_mp3): one pinned snapshot of the whole header-only repo, so the
# commit lives in exactly ONE place. The gzipped tarball (~220 KB) is smaller than the raw headers,
# and CMake strips the single top-level dir, so ${dr_libs_SOURCE_DIR} holds the headers directly.
FetchContent_Declare(dr_libs
  URL https://github.com/mackron/dr_libs/archive/c629ca6f5ad6e013980b7db31043d5b4d1b63787.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)                            # wav-0.14.6 / flac-0.13.4 / mp3-0.7.4, PD or MIT-0
FetchContent_Declare(cjson_src
  URL https://raw.githubusercontent.com/DaveGamble/cJSON/c859b25da02955fef659d658b8f324b5cde87be3/cJSON.c
  DOWNLOAD_NO_EXTRACT TRUE)                                   # v1.7.19
FetchContent_Declare(cjson_hdr
  URL https://raw.githubusercontent.com/DaveGamble/cJSON/c859b25da02955fef659d658b8f324b5cde87be3/cJSON.h
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(dr_libs cjson_src cjson_hdr)
set(CJSON_C   "${cjson_src_SOURCE_DIR}/cJSON.c")
set(CJSON_INC "${cjson_hdr_SOURCE_DIR}")

# ---- optional Tracy profiler instrumentation (opt-in; default build stays lean) ----
# cmake -S . -B build -DBWA_TRACY=ON, then connect the standalone Tracy profiler to the running
# engine. ON_DEMAND => the client only collects while a profiler is attached (near-zero idle cost).
option(BWA_TRACY "Instrument with the Tracy profiler (fetches Tracy)" OFF)
if(BWA_TRACY)
  set(TRACY_ENABLE    ON CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(tracy)
endif()

# The OS portability shim (src/os/os.h): threads, sleep, the monotonic clock, mutexes, thread
# priority, strdup/strcasecmp and the UDP socket calls natnet.c makes. Exactly one half compiles.
if(WIN32)
  set(BWA_OS_SRC src/os/os_win.c)
else()
  set(BWA_OS_SRC src/os/os_posix.c)
endif()

# ---- Steam Audio (phonon) detection ----
# Detection only, and it runs HERE rather than beside the wiring below because it decides the whole
# tree's C runtime (see the CRT note at the end of this block) and a target reads that at creation.
# The wiring - sources, include dirs, link, BWA_HAVE_STEAMAUDIO - is further down, with the other
# backends. Apache-2.0 (GPLv3-compatible). Auto-enabled when the built phonon SDK is vendored at
# third_party/steam-audio-artifacts/ (third_party/README.md).
set(STEAMAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party/steam-audio-artifacts" CACHE PATH "Steam Audio (phonon) SDK root")
# phonon is linked STATICALLY (third_party/README.md): no phonon.dll, no libphonon.so, nothing to
# ship beside the engine. phonon.h serves both link modes unchanged, because IPLAPI expands to
# __declspec(dllexport) only while STEAMAUDIO_BUILDING_CORE is defined and to nothing for a consumer.
#
# Detection is keyed on the PLATFORM's lib dir plus the full static archive set, so the staged tree
# is platform-neutral: include/ is shared, lib/<platform>/ holds one archive set per platform, and a
# checkout can carry several at once. phonon's own archive holds only its core + hrtf objects, so its
# three third-party deps link beside it. Today only windows-x64 is built; the other names below are
# the contract the Linux, macOS and Android phonon builds stage into (docs/backends.md), not a claim
# that they exist. A platform with no lib dir staged is a no-SDK build, which is fully viable.
if(ANDROID)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(BWA_PHONON_PLATFORMS android-arm64)
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(BWA_PHONON_PLATFORMS android-x64)
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(BWA_PHONON_PLATFORMS android-armv7)
  else()
    set(BWA_PHONON_PLATFORMS "")
  endif()
elseif(WIN32)
  set(BWA_PHONON_PLATFORMS windows-x64)
elseif(APPLE)
  set(BWA_PHONON_PLATFORMS osx-universal osx-arm64 osx-x64)   # first one staged wins
elseif(UNIX)
  set(BWA_PHONON_PLATFORMS linux-x64)
else()
  set(BWA_PHONON_PLATFORMS "")
endif()

set(BWA_PHONON_PLATFORM "")
foreach(bwa_plat IN LISTS BWA_PHONON_PLATFORMS)
  if(IS_DIRECTORY "${STEAMAUDIO_DIR}/lib/${bwa_plat}")
    set(BWA_PHONON_PLATFORM "${bwa_plat}")
    break()
  endif()
endforeach()

set(BWA_PHONON_LIBS "")
if(BWA_PHONON_PLATFORM)
  set(BWA_PHONON_LIBDIR "${STEAMAUDIO_DIR}/lib/${BWA_PHONON_PLATFORM}")
  # The archive set, in the platform's own spelling. Android builds pffft like every other platform:
  # upstream can link FFTS there instead, but only behind a non-default flag and with NEON off.
  if(BWA_PHONON_PLATFORM MATCHES "^windows")
    set(bwa_phonon_names phonon.lib mysofa.lib zlibstatic.lib pffft.lib)
  else()
    set(bwa_phonon_names libphonon.a libmysofa.a libz.a libpffft.a)
  endif()
  set(bwa_phonon_missing "")
  foreach(bwa_name IN LISTS bwa_phonon_names)
    if(EXISTS "${BWA_PHONON_LIBDIR}/${bwa_name}")
      list(APPEND BWA_PHONON_LIBS "${BWA_PHONON_LIBDIR}/${bwa_name}")
    else()
      list(APPEND bwa_phonon_missing "${bwa_name}")
    endif()
  endforeach()
  if(NOT EXISTS "${STEAMAUDIO_DIR}/include/phonon.h")
    list(APPEND bwa_phonon_missing "../../include/phonon.h")
  endif()
  # Android only: phonon's own logger calls __android_log_print, which lives in the system liblog.
  # A SHARED phonon resolved that for itself; a static one hands the reference to whoever links it,
  # so it goes on the end of the archive list and every consumer of BWA_PHONON_LIBS gets it.
  if(ANDROID AND BWA_PHONON_LIBS)
    list(APPEND BWA_PHONON_LIBS log)
  endif()
  # A staged tree that is PRESENT but incomplete is a configuration error, never a quiet fall to the
  # no-SDK build: the commonest way to reach it is an artifacts dir left over from the old SHARED
  # staging (phonon.lib beside a phonon.dll), and silently dropping the HRTF decode, occlusion,
  # reflections and pathing out of that build is the worst possible way to find out.
  if(bwa_phonon_missing)
    string(REPLACE ";" " " bwa_phonon_missing_str "${bwa_phonon_missing}")
    message(FATAL_ERROR
      "Steam Audio staging at ${BWA_PHONON_LIBDIR} is INCOMPLETE: missing ${bwa_phonon_missing_str}. "
      "phonon is linked statically now, so lib/<platform>/ must hold the phonon archive plus its "
      "three companions (mysofa, zlib, pffft). A directory "
      "holding phonon.lib beside a phonon.dll is the OLD SHARED layout: rebuild phonon with "
      "BUILD_SHARED_LIBS=OFF and restage it, or delete ${STEAMAUDIO_DIR} for a no-SDK build. "
      "Recipe and layout: third_party/README.md, section \"Steam Audio\".")
  endif()
endif()

# The staged phonon is built RELEASE and /MD (third_party/README.md's CRT note), and a STATIC
# archive CARRIES that choice where the old import library did not: a /MDd Debug consumer gets one
# LNK2038 per archive member, not a warning. So when phonon is linked the whole tree uses the
# release dynamic CRT in EVERY config. A Debug build keeps /Od, /Zi, /RTC1 and full symbols; what
# it gives up is the debug CRT's own heap and iterator checks, which cost little here (the engine
# is C and carries no STL of its own) and which -DBWA_ASAN=ON covers properly. Downstream is
# unaffected: a consumer's /MDd application loading this DLL was always a two-CRT arrangement,
# exactly as it was when phonon was its own /MD DLL beside it.
#
# CACHE, so godot-cpp's own "if not already cached" set in bindings/godot defers to this one and
# the whole tree, extension included, agrees on one CRT.
if(MSVC AND BWA_PHONON_LIBS)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING
      "Select the MSVC runtime library for use by compilers targeting the MSVC ABI." FORCE)
endif()

# ---- shared core (everything except the public ABI shim and the device sinks) ----
# An OBJECT library so the off-RT-path tests can compile the core in directly and reach
# its internals, without re-listing sources per target.
add_library(bwa_core OBJECT
  src/core/rt.c
  src/core/sound.c
  src/core/assets.c
  src/core/layout.c
  src/spatial/dbap.c
  src/spatial/spcap.c
  src/spatial/vbap.c
  src/spatial/cap.c
  src/spatial/hole.c
  src/spatial/hull.c
  src/spatial/align.c
  src/binaural/binaural.c
  src/binaural/hpeq.c
  src/spatial/ambisonics.c
  src/spatial/allrad.c
  src/spatial/epad.c
  src/acoustics/fdn.c
  src/acoustics/ism.c
  src/tracking/natnet.c
  ${BWA_OS_SRC}
  src/calib/measure.c
  src/calib/calib.c
  src/calib/zylia.c
  src/calib/valid.c
  src/core/stream.c
  ${CJSON_C}
)
target_include_directories(bwa_core PUBLIC src include ${dr_libs_SOURCE_DIR} ${CJSON_INC})   # include/: frame.h consumes the BWA_ROOM_* basis
# The platform libraries. Named once here because three groups of targets need them: bwa_core, the
# dll (which takes bwa_core's OBJECTS, so bwa_core's link interface does NOT reach it), and the few
# tests that compile the shim in directly. libm is in the same list because it is the same kind of
# fact: the MSVC CRT carries the math functions and glibc does not.
function(bwa_link_os target scope)
  if(WIN32)
    # ws2_32: the UDP shim. winmm: timeBeginPeriod. avrt: the "Pro Audio" MMCSS task every
    # self-paced render thread joins through os_thread_set_realtime.
    target_link_libraries(${target} ${scope} ws2_32 winmm avrt)
  else()
    # CMAKE_DL_LIBS: os_dl_open/_sym/_close. -ldl on an older glibc, empty on a current one and on
    # Android and Apple, where dlopen lives in libc. It belongs here rather than beside the Linux
    # backends, because the shim is what calls it and every target that compiles the shim needs it.
    find_package(Threads REQUIRED)
    target_link_libraries(${target} ${scope} Threads::Threads m ${CMAKE_DL_LIBS})
  endif()
endfunction()
bwa_link_os(bwa_core PUBLIC)
target_compile_options(bwa_core PRIVATE ${BWA_C_WARNINGS})
# A shared library on gcc/clang exports everything by default; the public ABI says what it is
# (BWA_API in bw_audio.h, BWA_EXPORT in src/os/os.h for the internal test hooks).
set_target_properties(bwa_core PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(bwa_core PRIVATE /W4)
  # C11 <stdatomic.h> is gated behind /experimental:c11atomics on MSVC. Source properties are
  # DIRECTORY-scoped, so one entry covers every target in this file that compiles the file.
  # cJSON is third-party: silence its warnings.
  foreach(f src/core/rt.c            # the two SPSC rings, the commit snapshot, the clock seqlock
            src/core/stream.c        # the per-stream ring cursors
            src/core/assets.c        # the loader thread's job + result rings
            src/acoustics/fdn.c           # the live wet-gain atomic
            src/tracking/natnet.c        # the pose seqlock (src/tracking/pose.h) + the receiver's stop flag
            src/engine.c        # the hpeq handoff + the cave_both monitor seqlock
            src/sink/null_sink.c     # the health counters
            src/core/profile_self.c  # the zone table
            src/acoustics/steam_scene.c src/acoustics/steam_path.c)   # the sim threads' stop flags
    set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endforeach()
  set_source_files_properties(${CJSON_C} PROPERTIES COMPILE_OPTIONS /W0)
endif()

# ---- core library (public C ABI in include/bw_audio.h) ----
add_library(bw_audio SHARED
  src/engine.c
  src/sink/sink.c
  src/sink/sink_quant.c              # the fixed-quantum adapter every non-ASIO backend renders through
  src/sink/null_sink.c
  src/sink/manual_sink.c
  src/core/profile_self.c            # bwa_prof_reset/report exported unconditionally; accumulation gated by BWA_PROFILE_SELF
  $<TARGET_OBJECTS:bwa_core>
)
set_target_properties(bw_audio PROPERTIES C_VISIBILITY_PRESET hidden)
# ONE name for the engine in BOTH root modes: in-tree this aliases the real target, and SDK mode
# imports a target of the same spelling out of the installed package. Every binding links
# `bwa::bw_audio` and never asks which mode built it.
add_library(bwa::bw_audio ALIAS bw_audio)
# BUILD_INTERFACE / INSTALL_INTERFACE rather than a bare `include`: a source-tree path in the link
# interface is an install(EXPORT) error, and the SDK below exports this target.
target_include_directories(bw_audio
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE src ${dr_libs_SOURCE_DIR} ${CJSON_INC})
target_compile_definitions(bw_audio PRIVATE BWA_BUILD_DLL)
target_compile_options(bw_audio PRIVATE ${BWA_C_WARNINGS})
if(MSVC)
  target_compile_options(bw_audio PRIVATE /W4)
endif()
bwa_link_os(bw_audio PRIVATE)
if(NOT WIN32)
  # INTERFACE too: the console examples do their own math (sinf for the test tones), and the MSVC
  # CRT carries those while glibc splits them into libm. A client of the shared library that calls
  # sinf needs the same line, so it belongs in the link interface rather than in each example.
  target_link_libraries(bw_audio INTERFACE m)
endif()

# Dead-code strip, on the linkers whose spelling this is (GNU ld and lld: Linux and Android). Every
# function and data item gets its own section so the linker can drop what no exported symbol
# reaches - which is most of the library, since -fvisibility=hidden already says the bwa_* ABI is
# the whole root set. Measured on Linux with a static phonon linked in: 7,214,352 stripped bytes
# down to 7,066,008, for no build-time cost worth naming. MSVC gets the same effect from /OPT:REF,
# which it applies to a release link by itself; Apple's ld64 spells it -dead_strip and no Mac was
# available to verify that, so it stays off there rather than going in unmeasured.
#
# LTO was measured beside this and NOT taken: 7,066,008 stripped bytes down to 7,033,208, which is
# 0.46 percent. phonon's archive is the bulk of the library and its objects are not LTO objects, so
# the plugin only ever sees our own - not worth changing the codegen of a real-time DSP library
# for, unprofiled.
#
# The obvious next question - phonon builds with only -fPIC -g -fvisibility=hidden, so the strip
# above can only reach OUR objects - was measured too, and also NOT taken. Rebuilding phonon with
# -ffunction-sections -fdata-sections (confirmed present in all 112 of its compile commands) took
# 7,066,008 stripped bytes to 6,988,184: 77,824 bytes, 1.1 percent, with the suite still 38/38. The
# reason it is so small is that a static archive already contributes only the MEMBERS something
# references, so the coarse pass had happened before the linker ever looked at a section, and what
# is left of phonon is reached through its factories and vtables. Against that, adopting it would
# have to thread a space-bearing -DCMAKE_C_FLAGS through the composite action's word-split
# cmake-flags input and bump every phonon cache key, since no key covers the flags.
if(UNIX AND NOT APPLE)
  target_compile_options(bwa_core PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(bw_audio PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(bw_audio PRIVATE -Wl,--gc-sections)
  # A GNU build id, EXPLICITLY. Debian's and Ubuntu's gcc pass --build-id through their specs,
  # so a runner build always had one; the manylinux image's gcc does not, and the first
  # container-built engine carried no .note.gnu.build-id at all. Two consumers need it: the
  # CI check that the release wheel's stripped engine is the SAME LINK as the tested SDK's
  # (bytes cannot be compared across a strip, the note survives one), and the split
  # libbw_audio.so.debug sidecar, which gdb and every symbolizer match to the binary by this
  # id before they fall back to the gnu_debuglink name. Costs 20 bytes.
  target_link_options(bw_audio PRIVATE -Wl,--build-id=sha1)
endif()

if(BWA_TRACY)
  # bwa_core is an OBJECT lib: linking TracyClient gives rt.c Tracy's headers (no link step here); the
  # DLL links the actual client. BWA_TRACY flips profile.h's macros on for both compile groups.
  target_link_libraries(bwa_core PRIVATE TracyClient)
  target_compile_definitions(bwa_core PRIVATE BWA_TRACY)
  target_link_libraries(bw_audio PRIVATE TracyClient)
  target_compile_definitions(bw_audio PRIVATE BWA_TRACY)
  message(STATUS "bw_audio: Tracy profiling ENABLED")
endif()

# In-process zone profiler (mutually exclusive with Tracy): headless per-zone numbers with no server
# tooling. Routes profile.h's zone macros into src/core/profile_self.c on both compile groups.
option(BWA_PROFILE_SELF "Instrument with the built-in in-process zone profiler (bwa_prof_report)" OFF)
if(BWA_PROFILE_SELF AND NOT BWA_TRACY)
  target_compile_definitions(bwa_core PRIVATE BWA_PROFILE_SELF)
  target_compile_definitions(bw_audio PRIVATE BWA_PROFILE_SELF)
  message(STATUS "bw_audio: in-process zone profiler ENABLED")
endif()

# ---- optional ASIO backend (production: 26ch -> Digiface -> Dante) ----
# Auto-enabled when the SDK is present; the SDK is GPLv3/proprietary dual-licensed
# and is NOT committed to this repo (third_party/README.md explains how to fetch it).
set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/asiosdk" CACHE PATH "Steinberg ASIO SDK root")
option(BWA_WITH_ASIO "Build the ASIO backend (needs the Steinberg ASIO SDK)" OFF)
# WIN32 as well as the SDK: ASIO is a Windows driver model and asio_sink.cpp is a Windows source.
# A checkout with the SDK fetched is the normal state on a dev machine, so without this guard a
# Linux or macOS configure of the SAME tree would "find" it and then fail to compile.
if(WIN32 AND EXISTS "${ASIO_SDK_DIR}/common/asio.h")
  set(BWA_WITH_ASIO ON)
elseif(NOT WIN32)
  set(BWA_WITH_ASIO OFF)
endif()

# Compile the Steinberg ASIO SDK host sources into a target that runs its own ASIO host (asiolist +
# asiodrivers enumerate/load drivers; asio.cpp is the C++ glue) and wire up its includes, the
# BWA_HAVE_ASIO define, and the COM/registry libs the host needs. Shared by the engine DLL and every
# capture tool. _CRT_SECURE_NO_WARNINGS is set globally at the top of this file; cxx_std_20 and the
# engine's own asio_sink.cpp are added at the DLL's call site. Guard each call with if(BWA_WITH_ASIO).
function(bwa_add_asio_host target)
  target_sources(${target} PRIVATE
    ${ASIO_SDK_DIR}/common/asio.cpp
    ${ASIO_SDK_DIR}/host/asiodrivers.cpp
    ${ASIO_SDK_DIR}/host/pc/asiolist.cpp)
  target_include_directories(${target} PRIVATE
    ${ASIO_SDK_DIR}/common ${ASIO_SDK_DIR}/host ${ASIO_SDK_DIR}/host/pc)
  target_compile_definitions(${target} PRIVATE BWA_HAVE_ASIO)
  target_link_libraries(${target} PRIVATE ole32 user32 advapi32)
endfunction()

if(BWA_WITH_ASIO)
  if(NOT EXISTS "${ASIO_SDK_DIR}/common/asio.h")
    message(FATAL_ERROR "BWA_WITH_ASIO=ON but no ASIO SDK at ${ASIO_SDK_DIR} (see third_party/README.md)")
  endif()
  enable_language(CXX)
  # C++20 on the DLL: asio_sink.cpp uses designated initializers (ASIO_VT) — MSVC needs /std:c++20 for
  # those. Target-scoped (not a global set()) so it can't leak into other targets or the vendored SDK.
  target_compile_features(bw_audio PRIVATE cxx_std_20)
  target_sources(bw_audio PRIVATE src/sink/asio_sink.cpp)   # the engine's own output-only ASIO sink
  bwa_add_asio_host(bw_audio)
  message(STATUS "bw_audio: ASIO backend ENABLED (${ASIO_SDK_DIR})")
else()
  message(STATUS "bw_audio: ASIO backend disabled (no SDK at ${ASIO_SDK_DIR}); null sink only")
endif()

# ---- WASAPI backend (Windows system mixer: the headphone profiles + the cave_both monitor) ----
# No SDK to vendor - the headers ship with the Windows SDK - so this is ON wherever it can build,
# and cannot be forced on elsewhere. ole32: COM + the endpoint enumerator. avrt: the "Pro Audio"
# MMCSS class the render thread joins.
option(BWA_WITH_WASAPI "Build the WASAPI backend (Windows only)" ON)
if(BWA_WITH_WASAPI AND WIN32)
  enable_language(CXX)
  # C++20 on the DLL: wasapi_sink.cpp uses designated initializers (WASAPI_VT), which MSVC gates
  # behind /std:c++20. Target-scoped, and idempotent with the ASIO block's identical request.
  target_compile_features(bw_audio PRIVATE cxx_std_20)
  target_sources(bw_audio PRIVATE src/sink/wasapi_sink.cpp)
  target_compile_definitions(bw_audio PRIVATE BWA_HAVE_WASAPI)
  target_link_libraries(bw_audio PRIVATE ole32)   # avrt comes from bwa_link_os (the MMCSS shim)
  message(STATUS "bw_audio: WASAPI backend ENABLED")
else()
  set(BWA_WITH_WASAPI OFF)
  message(STATUS "bw_audio: WASAPI backend disabled (Windows only)")
endif()

# ---- JACK backend (Linux: the production path, and a PipeWire desktop) ----
# No SDK to vendor. libjack is LGPL-2.1 and pipewire-jack's drop-in libjack (MIT) answers the same
# ABI, so which server the binary talks to is a RUN-TIME fact, not a build one.
#
# HEADERS ONLY, and this is the point of the block: jack_sink.c dlopens libjack.so.0 and resolves
# every symbol itself (os_dl_*), so nothing libjack defines is an undefined reference in
# libbw_audio.so and a box with no JACK loads the library and runs the offline sink. pkg-config
# still has to find jack, because the types, enums and callback signatures come from its headers -
# the INCLUDE DIRECTORIES of the imported target are taken, the link interface is not.
option(BWA_WITH_JACK "Build the JACK backend (Linux only; needs the libjack HEADERS via pkg-config)" ON)
if(BWA_WITH_JACK AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(BWA_JACK QUIET IMPORTED_TARGET jack)
  endif()
  if(BWA_JACK_FOUND)
    target_sources(bw_audio PRIVATE src/sink/jack_sink.c)
    target_compile_definitions(bw_audio PRIVATE BWA_HAVE_JACK)
    target_include_directories(bw_audio PRIVATE ${BWA_JACK_INCLUDE_DIRS})
    message(STATUS "bw_audio: JACK backend ENABLED (libjack ${BWA_JACK_VERSION} headers; loaded at run time)")
  else()
    set(BWA_WITH_JACK OFF)
    message(STATUS "bw_audio: JACK backend disabled (pkg-config found no jack; install libjack-jackd2-dev or pipewire-jack)")
  endif()
else()
  set(BWA_WITH_JACK OFF)
  message(STATUS "bw_audio: JACK backend disabled (Linux only)")
endif()

# ---- ALSA backend (Linux: the no-server path) ----
# alsa-lib is LGPL-2.1 - one line in build.md's license inventory and nothing else changes.
# HEADERS ONLY, same rule as JACK above: alsa_sink.c dlopens libasound.so.2 at run time, so
# find_package(ALSA) is asked for its include directory and not for its link interface.
option(BWA_WITH_ALSA "Build the ALSA backend (Linux only; needs the alsa-lib HEADERS)" ON)
if(BWA_WITH_ALSA AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    target_sources(bw_audio PRIVATE src/sink/alsa_sink.c)
    target_compile_definitions(bw_audio PRIVATE BWA_HAVE_ALSA)
    target_include_directories(bw_audio PRIVATE ${ALSA_INCLUDE_DIRS})
    message(STATUS "bw_audio: ALSA backend ENABLED (alsa-lib ${ALSA_VERSION_STRING} headers; loaded at run time)")
  else()
    set(BWA_WITH_ALSA OFF)
    message(STATUS "bw_audio: ALSA backend disabled (find_package(ALSA) failed; install libasound2-dev)")
  endif()
else()
  set(BWA_WITH_ALSA OFF)
  message(STATUS "bw_audio: ALSA backend disabled (Linux only)")
endif()

# ---- AAudio backend (Android: headphones on a standalone VR headset) ----
# No SDK to vendor and no license line to add: libaaudio is part of the Android system image and
# the NDK ships its stub, so this is ON wherever it can build and cannot be forced on elsewhere.
# ANDROID_PLATFORM 26 or later, which is AAudio's own floor.
option(BWA_WITH_AAUDIO "Build the AAudio backend (Android only)" ON)
if(BWA_WITH_AAUDIO AND ANDROID)
  if(ANDROID_PLATFORM_LEVEL AND ANDROID_PLATFORM_LEVEL LESS 26)
    message(FATAL_ERROR "BWA_WITH_AAUDIO=ON needs ANDROID_PLATFORM android-26 or later "
                        "(AAudio arrived in Android 8.0); got ${ANDROID_PLATFORM}")
  endif()
  target_sources(bw_audio PRIVATE src/sink/aaudio_sink.c)
  target_compile_definitions(bw_audio PRIVATE BWA_HAVE_AAUDIO)
  target_link_libraries(bw_audio PRIVATE aaudio)
  message(STATUS "bw_audio: AAudio backend ENABLED")
else()
  set(BWA_WITH_AAUDIO OFF)
  message(STATUS "bw_audio: AAudio backend disabled (Android only)")
endif()

# ---- optional Steam Audio backend (production binaural HRTF decode; later occlusion/reflections) ----
# The WIRING half. Detection ran near the top of this file, where it had to, so everything below
# reads BWA_PHONON_LIBS rather than touching the staged tree again. Stage 1 of the production
# monitor (the SH encode in src/spatial/ambisonics.c) is always built + tested; stage 2 (the
# ambisonics->binaural HRTF decode, gated BWA_HAVE_STEAMAUDIO) supersedes the first-cut pan in
# binaural.c and lands with the SDK, on-SDK-verified.
if(BWA_PHONON_LIBS)
  set(BWA_STEAMAUDIO_FOUND TRUE)   # consumed by the with-SDK reflection test below
  target_sources(bw_audio PRIVATE src/binaural/steam_decode.c src/acoustics/steam_scene.c src/acoustics/steam_reflect.c src/acoustics/steam_path.c)  # HRTF decode + occlusion + reflections + pathing (with-SDK)
  if(MSVC)   # steam_reflect.c uses C11 <stdatomic.h> (the IR-publish seqlock), like rt.c
    set_source_files_properties(src/acoustics/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()
  target_include_directories(bw_audio PRIVATE "${STEAMAUDIO_DIR}/include")
  target_link_libraries(bw_audio PRIVATE ${BWA_PHONON_LIBS})
  target_compile_definitions(bw_audio PRIVATE BWA_HAVE_STEAMAUDIO)
  # A static phonon is C++ and this library is mostly C. On Windows the C++ sinks already force the
  # C++ link driver, but a platform that builds none of them would pick the C one and leave every
  # std:: symbol in the archive unresolved, so say it here rather than let it depend on the backend.
  #
  # enable_language FIRST, and this is not defensive: project() declares LANGUAGES C, and the only
  # other enable_language(CXX) calls sit under ASIO, WASAPI and the GUI tools, all Windows. Ask for
  # a CXX link driver that the project never enabled and CMake does not error - it emits an EMPTY
  # link rule, so ninja reports "Linking CXX shared library libbw_audio.so", exits 0, and writes no
  # file at all. Every test still built (they link bwa_core's objects, not the library) and the
  # first thing that noticed was an example failing to start.
  enable_language(CXX)
  set_target_properties(bw_audio PROPERTIES LINKER_LANGUAGE CXX)
  if(UNIX AND NOT APPLE)   # GNU ld and lld: Linux and Android
    # phonon's Android build sets no -fvisibility=hidden of its own, so every ipl::, flatbuffers::
    # and libc++ symbol in the archive would land in this library's dynamic symbol table: about 3000
    # exports beside our 173, and a name a second .so in the same app could bind to by accident.
    # --exclude-libs is the NDK's own answer - it makes the symbols a STATIC archive contributes
    # local. Our own objects are not in an archive, so the bwa_* exports are untouched.
    #
    # LINUX NEEDS IT TOO, and -fvisibility=hidden is not the answer there either: phonon's Linux
    # build DOES set it, but IPLAPI is __attribute__((visibility("default"))) while
    # STEAMAUDIO_BUILDING_CORE is defined, so the archive deliberately marks all 216 ipl* entry
    # points exported. Measured on libbw_audio.so: 561 dynamic symbols without the flag, 173 (the
    # bwa_* ABI, exactly) with it.
    target_link_options(bw_audio PRIVATE -Wl,--exclude-libs,ALL)
  endif()
  if(MSVC)
    # phonon's objects carry /GL, so the consuming link runs whole-program codegen and the linker
    # drops incremental linking anyway - with an LNK4075 per target unless we say so first.
    # /OPT:REF: RelWithDebInfo passes /DEBUG, which turns unreferenced-function removal OFF by
    # default, and a static phonon is where that costs: measured 7,677,440 -> ~6.8 MB with it on
    # (most of the saving is phonon members nothing here calls). /OPT:ICF is deliberately NOT
    # added: folding identical functions makes the shipped, debuggable RelWithDebInfo harder to
    # step through, for a few hundred KB more.
    # /IGNORE:4099: phonon's objects were compiled /Zi and name a core.pdb that is never staged
    # beside the archive (nor shipped), so the linker would report one LNK4099 per member on
    # every machine but the one that built phonon. They link without debug info, as on CI.
    target_link_options(bw_audio PRIVATE /INCREMENTAL:NO /OPT:REF /IGNORE:4099)
  endif()
  message(STATUS "bw_audio: Steam Audio ENABLED (${STEAMAUDIO_DIR}, static, ${BWA_PHONON_PLATFORM}); binaural uses the HRTF decode")
else()
  message(STATUS "bw_audio: Steam Audio disabled (no SDK at ${STEAMAUDIO_DIR}); binaural uses the first-cut pan")
endif()

# ---- the engine SDK: the install tree every binding can link against instead of rebuilding ------
# `cmake --install <build> --prefix <sdk> [--config <cfg>]` writes:
#
#   <sdk>/include/bw_audio.h
#   <sdk>/lib/cmake/bw_audio/{bw_audioConfig,bw_audioConfigVersion,bw_audioTargets*}.cmake
#   <sdk>/lib/{bw_audio.lib | libbw_audio.so | libbw_audio.dylib}
#   <sdk>/bin/{bw_audio.dll, bw_audio.pdb}                                        (Windows only)
#
# It is the whole reason SDK MODE at the top of this file exists, and it is also just a normal
# CMake package: a C client outside this repo does find_package(bw_audio CONFIG) against it.
#
# NOTHING IS STRIPPED HERE. The library goes in as the build produced it - unstripped on Linux and
# macOS, with its .pdb beside it on Windows - because the consumers that want symbols gone already
# remove them at their own staging step (the dist packs split them into a sidecar, the Python
# install(FILES) strips its copy) and an SDK that arrived pre-stripped could not feed those.
# EVERY rule here carries COMPONENT bwa_sdk, and the install command names it:
#
#   cmake --install <build> --prefix <sdk> --component bwa_sdk --config RelWithDebInfo
#
# The component is not tidiness. This build tree also holds install rules it does not own -
# raylib's, when the playground is on - and rules it owns for a DIFFERENT destination: the Python
# binding installs the wheel's contents (component bwa_python). One unfiltered install would mix
# all three into one prefix, and the wheel would grow a second copy of the engine at bin/.
set(BWA_INSTALL_CMAKEDIR "lib/cmake/bw_audio")

install(TARGETS bw_audio EXPORT bw_audioTargets COMPONENT bwa_sdk
        RUNTIME DESTINATION bin            # the DLL on Windows
        LIBRARY DESTINATION lib            # the .so / .dylib
        ARCHIVE DESTINATION lib)           # the Windows import library
# No INCLUDES DESTINATION: the target already carries $<INSTALL_INTERFACE:include>, and both would
# put the same directory in the exported interface twice.
install(FILES include/bw_audio.h DESTINATION include COMPONENT bwa_sdk)
# GPLv3 travels with the binary, the same rule the wheel and both packs follow.
install(FILES LICENSE THIRD_PARTY-NOTICES.md DESTINATION . COMPONENT bwa_sdk)
if(MSVC)
  # OPTIONAL: a config without debug info (Release) generates no .pdb, and asking for one that was
  # never written would fail the install rather than skip it.
  install(FILES $<TARGET_PDB_FILE:bw_audio> DESTINATION bin COMPONENT bwa_sdk OPTIONAL)
endif()
install(EXPORT bw_audioTargets NAMESPACE bwa:: FILE bw_audioTargets.cmake
        DESTINATION "${BWA_INSTALL_CMAKEDIR}" COMPONENT bwa_sdk)

# The package version is the ABI version in include/bw_audio.h - the same number bwa_get_version
# returns and the same one the Python wheel takes its version from. There is no second stream to
# keep in step: read it out of the header.
file(STRINGS include/bw_audio.h BWA_VERSION_LINES REGEX "^#define BWA_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(part MAJOR MINOR PATCH)
  string(REGEX MATCH "#define BWA_VERSION_${part} ([0-9]+)" _m "${BWA_VERSION_LINES}")
  set(BWA_V_${part} "${CMAKE_MATCH_1}")
endforeach()
set(BWA_ABI_VERSION "${BWA_V_MAJOR}.${BWA_V_MINOR}.${BWA_V_PATCH}")

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/bw_audioConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/bw_audioConfig.cmake"
  INSTALL_DESTINATION "${BWA_INSTALL_CMAKEDIR}")
# SameMinorVersion: struct layouts and enum values are only guaranteed inside one major.minor
# (include/bw_audio.h), which is the same rule the MATLAB and Python load-time guards enforce.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/bw_audioConfigVersion.cmake"
  VERSION "${BWA_ABI_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bw_audioConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/bw_audioConfigVersion.cmake"
        DESTINATION "${BWA_INSTALL_CMAKEDIR}" COMPONENT bwa_sdk)

# ---- stage runtime libraries into the Unity package (bindings/unity/Runtime/Plugins/<abi>) ----
# Gitignored build artifacts; the UPM package picks them up so it "just works" after a build.
# Unity keys a native plugin by its FOLDER as well as by the import settings in its .meta, so each
# platform lands in its own: x86_64 for the Windows editor and standalone player, Android/
# arm64-v8a for a headset build, Linux/x86_64 and macOS for the other two desktops (editor and
# player alike). A x86_64 Android build stages nothing - the package ships one Android ABI, which
# is the one every current standalone headset runs.
if(WIN32)
  set(BWA_UNITY_PLUGINS "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/x86_64")
  if(EXISTS "${BWA_UNITY_PLUGINS}")
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bw_audio> "${BWA_UNITY_PLUGINS}")
  endif()
elseif(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
  # One library, whatever the SDK state: phonon is linked statically, so an Android phonon build
  # lands INSIDE libbw_audio.so and stages nothing of its own. (This is where a PHONON HOOK used to
  # sit, for the shared libphonon.so that a static link makes moot.)
  set(BWA_UNITY_PLUGINS_ANDROID
      "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/Android/arm64-v8a")
  if(EXISTS "${BWA_UNITY_PLUGINS_ANDROID}")
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bw_audio> "${BWA_UNITY_PLUGINS_ANDROID}")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  # The folder names the ABI, so an arm64 Linux build stages nothing rather than dropping an
  # aarch64 ELF into a folder whose .meta promises x86_64.
  set(BWA_UNITY_PLUGINS_LINUX "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/Linux/x86_64")
  if(EXISTS "${BWA_UNITY_PLUGINS_LINUX}")
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bw_audio> "${BWA_UNITY_PLUGINS_LINUX}")
  endif()
elseif(APPLE)
  # The SHIPPED file is universal (CI builds with CMAKE_OSX_ARCHITECTURES "x86_64;arm64"). A local
  # single-arch build stages here too, which is what makes a Mac developer's own build run in their
  # own editor; the .meta says AnyCPU either way, and only CI's copy reaches a package.
  set(BWA_UNITY_PLUGINS_MACOS "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/macOS")
  if(EXISTS "${BWA_UNITY_PLUGINS_MACOS}")
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bw_audio> "${BWA_UNITY_PLUGINS_MACOS}")
  endif()
endif()

# Windows .exe icon: compile the tool's resource script (examples/icons/<target>.rc -> .ico) into
# the target. The raylib tools name their icon resource GLFW_ICON so the window/taskbar pick it up;
# the rest use ordinal 1 (calib_view loads it for its window class). Regenerate the .ico family
# with examples/icons/gen_icons.py (the .ico files are committed; builds never need Python).
function(bwa_tool_icon target)
  if(WIN32)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples/icons/${target}.rc)
  endif()
endfunction()

# Stage the freshly built bw_audio.dll next to a target's .exe so it runs from the build tree without
# the DLL on PATH. WIN32-only (no-op elsewhere), like bwa_tool_icon. (The separate copy into the Unity
# Plugins dir near the DLL target is deliberately NOT this — that one has its own destination.)
function(bwa_copy_runtime_dll target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bw_audio> $<TARGET_FILE_DIR:${target}>)   # bw_audio.dll next to the exe
  endif()
endfunction()

# Build one with-SDK Steam Audio test: link the phonon archive set + bwa_core, define
# BWA_HAVE_STEAMAUDIO, and register the ctest. Nothing is staged next to the .exe: phonon is static.
# steam_reflect.c's /experimental:c11atomics is already set once in the SDK block above — source
# properties are directory-scoped, so it covers every target here that compiles it.
# Usage: bwa_add_steam_test(<target> <ctest-name> <source>...)
function(bwa_add_steam_test target test_name)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE src "${STEAMAUDIO_DIR}/include")
  target_link_libraries(${target} PRIVATE bwa_core ${BWA_PHONON_LIBS})
  target_compile_definitions(${target} PRIVATE BWA_HAVE_STEAMAUDIO)
  # These tests are pure C and the phonon archive is C++: without the C++ link driver every std::
  # symbol in the archive goes unresolved.
  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
  if(MSVC)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO /IGNORE:4099)   # /GL + no core.pdb; see bw_audio above
  endif()
  add_test(NAME ${test_name} COMMAND ${target})
endfunction()

# ---- console examples (built every build so they can't drift from the ABI) ----
# ---- minimal client example (the whole API lifecycle in one file) ----
add_executable(bwa_minimal examples/minimal.c)
target_link_libraries(bwa_minimal PRIVATE bw_audio)
bwa_tool_icon(bwa_minimal)
bwa_copy_runtime_dll(bwa_minimal)

# ---- feature examples (console, like bwa_minimal): the ambisonic-bed walkthrough (load AmbiX/FuMa,
# rotate/tilt, renderer + max-rE A/B) and the streaming walkthrough (disk streaming + push sources) ----
add_executable(bwa_ambisonic examples/ambisonic.c)
target_link_libraries(bwa_ambisonic PRIVATE bw_audio)
bwa_tool_icon(bwa_ambisonic)
bwa_copy_runtime_dll(bwa_ambisonic)
add_executable(bwa_streaming examples/streaming.c)
target_link_libraries(bwa_streaming PRIVATE bw_audio)
bwa_tool_icon(bwa_streaming)
bwa_copy_runtime_dll(bwa_streaming)
# ---- the convenience tier (shared assets + async, source descs, scene stops) ----
add_executable(bwa_convenience examples/convenience.c)
target_link_libraries(bwa_convenience PRIVATE bw_audio)
bwa_tool_icon(bwa_convenience)
bwa_copy_runtime_dll(bwa_convenience)

# ---- track monitor example (console; prints the live tracked pose) ----
add_executable(bwa_track_monitor examples/track_monitor.c)
target_link_libraries(bwa_track_monitor PRIVATE bw_audio)
bwa_tool_icon(bwa_track_monitor)
bwa_copy_runtime_dll(bwa_track_monitor)

# ---- profiling benches (opt-in; the default build stays lean) ----
# Both are dev/profiling tools, not part of the ABI-drift guard, so they gate behind one flag:
#   cmake -S . -B build -DBWA_BUILD_BENCH=ON     (pair with -DBWA_TRACY=ON for Tracy zones)
# See docs/profiling.md for what each measures.
option(BWA_BUILD_BENCH "profiling benches (bwa_profile_bench + bwa_bench_situations)" OFF)
if(BWA_BUILD_BENCH)
  # ---- headless profiling bench (representative load for Tracy; build with -DBWA_TRACY=ON) ----
  add_executable(bwa_profile_bench examples/profile_bench.c)
  target_link_libraries(bwa_profile_bench PRIVATE bw_audio)
  bwa_tool_icon(bwa_profile_bench)
  bwa_copy_runtime_dll(bwa_profile_bench)

  # ---- per-situation bench: times each configuration synchronously via the manual sink and prints a
  # table (audio-thread cost vs the block budget). Deterministic, no device; -DBWA_TRACY=ON adds zones. ----
  add_executable(bwa_bench_situations examples/bench_situations.c)
  target_link_libraries(bwa_bench_situations PRIVATE bw_audio)
  bwa_copy_runtime_dll(bwa_bench_situations)
endif()

# ---- tests ----
option(BWA_BUILD_TESTS "Build the bw_audio tests" ON)
if(BWA_BUILD_TESTS)
  enable_testing()

  # The console examples double as regression tests. `--tests` forces the offline sink (so CI never
  # depends on an ASIO driver being installed) and cuts the listening time; the CALLS are the same
  # ones a reader sees, which is the point of testing the example rather than a copy of it. An ABI
  # change that still compiles but misbehaves fails here. bwa_convenience additionally CHECKS every
  # claim it prints and exits nonzero on a mismatch.
  foreach(ex bwa_minimal bwa_ambisonic bwa_streaming bwa_convenience)
    add_test(NAME example_${ex} COMMAND ${ex} --tests)
    set_tests_properties(example_${ex} PROPERTIES TIMEOUT 120)
  endforeach()

  # lifecycle smoke against the public ABI / DLL
  # ${BWA_OS_SRC} + src/ on the include path buy exactly one thing: os_sleep_ms. The test itself
  # stays a public-ABI consumer (it links the dll and declares the one internal hook it uses).
  add_executable(test_smoke test/smoke.c ${BWA_OS_SRC})
  target_include_directories(test_smoke PRIVATE src)
  target_link_libraries(test_smoke PRIVATE bw_audio)
  bwa_link_os(test_smoke PRIVATE)
  add_test(NAME smoke COMMAND test_smoke)
  bwa_copy_runtime_dll(test_smoke)

  # The PURE tools API (bwa_spcap_focus_default / bwa_panner_gains_batch): no engine handle, so it
  # is all numeric assertions on the real DLL. Links bwa_core TOO, because the focus default has to
  # be cross-checked against the internal layout_derive_spcap_focus the DLL does not export. That
  # cross-check is why it is not a smoke.c section (smoke.c is the public ABI and nothing else).
  add_executable(test_tools_api test/tools_api_test.c)
  target_link_libraries(test_tools_api PRIVATE bw_audio bwa_core)
  add_test(NAME tools_api COMMAND test_tools_api)
  bwa_copy_runtime_dll(test_tools_api)

  # Deterministic end-to-end golden through the manual sink (bwa_render_block): links the real dll.
  add_executable(test_golden test/golden_test.c)
  target_link_libraries(test_golden PRIVATE bw_audio)
  add_test(NAME golden COMMAND test_golden)
  bwa_copy_runtime_dll(test_golden)

  # audio-loop check: drives the null sink directly (no core needed), plus one section per
  # compiled device backend. Those sections open a live device (a WASAPI endpoint, a JACK server, an
  # ALSA PCM), so they are the tests here that can legitimately be unrunnable; each exits 77 (see
  # SKIP_RETURN_CODE below) rather than pass.
  add_executable(test_audio_sink test/audio_sink_test.c src/sink/sink.c src/sink/sink_quant.c
                 src/sink/null_sink.c src/sink/manual_sink.c ${BWA_OS_SRC})
  target_include_directories(test_audio_sink PRIVATE src include)
  target_compile_options(test_audio_sink PRIVATE ${BWA_C_WARNINGS})
  bwa_link_os(test_audio_sink PRIVATE)
  if(BWA_WITH_WASAPI AND WIN32)
    target_sources(test_audio_sink PRIVATE src/sink/wasapi_sink.cpp)
    target_compile_definitions(test_audio_sink PRIVATE BWA_HAVE_WASAPI)
    target_compile_features(test_audio_sink PRIVATE cxx_std_20)
    target_link_libraries(test_audio_sink PRIVATE ole32)
  endif()
  # Headers only, the same way bw_audio takes them: the test drives the very same run-time loader,
  # which is what lets it prove the UNAVAILABLE path through bwa_sink_dl_override.
  if(BWA_WITH_JACK)
    target_sources(test_audio_sink PRIVATE src/sink/jack_sink.c)
    target_compile_definitions(test_audio_sink PRIVATE BWA_HAVE_JACK)
    target_include_directories(test_audio_sink PRIVATE ${BWA_JACK_INCLUDE_DIRS})
  endif()
  if(BWA_WITH_ALSA)
    target_sources(test_audio_sink PRIVATE src/sink/alsa_sink.c)
    target_compile_definitions(test_audio_sink PRIVATE BWA_HAVE_ALSA)
    target_include_directories(test_audio_sink PRIVATE ${ALSA_INCLUDE_DIRS})
  endif()
  if(BWA_WITH_AAUDIO)
    target_sources(test_audio_sink PRIVATE src/sink/aaudio_sink.c)
    target_compile_definitions(test_audio_sink PRIVATE BWA_HAVE_AAUDIO)
    target_link_libraries(test_audio_sink PRIVATE aaudio)
  endif()
  if(MSVC)   # the stall hook is a C11 atomic
    set_source_files_properties(test/audio_sink_test.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()
  add_test(NAME audio_sink COMMAND test_audio_sink)
  # The device sections open live devices. On a machine with none (a CI runner: no render endpoint,
  # no JACK server, no sound card) each sink fails cleanly and the test exits 77, which ctest
  # reports as SKIPPED — never as a pass.
  set_tests_properties(audio_sink PROPERTIES SKIP_RETURN_CODE 77)

  # The shared bus->device conversion (src/sink/sink_convert.h): header-only, so the test needs nothing
  # but the header. Pure, always runs — the backends that use it cannot be tested on CI.
  add_executable(test_sink_convert test/sink_convert_test.c)
  target_include_directories(test_sink_convert PRIVATE src include)
  target_compile_options(test_sink_convert PRIVATE ${BWA_C_WARNINGS})
  add_test(NAME sink_convert COMMAND test_sink_convert)

  # The fixed-quantum adapter (src/sink/sink_quant.c) every non-ASIO backend renders through. Also pure:
  # it fakes the device side, so the FIFO/timestamp/dropout arithmetic is pinned without hardware.
  add_executable(test_sink_quant test/sink_quant_test.c src/sink/sink_quant.c src/sink/sink.c
                 src/sink/null_sink.c src/sink/manual_sink.c ${BWA_OS_SRC})
  target_include_directories(test_sink_quant PRIVATE src include)
  target_compile_options(test_sink_quant PRIVATE ${BWA_C_WARNINGS})
  bwa_link_os(test_sink_quant PRIVATE)
  add_test(NAME sink_quant COMMAND test_sink_quant)

  # The OS shim itself (src/os/os.h): threads, sleep bounds, the monotonic clock against the C clock,
  # mutex exclusion, and the rewritten pose seqlock under a writer + a reader thread with torn-read
  # detection. Compiles the shim in directly rather than linking the core, so it stays runnable even
  # if something else in the core stops building.
  add_executable(test_os test/os_test.c ${BWA_OS_SRC})
  target_include_directories(test_os PRIVATE src include)
  target_compile_options(test_os PRIVATE ${BWA_C_WARNINGS})
  bwa_link_os(test_os PRIVATE)
  if(MSVC)
    set_source_files_properties(test/os_test.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()
  add_test(NAME os COMMAND test_os)

  # off-RT-path checks that compile the core in (bwa_core PUBLIC includes propagate).
  # The old rt_test monolith is split in two (shared harness in test/rt_test_util.h):
  #   test_rt_core    — the concurrency/lifecycle spine (rings, commit, generation handles,
  #                     voice lifecycle + steal + priority, scheduling, streaming + push,
  #                     pause/seek, fades/groups, limiter, clock/playhead/meter readbacks,
  #                     occlusion level/EQ/directivity — kept there as the generation-gating vehicle)
  #   test_rt_feature — the spatial-feature DSP toggles (panners, spread/extent, decorrelation,
  #                     pathing EQ, air/Doppler/loudness, beds, tracked room EQ, ISM, …)
  add_executable(test_rt_core    test/rt_core_test.c)
  target_link_libraries(test_rt_core PRIVATE bwa_core)
  add_test(NAME rt_core COMMAND test_rt_core)

  add_executable(test_rt_feature test/rt_feature_test.c)
  target_link_libraries(test_rt_feature PRIVATE bwa_core)
  add_test(NAME rt_feature COMMAND test_rt_feature)
  if(MSVC)   # publishes tracked poses through pose.h (the C11-atomics seqlock)
    set_source_files_properties(test/rt_feature_test.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()

  add_executable(test_sound test/sound_test.c)
  target_link_libraries(test_sound PRIVATE bwa_core)
  add_test(NAME sound COMMAND test_sound)

  # The by-path asset cache (bwa_sound_acquire/_release/_acquire_async) against the real DLL.
  # bwa_core comes in TOO, for dr_wav's write API used to synthesize the test files (same reason
  # as test_tools_api links both).
  add_executable(test_assets test/assets_test.c)
  target_link_libraries(test_assets PRIVATE bw_audio bwa_core)
  add_test(NAME assets COMMAND test_assets)
  bwa_copy_runtime_dll(test_assets)

  # Deliberate MISUSE of the public ABI (NULLs, stale/recycled handles, wrong call order, boundary
  # and NaN arguments, cap exhaustion). Links the DLL, because misuse has to hit the real exported
  # entry points and their guards, not the internals.
  add_executable(test_abuse test/abuse_test.c ${BWA_OS_SRC})   # ${BWA_OS_SRC}: os_sleep_ms only
  target_include_directories(test_abuse PRIVATE src)
  target_link_libraries(test_abuse PRIVATE bw_audio)
  bwa_link_os(test_abuse PRIVATE)
  add_test(NAME abuse COMMAND test_abuse)
  bwa_copy_runtime_dll(test_abuse)

  # Fill-then-apply source configuration (bwa_source_preset / _create_desc / _apply / _get_desc).
  # Links the DLL, like test_abuse: the struct_size guard and the stale-handle drop are ABI
  # contracts, so they have to be exercised through the real exported entry points.
  add_executable(test_source_desc test/source_desc_test.c)
  target_link_libraries(test_source_desc PRIVATE bw_audio)
  add_test(NAME source_desc COMMAND test_source_desc)
  bwa_copy_runtime_dll(test_source_desc)

  # Seeded random API call sequences. Deterministic per seed so a failure reproduces from its seed
  # alone; ctest runs a fixed set, and the binary takes --seed/--iters for longer local soaks.
  add_executable(test_fuzz_api test/fuzz_api.c)
  target_link_libraries(test_fuzz_api PRIVATE bw_audio)
  add_test(NAME fuzz_api COMMAND test_fuzz_api)
  bwa_copy_runtime_dll(test_fuzz_api)

  add_executable(test_dsp   test/dsp_test.c)
  target_link_libraries(test_dsp PRIVATE bwa_core)
  add_test(NAME dsp COMMAND test_dsp)

  add_executable(test_monitor test/monitor_test.c)
  target_link_libraries(test_monitor PRIVATE bwa_core)
  add_test(NAME monitor COMMAND test_monitor)

  add_executable(test_natnet test/natnet_test.c)
  target_link_libraries(test_natnet PRIVATE bwa_core)
  add_test(NAME natnet COMMAND test_natnet)
  if(MSVC)   # includes pose.h (the C11-atomics seqlock) directly
    set_source_files_properties(test/natnet_test.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()

  add_executable(test_ambi test/ambi_test.c)
  target_link_libraries(test_ambi PRIVATE bwa_core)
  add_test(NAME ambi COMMAND test_ambi)

  add_executable(test_measure test/measure_test.c)
  target_link_libraries(test_measure PRIVATE bwa_core)
  add_test(NAME measure COMMAND test_measure)

  add_executable(test_stream test/stream_test.c)
  target_link_libraries(test_stream PRIVATE bwa_core)
  add_test(NAME stream COMMAND test_stream)

  # Non-ASCII file paths through all four openers (sound / stream / layout / headphone EQ). The
  # test writes its own fixture through the shim, so it never depends on the bug it pins.
  add_executable(test_utf8_path test/utf8_path_test.c)
  target_link_libraries(test_utf8_path PRIVATE bwa_core)
  add_test(NAME utf8_path COMMAND test_utf8_path)

  # cave_both's two devices: `bwa_desc.device` names the ARRAY, and must not reach the monitor.
  # Two arms run anywhere (the device-free exception, and a foreign device string against whatever
  # stereo device AUTO finds); the ASIO array arm needs real hardware and says so when it cannot run.
  # ${BWA_OS_SRC} + src/ buy os_sleep_ms / os_fopen; the test is otherwise a public-ABI consumer
  # that declares the two internal monitor hooks it uses.
  add_executable(test_cave_both test/cave_both_test.c ${BWA_OS_SRC})
  target_include_directories(test_cave_both PRIVATE src)
  target_link_libraries(test_cave_both PRIVATE bw_audio)
  bwa_link_os(test_cave_both PRIVATE)
  add_test(NAME cave_both COMMAND test_cave_both)
  bwa_copy_runtime_dll(test_cave_both)

  # The two background threads that used to sleep-poll: idle wakeup counts + async-acquire latency.
  # Links bwa_core ONLY - the wakeup counters are file-scope in assets.c and stream.c, so a target
  # that also linked the dll would carry a second copy and read the one the engine never touches.
  add_executable(test_idle test/idle_test.c)
  target_link_libraries(test_idle PRIVATE bwa_core)
  add_test(NAME idle COMMAND test_idle)

  add_executable(test_calib test/calib_test.c)
  target_link_libraries(test_calib PRIVATE bwa_core)
  add_test(NAME calib COMMAND test_calib)

  # Cross-validation against independent implementations (scipy SH / l1-LP VBAP / qhull
  # AllRAD / bilinear RBJ / lfilter goldens in test/xval_data.h, regenerated by
  # tools/xval/gen_reference.py — the test itself is hermetic, no Python needed).
  add_executable(test_xval test/xval_test.c)
  target_link_libraries(test_xval PRIVATE bwa_core)
  add_test(NAME xval COMMAND test_xval)

  # Directional FDN reverb bed: drives fdn_tap directly (like the reflect test does the Steam bed) —
  # decay lands on target, anisotropy is real, output is directional, long runs stay finite.
  add_executable(test_fdn test/fdn_test.c)
  target_link_libraries(test_fdn PRIVATE bwa_core)
  add_test(NAME fdn COMMAND test_fdn)

  # Image-source early reflections: the shoebox mirror geometry (the rt test drives the render).
  add_executable(test_ism test/ism_test.c)
  target_link_libraries(test_ism PRIVATE bwa_core)
  add_test(NAME ism COMMAND test_ism)

  add_executable(test_zylia test/zylia_test.c)
  target_link_libraries(test_zylia PRIVATE bwa_core)
  add_test(NAME zylia COMMAND test_zylia)

  add_executable(test_valid test/valid_test.c)
  target_link_libraries(test_valid PRIVATE bwa_core)
  add_test(NAME valid COMMAND test_valid)

  # ---- with-SDK Steam Audio tests (each compiles the steam objects in + links phonon; see
  # bwa_add_steam_test above for the shared boilerplate) ----
  if(BWA_STEAMAUDIO_FOUND)
    # Directional reflection-bed check: drives steam_reflect_tap directly, asserts the reverb is
    # directional. Needs the alignment patch.
    bwa_add_steam_test(test_reflect reflect
      test/reflect_test.c src/acoustics/steam_scene.c src/acoustics/steam_reflect.c)

    bwa_add_steam_test(test_bake bake
      test/bake_test.c src/acoustics/steam_scene.c src/acoustics/steam_reflect.c)

    # Pathing: routes a source around an occluding wall (visibility bake + pathing sim).
    bwa_add_steam_test(test_path path
      test/path_test.c src/acoustics/steam_scene.c src/acoustics/steam_path.c)

    # Dynamic (instanced) occluder + shared-scene-lock: a movable wall added at runtime blocks the
    # source when slid between it and the listener, clears when moved away/removed — while a reflect
    # bed reads the same scene concurrently (pins Path B + blocker 1).
    bwa_add_steam_test(test_dynmesh dynmesh
      test/dynmesh_test.c src/acoustics/steam_scene.c src/acoustics/steam_reflect.c)

    # HRTF decode (production monitor): the 26->stereo Steam Audio decode runs + preserves laterality.
    bwa_add_steam_test(test_steam_decode steam_decode
      test/steam_decode_test.c src/binaural/steam_decode.c)
  endif()

  # ---- Unity binding: the PlayMode suite, under ctest (opt-in) ----
  #
  # The Unity binding was the one surface nothing in this repo ever EXECUTED: it is C#, it runs
  # inside an editor, and every change to it was compile-verified only. bindings/unity/test~/ holds
  # a PlayMode suite that drives the real components (Engine's per-frame pump and both event drains,
  # Emitter's and AmbisonicBed's completion feeds, the region and seek calls) against the real
  # bw_audio.dll on the offline sink.
  #
  # Opt-in for the same reason BWA_GODOT_EXE is: Unity is not a build dependency and CI has no
  # editor, so a find_program that silently skipped would hide the suite instead of declaring it.
  #   cmake ... -DBWA_UNITY_EXE="C:/.../6000.3.19f1/Editor/Unity.exe"
  #
  # The engine reaches the test project through bindings/unity/Runtime/Plugins/x86_64, which the
  # bw_audio POST_BUILD above stages on every build - so `cmake --build` then `ctest` exercises the
  # DLL that build produced, with no copy step to forget. The driver fails loudly if it is missing.
  set(BWA_UNITY_EXE "" CACHE FILEPATH
      "Unity editor binary; set it to run the Unity PlayMode suite under ctest")
  # A short scratch path on purpose: Unity's Library/PackageCache paths are deep, and this project
  # is disposable - the driver re-stages the manifest and every test source into it each run.
  if(DEFINED ENV{LOCALAPPDATA})
    set(_bwa_uproj "$ENV{LOCALAPPDATA}/Temp/bwa-uproj")
  else()
    set(_bwa_uproj "${CMAKE_BINARY_DIR}/uproj")
  endif()
  set(BWA_UNITY_TEST_PROJECT "${_bwa_uproj}" CACHE PATH
      "Scratch Unity project the PlayMode suite runs in")
  if(BWA_UNITY_EXE)
    # MIN_TESTS is the floor the driver refuses to pass below. It exists because a run that
    # discovered NOTHING is the failure this entry is most likely to have: raise it whenever a test
    # is added, and never lower it to make a run go green.
    add_test(NAME unity_playmode
      COMMAND ${CMAKE_COMMAND} -DUNITY_EXE=${BWA_UNITY_EXE}
              -DREPO=${CMAKE_SOURCE_DIR}
              -DPROJECT_DIR=${BWA_UNITY_TEST_PROJECT}
              -DMIN_TESTS=20
              -P ${CMAKE_SOURCE_DIR}/bindings/unity/test~/run_unity_tests.cmake)
    # The first run resolves packages and imports the project from scratch; later runs reuse its
    # Library cache. RUN_SERIAL because the editor takes an exclusive lock on that project.
    set_tests_properties(unity_playmode PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
  endif()

  # ---- diagnostics (built under BWA_BUILD_TESTS, but NOT registered as ctests: they print reports
  # for eyeballing, with no pass/fail assertion) ----
  # Doppler-quality probe: prints an FFT spectrum of a Doppler-shifted signal, not a ctest.
  add_executable(test_doppler_fft test/doppler_fft.c)
  target_link_libraries(test_doppler_fft PRIVATE bwa_core)

  # Optional: AddressSanitizer build of the sound test (M3 unload-safety / UAF check).
  # AddressSanitizer over the tests most likely to catch a lifetime bug: the sound retire handshake
  # (the original reason this existed), plus the two that deliberately provoke the engine.
  option(BWA_ASAN "Build the lifetime/misuse tests with AddressSanitizer" OFF)
  if(BWA_ASAN AND MSVC)
    foreach(t test_sound test_assets test_abuse test_fuzz_api)   # test_assets: refcounted unload + the loader thread's handoff
      target_compile_options(${t} PRIVATE /fsanitize=address)
      target_link_options(${t} PRIVATE /INCREMENTAL:NO)
    endforeach()
  endif()
endif()

# ---- Shared Dear ImGui stack for the GUI tools (bwa_calib_view, bwa_layout_tool) ----
# One pinned copy of imgui + imgui_test_engine, compiled INTO each tool as target sources (not a
# library: the test-engine define below must be seen by every imgui-including TU of an executable,
# and keeping it per-target makes that explicit). imgui and imgui_test_engine tags track each
# other — bump together. Test engine license note: NOT MIT — free for open source (dual-licensed;
# see its LICENSE.txt) — fine here, revisit if this repo's licensing changes.
# Test-engine integration (pattern from aforren1/lsl-viewer, which worked these out): the hooks
# define must be seen by EVERY imgui-including TU (or the ImGuiContext layout diverges), the
# std::thread coroutine impl must be explicitly selected (else IO.CoroutineFuncs is null -> crash
# in ImGuiTestEngine_Start), and capture must be explicitly enabled (else screenshots are silently
# skipped). imgui_te_imconfig.h would default the latter two but is not auto-included; explicit
# defines are clearer than routing IMGUI_USER_CONFIG through it.
option(BWA_BUILD_PLAYGROUND "Build the interactive raylib playground + the layout tool (fetches raylib/imgui)" OFF)
option(BWA_BUILD_CALIBVIEW "Build the imgui calibration report viewer (fetches imgui/implot/implot3d/test-engine)" OFF)
# The three GUI tools are Windows-only for now: calib_view is on the win32 + d3d11 imgui backend,
# and both raylib tools carry win32 resource scripts and the ASIO capture shells. Porting them is
# not part of the OS shim (docs/backends.md, phase 2) - the LIBRARY is what has to build elsewhere.
if(NOT WIN32 AND (BWA_BUILD_PLAYGROUND OR BWA_BUILD_CALIBVIEW))
  message(STATUS "bw_audio: GUI tools skipped (Windows-only: win32/d3d11 imgui backend, ASIO capture)")
  set(BWA_BUILD_PLAYGROUND OFF)
  set(BWA_BUILD_CALIBVIEW OFF)
endif()
if(BWA_BUILD_PLAYGROUND OR BWA_BUILD_CALIBVIEW)
  enable_language(CXX)
  # SOURCE_SUBDIR points at a non-existent dir: populate sources only, never their own demo builds.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_Declare(imgui_te
    URL https://github.com/ocornut/imgui_test_engine/archive/refs/tags/v1.92.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_MakeAvailable(imgui imgui_te)
  set(BWA_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # imgui_demo.cpp is NOT here: only bwa_calib_view calls ImGui::ShowDemoWindow, so it's added to
    # that target alone (below), next to implot_demo.cpp — keeps it out of the raylib tools' binaries.
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_engine.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_context.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_coroutine.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_exporters.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_perftool.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_ui.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_utils.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_capture_tool.cpp)
  set(BWA_IMGUI_INC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${imgui_te_SOURCE_DIR} ${imgui_te_SOURCE_DIR}/imgui_test_engine)
  set(BWA_IMGUI_DEFS
    IMGUI_ENABLE_TEST_ENGINE
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
    IMGUI_TEST_ENGINE_ENABLE_CAPTURE=1)
endif()

# ---- Optional: interactive playground (evaluate the binaural monitor by ear) ----
# Opt-in so the default build stays lean: cmake -S . -B build -DBWA_BUILD_PLAYGROUND=ON
# Drives the binaural profile live to headphones (auto-picked 2-ch ASIO driver; null-sink
# fallback keeps it rendering visual-only without one). Links the public ABI (bw_audio.h,
# propagated by the bw_audio target) + raylib; uses its own dr_wav copy to generate the wavs.
if(BWA_BUILD_PLAYGROUND)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)        # raylib: skip its own examples/games
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5                                       # release tag (stable); pin to a SHA if desired
    GIT_SHALLOW TRUE)
  # rlImGui — Dear ImGui platform/render backend on raylib (one .cpp). The Raylib_5_5 tag tracks
  # raylib 5.5 + imgui 1.92.x (RendererHasTextures + event-based input, which the test engine needs).
  FetchContent_Declare(rlimgui
    URL https://github.com/raylib-extras/rlImGui/archive/refs/tags/Raylib_5_5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_MakeAvailable(raylib rlimgui)

  # Same UI stack as the layout tool: raylib 3D scene + Dear ImGui control panel via rlImGui, themed
  # like the station (bwa_theme.h), with imgui_test_engine driving the panel under ctest (`--tests` —
  # forced onto the null sink, so the suite also pins the no-device fallback staying LIVE).
  add_executable(bwa_playground examples/playground.cpp ${CJSON_C}   # cJSON: constraints_view.h
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp ${BWA_IMGUI_SOURCES})
  target_include_directories(bwa_playground PRIVATE ${dr_libs_SOURCE_DIR} ${CJSON_INC}
    ${rlimgui_SOURCE_DIR} ${BWA_IMGUI_INC})
  target_compile_definitions(bwa_playground PRIVATE ${BWA_IMGUI_DEFS} NO_FONT_AWESOME STB_IMAGE_WRITE_STATIC)
  target_compile_features(bwa_playground PRIVATE cxx_std_20)      # C++20: the C-style designated inits stay
  target_link_libraries(bwa_playground PRIVATE bw_audio raylib)
  bwa_tool_icon(bwa_playground)
  bwa_copy_runtime_dll(bwa_playground)
  if(BWA_BUILD_TESTS)
    add_test(NAME playground COMMAND bwa_playground --tests)       # the GUI itself, driven by fake inputs
  endif()

  # Speaker-layout authoring tool: place the 26 speakers + identify each by ear (the channel test
  # signal) + export cave_layout.json. The 3D room view (orbit + head view, picking, coverage) is
  # raylib; every control surface is Dear ImGui via rlImGui, themed like the station (bwa_theme.h),
  # with imgui_test_engine driving the panel under ctest (`--tests`). Compiles its own cJSON copy.
  add_executable(bwa_layout_tool examples/layout_tool.cpp ${CJSON_C}
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp ${BWA_IMGUI_SOURCES})
  target_include_directories(bwa_layout_tool PRIVATE src ${CJSON_INC} ${rlimgui_SOURCE_DIR} ${BWA_IMGUI_INC})  # src/: dsp/sos.h (the layout-carried speed of sound)
  # STB_IMAGE_WRITE_STATIC: raylib's rtextures embeds stb_image_write with external linkage, and so
  # does the test engine's capture tool — making the latter's copy static avoids the LNK2005 clash.
  target_compile_definitions(bwa_layout_tool PRIVATE ${BWA_IMGUI_DEFS} NO_FONT_AWESOME STB_IMAGE_WRITE_STATIC)
  target_compile_features(bwa_layout_tool PRIVATE cxx_std_17)      # bwa_theme.h uses inline variables
  target_link_libraries(bwa_layout_tool PRIVATE bw_audio raylib)
  bwa_tool_icon(bwa_layout_tool)
  bwa_copy_runtime_dll(bwa_layout_tool)
  if(BWA_BUILD_TESTS)
    add_test(NAME layout_tool COMMAND bwa_layout_tool --tests)     # the GUI itself, driven by fake inputs
  endif()
endif()

# ---- Optional: calibration report viewer (Dear ImGui pilot: win32 + d3d11) ----
# cmake -S . -B build -DBWA_BUILD_CALIBVIEW=ON
# Inspect what bwa_calibrate wrote before trusting it: per-speaker trims, correction-EQ magnitudes,
# retained IRs, the array in 3D, and a layout DIFF (surveyed vs calibrated / before vs after
# writeback). This is the imgui + implot + implot3d pilot (vs the raylib tools): the win32+d3d11
# backend, and imgui_test_engine drives the GUI with FAKE INPUTS under ctest (`--selftest`) — a GUI
# with an automated regression test. imgui + test engine come from the shared stack above.
if(BWA_BUILD_CALIBVIEW)
  FetchContent_Declare(implot
    URL https://github.com/epezent/implot/archive/refs/tags/v1.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_Declare(implot3d
    URL https://github.com/brenocq/implot3d/archive/refs/tags/v0.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_MakeAvailable(implot implot3d)

  add_executable(bwa_calib_view examples/calib_view.cpp
    ${BWA_IMGUI_SOURCES}
    ${imgui_SOURCE_DIR}/imgui_demo.cpp                # ShowDemoWindow (this tool only; see the shared list)
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
    ${implot3d_SOURCE_DIR}/implot3d.cpp
    ${implot3d_SOURCE_DIR}/implot3d_items.cpp
    ${implot3d_SOURCE_DIR}/implot3d_meshes.cpp)
  target_include_directories(bwa_calib_view PRIVATE
    ${BWA_IMGUI_INC} ${implot_SOURCE_DIR} ${implot3d_SOURCE_DIR})
  target_compile_definitions(bwa_calib_view PRIVATE ${BWA_IMGUI_DEFS})
  target_compile_features(bwa_calib_view PRIVATE cxx_std_17)      # bwa_theme.h uses inline variables
  target_link_libraries(bwa_calib_view PRIVATE bwa_core d3d11 d3dcompiler comdlg32)   # comdlg32: file picker
  bwa_tool_icon(bwa_calib_view)
  # Zylia tab (ZM-1 capture shell) + Capture tab (the calibrate sweep backends) — both live with the
  # ASIO SDK, simulate-only without it. asio_session arbitrates the SDK's one current-driver slot
  # between them (they'd otherwise tear each other down).
  target_sources(bwa_calib_view PRIVATE examples/zylia_capture.cpp examples/calib_capture.cpp
                                       examples/asio_session.cpp)
  if(BWA_WITH_ASIO)
    bwa_add_asio_host(bwa_calib_view)
  endif()
  if(BWA_BUILD_TESTS)
    add_test(NAME calib_view COMMAND bwa_calib_view --tests)      # the GUI itself, driven by fake inputs
  endif()
endif()

# ---- Optional: speaker calibration tool (full-duplex ASIO sweep -> per-speaker delay/gain trims) ----
# cmake -S . -B build -DBWA_BUILD_CALIBRATE=ON ; run with --simulate (no hardware) or on the rig.
option(BWA_BUILD_CALIBRATE "Build the speaker calibration tool" OFF)
# Same rule as the GUI tools: the calibration, validation and Zylia tools all run their own
# full-duplex ASIO host, which is Windows-only.
if(NOT WIN32 AND BWA_BUILD_CALIBRATE)
  message(STATUS "bw_audio: calibration/validation tools skipped (Windows-only: full-duplex ASIO capture)")
  set(BWA_BUILD_CALIBRATE OFF)
endif()
if(BWA_BUILD_CALIBRATE)
  enable_language(CXX)
  add_executable(bwa_calibrate examples/calibrate.cpp examples/calib_capture.cpp examples/asio_session.cpp)
  target_link_libraries(bwa_calibrate PRIVATE bwa_core)
  bwa_tool_icon(bwa_calibrate)
  if(BWA_WITH_ASIO)
    # the tool runs its own full-duplex ASIO host, so it compiles the SDK host sources itself (the
    # bw_audio DLL keeps them internal) — independent of the engine's output-only asio_sink.
    bwa_add_asio_host(bwa_calibrate)
    message(STATUS "bw_audio: calibration tool with ASIO capture")
  else()
    message(STATUS "bw_audio: calibration tool (simulate only; no ASIO SDK)")
  endif()

  # Phantom-localization validation: render a source, measure where the array actually put it.
  # Full-duplex like the sweep above (speaker feeds out + the ZM-1's 19 capsules in, one device),
  # but steady-state, so it needs no latency solve. --simulate runs the whole flow off-hardware.
  add_executable(bwa_validate examples/validate.cpp examples/valid_capture.cpp examples/asio_session.cpp)
  target_link_libraries(bwa_validate PRIVATE bwa_core)
  if(BWA_WITH_ASIO)
    bwa_add_asio_host(bwa_validate)
  endif()
  if(BWA_BUILD_TESTS)
    # The whole session flow, hardware-free. Both backends run the SAME loop, so this covers the
    # capsule check and the exclusion threading the rig will execute — not just the analytic field.
    add_test(NAME validate_sim COMMAND bwa_validate --simulate --azimuths 6 --no-prompt)
    # ...and the fault path: corrupt a capsule in every capture and require it to be caught
    # (bwa_validate exits 3 if the integrity layer misses it).
    add_test(NAME validate_fault COMMAND bwa_validate --simulate --azimuths 6 --inject-fault 7)
    # ...and the tracked-mount path. This asserts the placement hook FIRES, not that the result looks
    # right: in simulate the field is synthesized from the same capsule table the estimator reads, so
    # a wrong table cancels out. The hook was once passed in and never called, which left --track
    # silently inert while announcing that the tracker was measuring. Exits 4 if it regresses.
    add_test(NAME validate_track COMMAND bwa_validate --simulate --track-sim --azimuths 4 --no-prompt)
    # ...and the SPCAP focus sweep: more than one measured condition per panner, which is a different
    # session shape (the condition list replaces the panner list) and a different report. The DSP the
    # sweep reads is pinned in the valid/zylia ctests; this pins that the tool actually walks it.
    add_test(NAME validate_focus
             COMMAND bwa_validate --simulate --focus 4,32 --azimuths 4 --no-prompt
                     --position 0.5,1.5,-0.3)
  endif()

  # ZM-1 hardware bring-up probe: console meter over the shared capture shell (zylia_capture.cpp).
  # The DOA view (clap -> direction on the capsule sphere) lives in bwa_calib_view's Zylia tab.
  add_executable(bwa_zylia_probe examples/zylia_probe.cpp examples/zylia_capture.cpp examples/asio_session.cpp)
  target_link_libraries(bwa_zylia_probe PRIVATE bwa_core)
  bwa_tool_icon(bwa_zylia_probe)
  if(BWA_WITH_ASIO)
    bwa_add_asio_host(bwa_zylia_probe)
  endif()
endif()

# ---- the optional bindings (Python / MATLAB + Octave / Godot) ------------------------------------
# LAST, and through the shared macro in cmake/bwa_bindings.cmake, because SDK MODE at the top of
# this file calls the same macro. One list of bindings, two ways in. Every one of them links
# `bwa::bw_audio`, which is the alias declared beside the target above.
bwa_add_bindings()
