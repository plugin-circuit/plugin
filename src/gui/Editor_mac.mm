// The editor's macOS part (AppKit): the container in the host's NSView, our WKWebView, and the inner GUI's holder.
// The logic is in EditorCore.cpp.
#include "gui/EditorCore.h"

#include "plugin/PluginCircuitPlugin.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

@interface PluginCircuitFlippedView : NSView
@end
@implementation PluginCircuitFlippedView
- (BOOL)isFlipped {
	return YES;
}
@end

// The editor's root view: fills the host's parent view (autoresizing) and reports every size change, so the layout
// always follows the size the host actually gave us, whatever request_resize returned.
@interface PluginCircuitContainerView : PluginCircuitFlippedView
@property(nonatomic, copy) void (^onResize)(void);
@end
@implementation PluginCircuitContainerView
- (void)setFrameSize:(NSSize)size {
	[super setFrameSize:size];
	if (self.onResize) self.onResize();
}
@end

namespace plugincircuit {

const char *EditorCore::windowApi() { return CLAP_WINDOW_API_COCOA; }

namespace {

class MacEditor final : public EditorCore {
public:
	using EditorCore::EditorCore;

	bool setParent(const clap_window *window) override {
		if (!window || std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) return false;
		NSView *parent = (__bridge NSView *)window->cocoa;
		[container setFrameSize:NSMakeSize(width, height)];
		[parent addSubview:container];
		layout();
		return true;
	}
	void setVisible(bool visible) override { [container setHidden:!visible]; }

protected:
	PluginCircuitContainerView *container = nil;
	PluginCircuitFlippedView *innerHolder = nil;
	id keyMonitor = nil; // copies note keys typed into the inner plugin's GUI to the on-screen keyboard

	bool createViews() override {
		container = [[PluginCircuitContainerView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
		container.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		container.onResize = ^{
			hostResized();
		};
		container.wantsLayer = YES;
		container.layer.backgroundColor = [NSColor colorWithSRGBRed:0x15 / 255.0 green:0x1b / 255.0 blue:0x24 / 255.0 alpha:1].CGColor;
		NSView *wv = (__bridge NSView *)web->getViewHandle();
		[container addSubview:wv];
		innerHolder = [[PluginCircuitFlippedView alloc] initWithFrame:NSMakeRect(0, Editor::headerHeight, width, height - Editor::headerHeight)];
		[innerHolder setHidden:YES];
		[container addSubview:innerHolder]; // above the webview
		return true;
	}

	void destroyViews() override {
		container.onResize = nil;
		[innerHolder removeFromSuperview];
		[container removeFromSuperview];
	}

	void *innerParent() override { return (__bridge void *)innerHolder; }
	bool parented() const override { return container && container.superview; }
	layout::Size containerSize() const override {
		return {uint32_t(container.bounds.size.width), uint32_t(container.bounds.size.height)};
	}
	void sizeContainer(uint32_t w, uint32_t h) override { [container setFrameSize:NSMakeSize(w, h)]; }
	void placeWeb(uint32_t w, uint32_t h) override {
		NSView *wv = (__bridge NSView *)web->getViewHandle();
		[wv setFrame:NSMakeRect(0, 0, w, h)];
	}
	void placeHolder(const layout::Frame &area, bool visible) override {
		[innerHolder setFrame:NSMakeRect(area.x, area.y, area.w, area.h)];
		[innerHolder setHidden:!visible];
	}

	// A fixed-size WCLAP webview GUI (a WKWebView) is scaled to fill the area with WebKit's page zoom, keeping its
	// aspect ratio (the page still lays out at its own size). Anything else is centred at its own size.
	void placeInnerViews(const layout::Frame &area, bool resizable, layout::Size own) override {
		for (NSView *v in innerHolder.subviews) {
			const bool scalable = !resizable && [v isKindOfClass:[WKWebView class]];
			const layout::Size size = resizable ? own : layout::Size{uint32_t(v.frame.size.width), uint32_t(v.frame.size.height)};
			const auto f = layout::placeInner(area, size, {innerW, innerH}, scalable);
			if (scalable) ((WKWebView *)v).pageZoom = f.zoom;
			if (std::getenv("PLUGINCIRCUIT_DEBUG_UI")) std::fprintf(stderr, "[plugincircuit] inner view %.0fx%.0f in %.0fx%.0f, scaled %d\n", f.w, f.h, area.w, area.h, int(scalable));
			[v setFrame:NSMakeRect(f.x, f.y, f.w, f.h)];
		}
	}

	bool innerIsZoomableWebview() override { return [innerHolder.subviews.firstObject isKindOfClass:[WKWebView class]]; }

	// No white flashes: a WKWebView paints white wherever the page hasn't drawn yet (while it re-renders after a
	// resize or zoom change). Turn that off and fill behind it with the plugin UI's own color instead, sampled from
	// its rendered edges once it has drawn (and again later, for UIs that load in stages).
	void innerGuiShown() override {
		innerHolder.wantsLayer = YES;
		innerHolder.layer.backgroundColor = container.layer.backgroundColor;
		for (NSView *v in innerHolder.subviews) {
			if (![v isKindOfClass:[WKWebView class]]) continue;
			WKWebView *wv = (WKWebView *)v;
			[wv setValue:@NO forKey:@"drawsBackground"]; // the same switch CHOC's transparentBackground uses
			__weak WKWebView *weakWeb = wv;
			__weak NSView *weakHolder = innerHolder;
			for (double delay : {0.6, 2.0, 5.0})
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
					sampleBackground(weakWeb, weakHolder);
				});
		}
	}

