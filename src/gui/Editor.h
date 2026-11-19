#pragma once
// Plugin Circuit's editor: one CHOC WebView for the whole host UI (header, browser/picker, macros, params),
// plus a native child view hosting the inner plugin's own GUI (wclap-bridge turns WCLAP webview GUIs
// into regular clap.gui views) placed below the header.
#include <clap/clap.h>

#include <cstdint>
#include <memory>

namespace plugincircuit {

class PluginCircuitPlugin;
class EditorCore;

class Editor {
public:
	explicit Editor(PluginCircuitPlugin &plugin);
	~Editor();

	static bool isApiSupported(const char *api, bool isFloating);
	static bool preferredApi(const char **api, bool *isFloating);

	bool create();
	bool setScale(double scale);
	bool getSize(uint32_t &w, uint32_t &h);
	bool adjustSize(uint32_t &w, uint32_t &h);
	bool setSize(uint32_t w, uint32_t h);
	bool setParent(const clap_window *window);
	bool show();
	bool hide();

	// main thread, called by the plugin around inner-plugin swaps
	void innerWillChange();
	void innerDidChange();
	bool innerRequestResize(uint32_t w, uint32_t h);

	static constexpr uint32_t headerHeight = 56;

private:
	std::unique_ptr<EditorCore> impl; // EditorCore.h: the shared logic; Editor_mac.mm / Editor_win.cpp: the windows
};

} // namespace plugincircuit
