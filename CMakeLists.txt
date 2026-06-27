# bwaudio — self-hosted spatial audio engine for a 26-speaker CAVE array.
# See docs/ for the design; CLAUDE.md for invariants.
#
# Milestones realized so far (docs/roadmap.md):
#   M0 — builds/links a stub library.
#   M1 — device-sink abstraction + audio loop (null sink always; ASIO when the SDK is
#        vendored under third_party/asiosdk/, see third_party/README.md).
#   M2 — concurrency spine in rt.c (SPSC rings, voice table, commit snapshot).
#   M3 — wav playback (sound.c + dr_wav via FetchContent) + the retire-ack handshake.
#   M4 — DBAP spatialization: layout.c (cave_layout.json via cJSON) + dbap.c + align.c.
cmake_minimum_required(VERSION 3.20)
project(bwaudio LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(WARNING "bwaudio targets Windows (ASIO + Win32 audio thread). Non-Windows is unsupported for now.")
endif()

# The codebase uses standard C string functions (strncpy etc.); silence MSVC's
# nonstandard *_s deprecation warnings rather than fork the code for one compiler.
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---- third-party single-header / single-file deps, fetched + pinned (not vendored) ----
# Offline builds can override with -DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>. See third_party/README.md.
include(FetchContent)
FetchContent_Declare(dr_wav
  URL https://raw.githubusercontent.com/mackron/dr_libs/fa931f3285ced10ace628f7f1ac951e1951e7ea6/dr_wav.h
  DOWNLOAD_NO_EXTRACT TRUE)                                   # wav-0.14.5
FetchContent_Declare(cjson_src
  URL https://raw.githubusercontent.com/DaveGamble/cJSON/c859b25da02955fef659d658b8f324b5cde87be3/cJSON.c
  DOWNLOAD_NO_EXTRACT TRUE)                                   # v1.7.19
FetchContent_Declare(cjson_hdr
  URL https://raw.githubusercontent.com/DaveGamble/cJSON/c859b25da02955fef659d658b8f324b5cde87be3/cJSON.h
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(dr_wav cjson_src cjson_hdr)
set(CJSON_C   "${cjson_src_SOURCE_DIR}/cJSON.c")
set(CJSON_INC "${cjson_hdr_SOURCE_DIR}")

# ---- shared core (everything except the public ABI shim and the device sinks) ----
# An OBJECT library so the off-RT-path tests can compile the core in directly and reach
# its internals, without re-listing sources per target.
add_library(bw_core OBJECT
  src/rt.c
  src/sound.c
  src/layout.c
  src/dbap.c
  src/align.c
  src/binaural.c
  ${CJSON_C}
)
target_include_directories(bw_core PUBLIC src ${dr_wav_SOURCE_DIR} ${CJSON_INC})
if(MSVC)
  target_compile_options(bw_core PRIVATE /W4)
  # rt.c uses C11 <stdatomic.h>, gated behind /experimental:c11atomics (directory-scoped,
  # so it applies wherever rt.c is compiled). cJSON is third-party: silence its warnings.
  set_source_files_properties(src/rt.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  set_source_files_properties(${CJSON_C} PROPERTIES COMPILE_OPTIONS /W0)
endif()

# ---- core library (public C ABI in include/bwaudio.h) ----
add_library(bwaudio SHARED
  src/engine.c
  src/sink.c
  src/null_sink.c
  $<TARGET_OBJECTS:bw_core>
)
target_include_directories(bwaudio PUBLIC include PRIVATE src ${dr_wav_SOURCE_DIR} ${CJSON_INC})
target_compile_definitions(bwaudio PRIVATE BW_BUILD_DLL)
if(MSVC)
  target_compile_options(bwaudio PRIVATE /W4)
endif()
if(WIN32)
  target_link_libraries(bwaudio PRIVATE winmm)   # timeBeginPeriod in null_sink.c
endif()

# ---- optional ASIO backend (production: 26ch -> DVS -> Dante) ----
# Auto-enabled when the SDK is present; the SDK is GPLv3/proprietary dual-licensed
# and is NOT committed to this repo (third_party/README.md explains how to fetch it).
set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/asiosdk" CACHE PATH "Steinberg ASIO SDK root")
option(BWAUDIO_WITH_ASIO "Build the ASIO backend (needs the Steinberg ASIO SDK)" OFF)
if(EXISTS "${ASIO_SDK_DIR}/common/asio.h")
  set(BWAUDIO_WITH_ASIO ON)
endif()

if(BWAUDIO_WITH_ASIO)
  if(NOT EXISTS "${ASIO_SDK_DIR}/common/asio.h")
    message(FATAL_ERROR "BWAUDIO_WITH_ASIO=ON but no ASIO SDK at ${ASIO_SDK_DIR} (see third_party/README.md)")
  endif()
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 14)
  target_sources(bwaudio PRIVATE
    src/asio_sink.cpp
    ${ASIO_SDK_DIR}/common/asio.cpp
    ${ASIO_SDK_DIR}/host/asiodrivers.cpp
    ${ASIO_SDK_DIR}/host/pc/asiolist.cpp)
  target_include_directories(bwaudio PRIVATE
    ${ASIO_SDK_DIR}/common ${ASIO_SDK_DIR}/host ${ASIO_SDK_DIR}/host/pc)
  target_compile_definitions(bwaudio PRIVATE BW_HAVE_ASIO _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(bwaudio PRIVATE ole32 user32 advapi32)
  message(STATUS "bwaudio: ASIO backend ENABLED (${ASIO_SDK_DIR})")
else()
  message(STATUS "bwaudio: ASIO backend disabled (no SDK at ${ASIO_SDK_DIR}); null sink only")
endif()

# ---- tests ----
option(BWAUDIO_BUILD_TESTS "Build the bwaudio tests" ON)
if(BWAUDIO_BUILD_TESTS)
  enable_testing()

  # lifecycle smoke against the public ABI / DLL
  add_executable(bw_smoke test/smoke.c)
  target_link_libraries(bw_smoke PRIVATE bwaudio)
  add_test(NAME smoke COMMAND bw_smoke)
  set_tests_properties(smoke PROPERTIES ENVIRONMENT "BWAUDIO_SINK=null")   # hermetic: no real device
  if(WIN32)
    add_custom_command(TARGET bw_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_smoke>)           # .dll next to the .exe
  endif()

  # audio-loop check: drives the null sink directly (no core needed)
  add_executable(bw_audio_smoke test/audio_sink_test.c src/sink.c src/null_sink.c)
  target_include_directories(bw_audio_smoke PRIVATE src)
  if(WIN32)
    target_link_libraries(bw_audio_smoke PRIVATE winmm)
  endif()
  add_test(NAME audio_sink COMMAND bw_audio_smoke)

  # off-RT-path checks that compile the core in (bw_core PUBLIC includes propagate)
  add_executable(bw_rt_test    test/rt_test.c)
  target_link_libraries(bw_rt_test PRIVATE bw_core)
  add_test(NAME rt COMMAND bw_rt_test)

  add_executable(bw_sound_test test/sound_test.c)
  target_link_libraries(bw_sound_test PRIVATE bw_core)
  add_test(NAME sound COMMAND bw_sound_test)

  add_executable(bw_dsp_test   test/dsp_test.c)
  target_link_libraries(bw_dsp_test PRIVATE bw_core)
  add_test(NAME dsp COMMAND bw_dsp_test)

  add_executable(bw_monitor_test test/monitor_test.c)
  target_link_libraries(bw_monitor_test PRIVATE bw_core)
  add_test(NAME monitor COMMAND bw_monitor_test)

  # Optional: AddressSanitizer build of the sound test (M3 unload-safety / UAF check).
  option(BWAUDIO_ASAN "Build bw_sound_test with AddressSanitizer" OFF)
  if(BWAUDIO_ASAN AND MSVC)
    target_compile_options(bw_sound_test PRIVATE /fsanitize=address)
    target_link_options(bw_sound_test PRIVATE /INCREMENTAL:NO)
  endif()
endif()

# ---- Optional: interactive raylib playground (evaluate the binaural monitor by ear) ----
# Opt-in so the default build stays lean: cmake -S . -B build -DBWAUDIO_BUILD_PLAYGROUND=ON
# Drives the binaural profile live to headphones (auto-picked 2-ch ASIO driver). Links the
# public ABI (bwaudio.h, propagated by the bwaudio target) + raylib; uses its own dr_wav copy
# to generate the source wav.
option(BWAUDIO_BUILD_PLAYGROUND "Build the interactive raylib playground (fetches raylib)" OFF)
if(BWAUDIO_BUILD_PLAYGROUND)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)        # raylib: skip its own examples/games
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5                                       # release tag (stable); pin to a SHA if desired
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)

  add_executable(bw_playground examples/playground.c)
  target_include_directories(bw_playground PRIVATE ${dr_wav_SOURCE_DIR})
  target_link_libraries(bw_playground PRIVATE bwaudio raylib)
  if(WIN32)
    add_custom_command(TARGET bw_playground POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_playground>)   # bwaudio.dll next to the exe
  endif()
endif()
