#pragma once
// The local caches: downloaded packages (Paths::packages(), one folder per sha256) and Wasmtime's compiled code
// (Paths::compiled()). Packages record when they were last loaded and last saved in a DAW session (usage.json), so
// the settings page can tell what's in use. A loaded package holds a shared lock on its folder; deleting needs the
// exclusive lock, so a package loaded anywhere (in any DAW process) is never deleted.
// All functions do file IO: main thread or a worker, never the audio thread.
#include "core/Util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace plugincircuit {

struct CachedPackage {
	std::string sha256;
	std::string name, vendor; // from the catalog metadata stored with it (first plugin in the package)
	uint64_t bytes = 0;
	int64_t lastUsed = 0;  // unix seconds: last loaded (else the folder's time)
	int64_t lastSaved = 0; // unix seconds: last saved in a DAW session, 0 if never
	bool inUse = false;    // loaded right now, in this or another process
};

class PackageCache {
public:
	static std::vector<CachedPackage> list();
	static uint64_t compiledBytes();

	static void markUsed(const std::string &sha256);
	static void markSaved(const std::string &sha256); // cheap to call on every state save: writes at most hourly

	// Deletes a package folder unless it's in use. Returns false (and why) if it wasn't deleted.
	static bool remove(const std::string &sha256, std::string &why);
	static void clearCompiled();

	// Removes the downloaded archives older versions kept next to each extracted bundle (once per process).
	static void dropArchives();

	// Writes Wasmtime's compile cache config (our folder, a size limit) and returns its path.
	static std::string writeCompileCacheConfig();

	// Held while a package is loaded: a shared lock on <package>/.lock. Blocks while a delete or a listing holds the
	// exclusive lock (only for a moment): construct it off the audio and main threads (the loader does).
	// held() is false if the package was deleted meanwhile: fetch it again and take a new InUse.
	class InUse {
	public:
		InUse() = default;
		explicit InUse(const std::string &sha256);
		bool held() const { return fd_ != -1; }
		~InUse();
		InUse(InUse &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
		InUse &operator=(InUse &&o) noexcept;
		InUse(const InUse &) = delete;
		InUse &operator=(const InUse &) = delete;

	private:
		intptr_t fd_ = -1; // a file descriptor, or a Windows HANDLE
	};
};

} // namespace plugincircuit
