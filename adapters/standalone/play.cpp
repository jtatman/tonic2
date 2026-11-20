// tonic2_play: run a tonic2 patch on an audio device with live MIDI input, no plugin host needed.
//
//   tonic2_play --list                          list audio outputs and MIDI inputs
//   tonic2_play --demo poly --midi 0            play the poly demo from MIDI input port 0
//   tonic2_play --demo mono --audio 2 --midi all
//   tonic2_play --null --notes --seconds 1      silent self-test (null audio backend + a test chord)
//
// Audio: miniaudio (WASAPI, CoreAudio, ALSA/PulseAudio, JACK). MIDI: RtMidi (WinMM, CoreMIDI, ALSA).
// MIDI messages go to the audio thread through a lock-free queue and take effect at the start of the next
// audio block. The audio callback never allocates or locks.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
#include "demo_synths.hpp"
#include "tonic/engine/engine.hpp"
#include "tonic/engine/spsc_queue.hpp"

using namespace tonic;

namespace {

struct App {
  engine::Engine eng;
  engine::SpscQueue<engine::Event, 1024> midiQueue;  // MIDI thread -> audio thread
  std::vector<float> left, right;                    // planar scratch, allocated before the device starts
  std::atomic<uint64_t> frames{0}, callbacks{0};
  std::atomic<float> peak{0.f};
  std::atomic<int> dropped{0};
};

constexpr int kMaxBlock = 512;

void audioCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
  auto* app = static_cast<App*>(device->pUserData);
  float* out = static_cast<float*>(output);
  engine::Event events[256];
  int numEvents = 0;
  engine::Event ev;
  while (numEvents < 256 && app->midiQueue.pop(ev)) { ev.frame = 0; events[numEvents++] = ev; }
  float blockPeak = 0.f;
  for (ma_uint32 done = 0; done < frameCount;) {
    const int n = static_cast<int>(std::min<ma_uint32>(frameCount - done, kMaxBlock));
    float* ptrs[2] = {app->left.data(), app->right.data()};
    app->eng.process(nullptr, 0, ptrs, 2, n, done == 0 ? events : nullptr, done == 0 ? numEvents : 0);
    for (int i = 0; i < n; ++i) {
      out[2 * (done + i)] = app->left[static_cast<size_t>(i)];
      out[2 * (done + i) + 1] = app->right[static_cast<size_t>(i)];
      blockPeak = std::max(blockPeak, std::max(std::fabs(app->left[static_cast<size_t>(i)]), std::fabs(app->right[static_cast<size_t>(i)])));
    }
    done += static_cast<ma_uint32>(n);
  }
  app->frames.fetch_add(frameCount, std::memory_order_relaxed);
  app->callbacks.fetch_add(1, std::memory_order_relaxed);
  if (blockPeak > app->peak.load(std::memory_order_relaxed)) app->peak.store(blockPeak, std::memory_order_relaxed);
}

// MIDI bytes -> engine event (note on/off, CC, pitch wheel). Returns false for other messages.
bool toEvent(const std::vector<unsigned char>& m, engine::Event& ev) {
  if (m.size() < 3) return false;
  const int status = m[0] & 0xF0, channel = m[0] & 0x0F, d1 = m[1] & 0x7F, d2 = m[2] & 0x7F;
  if (status == 0x90 && d2 > 0) ev = engine::Event::noteOn(0, d1, d2 / 127.f, channel);
  else if (status == 0x80 || status == 0x90) ev = engine::Event::noteOff(0, d1, channel);
  else if (status == 0xB0) ev = engine::Event::cc(0, d1, d2 / 127.f, channel);
  else if (status == 0xE0) ev = engine::Event::pitchWheel(0, (static_cast<float>(d1 | (d2 << 7)) - 8192.f) / 8192.f, channel);
  else return false;
  return true;
}

void midiCallback(double, std::vector<unsigned char>* message, void* user) {
  auto* app = static_cast<App*>(user);
  engine::Event ev;
  if (message && toEvent(*message, ev) && !app->midiQueue.push(ev)) app->dropped.fetch_add(1);
}

int usage() {
  std::printf("usage: tonic2_play [--list] [--demo NAME] [--audio N] [--midi N|all] [--seconds S] [--notes] [--null] [--sr HZ]\n");
  std::printf("demos:");
  for (const auto& d : demos::all()) std::printf(" %s", d.name.c_str());
  std::printf("\n");
  return 2;
}

}  // namespace

