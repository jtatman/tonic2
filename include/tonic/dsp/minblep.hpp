// Minimum-phase band-limited step (minBLEP) residual table, generated at runtime (Eli Brandt's method):
//   1. Blackman-windowed sinc (cutoff kCutoff x Nyquist), kZeroCrossings zero crossings, kOversample points per sample
//   2. minimum phase via the real cepstrum (log magnitude -> fold -> exp)
//   3. integrate to a step, normalise to end at 1; store residual = step - 1
// Generated once on first use (call it from prepare(), not on the audio thread) and read-only afterwards.
// Being computed from first principles keeps it portable (Rust/Go ports regenerate the same table).
#pragma once

#include <cmath>
#include <complex>
#include <vector>

#include "math.hpp"

namespace tonic::dsp {

struct MinBlepTable {
  static constexpr int kZeroCrossings = 16;
  static constexpr int kOversample = 64;
  static constexpr int kTaps = 2 * kZeroCrossings;  // output samples a step correction spans
  static constexpr double kCutoff = 0.9;  // fraction of Nyquist: room for the transition band keeps aliases out

  std::vector<float> residual;  // kTaps * kOversample + 1 points: step(t) - 1 for t in [0, kTaps] samples
  double area = 0.0;  // integral of the residual in samples (negative: the step is delayed); used for DC correction

  // Correction for the k-th sample after a unit step that happened `d` samples (0..1) before sample 0.
  float at(int k, double d) const {
    const double pos = (k + d) * kOversample;
    const int i = static_cast<int>(pos);
    if (i >= static_cast<int>(residual.size()) - 1) return 0.f;
    const float f = static_cast<float>(pos - i);
    return residual[static_cast<size_t>(i)] + f * (residual[static_cast<size_t>(i) + 1] - residual[static_cast<size_t>(i)]);
  }

  static const MinBlepTable& get() {
    static const MinBlepTable table = build();  // thread-safe one-time init (C++11)
    return table;
  }

 private:
  static void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
      const auto w = std::polar(1.0, (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len));
      for (size_t i = 0; i < n; i += len) {
        std::complex<double> wn(1.0);
        for (size_t k = 0; k < len / 2; ++k, wn *= w) {
          const auto u = a[i + k], v = a[i + k + len / 2] * wn;
          a[i + k] = u + v;
          a[i + k + len / 2] = u - v;
        }
      }
    }
    if (inverse) for (auto& x : a) x /= static_cast<double>(n);
  }

  static MinBlepTable build() {
    const int len = 2 * kZeroCrossings * kOversample + 1;  // windowed sinc length
    size_t n = 1;
    while (n < static_cast<size_t>(len) * 4) n <<= 1;      // padding keeps cepstral aliasing small
    std::vector<std::complex<double>> x(n, 0.0);
    for (int i = 0; i < len; ++i) {
      const double t = static_cast<double>(i - (len - 1) / 2) / kOversample;  // in samples, -Z..Z
      const double sinc = t == 0.0 ? kCutoff : std::sin(kPi * kCutoff * t) / (kPi * t);
      const double w = 0.42 - 0.5 * std::cos(kTwoPi * i / (len - 1)) + 0.08 * std::cos(2.0 * kTwoPi * i / (len - 1));
      x[static_cast<size_t>(i)] = sinc * w;
    }
    // Real cepstrum -> minimum phase.
    fft(x, false);
    for (auto& v : x) v = std::log(std::max(std::abs(v), 1e-100));
    fft(x, true);
    for (size_t i = 1; i < n / 2; ++i) { x[i] *= 2.0; x[n - i] = 0.0; }
    fft(x, false);
    for (auto& v : x) v = std::exp(v);
    fft(x, true);
    // Integrate the minimum-phase impulse into a step and normalise it to end at exactly 1.
    MinBlepTable t;
    const int points = kTaps * kOversample + 1;
    std::vector<double> step(static_cast<size_t>(points));
    double acc = 0.0;
    for (int i = 0; i < points; ++i) { acc += x[static_cast<size_t>(i)].real(); step[static_cast<size_t>(i)] = acc; }
    const double end = step.back();
    t.residual.resize(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) t.residual[static_cast<size_t>(i)] = static_cast<float>(step[static_cast<size_t>(i)] / end - 1.0);
    t.residual.back() = 0.f;
    for (int i = 0; i + 1 < points; ++i) t.area += 0.5 * (t.residual[static_cast<size_t>(i)] + t.residual[static_cast<size_t>(i) + 1]) / kOversample;
    return t;
  }
};

}  // namespace tonic::dsp
