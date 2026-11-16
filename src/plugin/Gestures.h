#pragma once
// The gesture and ownership state machine (docs/MESSAGING.md, "Gestures and ownership"; docs/PLAN.md, phase 2).
//
// Who edits a parameter: our UI (a drag in Macros or Params) or the WCLAP (its own GUI, reported as
// PARAM_GESTURE_BEGIN / END output events). The DAW never owns one: CLAP gestures go from a plugin to its host only,
// so the DAW's automation is a plain writer, dropped while someone owns the parameter (touch).
//
// Ownership is kept per inner parameter (in the ParamTable, so the main thread can read it) and per macro (the DAW's
// parameters: the DAW is told GESTURE_BEGIN / END on a macro, never on an inner parameter). A macro is owned while a
// gesture runs on it or on the one parameter it is a 1:1 view of.
//
// Only the thread that processes calls in (the audio thread, or the main thread in params.flush; never both at once).
// No allocation, no locks. Effects leave through the Sink, so a test can record them.
#include "core/ParamTable.h"
#include "plugin/Macros.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace plugincircuit {

enum class Phase : uint8_t { Set, Begin, Update, End }; // Set: a value outside a gesture (a click, a reset)

class Gestures {
public:
	struct Sink {
		virtual ~Sink() = default;
		virtual void toPlugin(clap_id param, double value) = 0;                // a value for the WCLAP (the table is set here)
		virtual void macroValue(uint32_t macro, double normalized, bool tellDaw) = 0;
		virtual void dawGesture(uint32_t macro, bool begin) = 0;
	};

	// Call at the start of every process / flush with the current table: a new table (a plugin loaded, unloaded or
	// rescanned) has no owners, so every gesture still open on a macro is closed.
	void begin(ParamTable *table, Sink &sink) {
		if (table != table_) {
			table_ = table;
			closeMacros(Owner::None, sink);
		}
		const uint8_t close = closeRequested_.exchange(0, std::memory_order_acquire);
		if (close & kCloseAll) closeAll(Owner::None, sink);
		else if (close & kCloseUi) closeAll(Owner::Ui, sink);
	}

	// Any thread: close open gestures at the next process / flush (unload, reset, deactivate, the editor closing).
	void requestCloseAll() { closeRequested_.fetch_or(kCloseAll, std::memory_order_release); }
	void requestCloseUi() { closeRequested_.fetch_or(kCloseUi, std::memory_order_release); }

	Owner macroOwner(uint32_t macro) const { return macroOwner_[macro].load(std::memory_order_relaxed); }

	// ---- our UI
	void uiMacro(const MacroMap *map, uint32_t m, double v, Phase phase, Sink &sink) {
		if (m >= kNumMacros) return;
		const Owner o = macroOwner(m);
		if (o == Owner::Plugin) return; // the plugin's gesture wins: begin ignored, values and end dropped
		if (phase == Phase::End && o != Owner::Ui) return; // nothing open
		if (phase == Phase::Begin && o == Owner::None) {
			setMacroOwner(m, Owner::Ui);
			sink.dawGesture(m, true);
			if (map && table_)
				for (auto &t : map->macros[m].targets)
					if (int i = table_->indexOf(t.paramId); i >= 0 && table_->owner(size_t(i)) == Owner::None)
						table_->setOwner(size_t(i), Owner::Ui);
		}
		sink.macroValue(m, v, true);
		if (map)
			for (auto &t : map->macros[m].targets)
				if (paramOwner(t.paramId) != Owner::Plugin) sink.toPlugin(t.paramId, t.map(v));
		if (phase == Phase::End) {
			sink.dawGesture(m, false);
			setMacroOwner(m, Owner::None);
			if (map && table_)
				for (auto &t : map->macros[m].targets) release(t.paramId, Owner::Ui);
		}
	}

	void uiParam(const MacroMap *map, clap_id p, double v, Phase phase, Sink &sink) {
		const Owner o = paramOwner(p);
		if (o == Owner::Plugin) return;
		if (phase == Phase::End && o != Owner::Ui) return;
		const int s = map ? map->slotForParam(p) : -1;
		if (phase == Phase::Begin && o == Owner::None) {
			setParamOwner(p, Owner::Ui);
			if (s >= 0 && macroOwner(uint32_t(s)) == Owner::None) {
				setMacroOwner(uint32_t(s), Owner::Ui);
				sink.dawGesture(uint32_t(s), true);
			}
		}
		sink.toPlugin(p, v);
		if (s >= 0 && macroOwner(uint32_t(s)) != Owner::Plugin)
			sink.macroValue(uint32_t(s), map->macros[size_t(s)].targets[0].unmap(v), true);
		if (phase == Phase::End) {
			release(p, Owner::Ui);
			if (s >= 0 && macroOwner(uint32_t(s)) == Owner::Ui) {
				sink.dawGesture(uint32_t(s), false);
				setMacroOwner(uint32_t(s), Owner::None);
			}
		}
	}

