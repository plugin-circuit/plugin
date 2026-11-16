// Test WCLAPs for the Plugin Circuit upload checker. One source, built once per variant:
//   -DTEST_GAIN          stereo effect, Gain (dB), latency 0, webview GUI (/ui/index.html served from the bundle)
//   -DTEST_LOOKAHEAD     stereo effect delaying audio by 128 samples but reporting latency 0 (deliberately wrong)
//   -DTEST_SINE          instrument, sample-accurate polyphonic sines with a 5 ms attack
//   -DTEST_BROKEN_STATE  stereo effect whose state load always fails
//   -DTEST_PARAM_EVENTS  instrument that changes its own parameter like a GUI does (for host tests): note-on key 60
//                        sets Level to -6 dB and reports it (gesture begin, value, gesture end as output events);
//                        key 61 sets it to -30 dB without reporting; its page's "set gain" reports with a gesture too
// Freestanding wasm32: no libc, no WASI imports. Exports memory, clap_entry, malloc, free and the function table.
#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "rt.h"

#if defined(TEST_GAIN)
#	define PLUGIN_ID "studio.plugincircuit.test.gain"
#	define PLUGIN_NAME "Test Gain"
#	define PLUGIN_DESC "Checker test: a well-behaved stereo gain with a webview GUI."
#	define IS_INSTRUMENT 0
#	define HAS_GUI 1
#	define DELAY_SAMPLES 0
#	define STATE_LOAD_FAILS 0
#elif defined(TEST_LOOKAHEAD)
#	define PLUGIN_ID "studio.plugincircuit.test.lookahead"
#	define PLUGIN_NAME "Test Lookahead"
#	define PLUGIN_DESC "Checker test: delays audio by 128 samples but reports 0 latency."
#	define IS_INSTRUMENT 0
#	define HAS_GUI 0
#	define DELAY_SAMPLES 128
#	define STATE_LOAD_FAILS 0
#elif defined(TEST_SINE)
#	define PLUGIN_ID "studio.plugincircuit.test.sine"
#	define PLUGIN_NAME "Test Sine"
#	define PLUGIN_DESC "Checker test: sample-accurate polyphonic sine instrument."
#	define IS_INSTRUMENT 1
#	define HAS_GUI 0
#	define DELAY_SAMPLES 0
#	define STATE_LOAD_FAILS 0
#elif defined(TEST_BROKEN_STATE)
#	define PLUGIN_ID "studio.plugincircuit.test.broken-state"
#	define PLUGIN_NAME "Test Broken State"
#	define PLUGIN_DESC "Checker test: a stereo gain whose state load always fails."
#	define IS_INSTRUMENT 0
#	define HAS_GUI 0
#	define DELAY_SAMPLES 0
#	define STATE_LOAD_FAILS 1
#elif defined(TEST_PARAM_EVENTS)
#	define PLUGIN_ID "studio.plugincircuit.test.param-events"
#	define PLUGIN_NAME "Test Param Events"
#	define PLUGIN_DESC "Host test: an instrument that changes its own parameter, reported or not."
#	define IS_INSTRUMENT 1
#	define HAS_GUI 1
#	define DELAY_SAMPLES 0
#	define STATE_LOAD_FAILS 0
#else
#	error "define one of TEST_GAIN, TEST_LOOKAHEAD, TEST_SINE, TEST_BROKEN_STATE, TEST_PARAM_EVENTS"
#endif

#if defined(TEST_PARAM_EVENTS)
#	define REPORTS_GESTURES 1
#else
#	define REPORTS_GESTURES 0
#endif

#if IS_INSTRUMENT
#	define PARAM_NAME "Level"
#	define PARAM_MIN -60.0
#	define PARAM_MAX 0.0
#	define PARAM_DEFAULT -12.0
#else
#	define PARAM_NAME "Gain"
#	define PARAM_MIN -60.0
#	define PARAM_MAX 12.0
#	define PARAM_DEFAULT 0.0
#endif

#define MAX_VOICES 16
#define STATE_MAGIC 0x31544243u /* "CBT1" little endian */

// ---- descriptor -------------------------------------------------------------------------------------------
static const char *features[] = {
#if IS_INSTRUMENT
	CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO,
#else
	CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO,
#endif
	0,
};

