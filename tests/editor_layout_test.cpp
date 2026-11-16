// Unit tests for the editor's sizing rules (src/gui/EditorLayout.h).
#include "gui/EditorLayout.h"

#include <cstdio>

using namespace plugincircuit::layout;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

static bool inside(Frame f, Frame area) { return f.x >= 0 && f.y >= 0 && f.x + f.w <= area.w && f.y + f.h <= area.h; }

int main() {
	constexpr uint32_t header = 56, kb = 112;

	// the inner area sits under the header and above the keyboard
	{
		auto a = innerArea({1000, 700}, header, kb);
		CHECK(a.y == header && a.w == 1000 && a.h == 700 - header - kb);
		CHECK(innerArea({1000, 100}, header, kb).h == 0); // never negative
	}

	// the editor for an inner GUI shows it 1:1, with the header and keyboard added
	{
		auto s = editorForInner({904, 569}, header, 0);
		CHECK(s.w == 904 && s.h == 569 + header);
		auto k = editorForInner({904, 569}, header, kb);
		CHECK(k.h == 569 + header + kb);
		auto n = editorForInner({300, 200}, header, 0); // narrow GUI: room for the header, min height
		CHECK(n.w == kMinWidthWithGui && n.h == 200 + header);
		// and that size gives the GUI exactly its size
		auto a = innerArea(s, header, 0);
		CHECK(a.w >= 904 && a.h == 569);
	}

	// host sizes are clamped to the minimum
	{
		CHECK(clampEditor({100, 100}) == (Size{kMinWidth, kMinHeight}));
		CHECK(clampEditor({1200, 900}) == (Size{1200, 900}));
	}

	// a scalable (webview) GUI always fits the area, keeps its aspect ratio, and is centred
	{
		const Size natural{904, 569};
		for (Size editor : {Size{1000, 680}, Size{640, 420}, Size{2000, 700}, Size{904, 625}, Size{3000, 2000}}) {
			for (uint32_t k : {0u, kb}) {
				auto area = innerArea(editor, header, k);
				auto f = placeInner(area, natural, natural, true);
				CHECK(inside(f, area));
				CHECK(f.zoom >= kMinZoom && f.zoom <= kMaxZoom);
				const double aspect = double(natural.w) / natural.h, got = f.w / f.h;
				CHECK(got > aspect * 0.99 && got < aspect * 1.01);
				CHECK(f.x == std::floor((area.w - f.w) / 2));
				// it fills one side of the area (no needless letterboxing), unless zoom is at a limit
				if (f.zoom > kMinZoom && f.zoom < kMaxZoom) CHECK(area.w - f.w < 1.5 || area.h - f.h < 1.5);
			}
		}
		// exactly 1:1 in the editor made for it
		auto area = innerArea(editorForInner(natural, header, 0), header, 0);
		auto f = placeInner(area, natural, natural, true);
		CHECK(f.zoom == 1 && f.w == natural.w && f.h == natural.h);
	}

	// a fixed native GUI keeps its size, centred, top-aligned
	{
		auto area = innerArea({1000, 700}, header, 0);
		auto f = placeInner(area, {600, 400}, {600, 400}, false);
		CHECK(f.w == 600 && f.h == 400 && f.x == 200 && f.y == 0 && f.zoom == 1);
		auto big = placeInner(area, {1200, 400}, {1200, 400}, false); // wider than the area: starts at the left
		CHECK(big.x == 0);
	}

	// a resizable GUI's adjusted size is only used if it fits
	{
		auto area = innerArea({1000, 700}, header, 0);
		CHECK(fits({1000, 644}, area));
		CHECK(!fits({1001, 644}, area));
		CHECK(!fits({1000, 645}, area));
	}

	// a flat panel (a strip of controls) keeps a sane width: the min height doesn't blow it up
	{
		auto s = snapToInner({760, 300}, {1100, 129}, header, 0);
		CHECK(s.w < 1800 && s.h >= kMinHeightWithGui);
		CHECK(clampEditor({800, 200}, kMinHeightWithGui) == (Size{800, kMinHeightWithGui}));
	}

	// snapping to a scalable GUI's shape: whatever the host proposes, the GUI then fills the editor's width with no
	// bars (within a pixel of rounding), and the editor stays within its limits
	{
		for (Size natural : {Size{904, 569}, Size{1048, 690}, Size{300, 900}, Size{2000, 300}, Size{760, 760}})
			for (uint32_t k : {0u, kb})
				for (Size proposed : {Size{100, 100}, Size{700, 460}, Size{1400, 900}, Size{5000, 300}, Size{1100, 3000}}) {
					auto s = snapToInner(proposed, natural, header, k);
					CHECK(s.w >= kMinWidth && s.h >= kMinHeightWithGui);
					auto area = innerArea(s, header, k);
					auto f = placeInner(area, natural, natural, true);
					CHECK(inside(f, area));
					if (f.zoom > kMinZoom && f.zoom < kMaxZoom) {
						CHECK(area.w - f.w <= 1.0);
						CHECK(area.h - f.h <= 1.0);
					}
					CHECK(snapToInner(s, natural, header, k) == s); // stable: snapping a snapped size changes nothing
				}
		// a GUI too narrow to reach the minimum width even at maximum zoom: the editor keeps its minimum
		{
			auto t = snapToInner({600, 300}, {150, 100}, header, 0);
			CHECK(t.w >= kMinWidth && t.w >= kMinWidthWithGui);
			CHECK(t == clampEditor(t, kMinHeightWithGui));
		}
		// the width leads
		auto s = snapToInner({1200, 50}, {1000, 500}, header, 0);
		CHECK(s.w == 1200 && s.h == 600 + header);
		// a narrow scalable GUI is zoomed up to the editor's minimum width, not padded
		auto n = editorForInner({300, 200}, header, 0, true);
		CHECK(n.w == kMinWidthWithGui && n.h == uint32_t(std::lround(kMinWidthWithGui * 200.0 / 300.0)) + header);
	}

	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("editor layout: all checks passed\n");
	return failures ? 1 : 0;
}
