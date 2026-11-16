cmake_minimum_required(VERSION 3.28)

set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
project(plugincircuit VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
option(PLUGINCIRCUIT_BUILD_PLUGIN "Build the Plugin Circuit host plugin" ON)

# --- clap-wrapper (VST3 / AUv2 / Standalone around our CLAP) -------------------------
if (PLUGINCIRCUIT_BUILD_PLUGIN)
	set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE)
	set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE)
	FetchContent_Declare(clap-wrapper
		GIT_REPOSITORY https://github.com/free-audio/clap-wrapper
		GIT_TAG v0.12.1
		GIT_SHALLOW ON)
	FetchContent_MakeAvailable(clap-wrapper)
endif()

# --- optional: a locally built Wasmtime C API (iOS has no official release) ------------
# PLUGINCIRCUIT_WASMTIME_LIB=<libwasmtime.a>, PLUGINCIRCUIT_WASMTIME_INCLUDE=<include dir>
set(PLUGINCIRCUIT_WASMTIME_LIB "" CACHE FILEPATH "Use this libwasmtime.a instead of the fetched release")
set(PLUGINCIRCUIT_WASMTIME_INCLUDE "" CACHE PATH "Wasmtime C API headers for PLUGINCIRCUIT_WASMTIME_LIB")
if (PLUGINCIRCUIT_WASMTIME_LIB)
	add_library(wasmtime INTERFACE)
	target_include_directories(wasmtime INTERFACE ${PLUGINCIRCUIT_WASMTIME_INCLUDE})
	target_link_libraries(wasmtime INTERFACE ${PLUGINCIRCUIT_WASMTIME_LIB})
endif()

# --- wclap-bridge (Wasmtime-based WCLAP -> native CLAP) -------------------------------
FetchContent_Declare(wclap-bridge
	# Our fork: upstream WebCLAP/wclap-bridge plus our fixes, one commit each, on the plugin-circuit branch (its
	# webview-gui submodule points at our webview-gui fork). See docs/UPSTREAM.md.
	GIT_REPOSITORY https://github.com/plugin-circuit/wclap-bridge
	GIT_TAG 6f6e08df613d3b1d83bcadd40bf456301f6cebed
	GIT_SUBMODULES_RECURSE ON
	UPDATE_DISCONNECTED ON
	SOURCE_SUBDIR do-not-add) # added manually below, after clap-wrapper
FetchContent_MakeAvailable(wclap-bridge)
add_subdirectory(${wclap-bridge_SOURCE_DIR} wclap-bridge-build EXCLUDE_FROM_ALL)
set(CHOC_DIR ${wclap-bridge_SOURCE_DIR}/modules/choc)

