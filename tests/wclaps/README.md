# Test WCLAPs

For the upload checker and for Plugin Circuit's own tests (moved here from the prototype). CMake builds them into
`build/test-wclaps/dist/` when Homebrew LLVM and `wasm-ld` are installed; by hand: `OUT=<dir> bash build.sh`.

Four tiny WCLAPs written in freestanding C, with no libc and no WASI imports. Each exports `memory`, `clap_entry`,
`malloc`, `free` and a growable `__indirect_function_table`, which is what both the native wclap-bridge and the JS hosts
need. Each has one parameter (dB, with value-to-text), state, audio ports, `clap.latency` and a unique id
`studio.plugincircuit.test.<name>`, vendor "Plugin Circuit Tests", version 1.0.0.

```sh
just test-wclaps          # builds dist/*.wclap.tar.gz (needs brew llvm + lld)
node examples/test-wclaps/check-js.mjs   # quick check in a JS host (Node)
```

| Bundle | What it is | What the checker should report |
|---|---|---|
| `test-gain.wclap.tar.gz` | Stereo gain with a webview GUI (`/ui/index.html` from the bundle; `get_resource` returns false, so the host serves the file). The slider talks to the plugin through `postMessage`. | Everything passes: latency 0 reported, 0 measured. State round trip OK. GUI renders and answers the page's hello. |
| `test-lookahead.wclap.tar.gz` | Stereo effect that delays the audio by 128 samples. | **Latency mismatch:** reports 0, measures 128. |
| `test-sine.wclap.tar.gz` | Polyphonic sine instrument with a 5 ms attack. Note events are applied at their sample offset. | Passes: note at sample 100 sounds at sample 100 (delay 0). |
| `test-broken-state.wclap.tar.gz` | Stereo gain whose `state.load` always returns false. | **State round trip fails.** |
| `test-param-events.wclap.tar.gz` | Instrument that changes its own Level like a GUI: note-on key 60 sets -6 dB and reports it (gesture begin, value, gesture end); key 61 sets -30 dB without reporting; its page's "set gain" reports with a gesture. | For host tests (`tests/plugin_param_events_test.cpp`). |

GUI page protocol (little endian):
- Page to plugin: `[0]` hello, `[1, f32 dB]` set gain.
- Plugin to page: `[2, f32 dB]` current gain. It's sent on hello, and after the host changes the value (automation or
  state load).

Results when built (`plugincircuit-smoke` natively, `check-js.mjs` in Node). Both hosts agree:

| Plugin | reported / measured latency | state round trip | webview |
|---|---|---|---|
| test-gain | 0 / 0 | ok | `/ui/index.html` served from the bundle |
| test-lookahead | 0 / **128** | ok | none |
| test-sine | 0 / 0 | ok | none |
| test-broken-state | 0 / 0 | **load fails** | none |
