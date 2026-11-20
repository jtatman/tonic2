// Band-limited oscillator. Saw and square use minBLEP by default (minimum-phase band-limited steps from a
// generated table, much lower aliasing) or PolyBLEP (2-sample polynomial, cheapest). Triangle uses
// PolyBLAMP for its corners; sine needs no correction.
#pragma once

#include <cmath>

#include <array>

#include "math.hpp"
#include "minblep.hpp"

namespace tonic::dsp {

enum class Waveform { Sine, Saw, Square, Triangle };
enum class Antialias { MinBlep, PolyBlep };

// Two-sample polynomial band-limited step residual. t = phase in [0,1), dt = phase increment.
inline double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
  if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
  return 0.0;
}

// Two-sample polynomial band-limited ramp (integrated BLEP) residual.
inline double polyBlamp(double t, double dt) {
  if (t < dt) { t = t / dt - 1.0; return -t * t * t / 3.0; }
  if (t > 1.0 - dt) { t = (t - 1.0) / dt + 1.0; return t * t * t / 3.0; }
  return 0.0;
}

struct Oscillator {
  void prepare(double sampleRate) { sampleRate_ = sampleRate; setFrequency(freq_); }
  void reset(double phase = 0.0) {
    phase_ = phase - std::floor(phase);
    ring_.fill(0.f);
    ringIdx_ = 0;
  }
  void setAntialias(Antialias a) { antialias_ = a; }
  void setWaveform(Waveform w) { wave_ = w; }
  void setFrequency(double hz) { freq_ = hz; inc_ = std::fabs(hz) / sampleRate_; }
  // Pulse width for Square, clamped so both edges stay at least one sample apart.
  void setPulseWidth(double pw) { pw_ = pw; }

  float process() {
    const double t = phase_, dt = inc_;
    if (antialias_ == Antialias::MinBlep && (wave_ == Waveform::Saw || wave_ == Waveform::Square)) return processMinBlep();
    double y = 0.0;
    switch (wave_) {
      case Waveform::Sine:
        y = std::sin(kTwoPi * t);
        break;
      case Waveform::Saw:
        y = 2.0 * t - 1.0 - polyBlep(t, dt);
        break;
      case Waveform::Square: {
        const double pw = std::clamp(pw_, dt, 1.0 - dt);
        double t2 = t + 1.0 - pw;
        t2 -= std::floor(t2);
        y = (t < pw ? 1.0 : -1.0) + polyBlep(t, dt) - polyBlep(t2, dt);
        break;
      }
      case Waveform::Triangle: {
        // Corners at t = 0 (minimum) and t = 0.5 (maximum); slope +/-4 per cycle.
        double t2 = t + 0.5;
        t2 -= std::floor(t2);
        y = 4.0 * std::fabs(t - 0.5) - 1.0;
        y = -y;  // rises from -1 at t=0 to +1 at t=0.5
        y += 4.0 * dt * (polyBlamp(t, dt) - polyBlamp(t2, dt));
        break;
      }
    }
    phase_ += dt;
    if (phase_ >= 1.0) phase_ -= 1.0;
    return static_cast<float>(y);
  }

  void process(float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = process();
  }

  double phase() const { return phase_; }

 private:
  // Naive waveform plus pending minBLEP corrections; steps found between this sample and the next are
  // corrected starting at the next sample, `d` samples after the step.
  float processMinBlep() {
    const double t = phase_, dt = inc_;
    const double pw = std::clamp(pw_, dt, 1.0 - dt);
    double y = wave_ == Waveform::Saw ? 2.0 * t - 1.0 : (t < pw ? 1.0 : -1.0);
    // The minimum-phase step lags the naive edge, which would leave a DC offset on the saw (its single edge per
    // cycle has height -2); cancel it exactly. The square's two edges cancel on their own.
    if (wave_ == Waveform::Saw) y += 2.0 * table_->area * dt;
    y += ring_[static_cast<size_t>(ringIdx_)];
    ring_[static_cast<size_t>(ringIdx_)] = 0.f;
    ringIdx_ = (ringIdx_ + 1) % MinBlepTable::kTaps;
    double next = t + dt;
    if (wave_ == Waveform::Square) {
      if (t < pw && next >= pw && dt > 0.0) addStep(-2.0, (next - pw) / dt);
      if (next >= 1.0) {
        next -= 1.0;
        if (dt > 0.0) addStep(2.0, next / dt);
        if (next >= pw && dt > 0.0) addStep(-2.0, (next - pw) / dt);
      }
    } else if (next >= 1.0) {
      next -= 1.0;
      if (dt > 0.0) addStep(-2.0, next / dt);
    }
    phase_ = next;
    return static_cast<float>(y);
  }
  void addStep(double height, double d) {
    for (int k = 0; k < MinBlepTable::kTaps; ++k)
      ring_[static_cast<size_t>((ringIdx_ + k) % MinBlepTable::kTaps)] += static_cast<float>(height) * table_->at(k, d);
  }

  const MinBlepTable* table_ = &MinBlepTable::get();  // shared, read-only after first use
  std::array<float, MinBlepTable::kTaps> ring_{};
  int ringIdx_ = 0;
  Antialias antialias_ = Antialias::MinBlep;
  double sampleRate_ = 48000.0;
  double freq_ = 440.0;
  double inc_ = 440.0 / 48000.0;
  double phase_ = 0.0;
  double pw_ = 0.5;
  Waveform wave_ = Waveform::Saw;
};

}  // namespace tonic::dsp
