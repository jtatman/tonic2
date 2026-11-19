// Band-limited oscillator using PolyBLEP (steps) and PolyBLAMP (corners).
// Table-free, cheap, and easy to port. A minBLEP variant can be added later for lower aliasing.
#pragma once

#include <cmath>

#include "math.hpp"

namespace tonic::dsp {

enum class Waveform { Sine, Saw, Square, Triangle };

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
  void reset(double phase = 0.0) { phase_ = phase - std::floor(phase); }
  void setWaveform(Waveform w) { wave_ = w; }
  void setFrequency(double hz) { freq_ = hz; inc_ = std::fabs(hz) / sampleRate_; }
  // Pulse width for Square, clamped so both edges stay at least one sample apart.
  void setPulseWidth(double pw) { pw_ = pw; }

  float process() {
    const double t = phase_, dt = inc_;
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
  double sampleRate_ = 48000.0;
  double freq_ = 440.0;
  double inc_ = 440.0 / 48000.0;
  double phase_ = 0.0;
  double pw_ = 0.5;
  Waveform wave_ = Waveform::Saw;
};

}  // namespace tonic::dsp
