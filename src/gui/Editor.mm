#include "gui/Editor.h"

#include "plugin/PluginCircuitPlugin.h"

#include <choc/gui/choc_MessageLoop.h>
#include <choc/gui/choc_WebView.h>
#include <choc/text/choc_JSON.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

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

namespace plugincircuit {

struct Editor::Impl {
	PluginCircuitPlugin &plugin;
	PluginCircuitFlippedView *container = nil;
	PluginCircuitFlippedView *innerHolder = nil;
	std::unique_ptr<choc::ui::WebView> web;
	choc::messageloop::Timer timer;
	uint32_t width = 1000, height = 680;
	std::string view = "browse"; // plugin | browse | macros | params
	bool innerGuiCreated = false;
	uint32_t innerW = 0, innerH = 0;
	uint32_t keyboardHeight = 0; // on-screen keyboard strip at the bottom (instruments), drawn by the web UI
	static constexpr uint32_t kKeyboardHeight = 112;
	static constexpr uint32_t kMinWidth = 640, kMinHeight = 420; // adjustSize's limits
	uint64_t lastVersion = 0;
	bool pageReady = false;
	id keyMonitor = nil; // copies note keys typed into the inner plugin's GUI to the on-screen keyboard

	explicit Impl(PluginCircuitPlugin &p) : plugin(p) {}

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
		fitToInner();
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

	void fitToInner() {
		if (!innerGuiCreated || innerW == 0 || innerH == 0) return;
		uint32_t w = std::max<uint32_t>(innerW, 760), h = innerH + Editor::headerHeight + keyboardHeight;
		if (w != width || h != height) {
			const uint32_t oldW = width, oldH = height;
			width = w;
			height = h;
			layout();
			if (!plugin.requestHostResize(w, h)) { // refused: keep the window, the inner GUI is scaled/centred instead
				width = oldW;
				height = oldH;
				layout();
			}
		}
	}

	void layout() {
		if (!container) return;
		[container setFrameSize:NSMakeSize(width, height)];
		if (web) {
			NSView *wv = (__bridge NSView *)web->getViewHandle();
			[wv setFrame:NSMakeRect(0, 0, width, height)];
		}
		uint32_t innerAreaH = height > Editor::headerHeight + keyboardHeight ? height - Editor::headerHeight - keyboardHeight : 0;
		[innerHolder setFrame:NSMakeRect(0, Editor::headerHeight, width, innerAreaH)];
		bool showInner = view == "plugin" && innerGuiCreated;
		[innerHolder setHidden:!showInner];
		if (innerGuiCreated && showInner) {
			auto *inner = plugin.innerForGui();
			auto *gui = innerGui();
			if (inner && gui && gui->can_resize(inner->plugin())) {
				uint32_t w = width, h = innerAreaH;
				bool adj = gui->adjust_size(inner->plugin(), &w, &h);
				if (adj) gui->set_size(inner->plugin(), w, h);
				if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner resizable: area %ux%u -> adjust %d %ux%u\n", width, innerAreaH, int(adj), w, h);
			}
			// A fixed-size inner GUI: centre it. A WCLAP webview GUI (a WKWebView) is scaled to fill the area,
			// keeping its aspect ratio, with WebKit's page zoom (the page still lays out at its own size).
			for (NSView *v in innerHolder.subviews) {
				NSRect f = v.frame;
				bool fixed = !(inner && gui && gui->can_resize(inner->plugin()));
				if (fixed && innerW > 0 && innerH > 0 && [v isKindOfClass:[WKWebView class]] && innerAreaH > 0) {
					double s = std::clamp(std::min(double(width) / innerW, double(innerAreaH) / innerH), 0.5, 4.0);
					((WKWebView *)v).pageZoom = s;
					f.size = NSMakeSize(std::floor(innerW * s), std::floor(innerH * s));
				}
				if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner view %.0fx%.0f in %ux%u, fixed %d\n", f.size.width, f.size.height, width, innerAreaH, int(fixed));
				f.origin.x = std::max<CGFloat>(0, (width - f.size.width) / 2);
				f.origin.y = 0;
				[v setFrame:f];
			}
		}
	}

	void setKeyboard(bool open) {
		uint32_t kb = open ? kKeyboardHeight : 0;
		if (kb == keyboardHeight) return;
		if (!open) plugin.uiAllNotesOff();
		// grow the window rather than squeeze the plugin's GUI; if the host refuses, the strip takes the space
		const uint32_t oldH = height;
		height = std::max<uint32_t>(height - keyboardHeight + kb, kMinHeight);
		keyboardHeight = kb;
		if (height != oldH && !plugin.requestHostResize(width, height)) height = oldH;
		layout();
	}

	void setView(const std::string &v) {
		if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] setView %s -> %s\n", view.c_str(), v.c_str());
		view = v;
		if (view == "plugin") createInnerGui();
		layout();
	}

	// --- web UI --------------------------------------------------------------------------------------------
	void push(bool full) {
		if (!web || !pageReady) return;
		auto snap = plugin.uiSnapshot(full);
		snap.addMember("view", view);
		snap.addMember("innerGui", innerGuiCreated);
		snap.addMember("keyboard", keyboardHeight > 0);
		web->evaluateJavascript("try { window.plugincircuitHost && window.plugincircuitHost.update(" + choc::json::toString(snap) + ") } catch (e) { plugincircuitCall('log', {text: 'update failed: ' + e + ' ' + e.stack}) }");
	}

