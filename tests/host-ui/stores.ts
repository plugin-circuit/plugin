// Host UI on the one API (docs/PLAN.md, phase 3): the views render from stores and follow their patches.
//   - the plugin pill and the Params view render from the plugin and params stores (the view virtualized)
//   - a params delta patch updates a row; a stale patch (not newer) is ignored
//   - a store.set racing its patches: one built before the set doesn't undo it, the one after it applies
//   - a dragged value stays the page's until the core acknowledges the drag's end
//   - the Advanced tab renders from the settings schema; a change is a settings set
//   - the keyboard octave is the ui store's (instance scope): it follows the store and sets it
//   - offline / online come from the app store; Try again asks the core to check
//   - keyboard.key events play notes; keyboard.notes events light the DAW's keys
//   - a stopped plugin (moduleError) shows the card with plugin.errors; Retry reloads it
//   - the settings page subscribes to its stores and stream only while shown
//
// Run: bun tests/host-ui/stores.ts   (uses Playwright from ../website/node_modules)
import { join } from "node:path"
import { defaultSchema, defaultStores, installMockHost, serveHostUi } from "./mock_host"

const playwright = await import(join(import.meta.dir, "../../../website/node_modules/@playwright/test/index.mjs"))
const host = serveHostUi(join(import.meta.dir, "../../host-ui"))
const browser = await playwright.chromium.launch()
const page = await browser.newPage()

let failures = 0
const check = (ok: boolean, what: string, got?: unknown) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${ok ? "" : `  (got ${JSON.stringify(got)})`}`)
  if (!ok) failures++
}

// a loaded synth with 300 parameters
const stores: any = defaultStores("http://127.0.0.1:9") // no catalog: nothing in these checks needs it
stores.plugin = { ...stores.plugin, status: "ready", plugin: { id: "test.synth", name: "Test Synth", vendor: "Tests", hasGui: false, latency: 0, noteInput: true } }
const infos = Array.from({ length: 300 }, (_, i) => ({ id: i, name: `Param ${i}`, module: i < 150 ? "A" : "B", min: 0, max: 1, default: 0, stepped: false, automatable: true, hidden: false }))
stores.params = { full: true, infos, values: infos.map((p) => ({ id: p.id, value: 0.5, text: `v${p.id}` })) }
stores.ui = { view: "params", keyboard: false, innerGui: false }

await page.addInitScript(installMockHost, { stores, schema: defaultSchema })
await page.goto(`http://127.0.0.1:${host.port}/`)
await page.waitForTimeout(500)

const slot = await page.textContent("#slotName")
check(slot === "Test Synth", "the plugin pill shows the plugin store's name", slot)
const rows = await page.evaluate(() => document.querySelectorAll("#params .param").length)
check(rows > 0 && rows < 100, "the Params view renders only the rows in view (300 parameters)", rows)
const first = await page.evaluate(() => document.querySelector('#params .param[data-id="0"] .ptext')?.textContent)
check(first === "v0", "a row shows its text from the params store", first)
const visible = await page.evaluate(() => (window as any).__requests.filter((r: any) => r.name === "params.visible").pop()?.args.ids.length)
check(visible > 0 && visible === rows, "the page tells the core which parameters are visible", visible)

// a delta, then a stale one
await page.evaluate(() => (window as any).__patch("params", { full: false, values: [{ id: 0, value: 0.9, text: "new" }] }, false))
const afterDelta = await page.evaluate(() => document.querySelector('#params .param[data-id="0"] .ptext')?.textContent)
check(afterDelta === "new", "a delta patch updates the row", afterDelta)
await page.evaluate(() => {
  const w = window as any
  w.plugincircuitReceive({ kind: "patch", topic: "params", version: w.__versions.params - 1, full: false, data: { full: false, values: [{ id: 0, value: 0.1, text: "stale" }] } })
})
const afterStale = await page.evaluate(() => document.querySelector('#params .param[data-id="0"] .ptext')?.textContent)
check(afterStale === "new", "a stale patch is ignored", afterStale)

// a drag's end (PLAN 2.4): the dragged value stays the page's until the core reports the end applied
const box = await page.locator('#params .param[data-id="1"] .slider').boundingBox()
await page.mouse.move(box.x + box.width * 0.5, box.y + box.height / 2)
await page.mouse.down()
await page.mouse.move(box.x + box.width * 0.8, box.y + box.height / 2, { steps: 4 })
await page.mouse.up()
const valueOf1 = () => page.evaluate(() => (0, eval)("state.paramValues.get(1).value"))
const dragged = await valueOf1()
await page.waitForTimeout(300) // longer than the 120 ms the page used to wait
await page.evaluate(() => (window as any).__patch("params", { full: false, values: [{ id: 1, value: 0.5, text: "old" }] }, false))
check(Math.abs(dragged - 0.8) < 0.05 && (await valueOf1()) === dragged, "a value from before the end is applied doesn't undo the drag", [dragged, await valueOf1()])
await page.evaluate(() => (window as any).__patch("params", { full: false, ended: [{ target: "param", id: 1 }], values: [{ id: 1, value: 0.3, text: "plugin" }] }, false))
check((await valueOf1()) === 0.3, "after the core's ack the value is the plugin's again", await valueOf1())
const ends = await page.evaluate(() => (window as any).__requests.filter((r: any) => r.name === "edit" && r.args.id === 1).map((r: any) => r.args.phase))
check(ends[0] === "begin" && ends[ends.length - 1] === "end", "the drag sends begin ... end", ends)

