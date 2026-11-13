# plugincircuit_embed_resources(OUTPUT <cpp> ROOT <dir> FILES <files...>)
# Generates a .cpp defining `plugincircuit::resources::find(path)` returning {data,size,mime} for each file,
# keyed by its path relative to ROOT with a leading "/". Regenerated when any input changes.

set(_PLUGINCIRCUIT_EMBED_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/embed_resources_run.cmake)

function(plugincircuit_embed_resources)
	cmake_parse_arguments(E "" "OUTPUT;ROOT" "FILES" ${ARGN})
	string(REPLACE ";" "|" files_arg "${E_FILES}")
	add_custom_command(
		OUTPUT ${E_OUTPUT}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${E_OUTPUT} -DROOT=${E_ROOT} "-DFILES=${files_arg}" -P ${_PLUGINCIRCUIT_EMBED_SCRIPT}
		DEPENDS ${E_FILES} ${_PLUGINCIRCUIT_EMBED_SCRIPT}
		COMMENT "Embedding host UI resources"
		VERBATIM)
endfunction()
