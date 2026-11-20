// 2x / 4x oversampling with linear-phase halfband FIR stages, for nonlinear processing (saturation,
// driven filters) whose new harmonics would otherwise fold back below Nyquist.
//
// Each stage is a 63-tap Kaiser-windowed halfband lowpass (beta 7.86, ~80 dB stopband) with its cutoff at
// the base-rate Nyquist; passband is flat to ~0.42 x base rate (about 20 kHz at 48 kHz). 4x cascades two stages.
// Latency (up + down) is reported in base-rate samples. No allocation after prepare().
#pragma once

#include <array>
#include <cmath>

#include "math.hpp"

namespace tonic::dsp {

class HalfbandFilter {
 public:
  static constexpr int kTaps = 63;
  static constexpr int kCenter = (kTaps - 1) / 2;

  HalfbandFilter() { design(); }
  void reset() { hist_.fill(0.f); pos_ = 0; }
  // Push one sample (at the higher rate) and return the filtered output.
  float process(float x) {
    pos_ = (pos_ == 0 ? kTaps : pos_) - 1;
    hist_[static_cast<size_t>(pos_)] = x;
    hist_[static_cast<size_t>(pos_ + kTaps)] = x;  // mirrored copy: contiguous window, no wrap in the loop
    const float* h = &hist_[static_cast<size_t>(pos_)];
    float acc = coef_[kCenter] * h[kCenter];
    for (int k = 1; k <= kCenter; k += 2) acc += coef_[static_cast<size_t>(kCenter - k)] * (h[kCenter - k] + h[kCenter + k]);
    return acc;
  }
  // Push a sample without computing an output (the decimator discards every other output).
  void push(float x) {
    pos_ = (pos_ == 0 ? kTaps : pos_) - 1;
    hist_[static_cast<size_t>(pos_)] = x;
    hist_[static_cast<size_t>(pos_ + kTaps)] = x;
  }
  static constexpr int latency() { return kCenter; }  // in samples at the filter's own rate

 private:
  static double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
      term *= (x / (2.0 * k)) * (x / (2.0 * k));
      sum += term;
      if (term < 1e-12 * sum) break;
    }
    return sum;
  }
  void design() {
    const double beta = 7.86;  // Kaiser: ~80 dB stopband
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
      const int n = i - kCenter;
      // Halfband: cutoff at a quarter of this filter's rate; even offsets (except 0) are exactly zero.
      const double sinc = n == 0 ? 0.5 : (n % 2 == 0 ? 0.0 : std::sin(kPi * n / 2.0) / (kPi * n));
      const double r = static_cast<double>(n) / kCenter;
      const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
      coef_[static_cast<size_t>(i)] = static_cast<float>(sinc * w);
      sum += sinc * w;
    }
    for (auto& c : coef_) c = static_cast<float>(c / sum);  // unity DC gain
    reset();
  }
  std::array<float, kTaps> coef_{};
  std::array<float, 2 * kTaps> hist_{};
  int pos_ = 0;
};

// Oversampler for factor 1, 2 or 4. Per base-rate sample: up() produces `factor` samples, the caller
// processes them at factor x the sample rate, and down() turns them back into one base-rate sample.
class Oversampler {
 public:
  void prepare(int factor) {
    factor_ = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
    for (auto& f : up_) f.reset();
    for (auto& f : down_) f.reset();
  }
  int factor() const { return factor_; }
  // Total latency of up + down, in base-rate samples (may be fractional for 4x).
  double latency() const {
    if (factor_ == 1) return 0.0;
    const double stage = 2.0 * HalfbandFilter::latency();  // up + down, at the stage's 2x rate
    return factor_ == 2 ? stage / 2.0 : stage / 2.0 + stage / 4.0;
  }

  void up(float x, float* out) {
    if (factor_ == 1) { out[0] = x; return; }
    // Zero-stuff and filter (gain 2 restores level).
    out[0] = 2.f * up_[0].process(x);
    out[1] = 2.f * up_[0].process(0.f);
    if (factor_ == 2) return;
    const float a = out[0], b = out[1];
    out[0] = 2.f * up_[1].process(a);
    out[1] = 2.f * up_[1].process(0.f);
    out[2] = 2.f * up_[1].process(b);
    out[3] = 2.f * up_[1].process(0.f);
  }

  float down(const float* in) {
    if (factor_ == 1) return in[0];
    // Keep the even phase (the positions of the original samples) so the 2x latency is a whole number.
    if (factor_ == 2) {
      const float y = down_[0].process(in[0]);
      down_[0].push(in[1]);
      return y;
    }
    const float a = down_[1].process(in[0]);
    down_[1].push(in[1]);
    const float b = down_[1].process(in[2]);
    down_[1].push(in[3]);
    const float y = down_[0].process(a);
    down_[0].push(b);
    return y;
  }

 private:
  int factor_ = 1;
  HalfbandFilter up_[2], down_[2];  // [0] = base <-> 2x, [1] = 2x <-> 4x
};

}  // namespace tonic::dsp
