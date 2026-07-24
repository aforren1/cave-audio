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
cmake_minimum_required(VERSION 3.20)
# No project VERSION here: nothing consumes ${PROJECT_VERSION}, and two version streams own the real
# numbers - the ABI version lives in include/bw_audio.h (BWA_VERSION*, what bwa_get_version returns),
# and the distribution/release version is the git tag (stamped into the Unity package at pack time).
project(bw_audio LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(WARNING "bw_audio targets Windows (ASIO + Win32 audio thread). Non-Windows is unsupported for now.")
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

# ---- shared core (everything except the public ABI shim and the device sinks) ----
# An OBJECT library so the off-RT-path tests can compile the core in directly and reach
# its internals, without re-listing sources per target.
add_library(bwa_core OBJECT
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
  src/epad.c
  src/fdn.c
  src/ism.c
  src/natnet.c
  src/measure.c
  src/calib.c
  src/zylia.c
  src/valid.c
  src/stream.c
  ${CJSON_C}
)
target_include_directories(bwa_core PUBLIC src include ${dr_libs_SOURCE_DIR} ${CJSON_INC})   # include/: frame.h consumes the BWA_ROOM_* basis
target_link_libraries(bwa_core PUBLIC ws2_32)   # natnet.c uses Winsock (propagates to consumers)
if(MSVC)
  target_compile_options(bwa_core PRIVATE /W4)
  # rt.c uses C11 <stdatomic.h>, gated behind /experimental:c11atomics (directory-scoped,
  # so it applies wherever rt.c is compiled). cJSON is third-party: silence its warnings.
  set_source_files_properties(src/rt.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  set_source_files_properties(src/stream.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  set_source_files_properties(src/fdn.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)   # the live wet-gain atomic
  set_source_files_properties(${CJSON_C} PROPERTIES COMPILE_OPTIONS /W0)
endif()

# ---- core library (public C ABI in include/bw_audio.h) ----
add_library(bw_audio SHARED
  src/engine.c
  src/sink.c
  src/null_sink.c
  src/manual_sink.c
  src/profile_self.c            # bwa_prof_reset/report exported unconditionally; accumulation gated by BWA_PROFILE_SELF
  $<TARGET_OBJECTS:bwa_core>
)
target_include_directories(bw_audio PUBLIC include PRIVATE src ${dr_libs_SOURCE_DIR} ${CJSON_INC})
target_compile_definitions(bw_audio PRIVATE BWA_BUILD_DLL)
if(MSVC)
  target_compile_options(bw_audio PRIVATE /W4)
endif()
if(WIN32)
  target_link_libraries(bw_audio PRIVATE winmm ws2_32)   # winmm: timeBeginPeriod (null_sink); ws2_32: natnet
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
# tooling. Routes profile.h's zone macros into src/profile_self.c on both compile groups.
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
if(EXISTS "${ASIO_SDK_DIR}/common/asio.h")
  set(BWA_WITH_ASIO ON)
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
  target_sources(bw_audio PRIVATE src/asio_sink.cpp)   # the engine's own output-only ASIO sink
  bwa_add_asio_host(bw_audio)
  message(STATUS "bw_audio: ASIO backend ENABLED (${ASIO_SDK_DIR})")
else()
  message(STATUS "bw_audio: ASIO backend disabled (no SDK at ${ASIO_SDK_DIR}); null sink only")
endif()

# ---- optional Steam Audio backend (production binaural HRTF decode; later occlusion/reflections) ----
# Apache-2.0 (GPLv3-compatible). Auto-enabled when the prebuilt phonon SDK is vendored at
# third_party/steam-audio-artifacts/ (third_party/README.md). Stage 1 of the production monitor (the SH encode
# in src/ambisonics.c) is always built + tested; stage 2 (the ambisonics->binaural HRTF decode, gated
# BWA_HAVE_STEAMAUDIO) supersedes the first-cut pan in binaural.c and lands with the SDK, on-SDK-verified.
set(STEAMAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party/steam-audio-artifacts" CACHE PATH "Steam Audio (phonon) SDK root")
if(EXISTS "${STEAMAUDIO_DIR}/include/phonon.h")
  set(BWA_STEAMAUDIO_FOUND TRUE)   # consumed by the with-SDK reflection test below
  target_sources(bw_audio PRIVATE src/steam_decode.c src/steam_scene.c src/steam_reflect.c src/steam_path.c)  # HRTF decode + occlusion + reflections + pathing (with-SDK)
  if(MSVC)   # steam_reflect.c uses C11 <stdatomic.h> (the IR-publish seqlock), like rt.c
    set_source_files_properties(src/steam_reflect.c PROPERTIES COMPILE_OPTIONS /experimental:c11atomics)
  endif()
  target_include_directories(bw_audio PRIVATE "${STEAMAUDIO_DIR}/include")
  target_link_libraries(bw_audio PRIVATE "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
  target_compile_definitions(bw_audio PRIVATE BWA_HAVE_STEAMAUDIO)
  if(WIN32)
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:bw_audio>)   # runtime dep
  endif()
  message(STATUS "bw_audio: Steam Audio ENABLED (${STEAMAUDIO_DIR}); binaural uses the HRTF decode")
else()
  message(STATUS "bw_audio: Steam Audio disabled (no SDK at ${STEAMAUDIO_DIR}); binaural uses the first-cut pan")
endif()

# ---- stage runtime DLLs into the Unity package (bindings/unity/Runtime/Plugins/x86_64) ----
# Gitignored build artifacts; the UPM package picks them up so it "just works" after a build.
if(WIN32)
  set(BWA_UNITY_PLUGINS "${CMAKE_SOURCE_DIR}/bindings/unity/Runtime/Plugins/x86_64")
  if(EXISTS "${BWA_UNITY_PLUGINS}")
    add_custom_command(TARGET bw_audio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bw_audio> "${BWA_UNITY_PLUGINS}")
    if(BWA_STEAMAUDIO_FOUND)
      add_custom_command(TARGET bw_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" "${BWA_UNITY_PLUGINS}")
    endif()
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

# Build one with-SDK Steam Audio test: link phonon + bwa_core, define BWA_HAVE_STEAMAUDIO, stage
# phonon.dll next to the .exe, and register the ctest. steam_reflect.c's /experimental:c11atomics is
# already set once in the SDK block above — source properties are directory-scoped, so it covers every
# target here that compiles it.
# Usage: bwa_add_steam_test(<target> <ctest-name> <source>...)
function(bwa_add_steam_test target test_name)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE src "${STEAMAUDIO_DIR}/include")
  target_link_libraries(${target} PRIVATE bwa_core "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.lib")
  target_compile_definitions(${target} PRIVATE BWA_HAVE_STEAMAUDIO)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${STEAMAUDIO_DIR}/lib/windows-x64/phonon.dll" $<TARGET_FILE_DIR:${target}>)
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

  # lifecycle smoke against the public ABI / DLL
  add_executable(test_smoke test/smoke.c)
  target_link_libraries(test_smoke PRIVATE bw_audio)
  add_test(NAME smoke COMMAND test_smoke)
  bwa_copy_runtime_dll(test_smoke)

  # Deterministic end-to-end golden through the manual sink (bwa_render_block): links the real dll.
  add_executable(test_golden test/golden_test.c)
  target_link_libraries(test_golden PRIVATE bw_audio)
  add_test(NAME golden COMMAND test_golden)
  bwa_copy_runtime_dll(test_golden)

  # audio-loop check: drives the null sink directly (no core needed)
  add_executable(test_audio_sink test/audio_sink_test.c src/sink.c src/null_sink.c src/manual_sink.c)
  target_include_directories(test_audio_sink PRIVATE src)
  if(WIN32)
    target_link_libraries(test_audio_sink PRIVATE winmm)
  endif()
  add_test(NAME audio_sink COMMAND test_audio_sink)

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

  add_executable(test_sound test/sound_test.c)
  target_link_libraries(test_sound PRIVATE bwa_core)
  add_test(NAME sound COMMAND test_sound)

  add_executable(test_dsp   test/dsp_test.c)
  target_link_libraries(test_dsp PRIVATE bwa_core)
  add_test(NAME dsp COMMAND test_dsp)

  add_executable(test_monitor test/monitor_test.c)
  target_link_libraries(test_monitor PRIVATE bwa_core)
  add_test(NAME monitor COMMAND test_monitor)

  add_executable(test_natnet test/natnet_test.c)
  target_link_libraries(test_natnet PRIVATE bwa_core)
  add_test(NAME natnet COMMAND test_natnet)

  add_executable(test_ambi test/ambi_test.c)
  target_link_libraries(test_ambi PRIVATE bwa_core)
  add_test(NAME ambi COMMAND test_ambi)

  add_executable(test_measure test/measure_test.c)
  target_link_libraries(test_measure PRIVATE bwa_core)
  add_test(NAME measure COMMAND test_measure)

  add_executable(test_stream test/stream_test.c)
  target_link_libraries(test_stream PRIVATE bwa_core)
  add_test(NAME stream COMMAND test_stream)

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
      test/reflect_test.c src/steam_scene.c src/steam_reflect.c)

    bwa_add_steam_test(test_bake bake
      test/bake_test.c src/steam_scene.c src/steam_reflect.c)

    # Pathing: routes a source around an occluding wall (visibility bake + pathing sim).
    bwa_add_steam_test(test_path path
      test/path_test.c src/steam_scene.c src/steam_path.c)

    # Dynamic (instanced) occluder + shared-scene-lock: a movable wall added at runtime blocks the
    # source when slid between it and the listener, clears when moved away/removed — while a reflect
    # bed reads the same scene concurrently (pins Path B + blocker 1).
    bwa_add_steam_test(test_dynmesh dynmesh
      test/dynmesh_test.c src/steam_scene.c src/steam_reflect.c)

    # HRTF decode (production monitor): the 26->stereo Steam Audio decode runs + preserves laterality.
    bwa_add_steam_test(test_steam_decode steam_decode
      test/steam_decode_test.c src/steam_decode.c)
  endif()

  # ---- diagnostics (built under BWA_BUILD_TESTS, but NOT registered as ctests: they print reports
  # for eyeballing, with no pass/fail assertion) ----
  # Doppler-quality probe: prints an FFT spectrum of a Doppler-shifted signal, not a ctest.
  add_executable(test_doppler_fft test/doppler_fft.c)
  target_link_libraries(test_doppler_fft PRIVATE bwa_core)

  # Optional: AddressSanitizer build of the sound test (M3 unload-safety / UAF check).
  option(BWA_ASAN "Build test_sound with AddressSanitizer" OFF)
  if(BWA_ASAN AND MSVC)
    target_compile_options(test_sound PRIVATE /fsanitize=address)
    target_link_options(test_sound PRIVATE /INCREMENTAL:NO)
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

# ---- Optional: Godot GDExtension binding ----
# Opt-in (it fetches godot-cpp, whose generated bindings are a multi-minute first build):
# cmake -S . -B build -DBWA_BUILD_GODOT=ON. See bindings/godot/CMakeLists.txt.
option(BWA_BUILD_GODOT "Build the Godot GDExtension binding (fetches godot-cpp)" OFF)
if(BWA_BUILD_GODOT)
  enable_language(CXX)
  add_subdirectory(bindings/godot)
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
  target_include_directories(bwa_layout_tool PRIVATE ${CJSON_INC} ${rlimgui_SOURCE_DIR} ${BWA_IMGUI_INC})
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
