#pragma once
// Opens WCLAP bundles through wclap-bridge and shares one opened module per bundle directory.
#include <clap/clap.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace plugincircuit {

struct PluginDescriptorInfo {
	std::string id, name, vendor, version, description;
	std::vector<std::string> features;
};

class WclapModule {
public:
	~WclapModule();
	const clap_plugin_factory *factory() const { return factory_; }
	const std::string &dir() const { return dir_; }
	std::vector<PluginDescriptorInfo> descriptors() const;

private:
	friend class WclapLibrary;
	WclapModule() = default;
	void *handle_ = nullptr;
	const clap_plugin_factory *factory_ = nullptr;
	std::string dir_;
};

class WclapLibrary {
public:
	// Thread safe. Compiling a large module can take seconds: call off the main/audio thread.
	// `dataDir` gets presets/, cache/ and var/ subfolders for the WCLAP's WASI filesystem.
	static std::shared_ptr<WclapModule> open(const std::string &bundleDir, const std::string &dataDir, std::string &error);

private:
	static bool ensureGlobalInit(std::string &error);
};

} // namespace plugincircuit