	static void sampleBackground(WKWebView *wv, NSView *holder) {
		if (!wv || !holder || wv.superview != holder) return;
		[wv takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
			if (!image) return;
			NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:[image TIFFRepresentation]];
			if (!rep || rep.pixelsWide < 4 || rep.pixelsHigh < 4) return;
			// the most common color along the edges (quantized), averaged within its bucket
			const NSInteger w = rep.pixelsWide, h = rep.pixelsHigh, step = std::max<NSInteger>(1, (w + h) / 400);
			struct Acc {
				double r = 0, g = 0, b = 0;
				int n = 0;
			};
			std::array<Acc, 4096> buckets{};
			auto add = [&](NSInteger x, NSInteger y) {
				NSColor *c = [[rep colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
				if (!c || c.alphaComponent < 0.5) return;
				const int key = int(c.redComponent * 15.99) << 8 | int(c.greenComponent * 15.99) << 4 | int(c.blueComponent * 15.99);
				auto &a = buckets[size_t(key)];
				a.r += c.redComponent, a.g += c.greenComponent, a.b += c.blueComponent, ++a.n;
			};
			for (NSInteger x = 0; x < w; x += step) add(x, 1), add(x, h - 2);
			for (NSInteger y = 0; y < h; y += step) add(1, y), add(w - 2, y);
			const Acc *best = nullptr;
			for (auto &a : buckets)
				if (a.n && (!best || a.n > best->n)) best = &a;
			if (!best) return;
			NSColor *bg = [NSColor colorWithSRGBRed:best->r / best->n green:best->g / best->n blue:best->b / best->n alpha:1];
			if (wv.superview != holder) return;
			holder.layer.backgroundColor = bg.CGColor;
			if (@available(macOS 12.0, *)) wv.underPageBackgroundColor = bg;
		}];
	}

	void clearHolder() override {
		for (NSView *v in [innerHolder.subviews copy]) [v removeFromSuperview];
	}

	void openUrl(const std::string &url) override {
		[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
	}

	// The inner plugin's GUI is its own view (a WCLAP webview GUI is a WKWebView): once clicked it has the keyboard
	// focus, and the host page no longer sees key events. While the on-screen keyboard is open, copy the note keys (by
	// physical position, like the page's e.code mapping) to the page. The event still reaches the inner GUI, so typing
	// into its text fields keeps working.
	void startPlatform() override {
		keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp)
		                                                   handler:^NSEvent *(NSEvent *e) {
			                                                   forwardNoteKey(e);
			                                                   return e;
		                                                   }];
	}
	void stopPlatform() override {
		if (keyMonitor) [NSEvent removeMonitor:keyMonitor];
		keyMonitor = nil;
	}

