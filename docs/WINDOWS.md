# Windows: the plugin, the SDK and the WCLAP features we don't support yet

Status: not started. Nothing of Plugin Circuit has ever been built or run on Windows.

Done when: Plugin Circuit (CLAP, VST3 and both standalone apps) installs and runs on Windows 10/11 x64 in at least
three DAWs, with the same behaviour as on macOS (browse, load, GUI, macros, params, keyboard, settings), the C SDK
builds and passes its tests there, CI builds and tests every push on Windows, and the WCLAP features in W5 work on
both platforms.

## Where things are (read first)

Paths are relative to the workspace `~/Code/work/plugin-circuit/` (this file is `plugin/docs/WINDOWS.md`).

| Part | Path | Windows state |
| --- | --- | --- |
| Plugin core (catalog, cache, inner host) | `plugin/src/core/` | mostly portable; see W2 |
| Plugin glue (CLAP plugin, params, gestures, API) | `plugin/src/plugin/`, `plugin/src/api/` | portable C++; never compiled with MSVC |
| Editor | `plugin/src/gui/Editor.mm` (Objective-C++, AppKit) | **macOS only, compiled unconditionally**: the plugin doesn't build on Windows at all |
| Host UI page | `plugin/host-ui/` (HTML/JS, CHOC webview) | portable; WebView2 differences untested |
| C SDK | `plugin/src/sdk/PluginCircuitC.cpp` (today's), `libplugincircuit/` (the planned standalone SDK; only ABI versioning so far, tanh conventions: branch on `TANH_OPERATING_SYSTEM`) | `PluginCircuitC.cpp` uses POSIX `setenv` |
| Bridge | `external/wclap-bridge` (our fork, branch `plugin-circuit`; pinned by `GIT_TAG` in `plugin/CMakeLists.txt`) | upstream builds on Windows; our additions (`realtime-lock.h`, `dropout-concealer.h`) are written portable but never compiled there |
| Webview GUI for WCLAPs | `external/webview-gui` (our fork) | our fixes are in the macOS backend (`apple-osx.h`) only; the CHOC backend (Windows, Linux) lacks them |
| Backlog notes | `plugin/docs/BACKLOG.md`, section "Platforms" | the list W1-W4 starts from |

## W1. CI on Windows first

Without a Windows machine in the loop, CI is how we find out what breaks.
- [ ] GitHub Actions job `windows-latest` (x64) for the plugin: configure, build core + tests, run the non-GUI
      tests (param-table, gestures, api, settings, dropout-concealer, catalog-health, plugin-errors,
      plugin-param-events, main-thread-block with the test WCLAPs).
- [ ] Toolchain: MSVC (VS 2022, the default on the runner) first; clang-cl later if needed. C++20.
- [ ] The test WCLAPs (`plugin/tests/wclaps`) need clang with the wasm32 target and `wasm-ld`: on Windows use LLVM
      from the runner, or build them once on Linux/macOS and use the artifacts.
- [ ] Cost: Actions minutes on private repos count double on Windows (2,000 free minutes a month on the Free plan);
      public repos are free. The plugin repo is private today.
- [ ] Later: `windows-11-arm` (ARM64) runner, since Windows on Apple Silicon (Parallels) is ARM64.

## W2. The core compiles and runs on Windows

From the backlog and a grep (verify each; don't assume):
- [ ] `Paths::root()` uses `$HOME/Library/Application Support/Plugin Circuit`: Windows `%APPDATA%\Plugin Circuit`
      (`SHGetKnownFolderPath(FOLDERID_RoamingAppData)`); Linux `$XDG_DATA_HOME/plugin-circuit`.
- [ ] `PackageCache` in-use lock is a no-op on Windows (`#if !defined(_WIN32)` around `flock`): `LockFileEx` /
      `UnlockFileEx` on `<package>/.lock`, shared for "in use", exclusive for delete.
- [ ] `PluginCircuitC.cpp`: `setenv` → `_putenv_s` (or pass the cache dir without the environment).
- [ ] Every `std::filesystem::path` to string conversion: UTF-8 on Windows needs `u8string()` / `path(u8string)`
      (user names with non-ASCII characters break `.string()`).
- [ ] tanh-net: the WinHTTP backend exists (`HttpClient_windows.cpp`); test HTTPS, redirects, proxies, progress.
- [ ] Wasmtime C API for Windows x64 (and ARM64): check the bridge's CMake downloads the right archive.
- [ ] `realtime-lock.h` `cpuRelax` on MSVC (`_mm_pause` / `__yield`), `steady_clock` (QueryPerformanceCounter).
- [ ] Compile cache folder, `settings.json` atomic rename (`MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`; is
      `std::filesystem::rename` enough over an existing file on Windows? verify), file locking during rename.
- [ ] `CatalogHealth` thread and its join at DLL unload (Windows: no joining threads in `DllMain`; we join in the
      entry's deinit and a static destructor: check the static destructor doesn't run under the loader lock).

## W3. The editor on Windows (`Editor_win.cpp`)

The macOS editor (`Editor.mm`) is the spec. Split what's portable (layout math is already in `EditorLayout.h`, the
API registration, the timer logic) from AppKit code first, so both platforms share it.
- [ ] Parent: CLAP gives an `HWND`; create a child window, our CHOC WebView (WebView2) for the header and views, and
      a child host for the WCLAP's own GUI (webview-gui's Windows backend gives it its `HWND`).
