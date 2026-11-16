#pragma once
// Global settings shared by every Plugin Circuit instance on this machine (Paths::root()/settings.json).
// File IO: main thread only.
namespace plugincircuit {

struct Settings {
	// Single-threaded WCLAPs: how long the audio thread may spin for the plugin's instance while the main thread is
	// inside it, as a fraction of the block (0 = try once; 0.9 max), before it skips the block. See wclap-bridge's
	// wclap_set_audio_wait_ratio().
	static constexpr float kDefaultAudioWaitRatio = 0.2f;
	float audioWaitRatio = kDefaultAudioWaitRatio;
	// A skipped block continues the audio (linear prediction, faded) instead of cutting to silence: no clicks at its
	// edges, no added latency. wclap_set_conceal_dropouts().
	bool concealDropouts = true;

	static Settings load();
	void save() const;
	void apply() const; // pushes the values to the bridge
};

} // namespace plugincircuit