	// Debug: PLUGINCIRCUIT_SNAPSHOT=<file.png> writes the webview (WebKit's own renderer, no screen-recording
	// permission needed) plus the inner GUI view every 2 s. PLUGINCIRCUIT_AUTOLOAD=<pluginId> loads at start,
	// PLUGINCIRCUIT_VIEW=<view> picks the tab.
	int ticks = 0;
	void debugHooks() {
		++ticks;
		if (ticks == 10) {
			if (const char *id = std::getenv("PLUGINCIRCUIT_AUTOLOAD")) plugin.uiLoad(id, "");
			if (const char *v = std::getenv("PLUGINCIRCUIT_VIEW")) { setView(v); push(true); }
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

	void tick() {
		debugHooks();
		plugin.housekeeping(); // also covers hosts without timer-support
		auto version = plugin.stateVersion_.load();
		bool full = version != lastVersion;
		lastVersion = version;
		push(full);
	}

	choc::value::Value call(const choc::value::ValueView &args) {
		std::string method = args.size() > 0 && args[0].isString() ? std::string(args[0].getString()) : "";
		auto a = args.size() > 1 ? args[1] : choc::value::ValueView();
		auto num = [&](const char *k, double d = 0) {
			return a.isObject() && a.hasObjectMember(k) && (a[k].isFloat() || a[k].isInt()) ? a[k].getWithDefault<double>(d) : d;
		};
		auto str = [&](const char *k) {
			return a.isObject() && a.hasObjectMember(k) && a[k].isString() ? std::string(a[k].getString()) : std::string();
		};
		if (method == "ready") {
			pageReady = true;
			lastVersion = 0;
			push(true);
		} else if (method == "load") {
			plugin.uiLoad(str("pluginId"), str("sha256"));
			setView("plugin");
		} else if (method == "unload") {
			plugin.uiUnload();
			setView("browse");
		} else if (method == "view") {
			setView(str("view"));
			push(true);
		} else if (method == "setMacro") {
			plugin.uiSetMacro((uint32_t)num("slot"), num("value"), (int)num("gesture"));
		} else if (method == "note") {
			plugin.uiNote((int16_t)num("key", -1), num("velocity", 0.8), num("on") != 0);
		} else if (method == "transport") {
			plugin.uiSetTransport(num("playing") != 0);
		} else if (method == "keyboard") {
			setKeyboard(num("open") != 0);
		} else if (method == "setParam") {
			plugin.uiSetParam((clap_id)num("id"), num("value"));
		} else if (method == "mapMacro") {
			plugin.uiMapMacro((uint32_t)num("slot"), (clap_id)num("paramId"), num("min"), num("max", 1), num("add") != 0);
		} else if (method == "unmapMacro") {
			plugin.uiUnmapMacro((uint32_t)num("slot"), (int64_t)num("paramId", -1));
		} else if (method == "renameMacro") {
			plugin.uiRenameMacro((uint32_t)num("slot"), str("name"));
		} else if (method == "autoMap") {
			plugin.uiAutoMap();
		} else if (method == "log") {
			std::fprintf(stderr, "[plugincircuit ui] %s\n", str("text").c_str());
		} else if (method == "openUrl") {
			auto url = str("url");
			if (url.rfind("http", 0) == 0)
				[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
		}
		return choc::value::Value(true);
	}

	bool create() {
		container = [[PluginCircuitFlippedView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
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
		web->bind("plugincircuitCall", [this](const choc::value::ValueView &args) -> choc::value::Value { return call(args); });
		NSView *wv = (__bridge NSView *)web->getViewHandle();
		[container addSubview:wv];

		innerHolder = [[PluginCircuitFlippedView alloc] initWithFrame:NSMakeRect(0, Editor::headerHeight, width, height - Editor::headerHeight)];
		[innerHolder setHidden:YES];
		[container addSubview:innerHolder]; // above the webview
		layout();

		view = plugin.innerForGui() ? "plugin" : "browse";
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
				web->evaluateJavascript(std::string("window.plugincircuitHost && window.plugincircuitHost.key && window.plugincircuitHost.key('") +
				                        code + "', " + (down ? "true" : "false") + ")");
				return;
			}
	}

	~Impl() {
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
	w = std::max<uint32_t>(w, Impl::kMinWidth);
	h = std::max<uint32_t>(h, Impl::kMinHeight);
	return true;
}
bool Editor::setSize(uint32_t w, uint32_t h) {
	impl->width = std::max<uint32_t>(w, Impl::kMinWidth);
	impl->height = std::max<uint32_t>(h, Impl::kMinHeight);
	impl->layout();
	return true;
}
bool Editor::setParent(const clap_window *window) {
	if (!window || std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) return false;
	NSView *parent = (__bridge NSView *)window->cocoa;
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
	impl->layout();
	impl->lastVersion = 0;
}
bool Editor::innerRequestResize(uint32_t w, uint32_t h) {
	impl->innerW = w;
	impl->innerH = h;
	impl->fitToInner();
	return true;
}

} // namespace plugincircuit
