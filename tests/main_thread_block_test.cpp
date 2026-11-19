// Does main-thread work inside a WCLAP stall the audio thread?
//
// Runs process() on its own thread like an audio callback (paced in real time, notes playing), while the main thread
// works inside the same plugin:
//   export  N webview messages (default {"t":"render"}: Spiral303's Export WAV, an offline render inside receive())
//   busy    3 s of short calls, as the UI did before the parameter table: value_to_text for every parameter, over
//           and over
//   state   N state loads (save once, then load that state again every 100 ms): a preset or project load
//   table   3 s of the UI's traffic since (docs/PLAN.md phase 6, rates measured in the editor): on each 50 ms tick
//           the safety net's get_value (64 a second, round robin), and once a second texts for 40 rows (a scroll)
// A single-threaded WCLAP (no shared memory) shares one instance between both threads. The upstream bridge made the
// audio thread wait for the main thread; ours spins for at most <ratio> of the block, then skips the block (to compare
// with blocking, build against a bridge checkout without that commit).
//
// Usage: plugincircuit-main-thread-block-test <bundle-dir> <pluginId> export|busy|table|state [--ratio R] [--count N]
//                                             [--message JSON] [--frames F]
// Reports the worst process() time, blocks slower than real time (a device underrun: what you hear as a click or
// gap) and blocks the bridge skipped. Passes if no block was slower than real time.
#include "core/InnerHost.h"
#include "core/Util.h"
#include "core/WclapLibrary.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <random>
#include <thread>
#include <vector>

#include <wclap-bridge.h> // our fork's audio-wait API (wclap_set_audio_wait_ratio etc.)

#include <cstdlib>

using namespace plugincircuit;
using clock_type = std::chrono::steady_clock;

namespace {
constexpr const char kExtWebview[] = "clap.webview/3";
struct PluginWebview {
	int32_t(CLAP_ABI *get_uri)(const clap_plugin_t *, char *, uint32_t);
	bool(CLAP_ABI *get_resource)(const clap_plugin_t *, const char *, char *, uint32_t, const clap_ostream_t *);
	bool(CLAP_ABI *receive)(const clap_plugin_t *, const void *, uint32_t);
};

thread_local bool tlsAudio = false;
std::thread::id mainId;

struct Events {
	std::vector<clap_event_note> notes;
	clap_input_events in{this, [](const clap_input_events *l) { return uint32_t(static_cast<const Events *>(l->ctx)->notes.size()); },
	                     [](const clap_input_events *l, uint32_t i) -> const clap_event_header * {
		                     return &static_cast<const Events *>(l->ctx)->notes[i].header;
	                     }};
	clap_output_events out{nullptr, [](const clap_output_events *, const clap_event_header *) { return true; }};
};
clap_event_note note(uint16_t type, int16_t key) {
	clap_event_note e{};
	e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
	e.note_id = -1;
	e.key = key;
	e.velocity = 0.8;
	return e;
}
} // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: %s <bundle-dir> <pluginId> export|busy|table|state [--ratio R] [--count N] [--message JSON] [--frames F]\n", argv[0]);
		return 2;
	}
	mainId = std::this_thread::get_id();
	const std::string bundle = argv[1], pluginId = argv[2], scenario = argv[3];
	double ratio = -1;
	int count = 10;
	uint32_t frames = 128;
	std::string message = R"({"t":"render"})";
	int conceal = -1;
	std::string wavPath;
	bool trace = false;
	for (int i = 4; i + 1 < argc; i += 2) {
		const std::string a = argv[i];
		if (a == "--ratio") ratio = std::atof(argv[i + 1]);
		else if (a == "--count") count = std::atoi(argv[i + 1]);
		else if (a == "--message") message = argv[i + 1];
		else if (a == "--frames") frames = uint32_t(std::atoi(argv[i + 1]));
		else if (a == "--conceal") conceal = std::atoi(argv[i + 1]);
		else if (a == "--wav") wavPath = argv[i + 1];
		else if (a == "--trace") trace = std::atoi(argv[i + 1]) != 0;
	}
	const bool hasWaitApi = true;
	// a scratch Plugin Circuit folder: opening the bridge writes its compile-cache config and cleans the package cache
	// there, and applies settings.json (which must not override this tool's arguments)
	const auto home = fs::temp_directory_path() / ("plugincircuit-block-test-" + std::to_string(std::random_device{}()));
	std::error_code ec;
	fs::create_directories(home, ec);
	if (ec) {
		std::fprintf(stderr, "can't create %s\n", home.string().c_str());
		return 1;
	}
#if defined(_WIN32)
	_putenv_s("PLUGINCIRCUIT_HOME", home.string().c_str());
#else
	setenv("PLUGINCIRCUIT_HOME", home.c_str(), 1);
