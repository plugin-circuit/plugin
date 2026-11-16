// libplugincircuit C API (include/plugincircuit/plugincircuit.h) on top of plugincircuit-core.
#include <plugincircuit/plugincircuit.h>

#include "core/Catalog.h"
#include "core/WclapLibrary.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace plugincircuit;

struct plugincircuit_module {
	std::shared_ptr<WclapModule> module;
};

namespace {

void setError(char *buffer, size_t capacity, const std::string &message) {
	if (!buffer || capacity == 0) return;
	std::snprintf(buffer, capacity, "%s", message.c_str());
}

char *dup(const std::string &s) {
	auto *out = static_cast<char *>(std::malloc(s.size() + 1));
	if (out) std::memcpy(out, s.c_str(), s.size() + 1);
	return out;
}

char *getJson(const std::string &url, char *error, size_t errorCapacity) {
	auto res = httpGetString(url, 20);
	if (!res.ok()) {
		setError(error, errorCapacity, res.error.empty() ? "HTTP " + std::to_string(res.status) : res.error);
		return nullptr;
	}
	return dup(res.text());
}

} // namespace

extern "C" {

void plugincircuit_configure(const char *apiBase, const char *cacheDir) {
	// Paths reads the environment, so configuration goes through it (process-wide, like the cache).
	if (apiBase && *apiBase) setenv("PLUGINCIRCUIT_API", apiBase, 1);
	if (cacheDir && *cacheDir) setenv("PLUGINCIRCUIT_HOME", cacheDir, 1);
}

char *plugincircuit_search_json(const char *query, char *error, size_t errorCapacity) {
	return getJson(Paths::apiBase() + "/v1/plugins" + (query && *query ? std::string("?") + query : std::string()), error,
	               errorCapacity);
}

char *plugincircuit_plugin_json(const char *pluginId, char *error, size_t errorCapacity) {
	if (!pluginId) return nullptr;
	return getJson(Paths::apiBase() + "/v1/plugins/" + urlEncode(pluginId), error, errorCapacity);
}

void plugincircuit_free(void *ptr) { std::free(ptr); }

bool plugincircuit_fetch(const char *pluginId, const char *sha256, plugincircuit_progress_fn progress, void *progressContext,
                     char *outDir, size_t outDirCapacity, char *error, size_t errorCapacity) {
	if (!pluginId) {
		setError(error, errorCapacity, "plugin_id is NULL");
		return false;
	}
	std::string err;
	std::string sha = sha256 ? sha256 : "";
	CatalogPlugin meta;
	if (auto fetched = Catalog::fetchPlugin(pluginId, err)) {
		meta = *fetched;
		if (sha.empty()) sha = meta.sha256;
	} else if (sha.empty()) {
		setError(error, errorCapacity, "catalog: " + err);
		return false;
	}
	HttpProgress cb;
	if (progress) cb = [&](uint64_t got, uint64_t total) { return progress(progressContext, got, total); };
	auto dir = Catalog::ensurePackage(sha, cb, err);
	if (dir.empty()) {
		setError(error, errorCapacity, err);
		return false;
	}
	if (!meta.id.empty()) Catalog::writeMeta(sha, meta);
	auto s = dir.string();
	if (!outDir || s.size() + 1 > outDirCapacity) {
		setError(error, errorCapacity, "out_dir too small");
		return false;
	}
	std::memcpy(outDir, s.c_str(), s.size() + 1);
	return true;
}

plugincircuit_module *plugincircuit_open(const char *bundleDir, char *error, size_t errorCapacity) {
	if (!bundleDir) return nullptr;
	std::string err;
	// Same data dir as the host plugin: keyed by package id (catalog meta), else by the package's sha256 dir.
	std::string key;
	std::error_code ec;
	auto rel = fs::relative(fs::weakly_canonical(bundleDir, ec), fs::weakly_canonical(Paths::packages(), ec), ec);
	if (!ec && !rel.empty() && *rel.begin() != "..") {
		const auto sha = rel.begin()->string();
		for (auto &p : Catalog::installedPlugins())
			if (p.sha256 == sha) key = p.packageId.empty() ? p.id : p.packageId;
		if (key.empty()) key = sha;
	} else {
		key = fs::path(bundleDir).filename().string(); // outside the cache: a local bundle
	}
	auto dataDir = Paths::pluginData(key);
	auto module = WclapLibrary::open(bundleDir, dataDir.string(), err);
	if (!module) {
		setError(error, errorCapacity, err);
		return nullptr;
	}
	return new plugincircuit_module{std::move(module)};
}

const clap_plugin_factory *plugincircuit_factory(plugincircuit_module *m) { return m ? m->module->factory() : nullptr; }

void plugincircuit_close(plugincircuit_module *m) { delete m; }

} // extern "C"
