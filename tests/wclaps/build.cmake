# Builds the test WCLAPs into <OUT>/dist/<name>.wclap.tar.gz, on every platform (no shell needed):
#   cmake -DCLANG=<clang with wasm32> -DWASM_LD=<wasm-ld> -DCLAP_INCLUDE=<clap/include> -DOUT=<dir> -P build.cmake
# The plugin's CMakeLists runs it; tests/wclaps/build.sh runs it by hand.
foreach(var CLANG WASM_LD CLAP_INCLUDE OUT)
	if(NOT ${var})
		message(FATAL_ERROR "build.cmake: set -D${var}=...")
	endif()
endforeach()
if(NOT EXISTS "${CLAP_INCLUDE}/clap/clap.h")
	message(FATAL_ERROR "CLAP headers not found at ${CLAP_INCLUDE} (configure the plugin first, or set CLAP_INCLUDE)")
endif()
set(SRC "${CMAKE_CURRENT_LIST_DIR}")
set(BUILD "${OUT}/build")
set(DIST "${OUT}/dist")
file(REMOVE_RECURSE "${BUILD}" "${DIST}")
file(MAKE_DIRECTORY "${BUILD}" "${DIST}")

set(CFLAGS --target=wasm32 -ffreestanding -nostdlib -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
	-fno-builtin "-I${CLAP_INCLUDE}" "-I${SRC}/src")
set(LDFLAGS --no-entry --export=clap_entry --export=malloc --export=free --export-table --growable-table
	--stack-first -z stack-size=65536 --strip-debug)

function(run)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
	if(NOT rc EQUAL 0)
		string(REPLACE ";" " " cmd "${ARGN}")
		message(FATAL_ERROR "failed (${rc}): ${cmd}")
	endif()
endfunction()

run("${CLANG}" ${CFLAGS} -c "${SRC}/src/rt.c" -o "${BUILD}/rt.o")

function(build_wclap name define ui)
	set(root "${BUILD}/${name}/${name}.wclap")
	file(MAKE_DIRECTORY "${root}")
	run("${CLANG}" ${CFLAGS} "-D${define}" -c "${SRC}/src/plugin.c" -o "${BUILD}/${name}.o")
	run("${WASM_LD}" ${LDFLAGS} "${BUILD}/${name}.o" "${BUILD}/rt.o" -o "${root}/module.wasm")
	if(ui)
		file(COPY "${SRC}/${ui}/" DESTINATION "${root}/ui")
	endif()
	execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${DIST}/${name}.wclap.tar.gz" "${name}.wclap"
		WORKING_DIRECTORY "${BUILD}/${name}" RESULT_VARIABLE rc)
	if(NOT rc EQUAL 0)
		message(FATAL_ERROR "packing ${name} failed")
	endif()
	file(SIZE "${root}/module.wasm" size)
	message(STATUS "${name}: ${size} bytes  ${DIST}/${name}.wclap.tar.gz")
endfunction()

build_wclap(test-gain         TEST_GAIN         ui/gain)
build_wclap(test-lookahead    TEST_LOOKAHEAD    "")
build_wclap(test-sine         TEST_SINE         "")
build_wclap(test-broken-state TEST_BROKEN_STATE "")
build_wclap(test-param-events TEST_PARAM_EVENTS ui/gain)
build_wclap(test-web-features TEST_WEB_FEATURES ui/features)
