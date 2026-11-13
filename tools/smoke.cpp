// plugincircuit-smoke <bundle.wclap.tar.gz | bundle-dir> [--blocks N] [--json]
// Loads every plugin in a WCLAP through wclap-bridge + InnerPlugin, processes audio
// (notes for instruments, noise for effects), round-trips state, and reports one line per plugin.
#include "core/Catalog.h"
#include "core/InnerHost.h"
#include "core/WclapLibrary.h"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

using namespace plugincircuit;

namespace {

thread_local bool tlsAudio = false;
std::thread::id mainId;

// Minimal sorted event list (input) and sink (output)
struct EventList {
	std::vector<std::vector<uint8_t>> events;
	clap_input_events in{this, &EventList::size, &EventList::get};
	static uint32_t size(const clap_input_events *l) { return (uint32_t)static_cast<const EventList *>(l->ctx)->events.size(); }
	static const clap_event_header *get(const clap_input_events *l, uint32_t i) {
		return reinterpret_cast<const clap_event_header *>(static_cast<const EventList *>(l->ctx)->events[i].data());
	}
	template <class T> void push(const T &e) {
		std::vector<uint8_t> b(sizeof(T));
		std::memcpy(b.data(), &e, sizeof(T));
		events.push_back(std::move(b));
	}
};
struct EventSink {
	uint32_t count = 0;
	clap_output_events out{this, &EventSink::tryPush};
	static bool tryPush(const clap_output_events *l, const clap_event_header *) {
		static_cast<EventSink *>(l->ctx)->count++;
		return true;
	}
};

clap_event_note note(uint16_t type, uint32_t time, int16_t key, double velocity) {
	clap_event_note e{};
	e.header = {sizeof(clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
	e.note_id = -1;
	e.port_index = 0;
	e.channel = 0;
	e.key = key;
	e.velocity = velocity;
	return e;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <bundle.wclap.tar.gz|dir> [--blocks N] [--json]\n", argv[0]);
		return 2;
	}
	mainId = std::this_thread::get_id();
	std::string input = argv[1];
	int blocks = 400;
	bool json = false;
	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--blocks" && i + 1 < argc) blocks = std::atoi(argv[++i]);
		if (a == "--json") json = true;
	}

	std::string error;
	fs::path dir = input;
	if (fs::is_regular_file(input)) {
		std::ifstream f(input, std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
		std::string sha;
		dir = Catalog::installArchive(bytes, sha, error);
		if (dir.empty()) {
			std::printf("{\"bundle\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", input.c_str(), error.c_str());
			return 1;
		}
	}

	auto t0 = std::chrono::steady_clock::now();
	auto module = WclapLibrary::open(dir.string(), (Paths::root() / "smoke-data").string(), error);
	double openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (!module) {
		std::printf("{\"bundle\":\"%s\",\"ok\":false,\"stage\":\"open\",\"error\":%s}\n", input.c_str(),
		            choc::json::getEscapedQuotedString(error).c_str());
		return 1;
	}

	int failures = 0;
	for (auto &desc : module->descriptors()) {
		bool isInstrument = false;
		for (auto &f : desc.features)
			if (f == CLAP_PLUGIN_FEATURE_INSTRUMENT) isInstrument = true;

		bool callbackPending = false;
		InnerHostCallbacks cb;
		cb.isMainThread = [] { return std::this_thread::get_id() == mainId && !tlsAudio; };
		cb.isAudioThread = [] { return tlsAudio; };
		cb.requestCallback = [&] { callbackPending = true; };
		cb.log = [](clap_log_severity s, const char *m) {
			if (s >= CLAP_LOG_WARNING) std::fprintf(stderr, "  [log %d] %s\n", (int)s, m);
		};

		auto r = choc::value::createObject("Result", "bundle", input, "plugin", desc.id, "name", desc.name,
		                                   "instrument", isInstrument, "openMs", openMs);
		auto fail = [&](const std::string &stage, const std::string &msg) {
			r.addMember("ok", false);
			r.addMember("stage", stage);
			r.addMember("error", msg);
			failures++;
		};

		auto t1 = std::chrono::steady_clock::now();
		auto inner = InnerPlugin::create(module, desc.id, cb, error);
		r.addMember("createMs", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
		if (!inner) {
			fail("create", error);
			std::cout << choc::json::toString(r) << std::endl;
			continue;
		}
		struct WebviewExt {
			int32_t (*get_uri)(const clap_plugin *, char *, uint32_t);
			bool (*get_resource)(const clap_plugin *, const char *, char *, uint32_t, const clap_ostream *);
		};
		if (auto *wv = (const WebviewExt *)inner->plugin()->get_extension(inner->plugin(), "clap.webview/3")) {
			char uri[2048] = {0};
			auto n = wv->get_uri(inner->plugin(), uri, sizeof(uri));
			r.addMember("webviewUri", std::string(n > 0 ? uri : "(error)"));
			if (n > 0 && uri[0] == '/') {
				std::vector<uint8_t> res;
				clap_ostream os{&res, [](const clap_ostream *s, const void *b, uint64_t len) -> int64_t {
					auto *v = (std::vector<uint8_t> *)s->ctx;
					v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + len);
					return (int64_t)len;
				}};
				char mime[256] = {0};
				bool got = wv->get_resource(inner->plugin(), uri, mime, sizeof(mime), &os);
				r.addMember("webviewResource", std::string(got ? "ok " : "FAILED ") + mime + " " + std::to_string(res.size()));
			}
		}
		r.addMember("params", (int)inner->params().size());
		r.addMember("hasGui", inner->gui() != nullptr);
		r.addMember("mainIn", (int)inner->mainInputChannels());
		r.addMember("mainOut", (int)inner->mainOutputChannels());
		r.addMember("noteIn", inner->hasNoteInput());
		r.addMember("mpe", inner->supportsMpe());

		const double sr = 48000;
		const uint32_t frames = 256;
		if (!inner->activate(sr, 1, frames)) {
			fail("activate", "activate returned false");
			std::cout << choc::json::toString(r) << std::endl;
			continue;
		}

		std::vector<float> inL(frames), inR(frames), outL(frames), outR(frames);
		const float *ins[2] = {inL.data(), inR.data()};
		float *outs[2] = {outL.data(), outR.data()};
		uint32_t rng = 1234;
		double sumSq = 0, peak = 0, worstBlockMs = 0, totalMs = 0;
		bool nan = false;
		uint32_t outEvents = 0;

		tlsAudio = true;
		bool started = inner->startProcessing();
		tlsAudio = false;
		if (!started) {
			fail("start_processing", "returned false");
		} else {
			for (int b = 0; b < blocks; ++b) {
				for (uint32_t i = 0; i < frames; ++i) {
					rng = rng * 1664525u + 1013904223u;
					float noise = isInstrument ? 0.0f : ((rng >> 9) * (1.0f / 8388608.0f) - 1.0f) * 0.25f;
					inL[i] = inR[i] = noise;
				}
				EventList events;
				EventSink sink;
				if (b % 100 == 0) {
					events.push(note(CLAP_EVENT_NOTE_ON, 0, 60, 0.8));
					events.push(note(CLAP_EVENT_NOTE_ON, 0, 64, 0.8));
					events.push(note(CLAP_EVENT_NOTE_ON, 0, 67, 0.8));
				}
				if (b % 100 == 60) {
					events.push(note(CLAP_EVENT_NOTE_OFF, 0, 60, 0.0));
					events.push(note(CLAP_EVENT_NOTE_OFF, 0, 64, 0.0));
					events.push(note(CLAP_EVENT_NOTE_OFF, 0, 67, 0.0));
				}
				tlsAudio = true;
				auto tb = std::chrono::steady_clock::now();
				inner->process(ins, 2, outs, 2, frames, (int64_t)b * frames, nullptr, &events.in, &sink.out);
				double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count();
				tlsAudio = false;
				totalMs += ms;
				worstBlockMs = std::max(worstBlockMs, ms);
				outEvents += sink.count;
				for (uint32_t i = 0; i < frames; ++i) {
					for (float v : {outL[i], outR[i]}) {
						if (!std::isfinite(v)) nan = true;
						else {
							sumSq += double(v) * v;
							peak = std::max(peak, (double)std::fabs(v));
						}
					}
				}
				if (callbackPending) {
					callbackPending = false;
					inner->onMainThread();
				}
			}
			// Latency probe: silence (all notes off) until tails decay, then an impulse (effects) or a note-on
			// (instruments) at sample `kAt` of a block. Measures samples until the output first responds, compared
			// with what the plugin reports. Block-quantised event handling or hidden buffering shows up as a delay.
			{
				constexpr uint32_t kAt = 100;
				const uint32_t silentBlocks = uint32_t(4.0 * sr / frames);
				const uint32_t probeBlocks = uint32_t(1.0 * sr / frames);
				auto run = [&](uint32_t b, bool trigger, EventList &events) {
					std::fill(inL.begin(), inL.end(), 0.0f);
					std::fill(inR.begin(), inR.end(), 0.0f);
					if (trigger && !isInstrument) inL[kAt] = inR[kAt] = 1.0f;
					EventSink sink;
					tlsAudio = true;
					inner->process(ins, 2, outs, 2, frames, (int64_t)(blocks + b) * frames, nullptr, &events.in, &sink.out);
					tlsAudio = false;
					if (callbackPending) {
						callbackPending = false;
						inner->onMainThread();
					}
				};
				for (uint32_t b = 0; b < silentBlocks; ++b) {
					EventList events;
					if (b == 0)
						for (int k : {60, 64, 67}) events.push(note(CLAP_EVENT_NOTE_OFF, 0, k, 0.0));
					run(b, false, events);
				}
				// noise floor of the decayed tail, so a reverb's leftover doesn't count as a response
				float floor = 0;
				for (uint32_t i = 0; i < frames; ++i) floor = std::max({floor, std::fabs(outL[i]), std::fabs(outR[i])});
				const float threshold = std::max(1e-4f, floor * 4);
				int64_t delay = -1;
				for (uint32_t b = 0; b < probeBlocks && delay < 0; ++b) {
					EventList events;
					if (b == 0 && isInstrument) events.push(note(CLAP_EVENT_NOTE_ON, kAt, 60, 1.0));
					run(silentBlocks + b, b == 0, events);
					for (uint32_t i = (b == 0 ? kAt : 0); i < frames && delay < 0; ++i)
						if (std::fabs(outL[i]) > threshold || std::fabs(outR[i]) > threshold)
							delay = int64_t(b) * frames + i - kAt;
				}
				r.addMember("reportedLatency", (int64_t)inner->latency());
				r.addMember("measuredDelay", delay); // -1: no response within 1 s
			}
			tlsAudio = true;
			inner->stopProcessing();
			tlsAudio = false;
		}
		double blockMs = frames / sr * 1000.0;
		double rms = std::sqrt(sumSq / std::max(1.0, double(blocks) * frames * 2));
		r.addMember("rmsDb", rms > 0 ? 20 * std::log10(rms) : -999.0);
		r.addMember("peak", peak);
		r.addMember("nan", nan);
		r.addMember("cpuPercent", 100.0 * (totalMs / std::max(1, blocks)) / blockMs);
		r.addMember("worstBlockPercent", 100.0 * worstBlockMs / blockMs);
		r.addMember("outEvents", (int)outEvents);

		// state round trip
		std::vector<uint8_t> state;
		bool saved = inner->saveState(state);
		bool loaded = saved ? inner->loadState(state) : false;
		std::vector<uint8_t> state2;
		if (loaded) inner->saveState(state2);
		r.addMember("stateBytes", (int)state.size());
		r.addMember("stateRoundTrip", saved && loaded);
		r.addMember("stateStable", saved && loaded && state == state2);

		inner->deactivate();
		inner.reset();
		if (!r.hasObjectMember("ok")) {
			bool silent = rms < 1e-6;
			r.addMember("ok", !nan);
			r.addMember("silent", silent);
		}
		std::cout << choc::json::toString(r) << std::endl;
	}
	(void)json;
	return failures ? 1 : 0;
}
