// Plugin Circuit host UI. Talks to the core through the one API (api.js, docs/MESSAGING.md): it subscribes to stores
// (plugin, params, macros, transport, ui, settings, cache) and the perf stream, sends commands, and edits values
// with gestures (`edit`).
"use strict";

const $ = (id) => document.getElementById(id);
// a command whose failure is logged, not thrown (the UI goes on)
const command = (name, args = {}) => api.command(name, args).catch((e) => { log(`${name} failed:`, e.message); return null; });

// a plugin UI, not a web page: no browser context menu (Reload, Inspect Element, ...)
window.addEventListener("contextmenu", (e) => e.preventDefault(), { capture: true });

// forward page errors to the native log (stderr / host log)
const log = (...a) => api.command("app.log", { text: a.map(String).join(" ") }).catch(() => {});
window.addEventListener("error", (e) => log("error:", e.message, e.filename + ":" + e.lineno));
window.addEventListener("unhandledrejection", (e) => log("rejection:", e.reason));

const PAGE = 16;
const state = {
  // what the stores say, in one object for the views: plugin (status, message, progress, instrument, apiBase, plugin,
  // missing, installed), macros (macros, autoMapped), params (params: infos), transport, ui (innerGui)
  snap: {
    status: "empty", message: "", progress: 0, instrument: false, apiBase: "", plugin: null, missing: null, installed: [],
    macros: [], autoMapped: true, params: [], hostTransport: true, transportPlaying: false, innerGui: false,
  },
  values: [],          // macro values 0..1
  paramValues: new Map(), // id -> {value, text}
  view: "browse",
  page: 0,
  dragging: new Map(), // "m3" / "p1234" -> drags not yet ended in the core: its values are ours, ignore the core's
  endWaiters: new Map(), // key -> callbacks, one per drag released by the user and not yet acknowledged
  pickerSrc: "",
  editSlot: -1,
  wasLoading: false,
};

// ---------------------------------------------------------------- core -> UI: stores
// texts come only for parameters the page shows (phase 1b): a value without one keeps the row's last text if the
// value is the same, else none (the core sends it when the row comes into view)
function mergeParamValues(values) {
  for (const pv of values) {
    const cur = state.paramValues.get(pv.id);
    if (pv.text === undefined) pv.text = cur && cur.value === pv.value ? cur.text : "";
    // while a param is dragged its value is ours, but its text is always the plugin's
    if (!state.dragging.has("p" + pv.id) || !cur) state.paramValues.set(pv.id, pv);
    else if (cur.value === pv.value) cur.text = pv.text;
  }
}

let macroShape = "";
function subscribeStores() {
  command("settings.schema").then((schema) => schema && renderAdvanced(schema)); // the Advanced tab
  api.subscribe("plugin", (v) => {
    const pluginChanged = JSON.stringify(v.plugin) !== JSON.stringify(state.snap.plugin);
    Object.assign(state.snap, v);
    render(pluginChanged);
    renderInstalled();
  });
  api.subscribe("macros", (v) => {
    state.snap.macros = v.macros;
    state.snap.autoMapped = v.autoMapped;
    v.values.forEach((x, i) => { if (!state.dragging.has("m" + i)) state.values[i] = x; });
    const shape = JSON.stringify([v.macros, v.autoMapped]);
    const rebuild = shape !== macroShape; // names or targets changed (the Params view's macro tags too)
    macroShape = shape;
    render(rebuild);
  });
  api.subscribe("params", (d) => {
    // ends the core has applied (PLAN 2.4): before the values, which are the plugin's again from here
    for (const e of d.ended || []) (state.endWaiters.get((e.target === "macro" ? "m" : "p") + e.id) || [])[0]?.();
    if (d.full) { state.snap.params = d.infos; state.paramValues = new Map(); }
    mergeParamValues(d.values);
    render(d.full);
  });
  api.subscribe("transport", (v) => {
    state.snap.hostTransport = v.host;
    state.snap.transportPlaying = v.playing;
    render(false);
  });
  api.subscribe("plugin.errors", (v) => renderErrors(v.errors || []));
  api.subscribe("app", (v) => {
    const was = state.app && state.app.online;
    state.app = v;
    if (v.online === false && was !== false) log("catalog unreachable:", v.error);
    if (state.view === "browse") ensurePicker(); // came online: the picker; went offline: the installed list
    else document.body.classList.toggle("offline", v.online === false);
  });
  api.subscribe("ui", (v) => {
    state.snap.innerGui = v.innerGui;
    // the first value always applies: the page starts on "browse", and entering a view does its work (the picker)
    if (v.view !== state.view || !state.initialised) { state.initialised = true; setView(v.view, false); }
    showKeyboard(v.keyboard, false);
    if (typeof v.keyboardBase === "number" && v.keyboardBase !== kbd.base) { kbd.allOff(); kbd.base = v.keyboardBase; buildKeys(); }
    render(false);
  });
}

// ---------------------------------------------------------------- views
function setView(view, tellHost = true) {
  state.view = view;
  document.body.dataset.view = view;
  for (const b of document.querySelectorAll(".tabs button")) b.setAttribute("aria-selected", String(b.dataset.view === view));
  $("pluginPill").setAttribute("aria-pressed", String(view === "browse"));
  if (view === "browse") { ensurePicker(); command("app.checkCatalog"); } // a fresh answer: it may have gone down
  $("settingsBtn").setAttribute("aria-pressed", String(view === "settings"));
  if (window.__settings) window.__settings.active(view === "settings"); // its stores and stream only while shown
  if (typeof scheduleVisibleParams === "function") scheduleVisibleParams(); // entering Params: rows + texts; leaving: none
  if (tellHost) {
    if (window.__debugViews) log("setView", view, new Error().stack.split("\n").slice(1, 4).join(" < "));
    api.set("ui", { view }).catch((e) => log("view:", e.message));
  }
}

function apiBase() { return (state.app && state.app.apiBase) || (state.snap && state.snap.apiBase) || "http://127.0.0.1:7790"; }

// The catalog's reachability is the core's (the app store, PLAN 5.1): the picker loads once it says online.
function ensurePicker(force = false) {
  const online = state.app && state.app.online;
  document.body.classList.toggle("offline", online === false);
  if (online === false) { renderInstalled(); return; }
  if (online !== true) return; // not checked yet: the app store's first answer calls this again
  const type = state.snap && state.snap.instrument ? "instrument" : "audio-effect,analyzer,note-effect";
  const src = `${apiBase()}/select?type=${encodeURIComponent(type)}&native=1&embed=1&state=host&theme=dark`;
  if (force || state.pickerSrc !== src) { state.pickerSrc = src; $("picker").src = src; }
}

