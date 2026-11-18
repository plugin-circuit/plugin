#include "plugin/PluginCircuitPlugin.h"

#include "api/SettingsSchema.h"
#include "core/Settings.h"

#include "gui/Editor.h"

#include <bit>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace plugincircuit {

// ---- descriptors ------------------------------------------------------------------------------
static const char *fxFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
static const char *instFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor PluginCircuitPlugin::fxDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "studio.plugincircuit.host.fx",
	.name = "Plugin Circuit FX",
	.vendor = "Plugin Circuit",
	.url = "http://127.0.0.1:7790",
	.manual_url = "",
	.support_url = "",
	.version = PLUGINCIRCUIT_VERSION,
	.description = "Loads WCLAP effects from the Plugin Circuit catalog",
	.features = fxFeatures,
};
const clap_plugin_descriptor PluginCircuitPlugin::instrumentDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "studio.plugincircuit.host.synth",
	.name = "Plugin Circuit Synth",
	.vendor = "Plugin Circuit",
	.url = "http://127.0.0.1:7790",
	.manual_url = "",
	.support_url = "",
	.version = PLUGINCIRCUIT_VERSION,
	.description = "Loads WCLAP instruments from the Plugin Circuit catalog",
	.features = instFeatures,
};

// ---- C trampolines ------------------------------------------------------------------------------
struct PluginGlue {
	static PluginCircuitPlugin *self(const clap_plugin *p) { return static_cast<PluginCircuitPlugin *>(p->plugin_data); }

	static constexpr clap_plugin_audio_ports audioPorts = {
		.count = [](const clap_plugin *p, bool in) { return self(p)->audioPortsCount(in); },
		.get = [](const clap_plugin *p, uint32_t i, bool in, clap_audio_port_info *info) { return self(p)->audioPortsGet(i, in, info); },
	};
	static constexpr clap_plugin_note_ports notePorts = {
		.count = [](const clap_plugin *p, bool in) { return self(p)->notePortsCount(in); },
		.get = [](const clap_plugin *p, uint32_t i, bool in, clap_note_port_info *info) { return self(p)->notePortsGet(i, in, info); },
	};
	static constexpr clap_plugin_params params = {
		.count = [](const clap_plugin *) -> uint32_t { return kNumMacros; },
		.get_info = [](const clap_plugin *p, uint32_t i, clap_param_info *info) { return self(p)->paramsGetInfo(i, info); },
		.get_value = [](const clap_plugin *p, clap_id id, double *v) { return self(p)->paramsGetValue(id, v); },
		.value_to_text = [](const clap_plugin *p, clap_id id, double v, char *d, uint32_t n) { return self(p)->paramsValueToText(id, v, d, n); },
		.text_to_value = [](const clap_plugin *p, clap_id id, const char *d, double *v) { return self(p)->paramsTextToValue(id, d, v); },
		.flush = [](const clap_plugin *p, const clap_input_events *in, const clap_output_events *out) { self(p)->paramsFlush(in, out); },
	};
	static constexpr clap_plugin_state state = {
		.save = [](const clap_plugin *p, const clap_ostream *s) { return self(p)->stateSave(s); },
		.load = [](const clap_plugin *p, const clap_istream *s) { return self(p)->stateLoad(s); },
	};
	static constexpr clap_plugin_tail tail = { // the WCLAP's ([main-thread, audio-thread]: an atomic)
		.get = [](const clap_plugin *p) { return self(p)->tailSamples_.load(std::memory_order_relaxed); },
	};
	static constexpr clap_plugin_latency latency = {
		.get = [](const clap_plugin *p) { return self(p)->latencyGet(); },
	};
	static constexpr clap_plugin_timer_support timer = {
		.on_timer = [](const clap_plugin *p, clap_id id) { self(p)->onTimer(id); },
	};
	static constexpr clap_plugin_gui gui = {
		.is_api_supported = [](const clap_plugin *, const char *api, bool floating) { return Editor::isApiSupported(api, floating); },
		.get_preferred_api = [](const clap_plugin *, const char **api, bool *floating) { return Editor::preferredApi(api, floating); },
		.create = [](const clap_plugin *p, const char *api, bool floating) {
			auto *s = self(p);
			if (s->editor_) return true;
			s->editor_ = std::make_unique<Editor>(*s);
			if (!s->editor_->create()) { s->editor_.reset(); return false; }
			return true;
		},
		.destroy = [](const clap_plugin *p) {
			self(p)->editor_.reset();
			self(p)->gestures_.requestCloseUi(); // a drag can't end without its page
		},
		.set_scale = [](const clap_plugin *p, double scale) { return self(p)->editor_ ? self(p)->editor_->setScale(scale) : false; },
		.get_size = [](const clap_plugin *p, uint32_t *w, uint32_t *h) { return self(p)->editor_ ? self(p)->editor_->getSize(*w, *h) : false; },
		.can_resize = [](const clap_plugin *) { return true; },
		.get_resize_hints = [](const clap_plugin *, clap_gui_resize_hints *hints) {
			*hints = {.can_resize_horizontally = true, .can_resize_vertically = true, .preserve_aspect_ratio = false};
			return true;
		},
		.adjust_size = [](const clap_plugin *p, uint32_t *w, uint32_t *h) { return self(p)->editor_ ? self(p)->editor_->adjustSize(*w, *h) : false; },
		.set_size = [](const clap_plugin *p, uint32_t w, uint32_t h) { return self(p)->editor_ ? self(p)->editor_->setSize(w, h) : false; },
		.set_parent = [](const clap_plugin *p, const clap_window *window) { return self(p)->editor_ ? self(p)->editor_->setParent(window) : false; },
		.set_transient = [](const clap_plugin *, const clap_window *) { return false; },
		.suggest_title = [](const clap_plugin *, const char *) {},
		.show = [](const clap_plugin *p) { return self(p)->editor_ ? self(p)->editor_->show() : false; },
		.hide = [](const clap_plugin *p) { return self(p)->editor_ ? self(p)->editor_->hide() : false; },
	};
};

