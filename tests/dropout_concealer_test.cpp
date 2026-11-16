// Unit tests for wclap-bridge's DropoutConcealer (source/dropout-concealer.h): synthetic signals in 128-frame blocks,
// some blocks skipped. Measures the clicks at the gap's edges (the largest sample-to-sample step) against the
// signal's own largest step, for the concealer and for hard silence (what a skipped block gave before).
#include "dropout-concealer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using wclap_bridge::DropoutConcealer;

namespace {
constexpr double kRate = 48000;
constexpr uint32_t kBlock = 128;
int failures = 0;

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);                                                \
			std::printf(__VA_ARGS__);                                                                                  \
			std::printf("\n");                                                                                         \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

struct Run {
	std::vector<float> out;
	double maxStep = 0, signalMaxStep = 0, peak = 0;
};

// Renders `blocks` blocks of `signal`, skipping [skipFrom, skipFrom + skipCount), and again every `repeatEvery`
// blocks after that if non-zero. conceal=false: hard silence.
Run render(const std::function<double(uint64_t)> &signal, uint32_t blocks, uint32_t skipFrom, uint32_t skipCount, bool conceal,
           uint32_t repeatEvery = 0) {
	DropoutConcealer c;
	c.prepare(1, kRate);
	Run r;
	r.out.resize(size_t(blocks) * kBlock);
	for (uint32_t b = 0; b < blocks; ++b) {
		float *block = r.out.data() + size_t(b) * kBlock;
		const uint32_t rel = b >= skipFrom && repeatEvery ? (b - skipFrom) % repeatEvery + skipFrom : b;
		const bool skipped = rel >= skipFrom && rel < skipFrom + skipCount;
		if (skipped) {
			if (conceal) c.conceal(0, block, kBlock);
			else std::fill_n(block, kBlock, 0.f);
		} else {
			for (uint32_t i = 0; i < kBlock; ++i) block[i] = float(signal(uint64_t(b) * kBlock + i));
			if (conceal) c.played(0, block, kBlock);
		}
		if (conceal) c.endBlock(skipped, kBlock);
	}
	for (size_t i = 1; i < r.out.size(); ++i) r.maxStep = std::max(r.maxStep, double(std::fabs(r.out[i] - r.out[i - 1])));
	for (uint64_t i = 1; i < r.out.size(); ++i) r.signalMaxStep = std::max(r.signalMaxStep, std::fabs(signal(i) - signal(i - 1)));
	for (float s : r.out) r.peak = std::max(r.peak, double(std::fabs(s)));
	return r;
}

void report(const char *name, uint32_t skipCount, const Run &concealed, const Run &silence) {
	std::printf("%-22s gap %2u blocks: largest step %.3f concealed vs %.3f silence (signal's own %.3f)\n", name, skipCount,
	            concealed.maxStep, silence.maxStep, concealed.signalMaxStep);
}
} // namespace

