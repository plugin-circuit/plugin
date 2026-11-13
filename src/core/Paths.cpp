#include "core/Util.h"

#include <cstdlib>

namespace plugincircuit {

fs::path Paths::root() {
	if (const char *override = std::getenv("PLUGINCIRCUIT_HOME")) return fs::path(override);
	const char *home = std::getenv("HOME");
	return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "Plugin Circuit";
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
