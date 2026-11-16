#pragma once
// InnerPlugin: Plugin Circuit acting as a CLAP *host* for one bridged WCLAP plugin.
//
// Threading contract (mirrors CLAP):
//  - create / activate / deactivate / params info / state / gui / onMainThread: main thread
//  - startProcessing / stopProcessing / process / reset: audio thread
// All buffers used on the audio thread are allocated in activate().
#include "core/WclapLibrary.h"

#include <clap/clap.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plugincircuit {

struct InnerHostCallbacks {
	std::function<bool()> isMainThread;
	std::function<bool()> isAudioThread;
	std::function<void()> requestCallback;      // ask the outer host for a main-thread callback
	std::function<void()> requestRestart;
	std::function<void()> requestProcess;
	std::function<void(clap_param_rescan_flags)> paramsRescan;
	std::function<void()> paramsRequestFlush;
	std::function<void()> stateMarkDirty;
	std::function<void()> latencyChanged;
	std::function<bool(uint32_t w, uint32_t h)> guiRequestResize;
	std::function<bool(uint32_t periodMs, clap_id *timerId)> registerTimer;
	std::function<bool(clap_id timerId)> unregisterTimer;
	std::function<void(clap_log_severity, const char *)> log;
};

struct ParamInfo {
	clap_id id = 0;
	std::string name, module;
	double minValue = 0, maxValue = 1, defaultValue = 0;
	uint32_t flags = 0;
	bool automatable() const { return (flags & CLAP_PARAM_IS_AUTOMATABLE) && !(flags & CLAP_PARAM_IS_READONLY); }
	bool stepped() const { return flags & CLAP_PARAM_IS_STEPPED; }
};

class InnerPlugin {
public:
	static std::unique_ptr<InnerPlugin> create(std::shared_ptr<WclapModule> module, const std::string &pluginId,
	                                           InnerHostCallbacks callbacks, std::string &error);
	~InnerPlugin();

	const clap_plugin *plugin() const { return plugin_; }
	const std::string &pluginId() const { return pluginId_; }
	const std::shared_ptr<WclapModule> &module() const { return module_; }

	// --- main thread ---
	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
	void deactivate();
	bool isActive() const { return active_; }
	void onMainThread();              // call when the outer host gives us a main-thread callback
	void onTimer(clap_id timerId);    // forward timer ticks registered via registerTimer
	bool ownsTimer(clap_id timerId) const;

	std::vector<ParamInfo> params() const;
	bool paramValue(clap_id id, double &value) const;
	std::string paramText(clap_id id, double value) const;
	// Main-thread param flush (when not processing). Events must be sorted by time.
	void paramsFlush(const clap_input_events *in, const clap_output_events *out);

	bool saveState(std::vector<uint8_t> &out);
	bool loadState(const std::vector<uint8_t> &in);

	uint32_t latency() const;
	const clap_plugin_gui *gui() const { return gui_; }
	bool hasNoteInput() const { return noteInputPorts_ > 0; }
	bool supportsMpe() const { return mpe_; } // a note input declares the MIDI-MPE dialect
	uint32_t noteDialects() const { return noteDialects_; }       // supported dialects of note input port 0
	uint32_t preferredNoteDialect() const { return preferredDialect_; }
	uint32_t mainInputChannels() const { return mainInChannels_; }
	uint32_t mainOutputChannels() const { return mainOutChannels_; }

	// --- audio thread ---
	bool startProcessing();
	void stopProcessing();
	bool isProcessing() const { return processing_; }
	// in/out: the outer main bus. Copies into the inner main ports (other ports get silence).
	clap_process_status process(const float *const *in, uint32_t inChannels, float *const *out, uint32_t outChannels,
	                            uint32_t frames, int64_t steadyTime, const clap_event_transport *transport,
	                            const clap_input_events *inEvents, const clap_output_events *outEvents);

private:
	friend struct InnerHostExt;
	InnerPlugin() = default;
	void readPortLayout();

	std::shared_ptr<WclapModule> module_;
	std::string pluginId_;
	InnerHostCallbacks cb_;
	clap_host host_{};
	const clap_plugin *plugin_ = nullptr;
	bool initialized_ = false, active_ = false;
	std::atomic<bool> processing_{false};
	std::atomic<bool> callbackRequested_{false};
	std::vector<clap_id> timers_;

	// extensions (resolved after init)
	const clap_plugin_params *params_ = nullptr;
	const clap_plugin_state *state_ = nullptr;
	const clap_plugin_gui *gui_ = nullptr;
	const clap_plugin_latency *latency_ = nullptr;
	const clap_plugin_audio_ports *audioPorts_ = nullptr;
	const clap_plugin_note_ports *notePorts_ = nullptr;
	const clap_plugin_timer_support *timerSupport_ = nullptr;

	// port layout + preallocated scratch
	std::vector<uint32_t> inPortChannels_, outPortChannels_;
	uint32_t mainInPort_ = 0, mainOutPort_ = 0, mainInChannels_ = 0, mainOutChannels_ = 0, noteInputPorts_ = 0;
	bool mpe_ = false;
	uint32_t noteDialects_ = 0, preferredDialect_ = 0;

	// Note-dialect conversion for inner plugins that accept only one of CLAP notes / MIDI (port 0's dialects).
	// Preallocated in activate(); used on the audio thread only.
	enum class NoteConversion { None, ClapToMidi, MidiToClap };
	NoteConversion noteConversion_ = NoteConversion::None;
	static constexpr size_t kMaxConvertedEvents = 4096;
	std::vector<const clap_event_header *> convertedList_;
	std::vector<clap_event_midi> convertedMidi_;
	std::vector<clap_event_note> convertedNotes_;
	clap_input_events convertedEvents_{};
	const clap_input_events *convertNotes(const clap_input_events *in);
	uint32_t maxFrames_ = 0;
	std::vector<std::vector<float>> scratch_;          // one per channel of every port
	std::vector<std::vector<float *>> inPtrs_, outPtrs_; // per port
	std::vector<clap_audio_buffer> inBufs_, outBufs_;

	// host extension tables
	static const void *getExtension(const clap_host *host, const char *id);
	static void requestRestart(const clap_host *host);
	static void requestProcess(const clap_host *host);
	static void requestCallback(const clap_host *host);
	static InnerPlugin *from(const clap_host *host) { return static_cast<InnerPlugin *>(host->host_data); }
};

} // namespace plugincircuit
