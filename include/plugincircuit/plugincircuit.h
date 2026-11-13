/*
 * libplugincircuit: Plugin Circuit catalog + WCLAP loading for native hosts.
 *
 * A host (DAW, app, a module slot inside a modular synth, ...) uses this to
 *   1. find plugins in a Plugin Circuit catalog (search / plugin info, as JSON),
 *   2. download + verify (sha256) + cache a plugin's bundle,
 *   3. open the bundle and get a regular `clap_plugin_factory`.
 * From there it hosts the WCLAP exactly like any native CLAP plugin (WebAssembly via wclap-bridge/Wasmtime).
 *
 * Threading: every function may block on network or disk (or wasm compilation in plugincircuit_open), so call
 * them from a worker thread, never the audio thread. The returned factory follows the normal CLAP rules.
 * Errors: functions returning bool/pointers fill `error` (if non-null) with a UTF-8 message.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct clap_plugin_factory;

#define PLUGINCIRCUIT_API_VERSION 1

/* Catalog base URL (default http://127.0.0.1:7777 or $PLUGINCIRCUIT_API) and cache root
 * (default ~/Library/Application Support/PluginCircuit or $PLUGINCIRCUIT_HOME). Pass NULL to keep a default.
 * Call once before anything else. */
void plugincircuit_configure(const char *api_base_url, const char *cache_dir);

/* GET /v1/plugins?<query> (see docs/API.md), e.g. "q=reverb&type=audio-effect&native=1".
 * Returns a malloc'd JSON string (free with plugincircuit_free) or NULL. */
char *plugincircuit_search_json(const char *query, char *error, size_t error_capacity);

/* GET /v1/plugins/<id>: PluginSummary JSON (malloc'd) or NULL. */
char *plugincircuit_plugin_json(const char *plugin_id, char *error, size_t error_capacity);

void plugincircuit_free(void *ptr);

/* Progress: return false to cancel. total may be 0 when unknown. */
typedef bool (*plugincircuit_progress_fn)(void *context, uint64_t received, uint64_t total);

/* Makes sure the plugin's bundle is in the local cache (download + sha256 check + extract) and writes the
 * bundle directory to out_dir. `sha256` pins an exact package (as stored in a project); NULL = catalog's
 * current one. Works offline for cached packages when sha256 is given. */
bool plugincircuit_fetch(const char *plugin_id, const char *sha256, plugincircuit_progress_fn progress, void *progress_context,
                     char *out_dir, size_t out_dir_capacity, char *error, size_t error_capacity);

/* Opens a bundle directory (from plugincircuit_fetch, or any local .wclap folder). Compiles the wasm module
 * (can take seconds; cached by Wasmtime afterwards). Returns an opaque handle or NULL. */
typedef struct plugincircuit_module plugincircuit_module;
plugincircuit_module *plugincircuit_open(const char *bundle_dir, char *error, size_t error_capacity);

/* The module's CLAP plugin factory. Valid until plugincircuit_close(). Plugin ids are the WCLAP's own. */
const struct clap_plugin_factory *plugincircuit_factory(plugincircuit_module *module);

/* Destroy every plugin created from the factory first. */
void plugincircuit_close(plugincircuit_module *module);

#ifdef __cplusplus
}
#endif
