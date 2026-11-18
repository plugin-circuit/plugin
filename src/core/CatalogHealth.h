#pragma once
// Is the catalog reachable? One check per process, on its own thread, only while some editor shows (want()):
// every 30 s while online, backed off 2, 4, 8 ... 60 s while offline; checkNow() asks at once. The `app` store
// (docs/PLAN.md 5.1) reads it; the page no longer fetches /v1/health itself. Never the audio thread.
#include <cstdint>
#include <string>

namespace plugincircuit {

class CatalogHealth {
public:
	struct Status {
		int online = -1; // -1 not checked yet, 0 offline, 1 online
		std::string error, apiBase;
	};
	static Status status();
	static uint64_t version(); // changes when status() does
	static void want(bool on); // an editor opened (true) or closed (false): checks run while any is open
	static void checkNow();
	static void shutdown(); // stops the thread (the entry's deinit)
	// for tests: the intervals, in seconds
	static void setIntervals(double online, double offlineFirst, double offlineMax);
};

} // namespace plugincircuit
