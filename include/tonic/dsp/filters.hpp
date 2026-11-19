// Zero-delay-feedback (topology-preserving transform) filters.
// Stay stable and well-behaved when cutoff is modulated quickly, unlike direct-form biquads.
#pragma once

#include "math.hpp"

namespace tonic::dsp {

// State-variable filter (Simper / Zavalishin TPT form). All four responses from one update.
struct Svf {
  struct Out { float low, band, high, notch; };

  void prepare(double sampleRate) { sampleRate_ = sampleRate; update(); }
  void reset() { ic1_ = ic2_ = 0.0; }
  void setCutoff(double hz) { cutoff_ = hz; update(); }
  // Q = 0.707 is Butterworth (no peak). Higher Q resonates.
  void setQ(double q) { q_ = q > 0.05 ? q : 0.05; update(); }

  Out process(float x) {
    const double v3 = x - ic2_;
    const double v1 = a1_ * ic1_ + a2_ * v3;
    const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0 * v1 - ic1_;
    ic2_ = 2.0 * v2 - ic2_;
    const double high = x - k_ * v1 - v2;
    return {static_cast<float>(v2), static_cast<float>(v1), static_cast<float>(high),
            static_cast<float>(v2 + high)};
  }
  float processLow(float x) { return process(x).low; }

 private:
  void update() {
    const double g = tptGain(cutoff_, sampleRate_);
    k_ = 1.0 / q_;
    a1_ = 1.0 / (1.0 + g * (g + k_));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }
  double sampleRate_ = 48000.0, cutoff_ = 1000.0, q_ = 0.70710678;
  double k_ = 1.41421356, a1_ = 0, a2_ = 0, a3_ = 0;
  double ic1_ = 0, ic2_ = 0;
};

// Four-pole (24 dB/oct) ladder low-pass, ZDF solve of the feedback loop.
// resonance 0..1 maps to feedback 0..4 (self-oscillation near 1). drive > 1 saturates the input.
struct Ladder {
  void prepare(double sampleRate) { sampleRate_ = sampleRate; update(); }
  void reset() { s_[0] = s_[1] = s_[2] = s_[3] = 0.0; }
  void setCutoff(double hz) { cutoff_ = hz; update(); }
  void setResonance(double r) { k_ = 4.0 * std::clamp(r, 0.0, 1.0); }
  void setDrive(double d) { drive_ = d > 0.01 ? d : 0.01; }

  float process(float in) {
    const double G2 = G_ * G_, G4 = G2 * G2;
    const double oneMinusG = 1.0 - G_;
    // Contribution of the stage states to the 4th stage output.
    const double S = G_ * G2 * oneMinusG * s_[0] + G2 * oneMinusG * s_[1] + G_ * oneMinusG * s_[2] +
                     oneMinusG * s_[3];
    const double x = drive_ * in;
    const double y4 = (G4 * x + S) / (1.0 + k_ * G4);
    double u = x - k_ * y4;
    u = padeTanh(static_cast<float>(u));  // nonlinearity at the loop input
    for (double& s : s_) {
      const double v = (u - s) * G_;
      const double y = v + s;
      s = y + v;
      u = y;
    }
    // Compensate passband loss from feedback so resonance doesn't thin the sound as much.
    // Divide by drive so drive changes the amount of saturation, not the output level.
    return static_cast<float>(u * (1.0 + 0.5 * k_) / drive_);
  }

 private:
  void update() {
    const double g = tptGain(cutoff_, sampleRate_);
    G_ = g / (1.0 + g);
  }
  double sampleRate_ = 48000.0, cutoff_ = 1000.0, k_ = 0.0, drive_ = 1.0, G_ = 0.0;
  double s_[4] = {0, 0, 0, 0};
};

}  // namespace tonic::dsp
