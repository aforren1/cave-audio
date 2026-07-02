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
  src/zylia.c
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
  target_sources(bwaudio PRIVATE src/steam_decode.c src/steam_scene.c src/steam_reflect.c src/steam_path.c)  # HRTF decode + occlusion + reflections + pathing (with-SDK)
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
  add_executable(test_smoke test/smoke.c)
  target_link_libraries(test_smoke PRIVATE bwaudio)
  add_test(NAME smoke COMMAND test_smoke)
  set_tests_properties(smoke PROPERTIES ENVIRONMENT "BWAUDIO_SINK=null")   # hermetic: no real device
  if(WIN32)
    add_custom_command(TARGET test_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:test_smoke>)           # .dll next to the .exe
  endif()

  # audio-loop check: drives the null sink directly (no core needed)
  add_executable(test_audio_sink test/audio_sink_test.c src/sink.c src/null_sink.c)
  target_include_directories(test_audio_sink PRIVATE src)
  if(WIN32)
    target_link_libraries(test_audio_sink PRIVATE winmm)
  endif()
  add_test(NAME audio_sink COMMAND test_audio_sink)

  # off-RT-path checks that compile the core in (bw_core PUBLIC includes propagate)
  add_executable(test_rt    test/rt_test.c)
  target_link_libraries(test_rt PRIVATE bw_core)
  add_test(NAME rt COMMAND test_rt)

  # Doppler-quality probe (diagnostic; prints an FFT spectrum report, not a ctest)
  add_executable(test_doppler_fft test/doppler_fft.c)
  target_link_libraries(test_doppler_fft PRIVATE bw_core)

  add_executable(test_sound test/sound_test.c)
  target_link_libraries(test_sound PRIVATE bw_core)
  add_test(NAME sound COMMAND test_sound)

  add_executable(test_dsp   test/dsp_test.c)
  target_link_libraries(test_dsp PRIVATE bw_core)
  add_test(NAME dsp COMMAND test_dsp)

  add_executable(test_monitor test/monitor_test.c)
  target_link_libraries(test_monitor PRIVATE bw_core)
  add_test(NAME monitor COMMAND test_monitor)

  add_executable(test_natnet test/natnet_test.c)
  target_link_libraries(test_natnet PRIVATE bw_core)
  add_test(NAME natnet COMMAND test_natnet)

  add_executable(test_ambi test/ambi_test.c)
  target_link_libraries(test_ambi PRIVATE bw_core)
  add_test(NAME ambi COMMAND test_ambi)

  add_executable(test_measure test/measure_test.c)
  target_link_libraries(test_measure PRIVATE bw_core)
  add_test(NAME measure COMMAND test_measure)

  add_executable(test_stream test/stream_test.c)
  target_link_libraries(test_stream PRIVATE bw_core)
  add_test(NAME stream COMMAND test_stream)

  add_executable(test_calib test/calib_test.c)
  target_link_libraries(test_calib PRIVATE bw_core)
  add_test(NAME calib COMMAND test_calib)

  add_executable(test_zylia test/zylia_test.c)
  target_link_libraries(test_zylia PRIVATE bw_core)
  add_test(NAME zylia COMMAND test_zylia)

  # Directional reflection-bed check (with-SDK only): compiles the steam objects in + links phonon,
  # drives steam_reflect_tap directly, asserts the reverb is directional. Needs the alignment patch.
  if(BW_STEAMAUDIO_FOUND)
    add_executable(test_reflect test/reflect_test.c src/steam_scene.c src/steam_reflect.c)
    target_include_directories(test_reflect PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(test_reflect PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(test_reflect PRIVATE BW_HAVE_STEAMAUDIO)
    if(MSVC)   # steam_reflect.c's seqlock uses C11 <stdatomic.h>
      set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
    endif()
    if(WIN32)
      add_custom_command(TARGET test_reflect POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:test_reflect>)
    endif()
    add_test(NAME reflect COMMAND test_reflect)

    add_executable(test_bake test/bake_test.c src/steam_scene.c src/steam_reflect.c)
    target_include_directories(test_bake PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(test_bake PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(test_bake PRIVATE BW_HAVE_STEAMAUDIO)
    if(MSVC)
      set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
    endif()
    if(WIN32)
      add_custom_command(TARGET test_bake POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:test_bake>)
    endif()
    add_test(NAME bake COMMAND test_bake)

    # Pathing: routes a source around an occluding wall (visibility bake + pathing sim).
    add_executable(test_path test/path_test.c src/steam_scene.c src/steam_path.c)
    target_include_directories(test_path PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(test_path PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(test_path PRIVATE BW_HAVE_STEAMAUDIO)
    if(WIN32)
      add_custom_command(TARGET test_path POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:test_path>)
    endif()
    add_test(NAME path COMMAND test_path)

    # HRTF decode (production monitor): the 26->stereo Steam Audio decode runs + preserves laterality.
    add_executable(test_steam_decode test/steam_decode_test.c src/steam_decode.c)
    target_include_directories(test_steam_decode PRIVATE src "${STEAMAUDIO_DIR}/include")
    target_link_libraries(test_steam_decode PRIVATE bw_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
    target_compile_definitions(test_steam_decode PRIVATE BW_HAVE_STEAMAUDIO)
    if(WIN32)
      add_custom_command(TARGET test_steam_decode POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:test_steam_decode>)
    endif()
    add_test(NAME steam_decode COMMAND test_steam_decode)
  endif()

  # Optional: AddressSanitizer build of the sound test (M3 unload-safety / UAF check).
  option(BWAUDIO_ASAN "Build test_sound with AddressSanitizer" OFF)
  if(BWAUDIO_ASAN AND MSVC)
    target_compile_options(test_sound PRIVATE /fsanitize=address)
    target_link_options(test_sound PRIVATE /INCREMENTAL:NO)
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

  # raygui — single-header immediate-mode GUI (header-only). SOURCE_SUBDIR points at a non-existent dir
  # so MakeAvailable populates the source without building raygui's own CMake examples.
  FetchContent_Declare(raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR  not_built)
  FetchContent_MakeAvailable(raygui)

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
  target_include_directories(bw_layout_tool PRIVATE ${CJSON_INC} ${raygui_SOURCE_DIR}/src)
  target_link_libraries(bw_layout_tool PRIVATE bwaudio raylib)
  if(WIN32)
    add_custom_command(TARGET bw_layout_tool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:bwaudio> $<TARGET_FILE_DIR:bw_layout_tool>)
  endif()
endif()

# ---- Optional: calibration report viewer (Dear ImGui pilot: win32 + d3d11) ----
# cmake -S . -B build -DBWAUDIO_BUILD_CALIBVIEW=ON
# Inspect what bw_calibrate wrote before trusting it: per-speaker trims, correction-EQ magnitudes,
# retained IRs, the array in 3D, and a layout DIFF (surveyed vs calibrated / before vs after
# writeback). This is the imgui + implot + implot3d pilot (vs the raylib tools): the win32+d3d11
# backend, and imgui_test_engine drives the GUI with FAKE INPUTS under ctest (`--selftest`) — a GUI
# with an automated regression test. Test engine license note: NOT MIT — free for open source
# (dual-licensed; see its LICENSE.txt) — fine here, revisit if this repo's licensing changes.
option(BWAUDIO_BUILD_CALIBVIEW "Build the imgui calibration report viewer (fetches imgui/implot/implot3d/test-engine)" OFF)
if(BWAUDIO_BUILD_CALIBVIEW)
  enable_language(CXX)
  # SOURCE_SUBDIR points at a non-existent dir (raygui trick above): populate sources only, never
  # their own example/demo builds. imgui and imgui_test_engine tags track each other — bump together.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_Declare(implot
    URL https://github.com/epezent/implot/archive/refs/tags/v1.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_Declare(implot3d
    URL https://github.com/brenocq/implot3d/archive/refs/tags/v0.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_Declare(imgui_te
    URL https://github.com/ocornut/imgui_test_engine/archive/refs/tags/v1.92.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR not_built)
  FetchContent_MakeAvailable(imgui implot implot3d imgui_te)

  add_executable(bw_calib_view examples/calib_view.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
    ${implot3d_SOURCE_DIR}/implot3d.cpp
    ${implot3d_SOURCE_DIR}/implot3d_items.cpp
    ${implot3d_SOURCE_DIR}/implot3d_meshes.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_engine.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_context.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_coroutine.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_exporters.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_perftool.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_ui.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_te_utils.cpp
    ${imgui_te_SOURCE_DIR}/imgui_test_engine/imgui_capture_tool.cpp)
  target_include_directories(bw_calib_view PRIVATE
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR} ${implot3d_SOURCE_DIR}
    ${imgui_te_SOURCE_DIR} ${imgui_te_SOURCE_DIR}/imgui_test_engine)
  # The canonical test-engine integration: compile EVERY imgui/implot/te TU with the test engine's
  # imconfig (defines IMGUI_ENABLE_TEST_ENGINE hooks + IMGUI_TEST_ENGINE_ENABLE_CAPTURE) — a plain
  # IMGUI_ENABLE_TEST_ENGINE define is not enough, the capture path silently compiles out without it.
  # The std::thread coroutine impl is the no-dependency option on Windows.
  target_compile_definitions(bw_calib_view PRIVATE
    "IMGUI_USER_CONFIG=\"imgui_te_imconfig.h\""
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1)
  target_link_libraries(bw_calib_view PRIVATE bw_core d3d11 d3dcompiler)
  if(BWAUDIO_BUILD_TESTS)
    add_test(NAME calib_view COMMAND bw_calib_view --selftest)   # the GUI itself, driven by fake inputs
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

  # ZM-1 hardware bring-up probe: input-only ASIO meter, plus a live DOA view (clap -> direction on the
  # capsule sphere) when the playground gate provides raylib. The GUI lives in its own C file because
  # raylib.h and windows.h cannot share a translation unit; --simulate drives the GUI hardware-free.
  add_executable(bw_zylia_probe examples/zylia_probe.cpp)
  target_link_libraries(bw_zylia_probe PRIVATE bw_core)
  if(TARGET raylib)                                   # BWAUDIO_BUILD_PLAYGROUND=ON: add the DOA GUI.
    target_sources(bw_zylia_probe PRIVATE examples/zylia_probe_gui.c)
    target_compile_definitions(bw_zylia_probe PRIVATE BW_HAVE_RAYLIB)
    target_link_libraries(bw_zylia_probe PRIVATE raylib)   # BEFORE user32: raylib defines CloseWindow too,
  endif()                                                  # and the GUI's call must resolve against raylib
  if(BWAUDIO_WITH_ASIO)
    target_sources(bw_zylia_probe PRIVATE
      ${ASIO_SDK_DIR}/common/asio.cpp
      ${ASIO_SDK_DIR}/host/asiodrivers.cpp
      ${ASIO_SDK_DIR}/host/pc/asiolist.cpp)
    target_include_directories(bw_zylia_probe PRIVATE ${ASIO_SDK_DIR}/common ${ASIO_SDK_DIR}/host ${ASIO_SDK_DIR}/host/pc)
    target_compile_definitions(bw_zylia_probe PRIVATE BW_HAVE_ASIO _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(bw_zylia_probe PRIVATE ole32 user32 advapi32)
  endif()
endif()