PluginCircuitPlugin::PluginCircuitPlugin(const clap_host *host, bool instrument) : host_(host), instrument_(instrument) {
	plugin_ = clap_plugin{
		.desc = instrument ? &instrumentDescriptor : &fxDescriptor,
		.plugin_data = this,
		.init = [](const clap_plugin *p) { return PluginGlue::self(p)->init(); },
		.destroy = [](const clap_plugin *p) { PluginGlue::self(p)->destroy(); },
		.activate = [](const clap_plugin *p, double sr, uint32_t mn, uint32_t mx) { return PluginGlue::self(p)->activate(sr, mn, mx); },
		.deactivate = [](const clap_plugin *p) { PluginGlue::self(p)->deactivate(); },
		.start_processing = [](const clap_plugin *p) { return PluginGlue::self(p)->startProcessing(); },
		.stop_processing = [](const clap_plugin *p) { PluginGlue::self(p)->stopProcessing(); },
		.reset = [](const clap_plugin *p) { PluginGlue::self(p)->reset(); },
		.process = [](const clap_plugin *p, const clap_process *proc) { return PluginGlue::self(p)->process(proc); },
		.get_extension = [](const clap_plugin *p, const char *id) { return PluginGlue::self(p)->getExtension(id); },
		.on_main_thread = [](const clap_plugin *p) { PluginGlue::self(p)->onMainThread(); },
	};
	innerIn_.list = clap_input_events{
		.ctx = &innerIn_,
		.size = [](const clap_input_events *l) { return (uint32_t)static_cast<EventBuffer *>(l->ctx)->offsets.size(); },
		.get = [](const clap_input_events *l, uint32_t i) {
			auto *b = static_cast<EventBuffer *>(l->ctx);
			return reinterpret_cast<const clap_event_header *>(b->storage.data() + b->offsets[i]);
		},
	};
	innerOut_ = clap_output_events{.ctx = this, .try_push = &PluginCircuitPlugin::innerOutTryPush};
	for (auto &v : macroValues_) v.store(0);
	ownedMacros_ = std::make_unique<MacroMap>();
	macros_.store(ownedMacros_.get());
}

PluginCircuitPlugin::~PluginCircuitPlugin() = default;

void PluginCircuitPlugin::EventBuffer::push(const clap_event_header *e) {
	if (offsets.size() == offsets.capacity() || storage.size() + e->size > storage.capacity()) return; // full: drop (no alloc)
	offsets.push_back((uint32_t)storage.size());
	auto *bytes = reinterpret_cast<const uint8_t *>(e);
	storage.insert(storage.end(), bytes, bytes + e->size);
}

// ---- lifecycle ------------------------------------------------------------------------------------
bool PluginCircuitPlugin::init() {
	mainThreadId_ = std::this_thread::get_id();
	auto ext = [&](const char *id) { return host_->get_extension(host_, id); };
	hostParams_ = (const clap_host_params *)ext(CLAP_EXT_PARAMS);
	hostState_ = (const clap_host_state *)ext(CLAP_EXT_STATE);
	hostLatency_ = (const clap_host_latency *)ext(CLAP_EXT_LATENCY);
	hostTail_ = (const clap_host_tail *)ext(CLAP_EXT_TAIL);
	hostGui_ = (const clap_host_gui *)ext(CLAP_EXT_GUI);
	hostTimer_ = (const clap_host_timer_support *)ext(CLAP_EXT_TIMER_SUPPORT);
	hostLog_ = (const clap_host_log *)ext(CLAP_EXT_LOG);
	if (hostTimer_ && hostTimer_->register_timer) hostTimer_->register_timer(host_, 50, &housekeepingTimer_);

	uiEvents_.reset(1024);
	uiNotes_.reset(1024);
	dawNotes_.reset(256);
	innerIn_.storage.reserve(1 << 16);
	innerIn_.offsets.reserve(4096);
	return true;
}

void PluginCircuitPlugin::destroy() {
	editor_.reset();
	if (loadThread_.joinable()) loadThread_.join();
	if (hostTimer_ && housekeepingTimer_ != CLAP_INVALID_ID) hostTimer_->unregister_timer(host_, housekeepingTimer_);
	// inner plugins must go before the module they come from (Slot owns both via InnerPlugin)
	lockInner();
	current_ = nullptr;
	audioStarted_ = nullptr;
	unlockInner();
	owned_.reset();
	retiredSlots_.clear();
	delete this;
}

bool PluginCircuitPlugin::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) {
	sampleRate_ = sampleRate;
	minFrames_ = minFrames;
	maxFrames_ = maxFrames;
	if (current_ && !current_->inner->activate(sampleRate, minFrames, maxFrames))
		setStatus(Status::Error, "inner plugin failed to activate");
	active_ = true;
	return true;
}

void PluginCircuitPlugin::deactivate() {
	gestures_.requestCloseAll(); // closed (GESTURE_END to the DAW) at the first process or flush after this
	lockInner();
	if (audioStarted_ && audioStarted_->started) { // host stopped processing without stop_processing reaching us
		audioStarted_->inner->stopProcessing();
		audioStarted_->started = false;
	}
	audioStarted_ = nullptr;
	unlockInner();
	if (current_) current_->inner->deactivate();
	active_ = false;
	housekeeping();
}

bool PluginCircuitPlugin::startProcessing() {
	processing_ = true;
	return true;
}

void PluginCircuitPlugin::stopProcessing() {
	lockInner();
	if (audioStarted_ && audioStarted_->started) {
		audioStarted_->inner->stopProcessing();
		audioStarted_->started = false;
	}
	audioStarted_ = nullptr;
	unlockInner();
	processing_ = false;
}

void PluginCircuitPlugin::reset() { gestures_.requestCloseAll(); }

