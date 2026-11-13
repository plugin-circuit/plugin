# wclap-bridge patches

`wclap-bridge.patch` is applied at fetch time (CMake `FetchContent` `PATCH_COMMAND`) to
WebCLAP/wclap-bridge@cd11d22. Each fix is general, not PluginCircuit-specific, and worth sending upstream:

1. **Initialise the WCLAP before querying its extensions.** `pluginInit()` ran `webviewGui.init()`, which calls
   the WCLAP's `get_extension(webview)`, before the WCLAP's own `init()`. clap-helpers-based plugins (e.g. Tapa)
   treat that as host misbehaviour and abort.
2. **Set `clap_version` and name, vendor, url and version in the host struct handed to the WCLAP.** It was zeroed,
   so plugins that check `clap_version_is_compatible(host->clap_version)` refused to create an instance. That
   includes every JUCE-kit port in open-webclap (ZL, Surge XT, Six Sines, RipplerX, ...).
3. **Enable Wasmtime's exceptions proposal** (`wasmtime_config_wasm_exceptions_set`). Modules built with
   `-fwasm-exceptions` need it. Legacy EH (`try`/`catch`) is still unsupported by Wasmtime; the catalog translates
   it with Binaryen (`scripts/normalize-wclap.sh`).
4. **Only call the WCLAP's `clap.gui` methods after its `create()` succeeded.** `get_size`, `set_size` etc. were
   forwarded even when `create` had failed or not run yet, and clap-helpers aborts on that.
5. **Stream proxies transfer the whole request.** `istream.read` and `ostream.write` were capped at 1 kB per
   call. CLAP allows short transfers, but many plugins write a resource or state in one call and fail
   otherwise. Tapa's webview `get_resource` returned 1 kB of HTML and `false`, which left its GUI blank.
6. **Use a parent project's Wasmtime** (`if(NOT TARGET wasmtime)`), and **`WCLAP_WASMTIME_TARGET`** (e.g.
   `pulley64`) to choose Wasmtime's compile target. Both are needed for the iOS build and the Pulley interpreter
   comparison. `WCLAP_WASMTIME_OPT=none|speed|speed_and_size` sets Cranelift's optimisation level. It doesn't
   rescue OB-Xf: still 12 GB, 475 s, then a crash.

Known bridge limitation, worked around in Plugin Circuit rather than patched: the host `thread-check` extension, when
offered, is called from the bridge's audio-thread instance and traps there (out-of-bounds table access). The inner
host doesn't offer `thread-check` (`src/core/InnerHost.cpp`).

Regenerate the patch after editing the fetched sources:
`cd native/build/_deps/wclap-bridge-src && git diff > ../../../patches/wclap-bridge.patch`
