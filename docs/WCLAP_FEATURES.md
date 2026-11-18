# WCLAP features we don't fully support (macOS and Windows)

Status: scanned 2026-11-18; macOS fixes for downloads, links and dialogs in our webview-gui fork (not committed yet);
Windows not started. Paths are relative to the workspace `~/Code/work/plugin-circuit/`.

## macOS, measured (2026-11-18)

With the web-features test WCLAP (`plugin/tests/wclaps/ui/features/`, `-DTEST_WEB_FEATURES`) in the editor harness:
`PLUGINCIRCUIT_AUTOLOAD_ARCHIVE` installs it without the catalog, `PLUGINCIRCUIT_TEST_CLICKS` clicks its buttons
(through `evaluateJavaScript`, which WebKit treats as user input; synthesized `NSEvent`s don't reach the page), a
watcher logs and closes every modal panel. ctest `webview-features` checks the save panel, the open panels, copy and
the dialogs.

| Feature | Before | After (our webview-gui, `apple-osx.h`) | How it's checked |
| --- | --- | --- | --- |
| Download / export (`<a download>`, blob URL) | nothing happened | `NSSavePanel` with the page's file name, then a `WKDownload` writes it (macOS 11.3+) | ctest (panel); the written file checked by hand: correct content |
| File inputs (static, created in JS) | `NSOpenPanel` (since `efbf1f5`) | same | ctest |
| `alert` / `confirm` / `prompt` | returned at once (`confirm` false, `prompt` null) | `NSAlert` dialogs; `confirm` true on OK, `prompt` returns the text | ctest |
| `window.open`, `target="_blank"`, links to other sites | nothing (`null`) | http(s) and mailto open in the default browser; the GUI page stays | by hand (2026-11-18): `window.open` and a `_blank` link both opened plugincircuit.studio in the default browser |
| Copy (`navigator.clipboard.writeText`) | works (the `webview-gui:` page is a secure context) | same | ctest |
| Paste (`readText`) | `NotAllowedError` from a script click | works with a real click, after WebKit's own "Paste" bubble (as in Safari: the user confirms each read). No catalog plugin reads the clipboard; if one needs paste without the bubble, answer `readText` natively (NSPasteboard) from the init script | by hand (2026-11-18): `paste: ok` with the clipboard's text |
| Pointer lock | `WrongDocumentError` from a script click | **refused** with a real, trusted click too (`WrongDocumentError`). Cause not verified: check in WebKit's source what WKWebView needs from its embedder to allow it. Slide, MNO and coopiano catch the refusal and fall back to normal dragging | by hand (2026-11-18) |
| `localStorage` | persists, but one origin `webview-gui://` for every plugin | open: needs a per-plugin origin (the bridge has to pass the plugin id to webview-gui) | probe `origin`, `storageRuns` |
| Drag and drop of files onto the page | unverified | works: the page gets the file (name, size) | by hand (2026-11-18): `drop: grainfernoLog.txt 0 B` |
| WebGL 2, page↔plugin messages | work | same | probe |

Done when: every feature below that a catalog plugin uses works on macOS and Windows (or is refused on purpose, with
the reason documented), each with a test, and the upload checker on the website warns plugin authors about what
hosts don't offer.

## How this was found

1. **The webview backends** (`external/webview-gui/include/webview-gui/_impl/platform/`): what each one implements.
   - `apple-osx.h` (macOS, WKWebView): a script-message handler (page ↔ plugin), a URL scheme handler
     (`webview-gui:`, serves the bundle), and a `WKUIDelegate` with only `runOpenPanelWithParameters` (file inputs,
     our commit `efbf1f5`). No navigation delegate, no download handling, no alert/confirm/prompt panels, no
     new-window handling; no website data store set (WebKit's default).
   - `choc.h` (Windows: WebView2, Linux: WebKitGTK; through CHOC): an init script and the message bridge only. It
     still has the bugs fixed only on macOS (`25e0f55`: `event.source`, the base64 decoder allocating `b64.length`
     bytes, non-binary messages forwarded) and none of the macOS policies (`0c72daa`: scroll bars, browser context
     menu; `efbf1f5`: file inputs, which WebView2 may handle by default: verify). Origin:
     `https://choc.localhost/` on Windows, `choc://choc.choc/` elsewhere.
2. **What the catalog's plugin pages use**: every bundle unpacked (38 packages, 44 plugins), their HTML and JS
   grepped, each hit read in context. (The JUCE-kit ports share `ui/webclap-ui.js`, `webclap-ui-worker.js` and an
   Emscripten `ui.js`; Emscripten's runtime contains `alert`/`prompt` calls for its own errors, which don't count.)
3. **The bridge** (`external/wclap-bridge/source/`): which CLAP extensions and factories it forwards.

## Page features

| Feature | Catalog plugins that use it (where) | macOS today | Windows today | Work |
| --- | --- | --- | --- | --- |
| **Export / download a file** | Spiral303: Export MIDI (`app.js`: blob URL, `<a download>`, `click()`); Polarity SC Dark: preset export | **not handled**: WKWebView needs a navigation delegate that turns the navigation into a `WKDownload` (macOS 11.3+); without it the click does nothing (verify) | WebView2's default: saves to the Downloads folder with the browser's download bubble (verify) | a native **save dialog** on both (macOS: navigation delegate + `WKDownload` + `NSSavePanel`; Windows: WebView2 `DownloadStarting` → `IFileSaveDialog`), blob URLs included |
| **Open a file** | Spiral303: MIDI import (`input` created in JS, `.mid`); Xenon: audio sample (`<input type="file" accept="audio/*">`); Radar: audio sample (`input` created in JS); Polarity: preset import (`.json`) | works: `NSOpenPanel` (`efbf1f5`); test each of the four | unknown (WebView2 may show its own dialog) | verify; large files (samples of 50+ MB) go to the DSP through messages: measure |
| **Drag and drop files onto the GUI** | Radar, Xenon, the OpenRB set (808, 909, 303, Compressor, Delay, Distortion, PCF): `dragenter`/`drop` with `dataTransfer.files` | works for Radar (noted when `efbf1f5` was found); the others unverified | unknown; inside a DAW the host window may take the drop | verify both; a drop into a plugin GUI hosted in a DAW window |
| **Clipboard** | the 12 JUCE-kit ports (Six Sines, Surge XT, OB-Xf, Odin 2, RipplerX, oXygen, TapElectric, Droplets, Radar, OpenSpatialDelay, ZL Compressor, ZL Spectrum EQ): `navigator.clipboard.writeText` for copy (JUCE text fields, patch names) | likely **missing**: `navigator.clipboard` needs a secure context; the `webview-gui:` scheme may not be one (the call is guarded, so copy silently does nothing): verify | `https://choc.localhost` is a secure context: likely works, verify | make the scheme secure or bridge copy/paste through the plugin page's messages; paste too |
| **Links and new windows** | Polarity SC Dark: license page and source link (`target="_blank"`) | **not handled**: no `createWebViewWith…`/navigation policy, so nothing opens (verify) | WebView2's default opens its own popup window (a browser inside the DAW) | open http(s) links in the default browser on both; refuse other popups |
| **Persistent page storage** | Spiral303: user presets in `localStorage` (`spiral303.userPresets`) | WebKit's default data store, origin `webview-gui:`; shared by every plugin? kept across DAW restarts? (verify) | per WebView2 user-data folder, origin `https://choc.localhost`: every plugin shares one origin | one store per plugin id, persistent; and tell authors to keep what a session needs in `clap.state` (a preset only in `localStorage` is lost on another machine) |
| **Pointer lock** (endless knob drags) | Slide, MNO (`compost-knob.js`), coopiano: `requestPointerLock` (all fall back if refused) | unknown | unknown | verify; fall back is fine if refused |
| **`alert` / `confirm` / `prompt`** | none in the catalog (only Emscripten's runtime) | **not handled**: without `WKUIDelegate` panels they return immediately (`confirm` returns false) | WebView2 shows its own dialogs | add native dialogs on macOS for parity; low priority |
| **WebGL 2** | Xenon (waveform) | works (seen in its preview) | unknown | verify |
| **Keyboard** | 21 plugins listen to `keydown`/`keyup` (JUCE ports, Helm, Slide, MNO, coopiano, ...) | works, but the DAW may take keys first; our host forwards note keys from a focused plugin GUI to its on-screen keyboard (macOS only) | unknown | verify text entry and shortcuts in DAWs; the forwarding on Windows (see `WINDOWS.md` W3) |
| **Context menu, scroll bars** | all | hidden (`0c72daa`) | WebView2 shows its browser menu ("Reload", "Inspect") | same policy in the CHOC backend |
| **Messages between page and plugin** | all | fixed (`25e0f55`) | the old bugs (Surge XT's presets don't apply; payloads with `=` padding decode wrong) | the shared init script for both backends |

Not used by any catalog plugin today: the File System Access API (`showOpenFilePicker`, `showSaveFilePicker`),
WebMIDI, microphone/camera, fullscreen, `SharedArrayBuffer` in pages. Expect them later (sample editors, MIDI
learn); decide when a plugin needs one.

## CLAP features the bridge doesn't forward

From `external/wclap-bridge/source/_generic/wclap-plugin.h` (the `CLAP_EXT_*` it handles) and `wclap-module.h`
(factories). Forwarded: ambisonic, audio-ports (+ activation, config, config-info, configurable), context-menu, gui,
latency, log, note-name, note-ports, param-indication, params, preset-load, remote-controls, render, state,
state-context, surround, tail, thread-check, thread-pool, timer-support, track-info, voice-info, webview.

| Missing | What it costs | Who needs it |
| --- | --- | --- |
| **Preset discovery factory** (only `CLAP_PLUGIN_FACTORY_ID` is offered) | hosts can't list a plugin's factory presets in their browser | Surge XT implements preset-discovery (amilajack/surge PR #2); any plugin with a preset library |
| **resource-directory** (draft) | no per-project folder for samples a session uses: a plugin that keeps only a path breaks on another machine | sample-based plugins (see `BACKLOG.md`, Caches: plugin data) |
| **undo** (draft) | the DAW's undo doesn't include plugin edits | large editors (Surge XT lists "CLAP undo" as not done) |
| **tuning** (draft), MTS-ESP | no microtuning from the host | Surge XT, Six Sines (both support MTS-ESP natively) |
| transport-control, triggers, project-location, event-registry, scratch-memory, extensible-audio-ports, gain-adjustment-metering, mini-curve-display (drafts) | | none in the catalog yet |

Also known (`plugin/docs/UPSTREAM.md`): the host `thread-check` extension, when offered, traps in the bridge's
audio-thread instance; our inner host doesn't offer it.

## Plan

1. **Test page first** (both platforms): a test WCLAP whose page exercises every row above (download a blob,
   file input, drop, clipboard copy and paste, `_blank` link, `localStorage` write/read across reloads, pointer
   lock, `alert`/`confirm`, WebGL 2, key events) and reports results to the DSP, so a test can read them. Extend
   `plugin/tests/wclaps/` (it already builds freestanding test WCLAPs with a page). Run it in the editor harness on
   macOS (`tests/editor_resize_test.mm` drives the page with `PLUGINCIRCUIT_UI_JS`) and later on Windows.
2. **Fix the backends in our webview-gui fork**, one commit per feature, each recorded in `plugin/docs/UPSTREAM.md`
   and proposed upstream:
   - one shared init script for both backends (messages, base64, scroll bars, context menu);
   - downloads → native save dialog (macOS `WKDownload` + `NSSavePanel`; Windows `DownloadStarting` + save dialog);
   - `_blank` / `window.open` → default browser (http/https only);
   - clipboard (secure context, or bridged);
   - per-plugin persistent storage;
   - `alert`/`confirm`/`prompt` on macOS.
3. **Bridge**: the preset-discovery factory first (a host-visible feature), then resource-directory.
4. **Upload checker** (website, `apps/web/src/check/checks.ts`): warn when a page uses a feature hosts don't offer
   (or offer differently), with the link to this list.
5. **Re-run the scan** (below) when the catalog changes; keep the tables current.

## Re-running the scan

Unpack every bundle (`website/data/bundles/*.wclap.tar.gz`) into a scratch folder and grep `*.html`, `*.js`,
`*.mjs` for: `\.download\s*=`, `createObjectURL`, `type\s*=\s*["']file`, `show(Open|Save)FilePicker`,
`dataTransfer\.(files|items)`, `navigator\.clipboard`, `(alert|confirm|prompt)\s*\(`, `window\.open|_blank`,
`localStorage|indexedDB`, `requestPointerLock`, `requestFullscreen`, `requestMIDIAccess`, `getUserMedia`,
`getContext\(["']webgl`, `addEventListener\(["']key`. Read each hit in context: generic runtime code (Emscripten's)
isn't a use.
