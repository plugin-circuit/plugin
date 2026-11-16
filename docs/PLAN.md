# Implementation plan: messaging, parameters, API

What to build, in what order, for the design in `MESSAGING.md`. Each phase ships on its own: it builds, its tests pass,
and the plugin works with only the phases before it. Sizes: S (a day or less), M (a few days), L (a week or more).

Rules for every phase: nothing new on the audio thread allocates, locks, logs or makes syscalls (preallocate in
`activate()`, lock-free structures only); every behaviour change comes with a test; `docs/` is updated in the same
change.

## Phase 0: fixes found in the audit (independent, do first) — S each

Status: done. 0.1 tested by `tests/host-ui/picker_messages.ts` (the old listener let a rogue frame and a same-origin
frame load plugins). 0.2: OpenRB Delay reports a 292,883-sample tail, [Basics] Reverb 144,000 (`inner-tail` test);
no catalog plugin returns `SLEEP` today, so the status pass-through and the wake on UI input are untested against a
real plugin. 0.3 has no plugin that changes its note ports to test against.

**0.1 Picker messages only from the picker.** `host-ui/app.js` (`message` listener): accept `plugincircuit:select` /
`cancel` only if `e.source === $("picker").contentWindow` and `e.origin === new URL(apiBase()).origin`.
- Test: a headless host-UI test (Playwright, like the keyboard test) where a second iframe posts `select`: nothing
  loads; the picker's own message still loads.

**0.2 The WCLAP's tail and process status.**
- `InnerPlugin`: resolve `clap_plugin_tail` after init; `tail()` (audio or main thread, as CLAP allows).
- Inner host `clap.tail` `changed()` → callback → the DAW's `clap_host_tail.changed()`. Both are `[audio-thread]` in
  CLAP's `ext/tail.h`, and the WCLAP calls it from its `process`, so it's forwarded directly, no thread hop.
- Our plugin offers `clap.tail` to the DAW: the inner plugin's tail, 0 without one.
- `processBlock` returns the inner plugin's status instead of always `CONTINUE`. Because a sleeping plugin isn't
  processed, `uiNote` / `uiSetMacro` / `uiSetParam` call the DAW's `request_process` (thread-safe).
- Test: smoke tool on a delay or reverb from the catalog (`[Basics] Reverb`, `OpenRB Delay`): tail > 0 reported; a
  plugin returning `SLEEP` is woken by an on-screen note (state test harness with a fake host counting
  `request_process`).

**0.3 Note-port changes.** Inner host `note_ports.rescan()` → `requestRestart` (the note-dialect conversion is
chosen in `activate()`). Test: unit-level, the callback fires.

## Phase 1: the parameter table — the single source of truth — L

Status: done (1.1 to 1.4). Measured with Six Sines (2,591 parameters), main-thread calls into the plugin's params
extension (`PLUGINCIRCUIT_DEBUG_CALLS`):

| | before | after |
| --- | --- | --- |
| calls per second, plugin view, idle | ~7,800 (`get_info` + `get_value` + `value_to_text`, 2,591 each) | 0 |
| calls per second, Params view, idle | same | 64 `get_value` (the safety net) |
| building one UI update | 0.9-1.3 s (so 1 update a second instead of 20) | 0.0 ms; once after a load: 620 ms (every text formatted once, then cached) |
| a parameter changed from the UI | a full rebuild | 1 `value_to_text` |