static const clap_plugin_descriptor_t descriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = PLUGIN_ID,
	.name = PLUGIN_NAME,
	.vendor = "Plugin Circuit Tests",
	.url = "",
	.manual_url = "",
	.support_url = "",
	.version = "1.0.0",
	.description = PLUGIN_DESC,
	.features = features,
};

// ---- instance ---------------------------------------------------------------------------------------------
typedef struct {
	int active;
	int16_t key, channel, port;
	int32_t noteId;
	float phase, inc, level, target, rate;
} Voice;

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	double sampleRate;
	volatile double value;      // the one parameter, in dB
	volatile int guiChanged;    // GUI moved the value: tell the host (output param event)
	volatile int gesture;       // ... wrapped in gesture begin / end (REPORTS_GESTURES)
	volatile int hostChanged;   // host/automation moved it: tell the GUI (on the main thread)
	int guiOpen;
	float *delay;               // DELAY_SAMPLES * 2 floats, allocated in activate
	uint32_t delayPos;
	Voice voices[MAX_VOICES];
} Plugin;

static Plugin *self(const clap_plugin_t *p) { return (Plugin *)p->plugin_data; }


// ---- events -----------------------------------------------------------------------------------------------
static void setValue(Plugin *s, double v) {
	if (v < PARAM_MIN) v = PARAM_MIN;
	if (v > PARAM_MAX) v = PARAM_MAX;
	s->value = v;
}

#if IS_INSTRUMENT
static void noteOn(Plugin *s, int16_t port, int16_t channel, int16_t key, int32_t noteId, double velocity) {
	Voice *v = 0;
	for (int i = 0; i < MAX_VOICES && !v; ++i)
		if (!s->voices[i].active) v = &s->voices[i];
	if (!v) v = &s->voices[0]; // steal
	float freq = 440.0f * rt_pow2(((float)key - 69.0f) / 12.0f);
	v->active = 1;
	v->key = key;
	v->channel = channel;
	v->port = port;
	v->noteId = noteId;
	v->phase = 0.25f; // start at the peak so the first sample already sounds (onset = the event time)
	v->inc = freq / (float)s->sampleRate;
	v->level = 0;
	v->target = (float)velocity;
	v->rate = (float)(1.0 / (0.005 * s->sampleRate)); // 5 ms attack (per unit of target)
}

static void noteOff(Plugin *s, int16_t channel, int16_t key, int32_t noteId) {
	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice *v = &s->voices[i];
		if (!v->active || v->target == 0) continue;
		if ((noteId >= 0 && v->noteId == noteId) || ((key < 0 || v->key == key) && (channel < 0 || v->channel == channel))) {
			v->target = 0;
			v->rate = (float)(1.0 / (0.020 * s->sampleRate)); // 20 ms release
		}
	}
}
#endif

static void handleEvent(Plugin *s, const clap_event_header_t *h) {
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
	switch (h->type) {
	case CLAP_EVENT_PARAM_VALUE: {
		const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
		if (e->param_id == 0) {
			setValue(s, e->value);
			s->hostChanged = 1;
		}
		break;
	}
#if IS_INSTRUMENT
	case CLAP_EVENT_NOTE_ON: {
		const clap_event_note_t *e = (const clap_event_note_t *)h;
#	if defined(TEST_PARAM_EVENTS)
		if (e->key == 60) { // a change made "in the GUI": reported, with a gesture
			setValue(s, -6.0);
			s->guiChanged = 1;
			s->gesture = 1;
			break;
		}
		if (e->key == 61) { // changed internally and not reported (a host can only notice by asking)
			setValue(s, -30.0);
			break;
		}
#	endif
		noteOn(s, e->port_index, e->channel, e->key, e->note_id, e->velocity);
		break;
	}
	case CLAP_EVENT_NOTE_OFF:
	case CLAP_EVENT_NOTE_CHOKE: {
		const clap_event_note_t *e = (const clap_event_note_t *)h;
		noteOff(s, e->channel, e->key, e->note_id);
		break;
	}
	case CLAP_EVENT_MIDI: {
		const clap_event_midi_t *e = (const clap_event_midi_t *)h;
		uint8_t status = e->data[0] & 0xf0, channel = e->data[0] & 0x0f;
		if (status == 0x90 && e->data[2] > 0) noteOn(s, (int16_t)e->port_index, channel, e->data[1], -1, e->data[2] / 127.0);
		else if (status == 0x80 || (status == 0x90 && e->data[2] == 0)) noteOff(s, channel, e->data[1], -1);
		break;
	}
#endif
	default:
		break;
	}
}