// switching the view: the core has a patch from before the set in flight (view "params"), answers the set, then
// sends the patch after it (view "macros", and more: innerGui)
const views = await page.evaluate(async () => {
  const w = window as any
  const seen: string[] = []
  new MutationObserver(() => seen.push(document.body.dataset.view!)).observe(document.body, { attributes: true, attributeFilter: ["data-view"] })
  w.__onSet = (args: any) => {
    w.__onSet = null
    w.__patch("ui", { view: "params", keyboard: false, innerGui: false }) // built before the set
    const version = w.__versions.ui
    setTimeout(() => w.__patch("ui", { view: "macros", keyboard: false, innerGui: true }), 0) // after it
    return new Promise((r) => setTimeout(() => r({ ok: true, result: { version } }), 30)) // the reply comes last
  }
  ;(window as any).setView("macros")
  await new Promise((r) => setTimeout(r, 120))
  return { seen, view: document.body.dataset.view, innerGui: (0, eval)("state.snap.innerGui") }
})
check(!views.seen.includes("params"), "a patch from before the set doesn't undo it", views.seen)
check(views.view === "macros" && views.innerGui === true, "the patch after the set applies (it carries more than the view)", views)

// the settings page: its stores and stream only while shown
const subs = await page.evaluate(async () => {
  const w = window as any
  const names = () => w.__requests.map((r: any) => r.name + (r.args.store || r.args.stream ? ":" + (r.args.store || r.args.stream) : ""))
  const before = names().length
  w.setView("settings")
  await new Promise((r) => setTimeout(r, 50))
  w.setView("macros")
  await new Promise((r) => setTimeout(r, 50))
  return names().slice(before)
})
for (const want of ["store.get:cache", "store.get:settings", "stream.subscribe:perf", "store.unsubscribe:cache", "store.unsubscribe:settings", "stream.unsubscribe:perf"])
  check(subs.includes(want), `settings page: ${want}`, subs)

// the Advanced tab from the schema
const advanced = await page.evaluate(async () => {
  const w = window as any
  w.setView("settings")
  await new Promise((r) => setTimeout(r, 50))
  const sel = document.querySelector("#setting-audioWaitRatio") as HTMLSelectElement
  const box = document.querySelector("#setting-concealDropouts") as HTMLInputElement
  const result = { options: sel ? [...sel.options].map((o) => o.textContent) : null, value: sel?.value, checked: box?.checked }
  sel.value = "0.5"
  sel.dispatchEvent(new Event("change"))
  box.click()
  await new Promise((r) => setTimeout(r, 30))
  const sets = w.__requests.filter((r: any) => r.name === "store.set" && r.args.store === "settings").map((r: any) => r.args.value)
  w.setView("params")
  return { ...result, sets }
})
check(advanced.options?.length === 10 && advanced.options[0] === "Off (try once)" && advanced.options[2] === "20% of a block (default)",
  "the Advanced tab renders a number setting from the schema", advanced.options)
check(advanced.value === "0.2" && advanced.checked === true, "its controls show the settings store's values", advanced)
check(JSON.stringify(advanced.sets) === JSON.stringify([{ audioWaitRatio: 0.5 }, { concealDropouts: false }]), "a change is a settings set", advanced.sets)

// the keyboard octave: from the ui store, and set to it
const octave = await page.evaluate(async () => {
  const w = window as any
  w.__patch("ui", { ...w.__stores.ui, keyboardBase: 60 })
  await new Promise((r) => setTimeout(r, 20))
  const fromStore = (0, eval)("kbd.base")
  ;(document.querySelector("#octDown") as HTMLElement).click()
  await new Promise((r) => setTimeout(r, 20))
  const sets = w.__requests.filter((r: any) => r.name === "store.set" && r.args.store === "ui" && "keyboardBase" in r.args.value)
  return { fromStore, after: (0, eval)("kbd.base"), set: sets.pop()?.args.value }
})
check(octave.fromStore === 60, "the keyboard's octave follows the ui store", octave)
check(octave.after === 48 && octave.set?.keyboardBase === 48, "an octave shift sets the ui store", octave)

