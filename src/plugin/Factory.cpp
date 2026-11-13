// The plugin factory: Plugin Circuit FX and Plugin Circuit Instrument, plus clap-wrapper's VST3/AUv2 hints.
#include "plugin/PluginCircuitPlugin.h"

#include <clapwrapper/auv2.h>
#include <clapwrapper/vst3.h>

#include <cstring>

using plugincircuit::PluginCircuitPlugin;

namespace {

const clap_plugin_descriptor *descriptors[] = {&PluginCircuitPlugin::fxDescriptor, &PluginCircuitPlugin::instrumentDescriptor};

const clap_plugin_factory pluginFactory = {
	.get_plugin_count = [](const clap_plugin_factory *) -> uint32_t { return 2; },
	.get_plugin_descriptor = [](const clap_plugin_factory *, uint32_t i) -> const clap_plugin_descriptor * {
		return i < 2 ? descriptors[i] : nullptr;
	},
	.create_plugin = [](const clap_plugin_factory *, const clap_host *host, const char *id) -> const clap_plugin * {
		if (!clap_version_is_compatible(host->clap_version)) return nullptr;
		for (int i = 0; i < 2; ++i)
			if (!std::strcmp(id, descriptors[i]->id)) return (new PluginCircuitPlugin(host, i == 1))->clapPlugin();
		return nullptr;
	},
};

const clap_plugin_factory_as_auv2 auv2Factory = {
	.manufacturer_code = "Pcir",
	.manufacturer_name = "Plugin Circuit",
	.get_auv2_info = [](const clap_plugin_factory_as_auv2 *, uint32_t index, clap_plugin_info_as_auv2 *info) {
		if (index >= 2) return false;
		std::strncpy(info->au_type, index == 1 ? "aumu" : "aufx", 5);
		std::strncpy(info->au_subt, index == 1 ? "PcIn" : "PcFx", 5);
		return true;
	},
};

const clap_plugin_factory_as_vst3 vst3Factory = {
	.vendor = "Plugin Circuit",
	.vendor_url = "http://127.0.0.1:7790",
	.email_contact = "",
	.get_vst3_info = [](const clap_plugin_factory_as_vst3 *, uint32_t) -> const clap_plugin_info_as_vst3 * { return nullptr; },
};

} // namespace

bool plugincircuit_entry_init(const char *) { return true; }
void plugincircuit_entry_deinit() {}
const void *plugincircuit_entry_get_factory(const char *id) {
	if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &pluginFactory;
	if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_AUV2)) return &auv2Factory;
	if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_VST3)) return &vst3Factory;
	return nullptr;
}
