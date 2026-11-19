#pragma once
// The editor's logic, shared by every platform: the page's API (the `ui` and `app` stores, loading), the views, the
// inner plugin's GUI and the sizing rules (EditorLayout.h), the 50 ms tick. A platform (Editor_mac.mm,
// Editor_win.cpp) only provides the windows: a container in the host's parent, our CHOC WebView filling it, and a
// holder above it for the inner plugin's GUI.
#include "api/Api.h"
#include "gui/Editor.h"
#include "gui/EditorLayout.h"

#include <choc/gui/choc_MessageLoop.h>
#include <choc/gui/choc_WebView.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace plugincircuit {

class EditorCore {
public:
	explicit EditorCore(PluginCircuitPlugin &p);
	virtual ~EditorCore() = default;

	// the platform's CLAP window API (CLAP_WINDOW_API_COCOA, CLAP_WINDOW_API_WIN32)
	static const char *windowApi();
	static std::unique_ptr<EditorCore> make(PluginCircuitPlugin &plugin); // per platform

	bool create();
	void shutdown(); // before destruction, while the platform part still exists

	// --- for Editor ---
	layout::Size adjust(layout::Size proposed);
	void setSizeFromHost(uint32_t w, uint32_t h);
	void innerWillChange() { destroyInnerGui(); }
	void innerDidChange();
	void applyInnerSize(uint32_t w, uint32_t h);
	uint32_t minHeight() const { return innerGuiCreated ? layout::kMinHeightWithGui : layout::kMinHeight; }
	virtual bool setParent(const clap_window *window) = 0;
	virtual void setVisible(bool visible) = 0;
	virtual bool setScale(double) { return false; }

	PluginCircuitPlugin &plugin;
	uint32_t width = 1000, height = 680; // the size we want (get_size); the layout uses the container's real size

protected:
	// --- the platform part ---------------------------------------------------------------------------------
	virtual bool createViews() = 0;                   // container and inner holder, with `web`'s view in the container
	virtual void destroyViews() = 0;                  // after the inner GUI and `web` are gone
	virtual void *innerParent() = 0;                  // the holder's native handle, for the inner GUI's clap_window
	virtual bool parented() const = 0;                // the host gave us a parent
	virtual layout::Size containerSize() const = 0;   // the container's real size (parented)
	virtual void sizeContainer(uint32_t w, uint32_t h) = 0;
	virtual void placeWeb(uint32_t w, uint32_t h) = 0;
	virtual void placeHolder(const layout::Frame &area, bool visible) = 0;
	// the inner GUI's views inside the holder: `own` is the size to use for a resizable GUI (else each view's own)
	virtual void placeInnerViews(const layout::Frame &area, bool resizable, layout::Size own) = 0;
	virtual bool innerIsZoomableWebview() = 0;       // a fixed-size webview GUI we can scale with page zoom
	virtual void innerGuiShown() {}                   // e.g. its background colour
	virtual void clearHolder() {}                     // after the inner GUI is destroyed
	virtual void openUrl(const std::string &url) = 0;
	virtual void startPlatform() {}                   // e.g. key monitors
	virtual void stopPlatform() {}
	virtual void debugSnapshot(const std::string &) {} // PLUGINCIRCUIT_SNAPSHOT

	// --- shared ---------------------------------------------------------------------------------------------
	const clap_plugin_gui *innerGui();
	void createInnerGui();
	void destroyInnerGui();
	uint32_t actualW() const { return parented() ? containerSize().w : width; }
	uint32_t actualH() const { return parented() ? containerSize().h : height; }
	void requestSize(uint32_t w, uint32_t h);
	bool innerScalable();
	void fitToInner();
	void layout();
	void hostResized(); // the platform saw the container change size
	void setKeyboard(bool open);
	void setView(const std::string &v);
	void registerApi();
	choc::value::Value request(const choc::value::ValueView &args);
	void sendPatches();
	void debugHooks();
	void debugCalls();
	void reconcile();
	void tick();
	void noteKey(const char *code, bool down); // a note key typed into the inner GUI (keyboard open)
	bool keyboardOpen() const { return keyboardHeight > 0; }

	std::unique_ptr<choc::ui::WebView> web;
	choc::messageloop::Timer timer;
	bool inLayout = false;
	std::atomic<uint64_t> pendingInnerSize{0}; // inner request_resize during our layout: (w << 32 | h), applied in tick()
	std::string view = "browse";               // plugin | browse | macros | params | settings
	bool innerGuiCreated = false;
	uint32_t innerW = 0, innerH = 0;
	uint32_t keyboardHeight = 0; // on-screen keyboard strip at the bottom (instruments), drawn by the web UI
	static constexpr uint32_t kKeyboardHeight = 112;
	Api api;                // the page's one API (docs/MESSAGING.md): this editor is transport plus window logic
	bool pageReady = false; // the page said hello
	int ticks = 0;
	std::chrono::steady_clock::time_point callsAt{}, reconcileAt{};
	uint64_t callsLast[4] = {0, 0, 0, 0};
};

} // namespace plugincircuit
