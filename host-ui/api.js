// The page's side of the one API (docs/MESSAGING.md, "One API"; the core's side is src/api/Api.h).
//   to the core:   plugincircuitRequest(name, args) -> Promise<{ok, result} | {ok: false, error}>
//   from the core: plugincircuitReceive({kind: "patch" | "stream", topic, version, full, data})
//
//   api.hello()                       the handshake; rejects on a protocol mismatch
//   api.subscribe(store, fn)          fn(data, {full, version}) with the whole value first, then each patch
//                                     (`full`: data is the whole value; else a delta, as the store defines it)
//   api.set(store, value)             validated by the store's owner; the change arrives as a patch
//   api.command(name, args)           the result, or throws the error
//   api.onStream(stream, fn)          fn(sample) while subscribed
// Every subscribe / onStream returns its unsubscribe.
"use strict";

window.api = (() => {
  const PROTOCOL = 1;
  const stores = new Map(); // name -> {version, listeners, pending: patches before the get's reply, sets: in flight}
  const streams = new Map(); // name -> listeners

  function raw(name, args) {
    if (typeof window.plugincircuitRequest !== "function") return Promise.resolve({ ok: false, error: "no host" });
    return Promise.resolve(window.plugincircuitRequest(name, args || {}));
  }
  async function request(name, args) {
    const r = await raw(name, args);
    if (!r || !r.ok) throw new Error((r && r.error) || `${name}: no reply`);
    return r.result;
  }

  async function get(name, s) {
    s.version = null; // patches until the reply are held: one sent after the get can arrive before its reply
    s.pending = [];
    const r = await request("store.get", { store: name });
    s.version = r.version;
    for (const fn of s.listeners) fn(r.value, { full: true, version: r.version });
    const held = s.pending;
    s.pending = [];
    for (const m of held) deliver(name, s, m);
  }

  function deliver(name, s, m) {
    if (s.version === null) { s.pending.push(m); return; }
    if (m.version <= s.version) return; // older than what we have
    // While one of our own sets is in flight, a patch may predate it (it would undo it) or follow it (it may carry
    // more: switching the view creates the plugin's GUI). Hold the newest; the set's reply tells which it is.
    if (s.sets > 0) { s.held = m; return; }
    s.version = m.version;
    for (const fn of s.listeners) fn(m.data, { full: !!m.full, version: m.version });
  }
  function setDone(name, s, version) {
    if (--s.sets > 0) return;
    const held = s.held;
    s.held = null;
    if (held && version !== undefined && held.version > version) deliver(name, s, held); // it follows the set
  }

  return {
    PROTOCOL,
    request,
    async hello() {
      return request("hello", { protocol: PROTOCOL });
    },
    subscribe(name, fn) {
      let s = stores.get(name);
      if (!s) stores.set(name, (s = { version: null, listeners: new Set(), pending: [], sets: 0, held: null }));
      s.listeners.add(fn);
      if (s.listeners.size === 1) get(name, s).catch((e) => console.error(e));
      else request("store.get", { store: name }).then((r) => fn(r.value, { full: true, version: r.version })).catch(() => {});
      return () => {
        s.listeners.delete(fn);
        if (s.listeners.size === 0) { stores.delete(name); raw("store.unsubscribe", { store: name }); }
      };
    },
    async set(name, value) {
      const s = stores.get(name);
      if (s) s.sets++;
      let r;
      try {
        r = await request("store.set", { store: name, value });
      } catch (e) {
        if (s) { setDone(name, s, undefined); get(name, s).catch(() => {}); } // refused: the owner's value is the truth
        throw e;
      }
      if (s) setDone(name, s, r && r.version);
      return r;
    },
    command: request,
    onStream(name, fn) {
      let l = streams.get(name);
      if (!l) { streams.set(name, (l = new Set())); raw("stream.subscribe", { stream: name }); }
      l.add(fn);
      return () => {
        l.delete(fn);
        if (l.size === 0) { streams.delete(name); raw("stream.unsubscribe", { stream: name }); }
      };
    },
    // the core calls this (through window.plugincircuitReceive)
    receive(m) {
      if (!m) return;
      if (m.kind === "patch") {
        const s = stores.get(m.topic);
        if (s) deliver(m.topic, s, m);
      } else if (m.kind === "stream") {
        for (const fn of streams.get(m.topic) || []) fn(m.data);
      }
    },
  };
})();
window.plugincircuitReceive = (m) => window.api.receive(m);