- [ ] Layout and resize: `gui_set_size`, `adjust_size`, host-driven resizes, the keyboard strip growing the window
      (the macOS logic in `requestSize` / `layout`).
- [ ] DPI: per-monitor DPI awareness v2; CLAP's `set_scale`; WebView2 zoom vs scale; moving between monitors.
- [ ] Message loop and timers: our 50 ms editor tick (`choc::messageloop::Timer`) on Windows; the plugin's
      housekeeping timer when the host has no timer support.
- [ ] Note keys typed into the hosted plugin's GUI reaching the on-screen keyboard (macOS: an `NSEvent` monitor;
      Windows: WebView2 `AcceleratorKeyPressed` or a thread keyboard hook; DAWs also steal keys: test).
- [ ] The hosted GUI's background colour sampling (no white flash on load).
- [ ] WebView2 runtime: present on Windows 11 and most Windows 10; detect and explain if missing. User data folder:
      per user under `%LOCALAPPDATA%`, not next to the DAW (WebView2 refuses folders it can't write).
- [ ] The editor resize GUI test (`tests/editor_resize_test.mm`) as a Windows test with a small Win32 host.

## W4. webview-gui's CHOC backend catches up with macOS

From `plugin/docs/BACKLOG.md`: messages to the page without `event.source` (Surge XT's presets would fail), the
base64 fallback's length bug, non-binary page messages forwarded, no context-menu or scroll-bar policy (WebView2
shows its browser menu). Fix with one shared init script for both backends, so they can't drift again.

## W5. WCLAP features we don't support (both platforms)

Scanned and planned in [WCLAP_FEATURES.md](WCLAP_FEATURES.md): export/download (Spiral303, Polarity), file inputs
(Spiral303, Xenon, Radar, Polarity), drag and drop (Radar, Xenon, OpenRB), clipboard (the 12 JUCE-kit ports), links,
`localStorage` (Spiral303), pointer lock, dialogs, and the CLAP extensions the bridge doesn't forward (preset
discovery, resource directory, undo, tuning). Several are missing on macOS too; the Windows column there is what to
verify in W3/W4.

## W6. Real-time and performance on Windows

- [ ] The non-blocking audio lock and dropout concealment under WASAPI and ASIO at 64 and 128 frames.
- [ ] Wasmtime compile times and the compile cache on Windows (antivirus scanning of the cache folder: measure).
- [ ] The Performance tab's numbers on Windows.

## W7. Testing on real Windows

- [ ] A machine: a Windows x64 PC is the reference; Parallels on Apple Silicon runs Windows on ARM64 (x64 plugins
      run emulated there, which hides or adds problems).
- [ ] DAWs: REAPER, Bitwig, Ableton Live, FL Studio, Cubase (VST3). Validators on Windows: clap-validator,
      pluginval, Steinberg's VST3 validator (in CI where they run headless).

## W8. Packaging

- [ ] Installer (Inno Setup or WiX) to `%COMMONPROGRAMFILES%\CLAP` and `%COMMONPROGRAMFILES%\VST3`, the standalone
      apps to `Program Files`.
- [ ] Code signing (Authenticode; e.g. Azure Trusted Signing) so SmartScreen doesn't block it.

## Order

W1 → W2 (CI green on core and tests) → W3 + W4 (the editor) → W7 (real machine, DAWs) → W5 (features, both
platforms) → W6 → W8. W5's audit can start any time on macOS.

## Open questions

1. A Windows machine for testing: x64 PC, or Parallels (ARM64) only?
2. CI on private repos costs double on Windows; or make the repos public (Actions free)?
3. MSVC only, or clang-cl too?
4. Which DAWs matter most on Windows?

## Notes and decisions (fill in while working)
