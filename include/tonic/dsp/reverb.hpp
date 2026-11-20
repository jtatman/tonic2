// Feedback delay network reverb (wet output only).
// Eight delay lines of mutually prime-ish length are fed back through an 8x8 Hadamard matrix scaled
// by 1/sqrt(8) (energy preserving). Each line has a gain g = 10^(-3 * len / rt60) so the tail decays
// 60 dB in rt60 seconds, and a one-pole lowpass in the feedback path for high-frequency damping.
// Stereo input is injected into alternating lines with alternating signs; the two outputs sum
// disjoint line groups (0-3 left, 4-7 right) so they are decorrelated.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "math.hpp"

namespace tonic::dsp {

struct Reverb {
  static constexpr int kLines = 8;

  void prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    size_t need = 1;
    while (need < static_cast<size_t>(kMaxScale * kBaseMs[kLines - 1] * 0.001 * sampleRate) + 2) need <<= 1;
    for (auto& b : buf_) b.assign(need, 0.0f);
    mask_ = need - 1;
    updateLengths();
    updateDamping();
    reset();
  }
  void reset() {
    for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
    std::fill(lp_, lp_ + kLines, 0.0f);
    pos_ = 0;
  }
  void setSize(float size01) {
    size_ = std::clamp(size01, 0.0f, 1.0f);
    if (!buf_[0].empty()) updateLengths();
  }
  void setDecay(float seconds) {
    rt60_ = std::clamp(seconds, 0.1f, 20.0f);
    if (!buf_[0].empty()) updateLengths();
  }
  void setDamping(float hz) {
    dampHz_ = hz;
    if (!buf_[0].empty()) updateDamping();
  }

  void process(float inL, float inR, float& outL, float& outR) {
    float y[kLines];
    for (int i = 0; i < kLines; ++i) {
      const float d = buf_[i][(pos_ + buf_[i].size() - len_[i]) & mask_];
      lp_[i] = (1.0f - damp_) * (d * gain_[i]) + damp_ * lp_[i];
      y[i] = lp_[i];
    }
    float oL = 0.0f, oR = 0.0f;
    for (int i = 0; i < 4; ++i) {
      const float s = (i & 1) ? -1.0f : 1.0f;
      oL += s * y[i];
      oR += s * y[i + 4];
    }
    outL = oL * 0.5f;
    outR = oR * 0.5f;

    // In-place fast Walsh-Hadamard transform, scaled by 1/sqrt(8).
    for (int h = 1; h < kLines; h <<= 1)
      for (int i = 0; i < kLines; i += h << 1)
        for (int j = i; j < i + h; ++j) {
          const float a = y[j], b = y[j + h];
          y[j] = a + b;
          y[j + h] = a - b;
        }
    constexpr float kNorm = 0.35355339059327373f;
    for (int i = 0; i < kLines; ++i) {
      const float s = (i & 1) ? -1.0f : 1.0f;
      const float in = s * ((i & 1) ? inR : inL);
      buf_[i][pos_] = y[i] * kNorm + in;
    }
    pos_ = (pos_ + 1) & mask_;
  }

 private:
  static constexpr double kBaseMs[kLines] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.1, 73.3};
  static constexpr double kMaxScale = 1.5;

  void updateLengths() {
    const double scale = 0.5 + size_;
    for (int i = 0; i < kLines; ++i) {
      len_[i] = std::max<size_t>(1, static_cast<size_t>(std::lround(kBaseMs[i] * 0.001 * scale * sampleRate_)));
      gain_[i] = static_cast<float>(std::pow(10.0, -3.0 * (static_cast<double>(len_[i]) / sampleRate_) / rt60_));
    }
  }
  void updateDamping() {
    const double hz = std::clamp<double>(dampHz_, 20.0, 0.49 * sampleRate_);
    damp_ = static_cast<float>(std::exp(-kTwoPi * hz / sampleRate_));
  }

  double sampleRate_ = 48000.0;
  float size_ = 0.5f, rt60_ = 2.0f, dampHz_ = 8000.0f, damp_ = 0.0f;
  std::vector<float> buf_[kLines];
  size_t mask_ = 0, pos_ = 0;
  size_t len_[kLines] = {};
  float gain_[kLines] = {};
  float lp_[kLines] = {};
};

}  // namespace tonic::dsp
