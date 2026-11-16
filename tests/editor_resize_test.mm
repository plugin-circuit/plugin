// GUI resize test (macOS): opens PluginCircuit's editor in a real window under a fake host and checks, every 50 ms,
// that the editor fills the host's view and that nothing is clipped, while the size changes from every direction:
// the hosted plugin's GUI loading and resizing, the on-screen keyboard opening/closing, view switches, and the
// "user" dragging the window.
//
// The host answers request_resize like a real one, picked by <mode>:
//   wrapper  resizes synchronously, then returns false (clap-wrapper's standalone and AUv2)
//   vst3     resizes synchronously, calls set_size, returns true (clap-wrapper's VST3 via IPlugFrame::resizeView)
//   async    returns true and resizes 150 ms later (a host that resizes on its next event loop pass)
//   refuse   returns false and doesn't resize
//
// Usage: plugincircuit-editor-resize-test <PluginCircuit.clap> <mode> [innerPluginId] [seconds]
// With innerPluginId (a catalog WCLAP with a GUI, e.g. org.surge-synth-team.surge-xt) the hosted GUI is exercised
// too; it needs the package in the local cache or the API running.
#include <clap/clap.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace {
enum class Mode { Wrapper, Vst3, Async, Refuse };
Mode mode = Mode::Wrapper;
const clap_plugin *plugin = nullptr;
const clap_plugin_gui *gui = nullptr;
NSWindow *window = nil;
int resizeRequests = 0;

void hostResize(uint32_t w, uint32_t h) {
	[window setContentSize:NSMakeSize(w, h)];
	NSSize s = window.contentView.bounds.size;
	gui->set_size(plugin, uint32_t(s.width), uint32_t(s.height)); // like windowDidResize / onSize
}

const clap_host_gui hostGui{
	[](const clap_host *) {},
	[](const clap_host *, uint32_t w, uint32_t h) -> bool {
		++resizeRequests;
		switch (mode) {
		case Mode::Wrapper: [window setContentSize:NSMakeSize(w, h)]; return false;
		case Mode::Vst3: hostResize(w, h); return true;
		case Mode::Async:
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ hostResize(w, h); });
			return true;
		case Mode::Refuse: return false;
		}
		return false;
	},
	[](const clap_host *) { return false; },
	[](const clap_host *) { return false; },
	[](const clap_host *, bool) {},
};
const clap_host_thread_check threadCheck{[](const clap_host *) { return bool([NSThread isMainThread]); },
                                         [](const clap_host *) { return false; }};

// what the plugin told the DAW about its parameters, printed at the end: "B0" gesture begin on parameter 0, "D0=0.5"
// a value, "E0" gesture end (a functional check, e.g. a UI drag, can match it)
std::string dawEvents;

// params: a DAW that isn't processing flushes when the plugin asks (how UI edits get applied without audio)
const clap_host_params hostParams{
	[](const clap_host *, clap_param_rescan_flags) {},
	[](const clap_host *, clap_id, clap_param_clear_flags) {},
	[](const clap_host *) {
		dispatch_async(dispatch_get_main_queue(), ^{
			if (!plugin) return;
			auto *params = (const clap_plugin_params *)plugin->get_extension(plugin, CLAP_EXT_PARAMS);
			static const clap_input_events none{nullptr, [](const clap_input_events *) -> uint32_t { return 0; },
			                                    [](const clap_input_events *, uint32_t) -> const clap_event_header * { return nullptr; }};
			static const clap_output_events sink{nullptr, [](const clap_output_events *, const clap_event_header *h) {
				                                     if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
				                                     char e[64] = {};
				                                     if (h->type == CLAP_EVENT_PARAM_VALUE) {
					                                     auto *v = reinterpret_cast<const clap_event_param_value *>(h);
					                                     std::snprintf(e, sizeof(e), "D%u=%g", v->param_id, v->value);
				                                     } else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
					                                     std::snprintf(e, sizeof(e), "%c%u", h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? 'B' : 'E',
					                                                   reinterpret_cast<const clap_event_param_gesture *>(h)->param_id);
				                                     }
				                                     if (*e) dawEvents += (dawEvents.empty() ? "" : " ") + std::string(e);
				                                     return true;
			                                     }};
			if (params && params->flush) params->flush(plugin, &none, &sink);
		});
	},
};

const void *getExt(const clap_host *, const char *id) {
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &hostParams;
	if (!std::strcmp(id, CLAP_EXT_GUI)) return &hostGui;
	if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &threadCheck;
	return nullptr;
}
clap_host host{CLAP_VERSION_INIT, nullptr, "editor-resize-test", "plugincircuit", "", "0", getExt,
               [](const clap_host *) {}, [](const clap_host *) {},
               [](const clap_host *) {
	               dispatch_async(dispatch_get_main_queue(), ^{ if (plugin) plugin->on_main_thread(plugin); });
               }};

