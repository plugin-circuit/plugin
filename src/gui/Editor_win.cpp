// The editor's Windows part (Win32): a child window in the host's HWND holding our CHOC WebView (WebView2) and,
// above it, a holder window for the inner plugin's GUI. The logic is in EditorCore.cpp. Sizes are in physical pixels
// for now: per-monitor DPI and CLAP's set_scale are open (docs/WINDOWS.md, W3).
#include "gui/EditorCore.h"

#include "plugin/PluginCircuitPlugin.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace plugincircuit {

const char *EditorCore::windowApi() { return CLAP_WINDOW_API_WIN32; }

namespace {

std::wstring widen(const std::string &s) {
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

HINSTANCE thisModule() {
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCWSTR>(&thisModule), &m);
	return m;
}

// Make a window a child of `parent` (CHOC creates its WebView2 host as a hidden popup).
void adopt(HWND child, HWND parent) {
	auto style = GetWindowLongPtrW(child, GWL_STYLE);
	style = (style & ~LONG_PTR(WS_POPUP)) | LONG_PTR(WS_CHILD | WS_CLIPSIBLINGS);
	SetWindowLongPtrW(child, GWL_STYLE, style);
	SetParent(child, parent);
	SetWindowPos(child, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

class WinEditor;

// One window class for the container and the holder; the container reports its size changes.
struct WindowClass {
	static constexpr const wchar_t *name = L"PluginCircuitEditorWindow";
	WindowClass() {
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = proc;
		wc.hInstance = thisModule();
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
		wc.hbrBackground = CreateSolidBrush(RGB(0x15, 0x1b, 0x24)); // the header's colour, behind everything
		wc.lpszClassName = name;
		RegisterClassExW(&wc); // fails harmlessly if another instance in this module registered it
	}
	static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

class WinEditor final : public EditorCore {
public:
	using EditorCore::EditorCore;

	bool setParent(const clap_window *window) override {
		if (!window || std::strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0 || !window->win32 || !container) return false;
		adopt(container, (HWND)window->win32);
		isParented = true;
		sizeContainer(width, height);
		ShowWindow(container, SW_SHOWNA);
		layout();
		return true;
	}
	void setVisible(bool visible) override {
		if (container) ShowWindow(container, visible ? SW_SHOWNA : SW_HIDE);
	}

	void containerResized() { hostResized(); }

protected:
	HWND container = nullptr, holder = nullptr;
	bool isParented = false;

	bool createViews() override {
		static WindowClass windowClass;
		// a hidden popup until the host parents it (setParent turns it into a child)
		container = CreateWindowExW(0, WindowClass::name, L"Plugin Circuit", WS_POPUP | WS_CLIPCHILDREN, 0, 0, int(width),
		                            int(height), nullptr, nullptr, thisModule(), nullptr);
		if (!container) return false;
		SetWindowLongPtrW(container, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		HWND webWindow = (HWND)web->getViewHandle();
		adopt(webWindow, container);
		ShowWindow(webWindow, SW_SHOWNA);
		holder = CreateWindowExW(0, WindowClass::name, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, int(Editor::headerHeight),
		                         int(width), int(height - Editor::headerHeight), container, nullptr, thisModule(), nullptr);
		if (!holder) return false;
		SetWindowPos(holder, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // above the webview
		return true;
	}

	void destroyViews() override {
		if (container) SetWindowLongPtrW(container, GWLP_USERDATA, 0);
		if (holder) DestroyWindow(holder);
		if (container) DestroyWindow(container);
		holder = container = nullptr;
		isParented = false;
	}

	void *innerParent() override { return holder; }
	bool parented() const override { return container && isParented; }
	layout::Size containerSize() const override {
		RECT r{};
		GetClientRect(container, &r);
		return {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
	}
	void sizeContainer(uint32_t w, uint32_t h) override {
		if (container) SetWindowPos(container, nullptr, 0, 0, int(w), int(h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	void placeWeb(uint32_t w, uint32_t h) override {
		// CHOC's window procedure fits the WebView2 controller to its window on WM_SIZE
		SetWindowPos((HWND)web->getViewHandle(), nullptr, 0, 0, int(w), int(h), SWP_NOZORDER | SWP_NOACTIVATE);
	}
	void placeHolder(const layout::Frame &area, bool visible) override {
		SetWindowPos(holder, HWND_TOP, int(area.x), int(area.y), int(area.w), int(area.h), SWP_NOACTIVATE);
		ShowWindow(holder, visible ? SW_SHOWNA : SW_HIDE);
	}

	std::vector<HWND> innerViews() const {
		std::vector<HWND> views;
		for (HWND c = GetWindow(holder, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) views.push_back(c);
		return views;
	}

	// Each inner view centred at its size (resizable GUIs at what adjust_size gave them). Fixed-size webview GUIs
	// aren't zoomed on Windows yet: WebView2's zoom lives in webview-gui's CHOC view (W4).
	void placeInnerViews(const layout::Frame &area, bool resizable, layout::Size own) override {
		for (HWND v : innerViews()) {
			RECT r{};
			GetWindowRect(v, &r);
			const layout::Size size = resizable ? own : layout::Size{uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
			const auto f = layout::placeInner(area, size, {innerW, innerH}, false);
			SetWindowPos(v, nullptr, int(f.x), int(f.y), int(f.w), int(f.h), SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	bool innerIsZoomableWebview() override { return false; }

	void openUrl(const std::string &url) override {
		ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
};

LRESULT CALLBACK WindowClass::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_SIZE)
		if (auto *editor = reinterpret_cast<WinEditor *>(GetWindowLongPtrW(h, GWLP_USERDATA))) editor->containerResized();
	if (msg == WM_ERASEBKGND) { // the class brush, without flicker from the default handling
		RECT r{};
		GetClientRect(h, &r);
		FillRect((HDC)wp, &r, (HBRUSH)GetClassLongPtrW(h, GCLP_HBRBACKGROUND));
		return 1;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

std::unique_ptr<EditorCore> EditorCore::make(PluginCircuitPlugin &plugin) { return std::make_unique<WinEditor>(plugin); }

} // namespace plugincircuit
