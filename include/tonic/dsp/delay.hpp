// Delay lines with fractional, modulatable read positions (cubic Hermite interpolation), and a stereo
// feedback delay with optional ping-pong (port of Tonic 1's StereoDelay, with smooth time modulation).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "math.hpp"

namespace tonic::dsp {

struct DelayLine {
  // Allocates room for maxSeconds of delay (control thread).
  void prepare(double sampleRate, double maxSeconds) {
    sampleRate_ = sampleRate;
    size_t n = 4;
    while (n < static_cast<size_t>(maxSeconds * sampleRate) + 4) n <<= 1;
    buf_.assign(n, 0.f);
    mask_ = n - 1;
    write_ = 0;
  }
  void reset() { std::fill(buf_.begin(), buf_.end(), 0.f); write_ = 0; }
  double sampleRate() const { return sampleRate_; }
  double maxDelaySamples() const { return static_cast<double>(buf_.size()) - 4.0; }
  void write(float x) { buf_[write_] = x; write_ = (write_ + 1) & mask_; }
  // Value `delay` samples ago (>= 1; fractional allowed), read before the next write().
  float read(double delay) const {
    delay = std::clamp(delay, 1.0, maxDelaySamples());
    const double pos = static_cast<double>(write_) - delay;
    const double fl = std::floor(pos);
    const float t = static_cast<float>(pos - fl);
    const size_t i = static_cast<size_t>(static_cast<long long>(fl)) & mask_;
    const float xm1 = buf_[(i - 1) & mask_], x0 = buf_[i], x1 = buf_[(i + 1) & mask_], x2 = buf_[(i + 2) & mask_];
    // Cubic Hermite (Catmull-Rom).
    const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }

 private:
  std::vector<float> buf_;
  size_t mask_ = 0, write_ = 0;
  double sampleRate_ = 48000.0;
};

// Stereo feedback delay. Times in seconds may change every sample (smooth, no zipper). With ping-pong the
// feedback crosses channels, so echoes alternate left and right.
struct StereoDelay {
  void prepare(double sampleRate, double maxSeconds) { l_.prepare(sampleRate, maxSeconds); r_.prepare(sampleRate, maxSeconds); }
  void reset() { l_.reset(); r_.reset(); }
  void setPingPong(bool on) { pingPong_ = on; }
  // Returns the wet signal; feedback 0..0.99.
  void process(float inL, float inR, double timeL, double timeR, float feedback, float& wetL, float& wetR) {
    const double sr = l_.sampleRate();
    wetL = l_.read(timeL * sr);
    wetR = r_.read(timeR * sr);
    const float fb = std::clamp(feedback, 0.f, 0.99f);
    if (pingPong_) {
      l_.write(0.5f * (inL + inR) + fb * wetR);
      r_.write(fb * wetL);
    } else {
      l_.write(inL + fb * wetL);
      r_.write(inR + fb * wetR);
    }
  }

 private:
  DelayLine l_, r_;
  bool pingPong_ = false;
};

// Bit depth and sample-rate reduction (port of Tonic 1's BitCrusher, plus rate reduction).
struct BitCrusher {
  void prepare(double sampleRate) { sampleRate_ = sampleRate; phase_ = 1.0; held_ = 0.f; }
  // bits 1..24 (fractional allowed), rateHz = sample-and-hold rate (>= sample rate means off).
  float process(float x, float bits, double rateHz) {
    phase_ += std::max(1.0, rateHz) / sampleRate_;
    if (phase_ >= 1.0) {
      phase_ -= std::floor(phase_);
      const float levels = std::exp2(std::clamp(bits, 1.f, 24.f) - 1.f);
      held_ = std::round(x * levels) / levels;
    }
    return held_;
  }

 private:
  double sampleRate_ = 48000.0, phase_ = 1.0;
  float held_ = 0.f;
};

}  // namespace tonic::dsp
