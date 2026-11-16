// Loads each dist/*.wclap.tar.gz the way the open-webclap JS host does (no WASI, trampolines installed into the
// plugin's growable function table, wasm32 struct offsets) and checks: create/init/activate, an impulse or note at
// sample 100 (measured delay vs latency.get), and a state save -> load round trip.
// Usage: node examples/test-wclaps/check-js.mjs [bundle.wclap.tar.gz ...]
import { readFileSync, readdirSync } from "node:fs"
import { gunzipSync } from "node:zlib"
import { dirname, join } from "node:path"
import { fileURLToPath } from "node:url"

const here = dirname(fileURLToPath(import.meta.url))
const FRAMES = 256, AT = 100

function untar(buf) {
  const files = new Map()
  for (let off = 0; off + 512 <= buf.length;) {
    const name = buf.subarray(off, off + 100).toString("latin1").replace(/\0.*$/s, "")
    if (!name) break
    const prefix = buf.subarray(off + 345, off + 500).toString("latin1").replace(/\0.*$/s, "")
    const size = parseInt(buf.subarray(off + 124, off + 136).toString("latin1").trim() || "0", 8)
    const type = String.fromCharCode(buf[off + 156] || 48)
    if (type === "0") files.set(prefix ? `${prefix}/${name}` : name, buf.subarray(off + 512, off + 512 + size))
    off += 512 + Math.ceil(size / 512) * 512
  }
  return files
}

const VALTYPE = { i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c }
function trampoline(params, results, fn) {
  const name = (s) => [s.length, ...[...s].map((c) => c.charCodeAt(0))]
  const section = (id, bytes) => [id, bytes.length, ...bytes]
  const type = [1, 0x60, params.length, ...params.map((t) => VALTYPE[t]), results.length, ...results.map((t) => VALTYPE[t])]
  const bytes = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...section(1, type),
    ...section(2, [1, ...name("e"), ...name("f"), 0, 0]), ...section(7, [1, ...name("f"), 0, 0])])
  return new WebAssembly.Instance(new WebAssembly.Module(bytes), { e: { f: fn } }).exports.f
}

