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