// --- invariants -------------------------------------------------------------------------------------------------
constexpr double kTol = 1.0;
constexpr double kHeader = 56;
bool within(NSRect r, NSRect outer) {
	return r.origin.x >= outer.origin.x - kTol && r.origin.y >= outer.origin.y - kTol &&
	       NSMaxX(r) <= NSMaxX(outer) + kTol && NSMaxY(r) <= NSMaxY(outer) + kTol;
}
bool same(NSRect a, NSRect b) {
	return std::fabs(a.origin.x - b.origin.x) <= kTol && std::fabs(a.origin.y - b.origin.y) <= kTol &&
	       std::fabs(a.size.width - b.size.width) <= kTol && std::fabs(a.size.height - b.size.height) <= kTol;
}
std::string str(NSRect r) {
	char b[96];
	std::snprintf(b, sizeof b, "%.0f,%.0f %.0fx%.0f", r.origin.x, r.origin.y, r.size.width, r.size.height);
	return b;
}

// Returns the first broken invariant, or "".
std::string check(bool &innerShown) {
	NSView *parent = window.contentView;
	if (parent.subviews.count != 1) return "host view should hold exactly the editor";
	NSView *container = parent.subviews[0];
	if (!same(container.frame, parent.bounds)) return "editor " + str(container.frame) + " != host view " + str(parent.bounds);
	NSView *innerHolder = nil;
	for (NSView *v in container.subviews) {
		if ([v isKindOfClass:[WKWebView class]]) {
			if (!same(v.frame, container.bounds)) return "page " + str(v.frame) + " != editor " + str(container.bounds);
		} else innerHolder = v;
	}
	if (!innerHolder) return "no inner GUI holder";
	NSRect ih = innerHolder.frame;
	if (std::fabs(ih.origin.y - kHeader) > kTol || std::fabs(ih.origin.x) > kTol || std::fabs(ih.size.width - container.bounds.size.width) > kTol)
		return "inner area " + str(ih) + " not under the header across the editor";
	if (NSMaxY(ih) > NSMaxY(container.bounds) + kTol) return "inner area " + str(ih) + " runs past the editor";
	if (!innerHolder.hidden)
		for (NSView *v in innerHolder.subviews) {
			innerShown = true;
			if (!within(v.frame, innerHolder.bounds)) return "inner GUI " + str(v.frame) + " clipped by its area " + str(innerHolder.bounds);
		}
	uint32_t w = 0, h = 0;
	gui->get_size(plugin, &w, &h);
	if (std::fabs(w - parent.bounds.size.width) > kTol || std::fabs(h - parent.bounds.size.height) > kTol) {
		char b[96];
		std::snprintf(b, sizeof b, "get_size %ux%u != host view %.0fx%.0f", w, h, parent.bounds.size.width, parent.bounds.size.height);
		return b;
	}
	return "";
}
// After a host resize through adjust_size: the inner GUI fills its area (within rounding), no bars at the sides.
std::string checkFill() {
	NSView *container = window.contentView.subviews.firstObject;
	for (NSView *holder in container.subviews) {
		if ([holder isKindOfClass:[WKWebView class]] || holder.hidden) continue;
		NSView *v = holder.subviews.firstObject;
		if (!v) return "no inner GUI to check the fill of";
		if (![v isKindOfClass:[WKWebView class]]) return ""; // a native fixed GUI isn't scaled: nothing to fill
		if (holder.bounds.size.width - v.frame.size.width > 1.5 || holder.bounds.size.height - v.frame.size.height > 1.5)
			return "inner GUI " + str(v.frame) + " doesn't fill its area " + str(holder.bounds);
	}
	return "";
}
} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::printf("usage: %s <PluginCircuit.clap> <wrapper|vst3|async|refuse> [innerPluginId] [seconds]\n", argv[0]);
		return 2;
	}
	const std::string m = argv[2];
	mode = m == "vst3" ? Mode::Vst3 : m == "async" ? Mode::Async : m == "refuse" ? Mode::Refuse : Mode::Wrapper;
	const bool withInner = argc > 3 && std::strlen(argv[3]) > 0;
	const double seconds = argc > 4 ? std::atof(argv[4]) : (withInner ? 22 : 14);

	// the editor's debug hooks drive the page: load the inner plugin, then toggle the keyboard and switch views
	if (withInner) setenv("PLUGINCIRCUIT_AUTOLOAD", argv[3], 1);
	if (!std::getenv("PLUGINCIRCUIT_UI_JS")) // a caller's own script wins (e.g. a functional check)
		setenv("PLUGINCIRCUIT_UI_JS",
	       "(async () => { const s = (ms) => new Promise((r) => setTimeout(r, ms));"
	       " const kb = () => document.getElementById('kbdToggle').click();"
	       " const tab = (v) => { const b = document.querySelector('.tabs button[data-view=' + v + ']'); if (b) b.click(); };"
	       " kb(); await s(1200); kb(); await s(1200); kb(); await s(1200);"
	       " tab('macros'); await s(800); tab('plugin'); await s(800); kb(); })()",
	       1);

	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

		std::string bin = std::string(argv[1]) + "/Contents/MacOS/PluginCircuit";
		void *lib = dlopen(bin.c_str(), RTLD_NOW);
		if (!lib) { std::printf("dlopen: %s\n", dlerror()); return 1; }
		auto *entry = (const clap_plugin_entry *)dlsym(lib, "clap_entry");
		entry->init(argv[1]);
		auto *factory = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
		plugin = factory->create_plugin(factory, &host, "studio.plugincircuit.host.synth");
		if (!plugin || !plugin->init(plugin)) { std::printf("plugin init failed\n"); return 1; }
		gui = (const clap_plugin_gui *)plugin->get_extension(plugin, CLAP_EXT_GUI);
		if (!gui || !gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) { std::printf("gui create failed\n"); return 1; }

		uint32_t w = 0, h = 0;
		gui->get_size(plugin, &w, &h);
		window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, w, h)
		                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
		                                       backing:NSBackingStoreBuffered
		                                         defer:NO];
		clap_window cw{CLAP_WINDOW_API_COCOA, {.cocoa = (__bridge void *)window.contentView}};
		if (!gui->set_parent(plugin, &cw)) { std::printf("set_parent failed\n"); return 1; }
		gui->show(plugin);
		[window orderFrontRegardless];

		// the "user" drags the window near the end: sizes through adjust_size, like a host's live resize
		const double dragAt = seconds - 4;
		const NSSize drags[] = {{700, 460}, {1400, 900}, {500, 300}, {1100, 800}};
		for (int i = 0; i < 4; ++i) {
			const NSSize d = drags[i];
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t((dragAt + i * 0.7) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
				uint32_t dw = uint32_t(d.width), dh = uint32_t(d.height);
				gui->adjust_size(plugin, &dw, &dh);
				hostResize(dw, dh);
			});
		}

		// check every 50 ms; a broken invariant fails once it lasts longer than 150 ms (a host's async resize may
		// leave a frame or two in between)
		__block int ticks = 0, streak = 0, failures = 0, sizeChanges = 0;
		__block bool innerShown = false, fillChecked = false;
		__block NSSize lastSize = window.contentView.bounds.size;
		__block std::string lastError;
		NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
			++ticks;
			NSSize s = window.contentView.bounds.size;
			if (s.width != lastSize.width || s.height != lastSize.height) ++sizeChanges;
			lastSize = s;
			std::string err = check(innerShown);
			// once the drags (sized through adjust_size) are done, a scalable inner GUI must span the editor: no bars
			if (err.empty() && withInner && !fillChecked && ticks * 0.05 >= dragAt + 4 * 0.7 + 0.3) {
				fillChecked = true;
				err = checkFill();
				if (!err.empty()) streak = 3; // fail now
			}
			if (!err.empty()) {
				if (++streak == 4) {
					++failures;
					std::printf("FAIL t=%.2fs  %s\n", ticks * 0.05, err.c_str());
				}
				lastError = err;
			} else streak = 0;
			if (ticks * 0.05 >= seconds) {
				[t invalidate];
				[NSApp stop:nil];
				[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
				                                windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
			}
		}];
		(void)timer;
		[NSApp run];

		std::printf("mode %s: %d check(s), %d size change(s), %d resize request(s), inner GUI %s, %d failure(s)\n", m.c_str(), ticks,
		            sizeChanges, resizeRequests, innerShown ? "shown" : "not shown", failures);
		bool ok = failures == 0 && sizeChanges >= 3; // the run actually resized
		if (withInner && !innerShown) {
			std::printf("FAIL the inner plugin's GUI never showed\n");
			ok = false;
		}
		gui->destroy(plugin);
		plugin->destroy(plugin);
		entry->deinit();
		std::printf("daw events: [%s]\n", dawEvents.c_str());
		std::printf(ok ? "PASS\n" : "FAIL\n");
		std::fflush(stdout);
		_exit(ok ? 0 : 1); // skip static destructors of the dlopen'ed bundle
	}
}
