#include "plugin/StateCodec.h"

#include <choc/text/choc_JSON.h>

#include <cstring>

namespace plugincircuit {

namespace {

void putU32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
bool getU32(const std::vector<uint8_t> &in, size_t &pos, uint32_t &v) {
	if (pos + 4 > in.size()) return false;
	v = 0;
	for (int i = 0; i < 4; ++i) v |= uint32_t(in[pos + i]) << (8 * i);
	pos += 4;
	return true;
}
std::string str(const choc::value::ValueView &v, const char *key) {
	return v.isObject() && v.hasObjectMember(key) && v[key].isString() ? std::string(v[key].getString()) : std::string();
}
double num(const choc::value::ValueView &v, const char *key, double fallback = 0) {
	if (!v.isObject() || !v.hasObjectMember(key)) return fallback;
	auto m = v[key];
	if (m.isFloat() || m.isInt()) return m.getWithDefault<double>(fallback);
	return fallback;
}

} // namespace

std::vector<uint8_t> encodeState(const HostState &s) {
	auto macros = choc::value::createEmptyArray();
	for (uint32_t i = 0; i < kNumMacros; ++i) {
		auto &m = s.macros.macros[i];
		auto targets = choc::value::createEmptyArray();
		for (auto &t : m.targets)
			targets.addArrayElement(choc::value::createObject("T", "id", (int64_t)t.paramId, "min", t.minValue, "max", t.maxValue,
			                                                  "stepped", t.stepped));
		macros.addArrayElement(choc::value::createObject("M", "name", m.name, "custom", m.customName, "default", m.defaultValue,
		                                                 "value", s.macroValues[i], "targets", targets));
	}
	auto params = choc::value::createEmptyArray();
	for (auto &[id, value] : s.innerParams) params.addArrayElement(choc::value::createObject("P", "id", (int64_t)id, "v", value));

	auto json = choc::value::createObject("PluginCircuitState", "pluginId", s.pluginId, "pluginName", s.pluginName, "vendor", s.vendor,
	                                      "packageId", s.packageId, "packageSha256", s.packageSha256, "moduleSha256", s.moduleSha256,
	                                      "autoMapped", s.macros.autoMapped, "macros", macros, "innerParams", params);
	auto text = choc::json::toString(json, false);

	std::vector<uint8_t> out = {'P', 'L', 'C', 'I'};
	putU32(out, 1);
	putU32(out, (uint32_t)text.size());
	out.insert(out.end(), text.begin(), text.end());
	putU32(out, (uint32_t)s.innerState.size());
	out.insert(out.end(), s.innerState.begin(), s.innerState.end());
	return out;
}

std::optional<HostState> decodeState(const std::vector<uint8_t> &in, std::string &error) {
	if (in.size() < 16 || std::memcmp(in.data(), "PLCI", 4) != 0) {
		error = "not a Plugin Circuit state";
		return std::nullopt;
	}
	size_t pos = 4;
	uint32_t version = 0, jsonSize = 0, innerSize = 0;
	if (!getU32(in, pos, version) || version != 1) {
		error = "unsupported state version";
		return std::nullopt;
	}
	if (!getU32(in, pos, jsonSize) || pos + jsonSize > in.size()) {
		error = "truncated state";
		return std::nullopt;
	}
	std::string text(in.begin() + (long)pos, in.begin() + (long)(pos + jsonSize));
	pos += jsonSize;
	if (!getU32(in, pos, innerSize) || pos + innerSize > in.size()) {
		error = "truncated inner state";
		return std::nullopt;
	}
	HostState s;
	s.innerState.assign(in.begin() + (long)pos, in.begin() + (long)(pos + innerSize));
	try {
		auto j = choc::json::parse(text);
		s.pluginId = str(j, "pluginId");
		s.pluginName = str(j, "pluginName");
		s.vendor = str(j, "vendor");
		s.packageId = str(j, "packageId");
		s.packageSha256 = str(j, "packageSha256");
		s.moduleSha256 = str(j, "moduleSha256");
		s.macros.autoMapped = j.hasObjectMember("autoMapped") && j["autoMapped"].isBool() ? j["autoMapped"].getBool() : true;
		if (j.hasObjectMember("macros") && j["macros"].isArray()) {
			auto arr = j["macros"];
			for (uint32_t i = 0; i < std::min<uint32_t>(kNumMacros, arr.size()); ++i) {
				auto m = arr[i];
				Macro &macro = s.macros.macros[i];
				macro.name = str(m, "name");
				macro.customName = m.hasObjectMember("custom") && m["custom"].isBool() && m["custom"].getBool();
				macro.defaultValue = num(m, "default");
				s.macroValues[i] = num(m, "value");
				if (m.hasObjectMember("targets") && m["targets"].isArray()) {
					for (auto t : m["targets"]) {
						MacroTarget target;
						target.paramId = (clap_id)num(t, "id");
						target.minValue = num(t, "min");
						target.maxValue = num(t, "max", 1);
						target.stepped = t.hasObjectMember("stepped") && t["stepped"].isBool() && t["stepped"].getBool();
						macro.targets.push_back(target);
					}
				}
			}
		}
		if (j.hasObjectMember("innerParams") && j["innerParams"].isArray())
			for (auto p : j["innerParams"]) s.innerParams[(clap_id)num(p, "id")] = num(p, "v");
	} catch (const std::exception &e) {
		error = std::string("state JSON: ") + e.what();
		return std::nullopt;
	}
	return s;
}

} // namespace plugincircuit