// Messages from the catalog's picker: only from our picker iframe, served by the catalog. Anything else in the page
// (a plugin's own iframe, an ad, a compromised page) must not be able to load plugins.
function fromPicker(e) {
  const picker = $("picker");
  if (!picker || e.source !== picker.contentWindow) return false;
  try { return e.origin === new URL(apiBase()).origin; } catch (err) { return false; }
}

window.addEventListener("message", (e) => {
  if (!fromPicker(e)) return;
  const d = e.data || {};
  if (d.type === "plugincircuit:select" && d.pluginId) {
    command("plugin.load", { pluginId: d.pluginId, sha256: d.sha256 || "" });
    state.pickerSrc = ""; // the iframe now shows "Picked …": reload the picker next time
    setView("plugin");
  } else if (d.type === "plugincircuit:cancel") {
    if (document.body.classList.contains("has-plugin")) setView("plugin");
  }
});

// ---------------------------------------------------------------- rendering

function render(full) {
  const s = state.snap;
  if (!s) return;
  const plugin = s.plugin;
  const loading = ["fetching", "downloading", "compiling"].includes(s.status);
  document.body.classList.toggle("has-plugin", !!plugin);
  document.body.classList.toggle("instrument", !!s.instrument);
  const acceptsNotes = !!(plugin && plugin.noteInput);
  document.body.classList.toggle("accepts-notes", acceptsNotes);
  if (!acceptsNotes && kbd.open) showKeyboard(false);
  document.body.classList.toggle("own-transport", !!plugin && s.hostTransport === false);
  $("transportToggle").setAttribute("aria-pressed", String(!!s.transportPlaying));
  document.body.classList.toggle("loading", loading);
  const crashed = !!(plugin && s.moduleError); // the module trapped or timed out: it doesn't run any more
  document.body.classList.toggle("missing", crashed || (!loading && (s.status === "missing" || s.status === "error") && !plugin));

  if (state.wasLoading && !loading && plugin) {
    // loaded: the wordmark's italic c flashes signal yellow, then fades back
    const mark = document.querySelector(".topbar .wordmark");
    mark.classList.add("flash");
    setTimeout(() => mark.classList.remove("flash"), 450);
  }
  state.wasLoading = loading;

  // header
  if (plugin) {
    $("slotName").textContent = plugin.name || plugin.id;
    $("pluginPill").title = `${plugin.name || plugin.id}${plugin.vendor ? " by " + plugin.vendor : ""}: choose another plugin (⌘4)`;
    $("mpeBadge").hidden = !plugin.mpe;
  }
  $("slotHint").textContent = s.instrument ? "Choose an instrument" : "Choose an effect";
  const st = $("status");
  const label = { ready: "", fetching: "Fetching", downloading: `${Math.round((s.progress || 0) * 100)}%`, compiling: "Compiling", missing: "Missing", error: "Error", empty: "" }[s.status] || "";
  st.textContent = label;
  st.className = "status " + (s.status === "ready" ? "ok" : loading ? "warn" : (s.status === "missing" || s.status === "error") ? "bad" : "");
  if (plugin && plugin.latency) st.textContent = `${plugin.latency} smp`;

  for (const b of document.querySelectorAll(".tabs button"))
    b.disabled = !plugin && b.dataset.view !== "browse";

  // loader + missing
  $("loaderTitle").textContent = { fetching: "Finding plugin", downloading: "Downloading", compiling: "Compiling WebAssembly" }[s.status] || "Loading";
  $("loaderStep").textContent = s.message || "";
  $("progressBar").style.width = `${Math.round((s.progress || 0) * 100)}%`;
  document.querySelector(".progress").classList.toggle("indeterminate", s.status !== "downloading");
  renderPixels(loading, s.status === "downloading" ? s.progress || 0 : null);
  if (crashed) {
    $("missingTitle").textContent = `${plugin.name || plugin.id} stopped`;
    $("missingText").textContent = `${s.moduleError}. Retry loads it again, with its default settings.`;
  } else if (s.missing) {
    $("missingTitle").textContent = `${s.missing.name || s.missing.id} is unavailable`;
    $("missingText").textContent = s.message || "";
  } else if (s.status === "error") {
    $("missingTitle").textContent = "Could not load the plugin";
    $("missingText").textContent = s.message || "";
  }

  $("noGui").style.display = plugin && !s.innerGui ? "" : "none";
  // A rebuild would remove the element being dragged (its pointerup would never arrive): wait for the drag to end.
  if (full && state.dragging.size === 0) { state.pendingRebuild = false; renderPages(); renderKnobs(); renderParams(); }
  else { if (full) state.pendingRebuild = true; updateKnobs(); updateParams(); }
}

// loader: 24 pixel blocks; downloading fills them in order, the other phases run a wave
const PIXELS = 24;
function renderPixels(loading, progress) {
  const root = $("pixels");
  if (root.childElementCount !== PIXELS) {
    root.innerHTML = "";
    for (let i = 0; i < PIXELS; i++) {
      const p = document.createElement("span");
      p.style.animationDelay = `${((i % 8) + Math.floor(i / 8)) * 0.08}s`;
      root.appendChild(p);
    }
  }
  root.classList.toggle("indeterminate", loading && progress === null);
  const lit = progress === null ? 0 : Math.round(progress * PIXELS);
  [...root.children].forEach((p, i) => p.classList.toggle("on", i < lit));
}

// ---- macros
function renderPages() {
  const pages = $("macroPages");
  const n = Math.ceil(state.snap.macros.length / PAGE);
  pages.innerHTML = "";
  for (let i = 0; i < n; i++) {
    const b = document.createElement("button");
    b.textContent = String(i + 1).padStart(2, "0");
    b.setAttribute("aria-selected", String(i === state.page));
    b.onclick = () => { state.page = i; renderPages(); renderKnobs(); };
    pages.appendChild(b);
  }
  $("macroHint").textContent = state.snap.autoMapped ? "Auto-mapped to the plugin's first parameters. Your DAW automates these." : "Custom mapping. Your DAW automates these.";
}

const ARC = 270, R = 24, C = 32;
function arcPath(t) {
  const a0 = (-ARC / 2 - 90) * Math.PI / 180, a1 = a0 + ARC * t * Math.PI / 180;
  const p = (a) => `${C + R * Math.cos(a)} ${C + R * Math.sin(a)}`;
  return `M ${p(a0)} A ${R} ${R} 0 ${ARC * t > 180 ? 1 : 0} 1 ${p(a1)}`;
}

function macroText(i) {
  const m = state.snap.macros[i];
  if (!m.targets.length) return "—";
  if (m.targets.length > 1) return `${Math.round(state.values[i] * 100)} %  ·  ${m.targets.length} targets`;
  const pv = state.paramValues.get(m.targets[0].id);
  return pv ? pv.text : ""; // the target's text arrives with the next snapshot
}

