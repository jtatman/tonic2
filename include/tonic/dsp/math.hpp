// tonic2 DSP kernels: shared math helpers.
// Unlicense (public domain). Kernels are plain structs: no virtuals, no allocation after prepare(),
// no global state, no exceptions. Keep them simple enough to port line-for-line to Rust and Go.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tonic::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

// Pade approximation of tanh, accurate to ~0.2% inside [-3, 3], clamped outside (|y| -> 1).
inline float padeTanh(float x) {
  x = std::clamp(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-12f)); }
inline float midiToHz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// Prewarped bilinear integrator gain for a cutoff in Hz: g = tan(pi * fc / fs).
// Cutoff is clamped just below Nyquist so g stays finite.
inline double tptGain(double cutoffHz, double sampleRate) {
  const double fc = std::clamp(cutoffHz, 1.0, 0.49 * sampleRate);
  return std::tan(kPi * fc / sampleRate);
}

}  // namespace tonic::dsp
