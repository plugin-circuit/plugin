#include "core/Catalog.h"

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <random>

namespace plugincircuit {

namespace {

std::string str(const choc::value::ValueView &v, const char *key) {
	if (!v.isObject() || !v.hasObjectMember(key)) return {};
	auto m = v[key];
	return m.isString() ? std::string(m.getString()) : std::string();
}

CatalogPlugin fromJson(const choc::value::ValueView &v) {
	CatalogPlugin p;
	p.id = str(v, "id");
	p.name = str(v, "name");
	p.vendor = str(v, "vendor");
	p.type = str(v, "type");
	p.description = str(v, "description");
	p.license = str(v, "license");
	if (v.hasObjectMember("package")) {
		auto pkg = v["package"];
		p.packageId = str(pkg, "id");
		p.packageName = str(pkg, "name");
		p.sha256 = str(pkg, "sha256");
		p.moduleSha256 = str(pkg, "moduleSha256");
		if (pkg.hasObjectMember("size") && pkg["size"].isInt()) p.size = (uint64_t)pkg["size"].getInt64();
		else if (pkg.hasObjectMember("size") && pkg["size"].isFloat()) p.size = (uint64_t)pkg["size"].getFloat64();
		if (pkg.hasObjectMember("hasWebGui") && pkg["hasWebGui"].isBool()) p.hasWebGui = pkg["hasWebGui"].getBool();
	}
	return p;
}

choc::value::Value toJson(const CatalogPlugin &p) {
	auto pkg = choc::value::createObject("PackageRef", "id", p.packageId, "name", p.packageName, "sha256", p.sha256,
	                                     "moduleSha256", p.moduleSha256, "size", (int64_t)p.size, "hasWebGui", p.hasWebGui);
	return choc::value::createObject("PluginSummary", "id", p.id, "name", p.name, "vendor", p.vendor, "type", p.type,
	                                 "description", p.description, "license", p.license, "package", pkg);
}

bool isHexSha(const std::string &s) {
	if (s.size() != 64) return false;
	for (char c : s)
		if (!isxdigit((unsigned char)c)) return false;
	return true;
}

std::string randomSuffix() {
	std::random_device rd;
	return std::to_string(rd()) + std::to_string(rd());
}

} // namespace

std::optional<CatalogPlugin> Catalog::fetchPlugin(const std::string &pluginId, std::string &error) {
	auto res = httpGetString(Paths::apiBase() + "/v1/plugins/" + urlEncode(pluginId), 15);
	if (!res.ok()) {
		error = res.error.empty() ? "catalog HTTP " + std::to_string(res.status) : res.error;
		return std::nullopt;
	}
	try {
		auto json = choc::json::parse(res.text());
		auto p = fromJson(json);
		if (p.id.empty() || !isHexSha(p.sha256)) {
			error = "catalog response missing id/sha256";
			return std::nullopt;
		}
		return p;
	} catch (const std::exception &e) {
		error = std::string("catalog JSON: ") + e.what();
		return std::nullopt;
	}
}

fs::path Catalog::cachedPackage(const std::string &sha256) {
	if (!isHexSha(sha256)) return {};
	auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::exists(dir / "bundle", ec)) return {};
	return findWclapRoot(dir / "bundle");
}

static fs::path installVerified(const std::vector<uint8_t> &archive, const std::string &sha256, std::string &error) {
	auto finalDir = Paths::packages() / sha256;
	if (auto existing = Catalog::cachedPackage(sha256); !existing.empty()) return existing;
	std::error_code ec;
	fs::create_directories(Paths::packages(), ec);
	auto tmp = Paths::packages() / (".tmp-" + sha256.substr(0, 12) + "-" + randomSuffix());
	if (archive.size() >= 4 && archive[0] == 0 && archive[1] == 'a' && archive[2] == 's' && archive[3] == 'm') {
		// a bare module.wasm instead of a bundle tarball
		fs::create_directories(tmp / "bundle", ec);
		std::ofstream m(tmp / "bundle" / "module.wasm", std::ios::binary);
		m.write(reinterpret_cast<const char *>(archive.data()), (std::streamsize)archive.size());
		error.clear();
	} else {
		error = extractTarGz(archive, tmp / "bundle");
	}
	if (!error.empty()) {
		fs::remove_all(tmp, ec);
		return {};
	}
	{
		std::ofstream f(tmp / "bundle.wclap.tar.gz", std::ios::binary);
		f.write(reinterpret_cast<const char *>(archive.data()), (std::streamsize)archive.size());
	}
	if (findWclapRoot(tmp / "bundle").empty()) {
		error = "bundle has no module.wasm";
		fs::remove_all(tmp, ec);
		return {};
	}
	fs::rename(tmp, finalDir, ec);
	if (ec) { // another instance won the race
		fs::remove_all(tmp, ec);
	}
	auto root = Catalog::cachedPackage(sha256);
	if (root.empty()) error = "install failed for " + sha256;
	return root;
}

fs::path Catalog::ensurePackage(const std::string &sha256, const HttpProgress &progress, std::string &error) {
	if (!isHexSha(sha256)) {
		error = "invalid sha256";
		return {};
	}
	if (auto cached = cachedPackage(sha256); !cached.empty()) return cached;
	std::error_code ec;
	auto partial = Paths::packages() / ".downloads" / (sha256 + ".part");
	fs::create_directories(partial.parent_path(), ec);
	auto res = httpDownload(Paths::apiBase() + "/v1/bundles/" + sha256, partial, progress, 600);
	if (!res.ok()) {
		error = res.error.empty() ? "download HTTP " + std::to_string(res.status) : res.error;
		return {};
	}
	auto actual = sha256HexOfFile(partial);
	if (actual != sha256) {
		fs::remove(partial, ec); // corrupt or stale partial: start over next time
		error = "sha256 mismatch: expected " + sha256 + ", got " + actual;
		return {};
	}
	std::ifstream f(partial, std::ios::binary);
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
	f.close();
	auto dir = installVerified(bytes, sha256, error);
	fs::remove(partial, ec);
	return dir;
}

fs::path Catalog::installArchive(const std::vector<uint8_t> &archive, std::string &sha256Out, std::string &error) {
	sha256Out = sha256Hex(archive.data(), archive.size());
	return installVerified(archive, sha256Out, error);
}

void Catalog::writeMeta(const std::string &sha256, const CatalogPlugin &plugin) {
	if (!isHexSha(sha256)) return;
	auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::exists(dir, ec)) return;
	// one meta file per plugin id inside the package
	auto metaDir = dir / "meta";
	fs::create_directories(metaDir, ec);
	std::ofstream f(metaDir / (sha256Hex(plugin.id.data(), plugin.id.size()).substr(0, 16) + ".json"));
	f << choc::json::toString(toJson(plugin), true);
}

std::vector<CatalogPlugin> Catalog::installedPlugins() {
	std::vector<CatalogPlugin> out;
	std::error_code ec;
	for (auto &pkg : fs::directory_iterator(Paths::packages(), ec)) {
		if (!pkg.is_directory() || pkg.path().filename().string().rfind(".tmp", 0) == 0) continue;
		for (auto &meta : fs::directory_iterator(pkg.path() / "meta", ec)) {
			std::ifstream f(meta.path());
			std::string text((std::istreambuf_iterator<char>(f)), {});
			try {
				out.push_back(fromJson(choc::json::parse(text)));
			} catch (...) {
			}
		}
	}
	return out;
}

} // namespace plugincircuit
