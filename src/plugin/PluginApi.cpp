// The plugin's part of the API (docs/MESSAGING.md, "One API"): its stores, commands and the perf stream.
// The editor adds its own (`ui`, `app.*`, `plugin.load` / `plugin.unload`, which also switch the view).
#include "api/Api.h"
#include "core/PackageCache.h"
#include "api/SettingsSchema.h"
#include "core/Settings.h"
#include "core/Util.h"
#include "plugin/PluginCircuitPlugin.h"

#include <wclap-bridge.h>

#include <bit>
#include <chrono>
#include <cmath>

namespace plugincircuit {

namespace {
using Value = Api::Value;
using View = Api::View;

double number(const View &a, const char *key, double fallback = 0) {
	return a.isObject() && a.hasObjectMember(key) && (a[key].isFloat() || a[key].isInt()) ? a[key].getWithDefault<double>(fallback) : fallback;
}
std::string text(const View &a, const char *key) {
	return a.isObject() && a.hasObjectMember(key) && a[key].isString() ? std::string(a[key].getString()) : std::string();
}

// Every settable store rejects keys it doesn't know (a typo shouldn't pass silently).
std::string unknownKey(const View &v, std::initializer_list<const char *> known) {
	if (!v.isObject()) return "expected an object";
	for (uint32_t i = 0; i < v.size(); ++i) {
		const std::string k = v.getObjectMemberAt(i).name;
		bool found = false;
		for (auto *n : known) found = found || k == n;
		if (!found) return "unknown key '" + k + "'";
	}
	return {};
}
} // namespace

Value PluginCircuitPlugin::paramValues(ParamTable &table, bool full) {
	// Values from the table, never from the plugin: all when `full`, else only changed ones. Texts only for what the
	// page shows (and macro targets), within a time budget: value_to_text is a call into the plugin (main thread only,
	// CLAP), and formatting thousands at once froze the main thread (620 ms, Six Sines).
	auto values = choc::value::createEmptyArray();
	constexpr auto kTextBudget = std::chrono::milliseconds(8);
	const auto deadline = std::chrono::steady_clock::now() + kTextBudget;
	auto add = [&](size_t i) {
		// a parameter our UI is dragging: the page's value is the truth; the final value comes at the gesture's end
		if (table.owner(i) == Owner::Ui) return;
		const auto id = table.info(i).id;
		const double v = table.value(i);
		if (wantsText(id)) {
			if (std::chrono::steady_clock::now() < deadline) {
				values.addArrayElement(choc::value::createObject("PV", "id", (int64_t)id, "value", v, "text", paramTextCached(id, v)));
				return;
			}
			queueText(id); // out of time: next update
		}
		values.addArrayElement(choc::value::createObject("PV", "id", (int64_t)id, "value", v)); // keeps its last text
	};
	if (full) {
		table.takeChanged([](size_t) {});
		for (size_t i = 0; i < table.size(); ++i) add(i);
	} else {
		table.takeChanged(add);
	}
	// texts still owed (rows that just came into view, or over budget before)
	size_t done = 0;
	while (done < pendingTexts_.size() && std::chrono::steady_clock::now() < deadline) {
		const auto id = pendingTexts_[done++];
		const int i = table.indexOf(id);
		if (i < 0 || !wantsText(id)) continue;
		const double v = table.value(size_t(i));
		values.addArrayElement(choc::value::createObject("PV", "id", (int64_t)id, "value", v, "text", paramTextCached(id, v)));
	}
	pendingTexts_.erase(pendingTexts_.begin(), pendingTexts_.begin() + std::ptrdiff_t(done));
	return values;
}

void PluginCircuitPlugin::registerApi(Api &api) {
	// ---- plugin: what is loaded (or missing), its status, what else is installed
	api.store("plugin", {[this] {
		                     static const char *statusNames[] = {"empty", "fetching", "downloading", "compiling", "ready", "missing", "error"};
		                     std::string message;
		                     {
			                     std::lock_guard<std::mutex> lock(statusMutex_);
			                     message = statusMessage_;
		                     }
		                     auto v = choc::value::createObject("Plugin", "status", std::string(statusNames[status_.load()]), "message", message,
		                                                        "progress", downloadProgress_.load(), "instrument", instrument_,
		                                                        "apiBase", Paths::apiBase(), "moduleError", moduleError());
		                     if (current_) {
			                     auto &m = current_->meta;
			                     auto *inner = current_->inner.get();
			                     v.addMember("plugin", choc::value::createObject(
			                                               "Loaded", "id", inner->pluginId(), "name", m.name, "vendor", m.vendor, "type", m.type,
			                                               "license", m.license, "packageId", m.packageId, "sha256", m.sha256,
			                                               "hasGui", inner->gui() != nullptr, "latency", (int64_t)inner->latency(),
			                                               "mpe", inner->supportsMpe(), "noteInput", inner->hasNoteInput()));
		                     } else {
			                     v.addMember("plugin", Value());
		                     }
		                     if (!current_ && missingState_)
			                     v.addMember("missing", choc::value::createObject("Missing", "id", missingState_->pluginId, "name",
			                                                                      missingState_->pluginName, "vendor", missingState_->vendor,
			                                                                      "sha256", missingState_->packageSha256));
		                     else
			                     v.addMember("missing", Value());
		                     auto installed = choc::value::createEmptyArray();
		                     for (auto &p : installedPlugins())
			                     if ((p.type == "instrument") == instrument_ || p.type.empty())
				                     installed.addArrayElement(choc::value::createObject("I", "id", p.id, "name", p.name, "vendor", p.vendor,
				                                                                         "type", p.type, "sha256", p.sha256));
		                     v.addMember("installed", installed);
		                     return v;
	                     },
	                     [this] {
		                     // not the installed list: reading it rescans the packages (every 5 s); it follows any other change
		                     return Api::mix(Api::mix(stateVersion_.load(), uint64_t(status_.load())), std::bit_cast<uint64_t>(downloadProgress_.load()));
	                     },
	                     {}});

	// ---- plugin.errors: recent load failures, the module's error (a trap), the bridge's diagnostics (PLAN 5.2)
	api.store("plugin.errors", {[this] { return choc::value::createObject("Errors", "errors", errorsValue()); },
	                            [this] { return errorsVersion(); },
	                            {}});
	api.command("plugin.clearErrors", [this](const View &, std::string &) {
		clearErrors();
		return Value(true);
	});

	// ---- params: infos and values; deltas from the table's changed bits
	api.deltaStore("params", {[this] {
		                          paramsGeneration_ = 0;
		                          return paramsFull();
	                          },
	                          [this]() -> std::optional<Value> {
		                          auto *table = paramTable();
		                          const uint64_t generation = table ? table->generation() : 0;
		                          if (generation != paramsGeneration_) return paramsFull(); // a new shape: everything
		                          auto ended = takeEnded(); // first: a value read after it is the plugin's again
		                          auto values = table ? paramValues(*table, false) : choc::value::createEmptyArray();
		                          if (values.size() == 0 && ended.size() == 0) return std::nullopt;
		                          auto patch = choc::value::createObject("Params", "full", false);
		                          patch.addMember("values", values);
		                          patch.addMember("ended", ended);
		                          return patch;
	                          },
	                          {}});
	api.command("params.visible", [this](const View &a, std::string &) { // the Params view's rows in view
		std::vector<clap_id> ids;
		if (a.isObject() && a.hasObjectMember("ids") && a["ids"].isArray())
			for (auto v : a["ids"])
				if (v.isInt() || v.isFloat()) ids.push_back((clap_id)v.getWithDefault<int64_t>(0));
		setVisibleParams(std::move(ids));
		return Value(true);
	});
	// the plugin's own text for a value (value_to_text): a drag shows it at once, before the drag's end
	api.command("params.text", [this](const View &a, std::string &) {
		return Value(paramTextCached((clap_id)number(a, "id"), number(a, "value")));
	});
	// a value from our UI, with its gesture phase (docs/MESSAGING.md, "Gestures and ownership")
	api.command("edit", [this](const View &a, std::string &error) -> Value {
		const auto target = text(a, "target"), phaseName = text(a, "phase");
		if (target != "macro" && target != "param") {
			error = "edit: target must be macro or param";
			return {};
		}
		const Phase phase = phaseName == "begin" ? Phase::Begin : phaseName == "update" ? Phase::Update
		                    : phaseName == "end"  ? Phase::End : Phase::Set;
		uiEdit(target == "macro", (uint32_t)number(a, "id"), number(a, "value"), phase);
		return Value(true);
	});

	// ---- macros: names, targets and values
	api.store("macros", {[this] {
		                     auto *map = macros_.load();
		                     auto macros = choc::value::createEmptyArray();
		                     for (uint32_t i = 0; i < kNumMacros; ++i) {
			                     auto targets = choc::value::createEmptyArray();
			                     for (auto &t : map->macros[i].targets)
				                     targets.addArrayElement(choc::value::createObject("T", "id", (int64_t)t.paramId, "min", t.minValue, "max", t.maxValue));
			                     macros.addArrayElement(choc::value::createObject("M", "name", map->displayName(i), "custom",
			                                                                      map->macros[i].customName, "targets", targets));
		                     }
		                     auto values = choc::value::createEmptyArray();
		                     for (auto &v : macroValues_) values.addArrayElement(v.load());
		                     auto v = choc::value::createObject("Macros", "autoMapped", map->autoMapped);
		                     v.addMember("macros", macros);
		                     v.addMember("values", values);
		                     return v;
	                     },
	                     [this] {
		                     uint64_t h = Api::mix(0xcbf29ce484222325ull, reinterpret_cast<uintptr_t>(macros_.load()));
		                     for (auto &v : macroValues_) h = Api::mix(h, std::bit_cast<uint64_t>(v.load()));
		                     return h;
	                     },
	                     {}});
	api.command("macros.map", [this](const View &a, std::string &) {
		uiMapMacro((uint32_t)number(a, "slot"), (clap_id)number(a, "paramId"), number(a, "min"), number(a, "max", 1), number(a, "add") != 0);
		return Value(true);
	});
	api.command("macros.unmap", [this](const View &a, std::string &) {
		uiUnmapMacro((uint32_t)number(a, "slot"), (int64_t)number(a, "paramId", -1));
		return Value(true);
	});
	api.command("macros.rename", [this](const View &a, std::string &) {
		uiRenameMacro((uint32_t)number(a, "slot"), text(a, "name"));
		return Value(true);
	});
	api.command("macros.autoMap", [this](const View &, std::string &) {
		uiAutoMap();
		return Value(true);
	});

	// ---- transport: the fallback transport's play state (used only when the host sends none)
	api.store("transport", {[this] {
		                        return choc::value::createObject("Transport", "host", hostHasTransport_.load(), "playing", fallbackPlaying_.load());
	                        },
	                        [this] { return uint64_t(hostHasTransport_.load()) * 2 + uint64_t(fallbackPlaying_.load()) + 1; },
	                        [this](const View &v) -> std::string {
		                        if (auto e = unknownKey(v, {"playing"}); !e.empty()) return e;
		                        if (!v.hasObjectMember("playing") || !v["playing"].isBool()) return "playing must be true or false";
		                        uiSetTransport(v["playing"].getBool());
		                        return {};
	                        }});

	// ---- keyboard: the on-screen keyboard's notes (to the audio thread, like edit)
	api.command("keyboard.note", [this](const View &a, std::string &) {
		uiNote((int16_t)number(a, "key", -1), number(a, "velocity", 0.8), number(a, "on") != 0);
		return Value(true);
	});

	// ---- settings (machine scope: settings.json)
	api.store("settings", {[] { return Settings::values(); }, [] { return Settings::version(); },
	                       [](const View &v) { return Settings::set(v); }});
	api.command("settings.schema", [](const View &, std::string &) { return settings::describe(Scope::Machine); });

	// ---- cache: the downloaded packages and the compile cache
	api.store("cache", {[this] {
		                    const auto current = currentPackageSha();
		                    auto list = choc::value::createEmptyArray();
		                    uint64_t total = 0;
		                    for (auto &p : PackageCache::list()) {
			                    total += p.bytes;
			                    list.addArrayElement(choc::value::createObject("Package", "sha256", p.sha256, "name", p.name, "vendor", p.vendor,
			                                                                   "bytes", (int64_t)p.bytes, "lastUsed", p.lastUsed, "lastSaved", p.lastSaved,
			                                                                   "inUse", p.inUse, "current", p.sha256 == current));
		                    }
		                    auto v = choc::value::createObject("Caches", "packagesBytes", (int64_t)total, "compiledBytes",
		                                                       (int64_t)PackageCache::compiledBytes());
		                    v.addMember("packages", list);
		                    return v;
	                    },
	                    [this] { return Api::mix(cacheVersion_, stateVersion_.load()); }, // a load or unload changes "in use"
	                    {}});
	api.command("cache.delete", [this](const View &a, std::string &) { // {shas: [...]}: skips packages in use
		int deleted = 0;
		auto skipped = choc::value::createEmptyArray();
		if (a.isObject() && a.hasObjectMember("shas") && a["shas"].isArray())
			for (auto s : a["shas"]) {
				if (!s.isString()) continue;
				std::string why;
				if (PackageCache::remove(std::string(s.getString()), why)) ++deleted;
				else skipped.addArrayElement(choc::value::createObject("Skipped", "sha256", std::string(s.getString()), "why", why));
			}
		packagesChanged();
		auto r = choc::value::createObject("Deleted", "deleted", deleted);
		r.addMember("skipped", skipped);
		return r;
	});
	api.command("cache.clearCompiled", [this](const View &, std::string &) {
		PackageCache::clearCompiled();
		++cacheVersion_;
		return Value(true);
	});

	// ---- perf: totals; the page takes deltas (DSP load as JUCE's AudioProcessLoadMeasurer)
	api.stream("perf", [this] {
		return choc::value::createObject("Performance", "blocks", (int64_t)performance.blocks.load(), "overBudget",
		                                 (int64_t)performance.overBudget.load(), "loadPpmSum", (int64_t)performance.loadPpmSum.load(),
		                                 "peakPpm", (int64_t)performance.peakPpm.exchange(0), "skipped",
		                                 (int64_t)wclap_audio_skipped_blocks(), "blockFrames", (int64_t)performance.blockFrames.load(),
		                                 "sampleRate", sampleRate_);
	}, 10);
}

// A drag's end, once applied: {target, id} each. The page keeps a dragged value its own until then.
Value PluginCircuitPlugin::takeEnded() {
	const uint64_t applied = uiEventsApplied_.load(std::memory_order_acquire);
	auto ended = choc::value::createEmptyArray();
	while (!pendingEnds_.empty() && pendingEnds_.front().seq <= applied) {
		auto &e = pendingEnds_.front();
		ended.addArrayElement(choc::value::createObject("E", "target", e.macro ? "macro" : "param", "id", (int64_t)e.id));
		pendingEnds_.pop_front();
	}
	return ended;
}

Value PluginCircuitPlugin::paramsFull() {
	auto *table = paramTable();
	paramsGeneration_ = table ? table->generation() : 0;
	auto infos = choc::value::createEmptyArray();
	if (table)
		for (auto &p : table->infos())
			infos.addArrayElement(choc::value::createObject("P", "id", (int64_t)p.id, "name", p.name, "module", p.module, "min", p.minValue,
			                                                "max", p.maxValue, "default", p.defaultValue, "stepped", p.stepped(),
			                                                "automatable", p.automatable(), "hidden", (p.flags & CLAP_PARAM_IS_HIDDEN) != 0));
	auto v = choc::value::createObject("Params", "full", true);
	v.addMember("ended", takeEnded());
	v.addMember("infos", infos);
	v.addMember("values", table ? paramValues(*table, true) : choc::value::createEmptyArray());
	return v;
}

} // namespace plugincircuit
