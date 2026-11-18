#pragma once
// Machine settings: shared by every Plugin Circuit instance on this machine, in Paths::root()/settings.json. Declared
// in src/api/SettingsSchema.h (scope machine). One copy per process: every instance sees a change at once; another
// process's change is picked up by poll(). Main thread (and the loader's first init); never the audio thread.
#include <choc/containers/choc_Value.h>

#include <cstdint>
#include <string>

namespace plugincircuit {

class Settings {
public:
	static choc::value::Value values(); // the machine settings, whole (read from the file once)
	// A change from the page, validated against the schema (clamped, unknown keys refused): saved, applied to the
	// bridge, and the version bumped. "" when done, else the error.
	static std::string set(const choc::value::ValueView &change);
	static uint64_t version(); // changes whenever values() may have
	// Re-reads settings.json if it changed on disk (another process wrote it); at most every `minInterval` seconds.
	static void poll(double minInterval = 0.5);
	static void apply(); // pushes the values the audio thread uses to the bridge
};

} // namespace plugincircuit
