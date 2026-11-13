#pragma once
// Client for the Plugin Circuit API (docs/API.md) plus the local content-addressed package cache.
#include "core/Util.h"

#include <optional>
#include <string>
#include <vector>

namespace plugincircuit {

struct CatalogPlugin {
	std::string id, name, vendor, type, description, license;
	std::string packageId, packageName, sha256, moduleSha256;
	uint64_t size = 0;
	bool hasWebGui = false;
};

class Catalog {
public:
	// Blocking network calls: never on the audio thread, avoid on the main thread.
	static std::optional<CatalogPlugin> fetchPlugin(const std::string &pluginId, std::string &error);

	// Returns the extracted bundle dir for a package, downloading + verifying it if needed.
	static fs::path ensurePackage(const std::string &sha256, const HttpProgress &progress, std::string &error);
	// Extracted dir if the package is already cached (no network).
	static fs::path cachedPackage(const std::string &sha256);
	// Installs a local .wclap.tar.gz (drag & drop / tests). Returns the extracted dir.
	static fs::path installArchive(const std::vector<uint8_t> &archive, std::string &sha256Out, std::string &error);
	// Remembers catalog metadata next to the cached bundle (for offline listing).
	static void writeMeta(const std::string &sha256, const CatalogPlugin &plugin);
	static std::vector<CatalogPlugin> installedPlugins();
};

} // namespace plugincircuit
