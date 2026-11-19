#include "gui/EditorCore.h"

#include "core/Catalog.h"
#include "core/CatalogHealth.h"
#include "plugin/PluginCircuitPlugin.h"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace plugincircuit::resources {
struct Resource {
	const char *path;
	const unsigned char *data;
	size_t size;
	const char *mime;
};
const Resource *find(std::string_view path);
} // namespace plugincircuit::resources

namespace plugincircuit {

static bool debugUi() {
	static const bool on = std::getenv("PLUGINCIRCUIT_DEBUG_UI") != nullptr;
	return on;
}

EditorCore::EditorCore(PluginCircuitPlugin &p) : plugin(p) { CatalogHealth::want(true); }

// --- inner plugin GUI ------------------------------------------------------------------------------------------

const clap_plugin_gui *EditorCore::innerGui() {
	auto *inner = plugin.innerForGui();
	return inner ? inner->gui() : nullptr;
}

void EditorCore::createInnerGui() {
	if (innerGuiCreated) return;
	auto *inner = plugin.innerForGui();
	auto *gui = innerGui();
	if (!inner || !gui) return;
	auto *p = inner->plugin();
	if (!gui->is_api_supported(p, windowApi(), false)) return;
	if (!gui->create(p, windowApi(), false)) return;
	innerGuiCreated = true;
	uint32_t w = 0, h = 0;
	if (gui->get_size(p, &w, &h)) {
		innerW = w;
		innerH = h;
	}
	if (debugUi()) std::fprintf(stderr, "[plugincircuit] inner gui created, size %ux%u\n", w, h);
	clap_window win{};
	win.api = windowApi();
	win.ptr = innerParent();
	gui->set_parent(p, &win);
	gui->show(p);
	innerGuiShown();
	fitToInner();
}

void EditorCore::destroyInnerGui() {
	if (!innerGuiCreated) return;
	auto *inner = plugin.innerForGui();
	auto *gui = innerGui();
	if (inner && gui) {
		gui->hide(inner->plugin());
		gui->destroy(inner->plugin());
	}
	innerGuiCreated = false;
	innerW = innerH = 0;
	clearHolder();
}

// --- sizing ----------------------------------------------------------------------------------------------------

// Ask the host for a new size. The return value is not trusted: clap-wrapper's standalone and AUv2 hosts resize the
// view and still return false, and other hosts may answer true and resize later. The layout follows the container's
// real size (hostResized), so whatever the host does, nothing is laid out for a size we don't have.
void EditorCore::requestSize(uint32_t w, uint32_t h) {
	const auto want = layout::clampEditor({w, h}, minHeight());
	width = want.w;
	height = want.h;
	if (parented()) {
		plugin.requestHostResize(width, height);
		// a synchronous host has resized us by now; keep get_size truthful if it refused
		width = actualW();
		height = actualH();
	}
	layout();
}

// A fixed-size webview GUI: we scale it with page zoom, so the editor can take its aspect.
bool EditorCore::innerScalable() {
	if (!innerGuiCreated || innerW == 0 || innerH == 0) return false;
	auto *inner = plugin.innerForGui();
	auto *gui = innerGui();
	if (!inner || !gui || gui->can_resize(inner->plugin())) return false;
	return innerIsZoomableWebview();
}

// adjust_size: with an inner GUI the editor keeps to its shape, so the GUI always spans the full width.
layout::Size EditorCore::adjust(layout::Size proposed) {
	const uint32_t chrome = Editor::headerHeight + keyboardHeight;
	if (innerScalable()) return layout::snapToInner(proposed, {innerW, innerH}, Editor::headerHeight, keyboardHeight);
	auto *inner = plugin.innerForGui();
	auto *gui = innerGui();
	if (innerGuiCreated && inner && gui && gui->can_resize(inner->plugin()) && proposed.h > chrome) {
		uint32_t w = proposed.w, h = proposed.h - chrome; // the inner GUI picks its closest size for the area
		if (gui->adjust_size(inner->plugin(), &w, &h)) return layout::clampEditor({w, h + chrome}, minHeight());
	}
	return layout::clampEditor(proposed, minHeight());
}

void EditorCore::setSizeFromHost(uint32_t w, uint32_t h) {
	const auto s = layout::clampEditor({w, h}, minHeight());
	width = s.w;
	height = s.h;
	// the host sizes its parent; ours follows it. Hosts that only call set_size: size it here.
	if (parented()) sizeContainer(width, height);
	layout();
}

void EditorCore::hostResized() {
	if (!parented()) return;
	width = actualW(); // the host resized us (drag, request_resize, or on its own)
	height = actualH();
	layout();
}

void EditorCore::fitToInner() {
	if (!innerGuiCreated || innerW == 0 || innerH == 0) return;
	const auto want = layout::editorForInner({innerW, innerH}, Editor::headerHeight, keyboardHeight, innerScalable());
	if (want.w != actualW() || want.h != actualH()) requestSize(want.w, want.h);
	else layout();
}

void EditorCore::layout() {
	if (!web || inLayout) return;
	inLayout = true;
	struct Done {
		bool &f;
		~Done() { f = false; }
	} done{inLayout};
	if (!parented()) sizeContainer(width, height);
	const uint32_t w = actualW(), h = actualH(); // lay out what we have, not what we asked for
	if (debugUi()) std::fprintf(stderr, "[plugincircuit] layout %ux%u, kb %u\n", w, h, keyboardHeight);
	placeWeb(w, h);
	const auto area = layout::innerArea({w, h}, Editor::headerHeight, keyboardHeight);
	const bool showInner = view == "plugin" && innerGuiCreated;
	placeHolder(area, showInner);
	if (!showInner) return;
	auto *inner = plugin.innerForGui();
	auto *gui = innerGui();
	const bool resizable = inner && gui && gui->can_resize(inner->plugin());
	layout::Size viewSize{innerW, innerH};
	if (resizable) {
		uint32_t aw = uint32_t(area.w), ah = uint32_t(area.h);
		const bool adj = gui->adjust_size(inner->plugin(), &aw, &ah);
		if (adj && layout::fits({aw, ah}, area) && gui->set_size(inner->plugin(), aw, ah)) viewSize = {aw, ah};
		if (debugUi()) std::fprintf(stderr, "[plugincircuit] inner resizable: area %.0fx%.0f -> adjust %d %ux%u\n", area.w, area.h, int(adj), aw, ah);
	}
	placeInnerViews(area, resizable, viewSize);
}

void EditorCore::applyInnerSize(uint32_t w, uint32_t h) {
	innerW = w;
	innerH = h;
	if (inLayout) { // the inner GUI answered our set_size with a request: settle it on the next tick
		pendingInnerSize.store((uint64_t(w) << 32) | h);
		return;
	}
	fitToInner();
}

void EditorCore::innerDidChange() {
	if (plugin.innerForGui()) view = "plugin"; // a finished load always lands on its GUI
	if (view == "plugin") createInnerGui();
	layout(); // the ui store's innerGui changes with it: patched on the next tick
}

// --- views -----------------------------------------------------------------------------------------------------

void EditorCore::setKeyboard(bool open) {
	const uint32_t kb = open ? kKeyboardHeight : 0;
	if (kb == keyboardHeight) return;
	if (!open) plugin.uiAllNotesOff();
	plugin.showDawNotes(open);
	// grow the window rather than squeeze the plugin's GUI; if the host refuses, the strip takes the space
	const uint32_t h = actualH() - keyboardHeight + kb;
	keyboardHeight = kb;
	requestSize(actualW(), h);
}

void EditorCore::setView(const std::string &v) {
	if (debugUi()) std::fprintf(stderr, "[plugincircuit] setView %s -> %s\n", view.c_str(), v.c_str());
	view = v;
	if (view != "settings") plugin.changeInstance(choc::value::createObject("Ui", "view", view)); // reopened on it
	if (view == "plugin") createInnerGui();
	layout();
}

void EditorCore::noteKey(const char *code, bool down) {
	if (!keyboardHeight || !web || !pageReady) return;
	api.emit("keyboard.key", choc::value::createObject("Key", "code", code, "down", down));
}

// --- the page's API --------------------------------------------------------------------------------------------
// The editor's own part: the `ui` store (view, keyboard, the inner GUI), loading (which also switches the view), and
// `app.*`. The plugin adds the rest (PluginCircuitPlugin::registerApi).
void EditorCore::registerApi() {
	plugin.registerApi(api);
	// view and keyboardBase are instance-scope settings (SettingsSchema.h): the plugin keeps them in its state
	api.store("ui", {[this] {
		                 return choc::value::createObject("Ui", "view", view, "keyboard", keyboardHeight > 0, "innerGui", innerGuiCreated,
		                                                  "keyboardBase", plugin.instance()["keyboardBase"].getWithDefault<double>(48));
	                 },
	                 [this] {
		                 return Api::mix(Api::mix(std::hash<std::string>{}(view), (keyboardHeight > 0 ? 2 : 0) + (innerGuiCreated ? 1 : 0)),
		                                 plugin.instanceVersion());
	                 },
	                 [this](const choc::value::ValueView &v) -> std::string {
		                 if (!v.isObject()) return "expected an object";
		                 for (uint32_t i = 0; i < v.size(); ++i) {
			                 const std::string k = v.getObjectMemberAt(i).name;
			                 if (k != "view" && k != "keyboard" && k != "keyboardBase") return "unknown key '" + k + "'";
		                 }
		                 if (v.hasObjectMember("keyboardBase")) // checked first: a refused set changes nothing
			                 if (auto e = plugin.changeInstance(choc::value::createObject("Ui", "keyboardBase", v["keyboardBase"])); !e.empty())
				                 return e;
		                 if (v.hasObjectMember("view")) {
			                 const std::string next = v["view"].isString() ? std::string(v["view"].getString()) : "";
			                 if (next != "plugin" && next != "browse" && next != "macros" && next != "params" && next != "settings")
				                 return "unknown view '" + next + "'";
			                 setView(next);
		                 }
		                 if (v.hasObjectMember("keyboard")) {
			                 if (!v["keyboard"].isBool()) return "keyboard must be true or false";
			                 setKeyboard(v["keyboard"].getBool());
		                 }
		                 return {};
	                 }});
	api.command("plugin.load", [this](const choc::value::ValueView &a, std::string &error) -> choc::value::Value {
		const std::string id = a.isObject() && a.hasObjectMember("pluginId") && a["pluginId"].isString() ? std::string(a["pluginId"].getString()) : "";
		if (id.empty()) {
			error = "plugin.load needs a pluginId";
			return {};
		}
		const std::string sha = a.hasObjectMember("sha256") && a["sha256"].isString() ? std::string(a["sha256"].getString()) : "";
		plugin.uiLoad(id, sha);
		setView("plugin");
		return choc::value::Value(true);
	});
	api.command("plugin.unload", [this](const choc::value::ValueView &, std::string &) {
		plugin.uiUnload();
		setView("browse");
		return choc::value::Value(true);
	});
	// the catalog's reachability, checked by the core (CatalogHealth): online null until the first answer
	api.store("app", {[] {
		                  const auto h = CatalogHealth::status();
		                  auto v = choc::value::createObject("App", "apiBase", h.apiBase, "error", h.error);
		                  v.addMember("online", h.online < 0 ? choc::value::Value() : choc::value::Value(h.online == 1));
		                  return v;
	                  },
	                  [] { return CatalogHealth::version(); },
	                  {}});
	api.command("app.checkCatalog", [](const choc::value::ValueView &, std::string &) {
		CatalogHealth::checkNow();
		return choc::value::Value(true);
	});
	api.command("app.log", [](const choc::value::ValueView &a, std::string &) {
		if (a.isObject() && a.hasObjectMember("text") && a["text"].isString())
			std::fprintf(stderr, "[plugincircuit ui] %s\n", std::string(a["text"].getString()).c_str());
		return choc::value::Value(true);
	});
	api.command("app.openUrl", [this](const choc::value::ValueView &a, std::string &error) -> choc::value::Value {
		const std::string url = a.isObject() && a.hasObjectMember("url") && a["url"].isString() ? std::string(a["url"].getString()) : "";
		if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
			error = "only http(s) links";
			return {};
		}
		openUrl(url);
		return choc::value::Value(true);
	});
	api.send = [this](const choc::value::Value &m) {
		if (web) web->evaluateJavascript("window.plugincircuitReceive && window.plugincircuitReceive(" + choc::json::toString(m) + ")");
	};
}