// GUI-originated value changes go to the host as output events (process or flush)
static void pushGuiChange(Plugin *s, const clap_output_events_t *out) {
	if (!s->guiChanged || !out) return;
	s->guiChanged = 0;
	const int gesture = REPORTS_GESTURES && s->gesture;
	s->gesture = 0;
	clap_event_param_gesture_t g = {.header = {sizeof(g), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0}, .param_id = 0};
	if (gesture) out->try_push(out, &g.header);
	clap_event_param_value_t e = {
		.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
		.param_id = 0, .cookie = 0, .note_id = -1, .port_index = -1, .channel = -1, .key = -1,
		.value = s->value,
	};
	out->try_push(out, &e.header);
	g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
	if (gesture) out->try_push(out, &g.header);
}

// ---- audio ------------------------------------------------------------------------------------------------
static void render(Plugin *s, const clap_process_t *p, uint32_t from, uint32_t to) {
	if (from >= to || p->audio_outputs_count == 0) return;
	const clap_audio_buffer_t *ob = &p->audio_outputs[0];
	float gain = rt_db_to_gain((float)s->value);
#if IS_INSTRUMENT
	for (uint32_t ch = 0; ch < ob->channel_count; ++ch)
		for (uint32_t i = from; i < to; ++i) ob->data32[ch][i] = 0.0f;
	for (int k = 0; k < MAX_VOICES; ++k) {
		Voice *v = &s->voices[k];
		if (!v->active) continue;
		for (uint32_t i = from; i < to; ++i) {
			if (v->level < v->target) {
				v->level += v->rate * v->target;
				if (v->level > v->target) v->level = v->target;
			} else if (v->level > v->target) {
				v->level -= v->rate;
				if (v->level < 0) v->level = 0;
			}
			float x = rt_sin2pi(v->phase) * v->level * gain * 0.3f;
			v->phase += v->inc;
			if (v->phase >= 1.0f) v->phase -= 1.0f;
			for (uint32_t ch = 0; ch < ob->channel_count; ++ch) ob->data32[ch][i] += x;
		}
		if (v->target == 0 && v->level == 0) {
			v->active = 0;
			clap_event_note_t end = {
				.header = {sizeof(end), to, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0},
				.note_id = v->noteId, .port_index = v->port, .channel = v->channel, .key = v->key, .velocity = 0,
			};
			if (p->out_events) p->out_events->try_push(p->out_events, &end.header);
		}
	}
#else
	const clap_audio_buffer_t *ib = p->audio_inputs_count ? &p->audio_inputs[0] : 0;
	for (uint32_t ch = 0; ch < ob->channel_count; ++ch) {
		const float *in = ib && ib->channel_count ? ib->data32[ch < ib->channel_count ? ch : ib->channel_count - 1] : 0;
		float *out = ob->data32[ch];
#	if DELAY_SAMPLES
		float *line = s->delay + (ch < 2 ? ch : 1) * DELAY_SAMPLES;
		uint32_t pos = s->delayPos;
		for (uint32_t i = from; i < to; ++i) {
			float x = in ? in[i] : 0.0f;
			out[i] = line[pos] * gain;
			line[pos] = x;
			if (++pos == DELAY_SAMPLES) pos = 0;
		}
#	else
		for (uint32_t i = from; i < to; ++i) out[i] = (in ? in[i] : 0.0f) * gain;
#	endif
	}
#	if DELAY_SAMPLES
	s->delayPos = (s->delayPos + (to - from)) % DELAY_SAMPLES;
#	endif
#endif
}

