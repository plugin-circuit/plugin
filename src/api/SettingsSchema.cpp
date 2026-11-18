#include "api/SettingsSchema.h"

#include <algorithm>
#include <cmath>

namespace plugincircuit::settings {

namespace {
using Type = Setting::Type;
using choc::value::Value;
using choc::value::ValueView;

const char *scopeName(Scope s) { return s == Scope::Machine ? "a machine" : s == Scope::Instance ? "an instance" : "a session"; }

Value defaultOf(const Setting &s) {
	if (s.type == Type::Bool) return Value(s.def != 0);
	if (s.type == Type::Choice) return Value(s.choices.empty() ? std::string() : s.choices.front());
	return Value(s.def);
}

double fit(const Setting &s, double v) {
	v = std::clamp(v, s.min, s.max);
	if (s.step > 0) v = std::clamp(s.min + std::round((v - s.min) / s.step) * s.step, s.min, s.max);
	return std::round(v * 1e9) / 1e9; // 7 * 0.1 is 0.7000000000000001: settings.json shows 0.7
}

// a valid value for s from v, or an error
std::string coerce(const Setting &s, const ValueView &v, Value &out) {
	switch (s.type) {
	case Type::Bool:
		if (!v.isBool()) return std::string(s.key) + " must be true or false";
		out = Value(v.getBool());
		return {};
	case Type::Number: {
		if (!v.isFloat() && !v.isInt()) return std::string(s.key) + " must be a number";
		const double d = v.getWithDefault<double>(s.def);
		if (!std::isfinite(d)) return std::string(s.key) + " must be a finite number";
		out = Value(fit(s, d));
		return {};
	}
	case Type::Choice: {
		const std::string c = v.isString() ? std::string(v.getString()) : std::string();
		if (std::find(s.choices.begin(), s.choices.end(), c) == s.choices.end()) {
			std::string list;
			for (auto &x : s.choices) list += (list.empty() ? "" : ", ") + x;
			return std::string(s.key) + " must be one of " + list;
		}
		out = Value(c);
		return {};
	}
	}
	return "unknown type";
}
} // namespace

const std::vector<Setting> &schema() {
	static const std::vector<Setting> all = [] {
		std::vector<Setting> v;
		{ // wclap-bridge's wclap_set_audio_wait_ratio()
			Setting s{"audioWaitRatio", Scope::Machine, Type::Number};
			s.def = 0.2, s.min = 0, s.max = 0.9, s.step = 0.1;
			s.label = "Audio wait for single-threaded plugins";
			s.help = "Most catalog plugins run on one thread. While their UI or a preset load is busy, the audio waits up to "
			         "this share of each block, then skips the block instead of stalling your DAW.";
			s.format = "percent", s.suffix = " of a block", s.zeroLabel = "Off (try once)";
			v.push_back(s);
		}
		{ // wclap_set_conceal_dropouts()
			Setting s{"concealDropouts", Scope::Machine, Type::Bool};
			s.def = 1;
			s.label = "Smooth over skipped blocks";
			s.help = "A skipped block continues the plugin's last sound and fades it, instead of cutting to silence. No clicks "
			         "at the gap, no added latency.";
			v.push_back(s);
		}
		{ // the view the editor opens on while a plugin is loaded ("settings" is never restored)
			Setting s{"view", Scope::Instance, Type::Choice};
			s.choices = {"plugin", "browse", "macros", "params"};
			v.push_back(s);
		}
		{ // the on-screen keyboard's lowest key, in octaves (C3; 3 octaves shown, the top key at most 127)
			Setting s{"keyboardBase", Scope::Instance, Type::Number};
			s.def = 48, s.min = 0, s.max = 84, s.step = 12;
			v.push_back(s);
		}
		return v;
	}();
	return all;
}

const Setting *find(const std::string &key) {
	for (auto &s : schema())
		if (key == s.key) return &s;
	return nullptr;
}

Value defaults(Scope scope) {
	auto v = choc::value::createObject("Settings");
	for (auto &s : schema())
		if (s.scope == scope) v.addMember(s.key, defaultOf(s));
	return v;
}

Value sanitize(Scope scope, const ValueView &stored) {
	auto v = choc::value::createObject("Settings");
	for (auto &s : schema()) {
		if (s.scope != scope) continue;
		Value value;
		if (!stored.isObject() || !stored.hasObjectMember(s.key) || !coerce(s, stored[s.key], value).empty()) value = defaultOf(s);
		v.addMember(s.key, value);
	}
	return v;
}

std::string apply(Scope scope, const ValueView &change, Value &values) {
	if (!change.isObject()) return "expected an object";
	std::vector<std::pair<const Setting *, Value>> next; // all or nothing
	for (uint32_t i = 0; i < change.size(); ++i) {
		auto m = change.getObjectMemberAt(i);
		const Setting *s = find(m.name);
		if (!s) return std::string("unknown setting '") + m.name + "'";
		if (s->scope != scope) return std::string(m.name) + " is " + scopeName(s->scope) + " setting";
		Value v;
		if (auto e = coerce(*s, m.value, v); !e.empty()) return e;
		next.emplace_back(s, std::move(v));
	}
	for (auto &[s, v] : next) values.setMember(s->key, v);
	return {};
}

Value describe(Scope scope) {
	auto list = choc::value::createEmptyArray();
	for (auto &s : schema()) {
		if (s.scope != scope || !*s.label) continue;
		auto d = choc::value::createObject("Setting", "key", s.key, "type",
		                                   s.type == Type::Bool ? "bool" : s.type == Type::Number ? "number" : "choice",
		                                   "label", s.label, "help", s.help);
		d.addMember("default", defaultOf(s));
		if (s.type == Type::Number) {
			d.addMember("min", s.min), d.addMember("max", s.max), d.addMember("step", s.step);
			d.addMember("format", s.format), d.addMember("suffix", s.suffix), d.addMember("zeroLabel", s.zeroLabel);
		}
		if (s.type == Type::Choice) {
			auto c = choc::value::createEmptyArray();
			for (auto &x : s.choices) c.addArrayElement(x);
			d.addMember("choices", c);
		}
		list.addArrayElement(d);
	}
	return list;
}

} // namespace plugincircuit::settings
