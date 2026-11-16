// Host UI: only the catalog's picker iframe may make the plugin load something (phase 0.1 of docs/PLAN.md).
//
// Serves host-ui/ from one local server, a fake catalog (health + a picker page that posts plugincircuit:select to its
// parent) from a second, and a rogue page from a third that posts the same message. Checks with a headless Chromium
// that only the picker's message reaches the native "load" call.
//
// Run: bun tests/host-ui/picker_messages.ts   (uses Playwright from ../website/node_modules)
import { join } from "node:path"
import { defaultStores, installMockHost, serveHostUi } from "./mock_host"

const playwright = await import(join(import.meta.dir, "../../../website/node_modules/@playwright/test/index.mjs"))
const root = join(import.meta.dir, "../../host-ui")

const host = serveHostUi(root)
const select = (id: string) => `<script>parent.postMessage({type: "plugincircuit:select", pluginId: "${id}"}, "*")</script>`
const catalog = Bun.serve({
  port: 0,
  fetch(req) {
    const path = new URL(req.url).pathname
    const cors = { "Access-Control-Allow-Origin": "*" }
    if (path === "/v1/health") return new Response("ok", { headers: cors })
    if (path === "/select") return new Response(select("from.picker"), { headers: { "Content-Type": "text/html", ...cors } })
    return new Response("not found", { status: 404 })
  },
})
const rogue = Bun.serve({
  port: 0,
  fetch: () => new Response(select("from.rogue"), { headers: { "Content-Type": "text/html" } }),
})

const browser = await playwright.chromium.launch()
const page = await browser.newPage()
// the core: no plugin, the Browse view, the fake catalog
await page.addInitScript(installMockHost, { stores: defaultStores(`http://127.0.0.1:${catalog.port}`) })
await page.goto(`http://127.0.0.1:${host.port}/`)

// a rogue frame from another origin, and a same-origin frame that isn't the picker, both send select
await page.evaluate((src: string) => {
  const f = document.createElement("iframe")
  f.src = src
  document.body.appendChild(f)
}, `http://127.0.0.1:${rogue.port}/`)
await page.evaluate(() => {
  const f = document.createElement("iframe")
  f.srcdoc = `<script>parent.postMessage({type: "plugincircuit:select", pluginId: "from.same-origin"}, "*")</script>`
  document.body.appendChild(f)
})
await page.waitForTimeout(1500) // the picker loads after the health check

const loads: string[] = await page.evaluate(() =>
  (window as any).__requests.filter((r: any) => r.name === "plugin.load").map((r: any) => r.args.pluginId))
const ok = loads.length === 1 && loads[0] === "from.picker"
console.log(`loads: ${JSON.stringify(loads)}`)
console.log(ok ? "PASS: only the picker's message loaded a plugin" : "FAIL: expected exactly [\"from.picker\"]")
await browser.close()
host.stop()
catalog.stop()
rogue.stop()
process.exit(ok ? 0 : 1)
