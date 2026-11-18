#include "core/WclapLibrary.h"

#include "core/PackageCache.h"
#include "core/Settings.h"

#include "core/Util.h"

#include <map>
#include <wclap-bridge.h>

namespace plugincircuit {

namespace {
std::mutex libraryMutex;
std::map<std::string, std::weak_ptr<WclapModule>> openModules; // by bundle dir
bool globalInitDone = false;
} // namespace

WclapModule::~WclapModule() {
	std::lock_guard<std::mutex> lock(libraryMutex);
	if (handle_) wclap_close(handle_);
	// never wclap_global_deinit(): other instances in the same process may open modules later
}

std::vector<PluginDescriptorInfo> WclapModule::descriptors() const {
	std::vector<PluginDescriptorInfo> out;
	if (!factory_) return out;
	auto count = factory_->get_plugin_count(factory_);
	for (uint32_t i = 0; i < count; ++i) {
		auto *d = factory_->get_plugin_descriptor(factory_, i);
		if (!d) continue;
		PluginDescriptorInfo info;
		info.id = d->id ? d->id : "";
		info.name = d->name ? d->name : "";
		info.vendor = d->vendor ? d->vendor : "";
		info.version = d->version ? d->version : "";
		info.description = d->description ? d->description : "";
		for (auto f = d->features; f && *f; ++f) info.features.emplace_back(*f);
		out.push_back(std::move(info));
	}
	return out;
}

bool WclapLibrary::ensureGlobalInit(std::string &error) {
	if (globalInitDone) return true;
	// The bridge prefixes ids/names by default in its own plugin; we want the WCLAP's real ids.
	wclap_set_strings("", "", "");
	PackageCache::dropArchives();
	// compiled modules go to our own folder, with a size limit (Wasmtime prunes it)
	if (!wclap_global_init_with_cache(0, PackageCache::writeCompileCacheConfig().c_str())) {
		error = "wclap_global_init failed";
		return false;
	}
	globalInitDone = true;
	Settings::apply(); // the audio wait for single-threaded WCLAPs
	return true;
}

std::shared_ptr<WclapModule> WclapLibrary::open(const std::string &bundleDir, const std::string &dataDir, std::string &error) {
	std::unique_lock<std::mutex> lock(libraryMutex);
	if (auto existing = openModules[bundleDir].lock()) return existing;
	if (!ensureGlobalInit(error)) return nullptr;

	std::string presets, cache, var;
	if (!dataDir.empty()) {
		std::error_code ec;
		fs::path d(dataDir);
		for (auto sub : {"presets", "cache", "var"}) fs::create_directories(d / sub, ec);
		presets = (d / "presets").string();
		cache = (d / "cache").string();
		var = (d / "var").string();
	}

	void *handle = dataDir.empty() ? wclap_open(bundleDir.c_str())
	                               : wclap_open_with_dirs(bundleDir.c_str(), presets.c_str(), cache.c_str(), var.c_str());
	if (!handle) {
		error = "wclap_open failed for " + bundleDir;
		return nullptr;
	}
	char buffer[1024] = {0};
	if (wclap_get_error(handle, buffer, sizeof(buffer))) {
		error = std::string("WCLAP error: ") + buffer;
		wclap_close(handle);
		return nullptr;
	}
	auto *factory = (const clap_plugin_factory *)wclap_get_factory(handle, CLAP_PLUGIN_FACTORY_ID);
	if (!factory) {
		error = "WCLAP has no plugin factory";
		wclap_close(handle);
		return nullptr;
	}
	std::shared_ptr<WclapModule> module(new WclapModule());
	module->handle_ = handle;
	module->factory_ = factory;
	module->dir_ = bundleDir;
	openModules[bundleDir] = module;
	return module;
}

} // namespace plugincircuit
