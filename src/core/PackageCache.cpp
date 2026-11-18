#include "core/PackageCache.h"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
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

// <package>/.lock: loaders hold a shared lock while a package is loaded (InUse); a delete or a listing takes the
// exclusive one for a moment. flock on POSIX, LockFileEx on Windows (one byte), released when the file is closed.
constexpr intptr_t kNoLock = -1;
#if defined(_WIN32)
intptr_t openLockFile(const fs::path &path) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	return h == INVALID_HANDLE_VALUE ? kNoLock : (intptr_t)h;
}
bool lockFile(intptr_t h, bool exclusive, bool wait) {
	OVERLAPPED ov{};
	const DWORD flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
	return LockFileEx((HANDLE)h, flags, 0, 1, 0, &ov) != 0;
}
void closeLockFile(intptr_t h) { CloseHandle((HANDLE)h); }
// the locked file is still <package>/.lock (a delete moves the folder away): same volume and file index
bool stillAt(intptr_t h, const fs::path &path) {
	BY_HANDLE_FILE_INFORMATION locked{}, current{};
	if (!GetFileInformationByHandle((HANDLE)h, &locked)) return false;
	HANDLE c = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (c == INVALID_HANDLE_VALUE) return false;
	const bool ok = GetFileInformationByHandle(c, &current) != 0;
	CloseHandle(c);
	return ok && locked.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
	       locked.nFileIndexHigh == current.nFileIndexHigh && locked.nFileIndexLow == current.nFileIndexLow;
}
#else
intptr_t openLockFile(const fs::path &path) {
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	return fd < 0 ? kNoLock : fd;
}
bool lockFile(intptr_t fd, bool exclusive, bool wait) {
	int r;
	do r = ::flock(int(fd), (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB));
	while (r != 0 && errno == EINTR);
	return r == 0;
}
void closeLockFile(intptr_t fd) { ::close(int(fd)); }
bool stillAt(intptr_t fd, const fs::path &path) {
	struct stat locked {}, current {};
	return ::fstat(int(fd), &locked) == 0 && ::stat(path.c_str(), &current) == 0 && locked.st_ino == current.st_ino &&
	       locked.st_dev == current.st_dev;
}
#endif

// Try the exclusive lock: held if nobody has the package loaded (the caller then closes the returned lock).
intptr_t tryExclusive(const fs::path &dir) {
	const intptr_t h = openLockFile(dir / ".lock");
	if (h == kNoLock) return kNoLock;
	if (!lockFile(h, true, false)) {
		closeLockFile(h);
		return kNoLock;
	}
	return h;
}

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
		if (const intptr_t lock = tryExclusive(e.path()); lock != kNoLock) closeLockFile(lock); // got it: nobody has it loaded
		else p.inUse = true;
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
	const intptr_t lock = tryExclusive(dir);
	if (lock == kNoLock) {
		why = "in use";
		return false;
	}
	// Move it out of the way first (atomic), so a load never sees a half-deleted package, then delete.
	auto trash = Paths::packages() / (".trash-" + sha256.substr(0, 12) + "-" + std::to_string(std::random_device{}()));
#if defined(_WIN32)
	// Windows may refuse to move a folder with an open file in it: release first. A loader that takes the shared lock
	// in between finds afterwards that its .lock file moved away (InUse checks the file identity) and isn't held.
	closeLockFile(lock);
	fs::rename(dir, trash, ec);
#else
	fs::rename(dir, trash, ec);
	closeLockFile(lock);
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
	if (!isHexSha(sha256)) return;
	const auto dir = Paths::packages() / sha256;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return; // opening .lock must not recreate the folder of a deleted package
	const auto path = dir / ".lock";
	fd_ = openLockFile(path);
	if (fd_ == kNoLock) return;
	// Blocking: an exclusive holder (remove(), list()'s probe) keeps it only for a moment, and this runs on the loader.
	// A delete may have won: it moves the folder away. Then the locked file is no longer <package>/.lock (another
	// file, or the path is gone): not held.
	if (!lockFile(fd_, false, true) || !stillAt(fd_, path)) {
		closeLockFile(fd_);
		fd_ = kNoLock;
	}
}

PackageCache::InUse::~InUse() {
	if (fd_ != kNoLock) closeLockFile(fd_);
}

PackageCache::InUse &PackageCache::InUse::operator=(InUse &&o) noexcept {
	if (this != &o) {
		if (fd_ != kNoLock) closeLockFile(fd_);
		fd_ = o.fd_;
		o.fd_ = -1;
	}
	return *this;
}

} // namespace plugincircuit
