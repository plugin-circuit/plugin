// The gesture and ownership state machine (src/plugin/Gestures.h): one case per cell of the table in
// docs/MESSAGING.md ("Gestures and ownership"), plus multi-target macros, unmapped parameters and closing.
//
// Setup: inner parameters 10, 20, 30, 40. Macro 1 (slot 0) is a 1:1 view of 10 (range 0..100); macro 2 (slot 1)
// drives 20 and 30; 40 is on no macro. A case brings parameter 10 (or another) to an owner, then sends one event and
// compares what came out: "B0" / "E0" a gesture begin / end on macro slot 0 to the DAW, "D0=0.5" a macro value to
// the DAW, "M0=0.5" a macro value not told to the DAW, "P10=50" a value to the WCLAP.
#include "plugin/Gestures.h"

#include <cstdio>
#include <functional>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace plugincircuit;

namespace {
int failures = 0;

struct Recorder final : Gestures::Sink {
	ParamTable *table = nullptr;
	std::vector<std::string> log;
	void toPlugin(clap_id p, double v) override {
		if (table) table->setById(p, v);
		log.push_back("P" + std::to_string(p) + "=" + num(v));
	}
	void macroValue(uint32_t m, double v, bool tellDaw) override { log.push_back((tellDaw ? "D" : "M") + std::to_string(m) + "=" + num(v)); }
	void dawGesture(uint32_t m, bool begin) override { log.push_back((begin ? "B" : "E") + std::to_string(m)); }
	static std::string num(double v) {
		std::ostringstream s;
		s << v;
		return s.str();
	}
	std::string take() {
		std::string s;
		for (auto &e : log) s += (s.empty() ? "" : " ") + e;
		log.clear();
		return s;
	}
};

struct World {
	ParamTable table{[] {
		std::vector<ParamInfo> v;
		for (clap_id id : {10u, 20u, 30u, 40u}) {
			ParamInfo p;
			p.id = id;
			p.defaultValue = 0;
			v.push_back(p);
		}
		return v;
	}()};
	MacroMap map;
	Gestures g;
	Recorder r;
	World() {
		map.macros[0].targets.push_back({10, 0, 100});
		map.macros[1].targets.push_back({20, 0, 1});
		map.macros[1].targets.push_back({30, 0, 1});
		r.table = &table;
		g.begin(&table, r);
	}
	Owner owner(clap_id p) const { return table.owner(size_t(table.indexOf(p))); }
	double value(clap_id p) const {
		double v = -1;
		table.valueById(p, v);
		return v;
	}
	std::set<clap_id> changed() {
		std::set<clap_id> s;
		table.takeChanged([&](size_t i) { s.insert(table.info(i).id); });
		return s;
	}
};

const char *name(Owner o) { return o == Owner::None ? "none" : o == Owner::Ui ? "ui" : "plugin"; }

using Step = std::function<void(World &)>;
const Step none = [](World &) {};
Step uiParam(clap_id p, double v, Phase ph) { return [=](World &w) { w.g.uiParam(&w.map, p, v, ph, w.r); }; }
Step uiMacro(uint32_t m, double v, Phase ph) { return [=](World &w) { w.g.uiMacro(&w.map, m, v, ph, w.r); }; }
Step plugBegin(clap_id p) { return [=](World &w) { w.g.pluginGesture(&w.map, p, true, w.r); }; }
Step plugEnd(clap_id p) { return [=](World &w) { w.g.pluginGesture(&w.map, p, false, w.r); }; }
Step plugValue(clap_id p, double v) { return [=](World &w) { w.g.pluginValue(&w.map, p, v, w.r); }; }
Step daw(uint32_t m, double v) { return [=](World &w) { w.g.dawMacro(&w.map, m, v, w.r); }; }

struct Case {
	const char *name;
	Step setup, event;
	const char *expect; // the sink's log for the event
	clap_id param;      // whose owner and value are checked
	Owner owner;
	double value;       // the table's value after the event (-1: don't check)
};

void run(const Case &c) {
	World w;
	c.setup(w);
	w.r.take();
	w.changed();
	c.event(w);
	const std::string got = w.r.take();
	const Owner o = w.owner(c.param);
	const double v = w.value(c.param);
	const bool ok = got == c.expect && o == c.owner && (c.value < 0 || v == c.value);
	std::printf("%s %-58s [%s]%s\n", ok ? "ok  " : "FAIL", c.name, got.c_str(),
	            ok ? "" : (std::string("  want [") + c.expect + "] owner " + name(c.owner) + ", got owner " + name(o) +
	                       " value " + Recorder::num(v)).c_str());
	if (!ok) ++failures;
}
} // namespace

