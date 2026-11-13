#include "core/InnerHost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace plugincircuit {

// ---- host extension tables: host_data -> InnerPlugin ---------------------------------
struct InnerHostExt {
	static InnerHostCallbacks &cb(const clap_host *h) { return static_cast<InnerPlugin *>(h->host_data)->cb_; }

	static constexpr clap_host_log log = {
		.log = [](const clap_host *h, clap_log_severity severity, const char *msg) {
			auto &c = cb(h);
			if (c.log) c.log(severity, msg ? msg : "");
			else std::fprintf(stderr, "[wclap %d] %s\n", (int)severity, msg ? msg : "");
		},
	};
	static constexpr clap_host_thread_check threadCheck = {
		.is_main_thread = [](const clap_host *h) { auto &c = cb(h); return c.isMainThread ? c.isMainThread() : true; },
		.is_audio_thread = [](const clap_host *h) { auto &c = cb(h); return c.isAudioThread ? c.isAudioThread() : false; },
	};
	static constexpr clap_host_params params = {
		.rescan = [](const clap_host *h, clap_param_rescan_flags flags) { auto &c = cb(h); if (c.paramsRescan) c.paramsRescan(flags); },
		.clear = [](const clap_host *, clap_id, clap_param_clear_flags) {},
		.request_flush = [](const clap_host *h) { auto &c = cb(h); if (c.paramsRequestFlush) c.paramsRequestFlush(); },
	};
	static constexpr clap_host_state state = {
		.mark_dirty = [](const clap_host *h) { auto &c = cb(h); if (c.stateMarkDirty) c.stateMarkDirty(); },
	};
	static constexpr clap_host_latency latency = {
		.changed = [](const clap_host *h) { auto &c = cb(h); if (c.latencyChanged) c.latencyChanged(); },
	};
	static constexpr clap_host_gui gui = {
		.resize_hints_changed = [](const clap_host *) {},
		.request_resize = [](const clap_host *h, uint32_t w, uint32_t hh) {
			auto &c = cb(h);
			return c.guiRequestResize ? c.guiRequestResize(w, hh) : false;
		},
		.request_show = [](const clap_host *) { return false; },
		.request_hide = [](const clap_host *) { return false; },
		.closed = [](const clap_host *, bool) {},
	};
	static constexpr clap_host_timer_support timer = {
		.register_timer = [](const clap_host *h, uint32_t period, clap_id *id) {
			auto *self = static_cast<InnerPlugin *>(h->host_data);
			if (!self->cb_.registerTimer || !self->cb_.registerTimer(period, id)) return false;
			self->timers_.push_back(*id);
			return true;
		},
		.unregister_timer = [](const clap_host *h, clap_id id) {
			auto *self = static_cast<InnerPlugin *>(h->host_data);
			self->timers_.erase(std::remove(self->timers_.begin(), self->timers_.end(), id), self->timers_.end());
			return self->cb_.unregisterTimer ? self->cb_.unregisterTimer(id) : false;
		},
	};
	static constexpr clap_host_audio_ports audioPorts = {
		.is_rescan_flag_supported = [](const clap_host *, uint32_t) { return false; },
		.rescan = [](const clap_host *, uint32_t) {},
	};
	static constexpr clap_host_note_ports notePorts = {
		.supported_dialects = [](const clap_host *) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
		.rescan = [](const clap_host *, uint32_t) {},
	};
	static constexpr clap_host_tail tail = {
		.changed = [](const clap_host *) {},
	};
};

const void *InnerPlugin::getExtension(const clap_host *, const char *id) {
	if (!std::strcmp(id, CLAP_EXT_LOG)) return &InnerHostExt::log;
	// thread-check is deliberately not offered: wclap-bridge (cd11d22) exposes it to the WCLAP, but calling it
	// from the bridge's audio-thread instance traps (out-of-bounds table access), e.g. clap-helpers in activate().
	(void)InnerHostExt::threadCheck;
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &InnerHostExt::params;
	if (!std::strcmp(id, CLAP_EXT_STATE)) return &InnerHostExt::state;
	if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &InnerHostExt::latency;
	if (!std::strcmp(id, CLAP_EXT_GUI)) return &InnerHostExt::gui;
	if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &InnerHostExt::timer;
	if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &InnerHostExt::audioPorts;
	if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &InnerHostExt::notePorts;
	if (!std::strcmp(id, CLAP_EXT_TAIL)) return &InnerHostExt::tail;
	return nullptr;
}

