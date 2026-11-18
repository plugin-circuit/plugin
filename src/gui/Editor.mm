#include "gui/Editor.h"
#include "gui/EditorLayout.h"
#include "core/PackageCache.h"
#include "core/Settings.h"
#include "api/Api.h"
#include "core/CatalogHealth.h"

#include <wclap-bridge.h>

#include "plugin/PluginCircuitPlugin.h"

#include <choc/gui/choc_MessageLoop.h>
#include <choc/gui/choc_WebView.h>
#include <choc/text/choc_JSON.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstring>
#include <functional>
#include <string_view>

namespace plugincircuit::resources {
struct Resource {
	const char *path;
	const unsigned char *data;
	size_t size;
	const char *mime;
};
const Resource *find(std::string_view path);
} // namespace plugincircuit::resources

@interface PluginCircuitFlippedView : NSView
@end
@implementation PluginCircuitFlippedView
- (BOOL)isFlipped {
	return YES;
}
@end

// The editor's root view: fills the host's parent view (autoresizing) and reports every size change, so the layout
// always follows the size the host actually gave us, whatever request_resize returned.
@interface PluginCircuitContainerView : PluginCircuitFlippedView
@property(nonatomic, copy) void (^onResize)(void);
@end
@implementation PluginCircuitContainerView
- (void)setFrameSize:(NSSize)size {
	[super setFrameSize:size];
	if (self.onResize) self.onResize();
}
@end

namespace plugincircuit {

struct Editor::Impl {
	PluginCircuitPlugin &plugin;
	PluginCircuitContainerView *container = nil;
	PluginCircuitFlippedView *innerHolder = nil;
	std::unique_ptr<choc::ui::WebView> web;
	choc::messageloop::Timer timer;
	uint32_t width = 1000, height = 680; // the size we want (get_size); the layout uses the container's real bounds
	bool inLayout = false;
	std::atomic<uint64_t> pendingInnerSize{0}; // inner request_resize from another thread: (w << 32 | h), applied in tick()
	std::string view = "browse"; // plugin | browse | macros | params
	bool innerGuiCreated = false;
	uint32_t innerW = 0, innerH = 0;
	uint32_t keyboardHeight = 0; // on-screen keyboard strip at the bottom (instruments), drawn by the web UI
	static constexpr uint32_t kKeyboardHeight = 112;
	Api api;                // the page's one API (docs/MESSAGING.md): this editor is transport plus window logic
	bool pageReady = false; // the page said hello
	id keyMonitor = nil; // copies note keys typed into the inner plugin's GUI to the on-screen keyboard

	explicit Impl(PluginCircuitPlugin &p) : plugin(p) { CatalogHealth::want(true); }

	// --- inner plugin GUI ------------------------------------------------------------------------------
	const clap_plugin_gui *innerGui() {
		auto *inner = plugin.innerForGui();
		return inner ? inner->gui() : nullptr;
	}

	void createInnerGui() {
		if (innerGuiCreated) return;
		auto *inner = plugin.innerForGui();
		auto *gui = innerGui();
		if (!inner || !gui) return;
		auto *p = inner->plugin();
		if (!gui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false)) return;
		if (!gui->create(p, CLAP_WINDOW_API_COCOA, false)) return;
		innerGuiCreated = true;
		uint32_t w = 0, h = 0;
		if (gui->get_size(p, &w, &h)) {
			innerW = w;
			innerH = h;
		}
		if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner gui created, size %ux%u\n", w, h);
		clap_window win{};
		win.api = CLAP_WINDOW_API_COCOA;
		win.cocoa = (__bridge void *)innerHolder;
		gui->set_parent(p, &win);
		gui->show(p);
		prepareInnerBackground();
		fitToInner();
	}

