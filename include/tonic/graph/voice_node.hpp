// Voice node: runs N copies of a voice graph and sums them. Allocates notes to voices from the block's
// events, sample-accurately (each voice gets its own per-sample gate, trigger, pitch, velocity,
// pressure and timbre buffers, exactly like the engine's mono buffers).
//
// Modes: Poly (one voice per note, `unison` voices per note when > 1), Mono (last-note priority,
// retrigger on every note), Legato (last-note priority, no retrigger while notes overlap).
// Routing: a node can listen to one MIDI channel and/or a key range (VoiceConfig::channel, lowKey,
// highKey), so several voice nodes in one patch play different parts.
// Stealing when all voices are busy: the oldest released voice first, then the oldest held voice.
// A released voice is freed once its output stays below -100 dBFS for a whole block.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../dsp/math.hpp"
#include "graph.hpp"

namespace tonic::graph {

enum class VoiceMode { Poly, Mono, Legato };

struct VoiceConfig {
  int voices = 8;
  VoiceMode mode = VoiceMode::Poly;
  int unison = 1;            // voices per note (Poly) or voices playing the mono note (Mono/Legato)
  float detuneCents = 0.f;   // unison spread: voices spaced evenly from -detune to +detune
  int channel = -1;          // only notes on this MIDI channel (0..15); -1 = any. Notes without a channel always pass
  int lowKey = 0, highKey = 127;  // key range (keyboard split)
  float glideSeconds = 0.f;  // portamento time constant (0 = off); the note pitch glides, bends do not
  float bendRange = 2.f;     // pitch wheel range in semitones
};

class VoiceNode final : public Node {
 public:
  using Factory = std::function<std::unique_ptr<CompiledGraph>(double sampleRate, int maxBlock)>;

  VoiceNode(VoiceConfig cfg, int channels, Factory factory)
      : Node({}, {channels}), cfg_(sanitize(cfg)), factory_(std::move(factory)) {}

  void prepare(double sampleRate, int maxBlock) override {
    voices_.clear();
    voices_.resize(static_cast<size_t>(cfg_.voices));
    const int ch = outputChannels()[0];
    for (Voice& v : voices_) {
      v.graph = factory_(sampleRate, maxBlock);
      for (auto* b : {&v.gate, &v.trigger, &v.pitch, &v.velocity, &v.pressure, &v.timbre})
        b->assign(static_cast<size_t>(maxBlock), 0.f);
      v.out.assign(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(maxBlock), 0.f));
      v.outPtrs.clear();
      for (auto& o : v.out) v.outPtrs.push_back(o.data());
      v.control.gate = v.gate.data();
      v.control.trigger = v.trigger.data();
      v.control.pitchHz = v.pitch.data();
      v.control.velocity = v.velocity.data();
      v.control.pressure = v.pressure.data();
      v.control.timbre = v.timbre.data();
    }
    noteCount_ = 0;
    clock_ = 0;
    wheel_ = 0.f;
    glideCoef_ = cfg_.glideSeconds > 0.f ? 1.0 - std::exp(-1.0 / (cfg_.glideSeconds * sampleRate)) : 1.0;
  }

  void process(const ProcessArgs& a) override {
    const int n = a.numFrames, ch = outputChannels()[0];
    const ControlBuffers* parent = a.control;
    int e = 0;
    for (int i = 0; i < n; ++i) {
      if (parent)
        while (e < parent->numEvents && parent->events[e].frame - parent->eventOffset <= i) handle(parent->events[e++]);
      for (Voice& v : voices_) {
        v.gate[i] = v.held ? 1.f : 0.f;
        v.trigger[i] = v.pendingTrigger ? 1.f : 0.f;
        v.pendingTrigger = false;
        if (v.semi != v.note) {  // glide toward the note
          const double target = v.note;
          v.semi += (target - v.semi) * glideCoef_;
          if (std::fabs(target - v.semi) < 1e-6) v.semi = target;
          retune(v);
        }
        v.pitch[i] = v.hz;
        v.velocity[i] = v.vel;
        v.pressure[i] = v.press;
        v.timbre[i] = v.timb;
      }
    }
    for (int c = 0; c < ch; ++c) std::fill(a.output(0, c), a.output(0, c) + n, 0.f);
    for (Voice& v : voices_) {
      if (!v.active || !v.graph) continue;
      v.control.params = parent ? parent->params : nullptr;
      v.control.numParams = parent ? parent->numParams : 0;
      v.graph->process(n, nullptr, 0, 0, v.outPtrs.data(), ch, 0, &v.control);
      float peak = 0.f;
      for (int c = 0; c < ch; ++c) {
        const float* src = v.out[static_cast<size_t>(c)].data();
        float* dst = a.output(0, c);
        for (int i = 0; i < n; ++i) { dst[i] += src[i]; peak = std::max(peak, std::fabs(src[i])); }
      }
      if (!v.held && peak < 1e-5f) v.active = false;  // release finished
    }
  }

  // For tests and meters (call from the audio thread or while stopped).
  int activeVoices() const {
    return static_cast<int>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
  }