#endif

	std::string error;
	auto module = WclapLibrary::open(bundle, "", error);
	if (!module) {
		std::fprintf(stderr, "open: %s\n", error.c_str());
		return 1;
	}
	// after open: its global init applies the (scratch) settings
	if (ratio >= 0) wclap_set_audio_wait_ratio(float(ratio));
	if (conceal >= 0) wclap_set_conceal_dropouts(conceal != 0);
	InnerHostCallbacks cb;
	cb.isMainThread = [] { return std::this_thread::get_id() == mainId && !tlsAudio; };
	cb.isAudioThread = [] { return tlsAudio; };
	cb.requestCallback = [] {};
	cb.log = [](clap_log_severity, const char *) {};
	auto inner = InnerPlugin::create(module, pluginId, cb, error);
	if (!inner) {
		std::fprintf(stderr, "create: %s\n", error.c_str());
		return 1;
	}
	constexpr double sr = 48000;
	if (!inner->activate(sr, 1, frames)) {
		std::fprintf(stderr, "activate failed\n");
		return 1;
	}
	auto *webview = static_cast<const PluginWebview *>(inner->plugin()->get_extension(inner->plugin(), kExtWebview));
	const auto params = inner->params();

	std::atomic<bool> running{true};
	std::atomic<double> worstMs{0};
	std::atomic<int> blocks{0}, lateBlocks{0};
	std::vector<std::pair<int64_t, double>> late(64); // test only: (block, ms) of the first late blocks
	// the left channel and which blocks were skipped (test only: allocated up front for 30 s)
	std::vector<float> recorded(size_t(30 * 48000), 0.f);
	std::vector<uint8_t> skippedFlags(recorded.size() / 16 + 1, 0);
	const double blockMs = 1000.0 * frames / sr;
	std::thread audio([&] {
		tlsAudio = true;
		std::vector<float> l(frames), r(frames), il(frames, 0.f), ir(frames, 0.f);
		float *outs[2] = {l.data(), r.data()};
		const float *ins[2] = {il.data(), ir.data()};
		Events events;
		events.notes.reserve(4);
		inner->startProcessing();
		const auto period = std::chrono::duration<double>(frames / sr);
		const int notePeriod = std::max(1, int(0.25 * sr / frames)); // a note every 250 ms, held for half of it
		auto next = clock_type::now();
		for (int64_t b = 0; running; ++b) {
			events.notes.clear();
			if (b % notePeriod == 0) events.notes.push_back(note(CLAP_EVENT_NOTE_ON, 45 + int16_t((b / notePeriod) % 12)));
			if (b % notePeriod == notePeriod / 2) events.notes.push_back(note(CLAP_EVENT_NOTE_OFF, 45 + int16_t((b / notePeriod) % 12)));
			const auto t0 = clock_type::now();
			inner->process(ins, 2, outs, 2, frames, b * frames, nullptr, &events.in, &events.out);
			const double ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
			if (ms > worstMs) worstMs = ms;
			if (ms > blockMs) { // a real device would have underrun: a click or a gap
				if (size_t(lateBlocks) < late.size()) late[size_t(lateBlocks)] = {b, ms};
				++lateBlocks;
			}
			if (hasWaitApi) {
				static uint64_t lastSkipped = wclap_audio_skipped_blocks();
				const uint64_t now = wclap_audio_skipped_blocks();
				if (size_t(b) < skippedFlags.size()) skippedFlags[size_t(b)] = now != lastSkipped;
				lastSkipped = now;
			}
			if (size_t(b + 1) * frames <= recorded.size()) std::copy_n(l.data(), frames, recorded.data() + size_t(b) * frames);
			++blocks;
			next += std::chrono::duration_cast<clock_type::duration>(period);
			std::this_thread::sleep_until(next);
		}
		inner->stopProcessing();
	});

	const uint64_t skippedBefore = hasWaitApi ? wclap_audio_skipped_blocks() : 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	double longestCallMs = 0;
	int calls = 0;
	if (scenario == "export") {
		if (!webview) {
			std::fprintf(stderr, "plugin has no %s\n", kExtWebview);
			return 1;
		}
		for (int i = 0; i < count; ++i, ++calls) {
			const auto t0 = clock_type::now();
			webview->receive(inner->plugin(), message.data(), uint32_t(message.size()));
			longestCallMs = std::max(longestCallMs, std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	} else if (scenario == "state") {
		std::vector<uint8_t> saved;
		const auto s0 = clock_type::now();
		const bool savedOk = inner->saveState(saved);
		std::printf("save: %.1f ms\n", std::chrono::duration<double, std::milli>(clock_type::now() - s0).count());
		if (!savedOk) {
			std::fprintf(stderr, "saveState failed\n");
			return 1;
		}
		std::printf("state: %zu bytes\n", saved.size());
		for (int i = 0; i < count; ++i, ++calls) {
			const auto t0 = clock_type::now();
			if (!inner->loadState(saved)) std::fprintf(stderr, "loadState %d failed\n", i);
			longestCallMs = std::max(longestCallMs, std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	} else if (scenario == "table") {
		const auto start = clock_type::now(), end = start + std::chrono::seconds(3);
		size_t next = 0, row = 0;
		auto call = [&](auto &&f) {
			const auto t0 = clock_type::now();
			f();
			longestCallMs = std::max(longestCallMs, std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
			++calls;
		};
		for (int tick = 0; clock_type::now() < end; ++tick) {
			for (int i = 0; i < (tick % 5 == 4 ? 4 : 3) && !params.empty(); ++i) { // 64 a second at 20 ticks
				double v;
				call([&] { (void)inner->paramValue(params[next++ % params.size()].id, v); });
			}
			if (tick % 20 == 0) // a scroll: 40 rows come into view
				for (int i = 0; i < 40 && !params.empty(); ++i, ++row)
					call([&] { (void)inner->paramText(params[row % params.size()].id, params[row % params.size()].defaultValue); });
			std::this_thread::sleep_until(start + std::chrono::milliseconds(50 * (tick + 1)));
		}
	} else { // busy: a UI formatting every parameter's text, as fast as it can, for 3 s
		const auto end = clock_type::now() + std::chrono::seconds(3);
		while (clock_type::now() < end)
			for (auto &p : params) {
				const auto t0 = clock_type::now();
				(void)inner->paramText(p.id, p.defaultValue);
				longestCallMs = std::max(longestCallMs, std::chrono::duration<double, std::milli>(clock_type::now() - t0).count());
				++calls;
			}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	running = false;
	audio.join();
	inner->deactivate();

	const uint64_t skipped = hasWaitApi ? wclap_audio_skipped_blocks() - skippedBefore : 0;

	// clicks: the largest sample step at each gap edge (8 samples around it), against the steps elsewhere
	const size_t used = std::min(recorded.size(), size_t(blocks.load()) * frames);
	std::vector<double> steps;
	double entryStep = 0, exitStep = 0; // into a gap (sound -> concealed) / out of it (replayed notes may start here)
	int edges = 0;
	for (size_t i = 1; i < used; ++i) {
		const size_t blk = i / frames, prev = (i - 1) / frames;
		const bool edge = blk != prev && blk < skippedFlags.size() && skippedFlags[blk] != skippedFlags[prev];
		if (edge) {
			++edges;
			double &into = skippedFlags[blk] ? entryStep : exitStep;
			// the step exactly at the boundary: where cutting to silence (or back) clicks. A wider window would also
			// catch the material's own jumps (a sawtooth's reset), whichever way the gap is filled.
			into = std::max(into, double(std::fabs(recorded[i] - recorded[i - 1])));
		} else steps.push_back(std::fabs(recorded[i] - recorded[i - 1]));
	}
	const double edgeStep = std::max(entryStep, exitStep);
	std::sort(steps.begin(), steps.end());
	const double typical = steps.empty() ? 0 : steps[size_t(steps.size() * 0.999)];
	if (edges) std::printf("gap edges: %d, largest step entering %.3f, leaving %.3f (all %.3f; 99.9th percentile elsewhere %.3f)\n", edges, entryStep, exitStep, edgeStep, typical);
	if (!wavPath.empty()) { // 32-bit float mono WAV, for listening
		std::ofstream f(wavPath, std::ios::binary);
		auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
		auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
		const uint32_t bytes = uint32_t(used * 4);
		f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(1); u32(uint32_t(sr)); u32(uint32_t(sr) * 4); u16(4); u16(32);
		f.write("data", 4); u32(bytes); f.write(reinterpret_cast<const char *>(recorded.data()), bytes);
	}
	if (trace)
		for (int i = 0; i < std::min(lateBlocks.load(), int(late.size())); ++i) std::printf("late block %lld: %.2f ms\n", (long long)late[size_t(i)].first, late[size_t(i)].second);
	const bool ok = lateBlocks == 0;
	std::printf("bridge: non-blocking, wait ratio %s, concealment %s\n", ratio < 0 ? "default" : std::to_string(ratio).c_str(),
	            conceal < 0 ? "default" : (conceal ? "on" : "off"));
	std::printf("main thread: %s, %d calls, longest %.1f ms\n", scenario.c_str(), calls, longestCallMs);
	std::printf("audio: %d blocks of %.2f ms, worst process %.2f ms, %d slower than real time (underruns), %llu skipped\n",
	            blocks.load(), blockMs, worstMs.load(), lateBlocks.load(), (unsigned long long)skipped);
	std::printf(ok ? "PASS\n" : "FAIL: the audio thread waited for the main thread\n");
	return ok ? 0 : 1;
}