static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *p) {
	Plugin *s = self(plugin);
	uint32_t n = p->in_events ? p->in_events->size(p->in_events) : 0;
	uint32_t pos = 0;
	for (uint32_t k = 0; k < n; ++k) {
		const clap_event_header_t *h = p->in_events->get(p->in_events, k);
		uint32_t t = h->time < p->frames_count ? h->time : p->frames_count;
		render(s, p, pos, t); // sample accurate: render up to the event, then apply it
		pos = t > pos ? t : pos;
		handleEvent(s, h);
	}
	render(s, p, pos, p->frames_count);
	pushGuiChange(s, p->out_events);
	if (s->hostChanged && s->guiOpen) s->host->request_callback(s->host); // thread-safe; GUI update on main thread
	return CLAP_PROCESS_CONTINUE;
}

// ---- params -----------------------------------------------------------------------------------------------
static uint32_t params_count(const clap_plugin_t *p) { (void)p; return 1; }

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
	(void)p;
	if (index != 0) return false;
	memset(info, 0, sizeof(*info));
	info->id = 0;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE;
	rt_strncpy(info->name, PARAM_NAME, sizeof(info->name));
	info->module[0] = 0;
	info->min_value = PARAM_MIN;
	info->max_value = PARAM_MAX;
	info->default_value = PARAM_DEFAULT;
	return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *value) {
	if (id != 0) return false;
	*value = self(p)->value;
	return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double value, char *out, uint32_t cap) {
	(void)p;
	if (id != 0) return false;
	rt_format_fixed1(out, cap, value, " dB");
	return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id, const char *text, double *value) {
	(void)p;
	if (id != 0) return false;
	return rt_parse_number(text, value) != 0;
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
	Plugin *s = self(p);
	uint32_t n = in ? in->size(in) : 0;
	for (uint32_t k = 0; k < n; ++k) handleEvent(s, in->get(in, k));
	pushGuiChange(s, out);
}

static const clap_plugin_params_t paramsExt = {
	params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush,
};

// ---- state ------------------------------------------------------------------------------------------------
static bool writeAll(const clap_ostream_t *os, const void *data, uint64_t size) {
	const unsigned char *b = (const unsigned char *)data;
	while (size) {
		int64_t n = os->write(os, b, size);
		if (n <= 0) return false;
		b += n;
		size -= (uint64_t)n;
	}
	return true;
}

static bool readAll(const clap_istream_t *is, void *data, uint64_t size) {
	unsigned char *b = (unsigned char *)data;
	while (size) {
		int64_t n = is->read(is, b, size);
		if (n <= 0) return false;
		b += n;
		size -= (uint64_t)n;
	}
	return true;
}

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *os) {
	struct { uint32_t magic, pad; double value; } blob = {STATE_MAGIC, 0, self(p)->value};
	return writeAll(os, &blob, sizeof(blob));
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *is) {
	struct { uint32_t magic, pad; double value; } blob;
	if (!readAll(is, &blob, sizeof(blob)) || blob.magic != STATE_MAGIC) return false;
#if STATE_LOAD_FAILS
	(void)p;
	return false; // deliberately broken: the checker must flag the failed round trip
#else
	Plugin *s = self(p);
	setValue(s, blob.value);
	s->hostChanged = 1;
	return true;
#endif
}

static const clap_plugin_state_t stateExt = {state_save, state_load};

// ---- ports, latency ---------------------------------------------------------------------------------------
static uint32_t audio_ports_count(const clap_plugin_t *p, bool isInput) {
	(void)p;
	return isInput ? (IS_INSTRUMENT ? 0 : 1) : 1;
}

static bool audio_ports_get(const clap_plugin_t *p, uint32_t index, bool isInput, clap_audio_port_info_t *info) {
	(void)p;
	if (index != 0 || (isInput && IS_INSTRUMENT)) return false;
	memset(info, 0, sizeof(*info));
	info->id = isInput ? 0 : 1;
	rt_strncpy(info->name, isInput ? "Input" : "Output", sizeof(info->name));
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = IS_INSTRUMENT ? CLAP_INVALID_ID : (isInput ? 1 : 0);
	return true;
}

static const clap_plugin_audio_ports_t audioPortsExt = {audio_ports_count, audio_ports_get};