	// No white flashes: a WKWebView paints white wherever the page hasn't drawn yet (while it re-renders after a
	// resize or zoom change). Turn that off and fill behind it with the plugin UI's own color instead, sampled
	// from its rendered edges once it has drawn (and again later, for UIs that load in stages).
	void prepareInnerBackground() {
		innerHolder.wantsLayer = YES;
		innerHolder.layer.backgroundColor = container.layer.backgroundColor;
		for (NSView *v in innerHolder.subviews) {
			if (![v isKindOfClass:[WKWebView class]]) continue;
			WKWebView *wv = (WKWebView *)v;
			[wv setValue:@NO forKey:@"drawsBackground"]; // the same switch CHOC's transparentBackground uses
			__weak WKWebView *weakWeb = wv;
			__weak NSView *weakHolder = innerHolder;
			for (double delay : {0.6, 2.0, 5.0})
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
					sampleBackground(weakWeb, weakHolder);
				});
		}
	}

	static void sampleBackground(WKWebView *wv, NSView *holder) {
		if (!wv || !holder || wv.superview != holder) return;
		[wv takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
			if (!image) return;
			NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:[image TIFFRepresentation]];
			if (!rep || rep.pixelsWide < 4 || rep.pixelsHigh < 4) return;
			// the most common color along the edges (quantized), averaged within its bucket
			const NSInteger w = rep.pixelsWide, h = rep.pixelsHigh, step = std::max<NSInteger>(1, (w + h) / 400);
			struct Acc { double r = 0, g = 0, b = 0; int n = 0; };
			std::array<Acc, 4096> buckets{};
			auto add = [&](NSInteger x, NSInteger y) {
				NSColor *c = [[rep colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
				if (!c || c.alphaComponent < 0.5) return;
				const int key = int(c.redComponent * 15.99) << 8 | int(c.greenComponent * 15.99) << 4 | int(c.blueComponent * 15.99);
				auto &a = buckets[size_t(key)];
				a.r += c.redComponent, a.g += c.greenComponent, a.b += c.blueComponent, ++a.n;
			};
			for (NSInteger x = 0; x < w; x += step) add(x, 1), add(x, h - 2);
			for (NSInteger y = 0; y < h; y += step) add(1, y), add(w - 2, y);
			const Acc *best = nullptr;
			for (auto &a : buckets) if (a.n && (!best || a.n > best->n)) best = &a;
			if (!best) return;
			NSColor *bg = [NSColor colorWithSRGBRed:best->r / best->n green:best->g / best->n blue:best->b / best->n alpha:1];
			if (wv.superview != holder) return;
			holder.layer.backgroundColor = bg.CGColor;
			if (@available(macOS 12.0, *)) wv.underPageBackgroundColor = bg;
		}];
	}

	void destroyInnerGui() {
		if (!innerGuiCreated) return;
		auto *inner = plugin.innerForGui();
		auto *gui = innerGui();
		if (inner && gui) {
			gui->hide(inner->plugin());
			gui->destroy(inner->plugin());
		}
		innerGuiCreated = false;
		innerW = innerH = 0;
		for (NSView *v in [innerHolder.subviews copy]) [v removeFromSuperview];
	}

	// The real size of the editor: the container's bounds once the host parented it, else the size we asked for.
	uint32_t actualW() const { return container && container.superview ? uint32_t(container.bounds.size.width) : width; }
	uint32_t actualH() const { return container && container.superview ? uint32_t(container.bounds.size.height) : height; }

	// Ask the host for a new size. The return value is not trusted: clap-wrapper's standalone and AUv2 hosts resize
	// the view and still return false, and other hosts may answer true and resize later. The layout follows the
	// container's real bounds (onResize), so whatever the host does, nothing is laid out for a size we don't have.
	void requestSize(uint32_t w, uint32_t h) {
		const auto want = layout::clampEditor({w, h}, minHeight());
		width = want.w;
		height = want.h;
		if (container && container.superview) {
			plugin.requestHostResize(width, height);
			// a synchronous host has resized us by now; keep get_size truthful if it refused
			width = actualW();
			height = actualH();
		}
		layout();
	}

	// A fixed-size webview GUI (a WCLAP's WKWebView): we scale it with page zoom, so the editor can take its aspect.
	bool innerScalable() {
		if (!innerGuiCreated || innerW == 0 || innerH == 0) return false;
		auto *inner = plugin.innerForGui();
		auto *gui = innerGui();
		if (!inner || !gui || gui->can_resize(inner->plugin())) return false;
		return [innerHolder.subviews.firstObject isKindOfClass:[WKWebView class]];
	}

	uint32_t minHeight() const { return innerGuiCreated ? layout::kMinHeightWithGui : layout::kMinHeight; }

	// adjust_size: with an inner GUI the editor keeps to its shape, so the GUI always spans the full width.
	layout::Size adjust(layout::Size proposed) {
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

	void fitToInner() {
		if (!innerGuiCreated || innerW == 0 || innerH == 0) return;
		const auto want = layout::editorForInner({innerW, innerH}, Editor::headerHeight, keyboardHeight, innerScalable());
		if (want.w != actualW() || want.h != actualH()) requestSize(want.w, want.h);
		else layout();
	}

	void layout() {
		if (!container || inLayout) return;
		inLayout = true;
		struct Done { bool &f; ~Done() { f = false; } } done{inLayout};
		if (!container.superview) [container setFrameSize:NSMakeSize(width, height)];
		const uint32_t width = actualW(), height = actualH(); // lay out what we have, not what we asked for
		if (std::getenv("PLUGINCIRCUIT_DEBUG_UI") && container.window) std::fprintf(stderr, "[plugincircuit] layout %ux%u, window content %.0fx%.0f, kb %u\n", width, height, container.window.contentView.bounds.size.width, container.window.contentView.bounds.size.height, keyboardHeight);
		if (web) {
			NSView *wv = (__bridge NSView *)web->getViewHandle();
			[wv setFrame:NSMakeRect(0, 0, width, height)];
		}
		const auto area = layout::innerArea({width, height}, Editor::headerHeight, keyboardHeight);
		[innerHolder setFrame:NSMakeRect(area.x, area.y, area.w, area.h)];
		bool showInner = view == "plugin" && innerGuiCreated;
		[innerHolder setHidden:!showInner];
		if (!innerGuiCreated || !showInner) return;
		auto *inner = plugin.innerForGui();
		auto *gui = innerGui();
		const bool resizable = inner && gui && gui->can_resize(inner->plugin());
		layout::Size viewSize{innerW, innerH};
		if (resizable) {
			uint32_t w = uint32_t(area.w), h = uint32_t(area.h);
			bool adj = gui->adjust_size(inner->plugin(), &w, &h);
			if (adj && layout::fits({w, h}, area) && gui->set_size(inner->plugin(), w, h)) viewSize = {w, h};
			if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner resizable: area %.0fx%.0f -> adjust %d %ux%u\n", area.w, area.h, int(adj), w, h);
		}
		// A fixed-size WCLAP webview GUI (a WKWebView) is scaled to fill the area with WebKit's page zoom, keeping its
		// aspect ratio (the page still lays out at its own size). Anything else is centred at its own size.
		for (NSView *v in innerHolder.subviews) {
			const bool scalable = !resizable && [v isKindOfClass:[WKWebView class]];
			layout::Size own = resizable ? viewSize : layout::Size{uint32_t(v.frame.size.width), uint32_t(v.frame.size.height)};
			const auto f = layout::placeInner(area, own, {innerW, innerH}, scalable);
			if (scalable) ((WKWebView *)v).pageZoom = f.zoom;
			if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner view %.0fx%.0f in %.0fx%.0f, scaled %d\n", f.w, f.h, area.w, area.h, int(scalable));
			[v setFrame:NSMakeRect(f.x, f.y, f.w, f.h)];
		}
	}

	void setKeyboard(bool open) {
		uint32_t kb = open ? kKeyboardHeight : 0;
		if (kb == keyboardHeight) return;
		if (!open) plugin.uiAllNotesOff();
		plugin.showDawNotes(open);
		// grow the window rather than squeeze the plugin's GUI; if the host refuses, the strip takes the space
		const uint32_t h = actualH() - keyboardHeight + kb;
		keyboardHeight = kb;
		requestSize(actualW(), h);
	}

	void setView(const std::string &v) {
		if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] setView %s -> %s\n", view.c_str(), v.c_str());
		view = v;
		if (view != "settings") plugin.changeInstance(choc::value::createObject("Ui", "view", view)); // reopened on it
		if (view == "plugin") createInnerGui();
		layout();
	}

	// --- web UI --------------------------------------------------------------------------------------------
	// The editor's own part of the API: the `ui` store (view, keyboard, the inner GUI), loading (which also switches
	// the view), and `app.*`. The plugin adds the rest (PluginCircuitPlugin::registerApi).
	void registerApi() {
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
		api.command("app.openUrl", [](const choc::value::ValueView &a, std::string &error) -> choc::value::Value {
			const std::string url = a.isObject() && a.hasObjectMember("url") && a["url"].isString() ? std::string(a["url"].getString()) : "";
			if (url.rfind("http", 0) != 0) {
				error = "only http(s) links";
				return {};
			}
			[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
			return choc::value::Value(true);
		});
		api.send = [this](const choc::value::Value &m) {
			if (web) web->evaluateJavascript("window.plugincircuitReceive && window.plugincircuitReceive(" + choc::json::toString(m) + ")");
		};
	}

	// the page's one call: request(name, args) -> {ok, result | error}
	choc::value::Value request(const choc::value::ValueView &args) {
		const std::string name = args.size() > 0 && args[0].isString() ? std::string(args[0].getString()) : "";
		const auto a = args.size() > 1 ? args[1] : choc::value::ValueView();
		auto reply = api.request(name, a);
		if (name == "hello") pageReady = reply["ok"].getBool();
		return reply;
	}

	void sendPatches() {
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

	// Debug: PLUGINCIRCUIT_SNAPSHOT=<file.png> writes the webview (WebKit's own renderer, no screen-recording
	// permission needed) plus the inner GUI view every 2 s. PLUGINCIRCUIT_AUTOLOAD=<pluginId> loads at start,
	// PLUGINCIRCUIT_VIEW=<view> picks the tab.
	int ticks = 0;
	void debugHooks() {
		++ticks;
		if (ticks == 10) {
			if (const char *id = std::getenv("PLUGINCIRCUIT_AUTOLOAD")) plugin.uiLoad(id, "");
			if (const char *v = std::getenv("PLUGINCIRCUIT_VIEW")) setView(v);
		}
		// PLUGINCIRCUIT_UI_JS=<js>: run once in the host UI after the autoload settled (debug only)
		if (ticks == 160 && web)
			if (const char *js = std::getenv("PLUGINCIRCUIT_UI_JS")) web->evaluateJavascript(js);
		const char *out = std::getenv("PLUGINCIRCUIT_SNAPSHOT");
		if (!out || ticks % 40 != 0 || !web) return;
		WKWebView *wv = (__bridge WKWebView *)web->getViewHandle();
		std::string path = out;
		// the inner plugin's GUI (a WCLAP webview GUI is itself a WKWebView): separate file
		std::function<WKWebView *(NSView *)> findWeb = [&](NSView *v) -> WKWebView * {
			if ([v isKindOfClass:[WKWebView class]]) return (WKWebView *)v;
			for (NSView *c in v.subviews)
				if (auto *w = findWeb(c)) return w;
			return nil;
		};
		if (WKWebView *innerWeb = innerHolder.isHidden ? nil : findWeb(innerHolder)) {
			std::string innerPath = path + ".inner.png";
			// PLUGINCIRCUIT_INNER_JS=<js>: run once in the inner webview; it can report through window.__probe
			static bool innerJsDone = false;
			if (const char *js = std::getenv("PLUGINCIRCUIT_INNER_JS"); js && !innerJsDone) {
				innerJsDone = true;
				[innerWeb evaluateJavaScript:[NSString stringWithUTF8String:js] completionHandler:nil];
			}
			[innerWeb evaluateJavaScript:@"location.href + ' | html=' + document.documentElement.outerHTML.length + ' | body=' + (document.body ? document.body.innerText.slice(0,120) : 'none') + ' | probe=' + (window.__probe || '')"
			           completionHandler:^(id result, NSError *error) {
				              std::fprintf(stderr, "[plugincircuit inner] %s %s\n", result ? [[result description] UTF8String] : "(nil)",
				                           error ? error.localizedDescription.UTF8String : "");
			           }];
			[innerWeb takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
				if (!image) return;
				NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:[image TIFFRepresentation]];
				[[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:innerPath.c_str()] atomically:YES];
			}];
		}
		NSView *innerView = innerHolder.isHidden ? nil : innerHolder.subviews.firstObject;
		[wv takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
			if (!image) return;
			NSImage *composite = [[NSImage alloc] initWithSize:image.size];
			[composite lockFocus];
			[image drawAtPoint:NSZeroPoint fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
			if (innerView) {
				// a WCLAP webview GUI is itself a WKWebView: snapshot it the same way, synchronously enough for debug
				if ([innerView isKindOfClass:[WKWebView class]]) {
					// drawn on the next snapshot round; mark the region
				}
				NSBitmapImageRep *rep = [innerView bitmapImageRepForCachingDisplayInRect:innerView.bounds];
				[innerView cacheDisplayInRect:innerView.bounds toBitmapImageRep:rep];
				NSRect f = [innerView convertRect:innerView.bounds toView:container];
				[rep drawInRect:NSMakeRect(f.origin.x, image.size.height - f.origin.y - f.size.height, f.size.width, f.size.height)];
			}
			[composite unlockFocus];
			NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:[composite TIFFRepresentation]];
			NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
			[png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
		}];
	}

	void applyInnerSize(uint32_t w, uint32_t h) {
		innerW = w;
		innerH = h;
		if (inLayout) { // the inner GUI answered our set_size with a request: settle it on the next tick
			pendingInnerSize.store((uint64_t(w) << 32) | h);
			return;
		}
		fitToInner();
	}

	// PLUGINCIRCUIT_DEBUG_CALLS: once a second, the main-thread calls into the plugin's parameters (docs/PLAN.md phase 1)
	std::chrono::steady_clock::time_point callsAt{};
	uint64_t callsLast[4] = {0, 0, 0, 0};
	void debugCalls() {
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
	std::chrono::steady_clock::time_point reconcileAt{};
	void reconcile() {
		if (view != "params") return;
		const auto now = std::chrono::steady_clock::now();
		if (now - reconcileAt < std::chrono::seconds(1)) return;
		reconcileAt = now;
		plugin.reconcileParams(64);
	}

	void tick() {
		debugCalls();
		reconcile();
		if (uint64_t s = pendingInnerSize.exchange(0)) applyInnerSize(uint32_t(s >> 32), uint32_t(s));
		if (uint64_t s = plugin.pendingInnerResize.exchange(0, std::memory_order_acquire)) applyInnerSize(uint32_t(s >> 32), uint32_t(s));
		debugHooks();
		plugin.housekeeping(); // also covers hosts without timer-support
		sendPatches();
	}

	bool create() {
		container = [[PluginCircuitContainerView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
		container.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		container.onResize = ^{
			if (!container.superview) return;
			width = actualW(); // the host resized us (drag, request_resize, or on its own)
			height = actualH();
			layout();
		};
		container.wantsLayer = YES;
		container.layer.backgroundColor = [NSColor colorWithSRGBRed:0x15 / 255.0 green:0x1b / 255.0 blue:0x24 / 255.0 alpha:1].CGColor;

		choc::ui::WebView::Options options;
		options.enableDebugMode = std::getenv("PLUGINCIRCUIT_DEBUG_UI") != nullptr;
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
		if (!web->loadedOK()) return false;
		registerApi();
		web->bind("plugincircuitRequest", [this](const choc::value::ValueView &args) -> choc::value::Value { return request(args); });
		NSView *wv = (__bridge NSView *)web->getViewHandle();
		[container addSubview:wv];

		innerHolder = [[PluginCircuitFlippedView alloc] initWithFrame:NSMakeRect(0, Editor::headerHeight, width, height - Editor::headerHeight)];
		[innerHolder setHidden:YES];
		[container addSubview:innerHolder]; // above the webview
		layout();

		// the view this instance was last on (its state), if it still makes sense: no plugin, no plugin views
		const std::string last = plugin.instance()["view"].getWithDefault<std::string>("plugin");
		view = plugin.innerForGui() ? last : "browse";
		if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] editor created, view %s\n", view.c_str());
		if (view == "plugin") createInnerGui();
		keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp)
		                                                      handler:^NSEvent *(NSEvent *e) {
			                                                      forwardNoteKey(e);
			                                                      return e;
		                                                      }];
		timer = choc::messageloop::Timer(50, [this] {
			tick();
			return true;
		});
		return true;
	}

	// The inner plugin's GUI is its own view (a WCLAP webview GUI is a WKWebView): once clicked it has the keyboard
	// focus, and the host page no longer sees key events. While the on-screen keyboard is open, copy the note keys
	// (by physical position, like the page's e.code mapping) to the page. The event still reaches the inner GUI,
	// so typing into its text fields keeps working.
	void forwardNoteKey(NSEvent *e) {
		if (!keyboardHeight || !web || !pageReady || !container.window || e.window != container.window) return;
		auto *responder = (NSView *)container.window.firstResponder;
		if (![responder isKindOfClass:[NSView class]] || ![responder isDescendantOf:innerHolder]) return;
		const bool down = e.type == NSEventTypeKeyDown;
		if (down && (e.isARepeat || (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))))
			return;
		static const std::pair<unsigned short, const char *> kCodes[] = {
			{0x00, "KeyA"}, {0x0D, "KeyW"}, {0x01, "KeyS"}, {0x0E, "KeyE"}, {0x02, "KeyD"}, {0x03, "KeyF"},
			{0x11, "KeyT"}, {0x05, "KeyG"}, {0x10, "KeyY"}, {0x04, "KeyH"}, {0x20, "KeyU"}, {0x26, "KeyJ"},
			{0x28, "KeyK"}, {0x1F, "KeyO"}, {0x25, "KeyL"}, {0x23, "KeyP"}, {0x29, "Semicolon"}, {0x27, "Quote"},
			{0x06, "KeyZ"}, {0x07, "KeyX"}};
		for (auto &[keyCode, code] : kCodes)
			if (e.keyCode == keyCode) {
				api.emit("keyboard.key", choc::value::createObject("Key", "code", code, "down", down));
				return;
			}
	}

	~Impl() {
		CatalogHealth::want(false);
		plugin.showDawNotes(false);
		container.onResize = nil;
		if (keyMonitor) [NSEvent removeMonitor:keyMonitor];
		keyMonitor = nil;
		plugin.uiAllNotesOff(); // notes held on the on-screen keyboard die with the editor
		timer = {};
		destroyInnerGui();
		web.reset();
		[innerHolder removeFromSuperview];
		[container removeFromSuperview];
	}
};