// the page's one call: request(name, args) -> {ok, result | error}
choc::value::Value EditorCore::request(const choc::value::ValueView &args) {
	const std::string name = args.size() > 0 && args[0].isString() ? std::string(args[0].getString()) : "";
	const auto a = args.size() > 1 ? args[1] : choc::value::ValueView();
	auto reply = api.request(name, a);
	if (name == "hello") pageReady = reply["ok"].getBool();
	return reply;
}

void EditorCore::sendPatches() {
	if (!web || !pageReady) return;
	const auto t0 = std::chrono::steady_clock::now();
	api.tick(std::chrono::duration<double>(t0.time_since_epoch()).count());
	// the DAW's notes on the on-screen keyboard (queued by the audio thread while it is open)
	auto notes = choc::value::createEmptyArray();
	plugin.takeDawNotes([&](const PluginCircuitPlugin::DawNote &n) {
		notes.addArrayElement(choc::value::createObject("N", "key", (int32_t)n.key, "on", n.on));
	});
	if (notes.size()) api.emit("keyboard.notes", std::move(notes));
	if (static const bool dbg = std::getenv("PLUGINCIRCUIT_DEBUG_CALLS") != nullptr; dbg) {
		static double worst = 0, sum = 0;
		static int n = 0;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		worst = std::max(worst, ms), sum += ms, ++n;
		if (n % 10 == 0) std::fprintf(stderr, "[plugincircuit patches] last %.2f ms, avg %.2f ms, worst %.1f ms\n", ms, sum / n, worst);
	}
}