int run(int argc, char** argv) {
  std::string demoName = "poly", midiArg;
  int audioIndex = -1;
  double seconds = 0.0;
  bool list = false, notes = false, nullBackend = false;
  ma_uint32 sampleRate = 48000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--list") list = true;
    else if (a == "--demo") demoName = next();
    else if (a == "--audio") audioIndex = std::atoi(next().c_str());
    else if (a == "--midi") midiArg = next();
    else if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--notes") notes = true;
    else if (a == "--null") nullBackend = true;
    else if (a == "--sr") sampleRate = static_cast<ma_uint32>(std::atoi(next().c_str()));
    else return usage();
  }

  ma_context context;
  ma_backend nullOnly[] = {ma_backend_null};
  if (ma_context_init(nullBackend ? nullOnly : nullptr, nullBackend ? 1 : 0, nullptr, &context) != MA_SUCCESS) {
    std::fprintf(stderr, "could not initialise audio\n");
    return 1;
  }
  ma_device_info* playback = nullptr;
  ma_uint32 playbackCount = 0;
  ma_context_get_devices(&context, &playback, &playbackCount, nullptr, nullptr);

  std::unique_ptr<RtMidiIn> midiProbe;
  try { midiProbe = std::make_unique<RtMidiIn>(); } catch (const RtMidiError&) {}
  const unsigned midiPorts = midiProbe ? midiProbe->getPortCount() : 0;

  if (list) {
    std::printf("audio outputs (%s):\n", ma_get_backend_name(context.backend));
    for (ma_uint32 i = 0; i < playbackCount; ++i)
      std::printf("  %u: %s%s\n", i, playback[i].name, playback[i].isDefault ? "  (default)" : "");
    std::printf("MIDI inputs:\n");
    for (unsigned i = 0; i < midiPorts; ++i) std::printf("  %u: %s\n", i, midiProbe->getPortName(i).c_str());
    if (midiPorts == 0) std::printf("  (none)\n");
    ma_context_uninit(&context);
    return 0;
  }

  const auto demoList = demos::all();  // keep alive: `demo` points into it
  const demos::Demo* demo = nullptr;
  for (const auto& d : demoList) if (d.name == demoName) demo = &d;
  if (!demo) { ma_context_uninit(&context); return usage(); }

  auto app = std::make_unique<App>();
  app->left.assign(kMaxBlock, 0.f);
  app->right.assign(kMaxBlock, 0.f);

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = sampleRate;
  cfg.dataCallback = audioCallback;
  cfg.pUserData = app.get();
  if (audioIndex >= 0 && static_cast<ma_uint32>(audioIndex) < playbackCount) cfg.playback.pDeviceID = &playback[audioIndex].id;
  ma_device device;
  if (ma_device_init(&context, &cfg, &device) != MA_SUCCESS) {
    std::fprintf(stderr, "could not open the audio device\n");
    ma_context_uninit(&context);
    return 1;
  }

  // Compile the patch at the device's actual rate before audio starts.
  dsl::Patch patch;
  demo->build(patch);
  auto compiled = patch.compile(device.sampleRate, kMaxBlock);
  if (!compiled.graph) { std::fprintf(stderr, "compile failed: %s\n", compiled.error.c_str()); return 1; }
  app->eng.prepare(device.sampleRate, kMaxBlock, compiled.layout);
  app->eng.setGraph(std::move(compiled.graph));

  std::vector<std::unique_ptr<RtMidiIn>> inputs;
  if (!midiArg.empty()) {
    for (unsigned port = 0; port < midiPorts; ++port) {
      if (midiArg != "all" && std::to_string(port) != midiArg) continue;
      try {
        auto in = std::make_unique<RtMidiIn>();
        in->setCallback(midiCallback, app.get());
        in->ignoreTypes(true, true, true);  // no sysex, timing or active sensing
        in->openPort(port);
        std::printf("MIDI input: %s\n", in->getPortName(port).c_str());
        inputs.push_back(std::move(in));
      } catch (const RtMidiError& e) {
        std::fprintf(stderr, "MIDI port %u: %s\n", port, e.getMessage().c_str());
      }
    }
    if (inputs.empty()) std::fprintf(stderr, "no MIDI input opened (try --list)\n");
  }

  if (ma_device_start(&device) != MA_SUCCESS) { std::fprintf(stderr, "could not start audio\n"); return 1; }
  std::printf("playing '%s' on %s (%s, %u Hz)%s\n", demo->name.c_str(), device.playback.name, ma_get_backend_name(context.backend),
              device.sampleRate, seconds > 0 ? "" : " - press Enter to stop");

  if (notes) {  // test chord, released after a second
    for (int n : {57, 60, 64}) app->midiQueue.push(engine::Event::noteOn(0, n, 0.8f));
  }
  const auto start = std::chrono::steady_clock::now();
  bool released = !notes;
  if (seconds > 0) {
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
      if (!released && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > std::min(1.0, seconds * 0.5)) {
        for (int n : {57, 60, 64}) app->midiQueue.push(engine::Event::noteOff(0, n));
        released = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  } else {
    std::getchar();
  }

  for (auto& in : inputs) in->closePort();
  ma_device_uninit(&device);
  ma_context_uninit(&context);
  const float peak = app->peak.load();
  std::printf("stopped: %llu frames in %llu callbacks, peak %.3f, %d MIDI messages dropped\n",
              static_cast<unsigned long long>(app->frames.load()), static_cast<unsigned long long>(app->callbacks.load()), peak,
              app->dropped.load());
  // In self-test mode, fail if nothing came out.
  return notes && (peak <= 1e-4f || app->frames.load() == 0) ? 1 : 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "error: unknown exception\n");
  }
  return 1;
}
