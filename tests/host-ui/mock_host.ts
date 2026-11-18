// A fake core for host-UI tests: the page's one binding (plugincircuitRequest) answered from in-page stores, and
// window.__patch(topic, data, full?, version?) to send patches as the core would (docs/MESSAGING.md, "One API").
// Every request is recorded in window.__requests. Install with page.addInitScript(installMockHost, config).
export type MockConfig = { apiBase: string; stores?: Record<string, unknown> }

export function defaultStores(apiBase: string) {
  return {
    app: { online: true, apiBase, error: "" },
    "plugin.errors": { errors: [] },
    plugin: { status: "empty", message: "", progress: 0, instrument: true, apiBase, plugin: null, missing: null, installed: [] },
    macros: {
      macros: Array.from({ length: 64 }, () => ({ name: "", custom: false, targets: [] })),
      autoMapped: true,
      values: Array(64).fill(0),
    },
    params: { full: true, infos: [], values: [] },
    transport: { host: true, playing: false },
    ui: { view: "browse", keyboard: false, innerGui: false, keyboardBase: 48 },
    settings: { audioWaitRatio: 0.2, concealDropouts: true },
    cache: { packages: [], packagesBytes: 0, compiledBytes: 0 },
  }
}

// what settings.schema answers (the core's is src/api/SettingsSchema.cpp): a number with steps and a bool
export const defaultSchema = [
  { key: "audioWaitRatio", type: "number", label: "Audio wait", help: "Waits.", default: 0.2, min: 0, max: 0.9, step: 0.1,
    format: "percent", suffix: " of a block", zeroLabel: "Off (try once)" },
  { key: "concealDropouts", type: "bool", label: "Smooth over skipped blocks", help: "Smooths.", default: true },
]

export function installMockHost(cfg: { stores: Record<string, any>; schema?: unknown[] }) {
  const w = window as any
  const stores = cfg.stores
  const versions: Record<string, number> = {}
  w.__requests = []
  w.__stores = stores
  w.__versions = versions
  // a hook a test sets to act inside a store.set (before its reply), e.g. to deliver patches in flight
  w.__onSet = null
  w.plugincircuitRequest = (name: string, args: any) => {
    w.__requests.push({ name, args })
    const ok = (result: unknown) => Promise.resolve({ ok: true, result })
    if (name === "hello") return ok({ protocol: 1 })
    if (name === "store.get") {
      versions[args.store] = (versions[args.store] || 0) + 1
      return ok({ version: versions[args.store], value: stores[args.store] })
    }
    if (name === "store.set") {
      if (w.__onSet) return w.__onSet(args)
      Object.assign(stores[args.store], args.value)
      const version = versions[args.store] || 0
      setTimeout(() => w.__patch(args.store, stores[args.store]), 0) // the change follows as a patch
      return ok({ version })
    }
    if (name === "params.text") return ok("text")
    if (name === "settings.schema") return ok(cfg.schema ?? [])
    return ok(true)
  }
  w.__patch = (topic: string, data: unknown, full = true, version?: number) => {
    versions[topic] = version ?? (versions[topic] || 0) + 1
    w.plugincircuitReceive({ kind: "patch", topic, version: versions[topic], full, data })
  }
}

// host-ui/ from a local server
export function serveHostUi(root: string) {
  return Bun.serve({
    port: 0,
    fetch(req) {
      const path = new URL(req.url).pathname
      return new Response(Bun.file(root + (path === "/" ? "/index.html" : path)))
    },
  })
}
