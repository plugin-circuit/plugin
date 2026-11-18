// Builds resources/PluginCircuit.icns (the standalone apps' icon) from the website's app icon
// (website/design/logo/plugin-circuit-icon.svg): rendered in headless Chromium at macOS's icon grid (the rounded
// square 824 px inside a transparent 1024 px canvas), then every iconset size via sips, packed by iconutil.
// Run: bun tools/app-icon.ts   (uses Playwright from ../website/node_modules)
import { mkdirSync, readFileSync, rmSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dir, "..")
const svg = readFileSync(join(root, "../website/design/logo/plugin-circuit-icon.svg"), "utf8")
const work = join(root, "build/app-icon")
const iconset = join(work, "PluginCircuit.iconset")
rmSync(work, { recursive: true, force: true })
mkdirSync(iconset, { recursive: true })

const pw = await import(join(root, "../website/node_modules/@playwright/test/index.mjs"))
const browser = await pw.chromium.launch()
const page = await browser.newPage({ viewport: { width: 1024, height: 1024 }, deviceScaleFactor: 1 })
await page.setContent(
  `<html><body style="margin:0;background:transparent"><div style="width:1024px;height:1024px;display:grid;place-items:center">` +
    `<div style="width:824px;height:824px;filter:drop-shadow(0 10px 14px rgba(0,0,0,.28))">${svg.replace("<svg ", '<svg width="824" height="824" ')}</div></div></body></html>`,
)
const master = join(work, "icon_1024.png")
await page.screenshot({ path: master, omitBackground: true })
await browser.close()

for (const size of [16, 32, 128, 256, 512]) {
  for (const scale of [1, 2]) {
    const px = size * scale
    const name = `icon_${size}x${size}${scale === 2 ? "@2x" : ""}.png`
    const r = Bun.spawnSync(["sips", "-z", String(px), String(px), master, "--out", join(iconset, name)])
    if (r.exitCode !== 0) throw new Error(`sips failed for ${name}`)
  }
}
const out = join(root, "resources/PluginCircuit.icns")
const r = Bun.spawnSync(["iconutil", "-c", "icns", iconset, "-o", out])
if (r.exitCode !== 0) throw new Error(`iconutil: ${r.stderr.toString()}`)
console.log(`wrote ${out}`)
