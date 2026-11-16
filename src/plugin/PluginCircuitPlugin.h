#pragma once
// The Plugin Circuit host plugin (CLAP; wrapped as VST3/AU/standalone by clap-wrapper).
//
// One inner WCLAP at a time. Threading:
//  - The main thread owns `current_` and is the only writer. It swaps it under `innerLock_`.
//  - The audio thread try-locks `innerLock_` per block (never blocks); if the main thread holds it
//    for a swap, that block passes audio through. start/stop_processing of the inner plugin always
//    happen on the audio thread (`audioStarted_`), except when the outer plugin isn't processing.
//  - Loading (fetch metadata, download, verify, extract, compile wasm) runs on a worker thread;
//    creating/activating the inner plugin happens back on the main thread.
#include "core/Catalog.h"
#include "core/InnerHost.h"
#include "core/PackageCache.h"
#include "core/ParamTable.h"
#include "plugin/Gestures.h"
#include "plugin/Macros.h"
#include "plugin/StateCodec.h"

#include <choc/containers/choc_SingleReaderSingleWriterFIFO.h>
#include <choc/containers/choc_Value.h>
#include <clap/clap.h>

#include <array>
#include <bitset>
#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace plugincircuit {

class Editor;

class PluginCircuitPlugin {
public:
	static const clap_plugin_descriptor fxDescriptor;
	static const clap_plugin_descriptor instrumentDescriptor;

	PluginCircuitPlugin(const clap_host *host, bool instrument);
	~PluginCircuitPlugin();
	const clap_plugin *clapPlugin() const { return &plugin_; }

	// ---- called by the editor / UI bridge (main thread) ----
	choc::value::Value uiSnapshot(bool full);
	void uiLoad(const std::string &pluginId, const std::string &sha256);
	void uiUnload();
	// A value from our UI for a macro (normalized) or an inner parameter, with its gesture phase (docs/MESSAGING.md).
	void uiEdit(bool macro, uint32_t id, double value, Phase phase);
	void uiNote(int16_t key, double velocity, bool on); // on-screen keyboard
	void uiAllNotesOff(); // releases every note the UI still holds (editor closed or hidden, plugin swapped)
	void uiMapMacro(uint32_t slot, clap_id paramId, double minValue, double maxValue, bool add);
	void uiUnmapMacro(uint32_t slot, int64_t paramId); // -1 clears all targets
	void uiRenameMacro(uint32_t slot, const std::string &name);
	void uiAutoMap();
	void uiSetTransport(bool playing); // the fallback transport, used only when the host sends none
	bool isInstrument() const { return instrument_; }
	// DSP load, as JUCE's AudioProcessLoadMeasurer: the time a block took over the block's real-time duration. The
	// audio thread only adds to these (relaxed atomics, no lock); the settings page reads deltas ~10 times a second.
	struct Performance {
		std::atomic<uint64_t> blocks{0};
		std::atomic<uint64_t> overBudget{0};   // blocks slower than real time: the device would have underrun
		std::atomic<uint64_t> loadPpmSum{0};   // sum of each block's load, in millionths
		std::atomic<uint32_t> peakPpm{0};      // highest load since the last read (the reader resets it)
		std::atomic<uint32_t> blockFrames{0};  // the last block's size
	};
	Performance performance;
	// A WCLAP's request_resize from a non-main thread, handed to the editor's main-thread tick: (w << 32 | h), 0 = none.
	// Kept here, not in the Editor, which the main thread may destroy meanwhile.
	std::atomic<uint64_t> pendingInnerResize{0};
	bool isMainThread() const { return std::this_thread::get_id() == mainThreadId_; }
	double sampleRate() const { return sampleRate_; }
	InnerPlugin *innerForGui(); // main thread: the current inner plugin, or nullptr
	std::string currentPackageSha() const { return current_ ? current_->meta.sha256 : std::string(); } // main thread
	void packagesChanged() { installedAt_ = {}; stateVersion_++; } // main thread: after the settings page deleted some
	bool requestHostResize(uint32_t w, uint32_t h); // false: the host refused, keep the current size
	void housekeeping(); // main thread: finish loads, free retired objects

private:
	struct Slot {
		std::unique_ptr<InnerPlugin> inner;
		CatalogPlugin meta;
		PackageCache::InUse inUse; // the package can't be deleted while it's loaded
		// The parameter table (core/ParamTable.h): published here, read on the audio thread through this pointer.
		std::atomic<ParamTable *> params{nullptr};
		std::unique_ptr<ParamTable> ownedParams;
		std::atomic<bool> started{false}; // inner start_processing done (audio thread)
	};
	enum class Status { Empty, Fetching, Downloading, Compiling, Ready, Missing, Error };

