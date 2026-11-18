// Settings (docs/PLAN.md, phase 4): the schema's validation, instance settings in our state (and a state from before
// them), and machine settings shared in a process and picked up from another process's write.
#include "api/SettingsSchema.h"
#include "core/Settings.h"
#include "core/Util.h"
#include "plugin/StateCodec.h"

#include <choc/text/choc_JSON.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace plugincircuit;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

static choc::value::Value json(const std::string &s) { return choc::json::parse(s); }
static double num(const choc::value::ValueView &v, const char *k) { return v[k].getWithDefault<double>(-1); }
static std::string str(const choc::value::ValueView &v, const char *k) { return v[k].getWithDefault<std::string>(""); }

static void writeFile(const fs::path &p, const std::string &text) {
	auto tmp = p;
	tmp += ".other";
	std::ofstream(tmp) << text;
	fs::rename(tmp, p); // as another process would (and as we do)
}
static std::string readFile(const fs::path &p) {
	std::ifstream f(p);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

int main() {
	// ---- schema: clamping, snapping, refusals, all or nothing
	{
		auto v = settings::defaults(Scope::Machine);
		CHECK(num(v, "audioWaitRatio") == 0.2 && v["concealDropouts"].getBool());
		CHECK(!v.hasObjectMember("view")); // instance scope stays out
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": 1.5})"), v).empty() && num(v, "audioWaitRatio") == 0.9);
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": -3})"), v).empty() && num(v, "audioWaitRatio") == 0);
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": 0.33})"), v).empty() && std::abs(num(v, "audioWaitRatio") - 0.3) < 1e-9);
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": 0.7})"), v).empty() && num(v, "audioWaitRatio") == 0.7);
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": 0.33})"), v).empty());
		CHECK(settings::apply(Scope::Machine, json(R"({"bogus": 1})"), v) == "unknown setting 'bogus'");
		CHECK(settings::apply(Scope::Machine, json(R"({"audioWaitRatio": "much"})"), v) == "audioWaitRatio must be a number");
		CHECK(settings::apply(Scope::Machine, json(R"({"concealDropouts": 1})"), v) == "concealDropouts must be true or false");
		CHECK(settings::apply(Scope::Machine, json(R"({"view": "params"})"), v) == "view is an instance setting");
		CHECK(!settings::apply(Scope::Machine, json(R"({"audioWaitRatio": 0.5, "concealDropouts": "no"})"), v).empty());
		CHECK(std::abs(num(v, "audioWaitRatio") - 0.3) < 1e-9); // the refused change applied nothing
		auto i = settings::defaults(Scope::Instance);
		CHECK(str(i, "view") == "plugin" && num(i, "keyboardBase") == 48);
		CHECK(settings::apply(Scope::Instance, json(R"({"view": "settings"})"), i).rfind("view must be one of", 0) == 0);
		CHECK(settings::apply(Scope::Instance, json(R"({"keyboardBase": 61})"), i).empty() && num(i, "keyboardBase") == 60);
		// stored values: wrong types default, unknown keys dropped, out of range clamped
		auto s = settings::sanitize(Scope::Machine, json(R"({"audioWaitRatio": 7, "concealDropouts": "yes", "fromTheFuture": 1})"));
		CHECK(num(s, "audioWaitRatio") == 0.9 && s["concealDropouts"].getBool() && !s.hasObjectMember("fromTheFuture"));
		CHECK(settings::describe(Scope::Machine).size() == 2 && settings::describe(Scope::Instance).size() == 0);
	}

	// ---- instance settings in our state; a version-1 state loads with defaults; an unknown version is refused
	{
		HostState st;
		st.instance = json(R"({"view": "params", "keyboardBase": 60})");
		std::string error;
		auto back = decodeState(encodeState(st), error);
		CHECK(back && str(back->instance, "view") == "params" && num(back->instance, "keyboardBase") == 60);

		// a version-1 state, as the previous release wrote it: no "instance"
		const std::string v1json = R"({"pluginId":"","autoMapped":true,"macros":[],"innerParams":[]})";
		std::vector<uint8_t> v1 = {'P', 'L', 'C', 'I', 1, 0, 0, 0};
		const uint32_t n = uint32_t(v1json.size());
		for (int b = 0; b < 4; ++b) v1.push_back(uint8_t(n >> (8 * b)));
		v1.insert(v1.end(), v1json.begin(), v1json.end());
		v1.insert(v1.end(), {0, 0, 0, 0});
		auto old = decodeState(v1, error);
		CHECK(old && str(old->instance, "view") == "plugin" && num(old->instance, "keyboardBase") == 48);

		auto future = encodeState(st);
		future[4] = 9;
		CHECK(!decodeState(future, error) && error == "unsupported state version");

		HostState bad; // a hand-edited state: sanitized
		bad.instance = json(R"({"view": "nowhere", "keyboardBase": 1000})");
		auto fixed = decodeState(encodeState(bad), error);
		CHECK(fixed && str(fixed->instance, "view") == "plugin" && num(fixed->instance, "keyboardBase") == 84);
	}

	// ---- machine settings: one copy per process; another process's write is picked up
	{
		auto home = fs::temp_directory_path() / ("plugincircuit-settings-test-" + std::to_string(::getpid()));
		fs::remove_all(home);
		fs::create_directories(home);
		setenv("PLUGINCIRCUIT_HOME", home.c_str(), 1);
		const auto file = home / "settings.json";
		writeFile(file, R"({"audioWaitRatio": 0.4, "fromTheFuture": "keep me"})");

		CHECK(num(Settings::values(), "audioWaitRatio") == 0.4);
		const auto v0 = Settings::version();
		CHECK(Settings::set(json(R"({"audioWaitRatio": 2})")).empty()); // clamped
		CHECK(Settings::version() > v0);
		CHECK(num(Settings::values(), "audioWaitRatio") == 0.9); // every instance reads this one copy
		auto onDisk = json(readFile(file));
		CHECK(num(onDisk, "audioWaitRatio") == 0.9 && str(onDisk, "fromTheFuture") == "keep me"); // a newer version's key kept
		CHECK(!Settings::set(json(R"({"nope": true})")).empty());

		const auto v1 = Settings::version();
		Settings::poll(0);
		CHECK(Settings::version() == v1); // our own write isn't news

		writeFile(file, R"({"audioWaitRatio": 0.1, "concealDropouts": false})"); // "another process"
		fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2)); // a coarse clock can't hide it
		Settings::poll(0);
		CHECK(Settings::version() > v1);
		CHECK(std::abs(num(Settings::values(), "audioWaitRatio") - 0.1) < 1e-9 && !Settings::values()["concealDropouts"].getBool());

		const auto v2 = Settings::version();
		writeFile(file, R"({"audioWaitRatio": 0.1, "concealDropouts": false, "x": 1})"); // the same settings
		Settings::poll(0);
		CHECK(Settings::version() == v2);
		fs::remove_all(home);
	}

	std::printf(failures ? "%d failure(s)\n" : "PASS: settings\n", failures);
	return failures ? 1 : 0;
}
