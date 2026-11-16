// A hosted plugin changes its own parameter (as its GUI would): does Plugin Circuit follow? (docs/PLAN.md, 1b.2)
//
// Drives PluginCircuit.clap through CLAP like a DAW: offline, with a scratch data folder; installs the test WCLAP
// `test-param-events` (tests/wclaps) from its archive and loads it through a session state that maps macro 1 to its
// Level parameter. Then plays notes:
//   key 60: the plugin sets Level to -6 dB and reports it (gesture begin, value, gesture end as output events).
//           The DAW must receive macro 1's gesture begin, value and end, and the parameter table must hold -6 dB
//           (read back through a state save, which takes the table's values).
//   key 61: the plugin sets Level to -30 dB without reporting it. The table can't know (only the safety net, which
//           re-reads visible parameters, finds it): it must still hold -6 dB.
//
// Usage: plugincircuit-plugin-param-events-test <PluginCircuit.clap> <test-param-events.wclap.tar.gz>
#include "core/Catalog.h"
#include "plugin/StateCodec.h"

#include <clap/clap.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace plugincircuit;

namespace {
int failures = 0;
#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);                                                \
			std::printf(__VA_ARGS__);                                                                                  \
			std::printf("\n");                                                                                         \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

thread_local bool tlsAudio = false;
bool callbackRequested = false;

const clap_host_thread_check threadCheck{[](const clap_host *) { return !tlsAudio; }, [](const clap_host *) { return tlsAudio; }};
const clap_host_params hostParams{[](const clap_host *, clap_param_rescan_flags) {},
                                  [](const clap_host *, clap_id, clap_param_clear_flags) {}, [](const clap_host *) {}};
const clap_host_state hostState{[](const clap_host *) {}};
const clap_host_log hostLog{[](const clap_host *, clap_log_severity, const char *msg) { std::printf("  [plugin log] %s\n", msg); }};
const void *getExt(const clap_host *, const char *id) {
	if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &threadCheck;
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &hostParams;
	if (!std::strcmp(id, CLAP_EXT_STATE)) return &hostState;
	if (!std::strcmp(id, CLAP_EXT_LOG)) return &hostLog;
	return nullptr;
}
clap_host host{CLAP_VERSION_INIT, nullptr, "param-events-test", "plugincircuit", "", "0", getExt,
               [](const clap_host *) {}, [](const clap_host *) {}, [](const clap_host *) { callbackRequested = true; }};

// event lists
struct InEvents {
	std::vector<clap_event_note> notes;
	clap_input_events list{this, [](const clap_input_events *l) { return uint32_t(static_cast<const InEvents *>(l->ctx)->notes.size()); },
	                       [](const clap_input_events *l, uint32_t i) -> const clap_event_header * {
		                       return &static_cast<const InEvents *>(l->ctx)->notes[i].header;
	                       }};
};
struct Seen {
	uint16_t type;
	clap_id id;
	double value;
};
struct OutEvents {
	std::vector<Seen> seen;
	clap_output_events list{this, [](const clap_output_events *l, const clap_event_header *h) {
		                        auto *self = static_cast<OutEvents *>(l->ctx);
		                        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
		                        if (h->type == CLAP_EVENT_PARAM_VALUE) {
			                        auto *e = reinterpret_cast<const clap_event_param_value *>(h);
			                        self->seen.push_back({h->type, e->param_id, e->value});
		                        } else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
			                        self->seen.push_back({h->type, reinterpret_cast<const clap_event_param_gesture *>(h)->param_id, 0});
		                        }
		                        return true;
	                        }};
};

