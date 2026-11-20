// Generic DPF adapter: turns any dsl::Patch build function into a VST3 / CLAP / LV2 plugin.
//
// - Engine parameters (the patch's Layout) become host parameters, in declaration order.
// - MIDI note on/off becomes sample-accurate engine events (frame = MIDI event frame). The MIDI channel is
//   kept, so voices() nodes with VoiceConfig::channel play only their channel.
// - The patch is compiled in activate() (control thread) at the host's sample rate; the audio thread
//   only calls Engine::process, which never allocates, locks or throws.
// - MIDI CC is forwarded as sample-accurate engine CC events (mapped by Layout::ccBindings / MIDI learn).
// - Host automation arrives through setParameterValue() -> Engine::setParam (atomics, smoothed). DPF applies
//   parameter changes at the start of the host block (it does not split blocks at automation points), so
//   automation is block-accurate; MIDI CC stays sample-accurate.
#pragma once

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "DistrhoPlugin.hpp"
#include "tonic/dsl/patch.hpp"
#include "tonic/engine/engine.hpp"
#include "tonic/ui/ccmap.hpp"

START_NAMESPACE_DISTRHO

struct TonicPluginInfo {
  const char* label;        // short ASCII id, e.g. "TonicPoly"
  const char* description;
  const char* maker;
  const char* homepage;
  const char* license;
  uint32_t version;         // d_version(major, minor, micro)
  int64_t uniqueId;         // d_cconst('a','b','c','d')
};

class TonicDpfPlugin : public Plugin {
 public:
  using Build = std::function<void(tonic::dsl::Patch&)>;
  static constexpr int kMaxBlock = 512;
  static constexpr size_t kMaxEvents = 2048;
#if DISTRHO_PLUGIN_WANT_STATE
  static constexpr uint32_t kNumStates = 2;  // "ccmap" (saved with the project), "learn" (UI -> DSP command)
#else
  static constexpr uint32_t kNumStates = 0;
#endif

  // ---- direct access for the UI when it runs in the same process (all calls are lock-free) ----
  void learnCc(int param) { engine_.learnCc(param); }
  bool isLearning() const { return engine_.isLearning(); }
  int ccMapping(int cc) const {
    if (cc < 0 || cc >= 128) return -1;
    return active_ ? engine_.ccMapping(cc) : ccCache_[static_cast<size_t>(cc)].load();
  }
  void mapCc(int cc, int param) {
    if (cc < 0 || cc >= 128) return;
    ccCache_[static_cast<size_t>(cc)].store(param);
    if (active_) engine_.mapCc(cc, param);
  }

  TonicDpfPlugin(Build build, TonicPluginInfo info)
      : Plugin(static_cast<uint32_t>(layoutOf(build).params.size()), 0, kNumStates),
        build_(std::move(build)), info_(info), layout_(layoutOf(build_)) {
    for (auto& c : ccCache_) c.store(-1);
    for (const auto& b : layout_.ccBindings)
      if (b.cc >= 0 && b.cc < 128) ccCache_[static_cast<size_t>(b.cc)].store(b.param);
    values_.reserve(layout_.params.size());
    for (const auto& p : layout_.params) values_.push_back(p.def);
    events_.reserve(kMaxEvents);
  }

 protected:
  const char* getLabel() const override { return info_.label; }
  const char* getDescription() const override { return info_.description; }
  const char* getMaker() const override { return info_.maker; }
  const char* getHomePage() const override { return info_.homepage; }
  const char* getLicense() const override { return info_.license; }
  uint32_t getVersion() const override { return info_.version; }
  int64_t getUniqueId() const override { return info_.uniqueId; }

  void initParameter(uint32_t index, Parameter& p) override {
    const auto& spec = layout_.params[index];
    p.hints = kParameterIsAutomatable;
    p.name = spec.name.c_str();
    p.symbol = symbolFor(spec.name, index).c_str();
    p.ranges.min = spec.min;
    p.ranges.max = spec.max;
    p.ranges.def = spec.def;
  }

  float getParameterValue(uint32_t index) const override { return values_[index]; }
  void setParameterValue(uint32_t index, float value) override {
    values_[index] = value;
    engine_.setParam(static_cast<int>(index), value);
  }