void PluginCircuitPlugin::lockInner() {
	while (innerLock_.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
}

// ---- audio ---------------------------------------------------------------------------------------
void PluginCircuitPlugin::passThrough(const clap_process *p) {
	if (p->audio_outputs_count == 0) return;
	auto &out = p->audio_outputs[0];
	for (uint32_t c = 0; c < out.channel_count; ++c) {
		float *dst = out.data32[c];
		const float *src = (!instrument_ && p->audio_inputs_count > 0 && c < p->audio_inputs[0].channel_count)
		                       ? p->audio_inputs[0].data32[c] : nullptr;
		if (src && src != dst) std::memcpy(dst, src, sizeof(float) * p->frames_count);
		else if (!src) std::memset(dst, 0, sizeof(float) * p->frames_count);
	}
}

// Where the gesture state machine's decisions go: values into the WCLAP's input list (and the table), macro values to
// macroValues_ and the DAW, gestures to the DAW. `time` stamps what goes out (the event being handled).
struct PluginCircuitPlugin::EventSink final : Gestures::Sink {
	PluginCircuitPlugin &self;
	ParamTable *table;
	uint32_t time = 0;
	EventSink(PluginCircuitPlugin &s, ParamTable *t) : self(s), table(t) {}
	void toPlugin(clap_id param, double value) override {
		clap_event_param_value e{{sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, param, nullptr, -1, -1, -1, -1, value};
		self.innerIn_.push(&e.header);
		if (table) table->setById(param, value); // every value applied to the plugin is also the table's
	}
	void macroValue(uint32_t macro, double normalized, bool tellDaw) override {
		self.macroValues_[macro].store(normalized);
		if (!tellDaw || !self.outerOut_) return; // the DAW's own values aren't echoed
		clap_event_param_value e{{sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, macro, nullptr, -1, -1, -1, -1, normalized};
		self.outerOut_->try_push(self.outerOut_, &e.header);
	}
	void dawGesture(uint32_t macro, bool begin) override {
		if (!self.outerOut_) return;
		clap_event_param_gesture g{{sizeof(g), time, CLAP_CORE_EVENT_SPACE_ID,
		                            uint16_t(begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END), 0}, macro};
		self.outerOut_->try_push(self.outerOut_, &g.header);
	}
};

void PluginCircuitPlugin::translateEvents(const clap_input_events *in, Slot *slot, const MacroMap *map, bool drainUi,
                                          bool drainNotes) {
	innerIn_.clear();
	if (!slot && !drainUi) { // the main thread is swapping the plugin: only keep the DAW's macro values, unless owned
		for (uint32_t i = 0, n = in ? in->size(in) : 0; i < n; ++i) {
			auto *h = in->get(in, i);
			if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
			auto *e = reinterpret_cast<const clap_event_param_value *>(h);
			if (e->param_id < kNumMacros && gestures_.macroOwner(e->param_id) == Owner::None) macroValues_[e->param_id].store(e->value);
		}
		return;
	}
	ParamTable *table = slot ? slot->params.load(std::memory_order_acquire) : nullptr;
	EventSink sink(*this, table);
	gestures_.begin(table, sink); // closes gestures left open (a new table, unload, reset, the editor closing)
	// UI-originated events first: they are stamped at time 0, so the list stays sorted
	UiEvent ui;
	uint64_t applied = 0;
	while (drainUi && uiEvents_.pop(ui)) {
		if (ui.kind == UiEvent::Macro) gestures_.uiMacro(map, ui.id, ui.value, ui.phase, sink);
		else if (ui.kind == UiEvent::Param) gestures_.uiParam(map, ui.id, ui.value, ui.phase, sink);
		++applied;
	}
	if (applied) uiEventsApplied_.fetch_add(applied, std::memory_order_release); // after the table holds them
	while (drainNotes && uiNotes_.pop(ui)) {
		clap_event_note n{{sizeof(n), 0, CLAP_CORE_EVENT_SPACE_ID, uint16_t(ui.kind == UiEvent::NoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0},
		                  -1, 0, 0, int16_t(ui.id), ui.value};
		innerIn_.push(&n.header);
	}
	if (!in) return;
	auto count = in->size(in);
	for (uint32_t i = 0; i < count; ++i) {
		auto *h = in->get(in, i);
		if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
		if (h->type == CLAP_EVENT_PARAM_VALUE) {
			auto *e = reinterpret_cast<const clap_event_param_value *>(h);
			sink.time = h->time;
			gestures_.dawMacro(slot ? map : nullptr, e->param_id, e->value, sink);
		} else if (h->type == CLAP_EVENT_PARAM_MOD || h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
			continue; // macros aren't modulatable yet; gestures go from a plugin to its host, never back
		} else {
			if (showDawNotes_.load(std::memory_order_relaxed)) noteForKeyboard(h);
			innerIn_.push(h); // notes, note expressions, MIDI, transport
		}
	}
}

// [audio-thread] (or a flush): a DAW note for the on-screen keyboard. Lock-free; a full queue drops the note and
// later clears the keyboard (all off) rather than leaving a key lit.
void PluginCircuitPlugin::noteForKeyboard(const clap_event_header *h) {
	int key = -2;
	bool on = false;
	if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE) {
		auto *n = reinterpret_cast<const clap_event_note *>(h);
		key = n->key < 0 ? -1 : n->key;
		on = h->type == CLAP_EVENT_NOTE_ON && n->velocity > 0;
	} else if (h->type == CLAP_EVENT_MIDI) {
		auto *m = reinterpret_cast<const clap_event_midi *>(h);
		const uint8_t status = m->data[0] & 0xF0;
		if (status == 0x90 || status == 0x80) {
			key = m->data[1] & 0x7F;
			on = status == 0x90 && m->data[2] > 0;
		} else if (status == 0xB0 && (m->data[1] == 123 || m->data[1] == 120)) {
			key = -1; // all notes / all sound off
		}
	}
	if (key < -1 || key > 127) return; // not a note event (or out of range)
	if (!dawNotes_.push({int16_t(key), on})) dawNotesOverflow_.store(true, std::memory_order_relaxed);
}

void PluginCircuitPlugin::showDawNotes(bool on) {
	showDawNotes_.store(on, std::memory_order_relaxed);
	if (!on) takeDawNotes([](const DawNote &) {}); // stale notes don't light the keyboard when it opens again
}

bool PluginCircuitPlugin::innerOutTryPush(const clap_output_events *list, const clap_event_header *h) {
	auto *self = static_cast<PluginCircuitPlugin *>(list->ctx);
	auto *out = self->outerOut_;
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
	// the plugin changed or gestures its own parameter (its GUI, its DSP): through the state machine. current_ is
	// stable here: called from inside its process (innerLock_ held) or from params.flush on the main thread.
	if (h->type == CLAP_EVENT_PARAM_VALUE || h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
		auto *map = self->macros_.load(std::memory_order_acquire);
		EventSink sink(*self, self->current_ ? self->current_->params.load(std::memory_order_acquire) : nullptr);
		sink.time = h->time;
		if (h->type == CLAP_EVENT_PARAM_VALUE) {
			auto *e = reinterpret_cast<const clap_event_param_value *>(h);
			self->gestures_.pluginValue(map, e->param_id, e->value, sink);
		} else {
			auto *g = reinterpret_cast<const clap_event_param_gesture *>(h);
			self->gestures_.pluginGesture(map, g->param_id, h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN, sink);
		}
		return true;
	}
	return out ? out->try_push(out, h) : true; // notes, MIDI
}

const clap_event_transport *PluginCircuitPlugin::transportFor(const clap_process *p) {
	const bool hostTransport = p->transport != nullptr;
	if (hostTransport != hostHasTransport_.load(std::memory_order_relaxed)) {
		hostHasTransport_.store(hostTransport, std::memory_order_relaxed);
		stateVersion_++; // the UI shows its play button only without a host transport
	}
	if (hostTransport) {
		fallbackSamples_ = 0;
		return p->transport;
	}
	if (fallbackRewind_.exchange(false, std::memory_order_acquire)) fallbackSamples_ = 0;
	const bool playing = fallbackPlaying_.load(std::memory_order_relaxed);
	constexpr double kTempo = 120.0, kBeatsPerBar = 4.0;
	const double seconds = double(fallbackSamples_) / sampleRate_;
	const double beats = seconds * kTempo / 60.0;
	auto &t = fallbackTransport_;
	t = {};
	t.header = {sizeof(t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
	t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
	          CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
	t.song_pos_beats = clap_beattime(std::llround(beats * double(CLAP_BEATTIME_FACTOR)));
	t.song_pos_seconds = clap_sectime(std::llround(seconds * double(CLAP_SECTIME_FACTOR)));
	t.tempo = kTempo;
	const double bar = std::floor(beats / kBeatsPerBar);
	t.bar_start = clap_beattime(std::llround(bar * kBeatsPerBar * double(CLAP_BEATTIME_FACTOR)));
	t.bar_number = int32_t(bar);
	t.tsig_num = 4;
	t.tsig_denom = 4;
	if (playing) fallbackSamples_ += p->frames_count;
	return &t;
}

clap_process_status PluginCircuitPlugin::process(const clap_process *p) {
	const auto t0 = std::chrono::steady_clock::now(); // a user-space counter read: no syscall
	const auto status = processBlock(p);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	const double budget = sampleRate_ > 0 ? double(p->frames_count) / sampleRate_ : 0;
	if (budget > 0) {
		const auto ppm = uint32_t(std::min(seconds / budget, 100.0) * 1e6);
		performance.loadPpmSum.fetch_add(ppm, std::memory_order_relaxed);
		performance.blocks.fetch_add(1, std::memory_order_relaxed);
		if (seconds > budget) performance.overBudget.fetch_add(1, std::memory_order_relaxed);
		uint32_t peak = performance.peakPpm.load(std::memory_order_relaxed);
		while (ppm > peak && !performance.peakPpm.compare_exchange_weak(peak, ppm, std::memory_order_relaxed)) {
		}
		performance.blockFrames.store(p->frames_count, std::memory_order_relaxed);
	}
	return status;
}

clap_process_status PluginCircuitPlugin::processBlock(const clap_process *p) {
	outerOut_ = p->out_events;
	const MacroMap *map = macros_.load(std::memory_order_acquire);

	if (innerLock_.test_and_set(std::memory_order_acquire)) { // main thread is swapping
		translateEvents(p->in_events, nullptr, nullptr, false, false); // UI events wait for the next block
		passThrough(p);
		outerOut_ = nullptr;
		processEpoch_.fetch_add(1, std::memory_order_release);
		return CLAP_PROCESS_CONTINUE;
	}

	Slot *slot = current_;
	if (slot != audioStarted_) {
		if (audioStarted_ && audioStarted_->started) {
			audioStarted_->inner->stopProcessing();
			audioStarted_->started = false;
		}
		audioStarted_ = slot;
	}
	if (slot && !slot->started && slot->inner->isActive()) slot->started = slot->inner->startProcessing();

	translateEvents(p->in_events, slot, map, true, true);
	clap_process_status status = CLAP_PROCESS_CONTINUE; // the WCLAP's status (its tail / sleep), when it ran
	if (!slot || !slot->started || p->audio_outputs_count == 0) {
		passThrough(p);
	} else {
		auto &out = p->audio_outputs[0];
		const float *const *in = nullptr;
		uint32_t inChannels = 0;
		if (!instrument_ && p->audio_inputs_count > 0) {
			in = p->audio_inputs[0].data32;
			inChannels = p->audio_inputs[0].channel_count;
		}
		status = slot->inner->process(in, inChannels, out.data32, out.channel_count, p->frames_count, p->steady_time,
		                              transportFor(p), &innerIn_.list, &innerOut_);
		if (status == CLAP_PROCESS_ERROR) status = CLAP_PROCESS_CONTINUE; // the WCLAP's error isn't the DAW's to handle
		if (tailDirty_.exchange(false, std::memory_order_relaxed)) {
			tailSamples_.store(slot->inner->tail(), std::memory_order_relaxed);
			tailNotify_.store(true, std::memory_order_relaxed);
		}
	}
	if (tailNotify_.exchange(false, std::memory_order_relaxed) && hostTail_) hostTail_->changed(host_);
	lastStatus_.store(status, std::memory_order_relaxed);
	unlockInner();
	outerOut_ = nullptr;
	processEpoch_.fetch_add(1, std::memory_order_release);
	return status;
}

// ---- extensions ------------------------------------------------------------------------------------
const void *PluginCircuitPlugin::getExtension(const char *id) {
	if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &PluginGlue::audioPorts;
	if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &PluginGlue::notePorts;
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &PluginGlue::params;
	if (!std::strcmp(id, CLAP_EXT_STATE)) return &PluginGlue::state;
	if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &PluginGlue::latency;
	if (!std::strcmp(id, CLAP_EXT_TAIL)) return &PluginGlue::tail;
	if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &PluginGlue::timer;
	if (!std::strcmp(id, CLAP_EXT_GUI)) return &PluginGlue::gui;
	return nullptr;
}

uint32_t PluginCircuitPlugin::audioPortsCount(bool isInput) const { return isInput ? (instrument_ ? 0 : 1) : 1; }

bool PluginCircuitPlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info *info) const {
	if (index != 0 || (isInput && instrument_)) return false;
	*info = {};
	info->id = isInput ? 0 : 1;
	std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = instrument_ ? CLAP_INVALID_ID : (isInput ? 1 : 0);
	return true;
}

uint32_t PluginCircuitPlugin::notePortsCount(bool isInput) const { return isInput ? 1 : 1; }

bool PluginCircuitPlugin::notePortsGet(uint32_t index, bool isInput, clap_note_port_info *info) const {
	if (index != 0) return false;
	*info = {};
	info->id = isInput ? 0 : 1;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
	info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
	std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Notes In" : "Notes Out");
	return true;
}

bool PluginCircuitPlugin::paramsGetInfo(uint32_t index, clap_param_info *info) const {
	if (index >= kNumMacros) return false;
	auto *map = macros_.load(std::memory_order_acquire);
	*info = {};
	info->id = index;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	info->cookie = nullptr;
	std::snprintf(info->name, sizeof(info->name), "%s", map->displayName(index).c_str());
	std::snprintf(info->module, sizeof(info->module), "Macros");
	info->min_value = 0;
	info->max_value = 1;
	info->default_value = map->macros[index].defaultValue;
	return true;
}

bool PluginCircuitPlugin::paramsGetValue(clap_id id, double *value) const {
	if (id >= kNumMacros) return false;
	*value = macroValues_[id].load();
	return true;
}

bool PluginCircuitPlugin::paramsValueToText(clap_id id, double value, char *display, uint32_t size) const {
	if (id >= kNumMacros) return false;
	auto *map = macros_.load(std::memory_order_acquire);
	auto &m = map->macros[id];
	if (m.targets.size() == 1 && current_) {
		auto text = const_cast<PluginCircuitPlugin *>(this)->paramTextCached(m.targets[0].paramId, m.targets[0].map(value));
		std::snprintf(display, size, "%s", text.c_str());
		return true;
	}
	std::snprintf(display, size, "%.0f %%", value * 100.0);
	return true;
}

bool PluginCircuitPlugin::paramsTextToValue(clap_id id, const char *display, double *value) const {
	if (id >= kNumMacros) return false;
	char *end = nullptr;
	double v = std::strtod(display, &end);
	if (end == display) return false;
	if (std::strchr(display, '%')) v /= 100.0;
	*value = v < 0 ? 0 : (v > 1 ? 1 : v);
	return true;
}

void PluginCircuitPlugin::paramsFlush(const clap_input_events *in, const clap_output_events *out) {
	// Called when not processing ([active ? audio-thread : main-thread]); we hold no lock-free guarantees
	// beyond the try-lock, which is fine here.
	if (innerLock_.test_and_set(std::memory_order_acquire)) return;
	outerOut_ = out;
	auto *map = macros_.load(std::memory_order_acquire);
	translateEvents(in, current_, map, true, false);
	if (current_ && innerIn_.offsets.size()) current_->inner->paramsFlush(&innerIn_.list, &innerOut_);
	outerOut_ = nullptr;
	unlockInner();
}

uint32_t PluginCircuitPlugin::latencyGet() const { return current_ ? current_->inner->latency() : 0; }

void PluginCircuitPlugin::onTimer(clap_id timerId) {
	if (timerId == housekeepingTimer_) {
		housekeeping();
		return;
	}
	if (current_) current_->inner->onTimer(timerId);
}

void PluginCircuitPlugin::onMainThread() {
	if (current_) current_->inner->onMainThread();
	housekeeping();
}

// ---- state ------------------------------------------------------------------------------------------
bool PluginCircuitPlugin::stateSave(const clap_ostream *stream) {
	std::vector<uint8_t> bytes;
	if (!current_ && missingState_) {
		auto kept = *missingState_; // a plugin we couldn't load: keep its state untouched
		kept.instance = instance_;
		bytes = encodeState(kept);
	} else {
		HostState s;
		if (current_) {
			auto &meta = current_->meta;
			s.pluginId = current_->inner->pluginId();
			s.pluginName = meta.name;
			s.vendor = meta.vendor;
			s.packageId = meta.packageId;
			s.packageSha256 = meta.sha256;
			s.moduleSha256 = meta.moduleSha256;
			current_->inner->saveState(s.innerState);
			PackageCache::markSaved(meta.sha256); // the package is in a DAW session now
			if (auto *table = current_->params.load(std::memory_order_acquire))
				for (size_t i = 0; i < table->size(); ++i)
					if (table->info(i).automatable()) s.innerParams[table->info(i).id] = table->value(i);
		}
		s.macros = *macros_.load();
		for (uint32_t i = 0; i < kNumMacros; ++i) s.macroValues[i] = macroValues_[i].load();
		s.instance = instance_;
		bytes = encodeState(s);
	}
	size_t written = 0;
	while (written < bytes.size()) {
		auto n = stream->write(stream, bytes.data() + written, bytes.size() - written);
		if (n <= 0) return false;
		written += (size_t)n;
	}
	return true;
}

bool PluginCircuitPlugin::stateLoad(const clap_istream *stream) {
	std::vector<uint8_t> bytes;
	uint8_t buf[4096];
	while (true) {
		auto n = stream->read(stream, buf, sizeof(buf));
		if (n < 0) return false;
		if (n == 0) break;
		bytes.insert(bytes.end(), buf, buf + n);
	}
	std::string error;
	auto state = decodeState(bytes, error);
	if (!state) {
		setStatus(Status::Error, error);
		return false;
	}
	setInstance(state->instance); // ours, whatever happens to the plugin
	if (state->empty()) {
		uiUnload();
		publishMacros(state->macros);
		for (uint32_t i = 0; i < kNumMacros; ++i) macroValues_[i].store(state->macroValues[i]);
		if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
		stateVersion_++;
		return true;
	}
	// same plugin already loaded: apply in place
	if (current_ && current_->inner->pluginId() == state->pluginId && current_->meta.moduleSha256 == state->moduleSha256) {
		applyHostState(*current_->inner, *state);
		if (auto *table = paramTable()) refreshParamValues(*table, *current_->inner); // the state changed them
		return true;
	}
	LoadJob job;
	job.pluginId = state->pluginId;
	job.sha256 = state->packageSha256;
	job.state = std::move(*state);
	startLoad(std::move(job));
	return true;
}

void PluginCircuitPlugin::applyHostState(InnerPlugin &inner, const HostState &s) {
	bool loaded = !s.innerState.empty() && inner.loadState(s.innerState);
	if (!loaded && !s.innerParams.empty()) {
		// fall back to the parameter snapshot
		EventBuffer buf;
		buf.storage.reserve(s.innerParams.size() * sizeof(clap_event_param_value) + 64);
		buf.offsets.reserve(s.innerParams.size() + 1);
		buf.list = innerIn_.list;
		buf.list.ctx = &buf;
		for (auto &[id, v] : s.innerParams) {
			clap_event_param_value e{{sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, id, nullptr, -1, -1, -1, -1, v};
			buf.push(&e.header);
		}
		clap_output_events sink{nullptr, [](const clap_output_events *, const clap_event_header *) { return true; }};
		if (!inner.isProcessing()) inner.paramsFlush(&buf.list, &sink);
	}
	publishMacros(s.macros);
	for (uint32_t i = 0; i < kNumMacros; ++i) macroValues_[i].store(s.macroValues[i]);
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
	stateVersion_++;
}

// ---- loading ------------------------------------------------------------------------------------------
void PluginCircuitPlugin::setStatus(Status s, std::string message) {
	{
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusMessage_ = std::move(message);
	}
	status_.store((int)s);
	stateVersion_++;
	if (s == Status::Error || s == Status::Missing) {
		std::lock_guard<std::mutex> lock(statusMutex_);
		reportError("load", statusMessage_.empty() ? std::string(s == Status::Missing ? "missing" : "error") : statusMessage_);
	}
}

void PluginCircuitPlugin::reportError(const std::string &source, const std::string &message) {
	const double now = std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::lock_guard<std::mutex> lock(errorsMutex_);
	if (errors_.size() >= 30) errors_.pop_front();
	errors_.push_back({now, source, message});
	++errorsVersion_;
}

choc::value::Value PluginCircuitPlugin::errorsValue() {
	auto list = choc::value::createEmptyArray();
	std::lock_guard<std::mutex> lock(errorsMutex_);
	for (auto it = errors_.rbegin(); it != errors_.rend(); ++it) // newest first
		list.addArrayElement(choc::value::createObject("E", "time", it->time, "source", it->source, "message", it->message));
	return list;
}

void PluginCircuitPlugin::clearErrors() {
	std::lock_guard<std::mutex> lock(errorsMutex_);
	errors_.clear();
	++errorsVersion_;
}

std::string PluginCircuitPlugin::moduleError() const {
	return current_ && current_->inner->module() ? current_->inner->module()->error() : std::string();
}

void PluginCircuitPlugin::startLoad(LoadJob job) {
	if (loading_.exchange(true)) {
		if (hostLog_) hostLog_->log(host_, CLAP_LOG_WARNING, "Plugin Circuit: a load is already running");
		loading_ = true;
		return;
	}
	if (loadThread_.joinable()) loadThread_.join();
	missingState_.reset();
	setStatus(Status::Fetching, job.pluginId);
	downloadProgress_ = 0;
	loadThread_ = std::thread([this, job = std::move(job)]() mutable {
		auto result = std::make_unique<LoadJob>(std::move(job));
		std::string error;
		CatalogPlugin meta;
		// 1. metadata (catalog first; offline falls back to the local cache)
		if (auto fetched = Catalog::fetchPlugin(result->pluginId, error)) {
			meta = *fetched;
		} else {
			for (auto &p : Catalog::installedPlugins())
				if (p.id == result->pluginId && (result->sha256.empty() || p.sha256 == result->sha256)) meta = p;
			if (meta.id.empty() && !result->sha256.empty() && !Catalog::cachedPackage(result->sha256).empty()) {
				meta.id = result->pluginId;
				meta.sha256 = result->sha256;
				if (result->state) {
					meta.name = result->state->pluginName;
					meta.vendor = result->state->vendor;
					meta.packageId = result->state->packageId;
					meta.moduleSha256 = result->state->moduleSha256;
				}
			}
		}
		if (meta.id.empty()) {
			result->error = "not in the catalog or the local cache: " + error;
			result->missing = true;
		} else {
			// a state from another session pins the exact package
			if (result->state && !result->state->packageSha256.empty()) meta.sha256 = result->state->packageSha256;
			setStatus(Status::Downloading, meta.name);
			auto dir = Catalog::ensurePackage(meta.sha256, [this](uint64_t got, uint64_t total) {
				downloadProgress_ = total ? double(got) / double(total) : 0.0; // partial snapshots carry it
				return true;
			}, error);
			if (dir.empty()) {
				result->error = "download failed: " + error;
				result->missing = true;
			} else {
				// hold the package (a delete from the settings page skips it from now on); if a delete got it first,
				// fetch it again and hold that
				result->inUse = PackageCache::InUse(meta.sha256);
				for (int attempt = 0; !result->inUse.held() && attempt < 3 && !dir.empty(); ++attempt) {
					dir = Catalog::ensurePackage(meta.sha256, {}, error);
					if (!dir.empty()) result->inUse = PackageCache::InUse(meta.sha256);
				}
			}
			if (dir.empty() || !result->inUse.held()) {
				result->error = "download failed: " + (error.empty() ? std::string("the package was deleted while loading") : error);
				result->missing = true;
			} else {
				PackageCache::markUsed(meta.sha256);
				Catalog::writeMeta(meta.sha256, meta);
				setStatus(Status::Compiling, meta.name);
				result->module = WclapLibrary::open(dir.string(), Paths::pluginData(meta.packageId.empty() ? meta.id : meta.packageId).string(), error);
				if (!result->module) result->error = error;
			}
		}
		result->meta = meta;
		{
			std::lock_guard<std::mutex> lock(jobMutex_);
			finishedJob_ = std::move(result);
		}
		host_->request_callback(host_);
	});
}

InnerHostCallbacks PluginCircuitPlugin::makeInnerCallbacks() {
	InnerHostCallbacks cb;
	cb.requestCallback = [this] { host_->request_callback(host_); };
	cb.requestRestart = [this] { host_->request_restart(host_); };
	cb.requestProcess = [this] { host_->request_process(host_); };
	cb.paramsRescan = [this](clap_param_rescan_flags flags) {
		// called on the main thread by the inner plugin: the table follows (info: a new table; values: re-read)
		if (current_) {
			if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO)) buildParamTable(*current_);
			else if (flags & CLAP_PARAM_RESCAN_VALUES) {
				if (auto *table = paramTable()) refreshParamValues(*table, *current_->inner);
			}
			if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO)) textCache_.clear();
		}
		if (auto *map = macros_.load(); map && map->autoMapped && (flags & CLAP_PARAM_RESCAN_ALL) && current_ && paramTable()) {
			publishMacros(MacroMap::autoMap(paramTable()->infos()));
			if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
		} else if (hostParams_ && (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT))) {
			hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
		}
		stateVersion_++;
	};
	cb.paramsRequestFlush = [this] { if (hostParams_) hostParams_->request_flush(host_); };
	cb.stateMarkDirty = [this] { markDirty(); };
	cb.latencyChanged = [this] { host_->request_restart(host_); };
	cb.tailChanged = [this] { tailDirty_.store(true, std::memory_order_relaxed); }; // [audio-thread]: handled after its process
	cb.guiRequestResize = [this](uint32_t w, uint32_t h) {
		if (!isMainThread()) { // [thread-safe]: hand it to the main thread; the editor may be closing right now
			pendingInnerResize.store((uint64_t(w) << 32) | h, std::memory_order_release);
			return true;
		}
		return editor_ ? editor_->innerRequestResize(w, h) : false;
	};
	cb.registerTimer = [this](uint32_t period, clap_id *id) {
		return hostTimer_ && hostTimer_->register_timer(host_, period, id);
	};
	cb.unregisterTimer = [this](clap_id id) { return hostTimer_ && hostTimer_->unregister_timer(host_, id); };
	cb.log = [this](clap_log_severity s, const char *msg) {
		if (hostLog_) hostLog_->log(host_, s, msg);
	};
	return cb;
}

