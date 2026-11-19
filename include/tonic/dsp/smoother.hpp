// Parameter smoothers: remove zipper noise when a parameter jumps.
#pragma once

#include <cmath>

namespace tonic::dsp {

// One-pole exponential smoother. After `timeSeconds` it has covered ~63% of a step.
struct OnePoleSmoother {
  void prepare(double sampleRate) { sampleRate_ = sampleRate; setTime(timeSeconds_); }
  void setTime(double timeSeconds) {
    timeSeconds_ = timeSeconds;
    coef_ = timeSeconds > 0.0 ? 1.0 - std::exp(-1.0 / (timeSeconds * sampleRate_)) : 1.0;
  }
  void reset(float value) { current_ = target_ = value; }
  void setTarget(float value) { target_ = value; }
  float next() {
    current_ += static_cast<float>(coef_) * (target_ - current_);
    return current_;
  }
  float current() const { return current_; }
  float target() const { return target_; }

 private:
  double sampleRate_ = 48000.0;
  double timeSeconds_ = 0.02;
  double coef_ = 1.0;
  float current_ = 0.0f;
  float target_ = 0.0f;
};

// Linear ramp to a target over a fixed number of frames. Exact arrival, useful for gains.
struct LinearRamp {
  void reset(float value) { current_ = target_ = value; remaining_ = 0; }
  void setTarget(float value, int frames) {
    target_ = value;
    if (frames <= 0) { current_ = value; remaining_ = 0; return; }
    step_ = (target_ - current_) / static_cast<float>(frames);
    remaining_ = frames;
  }
  float next() {
    if (remaining_ > 0) {
      current_ += step_;
      if (--remaining_ == 0) current_ = target_;
    }
    return current_;
  }
  bool isRamping() const { return remaining_ > 0; }
  float current() const { return current_; }

 private:
  float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
  int remaining_ = 0;
};

}  // namespace tonic::dsp