// --- debug and housekeeping ------------------------------------------------------------------------------------

// PLUGINCIRCUIT_AUTOLOAD=<pluginId> loads at start, PLUGINCIRCUIT_VIEW=<view> picks the tab, PLUGINCIRCUIT_SNAPSHOT=
// <file.png> writes the editor every 2 s (where the platform can).
void EditorCore::debugHooks() {
	++ticks;
	if (ticks == 10) {
		// PLUGINCIRCUIT_AUTOLOAD_ARCHIVE=<.wclap.tar.gz>: install a local bundle first, then load PLUGINCIRCUIT_AUTOLOAD
		// from it (no catalog needed: test WCLAPs)
		std::string sha;
		if (const char *archive = std::getenv("PLUGINCIRCUIT_AUTOLOAD_ARCHIVE")) {
			std::ifstream f(archive, std::ios::binary);
			std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			std::string error;
			if (Catalog::installArchive(bytes, sha, error).empty())
				std::fprintf(stderr, "[plugincircuit] PLUGINCIRCUIT_AUTOLOAD_ARCHIVE: %s\n", error.c_str());
		}
		if (const char *id = std::getenv("PLUGINCIRCUIT_AUTOLOAD")) plugin.uiLoad(id, sha);
		if (const char *v = std::getenv("PLUGINCIRCUIT_VIEW")) setView(v);
	}
	// PLUGINCIRCUIT_UI_JS=<js>: run once in the host UI after the autoload settled (debug only)
	if (ticks == 160 && web)
		if (const char *js = std::getenv("PLUGINCIRCUIT_UI_JS")) web->evaluateJavascript(js);
	if (const char *out = std::getenv("PLUGINCIRCUIT_SNAPSHOT"); out && ticks % 40 == 0 && web) debugSnapshot(out);
}

