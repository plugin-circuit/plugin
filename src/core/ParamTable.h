#pragma once
// The single source of truth for a loaded plugin's parameters (docs/MESSAGING.md, "The single source of truth").
//
// Shape (which parameters exist, their info) is fixed when the table is built, on the main thread; a rescan(INFO)
// builds a new table. Values are atomics. The thread that processes (the audio thread, or the main thread inside
// params.flush) writes the values it applies; the main thread writes only when it re-reads the plugin (load, state
// load, rescan(VALUES), the safety net). Readers never call into the plugin for a value.
//
// A changed value sets a bit in a bitset; the main thread takes the bits to push only what changed.
// All memory is allocated in the constructor; set(), indexOf(), value() and takeChanged() don't allocate or lock.
#include "core/InnerHost.h" // ParamInfo

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>

namespace plugincircuit {

// Who is editing a parameter right now (docs/MESSAGING.md, "Gestures and ownership"). The DAW never owns one.
enum class Owner : uint8_t { None, Ui, Plugin };

class ParamTable {
public:
	explicit ParamTable(std::vector<ParamInfo> infos) : infos_(std::move(infos)) {
		const size_t n = infos_.size();
		values_ = std::make_unique<std::atomic<double>[]>(n);
		for (size_t i = 0; i < n; ++i) values_[i].store(infos_[i].defaultValue, std::memory_order_relaxed);
		words_ = (n + 63) / 64;
		changed_ = std::make_unique<std::atomic<uint64_t>[]>(std::max<size_t>(words_, 1));
		for (size_t w = 0; w < words_; ++w) changed_[w].store(0, std::memory_order_relaxed);
		owners_ = std::make_unique<std::atomic<Owner>[]>(std::max<size_t>(n, 1));
		for (size_t i = 0; i < n; ++i) owners_[i].store(Owner::None, std::memory_order_relaxed);
		byId_.reserve(n);
		for (size_t i = 0; i < n; ++i) byId_.push_back({infos_[i].id, uint32_t(i)});
		std::sort(byId_.begin(), byId_.end(), [](const IdIndex &a, const IdIndex &b) { return a.id < b.id; });
	}

	size_t size() const { return infos_.size(); }
	const std::vector<ParamInfo> &infos() const { return infos_; }
	const ParamInfo &info(size_t index) const { return infos_[index]; }

	// -1 if the plugin has no such parameter. Binary search: no allocation, any thread.
	int indexOf(clap_id id) const {
		auto it = std::lower_bound(byId_.begin(), byId_.end(), id, [](const IdIndex &e, clap_id v) { return e.id < v; });
		return it != byId_.end() && it->id == id ? int(it->index) : -1;
	}

	double value(size_t index) const { return values_[index].load(std::memory_order_relaxed); }
	bool valueById(clap_id id, double &out) const {
		const int i = indexOf(id);
		if (i < 0) return false;
		out = value(size_t(i));
		return true;
	}

	// Stores a value and marks it changed if it differs. Real-time safe.
	void set(size_t index, double v) {
		if (index >= infos_.size()) return;
		if (values_[index].exchange(v, std::memory_order_relaxed) == v) return;
		changed_[index / 64].fetch_or(uint64_t(1) << (index % 64), std::memory_order_release);
	}
	void setById(clap_id id, double v) {
		const int i = indexOf(id);
		if (i >= 0) set(size_t(i), v);
	}

	// Written by the thread that processes (the gesture state machine), read by the main thread.
	Owner owner(size_t index) const { return owners_[index].load(std::memory_order_relaxed); }
	void setOwner(size_t index, Owner o) { owners_[index].store(o, std::memory_order_relaxed); }

	// Pushes the value again even though it didn't change (the end of a gesture: the final value). Real-time safe.
	void markChanged(size_t index) {
		if (index < infos_.size()) changed_[index / 64].fetch_or(uint64_t(1) << (index % 64), std::memory_order_release);
	}

	void markAllChanged() {
		for (size_t w = 0; w < words_; ++w) {
			const size_t bits = std::min<size_t>(64, infos_.size() - w * 64);
			changed_[w].fetch_or(bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1), std::memory_order_release);
		}
	}

	// Main thread: calls f(index) for every value changed since the last call, and clears those bits.
	template <class F>
	void takeChanged(F &&f) {
		for (size_t w = 0; w < words_; ++w) {
			uint64_t bits = changed_[w].exchange(0, std::memory_order_acquire);
			while (bits) {
				const int b = std::countr_zero(bits);
				bits &= bits - 1;
				f(w * 64 + size_t(b));
			}
		}
	}

	// Main thread, before this table is published: carry the values of parameters that exist in both.
	void copyValuesFrom(const ParamTable &other) {
		for (size_t i = 0; i < infos_.size(); ++i) {
			double v;
			if (other.valueById(infos_[i].id, v)) values_[i].store(v, std::memory_order_relaxed);
		}
	}

private:
	struct IdIndex {
		clap_id id;
		uint32_t index;
	};
	std::vector<ParamInfo> infos_;
	std::vector<IdIndex> byId_;
	std::unique_ptr<std::atomic<double>[]> values_;
	std::unique_ptr<std::atomic<uint64_t>[]> changed_;
	std::unique_ptr<std::atomic<Owner>[]> owners_;
	size_t words_ = 0;
};

} // namespace plugincircuit
