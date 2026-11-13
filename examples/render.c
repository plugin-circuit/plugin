/*
 * plugincircuit-render: the smallest possible Plugin Circuit host, in plain C.
 *   plugincircuit-render <plugin-id> out.wav [seconds]
 * Fetches the plugin from the catalog, opens it, plays a chord (instruments) or a decaying noise burst
 * (effects) through it and writes a 48 kHz stereo float WAV. Only libplugincircuit + the CLAP headers.
 */
#include <plugincircuit/plugincircuit.h>
#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define BLOCK 256

static const void *host_get_extension(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void host_noop(const clap_host_t *h) { (void)h; }
static const clap_host_t host = {CLAP_VERSION_INIT, NULL, "plugincircuit-render", "Plugin Circuit", "", "0.1.0",
                                 host_get_extension, host_noop, host_noop, host_noop};

/* --- events --- */
typedef struct { clap_event_note_t notes[8]; uint32_t count; } note_list;
static uint32_t in_size(const clap_input_events_t *l) { return ((const note_list *)l->ctx)->count; }
static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i) { return &((const note_list *)l->ctx)->notes[i].header; }
static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }

static void add_note(note_list *l, uint16_t type, int16_t key) {
	clap_event_note_t *n = &l->notes[l->count++];
	memset(n, 0, sizeof(*n));
	n->header.size = sizeof(*n);
	n->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	n->header.type = type;
	n->note_id = -1;
	n->key = key;
	n->velocity = type == CLAP_EVENT_NOTE_ON ? 0.8 : 0.0;
}

static bool progress(void *ctx, uint64_t got, uint64_t total) {
	(void)ctx;
	if (total) fprintf(stderr, "\rdownloading %3d%%", (int)(100 * got / total));
	return true;
}

static void write_wav(const char *path, const float *interleaved, uint32_t frames) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t data = frames * 2 * 4, riff = 36 + data, fmt = 16, rate = SR, byteRate = SR * 8;
	uint16_t format = 3, channels = 2, align = 8, bits = 32;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
	fwrite(&format, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
	fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	fwrite(interleaved, 4, (size_t)frames * 2, f);
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <plugin-id> out.wav [seconds]\n", argv[0]);
		return 2;
	}
	const char *pluginId = argv[1];
	double seconds = argc > 3 ? atof(argv[3]) : 3.0;
	char error[512] = {0}, dir[2048] = {0};

	plugincircuit_configure(NULL, NULL);
	if (!plugincircuit_fetch(pluginId, NULL, progress, NULL, dir, sizeof(dir), error, sizeof(error))) {
		fprintf(stderr, "fetch failed: %s\n", error);
		return 1;
	}
	fprintf(stderr, "\nbundle: %s\n", dir);
	plugincircuit_module *module = plugincircuit_open(dir, error, sizeof(error));
	if (!module) {
		fprintf(stderr, "open failed: %s\n", error);
		return 1;
	}
	const clap_plugin_factory_t *factory = plugincircuit_factory(module);
	const clap_plugin_t *plugin = factory->create_plugin(factory, &host, pluginId);
	if (!plugin || !plugin->init(plugin) || !plugin->activate(plugin, SR, 1, BLOCK) || !plugin->start_processing(plugin)) {
		fprintf(stderr, "could not start %s\n", pluginId);
		return 1;
	}
	int instrument = 0;
	for (const char *const *f = factory->get_plugin_descriptor(factory, 0)->features; f && *f; ++f)
		if (!strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT)) instrument = 1;

	uint32_t frames = (uint32_t)(seconds * SR) / BLOCK * BLOCK;
	float *wav = calloc((size_t)frames * 2, sizeof(float));
	float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
	float *ins[2] = {inL, inR}, *outs[2] = {outL, outR};
	clap_audio_buffer_t in = {ins, NULL, 2, 0, 0}, out = {outs, NULL, 2, 0, 0};
	note_list notes;
	clap_input_events_t inEvents = {&notes, in_size, in_get};
	clap_output_events_t outEvents = {NULL, out_push};
	uint32_t seed = 1;

	for (uint32_t pos = 0; pos < frames; pos += BLOCK) {
		notes.count = 0;
		if (pos == 0) { add_note(&notes, CLAP_EVENT_NOTE_ON, 57); add_note(&notes, CLAP_EVENT_NOTE_ON, 60); add_note(&notes, CLAP_EVENT_NOTE_ON, 64); }
		if (pos == frames / 2 / BLOCK * BLOCK) { add_note(&notes, CLAP_EVENT_NOTE_OFF, 57); add_note(&notes, CLAP_EVENT_NOTE_OFF, 60); add_note(&notes, CLAP_EVENT_NOTE_OFF, 64); }
		for (int i = 0; i < BLOCK; ++i) {
			seed = seed * 1664525u + 1013904223u;
			float env = instrument ? 0.0f : expf(-(float)(pos + i) / (SR * 0.3f));
			inL[i] = inR[i] = env * ((seed >> 9) * (1.0f / 8388608.0f) - 1.0f) * 0.5f;
		}
		clap_process_t p = {(int64_t)pos, BLOCK, NULL, &in, &out, 1, 1, &inEvents, &outEvents};
		plugin->process(plugin, &p);
		for (int i = 0; i < BLOCK; ++i) {
			wav[2 * (pos + i)] = outL[i];
			wav[2 * (pos + i) + 1] = outR[i];
		}
	}
	plugin->stop_processing(plugin);
	plugin->deactivate(plugin);
	plugin->destroy(plugin);
	plugincircuit_close(module);

	write_wav(argv[2], wav, frames);
	double peak = 0;
	for (uint32_t i = 0; i < frames * 2; ++i) peak = fmax(peak, fabs(wav[i]));
	fprintf(stderr, "wrote %s (%.1f s, peak %.3f)\n", argv[2], frames / (double)SR, peak);
	free(wav);
	return 0;
}
