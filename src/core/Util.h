#pragma once
// Small shared helpers for the Plugin Circuit core: SHA-256, gzip+tar extraction, HTTP, paths.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace plugincircuit {

namespace fs = std::filesystem;

// --- SHA-256 ---------------------------------------------------------------------------
std::string sha256Hex(const void *data, size_t size);
std::string sha256HexOfFile(const fs::path &file); // "" on read error

// --- gzip + ustar ----------------------------------------------------------------------
// Extracts a .tar.gz held in memory into `destDir` (created). Rejects absolute paths and "..".
// Returns "" on success, else an error message.
std::string extractTarGz(const std::vector<uint8_t> &gz, const fs::path &destDir);
// Finds the directory holding module.wasm (destDir itself or a nested folder). Empty path if none.
fs::path findWclapRoot(const fs::path &dir);

// --- HTTP ------------------------------------------------------------------------------
struct HttpResult {
	long status = 0;
	std::vector<uint8_t> body;
	std::string error; // transport error (empty when a response arrived)
	bool ok() const { return error.empty() && status >= 200 && status < 300; }
	std::string text() const { return {body.begin(), body.end()}; }
};
// progress(received, total) is called on a worker thread; return false to cancel. total may be 0 (unknown).
using HttpProgress = std::function<bool(uint64_t received, uint64_t total)>;
// Small resources (JSON) into memory.
HttpResult httpGetString(const std::string &url, long timeoutSeconds = 15);
// Large payloads into a file, resuming a partial file when present. `body` stays empty.
HttpResult httpDownload(const std::string &url, const fs::path &destination, const HttpProgress &progress = {},
                        long timeoutSeconds = 120);
std::string urlEncode(const std::string &s);

// --- paths -----------------------------------------------------------------------------
struct Paths {
	static fs::path root();                                // ~/Library/Application Support/PluginCircuit
	static fs::path packages() { return root() / "packages"; } // <sha256>/ extracted bundles
	static fs::path compiled() { return root() / "compiled"; } // Wasmtime's compile cache (PackageCache)
	static fs::path pluginData(const std::string &packageId); // presets/cache/var for one package
	static std::string apiBase();                          // PLUGINCIRCUIT_API env or http://127.0.0.1:7790
};

} // namespace plugincircuit
