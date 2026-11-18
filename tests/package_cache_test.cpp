// The package in-use lock (core/PackageCache): a loaded package can't be deleted, a listing shows it in use, and a
// lock taken after a delete isn't held. flock on POSIX, LockFileEx on Windows; this runs on both in CI.
#include "core/PackageCache.h"
#include "core/Util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace plugincircuit;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

static bool listedInUse(const std::string &sha) {
	for (auto &p : PackageCache::list())
		if (p.sha256 == sha) return p.inUse;
	return false;
}

int main() {
	const auto home = fs::temp_directory_path() / ("plugincircuit-package-cache-" + std::to_string(std::rand()));
	fs::remove_all(home);
	fs::create_directories(home);
#if defined(_WIN32)
	_putenv_s("PLUGINCIRCUIT_HOME", home.string().c_str());
#else
	setenv("PLUGINCIRCUIT_HOME", home.c_str(), 1);
#endif
	// a package as the cache keeps it: <packages>/<sha256>/bundle/<name>.wclap/module.wasm
	const std::string sha(64, 'a');
	const auto bundle = Paths::packages() / sha / "bundle" / "test.wclap";
	fs::create_directories(bundle);
	std::ofstream(bundle / "module.wasm") << "not really wasm";

	{
		PackageCache::InUse held(sha);
		CHECK(held.held());
		CHECK(listedInUse(sha));
		std::string why;
		CHECK(!PackageCache::remove(sha, why));
		CHECK(why == "in use");
		CHECK(fs::exists(Paths::packages() / sha));
		PackageCache::InUse second(sha); // shared: a second instance of the same plugin
		CHECK(second.held());
	}
	CHECK(!listedInUse(sha));
	std::string why;
	CHECK(PackageCache::remove(sha, why));
	CHECK(!fs::exists(Paths::packages() / sha));
	PackageCache::InUse after(sha); // deleted: not held, and the folder isn't recreated
	CHECK(!after.held());
	CHECK(!fs::exists(Paths::packages() / sha));
	PackageCache::InUse bad("not-a-sha");
	CHECK(!bad.held());

	fs::remove_all(home);
	std::printf(failures ? "%d failure(s)\n" : "PASS: package cache lock\n", failures);
	return failures ? 1 : 0;
}
