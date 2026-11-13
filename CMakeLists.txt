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
	GIT_REPOSITORY https://github.com/WebCLAP/wclap-bridge
	GIT_TAG cd11d22afbe2af350f24cd56e6e0536e5ca86452
	GIT_SUBMODULES_RECURSE ON
	# fixes: init the WCLAP before querying its extensions; enable the wasm exceptions proposal
	PATCH_COMMAND sh -c "git apply --reverse --check ${CMAKE_CURRENT_SOURCE_DIR}/patches/wclap-bridge.patch 2>/dev/null || git apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/wclap-bridge.patch"
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
		standalone "PluginCircuit" "studio.plugincircuit.host.instrument")

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

# --- libplugincircuit: the C SDK for third-party hosts (MVP step 3) -----------------------------
add_library(plugincircuit STATIC src/sdk/PluginCircuitC.cpp)
target_include_directories(plugincircuit PUBLIC include ${wclap-bridge_SOURCE_DIR}/modules/clap/include)
target_link_libraries(plugincircuit PRIVATE plugincircuit-core)
add_executable(plugincircuit-render examples/render.c)
target_link_libraries(plugincircuit-render PRIVATE plugincircuit plugincircuit-core)