void PluginCircuitPlugin::finishLoad(std::unique_ptr<LoadJob> job) {
	loading_ = false;
	installedAt_ = {}; // the load may have added a package
	if (!job->module) {
		if (job->state) missingState_ = std::move(job->state); // keep the session's data
		setStatus(job->missing ? Status::Missing : Status::Error, job->error);
		return;
	}
	std::string error;
	auto inner = InnerPlugin::create(job->module, job->pluginId, makeInnerCallbacks(), error);
	if (!inner) {
		if (job->state) missingState_ = std::move(job->state);
		setStatus(Status::Error, error);
		return;
	}
	if (active_ && !inner->activate(sampleRate_, minFrames_, maxFrames_)) {
		setStatus(Status::Error, "activate failed");
		return;
	}
	auto slot = std::make_unique<Slot>();
	slot->meta = job->meta;
	slot->inUse = std::move(job->inUse);
	slot->inner = std::move(inner);

	buildParamTable(*slot); // the plugin's parameters, read once
	auto *table = slot->params.load(std::memory_order_relaxed);
	if (job->state) {
		applyHostState(*slot->inner, *job->state);
		refreshParamValues(*table, *slot->inner); // the state changed them
	} else {
		auto map = MacroMap::autoMap(table->infos());
		for (uint32_t i = 0; i < kNumMacros; ++i) {
			double v = map.macros[i].defaultValue;
			if (map.macros[i].targets.size() == 1) {
				double cur;
				if (table->valueById(map.macros[i].targets[0].paramId, cur)) v = map.macros[i].targets[0].unmap(cur);
			}
			macroValues_[i].store(v);
		}
		publishMacros(std::move(map));
	}
	tailSamples_.store(slot->inner->tail(), std::memory_order_relaxed);
	tailNotify_.store(true, std::memory_order_relaxed);
	swapIn(std::move(slot));
	setStatus(Status::Ready, job->meta.name);
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
	if (hostLatency_ && active_) host_->request_restart(host_); // latency may have changed
	markDirty();
}

