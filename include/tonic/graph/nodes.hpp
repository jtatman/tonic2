// Basic nodes wrapping the DSP kernels. Fixed settings are given at construction; nodes that take
// a parameter id read the engine's per-sample (smoothed, macro-applied) values instead.
#pragma once

#include <cmath>
#include <vector>

#include "../dsp/envelope.hpp"
#include "../dsp/filters.hpp"
#include "../dsp/oscillator.hpp"
#include "node.hpp"

namespace tonic::graph {

// Where an oscillator gets its frequency: a fixed value, the played note (times a ratio),
// or a parameter in Hz. Falls back to the fixed value if the source is unavailable.
struct FreqSource {
  enum class Kind { Fixed, NotePitch, Param } kind = Kind::Fixed;
  double hz = 440.0;
  double ratio = 1.0;
  int param = -1;
  static FreqSource fixed(double hz) { return {Kind::Fixed, hz, 1.0, -1}; }
  static FreqSource note(double ratio = 1.0, double fallbackHz = 440.0) { return {Kind::NotePitch, fallbackHz, ratio, -1}; }
  static FreqSource fromParam(int id, double fallbackHz = 440.0) { return {Kind::Param, fallbackHz, 1.0, id}; }
};

class OscillatorNode final : public Node {
 public:
  OscillatorNode(dsp::Waveform w, double hz) : OscillatorNode(w, FreqSource::fixed(hz)) {}
  OscillatorNode(dsp::Waveform w, FreqSource src) : Node({}, {1}), wave_(w), src_(src) {}
  void prepare(double sr, int) override {
    osc_.prepare(sr); osc_.setWaveform(wave_); osc_.setFrequency(src_.hz); osc_.reset();
  }
  void process(const ProcessArgs& a) override {
    const float* f = nullptr;
    if (src_.kind == FreqSource::Kind::NotePitch && a.control) f = a.control->pitchHz;
    if (src_.kind == FreqSource::Kind::Param) f = a.param(src_.param);
    float* out = a.output(0, 0);
    if (!f) { osc_.setFrequency(src_.hz); osc_.process(out, a.numFrames); return; }
    for (int i = 0; i < a.numFrames; ++i) {
      osc_.setFrequency(f[i] * src_.ratio);
      out[i] = osc_.process();
    }
  }

 private:
  dsp::Oscillator osc_;
  dsp::Waveform wave_;
  FreqSource src_;
};

// ADSR driven by the engine's gate and trigger (retriggers on every new note, including legato).
class EnvelopeNode final : public Node {
 public:
  EnvelopeNode(double attack, double decay, double sustain, double release)
      : Node({}, {1}), a_(attack), d_(decay), s_(sustain), r_(release) {}
  void prepare(double sr, int) override { env_.prepare(sr); env_.set(a_, d_, s_, r_); env_.reset(); }
  void process(const ProcessArgs& a) override {
    const float* gate = a.control ? a.control->gate : nullptr;
    const float* trig = a.control ? a.control->trigger : nullptr;
    float* out = a.output(0, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      if (trig && trig[i] > 0.f) env_.noteOn();
      else if (gate && gate[i] <= 0.f && wasOpen_) env_.noteOff();
      wasOpen_ = gate && gate[i] > 0.f;
      out[i] = env_.process();
    }
  }

 private:
  dsp::Adsr env_;
  double a_, d_, s_, r_;
  bool wasOpen_ = false;
};

// Multiplies an N-channel signal (port 0) by a mono control signal (port 1), e.g. an envelope.
class VcaNode final : public Node {
 public:
  explicit VcaNode(int channels) : Node({channels, 1}, {channels}) {}
  void process(const ProcessArgs& a) override {
    const float* cv = a.input(1, 0);
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float* in = a.input(0, c);
      float* out = a.output(0, c);
      for (int i = 0; i < a.numFrames; ++i) out[i] = in[i] * cv[i];
    }
  }
};

