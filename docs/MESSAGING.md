# Messaging: who owns what, and how changes travel

Status: design. "Today" notes describe the current code where it differs.

## Actors

| Actor | Thread | Talks to |
| --- | --- | --- |
| **DAW** | audio (`process`), main | core, through CLAP: parameter events, gestures, state, `rescan`, `mark_dirty` |
| **Core** (Plugin Circuit, C++) | audio and main | everyone |
| **Host UI** (our page, CHOC webview) | main (JS) | core, through the `plugincircuitCall` binding and pushed messages |
| **WCLAP DSP** (the hosted plugin, WebAssembly) | audio, main | core, through CLAP (it sees core as its host) |
| **WCLAP GUI** (its own page, separate webview) | main (JS) | only its own DSP, through webview messages. We relay the bytes and never read them |

## Ownership

One writer per piece of data at a time; everyone else holds a copy and accepts updates from the owner.

| Data | Owner | Others |
| --- | --- | --- |
| Parameter values (macros, the WCLAP's parameters) | **the audio thread**: the value being processed is the truth | DAW, host UI, WCLAP GUI hold copies |
| A parameter during a gesture (a drag in a UI) | **whoever started the gesture**, until its end | the core doesn't echo values back to the owner; competing writers for that parameter are ignored (see below) |
| The WCLAP's internal state that isn't a parameter (patterns, samples, preset name, ...) | **the WCLAP** | the core only saves and restores it as an opaque `clap.state` blob; the DAW learns it changed through `mark_dirty` |
| Macro mapping and names, auto-map | **the core, main thread** | the host UI requests changes; the core applies them and publishes a new version |
| UI-only state (view, keyboard, octave) | **the host UI** | the core keeps a copy only to restore the editor and size the window |
| Messages between the WCLAP GUI and its DSP | **the WCLAP** | the core is a pipe (webview-gui) |

Competing writers during a gesture: like a DAW's touch mode, the gesture owner wins for that parameter. The DAW's
automation events for it and the WCLAP's own changes to it are dropped until the gesture ends. Other parameters are
unaffected.

## The single source of truth: the core's parameter table

The core keeps one table for the loaded plugin: every WCLAP parameter plus the 64 macros. It's built on load and
rebuilt on `rescan(INFO|ALL)`, preallocated, and the audio thread is its only writer. That is how CLAP expects a host
to work: it learns values from the events it sends and receives, and asks the plugin (`get_value`) only after a load,
a state load or a `rescan(VALUES)`.

| Per parameter | Written by | Read by |
| --- | --- | --- |
| `info` (name, range, flags, module): cached `get_info` | main thread, on load / `rescan(INFO)` | UI, macro mapping, the DAW's `get_info` of macros |
| `value` (atomic double) | audio thread: every value it applies (from the UI queue, the DAW's events, the WCLAP's output events) | main thread (UI pushes, the DAW's `get_value`) |
| `owner` (none / UI / plugin) and the gesture | audio thread | audio thread (dropping competing writers), main thread (no echo to the owner) |
| `changed` bit (preallocated bitset) | audio thread, when `value` changes | the main-thread tick, which swaps it and sends the `values` message |

Text: one cache in the core, keyed by parameter and value, filled by `value_to_text` only for values that changed,
and used by both the host UI and the DAW (our macros' `value_to_text`). Main thread only.

Values that change without an event (a WCLAP that loads a preset internally and doesn't report it: a CLAP bug, but
it happens) would make the table drift. Safety net instead of polling: while the Params view is open, re-read the
values of the visible parameters with `get_value` at 1 Hz, a few at a time, and treat a difference as a change.

Implemented (`src/core/ParamTable.h`; numbers in `PLAN.md`, phase 1). Before, the editor called `count`, `get_info`,
`get_value` and `value_to_text` for every parameter on every update; each was a call into the WebAssembly and, for a
single-threaded WCLAP (23 of the 24 catalog plugins), took the lock the audio thread needs.

## Gestures and ownership

Who can own a parameter: **the host UI** (a drag in Macros or Params) or **the WCLAP** (its own GUI: its DSP sends
`PARAM_GESTURE_BEGIN`/`END` as output events). **The DAW never owns one**: in CLAP, gestures go from the plugin to the
host, never back, so the DAW's automation is a plain writer.

| Event for parameter p | owner none | owner UI | owner plugin |
| --- | --- | --- | --- |
| UI `begin` | owner = UI, tell the DAW `GESTURE_BEGIN` | (already) | ignored: the plugin's gesture wins |
| UI value | apply, tell the DAW and the WCLAP | apply | dropped |
| UI `end` | (nothing open) | apply the final value, `GESTURE_END` to the DAW, owner = none, `ack` to the UI | dropped |
| WCLAP `GESTURE_BEGIN` | owner = plugin, forward to the DAW (if p is mapped to a macro) | ignored: the UI's gesture wins | (already) |
| WCLAP value | apply, mark changed | dropped (the UI wins; the next UI value overrides it in the WCLAP) | apply, mark changed |
| WCLAP `GESTURE_END` | — | ignored | owner = none, forward to the DAW, mark changed |
| DAW value (automation) | apply | dropped (touch) | dropped (touch) |

A gesture still open after the plugin unloads, a `reset()`, or a skipped block's events lost to an overflow (see the
bridge's event carry-over) is closed by the core: owner = none, a `GESTURE_END` to the DAW if it was told of a begin.

## What the bridge does and what the core does

wclap-bridge stays a stateless translator between native CLAP and the WCLAP's memory; everything with state lives in
the core. Overlaps found, and what happens to each:

| Overlap | Decision |
| --- | --- |
| The core calls the bridge's `get_value` / `get_info` / `count` per parameter, per tick | Removed: the parameter table (values from events, info cached until `rescan`) |
| Two callers of `value_to_text` (the editor's text, our macros' text for the DAW) | One text cache in the core, filled only for changed values |
| Events kept across a skipped block: the bridge's carry-over buffer, and our UI-to-audio queue | Both stay: different layers. The bridge also carries gesture events, so a skip can't leave a gesture open |
| Output sanity: the bridge zeroes non-finite samples and samples >= 100 (`checkBuffers`) | Only in the bridge; the core doesn't repeat it |
| `params.flush` when not processing: ours (UI edits, state's parameter fallback) and the bridge's | The core calls it; the bridge translates it. One path |

## One API: stores, commands, streams

Everything the host UI and the core exchange is one of three kinds. Each kind has one set of rules, so a new feature
picks a kind and gets its threading, versioning, persistence and error handling for free instead of inventing calls.

| Kind | What it is | Rules | Examples |
| --- | --- | --- | --- |
| **Store** | State with a current value that others observe | Named, versioned, one owner. The UI `get`s it once, then receives `patch`es (only what changed, with the new version) while subscribed; it changes it with `set`, which the owner validates and answers. | `params`, `macros`, `plugin` (loaded plugin, status, progress), `settings`, `cache`, `transport`, `ui` (view, keyboard) |
| **Command** | An action with a result | A request that returns `{ok, result}` or `{ok: false, error}`. May change stores; the change then arrives as their patches, never only in the result. | `plugin.load`, `plugin.unload`, `cache.delete`, `cache.clearCompiled`, `macros.autoMap`, `app.openUrl` |
| **Stream** | High-rate measurements, not state | Subscribed only while visible; sampled on the main thread at its own rate (e.g. 10 Hz); latest wins, nothing is replayed or persisted. | `perf` (DSP load, peak, skipped and too-slow blocks), later meters |

Parameter edits with gestures (below) are the one high-rate input; they stay their own message (`edit`), because they
go to the audio thread, not to a store's owner.

### The wire

- Host UI to core: one function, `request(name, args)`, returning a promise of `{ok, result | error}`. `name` is
  `store.get` / `store.set` / `store.subscribe` / `store.unsubscribe` with the store in `args`, a command name, or
  `stream.subscribe` / `stream.unsubscribe`.
- Core to host UI: one function, `receive(message)`, with `{kind: "patch" | "stream" | "event", topic, version, data}`.
  Patches carry the store's version; the UI applies only newer ones (no timers guessing at staleness).
- On page load the UI sends `hello {protocol}`; the core answers with its protocol version and the full stores the UI
  subscribes to. A mismatch shows an error instead of half-working.

### Settings

`settings` is a store with a declared schema: per key its type, default, range, and **scope**:

| Scope | Lives in | Example |
| --- | --- | --- |
| machine | `settings.json` (every instance, every DAW) | audio wait ratio, concealment on/off, cache budget |
| instance | the DAW session (our CLAP state) | on-screen keyboard octave, last view |
| session | memory only | the Performance graph's history |

The schema is the one place a setting is declared: the core validates `set`s against it (clamps, rejects unknown
keys), the settings page renders from it, and machine-scope changes reach the other instances in this process at
once (one shared store) and other processes when `settings.json` changes (file watch, or re-read when the window
gets focus). Values the audio thread needs (the wait ratio) are pushed to it as atomics; it never reads the store.

### Implementation shape

- C++: one registry in the core, the only dispatch the editor calls. `api.store("settings", schema, get, set)`,
  `api.command("cache.delete", handler)`, `api.stream("perf", sampler, hz)`. Handlers run on the main thread; data
  from the audio thread reaches them only through lock-free structures (the parameter table, the perf counters).
  Testable without a webview: call the registry with JSON, inspect the replies and patches.
- JS: a small client (`api.get/set/subscribe/command/onStream`) over the one binding; views subscribe to the stores
  and streams they show and unsubscribe when hidden.
- Names are namespaced (`plugin.*`, `params.*`, `macros.*`, `settings.*`, `cache.*`, `perf.*`, `transport.*`,
  `keyboard.*`, `ui.*`, `app.*`), and the list of names lives in one file per side, so a grep shows the whole API.

### Today's calls, mapped

| Today | Becomes |
| --- | --- |
| `ready` | `hello` |
| `load`, `unload` | commands `plugin.load`, `plugin.unload` (store `plugin` for status) |
| `view`, `keyboard` | store `ui` (instance scope) |
| `setMacro`, `setParam` | `edit` (gestures) |
| `paramText` | gone: texts arrive with `params` patches |
| `mapMacro`, `unmapMacro`, `renameMacro`, `autoMap` | store `macros` (`set`) and command `macros.autoMap` |
| `note` | `edit`-like input `keyboard.note` (to the audio thread) |
| `transport` | store `transport` |
| `cacheList`, `cacheDelete`, `cacheClearCompiled` | store `cache`, commands `cache.delete`, `cache.clearCompiled` |
| `settingsGet`, `settingsSet` | store `settings` |
| `perfGet` | stream `perf` |
| `log`, `openUrl` | commands `app.log`, `app.openUrl` |
| the 50 ms snapshot push | `patch`es of the subscribed stores |

## Other channels

Found in an audit of the code; each needs a place in the API above.

| Channel | Today | Decision |
| --- | --- | --- |
| **Picker iframe → host UI** (`plugincircuit:select` / `cancel` via `postMessage`) | `host-ui/app.js` accepts it from any sender: no `e.source` / `e.origin` check, so any frame in the page could make the plugin load something | Accept only from the picker iframe (`e.source === picker.contentWindow`) and the catalog's origin. The message format is a contract with the website: versioned, documented in the website's `docs/API.md` |
| **Host UI → catalog server** (`/v1/health`, the picker URL, links) | the page decides "offline" itself; the core decides it separately | An `app` store owned by the core (online state, API base); the page stops fetching `/v1/health` |
| **Native → page events** beyond the snapshot: note keys forwarded from a focused plugin GUI (`plugincircuitHost.key`) | a second function on `window` | An `event` message on the one wire (`keyboard.key`) |
| **Errors** (load failures, WebAssembly traps, the bridge's log) | the DAW's log and stderr; the UI only sees status "Error" and a message | A `plugin.errors` store (recent errors, with time and source) |
| **Notes from the DAW** shown on the on-screen keyboard | not shown | Optional stream `keyboard.notes`: the audio thread pushes note on/off into a preallocated queue, the main thread samples it |
| **Between DAW processes**: `settings.json`, package locks, `usage.json` | locks and usage work; another process sees a settings change only after a restart | Machine-scope settings: watch `settings.json` (or re-read on window focus) |
| **The WCLAP's tail** | the inner host's `clap.tail` `changed()` is a no-op, the plugin's tail is never asked for, we offer the DAW no `clap.tail`, and `processBlock` always returns `CONTINUE` (the plugin's sleep / tail status is dropped) | Offer `clap.tail` (the inner plugin's value), forward `changed()` to the DAW, pass the inner process status through (with `request_process` when the UI queues events, so a sleeping plugin wakes for on-screen notes) |
| **The WCLAP's note ports** | the inner host's `note_ports.rescan()` is a no-op | Treat it as a restart (`request_restart`): our note-dialect conversion is chosen in `activate()` |
| Latency, `mark_dirty`, timers, `on_main_thread`, editor resizing | handled | unchanged |

## Messages

### Host UI to core: requests

Every call is `call(method, args)` and returns a promise with `{ok, error?, ...}`. Commands (load, unload, view,
mapMacro, renameMacro, autoMap, cache...) are applied on the main thread and answered.

Today: calls return `true` whatever happened, so the UI can't show a failure.

### Host UI to core: parameter edits, with gestures

One message for macros and parameters alike:

```
edit { target: "macro" | "param", id, value, phase: "begin" | "update" | "end" | "set" }
```

- `begin`: pointer down. Opens the gesture and makes this UI the parameter's owner.
- `update`: while dragging, coalesced to one per animation frame.
- `end`: pointer up, with the final value. Closes the gesture.
- `set`: a one-shot change (double-click reset, typed value): `begin` + value + `end` in one.

The main thread pushes each edit into the lock-free UI-to-audio queue (preallocated, single producer). On the next
block the audio thread applies it to the WCLAP (`CLAP_EVENT_PARAM_VALUE` on the mapped parameters) and reports it to
the DAW: `PARAM_GESTURE_BEGIN`, the values, then `PARAM_GESTURE_END`, so the DAW records one automation pass. When the
DAW isn't processing, the core asks for `params.flush`, which does the same on the main thread.

Today: only macros send gestures; parameter sliders send bare values (no gestures for the DAW).

### Core to host UI: pushed updates, not polling

| Message | When | Contents |
| --- | --- | --- |
| `state` | the structure changed: plugin loaded, parameter list rescanned, mapping edited, status | the full snapshot, with a `version` |
| `values` | some values changed | only the changed parameters and macros: `{id, value, text}`. Text comes from the WCLAP's `value_to_text`, called only for these. At most 30 per second, coalesced |
| `status` | loading progress | status, message, progress |
| `ack` | a gesture ended | the committed value and text for that parameter: the owner's copy now matches the truth |

How changes are noticed: the audio thread writes values into atomics and sets a bit in a preallocated "changed"
bitset (one bit per parameter, no allocation, no lock). A main-thread timer swaps the bitset, reads those values,
formats their text and sends one `values` message. Values of a parameter the UI owns (in a gesture) are not sent to
it; the `ack` at the end closes the loop.

Every message carries a `version`. The UI applies only newer versions, which replaces today's 400 ms "recently
changed" guard.

Today: every 50 ms a snapshot with the value and text of every parameter, including one `value_to_text` call into
WebAssembly per parameter (about 2,600 per tick for Six Sines, even when nothing moves). The UI drops values for
controls it's dragging.

### WCLAP to core

- **Parameter changes from its own GUI or DSP** arrive as output events (`process` or `params.flush`), with gestures
  when the WCLAP sends them. The core forwards them to the DAW for mapped parameters (as their macro, with the
  gesture) and marks them changed for the host UI.
- **Many changes at once** (it loaded a preset): the WCLAP sends many values or asks for `rescan`. The core turns a
  rescan into one `state` message, and a burst of values into one coalesced `values` message.
- **Other state changed** (`mark_dirty`): the core forwards it to the DAW. Nothing to show in our UI.
- **Its GUI and DSP talk directly** (presets, patterns, meters): opaque bytes through webview-gui. When the core
  restores a state into the WCLAP (session load, undo), the WCLAP's DSP tells its own GUI; the core does nothing.

## Example: dragging a macro

1. Pointer down on macro 3: the UI sends `edit {macro, 3, 0.42, begin}` and becomes its owner.
2. The audio thread: DAW gets `GESTURE_BEGIN 3` and `VALUE 3 = 0.42`; the WCLAP gets the mapped parameter values.
3. Dragging: `update` once per frame; the same path. The core sends the UI no values for macro 3, but keeps sending
   changes of other parameters (e.g. another macro the DAW automates).
4. Meanwhile the DAW plays automation on macro 3: dropped for macro 3 while the gesture is open.
5. Pointer up: `edit {macro, 3, 0.57, end}`. DAW gets `VALUE 3 = 0.57` and `GESTURE_END 3`.
6. The core sends `ack {macro 3, 0.57, "57 %"}`; the UI's ownership ends, and later changes to macro 3 from anyone are
   shown again.

## Threading rules (unchanged)

- Audio thread: no allocation, locks, logging or syscalls. Queues and the changed bitset are preallocated in `activate()`.
- Main thread only: anything that calls into the WebAssembly plugin outside `process` (`value_to_text`, state,
  `params.flush`), file IO, the webviews.
- The webviews are only touched on the main thread; a WCLAP's `request_resize` from another thread is handed over (as today).

## Changes from today, in order of value

1. Push changed values only, with text only for them (the 50 ms poll of every parameter goes).
2. One edit protocol with gestures for macros and parameters, so DAW automation is recorded as gestures.
3. Ownership in the core (no echo to the gesture owner, `ack` at the end) instead of timers in the UI.
4. Versioned messages, and requests that report success or failure.
