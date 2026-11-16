#include "core/Settings.h"

#include "core/Util.h"

#include <choc/text/choc_JSON.h>
#include <wclap-bridge.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace plugincircuit {

namespace {
fs::path file() { return Paths::root() / "settings.json"; }
float clampRatio(double r) { return float(std::clamp(r, 0.0, 0.9)); }
} // namespace

Settings Settings::load() {
	Settings s;
	std::ifstream f(file());
	if (!f) return s;
	std::stringstream ss;
	ss << f.rdbuf();
	try {
		auto v = choc::json::parse(ss.str());
		if (v.hasObjectMember("audioWaitRatio")) s.audioWaitRatio = clampRatio(v["audioWaitRatio"].getWithDefault<double>(kDefaultAudioWaitRatio));
		if (v.hasObjectMember("concealDropouts") && v["concealDropouts"].isBool()) s.concealDropouts = v["concealDropouts"].getBool();
	} catch (...) {
	}
	return s;
}

void Settings::save() const {
	std::error_code ec;
	fs::create_directories(Paths::root(), ec);
	auto tmp = file();
	tmp += ".tmp";
	{
		std::ofstream f(tmp);
		f << choc::json::toString(choc::value::createObject("Settings", "audioWaitRatio", double(audioWaitRatio), "concealDropouts", concealDropouts), true);
	}
	fs::rename(tmp, file(), ec); // atomic: other instances may read it
}

void Settings::apply() const {
	wclap_set_audio_wait_ratio(clampRatio(audioWaitRatio));
	wclap_set_conceal_dropouts(concealDropouts);
}

} // namespace plugincircuit
