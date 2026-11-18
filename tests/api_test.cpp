// The API registry (src/api/Api.h) without a webview: requests in, replies and messages out.
#include "api/Api.h"

#include <choc/text/choc_JSON.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace plugincircuit;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

static Api::Value args(const std::string &json) { return choc::json::parse(json); }

int main() {
	// stamps: small counters must not cancel out (the plugin store's version + status stayed "fetching" once)
	CHECK(Api::mix(Api::mix(10, 1), 0) != Api::mix(Api::mix(15, 4), 0));
	CHECK(Api::mix(3, 4) != Api::mix(4, 3));

	Api api;
	std::vector<Api::Value> sent;
	api.send = [&](const Api::Value &m) { sent.push_back(m); };

	// a plain store: a counter with a stamp, settable within 0..10
	int counter = 1;
	uint64_t counterStamp = 1;
	api.store("counter", {[&] { return choc::value::createObject("C", "n", counter); }, [&] { return counterStamp; },
	                      [&](const Api::View &v) -> std::string {
		                      if (!v.isInt() || v.getWithDefault<int64_t>(0) < 0 || v.getWithDefault<int64_t>(0) > 10) return "n must be 0..10";
		                      counter = int(v.getWithDefault<int64_t>(0));
		                      ++counterStamp;
		                      return {};
	                      }});
	// a delta store: a list whose changes are reported once
	std::vector<int> changes;
	api.deltaStore("list", {[&] {
		                        changes.clear();
		                        return choc::value::createObject("L", "all", true);
	                        },
	                        [&]() -> std::optional<Api::Value> {
		                        if (changes.empty()) return std::nullopt;
		                        auto a = choc::value::createEmptyArray();
		                        for (int c : changes) a.addArrayElement(c);
		                        changes.clear();
		                        return a;
	                        },
	                        {}});
	api.command("echo", [](const Api::View &a, std::string &error) -> Api::Value {
		if (!a.isObject() || !a.hasObjectMember("text")) {
			error = "echo needs text";
			return {};
		}
		return Api::Value(std::string(a["text"].getString()));
	});
	int samples = 0;
	api.stream("perf", [&] { return Api::Value(++samples); }, 10);

	// hello: protocol checked
	CHECK(!api.request("hello", args(R"({"protocol": 999})"))["ok"].getBool());
	CHECK(api.request("hello", args(R"({"protocol": 1})"))["ok"].getBool());

	// get: value and version; no patch while nothing changes
	auto got = api.request("store.get", args(R"({"store": "counter"})"));
	CHECK(got["ok"].getBool() && got["result"]["value"]["n"].getWithDefault<int64_t>(0) == 1);
	const int64_t v0 = got["result"]["version"].getWithDefault<int64_t>(0);
	api.tick(0);
	CHECK(sent.empty());

	// set: validated; a valid one arrives as a patch with a newer version, once
	auto bad = api.request("store.set", args(R"({"store": "counter", "value": 42})"));
	CHECK(!bad["ok"].getBool() && std::string(bad["error"].getString()) == "n must be 0..10");
	CHECK(api.request("store.set", args(R"({"store": "counter", "value": 7})"))["ok"].getBool());
	api.tick(0);
	CHECK(sent.size() == 1);
	if (sent.size() == 1) {
		CHECK(std::string(sent[0]["kind"].getString()) == "patch" && std::string(sent[0]["topic"].getString()) == "counter");
		CHECK(sent[0]["version"].getWithDefault<int64_t>(0) > v0 && sent[0]["data"]["n"].getWithDefault<int64_t>(0) == 7);
	}
	api.tick(0);
	CHECK(sent.size() == 1);

	// unsubscribed: no patches
	sent.clear();
	api.request("store.unsubscribe", args(R"({"store": "counter"})"));
	++counterStamp;
	api.tick(0);
	CHECK(sent.empty());

	// a delta store patches only after get, with only what changed
	changes = {1, 2};
	api.tick(0);
	CHECK(sent.empty()); // not subscribed: the changes wait
	api.request("store.get", args(R"({"store": "list"})")); // get starts over
	changes = {3};
	api.tick(0);
	CHECK(sent.size() == 1 && sent[0]["data"].size() == 1 && sent[0]["data"][0].getWithDefault<int64_t>(0) == 3);
	sent.clear();

	// versions only grow, also across get and patch
	auto g1 = api.request("store.get", args(R"({"store": "list"})"))["result"]["version"].getWithDefault<int64_t>(0);
	changes = {4};
	api.tick(0);
	CHECK(sent.size() == 1 && sent[0]["version"].getWithDefault<int64_t>(0) > g1);
	sent.clear();

	// commands: result or error; unknown names answered with an error
	auto echo = api.request("echo", args(R"({"text": "hi"})"));
	CHECK(echo["ok"].getBool() && std::string(echo["result"].getString()) == "hi");
	CHECK(!api.request("echo", args("{}"))["ok"].getBool());
	CHECK(!api.request("nope", args("{}"))["ok"].getBool());
	CHECK(!api.request("store.get", args(R"({"store": "nope"})"))["ok"].getBool());
	CHECK(!api.request("store.set", args(R"({"store": "list", "value": 1})"))["ok"].getBool()); // not settable

	// streams: only while subscribed, at their rate
	api.tick(0);
	CHECK(sent.empty());
	api.request("stream.subscribe", args(R"({"stream": "perf"})"));
	api.tick(1.00);
	api.tick(1.05); // not due yet (10 Hz)
	api.tick(1.11);
	CHECK(sent.size() == 2 && std::string(sent[0]["kind"].getString()) == "stream");
	api.request("stream.unsubscribe", args(R"({"stream": "perf"})"));
	api.tick(5);
	CHECK(sent.size() == 2);

	// a new hello (the page reloaded) drops every subscription
	sent.clear();
	api.request("store.get", args(R"({"store": "counter"})"));
	api.request("hello", args(R"({"protocol": 1})"));
	++counterStamp;
	api.tick(10);
	CHECK(sent.empty());

	std::printf(failures ? "%d failure(s)\n" : "api: all checks passed\n", failures);
	return failures ? 1 : 0;
}