Editor::Editor(PluginCircuitPlugin &plugin) : impl(std::make_unique<Impl>(plugin)) {}
Editor::~Editor() = default;

bool Editor::isApiSupported(const char *api, bool isFloating) { return !isFloating && !std::strcmp(api, CLAP_WINDOW_API_COCOA); }
bool Editor::preferredApi(const char **api, bool *isFloating) {
	*api = CLAP_WINDOW_API_COCOA;
	*isFloating = false;
	return true;
}

bool Editor::create() { return impl->create(); }
bool Editor::setScale(double) { return false; } // cocoa: points, the OS scales
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
	const auto s = layout::clampEditor({w, h}, impl->minHeight());
	impl->width = s.w;
	impl->height = s.h;
	// the host sizes its parent view; ours follows it (autoresizing). Hosts that only call set_size: size it here.
	if (impl->container.superview) [impl->container setFrameSize:NSMakeSize(impl->width, impl->height)];
	impl->layout();
	return true;
}
bool Editor::setParent(const clap_window *window) {
	if (!window || std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) return false;
	NSView *parent = (__bridge NSView *)window->cocoa;
	[impl->container setFrameSize:NSMakeSize(impl->width, impl->height)];
	[parent addSubview:impl->container];
	impl->layout();
	return true;
}
bool Editor::show() {
	[impl->container setHidden:NO];
	return true;
}
bool Editor::hide() {
	impl->plugin.uiAllNotesOff();
	[impl->container setHidden:YES];
	return true;
}
void Editor::innerWillChange() { impl->destroyInnerGui(); }
void Editor::innerDidChange() {
	if (impl->plugin.innerForGui()) impl->view = "plugin"; // a finished load always lands on its GUI
	if (impl->view == "plugin") impl->createInnerGui();
	impl->layout(); // the ui store's innerGui changes with it: patched on the next tick
}
bool Editor::innerRequestResize(uint32_t w, uint32_t h) { // main thread (other threads: the plugin's pendingInnerResize)
	impl->applyInnerSize(w, h);
	return true;
}

} // namespace plugincircuit