// the catalog's reachability is the app store's (PLAN 5.1): no /v1/health from the page
const catalog = await page.evaluate(async () => {
  const w = window as any
  const sleep = (t: number) => new Promise((r) => setTimeout(r, t))
  w.setView("browse")
  w.__patch("app", { online: false, apiBase: "http://127.0.0.1:9", error: "refused" })
  await sleep(20)
  const offline = document.body.classList.contains("offline")
  const checks = () => w.__requests.filter((r: any) => r.name === "app.checkCatalog").length
  const before = checks()
  ;(document.querySelector("#retryCatalog") as HTMLElement).click()
  const retried = checks() === before + 1
  w.__patch("app", { online: true, apiBase: "http://127.0.0.1:9", error: "" })
  await sleep(20)
  const picker = (document.querySelector("#picker") as HTMLIFrameElement).getAttribute("src") || ""
  w.setView("params")
  return { offline, retried, back: !document.body.classList.contains("offline"), picker }
})
check(catalog.offline && catalog.retried, "offline from the app store; Try again asks the core", catalog)
check(catalog.back && catalog.picker.startsWith("http://127.0.0.1:9/select"), "online again: the picker loads", catalog)

// events (PLAN 5.3, 5.4): a note key typed into the plugin's GUI plays; the DAW's notes light the keyboard
const keys = await page.evaluate(async () => {
  const w = window as any
  const sleep = (t: number) => new Promise((r) => setTimeout(r, t))
  w.__patch("ui", { ...w.__stores.ui, keyboard: true, keyboardBase: 48 })
  await sleep(20)
  w.plugincircuitReceive({ kind: "event", topic: "keyboard.key", data: { code: "KeyA", down: true } })
  w.plugincircuitReceive({ kind: "event", topic: "keyboard.key", data: { code: "KeyA", down: false } })
  const notes = w.__requests.filter((r: any) => r.name === "keyboard.note").map((r: any) => `${r.args.key}:${r.args.on}`)
  w.plugincircuitReceive({ kind: "event", topic: "keyboard.notes", data: [{ key: 50, on: true }, { key: 52, on: true }, { key: 52, on: false }] })
  const lit = [...document.querySelectorAll("#keys .key.daw")].map((e: any) => +e.dataset.key)
  w.plugincircuitReceive({ kind: "event", topic: "keyboard.notes", data: [{ key: -1, on: false }] })
  const cleared = document.querySelectorAll("#keys .key.daw").length
  return { notes, lit, cleared }
})
check(JSON.stringify(keys.notes) === JSON.stringify(["48:1", "48:0"]), "a keyboard.key event plays the note", keys.notes)
check(JSON.stringify(keys.lit) === JSON.stringify([50]), "keyboard.notes events light the DAW's keys", keys.lit)
check(keys.cleared === 0, "an all-off clears them", keys.cleared)

// a plugin that stopped (its module trapped, PLAN 5.2): the card says so, lists the errors, and Retry reloads it
const crash = await page.evaluate(async () => {
  const w = window as any
  const sleep = (t: number) => new Promise((r) => setTimeout(r, t))
  w.setView("plugin")
  w.__patch("plugin.errors", { errors: [{ time: Date.now(), source: "plugin", message: "WCLAP function call threw (trapped)" },
    { time: Date.now() - 1000, source: "bridge", message: "wasm trap: unreachable" }] })
  w.__patch("plugin", { ...w.__stores.plugin, moduleError: "WCLAP function call threw (trapped)" })
  await sleep(20)
  const shown = document.body.classList.contains("missing")
  const title = document.querySelector("#missingTitle")!.textContent
  const items = [...document.querySelectorAll("#errorList li")].map((li) => li.textContent)
  const details = !(document.querySelector("#errorDetails") as HTMLElement).hidden
  ;(document.querySelector('#missing [data-goto="browse"]') as HTMLElement).click() // Choose another
  const overBrowse = getComputedStyle(document.querySelector("#missing")!).display !== "none"
  w.setView("plugin")
  ;(document.querySelector("#retryLoad") as HTMLElement).click()
  const load = w.__requests.filter((r: any) => r.name === "plugin.load").pop()?.args
  w.__patch("plugin", { ...w.__stores.plugin, moduleError: "" })
  await sleep(20)
  return { shown, title, items, details, overBrowse, load, gone: !document.body.classList.contains("missing") }
})
check(crash.shown && crash.title === "Test Synth stopped", "a stopped plugin shows the card", crash)
check(crash.details && crash.items.length === 2 && crash.items[0]!.includes("plugin") && crash.items[0]!.includes("trapped"), "its errors are listed, newest first", crash.items)
check(crash.load?.pluginId === "test.synth", "Retry loads it again", crash.load)
check(!crash.overBrowse, "Choose another: the card doesn't cover Browse", crash)
check(crash.gone, "the card goes once it runs again", crash)

console.log(failures ? `${failures} failure(s)` : "PASS: the host UI follows its stores")
await browser.close()
host.stop()
process.exit(failures ? 1 : 0)
