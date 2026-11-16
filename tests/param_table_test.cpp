// Unit tests for the parameter table (src/core/ParamTable.h), the single source of truth for parameter values.
#include "core/ParamTable.h"

#include <atomic>
#include <cstdio>
#include <set>
#include <thread>
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

static std::vector<ParamInfo> makeInfos(size_t n, clap_id firstId = 1000, clap_id step = 7) {
	std::vector<ParamInfo> v;
	for (size_t i = 0; i < n; ++i) {
		ParamInfo p;
		p.id = firstId + clap_id(i) * step; // sparse, unsorted below
		p.defaultValue = 0.25;
		v.push_back(p);
	}
	std::swap(v.front(), v.back()); // the table must not assume sorted ids
	return v;
}

int main() {
	// shape, defaults, id lookup (sparse, unsorted ids)
	{
		ParamTable t(makeInfos(130));
		CHECK(t.size() == 130);
		for (size_t i = 0; i < t.size(); ++i) {
			CHECK(t.indexOf(t.info(i).id) == int(i));
			CHECK(t.value(i) == 0.25);
		}
		CHECK(t.indexOf(5) == -1);
		CHECK(t.indexOf(1001) == -1); // between two ids
		double v = 0;
		CHECK(!t.valueById(5, v));
	}

	// set marks changed only when the value changes; takeChanged reports each once and clears
	{
		ParamTable t(makeInfos(130));
		t.set(3, 0.25); // same value: not a change
		t.set(64, 0.5);
		t.set(129, 0.75);
		t.setById(t.info(70).id, 0.1);
		t.setById(5, 0.9); // unknown id: ignored
		std::vector<size_t> got;
		t.takeChanged([&](size_t i) { got.push_back(i); });
		CHECK((got == std::vector<size_t>{64, 70, 129}));
		got.clear();
		t.takeChanged([&](size_t i) { got.push_back(i); });
		CHECK(got.empty());
		CHECK(t.value(70) == 0.1);
		t.markAllChanged();
		size_t count = 0;
		t.takeChanged([&](size_t) { ++count; });
		CHECK(count == 130); // exactly the parameters, not the bitset's padding
	}

	// a new shape keeps the values of parameters that still exist
	{
		ParamTable a(makeInfos(10));
		a.set(2, 0.9);
		auto infos = makeInfos(12); // the same ids plus two more
		ParamTable b(infos);
		b.copyValuesFrom(a);
		CHECK(b.value(size_t(b.indexOf(a.info(2).id))) == 0.9);
		CHECK(b.value(10) == 0.25 || b.info(10).id == a.info(2).id);
	}

	// a writer (the audio thread) and a reader (the main thread taking the changed bits) at once: no change is lost.
	// The writer sets each parameter to a sequence of values; the reader must end up having seen every parameter's
	// last value, and must see a change after the last write (nothing swallowed by a concurrent clear).
	{
		constexpr size_t N = 1000;
		ParamTable t(makeInfos(N));
		std::atomic<bool> done{false};
		std::vector<double> lastSeen(N, 0.25);
		std::thread writer([&] {
			for (int round = 1; round <= 200; ++round)
				for (size_t i = 0; i < N; ++i) t.set(i, double(round) + double(i) * 1e-6);
			done = true;
		});
		while (!done) t.takeChanged([&](size_t i) { lastSeen[i] = t.value(i); });
		writer.join();
		t.takeChanged([&](size_t i) { lastSeen[i] = t.value(i); }); // what the last round left
		size_t wrong = 0;
		for (size_t i = 0; i < N; ++i)
			if (lastSeen[i] != 200.0 + double(i) * 1e-6) ++wrong;
		std::printf("concurrent writer/reader: %zu of %zu final values missed\n", wrong, N);
		CHECK(wrong == 0);
	}

	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("param table: all checks passed\n");
	return failures ? 1 : 0;
}
