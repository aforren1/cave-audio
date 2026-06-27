# bwaudio — self-hosted spatial audio engine for a 26-speaker CAVE array.
# See docs/ for the design; CLAUDE.md for invariants. This is M0 scaffolding:
# a stub library that builds/links against include/bwaudio.h. Device (ASIO),
# spatialization (DBAP), and the binaural monitor land in later milestones
# (see docs/roadmap.md) — third_party/ is wired here but not yet required by
# the stub.
cmake_minimum_required(VERSION 3.20)
project(bwaudio LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(WARNING "bwaudio targets Windows (ASIO). Non-Windows builds are stub-only for now.")
endif()

# ---- core library (public C ABI in include/bwaudio.h) ----
add_library(bwaudio SHARED
  src/engine.c
)
target_include_directories(bwaudio PUBLIC include)
# BW_BUILD_DLL is PRIVATE: the library exports (dllexport); consumers import (dllimport).
target_compile_definitions(bwaudio PRIVATE BW_BUILD_DLL)

if(MSVC)
  target_compile_options(bwaudio PRIVATE /W4)
endif()

# ---- smoke test: exercises the full lifecycle against the stub ----
option(BWAUDIO_BUILD_TESTS "Build the bwaudio smoke test" ON)
if(BWAUDIO_BUILD_TESTS)
  enable_testing()
  add_executable(bw_smoke test/smoke.c)
  target_link_libraries(bw_smoke PRIVATE bwaudio)
  # Keep the .dll next to the .exe so the test can load it (Windows).
  if(WIN32)
    add_custom_command(TARGET bw_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_smoke>)
  endif()
  add_test(NAME smoke COMMAND bw_smoke)
endif()
