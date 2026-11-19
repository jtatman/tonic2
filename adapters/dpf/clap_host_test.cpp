// Minimal CLAP host used as a test: loads a built .clap, checks the descriptor and parameters, sends a
// chord as CLAP note events plus a parameter change, renders 2 s and checks the audio.
// Usage: tonic2_clap_host_test path/to/plugin.clap [out.wav]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "entry.h"
#include "events.h"
#include "ext/params.h"
#include "host.h"
#include "plugin-factory.h"
#include "plugin.h"
#include "process.h"
#include "tonic/render/wav.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostNoop(const clap_host_t*) {}

// Input event list over a fixed array of events (one block's worth).
struct EventList {
  std::vector<const clap_event_header_t*> events;
  static uint32_t size(const clap_input_events_t* l) {
    return static_cast<uint32_t>(static_cast<const EventList*>(l->ctx)->events.size());
  }
  static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
    return static_cast<const EventList*>(l->ctx)->events[i];
  }
};
bool tryPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

clap_event_note_t note(uint16_t type, uint32_t time, int16_t key, double velocity) {
  clap_event_note_t e{};
  e.header.size = sizeof(e);
  e.header.time = time;
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = type;
  e.note_id = -1;
  e.port_index = 0;
  e.channel = 0;
  e.key = key;
  e.velocity = velocity;
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s plugin.clap [out.wav]\n", argv[0]); return 2; }
  const char* path = argv[1];
#if defined(_WIN32)
  HMODULE lib = LoadLibraryA(path);
  auto* entry = lib ? reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry")) : nullptr;
#else
  void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) std::fprintf(stderr, "dlopen: %s\n", dlerror());
  auto* entry = lib ? static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry")) : nullptr;
#endif
  check(entry != nullptr, "load plugin and find clap_entry", path);
  if (!entry) return 1;
  check(entry->init(path), "entry init", "ok");
  auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  const uint32_t count = factory ? factory->get_plugin_count(factory) : 0;
  check(count == 1, "factory has one plugin", std::to_string(count));
  if (count != 1) return 1;
  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
  check(std::string(desc->id) == "io.github.jtatman.tonic2.poly", "descriptor id", desc->id);

  clap_host_t host{CLAP_VERSION, nullptr, "tonic2 test host", "tonic2", "", "0.1",
                   hostGetExtension, hostNoop, hostNoop, hostNoop};
  const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
  check(plugin && plugin->init(plugin), "create + init plugin", "ok");
  if (!plugin) return 1;

  auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const uint32_t nParams = params ? params->count(plugin) : 0;
  clap_param_info_t cutoff{};
  bool haveCutoff = false;
  for (uint32_t i = 0; i < nParams; ++i) {
    clap_param_info_t info{};
    if (params->get_info(plugin, i, &info) && std::string(info.name) == "cutoff") { cutoff = info; haveCutoff = true; }
  }
  check(haveCutoff && cutoff.min_value == 100.0 && cutoff.max_value == 8000.0 && cutoff.default_value == 900.0,
        "parameter 'cutoff' exposed with range and default", std::to_string(nParams) + " param(s)");

  const double sr = 48000.0;
  const uint32_t block = 256;
  check(plugin->activate(plugin, sr, 1, block) && plugin->start_processing(plugin), "activate + start", "48 kHz");

  const int blocks = static_cast<int>(2.0 * sr / block);
  std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(blocks) * block));
  std::vector<float> l(block), r(block);
  float* chans[2] = {l.data(), r.data()};
  clap_audio_buffer_t audioOut{};
  audioOut.data32 = chans;
  audioOut.channel_count = 2;

  const int16_t chord[3] = {57, 60, 64};
  for (int b = 0; b < blocks; ++b) {
    std::vector<clap_event_note_t> notes;
    clap_event_param_value_t pv{};
    EventList list;
    if (b == 0) for (int16_t k : chord) notes.push_back(note(CLAP_EVENT_NOTE_ON, 10, k, 0.8));
    if (b == blocks / 2) for (int16_t k : chord) notes.push_back(note(CLAP_EVENT_NOTE_OFF, 0, k, 0.0));
    for (auto& n : notes) list.events.push_back(&n.header);
    if (b == 20) {
      pv.header.size = sizeof(pv);
      pv.header.time = 0;
      pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      pv.header.type = CLAP_EVENT_PARAM_VALUE;
      pv.param_id = cutoff.id;
      pv.note_id = -1;
      pv.port_index = -1;
      pv.channel = -1;
      pv.key = -1;
      pv.value = 3000.0;
      list.events.push_back(&pv.header);
    }
    clap_input_events_t in{&list, EventList::size, EventList::get};
    clap_output_events_t outEv{nullptr, tryPush};
    clap_process_t proc{};
    proc.steady_time = static_cast<int64_t>(b) * block;
    proc.frames_count = block;
    proc.audio_outputs = &audioOut;
    proc.audio_outputs_count = 1;
    proc.in_events = &in;
    proc.out_events = &outEv;
    plugin->process(plugin, &proc);
    for (uint32_t i = 0; i < block; ++i) {
      out[0][static_cast<size_t>(b) * block + i] = l[i];
      out[1][static_cast<size_t>(b) * block + i] = r[i];
    }
  }

  double value = 0;
  const bool got = params && params->get_value(plugin, cutoff.id, &value);
  check(got && value == 3000.0, "parameter change event reaches the plugin", std::to_string(value));

  double peak = 0, sum = 0;
  bool finite = true;
  for (const auto& c : out)
    for (float v : c) { finite &= std::isfinite(v); peak = std::max(peak, double(std::fabs(v))); sum += double(v) * v; }
  const double rms = std::sqrt(sum / (2.0 * out[0].size()));
  check(finite && peak > 0.01 && peak < 1.0 && rms > 1e-3, "renders a chord through the plugin",
        "peak " + std::to_string(peak) + ", rms " + std::to_string(rms));
  // The first block before the note (frames < 10) must be silent: notes are sample-accurate.
  check(out[0][0] == 0.f && out[0][5] == 0.f, "silent before the first note-on frame", "ok");
  if (argc > 2) tonic::render::writeWavFloat(argv[2], out, static_cast<int>(sr));

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  entry->deinit();
#if defined(_WIN32)
  FreeLibrary(lib);
#else
  dlclose(lib);
#endif
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