  void activate() override {
    tonic::dsl::Patch patch;
    build_(patch);
    auto compiled = patch.compile(getSampleRate(), kMaxBlock);
    engine_.collectGarbage();
    engine_.prepare(getSampleRate(), kMaxBlock, compiled.layout);
    if (compiled.graph) engine_.setGraph(std::move(compiled.graph));
    for (size_t i = 0; i < values_.size(); ++i) engine_.setParam(static_cast<int>(i), values_[i]);
    for (int cc = 0; cc < 128; ++cc) engine_.mapCc(cc, ccCache_[static_cast<size_t>(cc)].load());  // restored or learned bindings
    active_ = true;
  }
  void deactivate() override {
    snapshotCcMap();  // keep bindings learned while active
    active_ = false;
    engine_.collectGarbage();
  }

#if DISTRHO_PLUGIN_WANT_STATE
  void initState(uint32_t index, State& s) override {
    if (index == 0) {
      s.key = "ccmap";
      s.defaultValue = tonic::ui::serializeCcMap(cacheCopy()).c_str();
      s.label = "MIDI CC map";
      s.hints = kStateIsOnlyForDSP;
    } else {
      s.key = "learn";
      s.defaultValue = "-1";
      s.label = "MIDI learn";
      s.hints = kStateIsOnlyForDSP;
    }
  }
  void setState(const char* key, const char* value) override {
    if (std::strcmp(key, "ccmap") == 0) {
      const auto m = tonic::ui::parseCcMap(value, static_cast<int>(layout_.params.size()));
      for (int cc = 0; cc < 128; ++cc) ccCache_[static_cast<size_t>(cc)].store(m[static_cast<size_t>(cc)]);
      if (active_) for (int cc = 0; cc < 128; ++cc) engine_.mapCc(cc, m[static_cast<size_t>(cc)]);
    } else if (std::strcmp(key, "learn") == 0) {
      engine_.learnCc(std::atoi(value));  // -1 cancels
    }
  }
#if DISTRHO_PLUGIN_WANT_FULL_STATE
  String getState(const char* key) const override {
    if (std::strcmp(key, "ccmap") != 0) return String("-1");
    tonic::ui::CcMap m;
    for (int cc = 0; cc < 128; ++cc) m[static_cast<size_t>(cc)] = ccMapping(cc);
    return String(tonic::ui::serializeCcMap(m).c_str());
  }
#endif
#endif

  void run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount) override {
    events_.clear();  // capacity reserved in the constructor: no allocation here
    for (uint32_t i = 0; i < midiCount && events_.size() < kMaxEvents; ++i) {
      const MidiEvent& m = midi[i];
      if (m.size < 3) continue;
      const uint8_t status = m.data[0] & 0xF0, note = m.data[1] & 0x7F, vel = m.data[2] & 0x7F;
      const int frame = static_cast<int>(m.frame), channel = m.data[0] & 0x0F;
      if (status == 0x90 && vel > 0) events_.push_back(tonic::engine::Event::noteOn(frame, note, vel / 127.f, channel));
      else if (status == 0x80 || (status == 0x90 && vel == 0)) events_.push_back(tonic::engine::Event::noteOff(frame, note, channel));
      else if (status == 0xB0) events_.push_back(tonic::engine::Event::cc(frame, note, vel / 127.f, channel));  // data1 = CC number
      else if (status == 0xE0)  // pitch wheel: 14-bit, centre 8192
        events_.push_back(tonic::engine::Event::pitchWheel(frame, (static_cast<float>(note | (vel << 7)) - 8192.f) / 8192.f, channel));
    }
    engine_.process(nullptr, 0, outputs, DISTRHO_PLUGIN_NUM_OUTPUTS, static_cast<int>(frames), events_.data(),
                    static_cast<int>(events_.size()));
  }

 private:
  tonic::ui::CcMap cacheCopy() const {
    tonic::ui::CcMap m;
    for (int cc = 0; cc < 128; ++cc) m[static_cast<size_t>(cc)] = ccCache_[static_cast<size_t>(cc)].load();
    return m;
  }
  void snapshotCcMap() {
    if (!active_) return;
    for (int cc = 0; cc < 128; ++cc) ccCache_[static_cast<size_t>(cc)].store(engine_.ccMapping(cc));
  }
  static tonic::engine::Layout layoutOf(const Build& build) {
    tonic::dsl::Patch p;
    build(p);
    return p.compile(48000.0, kMaxBlock).layout;
  }
  // LV2 symbols: [a-z_][a-z0-9_]*, unique.
  const std::string& symbolFor(const std::string& name, uint32_t index) {
    std::string s;
    for (char c : name) s += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(c)) : '_';
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) s = "p" + s;
    s += "_" + std::to_string(index);
    symbols_.resize(std::max<size_t>(symbols_.size(), index + 1));
    symbols_[index] = s;
    return symbols_[index];
  }

  Build build_;
  TonicPluginInfo info_;
  tonic::engine::Layout layout_;
  tonic::engine::Engine engine_;
  // Bindings while inactive (from the layout, saved state, or the last session). Atomic: the host thread
  // (setState) and the UI thread (ccMapping) may touch it concurrently.
  std::array<std::atomic<int>, 128> ccCache_;
  std::atomic<bool> active_{false};
  std::vector<float> values_;
  std::vector<tonic::engine::Event> events_;
  std::vector<std::string> symbols_;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonicDpfPlugin)
};

END_NAMESPACE_DISTRHO
