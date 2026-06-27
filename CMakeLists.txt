# bwaudio — self-hosted spatial audio engine for a 26-speaker CAVE array.
# See docs/ for the design; CLAUDE.md for invariants.
#
# Milestones realized so far (docs/roadmap.md):
#   M0 — builds/links a stub library.
#   M1 — device-sink abstraction + audio loop: a null (offline) sink always, and the
#        real ASIO backend when the Steinberg ASIO SDK is vendored under
#        third_party/asiosdk/ (see third_party/README.md). DSP is still M2+.
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

# ---- core library (public C ABI in include/bwaudio.h) ----
add_library(bwaudio SHARED
  src/engine.c
  src/sink.c
  src/null_sink.c
)
target_include_directories(bwaudio
  PUBLIC  include
  PRIVATE src)
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
  # Keep the lifecycle test hermetic: force the offline sink so it never grabs a real
  # ASIO device. Drop this env var to exercise the ASIO path on hardware.
  set_tests_properties(smoke PROPERTIES ENVIRONMENT "BWAUDIO_SINK=null")

  # audio-loop check: drives the null sink directly (compiles the sink sources in)
  add_executable(bw_audio_smoke test/audio_sink_test.c src/sink.c src/null_sink.c)
  target_include_directories(bw_audio_smoke PRIVATE src)
  if(WIN32)
    target_link_libraries(bw_audio_smoke PRIVATE winmm)
  endif()
  add_test(NAME audio_sink COMMAND bw_audio_smoke)

  # keep the .dll next to the dll-linked test exe
  if(WIN32)
    add_custom_command(TARGET bw_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_smoke>)
  endif()
endif()