void PluginCircuitPlugin::swapIn(std::unique_ptr<Slot> slot) {
	uiAllNotesOff(); // the old plugin is going: don't leave the new one with notes nobody releases
	if (editor_) editor_->innerWillChange();
	Slot *old = current_;
	lockInner();
	current_ = slot.get();
	if (!processing_ && audioStarted_ == old && old && old->started) { // nobody will stop it on the audio thread
		old->inner->stopProcessing();
		old->started = false;
		audioStarted_ = nullptr;
	}
	unlockInner();
	if (owned_) retiredSlots_.push_back(std::move(owned_));
	owned_ = std::move(slot);
	if (editor_) editor_->innerDidChange();
	housekeeping();
}

void PluginCircuitPlugin::publishMacros(MacroMap map) {
	auto next = std::make_unique<MacroMap>(std::move(map));
	auto *prev = macros_.exchange(next.get(), std::memory_order_acq_rel);
	(void)prev;
	retiredMacros_.emplace_back(processEpoch_.load(std::memory_order_acquire), std::move(ownedMacros_));
	ownedMacros_ = std::move(next);
	stateVersion_++;
}

// ---- instance-scope settings (src/api/SettingsSchema.h): this instance, saved in our CLAP state
void PluginCircuitPlugin::setInstance(const choc::value::ValueView &stored) {
	instance_ = settings::sanitize(Scope::Instance, stored);
	++instanceVersion_;
}