	struct LoadJob {
		std::string pluginId, sha256;
		std::optional<HostState> state; // apply after load
		// results
		std::shared_ptr<WclapModule> module;
		CatalogPlugin meta;
		PackageCache::InUse inUse;
		std::string error;
		bool missing = false; // the plugin can't be fetched: keep its state
	};

	struct UiEvent {
		enum Kind : uint8_t { Macro, Param, NoteOn, NoteOff } kind;
		Phase phase;
		uint32_t id;
		double value;
	};

	// CLAP plugin
	bool init();
	void destroy();
	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
	void deactivate();
	bool startProcessing();
	void stopProcessing();
	void reset();
	clap_process_status process(const clap_process *process); // measures the DSP load around processBlock()
	const void *getExtension(const char *id);
	void onMainThread();

	// extensions
	uint32_t audioPortsCount(bool isInput) const;
	bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info *info) const;
	uint32_t notePortsCount(bool isInput) const;
	bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info *info) const;
	bool paramsGetInfo(uint32_t index, clap_param_info *info) const;
	bool paramsGetValue(clap_id id, double *value) const;
	bool paramsValueToText(clap_id id, double value, char *display, uint32_t size) const;
	bool paramsTextToValue(clap_id id, const char *display, double *value) const;
	void paramsFlush(const clap_input_events *in, const clap_output_events *out);
	bool stateSave(const clap_ostream *stream);
	bool stateLoad(const clap_istream *stream);
	uint32_t latencyGet() const;
	void onTimer(clap_id timerId);

	// internals
	void startLoad(LoadJob job);
	void finishLoad(std::unique_ptr<LoadJob> job);
	void swapIn(std::unique_ptr<Slot> slot);
	void publishMacros(MacroMap map);
	void applyHostState(InnerPlugin &inner, const HostState &state);
	InnerHostCallbacks makeInnerCallbacks();
	// drainUi: macro/param/gesture UI events; drainNotes: UI notes (only real process blocks, never params flush)
	void translateEvents(const clap_input_events *in, Slot *slot, const MacroMap *map, bool drainUi, bool drainNotes);
	void passThrough(const clap_process *process);
	void lockInner();
	void unlockInner() { innerLock_.clear(std::memory_order_release); }
	void setStatus(Status s, std::string message = {});
	void markDirty();

	clap_plugin plugin_;
	const clap_host *host_;
	const bool instrument_;
	const clap_host_params *hostParams_ = nullptr;
	const clap_host_state *hostState_ = nullptr;
	const clap_host_latency *hostLatency_ = nullptr;
	const clap_host_tail *hostTail_ = nullptr;
	// The WCLAP's tail, for the DAW's clap.tail (read on main and audio threads). tailNotify_: tell the DAW on the next
	// block (clap_host_tail.changed is [audio-thread]); tailDirty_: the WCLAP said its tail changed, re-read it there.
	std::atomic<uint32_t> tailSamples_{0};
	std::atomic<bool> tailNotify_{false}, tailDirty_{false};
	// The last status the WCLAP returned: if it allows sleeping, UI input asks the DAW to process again.
	std::atomic<int32_t> lastStatus_{CLAP_PROCESS_CONTINUE};
	void wakeIfSleeping() {
		if (lastStatus_.load(std::memory_order_relaxed) != CLAP_PROCESS_CONTINUE) host_->request_process(host_);
	}
	const clap_host_gui *hostGui_ = nullptr;
	const clap_host_timer_support *hostTimer_ = nullptr;
	const clap_host_log *hostLog_ = nullptr;
	clap_id housekeepingTimer_ = CLAP_INVALID_ID;

	// audio config
	bool active_ = false;
	std::atomic<bool> processing_{false};
	clap_process_status processBlock(const clap_process *p);
	std::thread::id mainThreadId_ = std::this_thread::get_id(); // set again in init(), which CLAP calls on the main thread
	double sampleRate_ = 48000;
	uint32_t minFrames_ = 1, maxFrames_ = 4096;

	// inner plugin
	std::atomic_flag innerLock_ = ATOMIC_FLAG_INIT;
	Slot *current_ = nullptr;      // main thread writes (under lock), audio thread reads (under lock)
	Slot *audioStarted_ = nullptr; // audio thread only (under lock)
	std::unique_ptr<Slot> owned_;  // owns current_
	std::vector<std::unique_ptr<Slot>> retiredSlots_;
	std::optional<HostState> missingState_; // state of a plugin that couldn't load: saved back unchanged

	// macros
	std::atomic<MacroMap *> macros_{nullptr};
	std::unique_ptr<MacroMap> ownedMacros_;
	std::vector<std::pair<uint64_t, std::unique_ptr<MacroMap>>> retiredMacros_;
	std::vector<std::pair<uint64_t, std::unique_ptr<ParamTable>>> retiredTables_;

	// ---- parameters (docs/MESSAGING.md: the table is the single source of truth) ----
