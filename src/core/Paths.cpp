#include "core/Util.h"

#include <cstdlib>

namespace plugincircuit {

// The per-user data folder: macOS ~/Library/Application Support/Plugin Circuit, Windows %APPDATA%\Plugin Circuit,
// Linux $XDG_DATA_HOME/plugin-circuit (default ~/.local/share). PLUGINCIRCUIT_HOME overrides it (tests, the SDK).
fs::path Paths::root() {
#if defined(_WIN32)
	// wide: a user name with non-ASCII characters survives (the narrow environment is the ANSI code page)
	if (const wchar_t *override = _wgetenv(L"PLUGINCIRCUIT_HOME")) return fs::path(override);
	if (const wchar_t *appData = _wgetenv(L"APPDATA")) return fs::path(appData) / L"Plugin Circuit";
	if (const wchar_t *profile = _wgetenv(L"USERPROFILE")) return fs::path(profile) / L"AppData" / L"Roaming" / L"Plugin Circuit";
	return fs::temp_directory_path() / "Plugin Circuit";
#else
	if (const char *override = std::getenv("PLUGINCIRCUIT_HOME")) return fs::path(override);
	const char *home = std::getenv("HOME");
#	if defined(__APPLE__)
	return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "Plugin Circuit";
#	else
	if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return fs::path(xdg) / "plugin-circuit";
	return fs::path(home ? home : "/tmp") / ".local" / "share" / "plugin-circuit";
#	endif
#endif
}

fs::path Paths::pluginData(const std::string &packageId) {
	std::string safe;
	for (char c : packageId) safe += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
	return root() / "data" / (safe.empty() ? "unknown" : safe);
}

std::string Paths::apiBase() {
	if (const char *env = std::getenv("PLUGINCIRCUIT_API")) {
		std::string s = env;
		while (!s.empty() && s.back() == '/') s.pop_back();
		if (!s.empty()) return s;
	}
	return "http://127.0.0.1:7790";
}

} // namespace plugincircuit