class GainNode final : public Node {
 public:
  // gainParam: optional parameter id (linear gain) read per sample instead of the fixed gain.
  GainNode(int channels, float gain, int gainParam = -1) : Node({channels}, {channels}), gain_(gain), param_(gainParam) {}
  void process(const ProcessArgs& a) override {
    const float* g = a.param(param_);
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float* in = a.input(0, c);
      float* out = a.output(0, c);
      if (g) for (int i = 0; i < a.numFrames; ++i) out[i] = in[i] * g[i];
      else for (int i = 0; i < a.numFrames; ++i) out[i] = in[i] * gain_;
    }
  }

 private:
  float gain_;
  int param_;
};

// Equal-power pan of a mono input to stereo. pan in [-1, 1].
class PanNode final : public Node {
 public:
  explicit PanNode(float pan) : Node({1}, {2}) {
    const double a = (pan + 1.0) * 0.25 * dsp::kPi;
    l_ = static_cast<float>(std::cos(a));
    r_ = static_cast<float>(std::sin(a));
  }
  void process(const ProcessArgs& a) override {
    const float* in = a.input(0, 0);
    float *l = a.output(0, 0), *r = a.output(0, 1);
    for (int i = 0; i < a.numFrames; ++i) { l[i] = in[i] * l_; r[i] = in[i] * r_; }
  }

 private:
  float l_ = 0.70710678f, r_ = 0.70710678f;
};

// N-channel state-variable low-pass (one kernel per channel).
class SvfLowpassNode final : public Node {
 public:
  SvfLowpassNode(int channels, double cutoff, double q)
      : Node({channels}, {channels}), cutoff_(cutoff), q_(q), filters_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : filters_) { f.prepare(sr); f.setCutoff(cutoff_); f.setQ(q_); f.reset(); }
  }
  void process(const ProcessArgs& a) override {
    for (size_t c = 0; c < filters_.size(); ++c) {
      const float* in = a.input(0, static_cast<int>(c));
      float* out = a.output(0, static_cast<int>(c));
      for (int i = 0; i < a.numFrames; ++i) out[i] = filters_[c].processLow(in[i]);
    }
  }

 private:
  double cutoff_, q_;
  std::vector<dsp::Svf> filters_;
};

// N-channel ladder low-pass.
class LadderNode final : public Node {
 public:
  // cutoffParam / resonanceParam: optional parameter ids (Hz, 0..1) read per sample.
  LadderNode(int channels, double cutoff, double resonance, double drive, int cutoffParam = -1,
             int resonanceParam = -1)
      : Node({channels}, {channels}), cutoff_(cutoff), res_(resonance), drive_(drive),
        cutoffParam_(cutoffParam), resParam_(resonanceParam), filters_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : filters_) { f.prepare(sr); f.setCutoff(cutoff_); f.setResonance(res_); f.setDrive(drive_); f.reset(); }
    lastCutoff_ = static_cast<float>(cutoff_);
    lastRes_ = static_cast<float>(res_);
  }
  void process(const ProcessArgs& a) override {
    const float* fc = a.param(cutoffParam_);
    const float* rs = a.param(resParam_);
    for (int i = 0; i < a.numFrames; ++i) {
      // Recompute coefficients only when the value moves (tan() is the costly part).
      if (fc && fc[i] != lastCutoff_) { lastCutoff_ = fc[i]; for (auto& f : filters_) f.setCutoff(lastCutoff_); }
      if (rs && rs[i] != lastRes_) { lastRes_ = rs[i]; for (auto& f : filters_) f.setResonance(lastRes_); }
      for (size_t c = 0; c < filters_.size(); ++c)
        a.output(0, static_cast<int>(c))[i] = filters_[c].process(a.input(0, static_cast<int>(c))[i]);
    }
  }

 private:
  double cutoff_, res_, drive_;
  int cutoffParam_, resParam_;
  float lastCutoff_ = 0.f, lastRes_ = 0.f;
  std::vector<dsp::Ladder> filters_;
};

}  // namespace tonic::graph
