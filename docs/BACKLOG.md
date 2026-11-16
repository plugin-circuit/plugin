# Backlog

Ideas and known issues for the Plugin Circuit host plugin, roughly by priority within each section. Fixes we carry
in upstream code are listed in `UPSTREAM.md`.

## Hosted plugin GUIs

- **GUI size from the plugin or the catalog, not guessed.** WCLAPs with only `clap.webview` declare no size: the bridge
  defaults to 400x250, resizable, so a fixed panel like OpenRB 808 fills only the top of its area. Plan: prefer the
  WCLAP's `clap.gui` (`get_size` / `can_resize` / `adjust_size`); else the catalog's package metadata (preferred size,
  resizable), recorded by the upload checker, which already loads the GUI, and editable by the developer. A content
  measurement heuristic was tried and removed (unreliable for pages that build late or animate).
- **Catalog-wide GUI check.** Open every catalog plugin's GUI in the test harness (`tests/editor_resize_test.mm`):
  created, fills its area, a message round trip with the DSP, screenshots at a few sizes. Run on upload. The earlier
  catalog check only tested audio, which is how the GUI bugs in `UPSTREAM.md` went unnoticed.
- **Zoom stays the fallback** for fixed-size webview GUIs. To check: canvas-based UIs (Surge XT, Six Sines) under page
  zoom: sharp, or upscaled bitmaps?
- **White flashes on resize.** The webview now draws no background and the plugin's sampled edge color fills behind it.
  If flashes remain: freeze a snapshot during live resize, remove it once the page has redrawn. Longer term: propose a
  background color field for webview GUIs in the WebCLAP / webview-gui spec.
- **Picker embedded in the host** repeats our header (its own wordmark and "Choose an instrument"): hide both when
  embedded, keep the search.
- **Single webview (iframe) hosting.** Considered and parked: the two-view design works and is tested; an iframe would
  mean serving the WCLAP's files and relaying its messages ourselves.

## Platforms

- **Windows editor:** `Editor_win.cpp` with a child HWND and CHOC's WebView2 view. `src/gui/Editor.mm` is compiled
  unconditionally today, so Windows doesn't build. The bridge's webview-gui has a CHOC backend for the plugin's own UI
  (untested on Windows).
- **Linux** after Windows (embedded CHOC webviews are reported to render black on Linux; needs checking).

## Caches

- Done: compiled code in `Plugin Circuit/compiled` (Wasmtime prunes it at 1 GiB); no duplicate archives; per package
  `usage.json` (last loaded, last saved in a DAW session); loaded packages are locked against deletion across processes;
  Settings > Cache lists them and deletes selected / unused for 30 days / all, and clears compiled code.
- **Automatic eviction** for packages: a size budget (e.g. 4 GB), evicting try-outs (never saved in a session) and the
  least recently used first, never anything in use. Today packages only go when the user deletes them.
- **Windows:** the in-use lock (`PackageCache::InUse`) is a no-op there; needs `LockFileEx`.
- **Catalog keeps every published version** (content-addressed by sha256), so an evicted package an old session needs
  can be downloaded again. Server-side guarantee, not enforced yet.
- Wasmtime's old default cache (`~/Library/Caches/BytecodeAlliance.wasmtime`) is no longer used by us but isn't
  deleted automatically: other Wasmtime apps may share it.

## Session state and loading

- **Offline bounce before the plugin has loaded renders silence (synth) or the dry signal (FX).** `state load` starts
  the download and compile on a thread and returns at once, and nothing stops the host from bouncing meanwhile.
  Plan: implement CLAP's `render` extension; in offline mode `process()` waits for the load (with a timeout), which is
  allowed when not realtime.
- **Load synchronously when the package is cached.** Inside `state load`, so the session opens with the plugin ready,
  as with any heavy plugin, and audio only runs once it's loaded. Stay asynchronous only when a download is needed.
- **Data outside the session.** The session holds the plugin's full `clap.state` (plus a parameter snapshot), but not
  files it writes to its `/presets`, `/cache`, `/var` folders (per package, on this machine): samples loaded into it,
  its own user presets. A plugin that keeps only a path in its state breaks on another machine. Plan: WebCLAP docs
  require everything a session needs to be in `clap.state`; the upload checker flags plugins that write to `/var`;
  later CLAP's resource-directory extension (not in the bridge yet).
- Already handled: a plugin that fails to load keeps its saved state untouched (re-saved as is) and shows "unavailable".

## Plugin entries and I/O

- **FX with a note input** (vocoders, MIDI-triggered effects) and **Synth with a side-chain input** (synths that
  process audio). The latter is also how Logic hosts plugins that need both.
- **A MIDI FX entry** for arpeggiators and the like, if the catalog needs it.

## Upstream

- PRs to wclap-bridge / webview-gui for each fix in `UPSTREAM.md` (drafted by us, submitted by Valentin).
- Issue for `wclap-cmajor-shell`: `value_to_text` drops the parameter's `unit` (Pro-54 shows "29.92" instead of "29.92 st").
- `/user`, `/factory` mounts: Surge XT and Six Sines expect a writable user folder ("User directory is non-writable"),
  so user presets can't be saved. Bridge mounts today: `/plugin.wclap`, `/presets`, `/cache`, `/var`.

## To verify in the real app

- MIDI-only note dialect conversion against a real plugin.
- Note keys forwarded from a focused plugin GUI to the on-screen keyboard.
- The play button (standalone, no host transport).
- Coopiano reports 140 samples of latency but measures 305 (same as the prototype).

## Not this repo

- Website: open source after a gitleaks scan of the full history and a licence choice (AGPL-3.0 or MIT).
- libplugincircuit: check the CI run of the last push.
