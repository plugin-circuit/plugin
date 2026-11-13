#pragma once
// Macro slots: the fixed parameter set Plugin Circuit exposes to the DAW (like PatchWork's "Control 01..40"
// or Metaplugin's 100 params). Each macro drives one or more inner-plugin parameters over a range.
#include "core/InnerHost.h"

#include <array>
#include <cmath>
#include <atomic>
#include <string>
#include <vector>

namespace plugincircuit {

constexpr uint32_t kNumMacros = 64;

struct MacroTarget {
	clap_id paramId = 0;
	double minValue = 0, maxValue = 1; // in the inner parameter's units
	bool stepped = false;
	double map(double normalized) const {
		double v = minValue + (maxValue - minValue) * normalized;
		return stepped ? std::round(v) : v;
	}
	double unmap(double value) const {
		double span = maxValue - minValue;
		if (span == 0) return 0;
		double n = (value - minValue) / span;
		return n < 0 ? 0 : (n > 1 ? 1 : n);
	}
};

struct Macro {
	std::string name;        // shown to the DAW; "" means "Macro N"
	bool customName = false; // user renamed it
	std::vector<MacroTarget> targets;
	double defaultValue = 0; // normalized
};

// Immutable once published to the audio thread (copy-on-write from the main thread).
struct MacroMap {
	std::array<Macro, kNumMacros> macros;
	bool autoMapped = true;

	std::string displayName(uint32_t slot) const;
	// The macro that is a 1:1 view of this inner parameter (single target), or -1.
	int slotForParam(clap_id paramId) const;

	// First kNumMacros automatable inner parameters, full range, named after the parameter.
	static MacroMap autoMap(const std::vector<ParamInfo> &params);
};

} // namespace plugincircuit
