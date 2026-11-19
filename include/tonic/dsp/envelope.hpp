// Analog-style exponential ADSR (after Nigel Redmon's EarLevel design).
// Segment times are exact: attack reaches 1.0 after `attack` seconds. Decay and release times are
// defined over the full 1.0 -> 0 range, so releasing from a lower level finishes proportionally
// sooner (from 0.5: ~92% of the release time), as on analog envelopes.
#pragma once

#include <cmath>

namespace tonic::dsp {

struct Adsr {
  enum class Stage { Idle, Attack, Decay, Sustain, Release };

  void prepare(double sampleRate) { sampleRate_ = sampleRate; recompute(); }
  // Times in seconds, sustain 0..1.
  void set(double attack, double decay, double sustain, double release) {
    attack_ = attack; decay_ = decay; sustain_ = sustain; release_ = release;
    recompute();
  }
  void noteOn() { stage_ = Stage::Attack; }
  void noteOff() { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
  void reset() { stage_ = Stage::Idle; out_ = 0.0; }

  float process() {
    switch (stage_) {
      case Stage::Idle: break;
      case Stage::Attack:
        out_ = attackBase_ + out_ * attackCoef_;
        if (out_ >= 1.0) { out_ = 1.0; stage_ = Stage::Decay; }
        break;
      case Stage::Decay:
        out_ = decayBase_ + out_ * decayCoef_;
        if (out_ <= sustain_) { out_ = sustain_; stage_ = Stage::Sustain; }
        break;
      case Stage::Sustain: out_ = sustain_; break;
      case Stage::Release:
        out_ = releaseBase_ + out_ * releaseCoef_;
        if (out_ <= 0.0) { out_ = 0.0; stage_ = Stage::Idle; }
        break;
    }
    return static_cast<float>(out_);
  }

  Stage stage() const { return stage_; }
  bool isActive() const { return stage_ != Stage::Idle; }

 private:
  static double coef(double samples, double ratio) {
    return samples <= 0.0 ? 0.0 : std::exp(-std::log((1.0 + ratio) / ratio) / samples);
  }
  void recompute() {
    attackCoef_ = coef(attack_ * sampleRate_, kRatioA);
    attackBase_ = (1.0 + kRatioA) * (1.0 - attackCoef_);
    decayCoef_ = coef(decay_ * sampleRate_, kRatioDR);
    decayBase_ = (sustain_ - kRatioDR) * (1.0 - decayCoef_);
    releaseCoef_ = coef(release_ * sampleRate_, kRatioDR);
    releaseBase_ = -kRatioDR * (1.0 - releaseCoef_);
  }
  static constexpr double kRatioA = 0.3;       // attack curve: slightly convex like analog
  static constexpr double kRatioDR = 0.0001;   // decay/release: near-exponential
  double sampleRate_ = 48000.0;
  double attack_ = 0.01, decay_ = 0.1, sustain_ = 0.7, release_ = 0.2;
  double attackCoef_ = 0, attackBase_ = 0, decayCoef_ = 0, decayBase_ = 0;
  double releaseCoef_ = 0, releaseBase_ = 0;
  double out_ = 0.0;
  Stage stage_ = Stage::Idle;
};

}  // namespace tonic::dsp