int main() {
	const double tau = 2 * 3.14159265358979323846;
	auto sine = [&](uint64_t n) { return 0.5 * std::sin(tau * 220.0 * n / kRate); };
	auto chord = [&](uint64_t n) {
		return 0.25 * (std::sin(tau * 220.0 * n / kRate) + std::sin(tau * 277.18 * n / kRate) + std::sin(tau * 329.63 * n / kRate));
	};
	auto saw = [&](uint64_t n) { // band-limited saw, 110 Hz, 20 harmonics
		double s = 0;
		for (int h = 1; h <= 20; ++h) s += std::sin(tau * 110.0 * h * n / kRate) / h;
		return 0.3 * s;
	};

	// tonal material, gaps of 1, 3 and 15 blocks (2.7 ms, 8 ms, 40 ms: longer than the fade)
	for (auto [name, signal] : std::vector<std::pair<const char *, std::function<double(uint64_t)>>>{{"sine 220 Hz", sine}, {"chord A minor", chord}, {"saw 110 Hz", saw}}) {
		for (uint32_t gap : {1u, 3u, 15u}) {
			auto concealed = render(signal, 60, 20, gap, true);
			auto silence = render(signal, 60, 20, gap, false);
			report(name, gap, concealed, silence);
			// no click: the largest step stays near the signal's own (silence jumps by up to the amplitude)
			CHECK(concealed.maxStep <= 2.0 * concealed.signalMaxStep + 0.01, "step %.3f vs signal %.3f", concealed.maxStep, concealed.signalMaxStep);
			CHECK(concealed.maxStep < silence.maxStep, "concealed %.3f not better than silence %.3f", concealed.maxStep, silence.maxStep);
			CHECK(concealed.peak <= 1.2 * render(signal, 60, 1000, 0, false).peak + 1e-6, "peak grew to %.3f", concealed.peak);
		}
	}

	// many gaps in a row (each one starts its own fade: a later gap must not begin with silence)
	for (uint32_t gap : {1u, 3u, 15u}) {
		auto concealed = render(saw, 400, 20, gap, true, 30);
		auto silence = render(saw, 400, 20, gap, false, 30);
		report("saw, repeated gaps", gap, concealed, silence);
		CHECK(concealed.maxStep <= 2.0 * concealed.signalMaxStep + 0.01, "repeated gaps: step %.3f vs signal %.3f", concealed.maxStep, concealed.signalMaxStep);
	}

	// the first concealed samples continue a sine closely (it's what the model is for)
	{
		auto r = render(sine, 40, 20, 1, true);
		double err = 0;
		for (uint32_t i = 0; i < 48; ++i) err = std::max(err, std::fabs(r.out[20 * kBlock + i] - sine(20 * kBlock + i)));
		std::printf("sine: largest error over the first 1 ms of the gap %.4f\n", err);
		CHECK(err < 0.02, "prediction error %.4f", err);
	}

	// noise: no blow-up, stays within the input's level
	{
		std::mt19937 rng(1);
		std::vector<double> noise(60 * kBlock);
		std::normal_distribution<double> nd(0, 0.2);
		for (auto &x : noise) x = std::clamp(nd(rng), -1.0, 1.0);
		auto signal = [&](uint64_t n) { return noise[n]; };
		auto r = render(signal, 60, 20, 3, true);
		std::printf("noise: peak %.3f (input peak ~%.3f)\n", r.peak, render(signal, 60, 1000, 0, false).peak);
		CHECK(r.peak <= 1.2 * render(signal, 60, 1000, 0, false).peak, "noise peak grew to %.3f", r.peak);
	}

	// silence stays silent
	{
		auto r = render([](uint64_t) { return 0.0; }, 40, 20, 3, true);
		CHECK(r.peak == 0, "silence produced %.6f", r.peak);
	}

	// a long gap fades to silence: the last concealed block of a 15-block (40 ms) gap is silent
	{
		auto r = render(sine, 60, 20, 15, true);
		double tail = 0;
		for (uint32_t i = 0; i < kBlock; ++i) tail = std::max(tail, double(std::fabs(r.out[(20 + 14) * kBlock + i])));
		CHECK(tail == 0, "still %.4f after the fade", tail);
	}

	// cost of the model fit (on the audio thread, once per gap): 2 channels, as for a stereo plugin
	{
		DropoutConcealer c;
		c.prepare(2, kRate);
		std::vector<float> l(kBlock), r(kBlock);
		for (int b = 0; b < 20; ++b) {
			for (uint32_t i = 0; i < kBlock; ++i) l[i] = r[i] = float(saw(uint64_t(b) * kBlock + i));
			c.played(0, l.data(), kBlock);
			c.played(1, r.data(), kBlock);
			c.endBlock(false, kBlock);
		}
		const auto t0 = std::chrono::steady_clock::now();
		c.conceal(0, l.data(), kBlock);
		c.conceal(1, r.data(), kBlock);
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		std::printf("first concealed block, 2 channels (fit + %u samples each): %.0f us (block %.0f us)\n", kBlock, us, 1e6 * kBlock / kRate);
	}

	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("dropout concealer: all checks passed\n");
	return failures ? 1 : 0;
}