async function check(path) {
  const files = untar(gunzipSync(readFileSync(path)))
  const wasmPath = [...files.keys()].find((f) => f.endsWith("/module.wasm"))
  const module = await WebAssembly.compile(files.get(wasmPath))
  if (WebAssembly.Module.imports(module).length) throw new Error("unexpected imports")
  const { exports: x } = await WebAssembly.instantiate(module, {})
  const mem = x.memory, table = x.__indirect_function_table
  const v = () => new DataView(mem.buffer), u8 = () => new Uint8Array(mem.buffer)
  const u32 = (p) => v().getUint32(p, true)
  const fn = (p) => table.get(u32(p))
  const cstr = (p) => { const b = u8(); let e = p; while (b[e]) e++; return Buffer.from(b.subarray(p, e)).toString() }
  const str = (s) => { const b = Buffer.from(s + "\0"); const p = x.malloc(b.length); u8().set(b, p); return p }
  const install = (ptr, params, results, f) => { const i = table.grow(1); table.set(i, trampoline(params, results, f)); v().setUint32(ptr, i, true) }

  const entry = x.clap_entry.value
  if (!fn(entry + 12)(str("/"))) throw new Error("entry.init failed")
  const factory = fn(entry + 20)(str("clap.plugin-factory"))
  const desc = fn(factory + 4)(factory, 0)
  const id = cstr(u32(desc + 12))
  const features = []
  for (let p = u32(desc + 44); u32(p); p += 4) features.push(cstr(u32(p)))
  const instrument = features.includes("instrument")

  const host = x.malloc(48)
  for (const [o, val] of [[0, 1], [4, 2], [8, 0], [12, 0]]) v().setUint32(host + o, val, true)
  const strings = ["check-js", "plugincircuit", "", "0"].map(str)
  strings.forEach((p, i) => v().setUint32(host + 16 + i * 4, p, true))
  install(host + 32, ["i32", "i32"], ["i32"], () => 0)
  for (const o of [36, 40, 44]) install(host + o, ["i32"], [], () => {})

  const plugin = fn(factory + 8)(factory, host, str(id))
  if (!plugin || !fn(plugin + 8)(plugin)) throw new Error("create/init failed")
  const ext = (name) => fn(plugin + 40)(plugin, str(name))
  const latencyExt = ext("clap.latency"), stateExt = ext("clap.state"), gui = ext("clap.gui"), webview = ext("clap.webview/3")
  if (!fn(plugin + 16)(plugin, 48000, 1, FRAMES)) throw new Error("activate failed")
  fn(plugin + 24)(plugin)
  const reported = latencyExt ? fn(latencyExt)(plugin) : 0

  // buffers + process struct
  const chans = (n) => Array.from({ length: n }, () => x.malloc(FRAMES * 4))
  const inCh = chans(2), outCh = chans(2)
  const buf = (ch) => { const data = x.malloc(8); ch.forEach((c, i) => v().setUint32(data + i * 4, c, true)); const b = x.malloc(24)
    u8().fill(0, b, b + 24); v().setUint32(b, data, true); v().setUint32(b + 8, 2, true); return b }
  const inBuf = buf(inCh), outBuf = buf(outCh)
  let events = []
  const evSlot = x.malloc(48)
  const inEvents = x.malloc(12)
  install(inEvents + 4, ["i32"], ["i32"], () => events.length)
  install(inEvents + 8, ["i32", "i32"], ["i32"], (_l, i) => {
    const e = events[i]; u8().fill(0, evSlot, evSlot + 48)
    const d = v(); d.setUint32(evSlot, 40, true); d.setUint32(evSlot + 4, e.time, true); d.setUint16(evSlot + 10, 0, true)
    d.setInt32(evSlot + 16, -1, true); d.setInt16(evSlot + 20, 0, true); d.setInt16(evSlot + 22, 0, true)
    d.setInt16(evSlot + 24, 60, true); d.setFloat64(evSlot + 32, 1, true); return evSlot })
  const outEvents = x.malloc(8)
  install(outEvents + 4, ["i32", "i32"], ["i32"], () => 1)
  const proc = x.malloc(40)
  let steady = 0n
  const run = (impulse) => {
    const d = v()
    for (const c of inCh) new Float32Array(mem.buffer, c, FRAMES).fill(0)
    if (impulse) for (const c of inCh) new Float32Array(mem.buffer, c, FRAMES)[AT] = 1
    d.setBigUint64(proc, steady, true); d.setUint32(proc + 8, FRAMES, true); d.setUint32(proc + 12, 0, true)
    d.setUint32(proc + 16, instrument ? 0 : inBuf, true); d.setUint32(proc + 20, outBuf, true)
    d.setUint32(proc + 24, instrument ? 0 : 1, true); d.setUint32(proc + 28, 1, true)
    d.setUint32(proc + 32, inEvents, true); d.setUint32(proc + 36, outEvents, true)
    fn(plugin + 36)(plugin, proc)
    steady += BigInt(FRAMES)
    events = []
    return outCh.map((c) => new Float32Array(mem.buffer, c, FRAMES).slice())
  }
  for (let b = 0; b < 8; b++) run(false)
  let delay = -1
  for (let b = 0; b < 20 && delay < 0; b++) {
    if (b === 0 && instrument) events = [{ time: AT }]
    const [l, r] = run(b === 0 && !instrument)
    for (let i = b === 0 ? AT : 0; i < FRAMES && delay < 0; i++) if (Math.abs(l[i]) > 1e-4 || Math.abs(r[i]) > 1e-4) delay = b * FRAMES + i - AT
  }

  // state round trip
  let chunks = []
  const os = x.malloc(8)
  install(os + 4, ["i32", "i32", "i64"], ["i64"], (_s, p, n) => { chunks.push(u8().slice(p, p + Number(n))); return n })
  const saved = !!fn(stateExt)(plugin, os)
  const blob = Buffer.concat(chunks)
  let cursor = 0
  const is = x.malloc(8)
  install(is + 4, ["i32", "i32", "i64"], ["i64"], (_s, p, n) => { const k = Math.min(Number(n), blob.length - cursor)
    u8().set(blob.subarray(cursor, cursor + k), p); cursor += k; return BigInt(k) })
  const loaded = saved && !!fn(stateExt + 4)(plugin, is)

  let uri = null
  if (gui && webview) { const t = x.malloc(256); const n = fn(webview)(plugin, t, 256); uri = n > 0 ? cstr(t) : "(error)" }
  return { id, instrument, reportedLatency: reported, measuredDelay: delay, stateSaved: saved, stateLoaded: loaded,
    webviewUri: uri, uiFile: uri ? files.has(wasmPath.replace(/module\.wasm$/, uri.slice(1))) : null }
}

const args = process.argv.slice(2)
const bundles = args.length ? args : readdirSync(join(here, "dist")).filter((f) => f.endsWith(".tar.gz")).map((f) => join(here, "dist", f))
for (const b of bundles) {
  try { console.log(JSON.stringify(await check(b))) } catch (e) { console.log(JSON.stringify({ bundle: b, error: String(e) })) }
}