std::string PluginCircuitPlugin::changeInstance(const choc::value::ValueView &change) {
	if (auto e = settings::apply(Scope::Instance, change, instance_); !e.empty()) return e;
	++instanceVersion_; // not markDirty(): a view or an octave isn't worth a "save changes?"; saved with the session
	return {};
}

void PluginCircuitPlugin::housekeeping() {
	Settings::poll(); // another process changed settings.json
	// errors: the bridge's diagnostics since the last look, and the module's own (sticky: reported once)
	for (const auto end = bridgelog::end(); bridgeCursor_ < end; ++bridgeCursor_) {
		if (end - bridgeCursor_ > 64) bridgeCursor_ = end - 64; // the ring's size: older ones are gone
		std::string message;
		if (bridgelog::read(bridgeCursor_, message)) reportError("bridge", message);
	}
	if (auto e = moduleError(); !e.empty() && e != lastModuleError_) {
		lastModuleError_ = e;
		reportError("plugin", e);
		stateVersion_++; // the plugin store shows it
	} else if (e.empty()) {
		lastModuleError_.clear();
	}
	std::unique_ptr<LoadJob> job;
	{
		std::lock_guard<std::mutex> lock(jobMutex_);
		job = std::move(finishedJob_);
	}
	if (job) {
		if (loadThread_.joinable()) loadThread_.join();
		finishLoad(std::move(job));
	}
	// free retired slots once the audio thread has let go of them
	bool audioIdle = !processing_.load();
	for (auto it = retiredSlots_.begin(); it != retiredSlots_.end();) {
		Slot *s = it->get();
		bool canFree;
		lockInner();
		canFree = audioStarted_ != s && !s->started;
		if (audioIdle && audioStarted_ == s) {
			if (s->started) s->inner->stopProcessing();
			s->started = false;
			audioStarted_ = nullptr;
			canFree = true;
		}
		unlockInner();
		if (canFree) {
			(*it)->inner->deactivate();
			it = retiredSlots_.erase(it);
		} else {
			++it;
		}
	}
	// free macro maps the audio thread can no longer be reading (two blocks later, or when idle)
	auto epoch = processEpoch_.load(std::memory_order_acquire);
	for (auto it = retiredTables_.begin(); it != retiredTables_.end();) {
		if (audioIdle || epoch > it->first + 1) it = retiredTables_.erase(it);
		else ++it;
	}
	for (auto it = retiredMacros_.begin(); it != retiredMacros_.end();) {
		if (audioIdle || epoch > it->first + 1) it = retiredMacros_.erase(it);
		else ++it;
	}
}

