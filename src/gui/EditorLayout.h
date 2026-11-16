#pragma once
// The editor's sizing rules, free of any GUI toolkit so they can be unit-tested (tests/editor_layout_test.cpp).
//
// The editor is: header strip on top, the inner plugin's GUI area below it, the on-screen keyboard strip at the
// bottom. Like Logic's plug-in window, nothing of ours covers the inner GUI's area.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace plugincircuit::layout {

struct Size {
	uint32_t w = 0, h = 0;
	bool operator==(const Size &o) const { return w == o.w && h == o.h; }
};

struct Frame {
	double x = 0, y = 0, w = 0, h = 0;
	double zoom = 1; // page zoom for a scaled webview GUI
};

constexpr uint32_t kMinWidth = 640, kMinHeight = 420; // the editor's smallest size
constexpr uint32_t kMinWidthWithGui = 760;            // room for the header's controls next to a narrow inner GUI
constexpr uint32_t kMinHeightWithGui = 240;           // a flat plugin panel (a strip of controls) may be short
constexpr double kMinZoom = 0.25, kMaxZoom = 4.0;

// The inner GUI's area, in editor coordinates (y down).
inline Frame innerArea(Size editor, uint32_t header, uint32_t keyboard) {
	const uint32_t chrome = header + keyboard;
	return {0, double(header), double(editor.w), double(editor.h > chrome ? editor.h - chrome : 0)};
}

// The editor size that shows an inner GUI of `inner` at 1:1. A scalable GUI narrower than the minimum is zoomed up
// to the full width instead (so it always spans the editor); a fixed one is padded.
inline Size editorForInner(Size inner, uint32_t header, uint32_t keyboard, bool scalable = false) {
	const uint32_t w = std::max({inner.w, kMinWidthWithGui, kMinWidth});
	if (scalable && inner.w > 0 && inner.h > 0) {
		const uint32_t h = uint32_t(std::lround(double(w) * inner.h / inner.w)) + header + keyboard;
		return {w, std::max(h, kMinHeightWithGui)};
	}
	return {w, std::max(inner.h + header + keyboard, kMinHeightWithGui)};
}

// Snap a size the host proposes to the inner GUI's aspect ratio (plus the header and keyboard strips), so a scalable
// GUI always fills the editor's width with no bars. The width leads; it grows if the height would go under the min.
inline Size snapToInner(Size proposed, Size natural, uint32_t header, uint32_t keyboard) {
	if (natural.w == 0 || natural.h == 0) return proposed;
	const double ratio = double(natural.h) / natural.w;
	const uint32_t chrome = header + keyboard;
	// the zoom range first, then the editor's minimum: a tiny GUI at its maximum zoom is centred in a minimum-size
	// editor rather than making the editor narrower than its minimum
	uint32_t w = std::min(proposed.w, uint32_t(std::floor(natural.w * kMaxZoom)));
	w = std::max(w, uint32_t(std::ceil(natural.w * kMinZoom)));
	w = std::max({w, kMinWidth, kMinWidthWithGui});
	if (kMinHeightWithGui > chrome) w = std::max(w, uint32_t(std::ceil((kMinHeightWithGui - chrome) / ratio)));
	return {w, uint32_t(std::lround(w * ratio)) + chrome};
}

// Clamp a size the host proposes (adjust_size / set_size).
inline Size clampEditor(Size s, uint32_t minHeight = kMinHeight) { return {std::max(s.w, kMinWidth), std::max(s.h, minHeight)}; }

// Where the inner GUI's view goes inside `area` (area-relative coordinates).
//  - scalable (a fixed-size webview GUI): zoomed to fit the area, keeping its aspect ratio, never past it
//  - otherwise the view keeps `view` (its own size, or what adjust_size gave a resizable GUI), centred
//    horizontally, top-aligned; a native fixed-size GUI larger than the area is cut at the right/bottom
inline Frame placeInner(Frame area, Size view, Size natural, bool scalable) {
	Frame f{0, 0, double(view.w), double(view.h)};
	if (scalable && natural.w > 0 && natural.h > 0 && area.w > 0 && area.h > 0) {
		const double s = std::clamp(std::min(area.w / natural.w, area.h / natural.h), kMinZoom, kMaxZoom);
		f.zoom = s;
		f.w = std::min(std::floor(natural.w * s), area.w);
		f.h = std::min(std::floor(natural.h * s), area.h);
	}
	f.x = std::max(0.0, std::floor((area.w - f.w) / 2));
	return f;
}

// Whether a resizable inner GUI's adjust_size answer fits the area (else we keep its current size).
inline bool fits(Size s, Frame area) { return s.w <= area.w && s.h <= area.h; }

} // namespace plugincircuit::layout
