// Feed-forward compressor / limiter with a log-domain gain computer.
// Detector: peak envelope of |x| (instant attack, decays at the release rate) in dB, so the gain computer
// sees the waveform's peak level rather than every half-cycle dip. Static curve: threshold, ratio, quadratic soft knee. The resulting gain
// reduction (dB, >= 0) is smoothed by a one-pole with separate attack (reduction rising) and release
// coefficients, then converted to a linear gain together with makeup. Use ratio ~1000 for limiting.
#pragma once

#include <algorithm>
#include <cmath>

#include "math.hpp"

namespace tonic::dsp {

struct Compressor {
  void prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    setAttack(attack_);
    setRelease(release_);
    reset();
  }
  void reset() { reductionDb_ = 0.0f; env_ = 0.0f; }
  void setThreshold(float db) { threshold_ = db; }
  void setRatio(float ratio) { ratio_ = std::max(ratio, 1.0f); }
  void setKnee(float db) { knee_ = std::max(db, 0.0f); }
  void setAttack(float seconds) {
    attack_ = seconds;
    attackCoef_ = coef(seconds);
  }
  void setRelease(float seconds) {
    release_ = seconds;
    releaseCoef_ = coef(seconds);
  }
  void setMakeup(float db) { makeup_ = db; }

  float process(float in) { return in * gainFor(std::fabs(in)); }
  void process(float& l, float& r) {
    const float g = gainFor(std::max(std::fabs(l), std::fabs(r)));
    l *= g;
    r *= g;
  }

  // Linear gain for an external detector value (linear magnitude); advances the smoother.
  float gainFor(float detector) {
    env_ = std::max(detector, env_ * releaseCoef_);  // peak envelope
    const float over = gainToDb(env_) - threshold_;
    const float slope = 1.0f / ratio_ - 1.0f;  // <= 0
    float target;                              // gain reduction in dB, >= 0
    if (2.0f * over < -knee_ || (knee_ <= 0.0f && over <= 0.0f)) target = 0.0f;  // hard knee: no 0/0 at threshold
    else if (knee_ > 0.0f && 2.0f * over <= knee_) {
      const float x = over + knee_ * 0.5f;
      target = -slope * x * x / (2.0f * knee_);
    } else target = -slope * over;
    const float c = target > reductionDb_ ? attackCoef_ : releaseCoef_;
    reductionDb_ = c * reductionDb_ + (1.0f - c) * target;
    return dbToGain(makeup_ - reductionDb_);
  }
  float gainReductionDb() const { return reductionDb_; }

 private:
  float coef(float seconds) const {
    return seconds > 0.0f ? static_cast<float>(std::exp(-1.0 / (static_cast<double>(seconds) * sampleRate_))) : 0.0f;
  }

  double sampleRate_ = 48000.0;
  float threshold_ = -18.0f, ratio_ = 2.0f, knee_ = 0.0f, makeup_ = 0.0f;
  float attack_ = 0.005f, release_ = 0.1f;
  float attackCoef_ = coef(0.005f), releaseCoef_ = coef(0.1f);
  float reductionDb_ = 0.0f, env_ = 0.0f;
};

}  // namespace tonic::dsp
