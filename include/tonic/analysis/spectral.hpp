// Spectral analysis for comparing two sounds: a radix-2 FFT, log-magnitude STFT frames, and a
// multi-resolution STFT distance (the loss used by DDSP and most sound-matching work).
// Control thread / offline only: these functions allocate.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace tonic::analysis {

// In-place iterative radix-2 FFT. data.size() must be a power of two.
inline void fft(std::vector<std::complex<double>>& data) {
  const size_t n = data.size();
  for (size_t i = 1, j = 0; i < n; ++i) {  // bit reversal
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(data[i], data[j]);
  }
  const double pi = 3.14159265358979323846;
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * pi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = data[i + k], v = data[i + k + len / 2] * w;
        data[i + k] = u + v;
        data[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Magnitude spectrogram: frames[f][bin], bins 0..fftSize/2, Hann window, hop = fftSize/4.
// The signal is zero-padded so every sample is covered by at least one full frame.
inline std::vector<std::vector<float>> magnitudeFrames(const std::vector<float>& x, int fftSize) {
  const size_t n = static_cast<size_t>(fftSize), hop = n / 4, bins = n / 2 + 1;
  std::vector<double> window(n);
  const double pi = 3.14159265358979323846;
  for (size_t i = 0; i < n; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(n));
  std::vector<std::vector<float>> frames;
  std::vector<std::complex<double>> buf(n);
  for (size_t start = 0; start < x.size() + hop; start += hop) {
    for (size_t i = 0; i < n; ++i) {
      const size_t idx = start + i;
      buf[i] = {idx < x.size() ? x[idx] * window[i] : 0.0, 0.0};
    }
    fft(buf);
    std::vector<float> mag(bins);
    for (size_t b = 0; b < bins; ++b) mag[b] = static_cast<float>(std::abs(buf[b]));
    frames.push_back(std::move(mag));
    if (start + n >= x.size()) break;
  }
  return frames;
}

// Sum of channels / channel count. Comparing mono mixes keeps the distance independent of panning.
inline std::vector<float> monoMix(const std::vector<std::vector<float>>& channels) {
  if (channels.empty()) return {};
  std::vector<float> m(channels[0].size(), 0.f);
  for (const auto& c : channels)
    for (size_t i = 0; i < m.size() && i < c.size(); ++i) m[i] += c[i];
  for (float& v : m) v /= static_cast<float>(channels.size());
  return m;
}

struct StftDistanceOptions {
  std::vector<int> fftSizes{256, 1024, 4096};  // short sizes catch transients, long ones pitch
  double logWeight = 1.0;                      // weight of the log-magnitude L1 term
  double convergenceWeight = 1.0;              // weight of the spectral-convergence term
};

// Precomputed spectrograms of a target, so a search compares against it without re-analysing it.
struct SpectralTarget {
  std::vector<std::vector<std::vector<float>>> perSize;  // [size][frame][bin]
  StftDistanceOptions options;
};

inline SpectralTarget analyse(const std::vector<float>& x, const StftDistanceOptions& options = {}) {
  SpectralTarget t;
  t.options = options;
  for (int size : options.fftSizes) t.perSize.push_back(magnitudeFrames(x, size));
  return t;
}

// Multi-resolution STFT distance between `x` and an analysed target. For each FFT size:
//   spectral convergence  ||S_t - S_x||_F / ||S_t||_F   (relative error, dominated by loud partials)
//   log-magnitude L1      mean |log(S_t + eps) - log(S_x + eps)|   (hears quiet detail too)
// averaged over sizes. 0 for identical signals. Frames past the shorter signal count as silence.
inline double stftDistance(const SpectralTarget& target, const std::vector<float>& x) {
  const double eps = 1e-5;
  double total = 0.0;
  const auto& opt = target.options;
  for (size_t s = 0; s < opt.fftSizes.size(); ++s) {
    const auto& tf = target.perSize[s];
    const auto xf = magnitudeFrames(x, opt.fftSizes[s]);
    const size_t frames = std::max(tf.size(), xf.size());
    const size_t bins = static_cast<size_t>(opt.fftSizes[s]) / 2 + 1;
    double diff2 = 0.0, ref2 = 0.0, logSum = 0.0;
    for (size_t f = 0; f < frames; ++f)
      for (size_t b = 0; b < bins; ++b) {
        const double a = f < tf.size() ? tf[f][b] : 0.0, c = f < xf.size() ? xf[f][b] : 0.0;
        diff2 += (a - c) * (a - c);
        ref2 += a * a;
        logSum += std::fabs(std::log(a + eps) - std::log(c + eps));
      }
    const double sc = std::sqrt(diff2) / std::max(std::sqrt(ref2), 1e-12);
    const double lm = logSum / static_cast<double>(frames * bins);
    total += opt.convergenceWeight * sc + opt.logWeight * lm;
  }
  return total / static_cast<double>(opt.fftSizes.size());
}

inline double stftDistance(const std::vector<float>& a, const std::vector<float>& b, const StftDistanceOptions& options = {}) {
  return stftDistance(analyse(a, options), b);
}

}  // namespace tonic::analysis
