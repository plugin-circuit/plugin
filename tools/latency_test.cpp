// End-to-end latency check through Plugin Circuit itself, the way a DAW sees it.
// Loads PluginCircuit.clap, activates it, restores a session naming <pluginId> (+ package sha256), waits for the load,
// checks that Plugin Circuit asked for a restart (so the DAW re-reads latency), restarts, then sends an impulse (FX) or a
// note-on (instrument) at sample 100 of a block and reports samples until the output responds vs latency.get().
// Usage: plugincircuit-latency-test <path/to/PluginCircuit.clap> <fx|synth> <pluginId> <packageSha256>
#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "ClapLoad.h"
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> callbackRequested{false}, dirty{false}, restartRequested{false};

const clap_host_state hostState{[](const clap_host *) { dirty = true; }};
const clap_host_latency hostLatency{[](const clap_host *) {}};
const void *getExt(const clap_host *, const char *id) {
	if (!std::strcmp(id, CLAP_EXT_STATE)) return &hostState;
	if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &hostLatency;
	return nullptr;
}
clap_host host{CLAP_VERSION_INIT, nullptr, "latency-test", "plugincircuit", "", "0", getExt,
               [](const clap_host *) { restartRequested = true; }, [](const clap_host *) {},
               [](const clap_host *) { callbackRequested = true; }};

struct Events {
	std::vector<clap_event_note> v;
	clap_input_events list{this, [](const clap_input_events *l) { return (uint32_t)((Events *)l->ctx)->v.size(); },
	                       [](const clap_input_events *l, uint32_t i) -> const clap_event_header * { return &((Events *)l->ctx)->v[i].header; }};
};
clap_output_events sink{nullptr, [](const clap_output_events *, const clap_event_header *) { return true; }};

void putU32(std::vector<uint8_t> &b, uint32_t v) {
	for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
} // namespace

int main(int argc, char **argv) {
	if (argc < 5) {
		std::fprintf(stderr, "usage: %s <PluginCircuit.clap> <fx|synth> <pluginId> <packageSha256>\n", argv[0]);
		return 2;
	}
	const bool instrument = std::string(argv[2]) == "synth";
	auto *entry = loadClapEntry(argv[1]);
	if (!entry) return 1;
	entry->init(argv[1]);
	auto *factory = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	auto *p = factory->create_plugin(factory, &host, instrument ? "studio.plugincircuit.host.synth" : "studio.plugincircuit.host.fx");
	p->init(p);
	auto *state = (const clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
	auto *latency = (const clap_plugin_latency *)p->get_extension(p, CLAP_EXT_LATENCY);

	const double sr = 48000;
	const uint32_t frames = 256;
	std::vector<float> inL(frames), inR(frames), outL(frames), outR(frames);
	float *ins[2] = {inL.data(), inR.data()}, *outs[2] = {outL.data(), outR.data()};
	clap_audio_buffer inBuf{ins, nullptr, 2, 0, 0}, outBuf{outs, nullptr, 2, 0, 0};
	int64_t steady = 0;
	auto process = [&](Events &ev) {
		clap_process pr{};
		pr.steady_time = steady;
		pr.frames_count = frames;
		pr.audio_inputs = instrument ? nullptr : &inBuf;
		pr.audio_inputs_count = instrument ? 0 : 1;
		pr.audio_outputs = &outBuf;
		pr.audio_outputs_count = 1;
		pr.in_events = &ev.list;
		pr.out_events = &sink;
		p->process(p, &pr);
		steady += frames;
	};
	auto pump = [&] {
		if (callbackRequested.exchange(false)) p->on_main_thread(p);
	};

	// running, like a DAW with transport stopped
	p->activate(p, sr, 1, frames);
	p->start_processing(p);

	// restore a session that names the plugin
	std::string json = std::string("{\"pluginId\":\"") + argv[3] + "\",\"packageSha256\":\"" + argv[4] + "\"}";
	std::vector<uint8_t> blob = {'P', 'L', 'C', 'I'};
	putU32(blob, 1);
	putU32(blob, (uint32_t)json.size());
	blob.insert(blob.end(), json.begin(), json.end());
	putU32(blob, 0);
	struct R { std::vector<uint8_t> *v; size_t pos; } r{&blob, 0};
	clap_istream is{&r, [](const clap_istream *s, void *b, uint64_t n) -> int64_t {
		auto *r = (R *)s->ctx;
		n = std::min<uint64_t>(n, r->v->size() - r->pos);
		std::memcpy(b, r->v->data() + r->pos, n);
		r->pos += n;
		return (int64_t)n;
	}};
	if (!state->load(p, &is)) { std::printf("state load failed\n"); return 1; }
	restartRequested = false;
	auto t0 = std::chrono::steady_clock::now();
	while (!dirty) {
		Events none;
		std::fill(inL.begin(), inL.end(), 0.f);
		std::fill(inR.begin(), inR.end(), 0.f);
		process(none);
		pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { std::printf("load timeout\n"); return 1; }
	}
	bool restart = restartRequested.load();
	uint32_t latencyWhileActive = latency->get(p);

	// what a DAW does on request_restart: deactivate, read latency, activate
	p->stop_processing(p);
	p->deactivate(p);
	p->activate(p, sr, 1, frames);
	p->start_processing(p);
	uint32_t reported = latency->get(p);

	constexpr uint32_t kAt = 100;
	float settlePeak = 0; // output with no input and no host transport (a standalone with nothing playing)
	for (int b = 0; b < int(4 * sr / frames); ++b) { // settle
		Events none;
		std::fill(inL.begin(), inL.end(), 0.f);
		std::fill(inR.begin(), inR.end(), 0.f);
		process(none);
		pump();
		for (uint32_t i = 0; i < frames; ++i) settlePeak = std::max({settlePeak, std::fabs(outL[i]), std::fabs(outR[i])});
	}
	float floor = 0;
	for (uint32_t i = 0; i < frames; ++i) floor = std::max({floor, std::fabs(outL[i]), std::fabs(outR[i])});
	const float threshold = std::max(1e-4f, floor * 4);
	int64_t delay = -1;
	for (int b = 0; b < int(sr / frames) && delay < 0; ++b) {
		Events ev;
		std::fill(inL.begin(), inL.end(), 0.f);
		std::fill(inR.begin(), inR.end(), 0.f);
		if (b == 0) {
			if (instrument) {
				clap_event_note n{};
				n.header = {sizeof(n), kAt, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
				n.note_id = -1;
				n.port_index = 0;
				n.channel = 0;
				n.key = 60;
				n.velocity = 1.0;
				ev.v.push_back(n);
			} else {
				inL[kAt] = inR[kAt] = 1.0f;
			}
		}
		process(ev);
		for (uint32_t i = (b == 0 ? kAt : 0); i < frames && delay < 0; ++i)
			if (std::fabs(outL[i]) > threshold || std::fabs(outR[i]) > threshold) delay = int64_t(b) * frames + i - kAt;
	}
	std::printf("{\"plugin\":\"%s\",\"restartRequested\":%s,\"latencyBeforeRestart\":%u,\"reportedLatency\":%u,\"measuredDelay\":%lld,\"settlePeak\":%g}\n",
	            argv[3], restart ? "true" : "false", latencyWhileActive, reported, (long long)delay, double(settlePeak));
	p->stop_processing(p);
	p->deactivate(p);
	p->destroy(p);
	entry->deinit();
	return 0;
}