function renderKnobs() {
  const root = $("knobs");
  root.innerHTML = "";
  const start = state.page * PAGE;
  state.snap.macros.slice(start, start + PAGE).forEach((m, k) => {
    const i = start + k;
    const el = document.createElement("div");
    el.className = "knob" + (m.targets.length ? "" : " unmapped");
    el.dataset.slot = i;
    el.innerHTML = `
      <span class="num">${String(i + 1).padStart(2, "0")}</span>
      <button class="more" title="Map / rename">⋯</button>
      <svg viewBox="0 0 64 64"><path class="track" d="${arcPath(1)}"/><path class="fill"/><circle class="cap" cx="32" cy="32" r="15"/><line class="needle" x1="32" y1="32" x2="32" y2="20"/></svg>
      <span class="name"></span><span class="value"></span>`;
    el.querySelector(".name").textContent = m.name;
    el.querySelector(".more").onclick = (e) => openPopover(i, e.currentTarget);
    el.querySelector(".name").ondblclick = (e) => openPopover(i, e.currentTarget);
    bindDrag(el.querySelector("svg"), "m" + i, () => state.values[i], (v, g) => {
      state.values[i] = v;
      edit("macro", i, v, g);
      updateKnob(el, i);
    }, "vertical", () => edit("macro", i, state.snap.macros[i].targets.length === 1 ? defaultOf(i) : 0.5, "set"));
    root.appendChild(el);
    updateKnob(el, i);
  });
}

function defaultOf(i) {
  const t = state.snap.macros[i].targets[0];
  const p = state.snap.params.find((p) => p.id === t.id);
  if (!p) return 0;
  const span = t.max - t.min;
  return span ? Math.min(1, Math.max(0, (p.default - t.min) / span)) : 0;
}

function updateKnob(el, i) {
  const v = state.values[i] || 0;
  el.querySelector(".fill").setAttribute("d", v > 0.001 ? arcPath(v) : "");
  const a = (-ARC / 2 + ARC * v) * Math.PI / 180;
  const n = el.querySelector(".needle");
  n.setAttribute("x2", C + 12 * Math.sin(a));
  n.setAttribute("y2", C - 12 * Math.cos(a));
  el.querySelector(".value").textContent = macroText(i);
}
function updateKnobs() { for (const el of document.querySelectorAll(".knob")) updateKnob(el, +el.dataset.slot); }

// shared drag behaviour (knobs vertical, sliders horizontal); shift = fine; double-click = reset
// One way to change a value (docs/MESSAGING.md): phase "begin" / "update" / "end" for a drag, "set" for a single change.
function edit(target, id, value, phase) { command("edit", { target, id, value, phase }); }

// set(value, phase) is called with "begin" on press, "update" while moving, "end" on release
function bindDrag(target, key, get, set, axis, reset) {
  target.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    target.setPointerCapture(e.pointerId);
    state.dragging.set(key, (state.dragging.get(key) || 0) + 1);
    const rect = target.getBoundingClientRect();
    let last = axis === "vertical" ? e.clientY : e.clientX;
    if (axis === "horizontal") set(Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width)), "begin");
    else set(get(), "begin");
    const move = (ev) => {
      const pos = axis === "vertical" ? ev.clientY : ev.clientX;
      if (axis === "vertical") {
        const d = (last - pos) / (ev.shiftKey ? 800 : 160);
        last = pos;
        set(Math.min(1, Math.max(0, get() + d)), "update");
      } else {
        set(Math.min(1, Math.max(0, (pos - rect.left) / rect.width)), "update");
      }
    };
    let ended = false;
    const up = () => {
      if (ended) return;
      ended = true;
      target.removeEventListener("pointermove", move);
      set(get(), "end");
      // the value stays ours until the core reports the end applied (the params store's `ended`); 2 s at most,
      // should no ack come (the DAW neither processes nor flushes)
      let done = false;
      const release = () => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        const waiters = state.endWaiters.get(key);
        waiters.splice(waiters.indexOf(release), 1);
        if (!waiters.length) state.endWaiters.delete(key);
        const n = state.dragging.get(key) - 1;
        if (n > 0) state.dragging.set(key, n);
        else state.dragging.delete(key);
        if (state.dragging.size === 0 && state.pendingRebuild) render(true);
      };
      const timer = setTimeout(release, 2000);
      if (!state.endWaiters.has(key)) state.endWaiters.set(key, []);
      state.endWaiters.get(key).push(release);
    };
    target.addEventListener("pointermove", move);
    target.addEventListener("pointerup", up, { once: true });
    target.addEventListener("pointercancel", up, { once: true });
    target.addEventListener("lostpointercapture", up, { once: true });
  });
  if (reset) target.addEventListener("dblclick", reset);
}

// ---- macro popover
function openPopover(i, anchor) {
  state.editSlot = i;
  const m = state.snap.macros[i];
  const pop = $("mapPopover");
  $("macroName").value = m.custom ? m.name : "";
  $("macroName").placeholder = m.name;
  const sel = $("mapParam");
  sel.innerHTML = "";
  for (const p of state.snap.params.filter((p) => p.automatable && !p.hidden)) {
    const o = document.createElement("option");
    o.value = p.id;
    o.textContent = p.module ? `${p.module} / ${p.name}` : p.name;
    sel.appendChild(o);
  }
  if (m.targets[0]) sel.value = m.targets[0].id;
  $("mapMin").value = 0;
  $("mapMax").value = 1;
  renderTargets();
  pop.hidden = false;
  const r = anchor.getBoundingClientRect();
  pop.style.left = Math.min(window.innerWidth - 310, Math.max(8, r.left - 140)) + "px";
  pop.style.top = Math.min(window.innerHeight - pop.offsetHeight - 8, r.bottom + 6) + "px";
}

function renderTargets() {
  const m = state.snap.macros[state.editSlot];
  const root = $("targets");
  root.innerHTML = "";
  for (const t of m.targets) {
    const p = state.snap.params.find((p) => p.id === t.id);
    const row = document.createElement("div");
    row.className = "t";
    row.innerHTML = `<span></span><button title="Remove">×</button>`;
    row.firstChild.textContent = p ? p.name : `#${t.id}`;
    row.lastChild.onclick = () => command("macros.unmap", { slot: state.editSlot, paramId: t.id });
    root.appendChild(row);
  }
}