	// ---- the WCLAP's output events
	void pluginGesture(const MacroMap *map, clap_id p, bool begin, Sink &sink) {
		const Owner o = paramOwner(p);
		const int s = map ? map->slotForParam(p) : -1;
		if (begin) {
			if (o != Owner::None) return; // the UI's gesture wins (or already open)
			setParamOwner(p, Owner::Plugin);
			if (s >= 0 && macroOwner(uint32_t(s)) == Owner::None) {
				setMacroOwner(uint32_t(s), Owner::Plugin);
				sink.dawGesture(uint32_t(s), true);
			}
			return;
		}
		if (o != Owner::Plugin) return;
		release(p, Owner::Plugin);
		if (s >= 0 && macroOwner(uint32_t(s)) == Owner::Plugin) {
			sink.dawGesture(uint32_t(s), false);
			setMacroOwner(uint32_t(s), Owner::None);
		}
	}

	// The plugin changed its own value. Dropped while the UI owns it (the UI's next value overrides it in the WCLAP).
	void pluginValue(const MacroMap *map, clap_id p, double v, Sink &sink) {
		if (paramOwner(p) == Owner::Ui) return;
		if (table_) table_->setById(p, v);
		const int s = map ? map->slotForParam(p) : -1;
		if (s >= 0 && macroOwner(uint32_t(s)) != Owner::Ui)
			sink.macroValue(uint32_t(s), map->macros[size_t(s)].targets[0].unmap(v), true);
	}

	// ---- the DAW (automation, a knob in its generic editor): applied only to what nobody owns
	void dawMacro(const MacroMap *map, uint32_t m, double v, Sink &sink) {
		if (m >= kNumMacros || macroOwner(m) != Owner::None) return;
		sink.macroValue(m, v, false);
		if (map)
			for (auto &t : map->macros[m].targets)
				if (paramOwner(t.paramId) == Owner::None) sink.toPlugin(t.paramId, t.map(v));
	}

private:
	static constexpr uint8_t kCloseAll = 1, kCloseUi = 2;

	Owner paramOwner(clap_id p) const {
		const int i = table_ ? table_->indexOf(p) : -1;
		return i >= 0 ? table_->owner(size_t(i)) : Owner::None;
	}
	void setParamOwner(clap_id p, Owner o) {
		if (int i = table_ ? table_->indexOf(p) : -1; i >= 0) table_->setOwner(size_t(i), o);
	}
	// ends p's gesture if `from` owns it, and pushes its final value to the page again
	void release(clap_id p, Owner from) {
		if (int i = table_ ? table_->indexOf(p) : -1; i >= 0 && table_->owner(size_t(i)) == from) {
			table_->setOwner(size_t(i), Owner::None);
			table_->markChanged(size_t(i));
		}
	}
	void setMacroOwner(uint32_t m, Owner o) { macroOwner_[m].store(o, std::memory_order_relaxed); }

	// which == None: every owner
	void closeMacros(Owner which, Sink &sink) {
		for (uint32_t m = 0; m < kNumMacros; ++m) {
			const Owner o = macroOwner(m);
			if (o == Owner::None || (which != Owner::None && o != which)) continue;
			sink.dawGesture(m, false);
			setMacroOwner(m, Owner::None);
		}
	}
	void closeAll(Owner which, Sink &sink) {
		closeMacros(which, sink);
		if (!table_) return;
		for (size_t i = 0; i < table_->size(); ++i) {
			const Owner o = table_->owner(i);
			if (o != Owner::None && (which == Owner::None || o == which)) {
				table_->setOwner(i, Owner::None);
				table_->markChanged(i);
			}
		}
	}

	ParamTable *table_ = nullptr;
	std::array<std::atomic<Owner>, kNumMacros> macroOwner_{};
	std::atomic<uint8_t> closeRequested_{0};
};

} // namespace plugincircuit
