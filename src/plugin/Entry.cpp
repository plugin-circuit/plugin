// clap_entry for every format clap-wrapper builds (CLAP, VST3, AUv2, standalone).
#include <clap/clap.h>

bool plugincircuit_entry_init(const char *);
void plugincircuit_entry_deinit();
const void *plugincircuit_entry_get_factory(const char *);

extern "C" {
CLAP_EXPORT const clap_plugin_entry clap_entry = {
	.clap_version = CLAP_VERSION_INIT,
	.init = plugincircuit_entry_init,
	.deinit = plugincircuit_entry_deinit,
	.get_factory = plugincircuit_entry_get_factory,
};
}