 private:
  struct Voice {
    std::unique_ptr<CompiledGraph> graph;
    std::vector<float> gate, trigger, pitch, velocity, pressure, timbre;
    std::vector<std::vector<float>> out;
    std::vector<float*> outPtrs;
    ControlBuffers control;
    int note = 60;
    float vel = 0.f, bend = 0.f, press = 0.f, timb = 0.f, detune = 1.f, hz = 261.6256f;
    double semi = 60.0;  // gliding note pitch in semitones (double: float steps stall short of the target)
    bool held = false, active = false, pendingTrigger = false;
    uint64_t age = 0;  // allocation time (held) or release time (released)
  };

  static VoiceConfig sanitize(VoiceConfig c) {
    c.voices = std::max(1, c.voices);
    c.unison = std::clamp(c.unison, 1, c.voices);
    return c;
  }

  void retune(Voice& v) const { v.hz = dsp::midiToHz(static_cast<float>(v.semi + v.bend + wheel_ * cfg_.bendRange)) * v.detune; }

  float detuneFor(int u) const {
    if (cfg_.unison <= 1) return 1.f;
    const float pos = 2.f * static_cast<float>(u) / static_cast<float>(cfg_.unison - 1) - 1.f;  // -1..1
    return std::exp2(pos * cfg_.detuneCents / 1200.f);
  }

  void start(Voice& v, int note, float vel, int u, bool retrigger) {
    const bool fresh = !v.active;  // a voice coming from silence jumps to its note instead of gliding
    v.note = note;
    v.vel = vel;
    v.bend = 0.f;
    v.press = 0.f;
    v.timb = 0.f;
    v.detune = detuneFor(u);
    if (fresh || glideCoef_ >= 1.0) v.semi = note;
    retune(v);
    v.held = true;
    v.active = true;
    if (retrigger) v.pendingTrigger = true;
    v.age = ++clock_;
  }

  Voice& pickVoice() {
    for (Voice& v : voices_) if (!v.active) return v;
    Voice* best = nullptr;
    for (Voice& v : voices_)  // oldest released
      if (!v.held && (!best || v.age < best->age)) best = &v;
    if (best) return *best;
    for (Voice& v : voices_)  // oldest held
      if (!best || v.age < best->age) best = &v;
    return *best;
  }

  void handle(const engine::Event& ev) {
    using T = engine::Event::Type;
    if (cfg_.channel >= 0 && ev.channel >= 0 && ev.channel != cfg_.channel) return;  // routing
    if (ev.type == engine::Event::Type::PitchWheel) {
      wheel_ = std::clamp(ev.value, -1.f, 1.f);
      for (Voice& v : voices_) retune(v);
      return;
    }
    if (ev.key < cfg_.lowKey || ev.key > cfg_.highKey) return;
    if (ev.type == T::NoteOn && ev.value <= 0.f) return noteOff(ev.key);
    if (ev.type == T::NoteOn) return noteOn(ev.key, ev.value);
    if (ev.type == T::NoteOff) return noteOff(ev.key);
    if (ev.type != T::Expression) return;
    for (Voice& v : voices_) {
      if (!v.active || (cfg_.mode == VoiceMode::Poly && v.note != ev.key)) continue;
      switch (ev.expr) {
        case engine::Event::Expr::PitchBend: v.bend = ev.value; retune(v); break;
        case engine::Event::Expr::Pressure: v.press = ev.value; break;
        case engine::Event::Expr::Timbre: v.timb = ev.value; break;
      }
    }
  }

  void noteOn(int note, float vel) {
    if (cfg_.mode == VoiceMode::Poly) {
      for (int u = 0; u < cfg_.unison; ++u) start(pickVoice(), note, vel, u, true);
      return;
    }
    const bool overlapping = noteCount_ > 0;
    removeNote(note);
    if (noteCount_ == kMaxNotes) { std::copy(notes_ + 1, notes_ + kMaxNotes, notes_); --noteCount_; }
    notes_[noteCount_++] = note;
    const bool retrigger = cfg_.mode == VoiceMode::Mono || !overlapping;
    for (int u = 0; u < cfg_.unison; ++u) start(voices_[static_cast<size_t>(u)], note, vel, u, retrigger);
  }

  void noteOff(int note) {
    if (cfg_.mode == VoiceMode::Poly) {
      for (Voice& v : voices_)
        if (v.held && v.note == note) { v.held = false; v.age = ++clock_; }
      return;
    }
    removeNote(note);
    for (int u = 0; u < cfg_.unison; ++u) {
      Voice& v = voices_[static_cast<size_t>(u)];
      if (noteCount_ > 0) { v.note = notes_[noteCount_ - 1]; retune(v); }  // fall back to the previous held note
      else if (v.held) { v.held = false; v.age = ++clock_; }
    }
  }

  void removeNote(int note) {
    auto* end = std::remove(notes_, notes_ + noteCount_, note);
    noteCount_ = static_cast<int>(end - notes_);
  }

  static constexpr int kMaxNotes = 16;
  VoiceConfig cfg_;
  Factory factory_;
  std::vector<Voice> voices_;
  int notes_[kMaxNotes] = {};
  int noteCount_ = 0;
  uint64_t clock_ = 0;
  float wheel_ = 0.f;
  double glideCoef_ = 1.0;
};

}  // namespace tonic::graph