public:
	ParamTable *paramTable() const { return current_ ? current_->params.load(std::memory_order_acquire) : nullptr; } // main thread
	std::string paramTextCached(clap_id id, double value); // main thread: value_to_text, cached per (id, value)
	void reconcileParams(size_t count); // main thread: the safety net, re-reads `count` values round-robin
	// The parameters the page shows (its virtualized Params view): texts are formatted only for these and for macro
	// targets. Newly visible ones get a fresh text on the next update.
	void setVisibleParams(std::vector<clap_id> ids); // main thread

private:
	void buildParamTable(Slot &slot);                        // main thread: (re)build from the plugin, then publish
	void refreshParamValues(ParamTable &table, InnerPlugin &inner); // main thread: get_value for every parameter
	struct TextKey {
		clap_id id;
		uint64_t bits;
		bool operator==(const TextKey &o) const { return id == o.id && bits == o.bits; }
	};
	struct TextKeyHash {
		size_t operator()(const TextKey &k) const { return std::hash<uint64_t>()(k.bits * 0x9E3779B97F4A7C15ull ^ k.id); }
	};
	std::unordered_map<TextKey, std::string, TextKeyHash> textCache_;
	size_t reconcileNext_ = 0;
	std::vector<clap_id> visibleParams_;  // sorted
	std::vector<clap_id> pendingTexts_;   // visible parameters still owed a text (formatting is time-budgeted)
	bool wantsText(clap_id id) const;
	void queueText(clap_id id);
	std::array<std::atomic<double>, kNumMacros> macroValues_{};
	std::atomic<uint64_t> processEpoch_{0};
	// Hosts without a transport (clap-wrapper's standalone passes none): our own 120 bpm 4/4 transport, started
	// and stopped from the host UI, so sequencer-driven instruments can play. Starts stopped, so grooveboxes don't
	// run on their own. fallbackTransport_ / fallbackSamples_ are audio-thread only.
	clap_event_transport fallbackTransport_{};
	int64_t fallbackSamples_ = 0;
	std::atomic<bool> fallbackPlaying_{false}, fallbackRewind_{false}, hostHasTransport_{true};
	const clap_event_transport *transportFor(const clap_process *p);

	// UI -> audio events
	choc::fifo::SingleReaderSingleWriterFIFO<UiEvent> uiEvents_;
	choc::fifo::SingleReaderSingleWriterFIFO<UiEvent> uiNotes_; // own queue: parameter flushes never drain notes
	std::mutex uiPushMutex_; // UI pushes come from the main thread only, but keep pushes serialised
	std::bitset<128> uiHeldNotes_; // main thread: notes the UI has started and not yet released

	// installed packages for the snapshot: a disk scan, so cached (main thread)
	std::vector<CatalogPlugin> installedCache_;
	std::chrono::steady_clock::time_point installedAt_{};
	const std::vector<CatalogPlugin> &installedPlugins();

	// per-block event storage (audio thread)
	struct EventBuffer {
		std::vector<uint8_t> storage;
		std::vector<uint32_t> offsets;
		clap_input_events list;
		void clear() { storage.clear(); offsets.clear(); }
		void push(const clap_event_header *e);
	} innerIn_;
	const clap_output_events *outerOut_ = nullptr; // valid during process
	clap_output_events innerOut_;
	static bool innerOutTryPush(const clap_output_events *list, const clap_event_header *event);
	struct EventSink;
	Gestures gestures_; // who owns which parameter; only the thread that processes touches it

	// loading
	std::thread loadThread_;
	std::mutex jobMutex_;
	std::unique_ptr<LoadJob> finishedJob_;
	std::atomic<bool> loading_{false};
	std::atomic<int> status_{(int)Status::Empty};
	std::atomic<double> downloadProgress_{0};
	std::mutex statusMutex_;
	std::string statusMessage_;
	std::atomic<uint64_t> stateVersion_{1}; // bumps when the UI needs a full snapshot

	std::unique_ptr<Editor> editor_;
	friend class Editor;
	friend struct PluginGlue;
};

} // namespace plugincircuit
