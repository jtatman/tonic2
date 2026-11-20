// Signal nodes for the ported Tonic 1 units: noise colours, clocks and sequencing, modulated delay,
// bitcrusher, reverb and compressor. As in signal_nodes.hpp every setting is a mono input port, and
// coefficients are only recomputed when a setting's value changes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "../dsp/delay.hpp"
#include "../dsp/dynamics.hpp"
#include "../dsp/noise.hpp"
#include "../dsp/reverb.hpp"
#include "../dsp/sequencing.hpp"
#include "node.hpp"

namespace tonic::graph {

class PinkNoiseNode final : public Node {
 public:
  explicit PinkNoiseNode(uint32_t seed) : Node({}, {1}), seed_(seed) {}
  void prepare(double, int) override { pink_.seed(seed_); }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = pink_.process();
  }
 private:
  dsp::PinkNoise pink_;
  uint32_t seed_;
};

// Input 0 = rate (Hz). Output -1..1, stepped or ramped.
class LfNoiseNode final : public Node {
 public:
  LfNoiseNode(uint32_t seed, bool smooth) : Node({1}, {1}), seed_(seed), smooth_(smooth) {}
  void prepare(double sr, int) override { lf_.prepare(sr); lf_.seed(seed_); lf_.setSmooth(smooth_); }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = lf_.process(a.input(0, 0)[i]);
  }
 private:
  dsp::LfNoise lf_;
  uint32_t seed_;
  bool smooth_;
};

// Input 0 = tempo (bpm). Output: trigger (1 for one sample) every 1/division beat.
class MetroNode final : public Node {
 public:
  explicit MetroNode(double division) : Node({1}, {1}), division_(division) {}
  void prepare(double sr, int) override { metro_.prepare(sr); metro_.reset(); }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = metro_.process(a.input(0, 0)[i], division_);
  }
 private:
  dsp::Metro metro_;
  double division_;
};

// Input 0 = trigger. Output: current step value (advances on each trigger).
class StepperNode final : public Node {
 public:
  explicit StepperNode(std::vector<float> values) : Node({1}, {1}), values_(std::move(values)) {}
  void prepare(double, int) override {
    stepper_.setSteps(values_.data(), static_cast<int>(values_.size()));  // up to 64 steps
    stepper_.reset();
  }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = stepper_.process(a.input(0, 0)[i]);
  }
 private:
  dsp::Stepper stepper_;
  std::vector<float> values_;
};

// Inputs: 0 = trigger, 1 = min, 2 = max. Output: random value held until the next trigger.
class SampleHoldNode final : public Node {
 public:
  explicit SampleHoldNode(uint32_t seed) : Node({1, 1, 1}, {1}), seed_(seed) {}
  void prepare(double, int) override { sh_.seed(seed_); }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = sh_.process(a.input(0, 0)[i], a.input(1, 0)[i], a.input(2, 0)[i]);
  }
 private:
  dsp::SampleHold sh_;
  uint32_t seed_;
};

// Stereo feedback delay with modulatable time. Inputs: 0 = signal (stereo; mono is broadcast),
// 1 = time (s), 2 = feedback 0..0.99, 3 = mix (wet amount 0..1). Right time = time * rightRatio.
class ModDelayNode final : public Node {
 public:
  ModDelayNode(double maxSeconds, bool pingPong, float rightRatio)
      : Node({2, 1, 1, 1}, {2}), max_(maxSeconds), pingPong_(pingPong), ratio_(rightRatio) {}
  void prepare(double sr, int) override { d_.prepare(sr, max_ * std::max(1.f, ratio_)); d_.setPingPong(pingPong_); }
  void process(const ProcessArgs& a) override {
    const float *inL = a.input(0, 0), *inR = a.input(0, 1), *t = a.input(1, 0), *fb = a.input(2, 0), *mix = a.input(3, 0);
    float *oL = a.output(0, 0), *oR = a.output(0, 1);
    for (int i = 0; i < a.numFrames; ++i) {
      float wl, wr;
      d_.process(inL[i], inR[i], t[i], t[i] * ratio_, fb[i], wl, wr);
      const float m = std::clamp(mix[i], 0.f, 1.f);
      oL[i] = inL[i] * (1.f - m) + wl * m;
      oR[i] = inR[i] * (1.f - m) + wr * m;
    }
  }
 private:
  dsp::StereoDelay d_;
  double max_;
  bool pingPong_;
  float ratio_;
};