void InnerPlugin::requestRestart(const clap_host *h) {
	auto &c = from(h)->cb_;
	if (c.requestRestart) c.requestRestart();
}
void InnerPlugin::requestProcess(const clap_host *h) {
	auto &c = from(h)->cb_;
	if (c.requestProcess) c.requestProcess();
}
void InnerPlugin::requestCallback(const clap_host *h) {
	auto *self = from(h);
	self->callbackRequested_.store(true);
	if (self->cb_.requestCallback) self->cb_.requestCallback();
}

// ---- lifecycle --------------------------------------------------------------------------
std::unique_ptr<InnerPlugin> InnerPlugin::create(std::shared_ptr<WclapModule> module, const std::string &pluginId,
                                                 InnerHostCallbacks callbacks, std::string &error) {
	if (!module || !module->factory()) {
		error = "no module";
		return nullptr;
	}
	std::unique_ptr<InnerPlugin> self(new InnerPlugin());
	self->module_ = std::move(module);
	self->pluginId_ = pluginId;
	self->cb_ = std::move(callbacks);
	self->host_ = clap_host{
		.clap_version = CLAP_VERSION,
		.host_data = self.get(),
		.name = "Plugin Circuit",
		.vendor = "Plugin Circuit",
		.url = "https://github.com/WebCLAP",
		.version = "0.1.0",
		.get_extension = &InnerPlugin::getExtension,
		.request_restart = &InnerPlugin::requestRestart,
		.request_process = &InnerPlugin::requestProcess,
		.request_callback = &InnerPlugin::requestCallback,
	};
	auto *factory = self->module_->factory();
	self->plugin_ = factory->create_plugin(factory, &self->host_, pluginId.c_str());
	if (!self->plugin_) {
		error = "create_plugin failed for " + pluginId;
		return nullptr;
	}
	if (!self->plugin_->init(self->plugin_)) {
		error = "plugin init failed for " + pluginId;
		self->plugin_->destroy(self->plugin_);
		self->plugin_ = nullptr;
		return nullptr;
	}
	self->initialized_ = true;
	auto ext = [&](const char *id) { return self->plugin_->get_extension(self->plugin_, id); };
	self->params_ = (const clap_plugin_params *)ext(CLAP_EXT_PARAMS);
	self->state_ = (const clap_plugin_state *)ext(CLAP_EXT_STATE);
	self->gui_ = (const clap_plugin_gui *)ext(CLAP_EXT_GUI);
	self->latency_ = (const clap_plugin_latency *)ext(CLAP_EXT_LATENCY);
	self->audioPorts_ = (const clap_plugin_audio_ports *)ext(CLAP_EXT_AUDIO_PORTS);
	self->notePorts_ = (const clap_plugin_note_ports *)ext(CLAP_EXT_NOTE_PORTS);
	self->timerSupport_ = (const clap_plugin_timer_support *)ext(CLAP_EXT_TIMER_SUPPORT);
	self->readPortLayout();
	return self;
}

InnerPlugin::~InnerPlugin() {
	if (!plugin_) return;
	if (processing_) stopProcessing(); // should have happened on the audio thread already
	if (active_) deactivate();
	for (auto id : std::vector<clap_id>(timers_))
		if (cb_.unregisterTimer) cb_.unregisterTimer(id);
	plugin_->destroy(plugin_);
	plugin_ = nullptr;
}

void InnerPlugin::readPortLayout() {
	inPortChannels_.clear();
	outPortChannels_.clear();
	mainInPort_ = mainOutPort_ = 0;
	if (audioPorts_) {
		for (int isInput = 1; isInput >= 0; --isInput) {
			auto count = audioPorts_->count(plugin_, isInput);
			auto &channels = isInput ? inPortChannels_ : outPortChannels_;
			bool foundMain = false;
			for (uint32_t i = 0; i < count; ++i) {
				clap_audio_port_info info{};
				if (!audioPorts_->get(plugin_, i, isInput, &info)) {
					channels.push_back(0);
					continue;
				}
				channels.push_back(info.channel_count);
				if (!foundMain && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)) {
					(isInput ? mainInPort_ : mainOutPort_) = i;
					foundMain = true;
				}
			}
		}
	}
	mainInChannels_ = inPortChannels_.empty() ? 0 : inPortChannels_[mainInPort_];
	mainOutChannels_ = outPortChannels_.empty() ? 0 : outPortChannels_[mainOutPort_];
	noteInputPorts_ = notePorts_ ? notePorts_->count(plugin_, true) : 0;
	mpe_ = false;
	for (uint32_t i = 0; i < noteInputPorts_; ++i) {
		clap_note_port_info info{};
		if (notePorts_->get(plugin_, i, true, &info) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE)) mpe_ = true;
	}
}

