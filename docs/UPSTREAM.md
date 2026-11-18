# Upstream dependencies and our fixes

The plugin fetches wclap-bridge from our fork. Each fix is one commit on the fork's `plugin-circuit` branch, on top
of the upstream commit we started from. Each one is general, not Plugin Circuit-specific, and meant to go upstream
as its own pull request. None has been sent yet. When upstream merges one, rebase the branch and the commit drops out.

| Fork | Upstream | Branch | Based on |
| --- | --- | --- | --- |
| [plugin-circuit/wclap-bridge](https://github.com/plugin-circuit/wclap-bridge) | [WebCLAP/wclap-bridge](https://github.com/WebCLAP/wclap-bridge) | `plugin-circuit` | `cd11d22` |
| [plugin-circuit/webview-gui](https://github.com/plugin-circuit/webview-gui) | [geraintluff/webview-gui](https://github.com/geraintluff/webview-gui) | `plugin-circuit` | `172164b` |

webview-gui is the bridge's GUI layer: it turns a WCLAP's web page into a native plugin window (a WKWebView on macOS,
CHOC elsewhere), serves the page's files from the plugin and relays messages between the page and the DSP. It's a
git submodule of the bridge (`modules/webview-gui`); our bridge fork points it at our webview-gui fork.

## Updating

Work in a checkout of the fork (e.g. `../external/wclap-bridge`), one commit per fix, push the `plugin-circuit`
branch, then set `GIT_TAG` in `CMakeLists.txt` to the new commit. A change to webview-gui is committed and pushed in
its fork first, then the bridge fork's submodule pointer is bumped in a commit of its own.

## wclap-bridge commits

| Commit | Fix | Exposed by |
| --- | --- | --- |
| `34f15dd` | Enable Wasmtime's exceptions proposal (`wasmtime_config_wasm_exceptions_set`). Legacy EH (`try`/`catch`) is still unsupported by Wasmtime; the catalog translates it with Binaryen (`scripts/normalize-wclap.sh`). | every C++ WCLAP built with `-fwasm-exceptions` (JUCE ports) |
| `f565766` | Use a parent project's `wasmtime` target when it provides one. | the iOS build |
| `6bc26a2` | `WCLAP_WASMTIME_TARGET` CMake option for Wasmtime's compile target, e.g. `pulley64`: the portable interpreter, no executable memory (iOS). Review: replaces the iOS spike's runtime environment variables (a shipped plugin shouldn't change its engine from the user's environment); the `WCLAP_WASMTIME_OPT` knob was dropped (it didn't help: OB-Xf still needed 12 GB and crashed). | iOS |
| `1e8ee00` | Hand WCLAPs a host struct with a CLAP version, name, vendor, url and version. It was zeroed, so plugins checking `clap_version_is_compatible(host->clap_version)` refused to create an instance. | every JUCE-kit port in open-webclap (ZL, Surge XT, Six Sines, RipplerX, ...) |
| `1c5a46a` | Resolve the webview host extension before the plugin lookup: WCLAPs may query it inside `create()`, before the instance is registered. | WCLAPs that query extensions in `create()` |
| `76ade06` | Initialise the WCLAP before the webview helper queries its extensions. Known edge: a webview message the WCLAP sends during its own `init()` is dropped (the relay is set up right after). | clap-helpers-based plugins (Tapa) abort otherwise |
| `56ca86c` | Stream proxies transfer whole requests (looping in 1 kB chunks; short reads/writes and partial counts on error handled) instead of at most 1 kB per call. | Tapa's GUI was blank: `get_resource` returned 1 kB of HTML and `false` |
| `7143682` | Call the WCLAP's `clap.gui` methods only between a successful `create()` and `destroy()`. | clap-helpers aborts otherwise |
| `8bf7670` | Provide `clap.gui` for WCLAPs that implement only `clap.webview`, and only when the webview extension exists (the bridge can only draw a GUI through it). Also fixes the old line that cast the gui extension's pointer as the webview extension. | OpenRB 808 had no GUI (and 14 of the cached catalog packages are webview-only) |
| `b7dec90` | Serve relative webview resources the WCLAP doesn't provide from its bundle. Paths are percent-decoded (`%20`), the query and hash dropped (also for the MIME guess), and refused if they could leave the bundle: `..` components, backslashes, NUL bytes. | the JUCE-kit ports (`get_resource` returns false: "hosts serve the bundle"); files with spaces |
| `2e8eea0` | Point the `webview-gui` submodule at our fork. | |
| `380bc83` | `wclap_global_init_with_cache()`: Wasmtime's compile cache configured from a TOML file (folder, size limit); a broken config falls back to Wasmtime's defaults instead of failing every load. | Plugin Circuit keeps compiled code in its own folder (`Plugin Circuit/compiled`, 1 GiB) |
| `1253ed1` | Point the `webview-gui` submodule at the file-dialog commit. | |
| `4692aad` | Single-threaded WCLAPs (no shared memory, most of the catalog) share one instance between the main and audio threads, behind a mutex the audio thread blocked on: a long main-thread call (Spiral303's Export WAV renders offline inside `webview_receive`) stalled the whole audio device. Now the instance lock is a recursive atomic spin-lock (`source/realtime-lock.h`); the audio thread only tries it, spinning against a clock deadline of `ratio x block duration` (default 0.2, `wclap_set_audio_wait_ratio()`), then skips the block (silence) and replays its events in the next one; `wclap_audio_skipped_blocks()` counts. Measured on Spiral303, 128 frames at 48 kHz: 10 exports caused 10 device underruns (worst block 41.5 ms) before, none after (about 15 blocks of that plugin skipped per export); a busy UI skipped half the blocks with ratio 0, none with 0.2. Upstream README calls this out: "the audio thread _may_ block". | Spiral303: clicks on every Export WAV |
| `6f6e08d` | A skipped block (see `4692aad`) no longer cuts to silence: `source/dropout-concealer.h` continues each output channel with a Burg linear-prediction model (order 32, fitted once per gap on the last 25 ms, from direct sums so |k| < 1), fades it out over 30 ms, and crossfades back into real audio over 3 ms. No added latency; ~130 us per gap for two channels. `wclap_set_conceal_dropouts()` (default on). Measured on Spiral303's Export WAV, 128 frames at 48 kHz: the step at a gap's start went from 0.41-0.45 to 0.011-0.016 and at its end from 0.42-0.46 to 0.000 (the plugin's own 99.9th-percentile step is 0.078). Unit tests: `plugin/tests/dropout_concealer_test.cpp`. | Spiral303: clicks at the edges of skipped blocks |
| `f33cd85` | Point the `webview-gui` submodule at `637f38c` (downloads, links, dialogs on macOS). | |
| `c913ef3` | Point the `webview-gui` submodule at `7047041` (a page origin per plugin on macOS). | |

## webview-gui commits (macOS backend, `apple-osx.h`)

| Commit | Fix | Exposed by |
| --- | --- | --- |
| `25e0f55` | Messages sent to the page get `event.source == window` (the spec's `window.parent` for the page); only trusted (real `postMessage`) binary messages are forwarded to the plugin, so our own synthetic ones aren't echoed back; non-binary messages are left to the page; the base64 fallback decoder allocates the right length. Review: replaced a global "incoming" flag with the browser's own `event.isTrusted`. | Surge XT: presets changed in the UI but not the sound (its page drops messages from any other source) |
| `0c72daa` | Hide scroll bars and the browser context menu in plugin pages. The document itself doesn't scroll; panels inside it still scroll, without bars. The page still gets the `contextmenu` event and can draw its own menu. Host policy rather than a bug: propose upstream as an option. Windows (the CHOC backend) needs the same. | Helm showed scroll bars; right-click showed Reload / Inspect Element |
| `efbf1f5` | `<input type="file">` opens a file dialog: a `WKUIDelegate` with `runOpenPanelWithParameters` shows a modal `NSOpenPanel` (the page's multiple/directory flags). Without one, WebKit silently ignores file pickers. | Radar: its Load button did nothing (drag and drop worked) |
| `637f38c` | Downloads (`<a download>`, blob URLs, responses the page can't show) become a `WKDownload` saved where the user picks in an `NSSavePanel` (a navigation delegate, macOS 11.3+); http(s)/mailto links to other sites, `target="_blank"` and `window.open` open in the default browser (the GUI page stays); `alert`, `confirm` and `prompt` are native `NSAlert` dialogs. Without these WebKit dropped downloads and new windows and returned from dialogs at once. Tested with the web-features test WCLAP (ctest `webview-features`, `docs/WCLAP_FEATURES.md`). | Spiral303's Export MIDI and Polarity's preset export did nothing; Polarity's license link opened nothing |
| `7047041` | Each plugin's page has its own origin, `webview-gui://<plugin id>/` (the CLAP helper passes the plugin id, made a valid hostname; `WebviewGui::create` takes an origin host; the scheme handler skips it), so plugins no longer share `localStorage` and the like. The CHOC backend (Windows, Linux) accepts the host and ignores it for now. What pages stored under the old shared origin isn't visible any more. | every plugin page shared one origin (`webview-gui://`): a page could read or overwrite another plugin's storage |

Known bridge limitation, worked around in Plugin Circuit rather than fixed: the host `thread-check` extension, when
offered, is called from the bridge's audio-thread instance and traps there (out-of-bounds table access). The inner
host doesn't offer `thread-check` (`src/core/InnerHost.cpp`).

## Worked around in our code

- **clap-wrapper v0.12.1 names the VST3 and AUv2 bundles `.vst3` / `.auv2`:** `make_clapfirst.cmake` reads
  `C1ST_BUNDLE_IDENTIFER` (typo) instead of `C1ST_BUNDLE_IDENTIFIER` for those two formats. `CMakeLists.txt` sets the
  misspelled variable too. Upstream fix to send.

- **clap-wrapper's standalone and AUv2 hosts resize the window but return `false` from `request_resize`.** The
  editor lays out from its real bounds, not the return value (`src/gui/Editor.mm`; tests in `tests/`).
- **WCLAPs that declare no GUI size** (webview-only, e.g. OpenRB 808) get the bridge's 400x250 resizable default, so
  a fixed panel fills only the top of its area. Open, see `BACKLOG.md`.
- **Value text without units** (Pro-54, built with `wclap-cmajor-shell`, whose `value_to_text` drops the patch's
  `unit`). We show the plugin's text as is. Upstream issue to file.