Helm (139 parameters) went from ~7,900 calls per second to 0. Tests: `tests/param_table_test.cpp` (including a
concurrent writer and reader: no change lost); the end-to-end check (a UI edit shows the plugin's own text) runs in
the editor harness, whose host now flushes like a DAW that isn't processing.

Left for later: the 620 ms after a load could go by formatting texts only for the parameters the page shows.

**1.1 `src/core/ParamTable.h/.cpp`.** One table per loaded slot: the WCLAP's parameters plus the 64 macros.
- Per parameter: `info` (cached `get_info`), `std::atomic<double> value`, `std::atomic<uint8_t> owner`
  (none / UI / plugin), a bit in a preallocated `changed` bitset (`std::atomic<uint64_t>` words).
- Built on the main thread (load, `rescan(INFO|ALL)`), with the current values copied over; published by an atomic
  pointer swap; the old table freed after the audio thread has finished a block with the new one (`processEpoch_`,
  as the slot swap).
- `id → index`: a sorted vector built with the table (no map lookups on the audio thread).

**1.2 Writers: only the thread that processes** (the audio thread, or the main thread inside `params.flush` when
not processing).
- `translateEvents`: UI edits and the DAW's values (macros → their targets) write `value` and set `changed`.
- `innerOutTryPush`: the WCLAP's output values write the table (today they only update macros).
- State load (`applyHostState`): the restored values, then one `changed` for all.

**1.3 Readers stop calling into the WebAssembly.**
- `uiSnapshot` reads values from the table and sends only `changed` ones.
- `InnerPlugin::params()` (`count` + `get_info` per parameter) only on load and `rescan(INFO)`.
- **Text cache** (main thread): `(param id, value) → text`, filled by `value_to_text` only for changed values,
  bounded (e.g. 4096 entries, least recently used). Used by the editor and by our macros' `value_to_text` for the DAW.
- `get_value` only on load, state load and `rescan(VALUES)`.

**1.4 Safety net.** While the Params view is open: re-read the visible parameters with `get_value` at 1 Hz, at most
32 per tick; a difference counts as a change.

**Tests.**
- Unit (`tests/param_table_test.cpp`): build / swap / free; the bitset swap loses nothing under a concurrent writer
  (a thread hammering writes while the reader swaps); id lookup.
- Count of calls into the WebAssembly: an instrumented `InnerPlugin` (call counters) with Six Sines (2,600
  parameters) loaded and the Params view open: before, ~100,000 calls/s; after, at most the changed parameters plus
  the 1 Hz safety net.
- The main-thread block test, busy scenario, with the editor's real traffic instead of the synthetic loop: skipped
  blocks before and after (wait ratio Off, where contention shows most).

## Phase 2: gestures and ownership — M

**2.1 `edit {target, id, value, phase}`** for macros and parameters (`begin` / `update` / `end` / `set`), replacing
`setMacro` / `setParam`. The UI queue carries the phase.

**2.2 The state machine** of `MESSAGING.md` ("Gestures and ownership") on the audio thread, per parameter, in the
table's `owner`: UI begin / value / end, WCLAP `GESTURE_BEGIN` / value / `END`, DAW values dropped while owned.
`GESTURE_BEGIN` / `END` reach the DAW for macros (and for parameters through their macro).

**2.3 Stray gestures closed** on unload, `reset()`, deactivate, and after a skipped block's carry-over overflowed.

**2.4 No echo to the owner, `ack` at the end** (needs the push channel of phase 3; until then: the snapshot leaves
out values of UI-owned parameters, and the UI's 400 ms guard stays).

**Tests.** A table-driven unit test: one case per cell of the ownership table (event × current owner → new owner,
applied or dropped, what the DAW receives). An integration test with a fake host checking the DAW gets
`BEGIN`, values, `END` in order for a UI drag, and nothing for a plugin gesture on an unmapped parameter.

## Phase 3: one API — stores, commands, streams — L

**3.1 `src/api/Api.h/.cpp`: the registry**, the only dispatch the editor calls.
- `store(name, schema?, get, set)` with a version per store; `patch` = the changed part plus the new version.
- `command(name, handler)` → `{ok, result | error}`.
- `stream(name, sampler, hz)`, sampled only while subscribed.
- Subscriptions per editor (an editor = one page).
- All handlers on the main thread.

**3.2 The wire.** One binding `request(name, args)` from the page; `receive(message)` to the page (`{kind, topic,
version, data}`); `hello {protocol}` handshake, with an error page on a mismatch. `Editor.mm` becomes transport only:
no logic, no snapshot building.

**3.3 Stores and commands** as mapped in `MESSAGING.md` ("Today's calls, mapped"): `plugin`, `params`, `macros`,
`settings`, `cache`, `transport`, `ui`, `app`; commands `plugin.load/unload`, `cache.delete/clearCompiled`,
`macros.autoMap`, `app.openUrl/log`; stream `perf`. The 50 ms snapshot goes; patches replace it.

**3.4 `host-ui/api.js`: the JS client** (`get / set / subscribe / command / onStream`), and the views move to it,
one at a time (settings first: it's self-contained; then macros/params; the plugin pill and status last).

**Tests.**
- Registry unit tests in C++, no webview: JSON requests in, replies and patches out, versions increasing, unknown
  names and invalid `set`s answered with errors.
- Host-UI tests with a mock binding (Playwright, headless): each view renders from stores and re-renders on patches;
  a stale patch (older version) is ignored.
- The existing GUI and resize tests keep passing.

## Phase 4: settings schema and scopes — M

**4.1 Schema** (`src/api/SettingsSchema.h`): key, type, default, range, scope, label, help text. The core validates
`settings.set` against it; the Advanced tab renders from it (no hand-written controls per setting).

**4.2 Instance scope** in our CLAP state: `StateCodec` gets a version bump and a migration (old states load with
defaults); keyboard octave and last view move there.

**4.3 Machine scope across processes**: watch `settings.json` (macOS: a `dispatch_source` / FSEvents; Windows:
`ReadDirectoryChangesW`; Linux: inotify), or re-read on window focus as the first step. Values for the audio thread
(wait ratio, concealment) are pushed to the bridge on change.

**Tests.** Schema validation (clamping, unknown keys, wrong types); a state round trip with instance settings,
including loading a state from before the version bump; two `Settings` instances in one process see each other's
change; a file written by "another process" (the test) is picked up.

## Phase 5: the remaining channels — M

**5.1 `app` store**: online / offline decided by the core (a health check on the loader's thread, backed off), API
base; the page stops fetching `/v1/health`.

**5.2 `plugin.errors`**: load failures, the bridge's errors (its log routed to our host log callback, captured),
WebAssembly traps; recent ones, with time and source; shown in the plugin view when the plugin fails.

**5.3 `keyboard.key` events** replace `plugincircuitHost.key`.

**5.4 `keyboard.notes` stream (optional)**: DAW notes on the on-screen keyboard. The audio thread pushes note on/off
into a preallocated SPSC queue; the stream drains it while the keyboard is open.

**5.5 The picker contract**: the `plugincircuit:select` / `cancel` messages written down in the website's
`docs/API.md`, with a version field, and a website e2e test that the picker sends them.

## Phase 6: verify and clean up — S

- Measure against the baseline taken at the start of phase 1: calls into the WebAssembly per second (idle and with
  Params open), main-thread time per second in the editor, skipped blocks under real UI use (Six Sines, Surge XT,
  Helm), the Performance tab's DSP load.
- Remove what the API replaced: the snapshot builder, `paramText`, the old calls, the UI's 400 ms staleness guard.
- `MESSAGING.md`: drop the "Today" notes that no longer apply.

## Order and why

| # | Phase | Size | Depends on | Why here |
| --- | --- | --- | --- | --- |
| 1 | 0: audit fixes | S each | — | small, independent; 0.1 is a security fix |
| 2 | 1: parameter table | L | — | removes the WebAssembly polling: the biggest real-time win for single-threaded plugins |
| 3 | 2: gestures | M | 1 | needs the table's `owner`; gives the DAW proper automation passes |
| 4 | 3: API | L | 1 (stores read the table) | the long-term structure; easier once the data has one home |
| 5 | 4: settings | M | 3 | the schema lives in the registry |
| 6 | 5: other channels | M | 3 | each is a store / event / stream on the new wire |
| 7 | 6: verify | S | all | numbers against the baseline; delete the old paths |

## Risks

- **Plugins that change parameters without reporting them** (no output events, no `rescan`): the table drifts. The
  1 Hz safety net catches what's visible; anything else is a plugin bug to report (and a candidate for the upload
  checker: change a parameter through the GUI, compare `get_value`).
- **DAWs that call our `get_value` / `value_to_text` often**: answered from the table and the text cache, so cheap.
- **State format change** (phase 4.2): the migration must load every existing session; test with states saved by
  the current version.
- **The protocol between page and core** changes in one go per view: keep both paths only while a view moves, never
  longer than its phase.
- **Windows / Linux** (see `BACKLOG.md`, Platforms) are not part of this plan, but nothing here may add macOS-only
  code outside `src/gui/`.