bool InnerPlugin::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) {
	if (active_) return true;
	readPortLayout();
	maxFrames_ = maxFrames;
	scratch_.clear();
	inPtrs_.assign(inPortChannels_.size(), {});
	outPtrs_.assign(outPortChannels_.size(), {});
	for (size_t p = 0; p < inPortChannels_.size(); ++p)
		for (uint32_t c = 0; c < inPortChannels_[p]; ++c) scratch_.emplace_back(maxFrames, 0.0f);
	for (size_t p = 0; p < outPortChannels_.size(); ++p)
		for (uint32_t c = 0; c < outPortChannels_[p]; ++c) scratch_.emplace_back(maxFrames, 0.0f);
	size_t s = 0;
	for (size_t p = 0; p < inPortChannels_.size(); ++p)
		for (uint32_t c = 0; c < inPortChannels_[p]; ++c) inPtrs_[p].push_back(scratch_[s++].data());
	for (size_t p = 0; p < outPortChannels_.size(); ++p)
		for (uint32_t c = 0; c < outPortChannels_[p]; ++c) outPtrs_[p].push_back(scratch_[s++].data());
	inBufs_.assign(inPortChannels_.size(), clap_audio_buffer{});
	outBufs_.assign(outPortChannels_.size(), clap_audio_buffer{});
	for (size_t p = 0; p < inBufs_.size(); ++p) {
		inBufs_[p].data32 = inPtrs_[p].data();
		inBufs_[p].channel_count = inPortChannels_[p];
	}
	for (size_t p = 0; p < outBufs_.size(); ++p) {
		outBufs_[p].data32 = outPtrs_[p].data();
		outBufs_[p].channel_count = outPortChannels_[p];
	}
	active_ = plugin_->activate(plugin_, sampleRate, minFrames, maxFrames);
	return active_;
}

void InnerPlugin::deactivate() {
	if (!active_) return;
	plugin_->deactivate(plugin_);
	active_ = false;
}

void InnerPlugin::onMainThread() {
	if (callbackRequested_.exchange(false) && plugin_) plugin_->on_main_thread(plugin_);
}

void InnerPlugin::onTimer(clap_id timerId) {
	if (timerSupport_ && ownsTimer(timerId)) timerSupport_->on_timer(plugin_, timerId);
}

bool InnerPlugin::ownsTimer(clap_id timerId) const {
	return std::find(timers_.begin(), timers_.end(), timerId) != timers_.end();
}

// ---- params / state ---------------------------------------------------------------------
std::vector<ParamInfo> InnerPlugin::params() const {
	std::vector<ParamInfo> out;
	if (!params_) return out;
	auto count = params_->count(plugin_);
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info info{};
		if (!params_->get_info(plugin_, i, &info)) continue;
		ParamInfo p;
		p.id = info.id;
		p.name = info.name;
		p.module = info.module;
		p.minValue = info.min_value;
		p.maxValue = info.max_value;
		p.defaultValue = info.default_value;
		p.flags = info.flags;
		out.push_back(std::move(p));
	}
	return out;
}

bool InnerPlugin::paramValue(clap_id id, double &value) const {
	return params_ && params_->get_value(plugin_, id, &value);
}

std::string InnerPlugin::paramText(clap_id id, double value) const {
	char buf[256] = {0};
	if (params_ && params_->value_to_text(plugin_, id, value, buf, sizeof(buf))) return buf;
	char num[64];
	std::snprintf(num, sizeof(num), "%.3g", value);
	return num;
}

void InnerPlugin::paramsFlush(const clap_input_events *in, const clap_output_events *out) {
	if (params_ && params_->flush) params_->flush(plugin_, in, out);
}