void PluginCircuitPlugin::markDirty() {
	if (hostState_) hostState_->mark_dirty(host_);
}

// ---- UI-facing --------------------------------------------------------------------------------------------
InnerPlugin *PluginCircuitPlugin::innerForGui() { return current_ ? current_->inner.get() : nullptr; }

bool PluginCircuitPlugin::requestHostResize(uint32_t w, uint32_t h) {
	return hostGui_ && hostGui_->request_resize(host_, w, h);
}

void PluginCircuitPlugin::uiLoad(const std::string &pluginId, const std::string &sha256) {
	LoadJob job;
	job.pluginId = pluginId;
	job.sha256 = sha256;
	startLoad(std::move(job));
}

void PluginCircuitPlugin::uiUnload() {
	tailSamples_.store(0, std::memory_order_relaxed); // no plugin, no tail
	tailNotify_.store(true, std::memory_order_relaxed);
	if (!current_ && !missingState_) return;
	uiAllNotesOff();
	if (editor_) editor_->innerWillChange();
	lockInner();
	Slot *old = current_;
	current_ = nullptr;
	if (!processing_ && audioStarted_ == old && old && old->started) {
		old->inner->stopProcessing();
		old->started = false;
		audioStarted_ = nullptr;
	}
	unlockInner();
	if (owned_) retiredSlots_.push_back(std::move(owned_));
	missingState_.reset();
	publishMacros(MacroMap{});
	for (auto &v : macroValues_) v.store(0);
	if (editor_) editor_->innerDidChange();
	setStatus(Status::Empty);
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
	markDirty();
	housekeeping();
}

void PluginCircuitPlugin::uiEdit(bool macro, uint32_t id, double value, Phase phase) {
	wakeIfSleeping(); // a WCLAP that returned SLEEP isn't processed until the DAW is asked
	if (macro) {
		if (id >= kNumMacros) return;
		value = value < 0 ? 0 : (value > 1 ? 1 : value);
	}
	std::lock_guard<std::mutex> lock(uiPushMutex_);
	const bool pushed = uiEvents_.push({macro ? UiEvent::Macro : UiEvent::Param, phase, id, value});
	if (pushed) ++uiEventsPushed_;
	// queue full (nothing processes or flushes): a lost end would leave the parameter ours for good, so close
	// the UI's gestures at the next process / flush instead (it drains the queue: the ack follows then too)
	if (!pushed && phase == Phase::End) gestures_.requestCloseUi();
	if (phase == Phase::End) {
		if (pendingEnds_.size() >= 256) pendingEnds_.pop_front(); // nobody reads them (the page isn't subscribed)
		pendingEnds_.push_back({macro, id, uiEventsPushed_});
	}
	if (!processing_ && hostParams_) hostParams_->request_flush(host_);
}

void PluginCircuitPlugin::uiNote(int16_t key, double velocity, bool on) {
	wakeIfSleeping(); // a WCLAP that returned SLEEP isn't processed until the DAW is asked
	if (key < 0 || key > 127) return;
	std::lock_guard<std::mutex> lock(uiPushMutex_);
	// A failed push (queue full) keeps the held bit as it was: a lost note-on is not held, a lost note-off still is,
	// so uiAllNotesOff() retries it.
	if (uiNotes_.push({on ? UiEvent::NoteOn : UiEvent::NoteOff, Phase::Set, uint32_t(key), std::clamp(velocity, 0.0, 1.0)}))
		uiHeldNotes_.set(size_t(key), on);
}

