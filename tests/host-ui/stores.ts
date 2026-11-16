// Host UI on the one API (docs/PLAN.md, phase 3): the views render from stores and follow their patches.
//   - the plugin pill and the Params view render from the plugin and params stores (the view virtualized)
//   - a params delta patch updates a row; a stale patch (not newer) is ignored
//   - a store.set racing its patches: one built before the set doesn't undo it, the one after it applies
//   - the settings page subscribes to its stores and stream only while shown
//
// Run: bun tests/host-ui/stores.ts   (uses Playwright from ../website/node_modules)
import { join } from "node:path"
import { defaultStores, installMockHost, serveHostUi } from "./mock_host"

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

await page.addInitScript(installMockHost, { stores })
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

console.log(failures ? `${failures} failure(s)` : "PASS: the host UI follows its stores")
await browser.close()
host.stop()
process.exit(failures ? 1 : 0)