bool InnerPlugin::saveState(std::vector<uint8_t> &out) {
	out.clear();
	if (!state_) return false;
	struct Ctx { std::vector<uint8_t> *v; } ctx{&out};
	clap_ostream os{
		.ctx = &ctx,
		.write = [](const clap_ostream *s, const void *buffer, uint64_t size) -> int64_t {
			auto *c = static_cast<Ctx *>(s->ctx);
			auto *p = static_cast<const uint8_t *>(buffer);
			c->v->insert(c->v->end(), p, p + size);
			return (int64_t)size;
		},
	};
	return state_->save(plugin_, &os);
}

bool InnerPlugin::loadState(const std::vector<uint8_t> &in) {
	if (!state_) return false;
	struct Ctx { const std::vector<uint8_t> *v; size_t pos; } ctx{&in, 0};
	clap_istream is{
		.ctx = &ctx,
		.read = [](const clap_istream *s, void *buffer, uint64_t size) -> int64_t {
			auto *c = static_cast<Ctx *>(s->ctx);
			auto n = std::min<uint64_t>(size, c->v->size() - c->pos);
			if (n) std::memcpy(buffer, c->v->data() + c->pos, n);
			c->pos += n;
			return (int64_t)n;
		},
	};
	return state_->load(plugin_, &is);
}

uint32_t InnerPlugin::latency() const {
	return (latency_ && active_) ? latency_->get(plugin_) : 0;
}

// ---- audio thread -------------------------------------------------------------------------
bool InnerPlugin::startProcessing() {
	if (processing_) return true;
	if (!active_) return false;
	processing_ = plugin_->start_processing(plugin_);
	return processing_;
}

void InnerPlugin::stopProcessing() {
	if (!processing_) return;
	plugin_->stop_processing(plugin_);
	processing_ = false;
}

clap_process_status InnerPlugin::process(const float *const *in, uint32_t inChannels, float *const *out,
                                         uint32_t outChannels, uint32_t frames, int64_t steadyTime,
                                         const clap_event_transport *transport, const clap_input_events *inEvents,
                                         const clap_output_events *outEvents) {
	if (!processing_ || frames > maxFrames_) {
		for (uint32_t c = 0; c < outChannels; ++c) std::memset(out[c], 0, sizeof(float) * frames);
		return CLAP_PROCESS_ERROR;
	}
	// inputs: main port gets the outer input (mono-sum or duplicate as needed), others silence
	for (size_t p = 0; p < inPortChannels_.size(); ++p) {
		for (uint32_t c = 0; c < inPortChannels_[p]; ++c) {
			float *dst = inPtrs_[p][c];
			if (p == mainInPort_ && inChannels > 0 && in) {
				if (inPortChannels_[p] == 1 && inChannels >= 2) {
					for (uint32_t i = 0; i < frames; ++i) dst[i] = 0.5f * (in[0][i] + in[1][i]);
				} else {
					std::memcpy(dst, in[std::min(c, inChannels - 1)], sizeof(float) * frames);
				}
			} else {
				std::memset(dst, 0, sizeof(float) * frames);
			}
		}
		inBufs_[p].constant_mask = 0;
	}
	for (size_t p = 0; p < outPortChannels_.size(); ++p) outBufs_[p].constant_mask = 0;

	clap_process proc{
		.steady_time = steadyTime,
		.frames_count = frames,
		.transport = transport,
		.audio_inputs = inBufs_.data(),
		.audio_outputs = outBufs_.data(),
		.audio_inputs_count = (uint32_t)inBufs_.size(),
		.audio_outputs_count = (uint32_t)outBufs_.size(),
		.in_events = inEvents,
		.out_events = outEvents,
	};
	auto status = plugin_->process(plugin_, &proc);

	// outputs: main port -> outer output (mono duplicated to both sides)
	uint32_t n = outPortChannels_.empty() ? 0 : outPortChannels_[mainOutPort_];
	for (uint32_t c = 0; c < outChannels; ++c) {
		if (n == 0 || status == CLAP_PROCESS_ERROR) {
			std::memset(out[c], 0, sizeof(float) * frames);
		} else {
			std::memcpy(out[c], outPtrs_[mainOutPort_][std::min(c, n - 1)], sizeof(float) * frames);
		}
	}
	return status;
}

} // namespace plugincircuit
