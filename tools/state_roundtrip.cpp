// Loads PluginCircuit.clap, sets macro values via params.flush, saves state, loads it into a fresh instance
// and compares. Usage: plugincircuit-state-test <path/to/PluginCircuit.clap> [pluginId]
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include "ClapLoad.h"
#include <string>
#include <vector>

namespace {
const void *getExt(const clap_host *, const char *) { return nullptr; }
clap_host host{CLAP_VERSION_INIT, nullptr, "state-test", "plugincircuit", "", "0", getExt,
               [](const clap_host *) {}, [](const clap_host *) {}, [](const clap_host *) {}};

struct Events {
	std::vector<clap_event_param_value> v;
	clap_input_events list{this, [](const clap_input_events *l) { return (uint32_t)((Events *)l->ctx)->v.size(); },
	                       [](const clap_input_events *l, uint32_t i) -> const clap_event_header * { return &((Events *)l->ctx)->v[i].header; }};
};
clap_output_events sink{nullptr, [](const clap_output_events *, const clap_event_header *) { return true; }};
} // namespace

int main(int argc, char **argv) {
	if (argc < 2) return 2;
	auto *entry = loadClapEntry(argv[1]);
	if (!entry) return 1;
	entry->init(argv[1]);
	auto *factory = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	const char *id = argc > 2 ? argv[2] : "studio.plugincircuit.host.fx";

	auto make = [&] {
		auto *p = factory->create_plugin(factory, &host, id);
		p->init(p);
		return p;
	};
	auto *a = make();
	auto *params = (const clap_plugin_params *)a->get_extension(a, CLAP_EXT_PARAMS);
	auto *state = (const clap_plugin_state *)a->get_extension(a, CLAP_EXT_STATE);
	Events ev;
	for (uint32_t i = 0; i < 8; ++i) {
		clap_event_param_value e{};
		e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
		e.param_id = i;
		e.note_id = e.port_index = e.channel = e.key = -1;
		e.value = 0.1 * (i + 1);
		ev.v.push_back(e);
	}
	params->flush(a, &ev.list, &sink);
	double va = -1;
	params->get_value(a, 3, &va);
	std::vector<uint8_t> blob;
	clap_ostream os{&blob, [](const clap_ostream *s, const void *b, uint64_t n) -> int64_t {
		auto *v = (std::vector<uint8_t> *)s->ctx;
		v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
		return (int64_t)n;
	}};
	state->save(a, &os);
	std::printf("A macro4=%.3f, state %zu bytes\n", va, blob.size());

	auto *b = make();
	auto *paramsB = (const clap_plugin_params *)b->get_extension(b, CLAP_EXT_PARAMS);
	auto *stateB = (const clap_plugin_state *)b->get_extension(b, CLAP_EXT_STATE);
	struct R { std::vector<uint8_t> *v; size_t pos; } r{&blob, 0};
	clap_istream is{&r, [](const clap_istream *s, void *b, uint64_t n) -> int64_t {
		auto *r = (R *)s->ctx;
		n = std::min<uint64_t>(n, r->v->size() - r->pos);
		std::memcpy(b, r->v->data() + r->pos, n);
		r->pos += n;
		return (int64_t)n;
	}};
	bool ok = stateB->load(b, &is);
	double vb = -1;
	paramsB->get_value(b, 3, &vb);
	std::printf("B load=%d macro4=%.3f -> %s\n", ok, vb, std::fabs(va - vb) < 1e-9 ? "OK" : "MISMATCH");
	a->destroy(a);
	b->destroy(b);
	entry->deinit();
	return std::fabs(va - vb) < 1e-9 ? 0 : 1;
}
