#include "core/CatalogHealth.h"

#include "core/Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace plugincircuit {

namespace {
std::mutex mutex;
std::condition_variable wake;
std::thread worker;
bool stopping = false, now = false, running = false;
int wanted = 0;
CatalogHealth::Status current;
std::atomic<uint64_t> versionCounter{1};
double onlineInterval = 30, offlineFirst = 2, offlineMax = 60;

void run() { // exits when no editor wants it; want(true) starts it again
	double backoff = offlineFirst;
	std::unique_lock<std::mutex> lock(mutex);
	while (!stopping && wanted > 0) {
		now = false;
		const std::string base = Paths::apiBase();
		lock.unlock();
		auto r = httpGetString(base + "/v1/health", 4);
		lock.lock();
		CatalogHealth::Status next;
		next.apiBase = base;
		next.online = r.ok() ? 1 : 0;
		if (!r.ok()) next.error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
		if (next.online != current.online || next.error != current.error || next.apiBase != current.apiBase) {
			current = next;
			++versionCounter;
		}
		const double wait = current.online == 1 ? onlineInterval : backoff;
		backoff = current.online == 1 ? offlineFirst : std::min(backoff * 2, offlineMax);
		wake.wait_for(lock, std::chrono::duration<double>(wait), [] { return stopping || now || wanted == 0; });
	}
	running = false;
}
} // namespace

CatalogHealth::Status CatalogHealth::status() {
	std::lock_guard<std::mutex> lock(mutex);
	auto s = current;
	if (s.apiBase.empty()) s.apiBase = Paths::apiBase();
	return s;
}

uint64_t CatalogHealth::version() { return versionCounter.load(); }

void CatalogHealth::want(bool on) {
	std::unique_lock<std::mutex> lock(mutex);
	wanted = std::max(0, wanted + (on ? 1 : -1));
	if (on && !running && !stopping) {
		if (worker.joinable()) { // finished (or finishing its last check)
			lock.unlock();
			worker.join();
			lock.lock();
		}
		if (!running && !worker.joinable()) {
			running = true;
			worker = std::thread(run);
		}
	}
	if (on) now = true; // a newly opened editor gets a fresh answer
	wake.notify_all();
}

void CatalogHealth::checkNow() {
	std::lock_guard<std::mutex> lock(mutex);
	now = true;
	wake.notify_all();
}

void CatalogHealth::shutdown() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
		wake.notify_all();
	}
	if (worker.joinable()) worker.join();
	std::lock_guard<std::mutex> lock(mutex);
	stopping = false; // a host may init the entry again
}

// the thread must not outlive the library: joined at exit should the host never call the entry's deinit
struct JoinAtExit {
	~JoinAtExit() { CatalogHealth::shutdown(); }
} joinAtExit;

void CatalogHealth::setIntervals(double online, double first, double max) {
	std::lock_guard<std::mutex> lock(mutex);
	onlineInterval = online, offlineFirst = first, offlineMax = max;
}

} // namespace plugincircuit