function mapSelected(add) {
  const id = +$("mapParam").value;
  const p = state.snap.params.find((p) => p.id === id);
  if (!p) return;
  const lo = +$("mapMin").value, hi = +$("mapMax").value;
  const span = p.max - p.min;
  command("macros.map", { slot: state.editSlot, paramId: id, min: p.min + span * lo, max: p.min + span * hi, add: add ? 1 : 0 });
}
$("mapReplace").onclick = () => mapSelected(false);
$("mapAdd").onclick = () => mapSelected(true);
$("mapClear").onclick = () => command("macros.unmap", { slot: state.editSlot, paramId: -1 });
$("closePop").onclick = () => closePopover();
$("macroName").addEventListener("keydown", (e) => {
  if (e.key === "Enter") { command("macros.rename", { slot: state.editSlot, name: e.target.value.trim() }); closePopover(); }
  if (e.key === "Escape") closePopover();
});
$("macroName").addEventListener("change", (e) => command("macros.rename", { slot: state.editSlot, name: e.target.value.trim() }));
function closePopover() { $("mapPopover").hidden = true; state.editSlot = -1; }
document.addEventListener("pointerdown", (e) => {
  if (!$("mapPopover").hidden && !$("mapPopover").contains(e.target) && !e.target.closest(".more")) closePopover();
});
$("autoMap").onclick = () => command("macros.autoMap");

// ---- params
function macroForParam(id) {
  const ms = state.snap.macros;
  for (let i = 0; i < ms.length; i++) if (ms[i].targets.some((t) => t.id === id)) return i;
  return -1;
}

// The Params view is virtualized (docs/PLAN.md, phase 1b): a plugin may have thousands of parameters, so only the
// rows in view (plus OVERSCAN above and below) exist in the page, and the core formats texts only for those.
const ROW_H = 40, OVERSCAN = 8;
const prows = { list: [], els: new Map(), sentIds: "" }; // list: {group} | {p}; els: index -> element

function renderParams() {
  const root = $("params");
  const filter = $("paramFilter").value.trim().toLowerCase();
  const params = state.snap.params.filter((p) => !p.hidden && (!filter || p.name.toLowerCase().includes(filter) || p.module.toLowerCase().includes(filter)));
  $("paramCount").textContent = `${params.length} of ${state.snap.params.length} parameters`;
  prows.list = [];
  let group = null;
  for (const p of params) {
    if (p.module !== group) {
      group = p.module;
      if (group) prows.list.push({ group });
    }
    prows.list.push({ p });
  }
  root.innerHTML = "";
  prows.els.clear();
  root.style.height = `${prows.list.length * ROW_H}px`;
  renderVisibleParams();
}

function paramRow(p) {
  const row = document.createElement("div");
  row.className = "param";
  row.dataset.id = p.id;
  const mi = macroForParam(p.id);
  row.innerHTML = `<span class="pname"></span><div class="slider"><span class="bar"></span><span class="thumb"></span></div><span class="ptext"></span><span class="macro-tag"></span>`;
  row.querySelector(".pname").textContent = p.name;
  row.querySelector(".pname").title = p.name;
  row.querySelector(".macro-tag").textContent = mi >= 0 ? "M" + (mi + 1) : "";
  row.querySelector(".macro-tag").title = mi >= 0 ? `Macro ${mi + 1}` : "";
  const norm = () => { const pv = state.paramValues.get(p.id); const v = pv ? pv.value : p.default; return p.max > p.min ? (v - p.min) / (p.max - p.min) : 0; };
  bindDrag(row.querySelector(".slider"), "p" + p.id, norm, (n, phase) => {
    let v = p.min + (p.max - p.min) * n;
    if (p.stepped) v = Math.round(v);
    const pv = state.paramValues.get(p.id) || { id: p.id, text: "" };
    const next = { ...pv, value: v }; // keep the last text until the plugin formats this value
    state.paramValues.set(p.id, next);
    edit("param", p.id, v, phase);
    api.command("params.text", { id: p.id, value: v }).then((t) => {
      if (typeof t === "string" && t && state.paramValues.get(p.id) === next) { next.text = t; updateParam(row, p); }
    }).catch(() => {});
    updateParam(row, p);
  }, "horizontal", () => edit("param", p.id, p.default, "set"));
  updateParam(row, p);
  return row;
}

// Adds the rows that came into view, removes the ones that left (rows kept in view stay: a drag isn't cut off), and
// tells the core which parameters are visible.
function renderVisibleParams() {
  if (state.view !== "params" || !prows.list.length) { sendVisibleParams([]); return; }
  const view = $("viewParams"), root = $("params");
  const top = view.scrollTop - root.offsetTop;
  const first = Math.max(0, Math.floor(top / ROW_H) - OVERSCAN);
  const last = Math.min(prows.list.length, Math.ceil((top + view.clientHeight) / ROW_H) + OVERSCAN);
  for (const [i, el] of prows.els) {
    if (i < first || i >= last) { el.remove(); prows.els.delete(i); }
  }
  for (let i = first; i < last; ++i) {
    if (prows.els.has(i)) continue;
    const item = prows.list[i];
    let el;
    if (item.group) { el = document.createElement("div"); el.className = "group-title"; el.textContent = item.group; }
    else el = paramRow(item.p);
    el.classList.add("vrow");
    el.style.top = `${i * ROW_H}px`;
    root.appendChild(el);
    prows.els.set(i, el);
  }
  sendVisibleParams([...prows.els.values()].filter((el) => el.dataset.id).map((el) => Number(el.dataset.id)));
}
function sendVisibleParams(ids) {
  const key = ids.join(",");
  if (key === prows.sentIds) return;
  prows.sentIds = key;
  command("params.visible", { ids });
}
// on the next frame, or after 50 ms if WebKit throttles frames (a window it considers hidden)
let visiblePending = false;
function scheduleVisibleParams() {
  if (visiblePending) return;
  visiblePending = true;
  const run = () => { if (!visiblePending) return; visiblePending = false; renderVisibleParams(); };
  requestAnimationFrame(run);
  setTimeout(run, 50);
}
$("viewParams").addEventListener("scroll", scheduleVisibleParams, { passive: true });
window.addEventListener("resize", () => renderVisibleParams());

function updateParam(row, p) {
  const pv = state.paramValues.get(p.id);
  const v = pv ? pv.value : p.default;
  const n = p.max > p.min ? Math.min(1, Math.max(0, (v - p.min) / (p.max - p.min))) : 0;
  row.querySelector(".bar").style.width = n * 100 + "%";
  row.querySelector(".thumb").style.left = n * 100 + "%";
  row.querySelector(".ptext").textContent = pv ? pv.text : "";
}
function updateParams() {
  if (state.view !== "params" || !state.snap) return;
  for (const [i, row] of prows.els) { // only the rendered rows
    const item = prows.list[i];
    if (item && item.p) updateParam(row, item.p);
  }
}
$("paramFilter").addEventListener("input", () => renderParams());

