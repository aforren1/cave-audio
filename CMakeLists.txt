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
# dr_libs (dr_wav / dr_flac / dr_mp3): one pinned snapshot of the whole header-only repo, so the
# commit lives in exactly ONE place. The gzipped tarball (~220 KB) is smaller than the raw headers,
# and CMake strips the single top-level dir, so ${dr_libs_SOURCE_DIR} holds the headers directly.
FetchContent_Declare(dr_libs
  URL https://github.com/mackron/dr_libs/archive/fa931f3285ced10ace628f7f1ac951e1951e7ea6.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)                            # wav-0.14.5 / flac-0.13.3 / mp3-0.7.3, PD or MIT-0
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
# cmake -S . -B build -DBWAUDIO_TRACY=ON, then connect the standalone Tracy profiler to the running
# engine. ON_DEMAND => the client only collects while a profiler is attached (near-zero idle cost).
option(BWAUDIO_TRACY "Instrument with the Tracy profiler (fetches Tracy)" OFF)
if(BWAUDIO_TRACY)
  set(TRACY_ENABLE    ON CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(tracy)
endif()

# ---- shared core (everything except the public ABI shim and the device sinks) ----
# An OBJECT library so the off-RT-path tests can compile the core in directly and reach
# its internals, without re-listing sources per target.
add_library(bw_core OBJECT
  src/rt.c
  src/sound.c
  src/layout.c
  src/dbap.c
  src/spcap.c
  src/vbap.c
  src/hull.c
  src/align.c
  src/binaural.c
  src/ambisonics.c
  src/allrad.c
  src/natnet.c
  src/measure.c
  src/calib.c
  src/stream.c
  ${CJSON_C}
)
target_include_directories(bw_core PUBLIC src ${dr_libs_SOURCE_DIR} ${CJSON_INC})
target_link_libraries(bw_core PUBLIC ws2_32)   # natnet.c uses Winsock (propagates to consumers)
if(MSVC)
  target_compile_options(bw_core PRIVATE /W4)
  # rt.c uses C11 <stdatomic.h>, gated behind /experimental:c11atomics (directory-scoped,
  # so it applies wherever rt.c is compiled). cJSON is third-party: silence its warnings.
  set_source_files_properties(src/rt.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  set_source_files_properties(src/stream.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  set_source_files_properties(${CJSON_C} PROPERTIES COMPILE_OPTIONS /W0)
endif()

# ---- core library (public C ABI in include/bwaudio.h) ----
add_library(bwaudio SHARED
  src/engine.c
  src/sink.c
  src/null_sink.c
  $<TARGET_OBJECTS:bw_core>
)
target_include_directories(bwaudio PUBLIC include PRIVATE src ${dr_libs_SOURCE_DIR} ${CJSON_INC})
target_compile_definitions(bwaudio PRIVATE BW_BUILD_DLL)
if(MSVC)
  target_compile_options(bwaudio PRIVATE /W4)
endif()
if(WIN32)
  target_link_libraries(bwaudio PRIVATE winmm ws2_32)   # winmm: timeBeginPeriod (null_sink); ws2_32: natnet
endif()

if(BWAUDIO_TRACY)
  # bw_core is an OBJECT lib: linking TracyClient gives rt.c Tracy's headers (no link step here); the
  # DLL links the actual client. BW_TRACY flips profile.h's macros on for both compile groups.
  target_link_libraries(bw_core PRIVATE TracyClient)
  target_compile_definitions(bw_core PRIVATE BW_TRACY)
  target_link_libraries(bwaudio PRIVATE TracyClient)
  target_compile_definitions(bwaudio PRIVATE BW_TRACY)
  message(STATUS "bwaudio: Tracy profiling ENABLED")
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

# ---- optional Steam Audio backend (production binaural HRTF decode; later occlusion/reflections) ----
# Apache-2.0 (GPLv3-compatible). Auto-enabled when the prebuilt phonon SDK is vendored at
# third_party/steamaudio/ (third_party/README.md). Stage 1 of the production monitor (the SH encode
# in src/ambisonics.c) is always built + tested; stage 2 (the ambisonics->binaural HRTF decode, gated
# BW_HAVE_STEAMAUDIO) supersedes the first-cut pan in binaural.c and lands with the SDK, on-SDK-verified.
set(STEAMAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party/steamaudio" CACHE PATH "Steam Audio (phonon) SDK root")
if(EXISTS "${STEAMAUDIO_DIR}/include/phonon.h")
  set(BW_STEAMAUDIO_FOUND TRUE)   # consumed by the with-SDK reflection test below
  target_sources(bwaudio PRIVATE src/steam_decode.c src/steam_scene.c src/steam_reflect.c)  # HRTF decode + occlusion + reflections (with-SDK)
  if(MSVC)   # steam_reflect.c uses C11 <stdatomic.h> (the IR-publish seqlock), like rt.c
    set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()
  target_include_directories(bwaudio PRIVATE "${STEAMAUDIO_DIR}/include")
  target_link_libraries(bwaudio PRIVATE "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
  target_compile_definitions(bwaudio PRIVATE BW_HAVE_STEAMAUDIO)
  if(WIN32)
    add_custom_command(TARGET bwaudio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:bwaudio>)   # runtime dep
  endif()
  message(STATUS "bwaudio: Steam Audio ENABLED (${STEAMAUDIO_DIR}); binaural uses the HRTF decode")
else()
  message(STATUS "bwaudio: Steam Audio disabled (no SDK at ${STEAMAUDIO_DIR}); binaural uses the first-cut pan")
endif()

# ---- stage runtime DLLs into the Unity package (bindings/unity/Runtime/Plugins/x86_64) ----
# Gitignored build artifacts; the UPM package picks them up so it "just works" after a build.
if(WIN32)
  set(BW_UNITY_PLUGINS "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/x86_64")
  if(EXISTS "${BW_UNITY_PLUGINS}")
    add_custom_command(TARGET bwaudio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bwaudio> "${BW_UNITY_PLUGINS}")
    if(BW_STEAMAUDIO_FOUND)
      add_custom_command(TARGET bwaudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" "${BW_UNITY_PLUGINS}")
    endif()
  endif()
endif()

# ---- tests ----
# ---- track monitor example (console; prints the live track_internal pose) ----
add_executable(bw_track_monitor examples/track_monitor.c)
target_link_libraries(bw_track_monitor PRIVATE bwaudio)
if(WIN32)
  add_custom_command(TARGET bw_track_monitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_track_monitor>)   # bwaudio.dll next to the exe
endif()

# ---- headless profiling bench (representative load for Tracy; build with -DBWAUDIO_TRACY=ON) ----
add_executable(bw_profile_bench examples/profile_bench.c)
target_link_libraries(bw_profile_bench PRIVATE bwaudio)
if(WIN32)
  add_custom_command(TARGET bw_profile_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_profile_bench>)
endif()

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

  # Doppler-quality probe (diagnostic; prints an FFT spectrum report, not a ctest)
  add_executable(bw_doppler_fft test/doppler_fft.c)
  target_link_libraries(bw_doppler_fft PRIVATE bw_core)

  add_executable(bw_sound_test test/sound_test.c)
  target_link_libraries(bw_sound_test PRIVATE bw_core)
  add_test(NAME sound COMMAND bw_sound_test)

  add_executable(bw_dsp_test   test/dsp_test.c)
  target_link_libraries(bw_dsp_test PRIVATE bw_core)
  add_test(NAME dsp COMMAND bw_dsp_test)

  add_executable(bw_monitor_test test/monitor_test.c)
  target_link_libraries(bw_monitor_test PRIVATE bw_core)
  add_test(NAME monitor COMMAND bw_monitor_test)

  add_executable(bw_natnet_test test/natnet_test.c)
  target_link_libraries(bw_natnet_test PRIVATE bw_core)
  add_test(NAME natnet COMMAND bw_natnet_test)

  add_executable(bw_ambi_test test/ambi_test.c)
  target_link_libraries(bw_ambi_test PRIVATE bw_core)
  add_test(NAME ambi COMMAND bw_ambi_test)

  add_executable(bw_measure_test test/measure_test.c)
  target_link_libraries(bw_measure_test PRIVATE bw_core)
  add_test(NAME measure COMMAND bw_measure_test)

  add_executable(bw_stream_test test/stream_test.c)
  target_link_libraries(bw_stream_test PRIVATE bw_core)
  add_test(NAME stream COMMAND bw_stream_test)

  add_executable(bw_calib_test test/calib_test.c)
  target_link_libraries(bw_calib_test PRIVATE bw_core)
  add_test(NAME calib COMMAND bw_calib_test)

  # Directional reflection-bed check (with-SDK only): compiles the steam objects in + links phonon,
  # drives steam_reflect_tap directly, asserts the reverb is directional. Needs the alignment patch.
  if(BW_STEAMAUDIO_FOUND)
    add_executable(bw_reflect_test test/reflect_test.c src/steam_scene.c src/steam_reflect.c)
    target_include_directories(bw_reflect_test PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(bw_reflect_test PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(bw_reflect_test PRIVATE BW_HAVE_STEAMAUDIO)
    if(MSVC)   # steam_reflect.c's seqlock uses C11 <stdatomic.h>
      set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
    endif()
    if(WIN32)
      add_custom_command(TARGET bw_reflect_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:bw_reflect_test>)
    endif()
    add_test(NAME reflect COMMAND bw_reflect_test)

    add_executable(bw_bake_test test/bake_test.c src/steam_scene.c src/steam_reflect.c)
    target_include_directories(bw_bake_test PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(bw_bake_test PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(bw_bake_test PRIVATE BW_HAVE_STEAMAUDIO)
    if(MSVC)
      set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
    endif()
    if(WIN32)
      add_custom_command(TARGET bw_bake_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:bw_bake_test>)
    endif()
    add_test(NAME bake COMMAND bw_bake_test)
  endif()

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
  target_include_directories(bw_playground PRIVATE ${dr_libs_SOURCE_DIR})
  target_link_libraries(bw_playground PRIVATE bwaudio raylib)
  if(WIN32)
    add_custom_command(TARGET bw_playground POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_playground>)   # bwaudio.dll next to the exe
  endif()

  # Speaker-layout authoring tool: place the 26 speakers + identify each by ear (the channel test
  # signal) + export cave_layout.json. Compiles its own cJSON copy to read existing layouts.
  add_executable(bw_layout_tool examples/layout_tool.c ${CJSON_C})
  target_include_directories(bw_layout_tool PRIVATE ${CJSON_INC})
  target_link_libraries(bw_layout_tool PRIVATE bwaudio raylib)
  if(WIN32)
    add_custom_command(TARGET bw_layout_tool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_layout_tool>)
  endif()
endif()

# ---- Optional: speaker calibration tool (full-duplex ASIO sweep -> per-speaker delay/gain trims) ----
# cmake -S . -B build -DBWAUDIO_BUILD_CALIBRATE=ON ; run with --simulate (no hardware) or on the rig.
option(BWAUDIO_BUILD_CALIBRATE "Build the speaker calibration tool" OFF)
if(BWAUDIO_BUILD_CALIBRATE)
  enable_language(CXX)
  add_executable(bw_calibrate examples/calibrate.cpp)
  target_link_libraries(bw_calibrate PRIVATE bw_core)
  if(BWAUDIO_WITH_ASIO)
    # the tool runs its own full-duplex ASIO host, so it compiles the SDK host sources itself (the
    # bwaudio DLL keeps them internal) — independent of the engine's output-only asio_sink.
    target_sources(bw_calibrate PRIVATE
      ${ASIO_SDK_DIR}/common/asio.cpp
      ${ASIO_SDK_DIR}/host/asiodrivers.cpp
      ${ASIO_SDK_DIR}/host/pc/asiolist.cpp)
    target_include_directories(bw_calibrate PRIVATE ${ASIO_SDK_DIR}/common ${ASIO_SDK_DIR}/host ${ASIO_SDK_DIR}/host/pc)
    target_compile_definitions(bw_calibrate PRIVATE BW_HAVE_ASIO _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(bw_calibrate PRIVATE ole32 user32 advapi32)
    message(STATUS "bwaudio: calibration tool with ASIO capture")
  else()
    message(STATUS "bwaudio: calibration tool (simulate only; no ASIO SDK)")
  endif()
endif()
