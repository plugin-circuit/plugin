#include "api/Api.h"

namespace plugincircuit {

namespace {
Api::Value ok(Api::Value result) {
	auto r = choc::value::createObject("Reply", "ok", true);
	r.addMember("result", std::move(result));
	return r;
}
Api::Value fail(const std::string &error) { return choc::value::createObject("Reply", "ok", false, "error", error); }
std::string member(const Api::View &v, const char *key) {
	return v.isObject() && v.hasObjectMember(key) && v[key].isString() ? std::string(v[key].getString()) : std::string();
}
} // namespace

void Api::store(const std::string &name, Store s) {
	auto &e = stores_[name];
	e.plain = std::move(s);
	e.isDelta = false;
}

void Api::deltaStore(const std::string &name, DeltaStore s) {
	auto &e = stores_[name];
	e.delta = std::move(s);
	e.isDelta = true;
}

void Api::command(const std::string &name, Command c) { commands_[name] = std::move(c); }

void Api::stream(const std::string &name, std::function<Value()> sample, double hz) {
	auto &e = streams_[name];
	e.sample = std::move(sample);
	e.period = hz > 0 ? 1.0 / hz : 0.1;
}

bool Api::has(const std::string &name) const { return stores_.count(name) || commands_.count(name) || streams_.count(name); }

Api::Value Api::storeGet(StoreEntry &s) {
	if (s.isDelta) return s.delta.get();
	s.lastStamp = s.plain.stamp ? s.plain.stamp() : 0;
	return s.plain.get();
}

void Api::patch(const std::string &name, StoreEntry &s, Value data, bool full) {
	++s.version;
	if (!send) return;
	auto m = choc::value::createObject("Message", "kind", "patch", "topic", name, "version", (int64_t)s.version, "full", full);
	m.addMember("data", std::move(data));
	send(m);
}

Api::Value Api::request(const std::string &name, const View &args) {
	if (name == "hello") { // the page tells its protocol; a mismatch is the page's to show
		const int64_t theirs = args.isObject() && args.hasObjectMember("protocol") && args["protocol"].isInt()
		                           ? args["protocol"].getWithDefault<int64_t>(-1) : -1;
		if (theirs != kProtocol)
			return fail("protocol mismatch: the page speaks " + std::to_string(theirs) + ", the plugin " + std::to_string(kProtocol));
		for (auto &[n, s] : stores_) s.subscribed = false; // a (re)loaded page starts over
		for (auto &[n, s] : streams_) s.subscribed = false;
		return ok(choc::value::createObject("Hello", "protocol", kProtocol));
	}
	if (name == "store.get" || name == "store.set" || name == "store.unsubscribe") {
		const auto storeName = member(args, "store");
		auto it = stores_.find(storeName);
		if (it == stores_.end()) return fail("no store '" + storeName + "'");
		auto &s = it->second;
		if (name == "store.unsubscribe") {
			s.subscribed = false;
			return ok(Value(true));
		}
		if (name == "store.set") {
			auto &setter = s.isDelta ? s.delta.set : s.plain.set;
			if (!setter) return fail("store '" + storeName + "' can't be set");
			if (!args.hasObjectMember("value")) return fail("store.set needs a value");
			if (auto error = setter(args["value"]); !error.empty()) return fail(error);
			// the change arrives as a patch with a newer version than this; one up to this version predates the set
			return ok(choc::value::createObject("Set", "version", (int64_t)s.version));
		}
		// get: the whole value, a new version, and patches from here on
		s.subscribed = true;
		++s.version;
		auto r = choc::value::createObject("Got", "version", (int64_t)s.version);
		r.addMember("value", storeGet(s));
		return ok(std::move(r));
	}
	if (name == "stream.subscribe" || name == "stream.unsubscribe") {
		const auto streamName = member(args, "stream");
		auto it = streams_.find(streamName);
		if (it == streams_.end()) return fail("no stream '" + streamName + "'");
		it->second.subscribed = name == "stream.subscribe";
		it->second.due = 0; // first sample on the next tick
		return ok(Value(true));
	}
	if (auto it = commands_.find(name); it != commands_.end()) {
		std::string error;
		auto result = it->second(args, error);
		return error.empty() ? ok(std::move(result)) : fail(error);
	}
	return fail("unknown request '" + name + "'");
}

void Api::tick(double now) {
	for (auto &[name, s] : stores_) {
		if (!s.subscribed) continue;
		if (s.isDelta) {
			if (auto d = s.delta.poll()) patch(name, s, std::move(*d), false);
		} else if (s.plain.stamp) {
			const auto stamp = s.plain.stamp();
			if (stamp == s.lastStamp) continue;
			s.lastStamp = stamp;
			patch(name, s, s.plain.get(), true);
		}
	}
	for (auto &[name, st] : streams_) {
		if (!st.subscribed || now < st.due) continue;
		st.due = now + st.period;
		++st.version;
		if (!send) continue;
		auto m = choc::value::createObject("Message", "kind", "stream", "topic", name, "version", (int64_t)st.version);
		m.addMember("data", st.sample());
		send(m);
	}
}

} // namespace plugincircuit
