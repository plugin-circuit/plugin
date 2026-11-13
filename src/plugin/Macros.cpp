#include "plugin/Macros.h"

namespace plugincircuit {

std::string MacroMap::displayName(uint32_t slot) const {
	if (slot >= kNumMacros) return {};
	auto &m = macros[slot];
	if (!m.name.empty()) return m.name;
	return "Macro " + std::to_string(slot + 1);
}

int MacroMap::slotForParam(clap_id paramId) const {
	for (uint32_t i = 0; i < kNumMacros; ++i) {
		auto &t = macros[i].targets;
		if (t.size() == 1 && t[0].paramId == paramId) return (int)i;
	}
	return -1;
}

MacroMap MacroMap::autoMap(const std::vector<ParamInfo> &params) {
	MacroMap map;
	map.autoMapped = true;
	uint32_t slot = 0;
	for (auto &p : params) {
		if (slot >= kNumMacros) break;
		if (!p.automatable() || (p.flags & CLAP_PARAM_IS_HIDDEN)) continue;
		Macro &m = map.macros[slot++];
		m.name = p.name;
		m.targets = {MacroTarget{p.id, p.minValue, p.maxValue, p.stepped()}};
		m.defaultValue = m.targets[0].unmap(p.defaultValue);
	}
	return map;
}

} // namespace plugincircuit
