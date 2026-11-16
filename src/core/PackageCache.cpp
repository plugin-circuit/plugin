#include "core/PackageCache.h"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#if defined(_WIN32)
// TODO(windows): LockFileEx. Until then packages count as not in use.
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace plugincircuit {

namespace {

constexpr int64_t kSavedWriteInterval = 3600; // markSaved writes at most hourly (hosts save often)

int64_t now() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isHexSha(const std::string &s) {
	if (s.size() != 64) return false;
	for (char c : s)
		if (!isxdigit((unsigned char)c)) return false;
	return true;
}

int64_t fileTime(const fs::path &p) {
	std::error_code ec;
	auto t = fs::last_write_time(p, ec);
	if (ec) return 0;
	auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
	return sys.time_since_epoch().count();
}

uint64_t dirBytes(const fs::path &dir) {
	uint64_t total = 0;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
	     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (it->is_regular_file(ec)) total += it->file_size(ec);
	}
	return total;
}

struct Usage {
	int64_t lastUsed = 0, lastSaved = 0;
};

std::mutex usageMutex; // usage.json read-modify-write within this process

Usage readUsage(const fs::path &dir) {
	Usage u;
	std::ifstream f(dir / "usage.json");
	if (!f) return u;
	std::stringstream ss;
	ss << f.rdbuf();
	try {
		auto v = choc::json::parse(ss.str());
		if (v.hasObjectMember("lastUsed")) u.lastUsed = v["lastUsed"].getWithDefault<int64_t>(0);
		if (v.hasObjectMember("lastSaved")) u.lastSaved = v["lastSaved"].getWithDefault<int64_t>(0);
	} catch (...) {
	}
	return u;
}

void writeUsage(const fs::path &dir, const Usage &u) {
	auto tmp = dir / (".usage-" + std::to_string(std::random_device{}()) + ".json");
	{
		std::ofstream f(tmp);
		if (!f) return;
		f << choc::json::toString(choc::value::createObject("Usage", "lastUsed", u.lastUsed, "lastSaved", u.lastSaved));
	}
	std::error_code ec;
	fs::rename(tmp, dir / "usage.json", ec); // atomic: another process may read it
	if (ec) fs::remove(tmp, ec);
}

#if !defined(_WIN32)
// Try the exclusive lock: true if nobody holds the package (the caller then owns the returned fd).
int tryExclusive(const fs::path &dir) {
	int fd = ::open((dir / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) return -1;
	if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}
#endif

void readName(const fs::path &dir, CachedPackage &p) {
	std::error_code ec;
	for (auto &meta : fs::directory_iterator(dir / "meta", ec)) {
		std::ifstream f(meta.path());
		std::stringstream ss;
		ss << f.rdbuf();
		try {
			auto v = choc::json::parse(ss.str());
			if (p.name.empty() && v.hasObjectMember("name") && v["name"].isString()) p.name = std::string(v["name"].getString());
			if (p.vendor.empty() && v.hasObjectMember("vendor") && v["vendor"].isString()) p.vendor = std::string(v["vendor"].getString());
		} catch (...) {
		}
		if (!p.name.empty()) return;
	}
	// no catalog metadata (a local install): the bundle folder's name
	for (auto &e : fs::directory_iterator(dir / "bundle", ec)) {
		p.name = e.path().stem().string();
		return;
	}
}

} // namespace

std::vector<CachedPackage> PackageCache::list() {
	std::vector<CachedPackage> out;
	std::error_code ec;
	for (auto &e : fs::directory_iterator(Paths::packages(), ec)) {
		auto sha = e.path().filename().string();
		if (!isHexSha(sha) || !e.is_directory(ec)) continue;
		CachedPackage p;
		p.sha256 = sha;
		readName(e.path(), p);
		p.bytes = dirBytes(e.path());
		Usage u;
		{
			std::lock_guard<std::mutex> lock(usageMutex);
			u = readUsage(e.path());
		}
		p.lastUsed = u.lastUsed ? u.lastUsed : fileTime(e.path() / "bundle");
		p.lastSaved = u.lastSaved;
#if !defined(_WIN32)
		if (int fd = tryExclusive(e.path()); fd >= 0) ::close(fd); // got it: nobody has it loaded
		else p.inUse = true;
#endif
		out.push_back(std::move(p));
	}
	return out;
}

uint64_t PackageCache::compiledBytes() { return dirBytes(Paths::compiled()); }

void PackageCache::markUsed(const std::string &sha256) {
	if (!isHexSha(sha256)) return;
	auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return;
	std::lock_guard<std::mutex> lock(usageMutex);
	auto u = readUsage(dir);
	u.lastUsed = now();
	writeUsage(dir, u);
}

void PackageCache::markSaved(const std::string &sha256) {
	if (!isHexSha(sha256)) return;
	auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return;
	std::lock_guard<std::mutex> lock(usageMutex);
	auto u = readUsage(dir);
	const auto t = now();
	if (t - u.lastSaved < kSavedWriteInterval) return;
	u.lastSaved = t;
	u.lastUsed = std::max(u.lastUsed, t);
	writeUsage(dir, u);
}

bool PackageCache::remove(const std::string &sha256, std::string &why) {
	if (!isHexSha(sha256)) {
		why = "invalid package";
		return false;
	}
	auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return true; // already gone
#if !defined(_WIN32)
	int fd = tryExclusive(dir);
	if (fd < 0) {
		why = "in use";
		return false;
	}
#endif
	// Move it out of the way first (atomic), so a load never sees a half-deleted package, then delete.
	auto trash = Paths::packages() / (".trash-" + sha256.substr(0, 12) + "-" + std::to_string(std::random_device{}()));
	fs::rename(dir, trash, ec);
#if !defined(_WIN32)
	::close(fd);
#endif
	if (ec) {
		why = ec.message();
		return false;
	}
	fs::remove_all(trash, ec);
	return true;
}

void PackageCache::clearCompiled() {
	std::error_code ec;
	for (auto &e : fs::directory_iterator(Paths::compiled(), ec)) fs::remove_all(e.path(), ec);
}

void PackageCache::dropArchives() {
	static std::once_flag once;
	std::call_once(once, [] {
		std::error_code ec;
		for (auto &e : fs::directory_iterator(Paths::packages(), ec)) {
			if (!isHexSha(e.path().filename().string())) continue;
			fs::remove(e.path() / "bundle.wclap.tar.gz", ec); // the extracted bundle is what's used; the archive can be re-downloaded by its hash
		}
		// leftovers of interrupted deletes
		for (auto &e : fs::directory_iterator(Paths::packages(), ec))
			if (e.path().filename().string().rfind(".trash-", 0) == 0) fs::remove_all(e.path(), ec);
	});
}

std::string PackageCache::writeCompileCacheConfig() {
	std::error_code ec;
	fs::create_directories(Paths::compiled(), ec);
	// TOML basic string: escape backslashes (Windows paths) and quotes
	std::string dir;
	for (char c : fs::absolute(Paths::compiled(), ec).string()) {
		if (c == '\\' || c == '"') dir += '\\';
		dir += c;
	}
	auto path = Paths::root() / "compiled-cache.toml";
	std::ofstream f(path);
	f << "# Wasmtime's compile cache for Plugin Circuit (written on every start)\n"
	     "[cache]\n"
	     "directory = \"" << dir << "\"\n"
	     "files-total-size-soft-limit = \"1Gi\"\n";
	return path.string();
}

PackageCache::InUse::InUse(const std::string &sha256) {
#if !defined(_WIN32)
	if (!isHexSha(sha256)) return;
	const auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return; // O_CREAT must not recreate the folder of a deleted package
	const auto path = dir / ".lock";
	fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd_ < 0) return;
	// Blocking: an exclusive holder (remove(), list()'s probe) keeps it only for a moment, and this runs on the loader.
	int r;
	do r = ::flock(fd_, LOCK_SH);
	while (r != 0 && errno == EINTR);
	// A delete may have won: it renames the folder away while holding the exclusive lock. Then the locked file is no
	// longer <package>/.lock (the inode differs or the path is gone): not held.
	struct stat locked {}, current {};
	if (r != 0 || ::fstat(fd_, &locked) != 0 || ::stat(path.c_str(), &current) != 0 || locked.st_ino != current.st_ino ||
	    locked.st_dev != current.st_dev) {
		::close(fd_);
		fd_ = -1;
	}
#else
	(void)sha256;
	fd_ = 0; // TODO(windows): LockFileEx; until then a package counts as held and not in use
#endif
}

PackageCache::InUse::~InUse() {
#if !defined(_WIN32)
	if (fd_ >= 0) ::close(fd_);
#endif
}

PackageCache::InUse &PackageCache::InUse::operator=(InUse &&o) noexcept {
	if (this != &o) {
#if !defined(_WIN32)
		if (fd_ >= 0) ::close(fd_);
#endif
		fd_ = o.fd_;
		o.fd_ = -1;
	}
	return *this;
}

} // namespace plugincircuit
