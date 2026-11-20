// Clocks, step sequencing and random modulation (ports of Tonic 1's ControlMetro, ControlStepper,
// ControlRandom and LFNoise), as per-sample kernels. Triggers are 1 on a single sample, else 0.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "noise.hpp"

namespace tonic::dsp {

// Tempo clock: a trigger every 1/division of a beat (division 4 = sixteenth notes at 4/4).
struct Metro {
  void prepare(double sampleRate) { sampleRate_ = sampleRate; }
  void reset() { phase_ = 1.0; }  // fires on the first sample
  // Returns 1 on the sample a tick happens.
  float process(double bpm, double division = 1.0) {
    const double inc = std::max(0.0, bpm) / 60.0 * std::max(division, 1e-6) / sampleRate_;
    // Tolerance: summing e.g. 1/6000 six thousand times lands just below 1.0 and would tick a sample late.
    if (phase_ >= 1.0 - 1e-9) { phase_ -= std::floor(phase_ + 1e-9); phase_ += inc; return 1.f; }
    phase_ += inc;
    return 0.f;
  }

 private:
  double sampleRate_ = 48000.0, phase_ = 1.0;
};

// Step sequencer: on each trigger, advance to the next of up to 64 values (wrapping).
struct Stepper {
  static constexpr int kMaxSteps = 64;
  void setSteps(const float* values, int n) {
    count_ = 0;
    for (int i = 0; i < n && count_ < kMaxSteps; ++i) steps_[static_cast<size_t>(count_++)] = values[i];
    if (count_ == 0) { steps_[0] = 0.f; count_ = 1; }
  }
  void setSteps(std::initializer_list<float> values) { setSteps(values.begin(), static_cast<int>(values.size())); }
  void reset() { index_ = -1; }
  int count() const { return count_; }
  // The current step value; a trigger advances first.
  float process(float trigger) {
    if (trigger > 0.f) index_ = (index_ + 1) % count_;
    return steps_[static_cast<size_t>(std::max(index_, 0))];
  }

 private:
  std::array<float, kMaxSteps> steps_{};
  int count_ = 1, index_ = -1;
};

// Sample and hold: a new uniform random value in [min, max] on each trigger (deterministic per seed).
struct SampleHold {
  explicit SampleHold(uint32_t seed = 0x51ED27A3u) : noise_(seed) {}
  void seed(uint32_t s) { noise_.seed(s); held_ = 0.f; }
  float process(float trigger, float min, float max) {
    if (trigger > 0.f) held_ = 0.5f * (noise_.process() + 1.f);  // 0..1
    return min + held_ * (max - min);
  }

 private:
  WhiteNoise noise_;
  float held_ = 0.f;
};

// Low-frequency noise: a new random target `rate` times per second, either held (stepped) or reached by a
// straight ramp (smooth). Output -1..1.
struct LfNoise {
  explicit LfNoise(uint32_t seed = 0x3C6EF372u) : noise_(seed) {}
  void prepare(double sampleRate) { sampleRate_ = sampleRate; }
  void seed(uint32_t s) { noise_.seed(s); phase_ = 1.0; from_ = to_ = 0.f; }
  void setSmooth(bool smooth) { smooth_ = smooth; }
  float process(double rateHz) {
    phase_ += std::max(0.0, rateHz) / sampleRate_;
    if (phase_ >= 1.0) {
      phase_ -= std::floor(phase_);
      from_ = to_;
      to_ = noise_.process();
    }
    return smooth_ ? from_ + static_cast<float>(phase_) * (to_ - from_) : to_;
  }

 private:
  WhiteNoise noise_;
  double sampleRate_ = 48000.0, phase_ = 1.0;
  float from_ = 0.f, to_ = 0.f;
  bool smooth_ = true;
};

}  // namespace tonic::dsp