std::vector<uint8_t> saveState(const clap_plugin *p, const clap_plugin_state *state) {
	std::vector<uint8_t> bytes;
	clap_ostream os{&bytes, [](const clap_ostream *s, const void *b, uint64_t n) -> int64_t {
		                auto *v = static_cast<std::vector<uint8_t> *>(s->ctx);
		                v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
		                return int64_t(n);
	                }};
	state->save(p, &os);
	return bytes;
}
bool loadState(const clap_plugin *p, const clap_plugin_state *state, const std::vector<uint8_t> &bytes) {
	struct Cursor {
		const std::vector<uint8_t> *b;
		size_t pos = 0;
	} c{&bytes};
	clap_istream is{&c, [](const clap_istream *s, void *b, uint64_t n) -> int64_t {
		                auto *c = static_cast<Cursor *>(s->ctx);
		                const size_t k = std::min<size_t>(n, c->b->size() - c->pos);
		                std::memcpy(b, c->b->data() + c->pos, k);
		                c->pos += k;
		                return int64_t(k);
	                }};
	return state->load(p, &is);
}
double savedInnerValue(const clap_plugin *p, const clap_plugin_state *state, clap_id id) {
	std::string error;
	auto s = decodeState(saveState(p, state), error);
	if (!s || !s->innerParams.count(id)) return NAN;
	return s->innerParams.at(id);
}
} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <PluginCircuit.clap> <test-param-events.wclap.tar.gz>\n", argv[0]);
		return 2;
	}
	// offline, in a scratch data folder
	char home[] = "/tmp/plugincircuit-param-events-XXXXXX";
	if (!mkdtemp(home)) return 1;
	setenv("PLUGINCIRCUIT_HOME", home, 1);
	setenv("PLUGINCIRCUIT_API", "http://127.0.0.1:9", 1); // nothing listens: the catalog is unreachable

	std::ifstream f(argv[2], std::ios::binary);
	std::vector<uint8_t> archive((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::string sha, error;
	if (Catalog::installArchive(archive, sha, error).empty()) {
		std::printf("install failed: %s\n", error.c_str());
		return 1;
	}

	// a session: the test plugin, macro 1 = its Level (-60..0 dB)
	HostState session;
	session.pluginId = "studio.plugincircuit.test.param-events";
	session.pluginName = "Test Param Events";
	session.packageSha256 = sha;
	session.macros.autoMapped = false;
	MacroTarget level;
	level.paramId = 0;
	level.minValue = -60;
	level.maxValue = 0;
	session.macros.macros[0].targets.push_back(level);
	session.macros.macros[0].name = "Level"; // published when the load finishes: the test's "loaded" signal
	session.macros.macros[0].customName = true;

	std::string bin = std::string(argv[1]) + "/Contents/MacOS/PluginCircuit";
	void *lib = dlopen(bin.c_str(), RTLD_NOW);
	if (!lib) {
		std::printf("dlopen: %s\n", dlerror());
		return 1;
	}
	auto *entry = (const clap_plugin_entry *)dlsym(lib, "clap_entry");
	entry->init(argv[1]);
	auto *factory = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	const clap_plugin *p = factory->create_plugin(factory, &host, "studio.plugincircuit.host.synth");
	if (!p || !p->init(p)) return 1;
	auto *state = (const clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
	auto *params = (const clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);

	loadState(p, state, encodeState(session));
	// the load runs on a thread and finishes on the main thread: run on_main_thread until the session's macros apply
	bool loaded = false;
	for (int i = 0; i < 1000 && !loaded; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (callbackRequested) {
			callbackRequested = false;
			p->on_main_thread(p);
		}
		clap_param_info info{};
		loaded = params->get_info(p, 0, &info) && std::string(info.name) == "Level";
	}
	CHECK(loaded, "the test plugin didn't load (macro 1 isn't named after its Level parameter)");
	if (!loaded) return 1;

	constexpr uint32_t frames = 256;
	p->activate(p, 48000, 1, frames);
	tlsAudio = true;
	p->start_processing(p);
	tlsAudio = false;
	std::vector<float> l(frames), r(frames);
	float *outs[2] = {l.data(), r.data()};
	clap_audio_buffer out{outs, nullptr, 2, 0, 0};
	auto block = [&](int key) {
		InEvents in;
		if (key >= 0) {
			clap_event_note n{};
			n.header = {sizeof(n), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
			n.note_id = -1;
			n.key = int16_t(key);
			n.velocity = 1;
			in.notes.push_back(n);
		}
		OutEvents seen;
		clap_process proc{};
		proc.frames_count = frames;
		proc.audio_outputs = &out;
		proc.audio_outputs_count = 1;
		proc.in_events = &in.list;
		proc.out_events = &seen.list;
		tlsAudio = true;
		p->process(p, &proc);
		tlsAudio = false;
		return seen.seen;
	};
	for (int i = 0; i < 4; ++i) block(-1); // settle (the slot starts processing)

	// key 60: a reported change, with a gesture
	auto seen = block(60);
	for (int i = 0; i < 2; ++i) {
		auto more = block(-1);
		seen.insert(seen.end(), more.begin(), more.end());
	}
	std::vector<uint16_t> types;
	double macroValue = NAN;
	for (auto &e : seen)
		if (e.id == 0) {
			types.push_back(e.type);
			if (e.type == CLAP_EVENT_PARAM_VALUE) macroValue = e.value;
		}
	std::printf("key 60: the DAW got %zu events for macro 1, macro value %.3f\n", types.size(), macroValue);
	CHECK((types == std::vector<uint16_t>{CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_PARAM_GESTURE_END}),
	      "expected gesture begin, value, gesture end for macro 1");
	CHECK(std::fabs(macroValue - 0.9) < 1e-6, "macro 1 should be -6 dB on -60..0 = 0.9, got %.6f", macroValue);
	const double afterReported = savedInnerValue(p, state, 0);
	std::printf("key 60: the table holds %.2f dB\n", afterReported);
	CHECK(std::fabs(afterReported - -6.0) < 1e-9, "the table should hold -6 dB, got %.6f", afterReported);

	// key 61: an unreported change; the table keeps the last value it was told about
	seen = block(61);
	block(-1);
	const double afterSilent = savedInnerValue(p, state, 0);
	std::printf("key 61 (not reported): the DAW got %zu events, the table holds %.2f dB (the plugin: -30)\n", seen.size(), afterSilent);
	CHECK(seen.empty(), "an unreported change must not reach the DAW");
	CHECK(std::fabs(afterSilent - -6.0) < 1e-9, "the table can't know an unreported change, got %.6f", afterSilent);

	tlsAudio = true;
	p->stop_processing(p);
	tlsAudio = false;
	p->deactivate(p);
	p->destroy(p);
	entry->deinit();
	std::printf(failures ? "%d failure(s)\n" : "PASS: the table and the DAW follow the plugin's reported changes\n", failures);
	std::fflush(stdout);
	_exit(failures ? 1 : 0);
}