void PluginCircuitPlugin::uiAllNotesOff() {
	std::lock_guard<std::mutex> lock(uiPushMutex_);
	for (size_t k = 0; k < uiHeldNotes_.size(); ++k)
		if (uiHeldNotes_.test(k) && uiNotes_.push({UiEvent::NoteOff, Phase::Set, uint32_t(k), 0.0})) uiHeldNotes_.reset(k);
}

void PluginCircuitPlugin::uiMapMacro(uint32_t slot, clap_id paramId, double minValue, double maxValue, bool add) {
	if (slot >= kNumMacros || !current_) return;
	MacroMap map = *macros_.load();
	map.autoMapped = false;
	auto &m = map.macros[slot];
	bool stepped = false;
	std::string name;
	if (auto *table = paramTable())
		if (const int i = table->indexOf(paramId); i >= 0) {
			stepped = table->info(size_t(i)).stepped();
			name = table->info(size_t(i)).name;
		}
	if (!add) m.targets.clear();
	m.targets.erase(std::remove_if(m.targets.begin(), m.targets.end(), [&](auto &t) { return t.paramId == paramId; }), m.targets.end());
	m.targets.push_back({paramId, minValue, maxValue, stepped});
	if (!m.customName) m.name = m.targets.size() == 1 ? name : name + " +" + std::to_string(m.targets.size() - 1);
	publishMacros(std::move(map));
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
	markDirty();
}

void PluginCircuitPlugin::uiUnmapMacro(uint32_t slot, int64_t paramId) {
	if (slot >= kNumMacros) return;
	MacroMap map = *macros_.load();
	map.autoMapped = false;
	auto &m = map.macros[slot];
	if (paramId < 0) m.targets.clear();
	else m.targets.erase(std::remove_if(m.targets.begin(), m.targets.end(), [&](auto &t) { return t.paramId == (clap_id)paramId; }), m.targets.end());
	if (m.targets.empty() && !m.customName) m.name.clear();
	publishMacros(std::move(map));
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
	markDirty();
}

void PluginCircuitPlugin::uiRenameMacro(uint32_t slot, const std::string &name) {
	if (slot >= kNumMacros) return;
	MacroMap map = *macros_.load();
	map.macros[slot].name = name;
	map.macros[slot].customName = !name.empty();
	publishMacros(std::move(map));
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO);
	markDirty();
}

void PluginCircuitPlugin::uiSetTransport(bool playing) {
	wakeIfSleeping(); // a WCLAP that returned SLEEP isn't processed until the DAW is asked
	if (playing && !fallbackPlaying_.load()) fallbackRewind_.store(true, std::memory_order_release); // play starts at bar 1
	fallbackPlaying_.store(playing);
	stateVersion_++;
}

void PluginCircuitPlugin::uiAutoMap() {
	auto *table = paramTable();
	if (!current_ || !table) return;
	publishMacros(MacroMap::autoMap(table->infos()));
	auto *map = macros_.load();
	for (uint32_t i = 0; i < kNumMacros; ++i) {
		double v = map->macros[i].defaultValue, cur;
		if (map->macros[i].targets.size() == 1 && table->valueById(map->macros[i].targets[0].paramId, cur))
			v = map->macros[i].targets[0].unmap(cur);
		macroValues_[i].store(v);
	}
	if (hostParams_) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
	markDirty();
}

const std::vector<CatalogPlugin> &PluginCircuitPlugin::installedPlugins() {
	auto now = std::chrono::steady_clock::now();
	if (now - installedAt_ > std::chrono::seconds(5)) { // packages installed by another instance show up too
		installedCache_ = Catalog::installedPlugins();
		installedAt_ = now;
	}
	return installedCache_;
}

// ---- the parameter table ---------------------------------------------------------------------------------
void PluginCircuitPlugin::refreshParamValues(ParamTable &table, InnerPlugin &inner) {
	for (size_t i = 0; i < table.size(); ++i) {
		double v;
		if (inner.paramValue(table.info(i).id, v)) table.set(i, v);
	}
}

void PluginCircuitPlugin::buildParamTable(Slot &slot) {
	auto next = std::make_unique<ParamTable>(slot.inner->params());
	refreshParamValues(*next, *slot.inner);
	next->markAllChanged();
	auto *prev = slot.params.exchange(next.get(), std::memory_order_acq_rel);
	(void)prev; // the audio thread may still be reading it: retired below, freed after its next block
	if (slot.ownedParams) retiredTables_.emplace_back(processEpoch_.load(std::memory_order_acquire), std::move(slot.ownedParams));
	slot.ownedParams = std::move(next);
	textCache_.clear();
	stateVersion_++;
}

std::string PluginCircuitPlugin::paramTextCached(clap_id id, double value) {
	const TextKey key{id, std::bit_cast<uint64_t>(value)};
	if (auto it = textCache_.find(key); it != textCache_.end()) return it->second;
	auto *inner = innerForGui();
	std::string text = inner ? inner->paramText(id, value) : std::string();
	if (textCache_.size() >= 8192) textCache_.clear(); // bounded: values seen recently come back quickly
	textCache_.emplace(key, text);
	return text;
}

void PluginCircuitPlugin::reconcileParams(size_t count) {
	auto *table = paramTable();
	if (!table || !current_ || table->size() == 0) return;
	for (size_t n = 0; n < std::min(count, table->size()); ++n) {
		if (reconcileNext_ >= table->size()) reconcileNext_ = 0;
		const size_t i = reconcileNext_++;
		double v;
		if (current_->inner->paramValue(table->info(i).id, v)) table->set(i, v); // marks it changed if the plugin moved it
	}
}


bool PluginCircuitPlugin::wantsText(clap_id id) const {
	if (std::binary_search(visibleParams_.begin(), visibleParams_.end(), id)) return true;
	if (auto *map = macros_.load(std::memory_order_acquire)) // macro targets: the Macros view shows their text
		for (auto &m : map->macros)
			for (auto &t : m.targets)
				if (t.paramId == id) return true;
	return false;
}

void PluginCircuitPlugin::setVisibleParams(std::vector<clap_id> ids) {
	std::sort(ids.begin(), ids.end());
	for (auto id : ids) // newly visible: a fresh text on the next update (the page kept a possibly stale one)
		if (!std::binary_search(visibleParams_.begin(), visibleParams_.end(), id)) queueText(id);
	visibleParams_ = std::move(ids);
}

// once per id: the queue stays bounded by what is visible (plus macro targets), however often values change
void PluginCircuitPlugin::queueText(clap_id id) {
	if (std::find(pendingTexts_.begin(), pendingTexts_.end(), id) == pendingTexts_.end()) pendingTexts_.push_back(id);
}

} // namespace plugincircuit