// PLUGINCIRCUIT_DEBUG_CALLS: once a second, the main-thread calls into the plugin's parameters (docs/PLAN.md phase 1)
void EditorCore::debugCalls() {
	static const bool on = std::getenv("PLUGINCIRCUIT_DEBUG_CALLS") != nullptr;
	auto *inner = plugin.innerForGui();
	if (!on || !inner) return;
	const auto now = std::chrono::steady_clock::now();
	if (now - callsAt < std::chrono::seconds(1)) return;
	auto &c = inner->paramCalls;
	const uint64_t cur[4] = {c.count.load(), c.info.load(), c.value.load(), c.text.load()};
	if (callsAt != std::chrono::steady_clock::time_point{})
		std::fprintf(stderr, "[plugincircuit calls/s] count %llu, get_info %llu, get_value %llu, value_to_text %llu, view %s\n",
		             (unsigned long long)(cur[0] - callsLast[0]), (unsigned long long)(cur[1] - callsLast[1]),
		             (unsigned long long)(cur[2] - callsLast[2]), (unsigned long long)(cur[3] - callsLast[3]), view.c_str());
	std::copy(cur, cur + 4, callsLast);
	callsAt = now;
}

// The table's safety net: a plugin that changes a parameter without reporting it. Only while the Params view shows
// values; 64 re-reads a second, round-robin.
void EditorCore::reconcile() {
	if (view != "params") return;
	const auto now = std::chrono::steady_clock::now();
	if (now - reconcileAt < std::chrono::seconds(1)) return;
	reconcileAt = now;
	plugin.reconcileParams(64);
}

