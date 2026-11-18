#pragma once
// One API between the host UI (a page) and the core: stores, commands, streams (docs/MESSAGING.md, "One API").
//
// - Store: state with a current value. The page `store.get`s it (subscribing), then receives patches while
//   subscribed; it changes it with `store.set`, which the owner validates. Each patch carries the store's version,
//   which only grows; the page ignores a patch that isn't newer than what it has.
// - Command: an action with a result, `{ok, result}` or `{ok: false, error}`. What it changes arrives as patches.
// - Stream: high-rate measurements, sampled only while subscribed, at the stream's rate. Latest wins.
//
// One registry per page (an editor has one page). Everything runs on the main thread: `request` when the page calls,
// `tick` from the editor's timer. Data from the audio thread reaches handlers only through lock-free structures.
// Testable without a webview: requests in, replies and messages (through `send`) out.
#include <choc/containers/choc_Value.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace plugincircuit {

class Api {
public:
	using Value = choc::value::Value;
	using View = choc::value::ValueView;
	static constexpr int kProtocol = 1;

	// Combines values into a store's stamp. A full 64-bit mix: an XOR-and-multiply step (FNV) let small counters
	// cancel out (version 10 + status 1 stamped the same as version 15 + status 4), so a change was never sent.
	static uint64_t mix(uint64_t h, uint64_t v) {
		uint64_t x = h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull; // splitmix64's finalizer
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
		return x ^ (x >> 31);
	}

	// A store read whole. `stamp` is cheap and changes whenever the value may have: a patch (the whole value) goes
	// out when it does. `set` validates and applies a change: "" when done, else the error.
	struct Store {
		std::function<Value()> get;
		std::function<uint64_t()> stamp;
		std::function<std::string(const View &)> set;
	};
	// A store with its own deltas (a large one: the parameters). `get` is the whole value and starts the deltas over;
	// `poll` returns what changed since the last get or poll, or nothing.
	struct DeltaStore {
		std::function<Value()> get;
		std::function<std::optional<Value>()> poll;
		std::function<std::string(const View &)> set;
	};
	// result, or error set
	using Command = std::function<Value(const View &args, std::string &error)>;

	void store(const std::string &name, Store s);
	void deltaStore(const std::string &name, DeltaStore s);
	void command(const std::string &name, Command c);
	void stream(const std::string &name, std::function<Value()> sample, double hz);

	// The page's one call. `name`: "hello", "store.get" / "store.set" / "store.unsubscribe" ({store, value}),
	// "stream.subscribe" / "stream.unsubscribe" ({stream}), or a command. Returns {ok, result} or {ok: false, error}.
	Value request(const std::string &name, const View &args);
	// Main thread, regularly: patches of subscribed stores that changed, samples of subscribed streams that are due.
	void tick(double nowSeconds);
	// To the page: {kind: "patch" | "stream", topic, version, data}
	std::function<void(const Value &)> send;

	bool has(const std::string &name) const;

private:
	struct StoreEntry {
		Store plain;
		DeltaStore delta;
		bool isDelta = false;
		bool subscribed = false;
		uint64_t version = 0; // of the last value the page got
		uint64_t lastStamp = 0;
	};
	struct StreamEntry {
		std::function<Value()> sample;
		double period = 0.1;
		bool subscribed = false;
		double due = 0;
		uint64_t version = 0;
	};
	Value storeGet(StoreEntry &s);
	void patch(const std::string &name, StoreEntry &s, Value data, bool full);

	std::map<std::string, StoreEntry> stores_;
	std::map<std::string, Command> commands_;
	std::map<std::string, StreamEntry> streams_;
};

} // namespace plugincircuit