if (PLUGINCIRCUIT_BUILD_PLUGIN)
# --- embedded host UI ----------------------------------------------------------------
include(cmake/embed_resources.cmake)
file(GLOB_RECURSE HOST_UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host-ui/*)
list(FILTER HOST_UI_FILES EXCLUDE REGEX "/(node_modules|\\.DS_Store)")
plugincircuit_embed_resources(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/HostUiResources.cpp
	ROOT ${CMAKE_CURRENT_SOURCE_DIR}/host-ui
	FILES ${HOST_UI_FILES})
endif()

# --- tanh-net (vendored from tanh-lib): platform-native HTTPS + SHA-256 ---------------
set(TANH_NET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tanh-net)
add_library(tanh-net STATIC ${TANH_NET_DIR}/src/HttpClient.cpp ${TANH_NET_DIR}/src/Sha256.cpp)
target_include_directories(tanh-net PUBLIC ${TANH_NET_DIR}/include PRIVATE ${TANH_NET_DIR}/src)
if (APPLE)
	target_sources(tanh-net PRIVATE ${TANH_NET_DIR}/src/HttpClient_apple.mm)
	set_source_files_properties(${TANH_NET_DIR}/src/HttpClient_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	target_link_libraries(tanh-net PRIVATE "-framework Foundation" "-lobjc")
elseif (WIN32)
	target_sources(tanh-net PRIVATE ${TANH_NET_DIR}/src/HttpClient_windows.cpp)
	target_link_libraries(tanh-net PRIVATE winhttp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(CURL REQUIRED)
	target_sources(tanh-net PRIVATE ${TANH_NET_DIR}/src/HttpClient_curl.cpp)
	target_link_libraries(tanh-net PRIVATE CURL::libcurl)
else()
	target_sources(tanh-net PRIVATE ${TANH_NET_DIR}/src/HttpClient_unsupported.cpp)
endif()

# --- core (no GUI): catalog client, bundle cache, inner CLAP host ----------------------
add_library(plugincircuit-core STATIC
	src/core/Sha256.cpp
	src/core/Archive.cpp
	src/core/Http.cpp
	src/core/Catalog.cpp
	src/core/PackageCache.cpp
	src/core/Settings.cpp
	src/core/Paths.cpp
	src/core/WclapLibrary.cpp
	src/core/InnerHost.cpp)
target_include_directories(plugincircuit-core PUBLIC src ${CHOC_DIR} ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
target_link_libraries(plugincircuit-core PUBLIC wclap-bridge tanh-net)
if (APPLE)
	target_link_libraries(plugincircuit-core PUBLIC "-framework Foundation" "-lobjc")
endif()
target_compile_options(plugincircuit-core PRIVATE -Wall -Wno-unused-parameter)
target_compile_definitions(plugincircuit-core PRIVATE PLUGINCIRCUIT_CORE_VERSION="${PROJECT_VERSION}")

if (PLUGINCIRCUIT_BUILD_PLUGIN)

# --- the CLAP plugin implementation (static, wrapped by clap-wrapper) -----------------
add_library(plugincircuit-impl STATIC
	src/plugin/Factory.cpp
	src/plugin/PluginCircuitPlugin.cpp
	src/plugin/Macros.cpp
	src/plugin/StateCodec.cpp
	src/gui/Editor.mm
	${CMAKE_CURRENT_BINARY_DIR}/generated/HostUiResources.cpp)
target_link_libraries(plugincircuit-impl PUBLIC plugincircuit-core clap)
target_include_directories(plugincircuit-impl PRIVATE ${clap-wrapper_SOURCE_DIR}/include)
target_compile_definitions(plugincircuit-impl PUBLIC PLUGINCIRCUIT_VERSION="${PROJECT_VERSION}")
if (APPLE)
	target_link_libraries(plugincircuit-impl PUBLIC "-framework Cocoa" "-framework WebKit" "-framework QuartzCore")
	set_source_files_properties(src/gui/Editor.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

# clap-wrapper v0.12.1 reads a misspelled C1ST_BUNDLE_IDENTIFER for the VST3 and AUv2 bundle ids (make_clapfirst.cmake),
# so they'd be ".vst3" / ".auv2". Set it until that's fixed upstream (docs/UPSTREAM.md).
set(C1ST_BUNDLE_IDENTIFER "studio.plugincircuit.host")
make_clapfirst_plugins(
	TARGET_NAME PluginCircuit
	IMPL_TARGET plugincircuit-impl
	OUTPUT_NAME "PluginCircuit"
	ENTRY_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/Entry.cpp
	BUNDLE_IDENTIFIER "studio.plugincircuit.host"
	BUNDLE_VERSION "${PROJECT_VERSION}"
	PLUGIN_FORMATS CLAP VST3 AUV2
	COPY_AFTER_BUILD FALSE
	ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
	STANDALONE_CONFIGURATIONS
		standalone "PluginCircuit" "studio.plugincircuit.host.synth")

endif()

# --- tools ---------------------------------------------------------------------------
add_executable(plugincircuit-smoke tools/smoke.cpp)
target_link_libraries(plugincircuit-smoke PRIVATE plugincircuit-core)
if (PLUGINCIRCUIT_BUILD_PLUGIN)
	add_executable(plugincircuit-state-test tools/state_roundtrip.cpp)
	target_include_directories(plugincircuit-state-test PRIVATE ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
	add_executable(plugincircuit-latency-test tools/latency_test.cpp)
	target_include_directories(plugincircuit-latency-test PRIVATE ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
endif()

# --- tests (ctest) -------------------------------------------------------------------------------
enable_testing()
add_executable(plugincircuit-editor-layout-test tests/editor_layout_test.cpp)
target_include_directories(plugincircuit-editor-layout-test PRIVATE src)
add_test(NAME editor-layout COMMAND plugincircuit-editor-layout-test)
# host UI: only the catalog's picker iframe may load plugins (headless Chromium, via the website's Playwright)
find_program(BUN_EXECUTABLE bun)
if (BUN_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../website/node_modules/@playwright/test)
	add_test(NAME host-ui-picker-messages COMMAND ${BUN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/host-ui/picker_messages.ts)
endif()
# The WCLAP's tail is read and reported (a bundle dir of a delay or reverb, e.g. OpenRB Delay)
set(PLUGINCIRCUIT_TEST_TAIL_WCLAP "" CACHE PATH "Bundle dir of a WCLAP with a tail (delay/reverb) for the tail test")
if (PLUGINCIRCUIT_TEST_TAIL_WCLAP)
	add_test(NAME inner-tail COMMAND plugincircuit-smoke ${PLUGINCIRCUIT_TEST_TAIL_WCLAP} --json --blocks 50)
	set_tests_properties(inner-tail PROPERTIES PASS_REGULAR_EXPRESSION "\"tail\": *[1-9]")
endif()
add_executable(plugincircuit-dropout-concealer-test tests/dropout_concealer_test.cpp)
target_include_directories(plugincircuit-dropout-concealer-test PRIVATE ${wclap-bridge_SOURCE_DIR}/source)
add_test(NAME dropout-concealer COMMAND plugincircuit-dropout-concealer-test)
add_executable(plugincircuit-main-thread-block-test tests/main_thread_block_test.cpp)
target_link_libraries(plugincircuit-main-thread-block-test PRIVATE plugincircuit-core)
# A single-threaded WCLAP whose webview message does long work (Spiral303's Export WAV renders offline): the audio
# thread must not wait for it. Set to "<bundle-dir>;<pluginId>" to run it.
set(PLUGINCIRCUIT_TEST_BLOCKING_WCLAP "" CACHE STRING "Bundle dir;plugin id;webview message for the main-thread block test")
if (PLUGINCIRCUIT_TEST_BLOCKING_WCLAP)
	add_test(NAME main-thread-block COMMAND plugincircuit-main-thread-block-test ${PLUGINCIRCUIT_TEST_BLOCKING_WCLAP} export --count 3)
endif()
if (PLUGINCIRCUIT_BUILD_PLUGIN AND APPLE)
	# opens real windows: needs a logged-in GUI session (label "gui")
	add_executable(plugincircuit-editor-resize-test tests/editor_resize_test.mm)
	target_include_directories(plugincircuit-editor-resize-test PRIVATE ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
	target_link_libraries(plugincircuit-editor-resize-test PRIVATE "-framework Cocoa" "-framework WebKit")
	set_source_files_properties(tests/editor_resize_test.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
	add_dependencies(plugincircuit-editor-resize-test PluginCircuit_clap)
	# a catalog WCLAP with a GUI to exercise the hosted GUI too (needs it cached or the API running)
	set(PLUGINCIRCUIT_TEST_INNER_PLUGIN "" CACHE STRING "Plugin id for the editor resize test's hosted GUI")
	foreach (mode wrapper vst3 async refuse)
		add_test(NAME editor-resize-${mode}
			COMMAND plugincircuit-editor-resize-test ${CMAKE_BINARY_DIR}/plugins/PluginCircuit.clap ${mode} "${PLUGINCIRCUIT_TEST_INNER_PLUGIN}")
		set_tests_properties(editor-resize-${mode} PROPERTIES LABELS gui RUN_SERIAL TRUE TIMEOUT 60)
	endforeach()
endif()

# --- libplugincircuit: the C SDK for third-party hosts (MVP step 3) -----------------------------
add_library(plugincircuit STATIC src/sdk/PluginCircuitC.cpp)
target_include_directories(plugincircuit PUBLIC include ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
target_link_libraries(plugincircuit PRIVATE plugincircuit-core)
add_executable(plugincircuit-render examples/render.c)
target_link_libraries(plugincircuit-render PRIVATE plugincircuit plugincircuit-core)