void EditorCore::tick() {
	debugCalls();
	reconcile();
	if (uint64_t s = pendingInnerSize.exchange(0)) applyInnerSize(uint32_t(s >> 32), uint32_t(s));
	if (uint64_t s = plugin.pendingInnerResize.exchange(0, std::memory_order_acquire)) applyInnerSize(uint32_t(s >> 32), uint32_t(s));
	debugHooks();
	plugin.housekeeping(); // also covers hosts without timer-support
	sendPatches();
}

// --- lifetime --------------------------------------------------------------------------------------------------

bool EditorCore::create() {
	choc::ui::WebView::Options options;
	options.enableDebugMode = debugUi();
	options.acceptsFirstMouseClick = true;
	options.transparentBackground = true;
	options.fetchResource = [](const std::string &path) -> std::optional<choc::ui::WebView::Options::Resource> {
		auto p = path == "/" ? std::string("/index.html") : path;
		auto q = p.find('?');
		if (q != std::string::npos) p = p.substr(0, q);
		if (auto *r = resources::find(p)) {
			choc::ui::WebView::Options::Resource res;
			res.data.assign(r->data, r->data + r->size);
			res.mimeType = r->mime;
			return res;
		}
		return std::nullopt;
	};
	web = std::make_unique<choc::ui::WebView>(options);
	if (!web->loadedOK()) {
		web.reset();
		return false;
	}
	registerApi();
	web->bind("plugincircuitRequest", [this](const choc::value::ValueView &args) -> choc::value::Value { return request(args); });
	if (!createViews()) {
		web.reset();
		return false;
	}
	layout();

	// the view this instance was last on (its state), if it still makes sense: no plugin, no plugin views
	const std::string last = plugin.instance()["view"].getWithDefault<std::string>("plugin");
	view = plugin.innerForGui() ? last : "browse";
	if (debugUi()) std::fprintf(stderr, "[plugincircuit] editor created, view %s\n", view.c_str());
	if (view == "plugin") createInnerGui();
	startPlatform();
	timer = choc::messageloop::Timer(50, [this] {
		tick();
		return true;
	});
	return true;
}

void EditorCore::shutdown() {
	CatalogHealth::want(false);
	plugin.showDawNotes(false);
	stopPlatform();
	plugin.uiAllNotesOff(); // notes held on the on-screen keyboard die with the editor
	timer = {};
	destroyInnerGui();
	web.reset();
	destroyViews();
}

// --- Editor ----------------------------------------------------------------------------------------------------

Editor::Editor(PluginCircuitPlugin &plugin) : impl(EditorCore::make(plugin)) {}
Editor::~Editor() {
	if (impl) impl->shutdown();
}

bool Editor::isApiSupported(const char *api, bool isFloating) { return !isFloating && !std::strcmp(api, EditorCore::windowApi()); }
bool Editor::preferredApi(const char **api, bool *isFloating) {
	*api = EditorCore::windowApi();
	*isFloating = false;
	return true;
}
bool Editor::create() { return impl->create(); }
bool Editor::setScale(double scale) { return impl->setScale(scale); }
bool Editor::getSize(uint32_t &w, uint32_t &h) {
	w = impl->width;
	h = impl->height;
	return true;
}
bool Editor::adjustSize(uint32_t &w, uint32_t &h) {
	const auto s = impl->adjust({w, h});
	w = s.w;
	h = s.h;
	return true;
}
bool Editor::setSize(uint32_t w, uint32_t h) {
	impl->setSizeFromHost(w, h);
	return true;
}
bool Editor::setParent(const clap_window *window) { return impl->setParent(window); }
bool Editor::show() {
	impl->setVisible(true);
	return true;
}
bool Editor::hide() {
	impl->plugin.uiAllNotesOff();
	impl->setVisible(false);
	return true;
}
void Editor::innerWillChange() { impl->innerWillChange(); }
void Editor::innerDidChange() { impl->innerDidChange(); }
bool Editor::innerRequestResize(uint32_t w, uint32_t h) { // main thread (other threads: the plugin's pendingInnerResize)
	impl->applyInnerSize(w, h);
	return true;
}

} // namespace plugincircuit