#if IS_INSTRUMENT
static uint32_t note_ports_count(const clap_plugin_t *p, bool isInput) { (void)p; return isInput ? 1 : 0; }

static bool note_ports_get(const clap_plugin_t *p, uint32_t index, bool isInput, clap_note_port_info_t *info) {
	(void)p;
	if (index != 0 || !isInput) return false;
	memset(info, 0, sizeof(*info));
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
	rt_strncpy(info->name, "Notes", sizeof(info->name));
	return true;
}

static const clap_plugin_note_ports_t notePortsExt = {note_ports_count, note_ports_get};
#endif

// Always 0, also for TEST_LOOKAHEAD (which is the point of that test)
static uint32_t latency_get(const clap_plugin_t *p) { (void)p; return 0; }
static const clap_plugin_latency_t latencyExt = {latency_get};

// ---- webview GUI ------------------------------------------------------------------------------------------
#if HAS_GUI
static const void *hostExt(Plugin *s, const char *id) { return s->host->get_extension(s->host, id); }

// Page protocol (binary, little endian): page -> plugin  [0] hello, [1, f32 dB] set gain
//                                        plugin -> page  [2, f32 dB] current gain
static void sendValue(Plugin *s) {
	const clap_host_webview_t *hw = (const clap_host_webview_t *)hostExt(s, CLAP_EXT_WEBVIEW);
	if (!hw || !s->guiOpen) return;
	unsigned char msg[5];
	float v = (float)s->value;
	msg[0] = 2;
	memcpy(msg + 1, &v, 4);
	hw->send(s->host, msg, sizeof(msg));
}

static bool gui_is_api_supported(const clap_plugin_t *p, const char *api, bool floating) {
	(void)p;
	return !floating && !strcmp(api, CLAP_WINDOW_API_WEBVIEW);
}
static bool gui_get_preferred_api(const clap_plugin_t *p, const char **api, bool *floating) {
	(void)p;
	*api = CLAP_WINDOW_API_WEBVIEW;
	*floating = false;
	return true;
}
static bool gui_create(const clap_plugin_t *p, const char *api, bool floating) {
	if (!gui_is_api_supported(p, api, floating)) return false;
	self(p)->guiOpen = 1;
	return true;
}
static void gui_destroy(const clap_plugin_t *p) { self(p)->guiOpen = 0; }
static bool gui_set_scale(const clap_plugin_t *p, double scale) { (void)p; (void)scale; return false; }
static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { (void)p; *w = 360; *h = 220; return true; }
static bool gui_can_resize(const clap_plugin_t *p) { (void)p; return false; }
static bool gui_get_resize_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) { (void)p; (void)h; return false; }
static bool gui_adjust_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return gui_get_size(p, w, h); }
static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) { (void)p; return w == 360 && h == 220; }
static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return true; }
static bool gui_set_transient(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return false; }
static void gui_suggest_title(const clap_plugin_t *p, const char *t) { (void)p; (void)t; }
static bool gui_show(const clap_plugin_t *p) { (void)p; return true; }
static bool gui_hide(const clap_plugin_t *p) { (void)p; return true; }

static const clap_plugin_gui_t guiExt = {
	gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size,
	gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
	gui_suggest_title, gui_show, gui_hide,
};

static int32_t webview_get_uri(const clap_plugin_t *p, char *uri, uint32_t cap) {
	(void)p;
	static const char start[] = "/ui/index.html";
	if (uri && cap) rt_strncpy(uri, start, cap);
	return (int32_t)sizeof(start); // length including the terminator
}

// The page lives in the bundle (ui/index.html): hosts serve it, so no resources from here
static bool webview_get_resource(const clap_plugin_t *p, const char *path, char *mime, uint32_t cap,
                                 const clap_ostream_t *os) {
	(void)p; (void)path; (void)mime; (void)cap; (void)os;
	return false;
}

