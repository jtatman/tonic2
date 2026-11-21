// Audio descriptors for one rendered sound: a few numbers that say how loud, bright, noisy and
// percussive it is. They are the feedback a text-to-patch agent (tools/patch_agent) gets in place of
// hearing, and they are handy in tests. Offline only: allocates.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "spectral.hpp"

namespace tonic::analysis {

struct AudioFeatures {
  double peakDb = -120.0;       // sample peak, dBFS
  double rmsDb = -120.0;        // whole-render RMS, dBFS
  double centroidHz = 0.0;      // energy-weighted mean spectral centroid ("brightness")
  double rolloffHz = 0.0;       // frequency below which 85% of the energy lies, energy-weighted mean
  double flatness = 0.0;        // spectral flatness 0 (pure tones) .. 1 (white noise), energy-weighted
  double bandLow = 0.0;         // share of energy below 200 Hz
  double bandLowMid = 0.0;      // 200 Hz .. 2 kHz
  double bandHighMid = 0.0;     // 2 kHz .. 8 kHz
  double bandHigh = 0.0;        // above 8 kHz
  double attackMs = 0.0;        // first onset to 90% of the first peak (10 ms RMS envelope)
  double sustainRatio = 0.0;    // mean envelope over the middle of the render / envelope max (1 = organ, ~0 = pluck)
  double tailDb = -120.0;       // envelope in the last 100 ms relative to its max, dB (reverb/delay/release tail)
};

inline double toDb(double v) { return 20.0 * std::log10(std::max(v, 1e-6)); }

inline AudioFeatures computeFeatures(const std::vector<float>& x, double sampleRate) {
  AudioFeatures f;
  if (x.empty() || sampleRate <= 0) return f;
  double peak = 0.0, sum2 = 0.0;
  for (float v : x) { peak = std::max(peak, double(std::fabs(v))); sum2 += double(v) * v; }
  f.peakDb = toDb(peak);
  f.rmsDb = toDb(std::sqrt(sum2 / double(x.size())));

  // Spectral shape from 2048-point frames, each weighted by its energy so silence does not count.
  const int n = 2048;
  const auto frames = magnitudeFrames(x, n);
  const double binHz = sampleRate / n;
  double wSum = 0.0, cSum = 0.0, rSum = 0.0, flSum = 0.0, bands[4] = {0, 0, 0, 0};
  for (const auto& mag : frames) {
    double e = 0.0, num = 0.0, logSum = 0.0;
    for (size_t b = 1; b < mag.size(); ++b) {
      const double p = double(mag[b]) * mag[b];
      e += p;
      num += p * b * binHz;
      logSum += std::log(p + 1e-20);
      const double hz = b * binHz;
      bands[hz < 200 ? 0 : hz < 2000 ? 1 : hz < 8000 ? 2 : 3] += p;
    }
    if (e < 1e-12) continue;
    double acc = 0.0, roll = 0.0;
    for (size_t b = 1; b < mag.size(); ++b) {
      acc += double(mag[b]) * mag[b];
      if (acc >= 0.85 * e) { roll = b * binHz; break; }
    }
    const double bins = double(mag.size() - 1);
    const double flat = std::exp(logSum / bins) / (e / bins);
    wSum += e;
    cSum += e * (num / e);
    rSum += e * roll;
    flSum += e * flat;
  }
  if (wSum > 0) {
    f.centroidHz = cSum / wSum;
    f.rolloffHz = rSum / wSum;
    f.flatness = std::clamp(flSum / wSum, 0.0, 1.0);
    const double bt = bands[0] + bands[1] + bands[2] + bands[3];
    f.bandLow = bands[0] / bt;
    f.bandLowMid = bands[1] / bt;
    f.bandHighMid = bands[2] / bt;
    f.bandHigh = bands[3] / bt;
  }

  // 10 ms RMS envelope.
  const size_t win = std::max<size_t>(1, static_cast<size_t>(sampleRate * 0.01));
  std::vector<double> env;
  for (size_t i = 0; i < x.size(); i += win) {
    double s = 0.0;
    const size_t end = std::min(x.size(), i + win);
    for (size_t k = i; k < end; ++k) s += double(x[k]) * x[k];
    env.push_back(std::sqrt(s / double(end - i)));
  }
  const double envMax = *std::max_element(env.begin(), env.end());
  if (envMax > 1e-6) {
    size_t onset = 0;
    while (onset < env.size() && env[onset] < 0.1 * envMax) ++onset;
    // First peak: the highest point within 500 ms of the onset.
    const size_t span = std::min(env.size(), onset + static_cast<size_t>(50));
    double firstPeak = 0.0;
    for (size_t i = onset; i < span; ++i) firstPeak = std::max(firstPeak, env[i]);
    size_t reach = onset;
    while (reach < span && env[reach] < 0.9 * firstPeak) ++reach;
    f.attackMs = double(reach - onset) * 10.0;
    const size_t a = env.size() / 4, b = std::max(a + 1, env.size() * 3 / 4);
    double mid = 0.0;
    for (size_t i = a; i < b; ++i) mid += env[i];
    f.sustainRatio = mid / double(b - a) / envMax;
    const size_t tailStart = env.size() > 10 ? env.size() - 10 : 0;
    double tail = 0.0;
    for (size_t i = tailStart; i < env.size(); ++i) tail += env[i];
    f.tailDb = toDb(tail / double(env.size() - tailStart) / envMax);
  }
  return f;
}

inline std::string toJson(const AudioFeatures& f) {
  char b[640];
  std::snprintf(b, sizeof b,
                "{\"peak_db\": %.2f, \"rms_db\": %.2f, \"centroid_hz\": %.1f, \"rolloff_hz\": %.1f, \"flatness\": %.4f, "
                "\"band_low\": %.4f, \"band_low_mid\": %.4f, \"band_high_mid\": %.4f, \"band_high\": %.4f, "
                "\"attack_ms\": %.1f, \"sustain_ratio\": %.3f, \"tail_db\": %.2f}",
                f.peakDb, f.rmsDb, f.centroidHz, f.rolloffHz, f.flatness, f.bandLow, f.bandLowMid, f.bandHighMid,
                f.bandHigh, f.attackMs, f.sustainRatio, f.tailDb);
  return b;
}

}  // namespace tonic::analysis