// ---- offline list
function renderInstalled() {
  const list = (state.snap && state.snap.installed) || [];
  const root = $("installed");
  root.innerHTML = "";
  $("offlineText").textContent = list.length ? "Plugins you already downloaded still work:" : `Start the catalog server at ${apiBase()}.`;
  for (const p of list) {
    const b = document.createElement("button");
    b.innerHTML = `<span></span><small></small>`;
    b.firstChild.textContent = p.name || p.id;
    b.lastChild.textContent = p.vendor || "";
    b.onclick = () => { command("plugin.load", { pluginId: p.id, sha256: p.sha256 }); setView("plugin"); };
    root.appendChild(b);
  }
}

// ---------------------------------------------------------------- wiring
for (const b of document.querySelectorAll(".tabs button")) b.onclick = () => setView(b.dataset.view);
// the plugin pill opens the picker; with a plugin loaded, a second click goes back to it
$("pluginPill").onclick = () => setView(state.view === "browse" && document.body.classList.contains("has-plugin") ? "plugin" : "browse");
for (const b of document.querySelectorAll("[data-goto]")) b.onclick = () => setView(b.dataset.goto);
$("unload").onclick = () => command("plugin.unload");
$("openCatalog").onclick = () => {
  const p = state.snap && state.snap.plugin;
  if (p) command("app.openUrl", { url: `${apiBase()}/plugins/${encodeURIComponent(p.id)}` });
};
$("retryCatalog").onclick = () => command("app.checkCatalog");
$("refreshCatalog").onclick = () => { command("app.checkCatalog"); ensurePicker(true); };
$("retryLoad").onclick = () => {
  const m = state.snap && (state.snap.missing || (state.snap.moduleError && state.snap.plugin)); // unavailable, or stopped
  if (m) command("plugin.load", { pluginId: m.id, sha256: m.sha256 || "" });
};
// recent errors (plugin.errors, PLAN 5.2): newest first, under the unavailable / stopped card
function renderErrors(errors) {
  const list = $("errorList");
  list.textContent = "";
  for (const e of errors) {
    const li = document.createElement("li");
    const head = document.createElement("b");
    head.textContent = `${new Date(e.time).toLocaleTimeString()} ${e.source}`;
    li.append(head, document.createTextNode(e.message));
    list.appendChild(li);
  }
  $("errorDetails").hidden = errors.length === 0;
}
// Text entry only: the velocity slider is an <input type=range> and must not swallow note keys.
const isTextEntry = (el) => el instanceof Element && el.matches("input:not([type=range]):not([type=checkbox]), select, textarea, [contenteditable]");
document.addEventListener("keydown", (e) => {
  if (isTextEntry(e.target)) return;
  const keys = { 1: "plugin", 2: "macros", 3: "params", 4: "browse" };
  if ((e.metaKey || e.ctrlKey) && keys[e.key]) { e.preventDefault(); setView(keys[e.key]); return; }
  if ((e.metaKey || e.ctrlKey) && e.key === "r" && state.view === "browse") { e.preventDefault(); command("app.checkCatalog"); ensurePicker(true); return; }
  if ((e.metaKey || e.ctrlKey) && e.key === "k" && document.body.classList.contains("accepts-notes")) { e.preventDefault(); userToggleKeyboard(); return; }
  if (!e.metaKey && !e.ctrlKey && !e.altKey) kbdKey(e, true);
});

document.addEventListener("keyup", (e) => kbdKey(e, false));
window.addEventListener("blur", () => kbd.allOff());
window.addEventListener("pagehide", () => kbd.allOff());
document.addEventListener("visibilitychange", () => { if (document.hidden) kbd.allOff(); });

// ---------------------------------------------------------------- on-screen keyboard (instruments)
// Notes go to the plugin through the keyboard.note command -> the lock-free UI queue -> the next process block.
const kbd = {
  open: false,
  base: 48,          // lowest key shown: C3
  octaves: 3,
  down: new Map(),   // key -> source ("ptr" | letter)
  daw: new Set(),    // keys the DAW holds (keyboard.notes events, while the keyboard is open)
  sent: new Set(),
  noteOn(key, source) {
    if (key < 0 || key > 127 || this.down.has(key)) return;
    this.down.set(key, source);
    this.sent.add(key);
    command("keyboard.note", { key, on: 1, velocity: Number($("velocity").value) / 127 });
    paintKey(key, true);
  },
  noteOff(key) {
    if (!this.down.has(key)) return;
    this.down.delete(key);
    this.sent.delete(key);
    command("keyboard.note", { key, on: 0, velocity: 0 });
    paintKey(key, false);
  },
  allOff() { heldCodes.clear(); for (const key of [...this.down.keys()]) this.noteOff(key); },
};

const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const isBlack = (k) => [1, 3, 6, 8, 10].includes(k % 12);
// computer keys: one octave plus a fifth from the left edge of the keyboard, like most DAWs. Mapped by physical
// position (KeyboardEvent.code), so Shift/Option and non-US layouts don't change which note a key plays or releases.
const NOTE_CODES = ["KeyA", "KeyW", "KeyS", "KeyE", "KeyD", "KeyF", "KeyT", "KeyG", "KeyY", "KeyH", "KeyU", "KeyJ",
  "KeyK", "KeyO", "KeyL", "KeyP", "Semicolon", "Quote"];
const heldCodes = new Map(); // code -> the key it started, so a release matches its press across octave shifts

function buildKeys() {
  const box = $("keys");
  box.textContent = "";
  const first = kbd.base, last = kbd.base + kbd.octaves * 12;
  const whites = [];
  for (let k = first; k <= last; k++) if (!isBlack(k)) whites.push(k);
  const w = 100 / whites.length;
  for (let k = first; k <= last; k++) {
    const el = document.createElement("div");
    el.className = "key " + (isBlack(k) ? "black" : "white");
    el.dataset.key = k;
    if (isBlack(k)) {
      const left = whites.indexOf(k - 1);
      el.style.left = `calc(${(left + 1) * w}% - ${w * 0.32}%)`;
      el.style.width = `${w * 0.64}%`;
    } else {
      el.style.left = `${whites.indexOf(k) * w}%`;
      el.style.width = `${w}%`;
      if (k % 12 === 0) el.innerHTML = `<span class="lbl">C${Math.floor(k / 12) - 1}</span>`;
    }
    if (kbd.down.has(k)) el.classList.add("on");
    if (kbd.daw.has(k)) el.classList.add("daw");
    box.append(el);
  }
  $("octLabel").textContent = `C${Math.floor(kbd.base / 12) - 1}`;
}
const paintKey = (key, on) => { const el = $("keys").querySelector(`[data-key="${key}"]`); if (el) el.classList.toggle("on", on); };

