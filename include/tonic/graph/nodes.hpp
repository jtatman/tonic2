// Basic nodes wrapping the DSP kernels. Settings are fixed at construction for M2; runtime
// parameters (sample-accurate, smoothed) arrive with the parameter system in M3.
#pragma once

#include <cmath>
#include <vector>

#include "../dsp/filters.hpp"
#include "../dsp/oscillator.hpp"
#include "node.hpp"

namespace tonic::graph {

class OscillatorNode final : public Node {
 public:
  OscillatorNode(dsp::Waveform w, double hz) : Node({}, {1}), wave_(w), hz_(hz) {}
  void prepare(double sr, int) override {
    osc_.prepare(sr); osc_.setWaveform(wave_); osc_.setFrequency(hz_); osc_.reset();
  }
  void process(const ProcessArgs& a) override { osc_.process(a.output(0, 0), a.numFrames); }

 private:
  dsp::Oscillator osc_;
  dsp::Waveform wave_;
  double hz_;
};

class GainNode final : public Node {
 public:
  GainNode(int channels, float gain) : Node({channels}, {channels}), gain_(gain) {}
  void process(const ProcessArgs& a) override {
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float* in = a.input(0, c);
      float* out = a.output(0, c);
      for (int i = 0; i < a.numFrames; ++i) out[i] = in[i] * gain_;
    }
  }

 private:
  float gain_;
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
  LadderNode(int channels, double cutoff, double resonance, double drive)
      : Node({channels}, {channels}), cutoff_(cutoff), res_(resonance), drive_(drive),
        filters_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : filters_) { f.prepare(sr); f.setCutoff(cutoff_); f.setResonance(res_); f.setDrive(drive_); f.reset(); }
  }
  void process(const ProcessArgs& a) override {
    for (size_t c = 0; c < filters_.size(); ++c) {
      const float* in = a.input(0, static_cast<int>(c));
      float* out = a.output(0, static_cast<int>(c));
      for (int i = 0; i < a.numFrames; ++i) out[i] = filters_[c].process(in[i]);
    }
  }

 private:
  double cutoff_, res_, drive_;
  std::vector<dsp::Ladder> filters_;
};

}  // namespace tonic::graph