static bool webview_receive(const clap_plugin_t *p, const void *buffer, uint32_t size) {
	Plugin *s = self(p);
	const unsigned char *b = (const unsigned char *)buffer;
	if (size < 1) return false;
	if (b[0] == 0) { // hello
		s->guiOpen = 1;
		sendValue(s);
		return true;
	}
	if (b[0] == 1 && size >= 5) {
		float v;
		memcpy(&v, b + 1, 4);
		setValue(s, v);
		s->guiChanged = 1;
		s->gesture = 1;
		const clap_host_params_t *hp = (const clap_host_params_t *)hostExt(s, CLAP_EXT_PARAMS);
		if (hp) hp->request_flush(s->host);
		return true;
	}
	return false;
}

static const clap_plugin_webview_t webviewExt = {webview_get_uri, webview_get_resource, webview_receive};
#endif

// ---- plugin -----------------------------------------------------------------------------------------------
static bool plugin_init(const clap_plugin_t *p) { (void)p; return true; }

static void plugin_destroy(const clap_plugin_t *p) {
	Plugin *s = self(p);
	free(s->delay);
	free(s);
}

static bool plugin_activate(const clap_plugin_t *p, double sr, uint32_t minFrames, uint32_t maxFrames) {
	(void)minFrames; (void)maxFrames;
	Plugin *s = self(p);
	s->sampleRate = sr;
#if DELAY_SAMPLES
	if (!s->delay) s->delay = (float *)malloc(sizeof(float) * DELAY_SAMPLES * 2);
	if (!s->delay) return false;
	memset(s->delay, 0, sizeof(float) * DELAY_SAMPLES * 2);
	s->delayPos = 0;
#endif
	memset(s->voices, 0, sizeof(s->voices));
	return true;
}

static void plugin_deactivate(const clap_plugin_t *p) { (void)p; }
static bool plugin_start_processing(const clap_plugin_t *p) { (void)p; return true; }
static void plugin_stop_processing(const clap_plugin_t *p) { (void)p; }

static void plugin_reset(const clap_plugin_t *p) {
	Plugin *s = self(p);
	memset(s->voices, 0, sizeof(s->voices));
	if (s->delay) memset(s->delay, 0, sizeof(float) * DELAY_SAMPLES * 2);
	s->delayPos = 0;
}

static const void *plugin_get_extension(const clap_plugin_t *p, const char *id) {
	(void)p;
	if (!strcmp(id, CLAP_EXT_PARAMS)) return &paramsExt;
	if (!strcmp(id, CLAP_EXT_STATE)) return &stateExt;
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExt;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &latencyExt;
#if IS_INSTRUMENT
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePortsExt;
#endif
#if HAS_GUI
	if (!strcmp(id, CLAP_EXT_GUI)) return &guiExt;
	if (!strcmp(id, CLAP_EXT_WEBVIEW)) return &webviewExt;
#endif
	return 0;
}

static void plugin_on_main_thread(const clap_plugin_t *p) {
	Plugin *s = self(p);
	if (!s->hostChanged) return;
	s->hostChanged = 0;
#if HAS_GUI
	sendValue(s);
#endif
}

static const clap_plugin_t *create(const clap_host_t *host) {
	Plugin *s = (Plugin *)malloc(sizeof(Plugin));
	if (!s) return 0;
	memset(s, 0, sizeof(*s));
	s->host = host;
	s->sampleRate = 48000;
	s->value = PARAM_DEFAULT;
	s->plugin.desc = &descriptor;
	s->plugin.plugin_data = s;
	s->plugin.init = plugin_init;
	s->plugin.destroy = plugin_destroy;
	s->plugin.activate = plugin_activate;
	s->plugin.deactivate = plugin_deactivate;
	s->plugin.start_processing = plugin_start_processing;
	s->plugin.stop_processing = plugin_stop_processing;
	s->plugin.reset = plugin_reset;
	s->plugin.process = process;
	s->plugin.get_extension = plugin_get_extension;
	s->plugin.on_main_thread = plugin_on_main_thread;
	return &s->plugin;
}

// ---- factory + entry --------------------------------------------------------------------------------------
static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t i) {
	(void)f;
	return i == 0 ? &descriptor : 0;
}
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
	(void)f;
	if (!clap_version_is_compatible(host->clap_version) || strcmp(id, PLUGIN_ID)) return 0;
	return create(host);
}
static const clap_plugin_factory_t factory = {factory_count, factory_descriptor, factory_create};

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? 0 : &factory; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
