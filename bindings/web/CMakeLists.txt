# The web binding: the engine as an ES module a browser page can load.
#
# ONE target, and it is an emcc link rather than a library: `bw_audio` is STATIC on wasm (the root
# CMakeLists says why), so the JS glue, the worklet bootstrap and the export list are all decided
# HERE, at the final link, and a page gets one self-contained `bw_audio.mjs` plus its `.wasm`.
#
# The only C is `src/bwa_web.c`, and it is a source of THIS target rather than of the library on
# purpose: EMSCRIPTEN_KEEPALIVE keeps an object the link already pulled in, and nothing in the
# engine references a glue function that only JS calls, so an archive member would be dropped.
#
# tools/wasm/build-web.sh is the one caller. It regenerates src/abi.js from the header first, so
# the export list below and the raw layer are the same list by construction.
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "bindings/web needs the Emscripten toolchain: run tools/wasm/build-web.sh")
endif()

set(BWA_WEB_DIST "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "where the web binding is staged")

add_executable(bwa_web src/bwa_web.c)
target_link_libraries(bwa_web PRIVATE bwa::bw_audio)
target_include_directories(bwa_web PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(bwa_web PROPERTIES OUTPUT_NAME "bw_audio" SUFFIX ".mjs")

# LINKER_LANGUAGE CXX when the Steam Audio SDK is in the link, for the reason docs/web.md records:
# bw_audio is STATIC on wasm, so phonon's archive reaches THIS link, and a plain C link driver
# leaves __cxa_throw, operator delete and the libc++ shared-count symbols undefined. The root
# CMakeLists does the same to every executable in ITS directory; this target is in another one, so
# it has to say so itself. Do not try to patch it from the link line - -lc++ -lc++abi picks the
# -noexcept variants and -fexceptions then fails on __cxa_begin_catch. Let emcc choose the driver.
if(BWA_PHONON_LIBS)
  set_target_properties(bwa_web PROPERTIES LINKER_LANGUAGE CXX)
endif()

# MODULARIZE + EXPORT_ES6 so a page and a module Worker load the same file with `import`, and
# --no-entry because this module is a LIBRARY: there is no main(), and the control thread is
# JavaScript's (see bindings/web/src/client.js for the two topologies and why).
#
# The two stack flags are the ones docs/web.md bisected: emcc defaults to 64 KB and the engine
# needs more than 128 KB, on the module's own thread AND on every pthread, because the stream
# refill, the asset loader and the sinks' host-paced threads all render or decode on theirs.
target_link_options(bwa_web PRIVATE
  --no-entry
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBwAudio
  # `node` is in the list beside the two browser environments on purpose: it is what lets the
  # SHIPPED artifact be the TESTED artifact. bindings/web/tests runs the same bw_audio.mjs a page
  # loads, under node, so the raw layer, the idiomatic layer and the frame slab are exercised on
  # every build instead of only by opening a browser. The node paths cost a few kB of glue and are
  # dead in a browser.
  -sENVIRONMENT=web,worker,node
  -pthread -sPTHREAD_POOL_SIZE=8
  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=1048576
  -sEXIT_RUNTIME=0
  # @file: the list is GENERATED from include/bw_audio.h beside src/abi.js, so the raw layer
  # cannot name a call the module does not export and the module cannot carry one the raw layer
  # has never heard of. EXPORTED_FUNCTIONS also forces the archive members in, which a static
  # engine library needs: nothing else references a bwa_* symbol in this link.
  "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/src/exported_functions.json"
  # emscriptenRegisterAudioObject is how a PAGE hands the worklet sink the AudioContext it created
  # inside its own user-gesture handler (worklet_sink.c takes the handle as bwa_desc.device).
  -sEXPORTED_RUNTIME_METHODS=stackSave,stackAlloc,stackRestore,stringToUTF8,lengthBytesUTF8,UTF8ToString,HEAPU8,HEAP32,HEAPU32,HEAPF32,HEAPF64,emscriptenRegisterAudioObject,emscriptenGetAudioObject
)

# STAGING IS THE SCRIPT'S, not a POST_BUILD step. emcc decides how many side files it emits (the
# .wasm always, `bw_audio.aw.js` when AUDIO_WORKLET is on, a worker script on some settings), and a
# CMake copy list that has to be kept in step with that is a copy of the truth. tools/wasm/
# build-web.sh copies whatever landed beside the .mjs plus the binding's own JavaScript, flat and
# under the same relative names the sources use - client.js resolves ./worker.js and ./bw_audio.mjs
# against its own URL. No bundler and no server-side step: a static file server with the two
# isolation headers is the whole hosting requirement.
message(STATUS "bw_audio: web binding -> ${BWA_WEB_DIST} (staged by tools/wasm/build-web.sh)")

# The suite runs against the BUILD tree's module rather than against dist/, so `ctest` right after
# `cmake --build` tests what was just linked and not what a previous staging left behind. It needs
# node, which the emsdk toolchain already put on PATH (emcc runs on it), and it SKIPS rather than
# fails when there is none - the same rule the python binding's pytest suite follows, because a C
# developer should not need a JavaScript runtime to build the engine.
find_program(BWA_NODE NAMES node nodejs)
if(BWA_NODE)
  # The files are NAMED rather than the directory handed over: `node --test <dir>` resolves the
  # path as a single module and dies with MODULE_NOT_FOUND (measured on node 24.14 and 24.19). A
  # glob string would need a shell to expand, which ctest does not give.
  file(GLOB BWA_WEB_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.test.mjs")
  add_test(NAME web_bindings
           COMMAND ${BWA_NODE} --test ${BWA_WEB_TESTS}
           WORKING_DIRECTORY $<TARGET_FILE_DIR:bwa_web>)
  set_tests_properties(web_bindings PROPERTIES
      ENVIRONMENT "BWA_WEB_MODULE=$<TARGET_FILE:bwa_web>;BWA_WEB_SRC=${CMAKE_CURRENT_SOURCE_DIR}/src"
      SKIP_RETURN_CODE 77)
  # The AudioWorklet sink needs a BROWSER: node has no Web Audio, so nothing above touches the one
  # backend this binding exists for. This test drives a self-checking page in headless Chromium and
  # SKIPS (77) when it finds none, so a machine with no browser is not a failing build. It runs
  # against the STAGED dist/ rather than the build tree, because a page loads files by URL and
  # tools/wasm/build-web.sh stages them a moment after the link.
  add_test(NAME web_browser
           COMMAND ${BWA_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-browser.mjs
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(web_browser PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  # The PLAYGROUND page, in the same headless Chromium. web_browser proves the SINK; this proves
  # the demo on top of it - that the page starts on that sink, that the coordinate seam puts a
  # source on the listener's left both on screen and in the ears, and that the channel walk's bus
  # meter lights the channel it drove. It SKIPS (77) with no browser, no dist/ and no vendored
  # three.js, the last of which needs network (tools/wasm/fetch-web-vendor.sh), so a rebuild-free
  # checkout is not a failing build.
  add_test(NAME web_playground
           COMMAND ${BWA_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-playground.mjs
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(web_playground PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
else()
  message(STATUS "bw_audio: web binding tests skipped (no node found)")
endif()
