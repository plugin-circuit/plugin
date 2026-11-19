// Errors (docs/PLAN.md 5.2): the lock-free ring for the bridge's diagnostics (connected once the bridge has a log
// API); a WCLAP that traps leaves its module with an error.
//   plugincircuit-plugin-errors-test <test-param-events.wclap.tar.gz>
#include "core/BridgeLog.h"
#include "core/Catalog.h"
#include "core/InnerHost.h"
#include "core/Util.h"
#include "core/WclapLibrary.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace plugincircuit;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

struct Events { // one note-on, as the inner plugin's input
	clap_event_note note{};
	clap_input_events list{this, [](const clap_input_events *l) -> uint32_t { return static_cast<const Events *>(l->ctx)->n; },
	                       [](const clap_input_events *l, uint32_t) -> const clap_event_header * { return &static_cast<const Events *>(l->ctx)->note.header; }};
	uint32_t n = 0;
};

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <test-param-events.wclap.tar.gz>\n", argv[0]);
		return 2;
	}

	// ---- the ring: in order, the oldest overwritten, long messages cut, writers on several threads at once
	{
		const auto start = bridgelog::end();
		bridgelog::writeForTest("first");
		bridgelog::writeForTest("second");
		std::string m;
		CHECK(bridgelog::read(start, m) && m == "first");
		CHECK(bridgelog::read(start + 1, m) && m == "second");
		CHECK(!bridgelog::read(start + 2, m)); // not written yet
		for (int i = 0; i < 70; ++i) bridgelog::writeForTest(("n" + std::to_string(i)).c_str());
		CHECK(!bridgelog::read(start, m));                                    // overwritten
		CHECK(bridgelog::read(bridgelog::end() - 1, m) && m == "n69");
		bridgelog::writeForTest(std::string(2000, 'x').c_str());
		CHECK(bridgelog::read(bridgelog::end() - 1, m) && m.size() == 479);
		std::vector<std::thread> writers;
		std::atomic<bool> bad{false};
		for (int t = 0; t < 4; ++t)
			writers.emplace_back([t] {
				const std::string text(100, char('a' + t));
				for (int i = 0; i < 2000; ++i) bridgelog::writeForTest(text.c_str());
			});
		std::thread reader([&] { // what it reads is whole: one letter, 100 of them
			for (int i = 0; i < 20000; ++i) {
				std::string r;
				if (bridgelog::read(bridgelog::end() - 1, r) && r.size() != 0 && (r.size() != 100 || r.find_first_not_of(r[0]) != std::string::npos))
					bad = true;
			}
		});
		for (auto &w : writers) w.join();
		reader.join();
		CHECK(!bad);
	}

	// ---- a WCLAP that traps
	const auto home = fs::temp_directory_path() / ("plugincircuit-errors-" + std::to_string(std::random_device{}()));
	std::error_code ec;
	fs::create_directories(home, ec);
	if (ec) return 1;
#if defined(_WIN32)
	_putenv_s("PLUGINCIRCUIT_HOME", home.string().c_str());
#else
	setenv("PLUGINCIRCUIT_HOME", home.c_str(), 1);
#endif
	std::ifstream f(argv[1], std::ios::binary);
	std::vector<uint8_t> archive((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::string sha, error;
	const auto bundle = Catalog::installArchive(archive, sha, error);
	CHECK(!bundle.empty());
	if (bundle.empty()) return 1;
	auto module = WclapLibrary::open(bundle.string(), (home / "data").string(), error);
	CHECK(module != nullptr);
	if (!module) return 1;
	CHECK(module->error().empty());
	auto inner = InnerPlugin::create(module, "studio.plugincircuit.test.param-events", {}, error);
	CHECK(inner != nullptr);
	if (!inner) return 1;
	CHECK(inner->activate(48000, 1, 256));
	CHECK(inner->startProcessing());

	const auto cursor = bridgelog::end();
	std::vector<float> l(256), r(256);
	float *outs[2] = {l.data(), r.data()};
	clap_output_events none{nullptr, [](const clap_output_events *, const clap_event_header *) { return true; }};
	Events in;
	in.note.header = {sizeof(clap_event_note), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
	in.note.note_id = -1;
	in.note.key = 62;
	in.note.velocity = 1;
	in.n = 1;
	const auto status = inner->process(nullptr, 0, outs, 2, 256, 0, nullptr, &in.list, &none);
	std::printf("process after the trap: status %d\n", int(status));

	const std::string moduleError = module->error();
	std::printf("module error: %s\n", moduleError.c_str());
	CHECK(!moduleError.empty());
	(void)cursor; // the trap's own message reaches bridgelog once the bridge has a log API (BridgeLog.h)

	inner.reset();
	module.reset();
	std::printf(failures ? "%d failure(s)\n" : "PASS: plugin errors\n", failures);
	return failures ? 1 : 0;
}
