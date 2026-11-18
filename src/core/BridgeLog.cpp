#include "core/BridgeLog.h"


#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace plugincircuit::bridgelog {

namespace {
constexpr size_t kSlots = 64, kText = 480;
struct Slot {
	std::atomic<uint64_t> seq{0}; // 2n + 1 while message n is written, 2n + 2 once it is complete
	char text[kText];
};
std::array<Slot, kSlots> slots;
std::atomic<uint64_t> next{0};

void write(const char *message) { // any thread, the audio thread included: a claimed slot, a bounded copy
	const uint64_t n = next.fetch_add(1, std::memory_order_relaxed);
	auto &s = slots[n % kSlots];
	s.seq.store(2 * n + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	size_t len = 0;
	while (message && message[len] && len < kText - 1) ++len;
	std::memcpy(s.text, message ? message : "", len);
	s.text[len] = 0;
	s.seq.store(2 * n + 2, std::memory_order_release);
}

void sink(void *, const char *message) { write(message); }
} // namespace

void install() {
	// The pinned bridge has no log API yet (its diagnostics go to stderr); `sink` is what it will call once it has
	// one (upstream's, not a fork change): the ring and its readers are ready for it.
	(void)&sink;
}

uint64_t end() { return next.load(std::memory_order_acquire); }

bool read(uint64_t cursor, std::string &message) {
	auto &s = slots[cursor % kSlots];
	if (s.seq.load(std::memory_order_acquire) != 2 * cursor + 2) return false;
	char copy[kText];
	std::memcpy(copy, s.text, kText); // a seqlock read: checked below, discarded if a writer came in between
	std::atomic_thread_fence(std::memory_order_acquire);
	if (s.seq.load(std::memory_order_relaxed) != 2 * cursor + 2) return false;
	copy[kText - 1] = 0;
	message = copy;
	if (std::getenv("PLUGINCIRCUIT_DEBUG_BRIDGE")) std::fprintf(stderr, "[wclap-bridge] %s\n", copy);
	return true;
}

void writeForTest(const char *message) { write(message); }

} // namespace plugincircuit::bridgelog