// Inputs: 0 = signal (N ch), 1 = bits, 2 = rate (Hz).
class CrushNode final : public Node {
 public:
  explicit CrushNode(int channels) : Node({channels, 1, 1}, {channels}), c_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override { for (auto& c : c_) c.prepare(sr); }
  void process(const ProcessArgs& a) override {
    for (size_t ch = 0; ch < c_.size(); ++ch)
      for (int i = 0; i < a.numFrames; ++i)
        a.output(0, static_cast<int>(ch))[i] = c_[ch].process(a.input(0, static_cast<int>(ch))[i], a.input(1, 0)[i], a.input(2, 0)[i]);
  }
 private:
  std::vector<dsp::BitCrusher> c_;
};

// Stereo reverb. Inputs: 0 = signal (stereo; mono is broadcast), 1 = size 0..1, 2 = decay (s),
// 3 = damping (Hz), 4 = mix (wet amount 0..1).
class ReverbNode final : public Node {
 public:
  ReverbNode() : Node({2, 1, 1, 1, 1}, {2}) {}
  void prepare(double sr, int) override { r_.prepare(sr); last_[0] = last_[1] = last_[2] = -1.f; }
  void process(const ProcessArgs& a) override {
    const float *inL = a.input(0, 0), *inR = a.input(0, 1), *mix = a.input(4, 0);
    float *oL = a.output(0, 0), *oR = a.output(0, 1);
    for (int i = 0; i < a.numFrames; ++i) {
      update(a.input(1, 0)[i], a.input(2, 0)[i], a.input(3, 0)[i]);
      float wl, wr;
      r_.process(inL[i], inR[i], wl, wr);
      const float m = std::clamp(mix[i], 0.f, 1.f);
      oL[i] = inL[i] * (1.f - m) + wl * m;
      oR[i] = inR[i] * (1.f - m) + wr * m;
    }
  }
 private:
  void update(float size, float decay, float damping) {
    if (size != last_[0]) { last_[0] = size; r_.setSize(size); }
    if (decay != last_[1]) { last_[1] = decay; r_.setDecay(decay); }
    if (damping != last_[2]) { last_[2] = damping; r_.setDamping(damping); }
  }
  dsp::Reverb r_;
  float last_[3] = {-1.f, -1.f, -1.f};
};

// Compressor / limiter, linked across channels. Inputs: 0 = signal (N ch), 1 = sidechain (mono; used
// only when constructed with sidechain = true), 2 = threshold (dB), 3 = ratio, 4 = attack (s),
// 5 = release (s), 6 = makeup (dB).
class CompressorNode final : public Node {
 public:
  CompressorNode(int channels, bool sidechain) : Node({channels, 1, 1, 1, 1, 1, 1}, {channels}), sidechain_(sidechain) {}
  void prepare(double sr, int) override { c_.prepare(sr); std::fill(std::begin(last_), std::end(last_), -1e9f); }
  void process(const ProcessArgs& a) override {
    const int ch = outputChannels()[0];
    for (int i = 0; i < a.numFrames; ++i) {
      update(a.input(2, 0)[i], a.input(3, 0)[i], a.input(4, 0)[i], a.input(5, 0)[i], a.input(6, 0)[i]);
      float det = 0.f;
      if (sidechain_) det = std::fabs(a.input(1, 0)[i]);
      else for (int c = 0; c < ch; ++c) det = std::max(det, std::fabs(a.input(0, c)[i]));
      const float g = c_.gainFor(det);
      for (int c = 0; c < ch; ++c) a.output(0, c)[i] = a.input(0, c)[i] * g;
    }
  }
 private:
  void update(float th, float ratio, float att, float rel, float makeup) {
    if (th != last_[0]) { last_[0] = th; c_.setThreshold(th); }
    if (ratio != last_[1]) { last_[1] = ratio; c_.setRatio(ratio); }
    if (att != last_[2]) { last_[2] = att; c_.setAttack(att); }
    if (rel != last_[3]) { last_[3] = rel; c_.setRelease(rel); }
    if (makeup != last_[4]) { last_[4] = makeup; c_.setMakeup(makeup); }
  }
  dsp::Compressor c_;
  bool sidechain_;
  float last_[5];
};

}  // namespace tonic::graph
