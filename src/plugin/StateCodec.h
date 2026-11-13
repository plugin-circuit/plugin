#pragma once
// Plugin Circuit's own state: which WCLAP is loaded (by catalog id + content hashes), the macro map,
// the inner plugin's state blob, and a parameter-value snapshot as a fallback.
//
// Binary layout (little endian):  "PLCI" | u32 version=1 | u32 jsonSize | json | u32 innerSize | inner bytes
#include "plugin/Macros.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace plugincircuit {

struct HostState {
	std::string pluginId, pluginName, vendor, packageId, packageSha256, moduleSha256;
	MacroMap macros;
	std::array<double, kNumMacros> macroValues{};
	std::map<clap_id, double> innerParams; // snapshot, used if the inner state blob fails to load
	std::vector<uint8_t> innerState;
	bool empty() const { return pluginId.empty(); }
};

std::vector<uint8_t> encodeState(const HostState &state);
std::optional<HostState> decodeState(const std::vector<uint8_t> &bytes, std::string &error);

} // namespace plugincircuit