// pointer: press, glissando while held, release anywhere
let dragging = null;
$("keys").addEventListener("pointerdown", (e) => {
  const el = e.target.closest(".key");
  if (!el) return;
  e.preventDefault();
  $("keys").setPointerCapture(e.pointerId);
  dragging = Number(el.dataset.key);
  kbd.noteOn(dragging, "ptr");
});
$("keys").addEventListener("pointermove", (e) => {
  if (dragging === null) return;
  const el = document.elementFromPoint(e.clientX, e.clientY)?.closest(".key");
  const key = el ? Number(el.dataset.key) : null;
  if (key === dragging) return;
  kbd.noteOff(dragging);
  dragging = key;
  if (key !== null) kbd.noteOn(key, "ptr");
});
const release = () => { if (dragging !== null) kbd.noteOff(dragging); dragging = null; };
$("keys").addEventListener("pointerup", release);
$("keys").addEventListener("pointercancel", release);

function kbdKey(e, down) {
  if (!down) { // a release always ends the note its key started, whatever has focus or modifiers now
    if (heldCodes.has(e.code)) { e.preventDefault(); kbdCode(e.code, false); }
    return;
  }
  if (!kbd.open || e.repeat || isTextEntry(e.target)) return;
  if (kbdCode(e.code, true)) e.preventDefault();
}
// also called natively when the plugin's own GUI has the keyboard focus (Editor::forwardNoteKey)
function kbdCode(code, down) {
  if (!down) {
    const key = heldCodes.get(code);
    if (key === undefined) return false;
    heldCodes.delete(code);
    kbd.noteOff(key);
    return true;
  }
  if (!kbd.open || heldCodes.has(code)) return false;
  if (code === "KeyZ" || code === "KeyX") { shiftOctave(code === "KeyZ" ? -1 : 1); return true; }
  const i = NOTE_CODES.indexOf(code);
  if (i < 0) return false;
  const key = kbd.base + i;
  if (key > 127) return false;
  heldCodes.set(code, key);
  kbd.noteOn(key, code);
  return true;
}
// the DAW's notes (PLAN 5.4): {key, on}; key -1 clears them all
api.onEvent("keyboard.notes", (notes) => {
  for (const n of notes) {
    if (n.key < 0) { for (const k of kbd.daw) paintDaw(k, false); kbd.daw.clear(); continue; }
    if (n.on) kbd.daw.add(n.key); else kbd.daw.delete(n.key);
    paintDaw(n.key, n.on);
  }
});
function paintDaw(key, on) { const el = $("keys").querySelector(`[data-key="${key}"]`); if (el) el.classList.toggle("daw", on); }
// keys typed into the hosted plugin's GUI, copied here by the editor (Editor.mm, forwardNoteKey)
api.onEvent("keyboard.key", (k) => kbdCode(k.code, k.down));

function shiftOctave(d) {
  const base = Math.min(127 - kbd.octaves * 12 - (127 - kbd.octaves * 12) % 12, Math.max(0, kbd.base + d * 12)); // top key <= 127
  if (base === kbd.base) return;
  kbd.allOff();
  kbd.base = base;
  buildKeys();
  api.set("ui", { keyboardBase: base }).catch((e) => log("octave:", e.message)); // instance scope: saved with the session
}
$("octDown").onclick = () => shiftOctave(-1);
$("octUp").onclick = () => shiftOctave(1);

function showKeyboard(open, tellHost = true) {
  if (open === kbd.open) return;
  kbd.open = open;
  if (!open) { kbd.allOff(); kbd.daw.clear(); } // the core stops sending DAW notes while closed
  document.body.classList.toggle("kbd-open", open);
  $("kbdToggle").setAttribute("aria-pressed", String(open));
  if (open) buildKeys();
  if (tellHost) api.set("ui", { keyboard: open }).catch((e) => log("keyboard:", e.message));
}
const userToggleKeyboard = () => showKeyboard(!kbd.open);
$("kbdToggle").onclick = userToggleKeyboard;
function toggleTransport() {
  const playing = !state.snap.transportPlaying;
  state.snap.transportPlaying = playing;
  $("transportToggle").setAttribute("aria-pressed", String(playing));
  api.set("transport", { playing }).catch((e) => log("transport:", e.message));
}
$("transportToggle").onclick = toggleTransport;

document.body.dataset.view = "browse";
// the handshake, then the stores; a page and a plugin that don't speak the same protocol say so instead of half-working
api.hello().then(subscribeStores).catch((e) => {
  document.body.innerHTML = `<div style="padding:24px;font:13px system-ui;color:#ccc;background:#111;height:100vh">
    Plugin Circuit's interface doesn't match this plugin build.<br><span style="opacity:.6"></span></div>`;
  document.body.querySelector("span").textContent = e.message;
});

// the wordmark opens the catalog website in the default browser
{
  const brand = document.querySelector(".brand");
  brand.title = "Open the Plugin Circuit website";
  brand.setAttribute("role", "link");
  brand.addEventListener("click", () => command("app.openUrl", { url: apiBase() + "/" }));
}

// ---------------------------------------------------------------- settings: caches
const DAY = 86400;
const cache = { list: [], selected: new Set(), back: "plugin" };
$("settingsBtn").onclick = () => {
  if (state.view === "settings") setView(cache.back);
  else { cache.back = state.view || "plugin"; setView("settings"); }
};

