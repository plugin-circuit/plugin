#include "core/Settings.h"

#include "api/SettingsSchema.h"
#include "core/Util.h"

#include <choc/text/choc_JSON.h>
#include <wclap-bridge.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

namespace plugincircuit {

namespace {
fs::path file() { return Paths::root() / "settings.json"; }

struct Stamp { // what tells a file changed: another process's write replaces it (a rename)
	fs::file_time_type mtime{};
	uintmax_t size = 0;
	bool exists = false;
	bool operator==(const Stamp &o) const { return mtime == o.mtime && size == o.size && exists == o.exists; }
};
Stamp stampOf(const fs::path &p) {
	std::error_code ec;
	Stamp s;
	s.mtime = fs::last_write_time(p, ec);
	if (ec) return {};
	s.size = fs::file_size(p, ec);
	s.exists = !ec;
	return s;
}

// the whole file: the machine keys and any it doesn't know (a newer version's), kept when saving
choc::value::Value readFile() {
	std::ifstream f(file());
	if (!f) return choc::value::createObject("Settings");
	std::stringstream ss;
	ss << f.rdbuf();
	try {
		auto v = choc::json::parse(ss.str());
		if (v.isObject()) return v;
	} catch (...) {
	}
	return choc::value::createObject("Settings");
}

std::mutex mutex;
bool loaded = false;
choc::value::Value current; // sanitized machine values
Stamp seen;                 // the file as last read or written
std::atomic<uint64_t> versionCounter{1};
std::chrono::steady_clock::time_point lastPoll{};

void loadLocked() {
	seen = stampOf(file());
	current = settings::sanitize(Scope::Machine, readFile());
	loaded = true;
}
void applyLocked() {
	wclap_set_audio_wait_ratio(float(current["audioWaitRatio"].getWithDefault<double>(0.2)));
	wclap_set_conceal_dropouts(current["concealDropouts"].getWithDefault<bool>(true));
}
} // namespace

choc::value::Value Settings::values() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!loaded) loadLocked();
	return current;
}

std::string Settings::set(const choc::value::ValueView &change) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!loaded) loadLocked();
	auto next = current;
	if (auto e = settings::apply(Scope::Machine, change, next); !e.empty()) return e;
	auto whole = readFile();
	for (uint32_t i = 0; i < next.size(); ++i) whole.setMember(next.getObjectMemberAt(i).name, next.getObjectMemberAt(i).value);
	std::error_code ec;
	fs::create_directories(Paths::root(), ec);
	auto tmp = file();
	tmp += ".tmp";
	{
		std::ofstream f(tmp);
		f << choc::json::toString(whole, true);
		if (!f) return "settings.json couldn't be written";
	}
	fs::rename(tmp, file(), ec); // atomic: other processes read it
	if (ec) return "settings.json couldn't be written: " + ec.message();
	seen = stampOf(file());
	current = next;
	applyLocked();
	++versionCounter;
	return {};
}

uint64_t Settings::version() { return versionCounter.load(); }

void Settings::poll(double minInterval) {
	std::lock_guard<std::mutex> lock(mutex);
	const auto now = std::chrono::steady_clock::now();
	if (now - lastPoll < std::chrono::duration<double>(minInterval)) return;
	lastPoll = now;
	if (!loaded) {
		loadLocked();
		return;
	}
	if (stampOf(file()) == seen) return;
	auto before = choc::json::toString(current);
	loadLocked();
	if (choc::json::toString(current) == before) return;
	applyLocked();
	++versionCounter;
}

void Settings::apply() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!loaded) loadLocked();
	applyLocked();
}

} // namespace plugincircuit