	void forwardNoteKey(NSEvent *e) {
		if (!keyboardOpen() || !container.window || e.window != container.window) return;
		auto *responder = (NSView *)container.window.firstResponder;
		if (![responder isKindOfClass:[NSView class]] || ![responder isDescendantOf:innerHolder]) return;
		const bool down = e.type == NSEventTypeKeyDown;
		if (down && (e.isARepeat || (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))))
			return;
		static const std::pair<unsigned short, const char *> kCodes[] = {
			{0x00, "KeyA"}, {0x0D, "KeyW"}, {0x01, "KeyS"}, {0x0E, "KeyE"}, {0x02, "KeyD"}, {0x03, "KeyF"},
			{0x11, "KeyT"}, {0x05, "KeyG"}, {0x10, "KeyY"}, {0x04, "KeyH"}, {0x20, "KeyU"}, {0x26, "KeyJ"},
			{0x28, "KeyK"}, {0x1F, "KeyO"}, {0x25, "KeyL"}, {0x23, "KeyP"}, {0x29, "Semicolon"}, {0x27, "Quote"},
			{0x06, "KeyZ"}, {0x07, "KeyX"}};
		for (auto &[keyCode, code] : kCodes)
			if (e.keyCode == keyCode) {
				noteKey(code, down);
				return;
			}
	}

	// PLUGINCIRCUIT_SNAPSHOT=<file.png>: the webview (WebKit's own renderer, no screen-recording permission needed)
	// plus the inner GUI view; the inner WKWebView also to <file>.inner.png. PLUGINCIRCUIT_INNER_JS=<js> runs once in
	// the inner webview; it can report through window.__probe.
	void debugSnapshot(const std::string &path) override {
		WKWebView *wv = (__bridge WKWebView *)web->getViewHandle();
		std::function<WKWebView *(NSView *)> findWeb = [&](NSView *v) -> WKWebView * {
			if ([v isKindOfClass:[WKWebView class]]) return (WKWebView *)v;
			for (NSView *c in v.subviews)
				if (auto *w = findWeb(c)) return w;
			return nil;
		};
		if (WKWebView *innerWeb = innerHolder.isHidden ? nil : findWeb(innerHolder)) {
			std::string innerPath = path + ".inner.png";
			static bool innerJsDone = false;
			if (const char *js = std::getenv("PLUGINCIRCUIT_INNER_JS"); js && !innerJsDone) {
				innerJsDone = true;
				[innerWeb evaluateJavaScript:[NSString stringWithUTF8String:js] completionHandler:nil];
			}
			[innerWeb evaluateJavaScript:@"location.href + ' | html=' + document.documentElement.outerHTML.length + ' | body=' + (document.body ? document.body.innerText.slice(0,120) : 'none') + ' | probe=' + (window.__probe || '')"
			           completionHandler:^(id result, NSError *error) {
				              std::fprintf(stderr, "[plugincircuit inner] %s %s\n", result ? [[result description] UTF8String] : "(nil)",
				                           error ? error.localizedDescription.UTF8String : "");
			           }];
			[innerWeb takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
				if (!image) return;
				NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:[image TIFFRepresentation]];
				[[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:innerPath.c_str()] atomically:YES];
			}];
		}
		NSView *innerView = innerHolder.isHidden ? nil : innerHolder.subviews.firstObject;
		NSView *root = container;
		[wv takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *) {
			if (!image) return;
			NSImage *composite = [[NSImage alloc] initWithSize:image.size];
			[composite lockFocus];
			[image drawAtPoint:NSZeroPoint fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
			if (innerView) {
				NSBitmapImageRep *rep = [innerView bitmapImageRepForCachingDisplayInRect:innerView.bounds];
				[innerView cacheDisplayInRect:innerView.bounds toBitmapImageRep:rep];
				NSRect f = [innerView convertRect:innerView.bounds toView:root];
				[rep drawInRect:NSMakeRect(f.origin.x, image.size.height - f.origin.y - f.size.height, f.size.width, f.size.height)];
			}
			[composite unlockFocus];
			NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:[composite TIFFRepresentation]];
			NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
			[png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
		}];
	}
};

} // namespace

std::unique_ptr<EditorCore> EditorCore::make(PluginCircuitPlugin &plugin) { return std::make_unique<MacEditor>(plugin); }

} // namespace plugincircuit