int main() {
	const Step uiOwns = uiParam(10, 50, Phase::Begin), pluginOwns = plugBegin(10);
	const Case cases[] = {
	    // owner none
	    {"none: UI begin -> owner UI, DAW begin + value", none, uiParam(10, 50, Phase::Begin), "B0 P10=50 D0=0.5", 10, Owner::Ui, 50},
	    {"none: UI set -> applied, DAW told", none, uiParam(10, 50, Phase::Set), "P10=50 D0=0.5", 10, Owner::None, 50},
	    {"none: UI end -> nothing open", none, uiParam(10, 50, Phase::End), "", 10, Owner::None, 0},
	    {"none: WCLAP begin -> owner plugin, DAW begin", none, plugBegin(10), "B0", 10, Owner::Plugin, 0},
	    {"none: WCLAP value -> table + DAW", none, plugValue(10, 20), "D0=0.2", 10, Owner::None, 20},
	    {"none: WCLAP end -> nothing open", none, plugEnd(10), "", 10, Owner::None, 0},
	    {"none: DAW value -> applied, not echoed", none, daw(0, 0.3), "M0=0.3 P10=30", 10, Owner::None, 30},
	    // owner UI
	    {"UI: UI begin again -> no second DAW begin", uiOwns, uiParam(10, 60, Phase::Begin), "P10=60 D0=0.6", 10, Owner::Ui, 60},
	    {"UI: UI update -> applied", uiOwns, uiParam(10, 70, Phase::Update), "P10=70 D0=0.7", 10, Owner::Ui, 70},
	    {"UI: UI end -> final value, DAW end, owner none", uiOwns, uiParam(10, 80, Phase::End), "P10=80 D0=0.8 E0", 10, Owner::None, 80},
	    {"UI: WCLAP begin -> ignored", uiOwns, plugBegin(10), "", 10, Owner::Ui, 50},
	    {"UI: WCLAP value -> dropped", uiOwns, plugValue(10, 5), "", 10, Owner::Ui, 50},
	    {"UI: WCLAP end -> ignored", uiOwns, plugEnd(10), "", 10, Owner::Ui, 50},
	    {"UI: DAW value -> dropped (touch)", uiOwns, daw(0, 0.1), "", 10, Owner::Ui, 50},
	    // owner plugin
	    {"plugin: UI begin -> ignored", pluginOwns, uiParam(10, 50, Phase::Begin), "", 10, Owner::Plugin, 0},
	    {"plugin: UI update -> dropped", pluginOwns, uiParam(10, 50, Phase::Update), "", 10, Owner::Plugin, 0},
	    {"plugin: UI end -> dropped", pluginOwns, uiParam(10, 50, Phase::End), "", 10, Owner::Plugin, 0},
	    {"plugin: WCLAP begin again -> nothing", pluginOwns, plugBegin(10), "", 10, Owner::Plugin, 0},
	    {"plugin: WCLAP value -> table + DAW", pluginOwns, plugValue(10, 40), "D0=0.4", 10, Owner::Plugin, 40},
	    {"plugin: WCLAP end -> DAW end, owner none", pluginOwns, plugEnd(10), "E0", 10, Owner::None, 0},
	    {"plugin: DAW value -> dropped (touch)", pluginOwns, daw(0, 0.9), "", 10, Owner::Plugin, 0},
	    // a macro with two targets
	    {"macro drag begin -> DAW begin, both targets owned by UI", none, uiMacro(1, 0.5, Phase::Begin), "B1 D1=0.5 P20=0.5 P30=0.5", 30, Owner::Ui, 0.5},
	    {"macro dragged: WCLAP value on a target -> dropped", uiMacro(1, 0.5, Phase::Begin), plugValue(20, 0.9), "", 20, Owner::Ui, 0.5},
	    {"macro dragged: DAW value on it -> dropped", uiMacro(1, 0.5, Phase::Begin), daw(1, 0.1), "", 20, Owner::Ui, 0.5},
	    {"macro drag end -> DAW end, targets released", uiMacro(1, 0.5, Phase::Begin), uiMacro(1, 0.6, Phase::End), "D1=0.6 P20=0.6 P30=0.6 E1", 30, Owner::None, 0.6},
	    {"macro: a target in a plugin gesture keeps it", plugBegin(20), uiMacro(1, 0.5, Phase::Begin), "B1 D1=0.5 P30=0.5", 20, Owner::Plugin, 0},
	    {"macro: a DAW value skips a target the plugin owns", plugBegin(20), daw(1, 0.3), "M1=0.3 P30=0.3", 20, Owner::Plugin, 0},
	    // not on a 1:1 macro: the DAW hears nothing
	    {"unmapped: WCLAP begin -> no DAW gesture", none, plugBegin(40), "", 40, Owner::Plugin, 0},
	    {"unmapped: WCLAP value -> table only", plugBegin(40), plugValue(40, 0.7), "", 40, Owner::Plugin, 0.7},
	    {"unmapped: WCLAP end -> no DAW gesture", plugBegin(40), plugEnd(40), "", 40, Owner::None, 0},
	    {"two-target macro param: UI drag -> no DAW gesture", none, uiParam(20, 0.4, Phase::Begin), "P20=0.4", 20, Owner::Ui, 0.4},
	    // closing gestures left open
	    {"editor closed mid-drag -> DAW end, owner none", uiOwns, [](World &w) { w.g.requestCloseUi(); w.g.begin(&w.table, w.r); }, "E0", 10, Owner::None, 50},
	    {"close UI only keeps the plugin's gesture", pluginOwns, [](World &w) { w.g.requestCloseUi(); w.g.begin(&w.table, w.r); }, "", 10, Owner::Plugin, 0},
	    {"reset / deactivate -> every gesture closed", pluginOwns, [](World &w) { w.g.requestCloseAll(); w.g.begin(&w.table, w.r); }, "E0", 10, Owner::None, 0},
	    {"plugin unloaded (new table) -> macro gestures closed", uiOwns, [](World &w) { w.g.begin(nullptr, w.r); }, "E0", 10, Owner::Ui, 50},
	    {"a new table at the old one's address -> macro gestures closed", uiOwns,
	     [](World &w) { // rebuild in place: same address, new generation
		     auto infos = w.table.infos();
		     w.table.~ParamTable();
		     new (&w.table) ParamTable(infos);
		     w.g.begin(&w.table, w.r);
	     }, "E0", 10, Owner::None, 0},
	};
	for (auto &c : cases) run(c);

	// the end of a gesture pushes the final value to the page again, even if unchanged
	{
		World w;
		w.g.uiParam(&w.map, 10, 50, Phase::Begin, w.r);
		w.changed();
		w.g.uiParam(&w.map, 10, 50, Phase::End, w.r);
		const bool ok = w.changed().count(10) == 1;
		std::printf("%s %-58s\n", ok ? "ok  " : "FAIL", "gesture end marks the parameter changed");
		if (!ok) ++failures;
	}

	std::printf(failures ? "%d failure(s)\n" : "gestures: all cases passed\n", failures);
	return failures ? 1 : 0;
}