function formatBytes(n) {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(n >= 1e8 ? 0 : 1)} MB`;
  return `${Math.max(1, Math.round(n / 1e3))} KB`;
}
function formatAgo(t) {
  if (!t) return "never";
  const d = Math.floor((Date.now() / 1000 - t) / DAY);
  return d <= 0 ? "today" : d === 1 ? "yesterday" : d < 60 ? `${d} days ago` : `${Math.round(d / 30)} months ago`;
}
const unused = (p) => !p.inUse && Date.now() / 1000 - Math.max(p.lastUsed, p.lastSaved) > 30 * DAY;

function onCache(r) { // the cache store
  if (!r || !Array.isArray(r.packages)) { $("cacheSummary").textContent = "Not available"; return; }
  cache.list = r.packages.sort((a, b) => Math.max(b.lastUsed, b.lastSaved) - Math.max(a.lastUsed, a.lastSaved));
  for (const sha of [...cache.selected]) if (!cache.list.some((p) => p.sha256 === sha && !p.inUse)) cache.selected.delete(sha);
  $("cacheSummary").textContent = `${cache.list.length} plugin${cache.list.length === 1 ? "" : "s"}, ${formatBytes(r.packagesBytes)} downloaded · ${formatBytes(r.compiledBytes)} compiled`;
  $("compiledSize").textContent = formatBytes(r.compiledBytes);
  renderCaches();
}

function renderCaches() {
  const root = $("cacheList");
  root.innerHTML = "";
  if (!cache.list.length) { root.innerHTML = `<div class="cache-empty">No downloaded plugins.</div>`; }
  for (const p of cache.list) {
    const row = document.createElement("label");
    row.className = "cache-row";
    row.setAttribute("role", "listitem");
    row.innerHTML = `<input type="checkbox"><span class="cache-name"><span class="n"></span><span class="cache-sub"></span></span><span class="cache-badges"></span><span class="cache-size"></span><span class="cache-used"></span>`;
    const box = row.querySelector("input");
    box.checked = cache.selected.has(p.sha256);
    box.disabled = p.inUse;
    box.onchange = () => { box.checked ? cache.selected.add(p.sha256) : cache.selected.delete(p.sha256); updateCacheButtons(); };
    row.querySelector(".n").textContent = p.name || p.sha256.slice(0, 12);
    row.querySelector(".cache-sub").textContent = p.vendor || "";
    const badges = row.querySelector(".cache-badges");
    const badge = (text, title) => { const c = document.createElement("span"); c.className = "chip"; c.textContent = text; c.title = title; badges.appendChild(c); };
    if (p.current) badge("Loaded", "Loaded here: can't be deleted");
    else if (p.inUse) badge("In use", "Loaded in another plugin instance: can't be deleted");
    if (p.lastSaved) badge("In a session", `Saved in a DAW session ${formatAgo(p.lastSaved)}`);
    row.querySelector(".cache-size").textContent = formatBytes(p.bytes);
    row.querySelector(".cache-used").textContent = formatAgo(Math.max(p.lastUsed, p.lastSaved));
    root.appendChild(row);
  }
  updateCacheButtons();
}

function updateCacheButtons() {
  $("cacheDeleteSelected").disabled = cache.selected.size === 0;
  $("cacheDeleteSelected").textContent = cache.selected.size ? `Delete selected (${cache.selected.size})` : "Delete selected";
  const n = cache.list.filter(unused).length;
  $("cacheDeleteUnused").disabled = n === 0;
  $("cacheDeleteAll").disabled = !cache.list.some((p) => !p.inUse);
}

// a destructive button asks once more inline (no browser dialogs inside a plugin)
function confirmFirst(button, label, action) {
  const original = button.textContent;
  return () => {
    if (!button.classList.contains("confirm")) {
      button.classList.add("confirm");
      button.textContent = label();
      button._timer = setTimeout(() => { button.classList.remove("confirm"); button.textContent = original; updateCacheButtons(); }, 3000);
      return;
    }
    clearTimeout(button._timer);
    button.classList.remove("confirm");
    action();
  };
}

async function deletePackages(shas) {
  if (!shas.length) return;
  const r = await command("cache.delete", { shas }); // the list follows as a patch
  for (const sha of shas) cache.selected.delete(sha);
  const skipped = r && Array.isArray(r.skipped) ? r.skipped.length : 0;
  $("cacheNote").textContent = `Deleted ${r ? r.deleted : 0}${skipped ? `, skipped ${skipped} in use` : ""}. A deleted plugin is downloaded again when a session needs it.`;
}

$("cacheDeleteSelected").onclick = confirmFirst($("cacheDeleteSelected"), () => `Delete ${cache.selected.size}? Click again`,
  () => deletePackages([...cache.selected]));
$("cacheDeleteUnused").onclick = confirmFirst($("cacheDeleteUnused"), () => `Delete ${cache.list.filter(unused).length}? Click again`,
  () => deletePackages(cache.list.filter(unused).map((p) => p.sha256)));
$("cacheDeleteAll").onclick = confirmFirst($("cacheDeleteAll"), () => `Delete all ${cache.list.filter((p) => !p.inUse).length}? Click again`,
  () => deletePackages(cache.list.filter((p) => !p.inUse).map((p) => p.sha256)));
$("cacheClearCompiled").onclick = confirmFirst($("cacheClearCompiled"), () => "Clear? Click again",
  () => command("cache.clearCompiled"));

// ---------------------------------------------------------------- settings: advanced
// Rendered from the schema (src/api/SettingsSchema.h, the machine settings with a label): a bool is a checkbox, a
// number with a step a menu of its steps. The core validates; a refused change comes back as the store's value.
const advanced = { controls: new Map(), values: null }; // key -> {def, el}
function fmtSetting(d, v) {
  if (v === 0 && d.zeroLabel) return d.zeroLabel;
  const t = d.format === "percent" ? `${Math.round(v * 100)}%` : String(v);
  return `${t}${d.suffix || ""}${v === d.default ? " (default)" : ""}`;
}
function renderAdvanced(schema) {
  const box = $("advancedRows");
  box.textContent = "";
  advanced.controls.clear();
  for (const d of schema) {
    const row = document.createElement(d.type === "bool" ? "label" : "div");
    row.className = "cache-row setting-row";
    const name = document.createElement("span");
    name.className = "cache-name";
    name.textContent = d.label;
    const sub = document.createElement("span");
    sub.className = "cache-sub";
    sub.textContent = d.help;
    name.appendChild(sub);
    let el;
    if (d.type === "bool") {
      el = document.createElement("input");
      el.type = "checkbox";
      el.onchange = () => api.set("settings", { [d.key]: el.checked }).catch((e) => log("settings:", e.message));
    } else if (d.type === "number" && d.step > 0) {
      el = document.createElement("select");
      for (let i = 0; d.min + i * d.step <= d.max + 1e-9; ++i) {
        const v = Math.round((d.min + i * d.step) * 1e6) / 1e6;
        const o = document.createElement("option");
        o.value = String(v);
        o.textContent = fmtSetting(d, v);
        el.appendChild(o);
      }
      el.onchange = () => api.set("settings", { [d.key]: Number(el.value) }).catch((e) => log("settings:", e.message));
    } else if (d.type === "choice") {
      el = document.createElement("select");
      for (const c of d.choices) { const o = document.createElement("option"); o.value = o.textContent = c; el.appendChild(o); }
      el.onchange = () => api.set("settings", { [d.key]: el.value }).catch((e) => log("settings:", e.message));
    } else continue; // a free number: no control yet
    el.id = "setting-" + d.key;
    el.setAttribute("aria-label", d.label);
    row.append(name, el);
    box.appendChild(row);
    advanced.controls.set(d.key, { d, el });
  }
  if (advanced.values) onSettings(advanced.values);
}
function onSettings(s) { // the settings store
  if (!s) return;
  advanced.values = s;
  for (const [key, { d, el }] of advanced.controls) {
    if (!(key in s)) continue;
    if (d.type === "bool") el.checked = !!s[key];
    else el.value = String(d.type === "number" ? Math.round(s[key] * 1e6) / 1e6 : s[key]);
  }
}
function showSkipped(skipped) { // from the perf stream
  $("audioWaitNote").textContent = skipped
    ? `${skipped} block${skipped === 1 ? "" : "s"} skipped since Plugin Circuit started. More than a few while you work: raise the wait.`
    : "No blocks skipped since Plugin Circuit started.";
}

// ---------------------------------------------------------------- settings: performance
// DSP load per sample (the perf stream, 10 Hz) = average of each block's time / its real-time duration; the last 60 s are drawn.
// Reached as window.__perf: setView() may run before this file has finished evaluating.
const perf = (window.__perf = (() => {
  const N = 600;
  const hist = []; // {load, peak, skipped, over}
  let last = null, totals = { skipped: 0, over: 0 }, base = null;

  function sample(r) { // the perf stream, 10 Hz while the settings page is shown
    if (!r || typeof r.blocks !== "number") return;
    showSkipped(r.skipped);
    if (!base) base = { skipped: r.skipped, over: r.overBudget };
    if (last) {
      const blocks = r.blocks - last.blocks;
      const load = blocks > 0 ? (r.loadPpmSum - last.loadPpmSum) / blocks / 1e6 : 0;
      hist.push({ load, peak: blocks > 0 ? r.peakPpm / 1e6 : 0, skipped: Math.max(0, r.skipped - last.skipped), over: Math.max(0, r.overBudget - last.overBudget), live: blocks > 0 });
      if (hist.length > N) hist.shift();
    }
    totals = { skipped: r.skipped - base.skipped, over: r.overBudget - base.over };
    last = r;
    render(r);
  }

  function pct(x) { return `${(x * 100).toFixed(x < 0.1 ? 1 : 0)} %`; }
  function render(r) {
    const live = hist.filter((h) => h.live);
    const recent = live.slice(-10); // the last second
    const load = recent.length ? recent.reduce((a, h) => a + h.load, 0) / recent.length : 0;
    const peak = live.length ? Math.max(...live.slice(-100).map((h) => h.peak)) : 0;
    $("perfLoad").textContent = live.length ? pct(load) : "–";
    $("perfPeak").textContent = live.length ? pct(peak) : "–";
    $("perfBlock").textContent = r.blockFrames ? `${r.blockFrames} @ ${Math.round(r.sampleRate / 100) / 10} kHz` : "–";
    $("perfSkipped").textContent = String(totals.skipped);
    $("perfOver").textContent = String(totals.over);
    $("perfSummary").textContent = live.length
      ? `${pct(load)} of each ${(1000 * r.blockFrames / r.sampleRate).toFixed(2)} ms block, peak ${pct(peak)} over the last 10 s`
      : "No audio is being processed.";
    draw();
  }

  function draw() {
    const c = $("perfGraph"), dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (!w) return;
    if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    const css = getComputedStyle(document.body);
    const col = (v, d) => css.getPropertyValue(v).trim() || d;
    const top = 14, bottom = h - 14, scale = Math.max(1, ...hist.map((x) => x.peak)) > 1 ? 1.5 : 1; // up to 100 % (150 % if exceeded)
    const y = (v) => bottom - Math.min(v / scale, 1) * (bottom - top);
    const x = (i) => w - (hist.length - 1 - i) * (w / (N - 1));
    // grid: 25 / 50 / 75 / 100 %
    g.strokeStyle = col("--c-line", "#333"); g.lineWidth = 1; g.fillStyle = col("--c-text-faint", "#777"); g.font = "10px ui-monospace, monospace";
    for (const v of [0.25, 0.5, 0.75, 1]) {
      g.beginPath(); g.moveTo(0, Math.round(y(v)) + 0.5); g.lineTo(w, Math.round(y(v)) + 0.5); g.stroke();
      g.fillText(`${v * 100}%`, 2, y(v) - 2);
    }
    if (hist.length < 2) return;
    // load: filled area
    g.beginPath(); g.moveTo(x(0), bottom);
    hist.forEach((p, i) => g.lineTo(x(i), y(p.load)));
    g.lineTo(x(hist.length - 1), bottom); g.closePath();
    g.globalAlpha = 0.35; g.fillStyle = col("--c-accent-ink", "#a78bfa"); g.fill(); g.globalAlpha = 1;
    g.beginPath(); hist.forEach((p, i) => (i ? g.lineTo(x(i), y(p.load)) : g.moveTo(x(i), y(p.load))));
    g.strokeStyle = col("--c-accent-ink", "#a78bfa"); g.lineWidth = 1.5; g.stroke();
    // peak: thin line
    g.beginPath(); hist.forEach((p, i) => (i ? g.lineTo(x(i), y(p.peak)) : g.moveTo(x(i), y(p.peak))));
    g.strokeStyle = col("--c-text-muted", "#999"); g.lineWidth = 1; g.stroke();
    // dropouts: ticks along the bottom (skipped: red, too slow: orange)
    hist.forEach((p, i) => {
      if (p.skipped) { g.fillStyle = col("--c-danger", "#ef4444"); g.fillRect(x(i) - 1, bottom + 2, 2, 10); }
      if (p.over) { g.fillStyle = "#e8a33d"; g.fillRect(x(i) - 1, top, 2, bottom - top); }
    });
  }

  $("perfReset").onclick = () => { hist.length = 0; base = null; last = null; draw(); };
  return { redraw: draw, sample };
})());

// The settings page's stores and stream, subscribed only while it is shown. Reached as window.__settings: setView()
// may run before this file has finished evaluating.
window.__settings = (() => {
  let subs = [];
  return {
    active(on) {
      if (on && !subs.length) subs = [api.subscribe("cache", onCache), api.subscribe("settings", onSettings), api.onStream("perf", perf.sample)];
      else if (!on && subs.length) { subs.forEach((off) => off()); subs = []; }
    },
  };
})();
if (state.view === "settings") window.__settings.active(true);

// ---------------------------------------------------------------- settings: tabs (on the right)
{
  const tabs = [...document.querySelectorAll(".settings-tabs button")];
  const show = (name) => {
    for (const t of tabs) t.setAttribute("aria-selected", String(t.dataset.tab === name));
    for (const g of document.querySelectorAll(".setting-group[data-tab]")) g.hidden = g.dataset.tab !== name;
    try { localStorage.setItem("plugincircuit.settingsTab", name); } catch (e) {}
    if (name === "performance") requestAnimationFrame(() => window.__perf && window.__perf.redraw());
  };
  for (const t of tabs) t.onclick = () => show(t.dataset.tab);
  let saved = "performance";
  try { saved = localStorage.getItem("plugincircuit.settingsTab") || saved; } catch (e) {}
  show(tabs.some((t) => t.dataset.tab === saved) ? saved : "performance");
}
