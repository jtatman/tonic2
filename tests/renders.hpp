// Reference renders shared by the golden-file generator and the tests.
// Changing a render on purpose: run `tonic2_golden --update` and commit the new files.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tonic/dsp/envelope.hpp"
#include "tonic/dsp/filters.hpp"
#include "tonic/dsp/noise.hpp"
#include "tonic/dsp/oscillator.hpp"

namespace renders {

using namespace tonic::dsp;
constexpr double kRate = 48000.0;
constexpr int kFrames = 4096;

inline std::vector<float> oscillator(Waveform w, double hz, double pw = 0.5) {
  Oscillator o;
  o.prepare(kRate);
  o.setWaveform(w);
  o.setFrequency(hz);
  o.setPulseWidth(pw);
  std::vector<float> out(kFrames);
  o.process(out.data(), kFrames);
  return out;
}

inline std::vector<float> svfSweep() {
  WhiteNoise n(1234);
  Svf f;
  f.prepare(kRate);
  f.setQ(4.0);
  std::vector<float> out(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    f.setCutoff(100.0 * std::pow(100.0, i / double(kFrames)));  // 100 Hz -> 10 kHz
    out[i] = f.processLow(n.process() * 0.5f);
  }
  return out;
}

inline std::vector<float> ladderResonant() {
  Oscillator o;
  o.prepare(kRate);
  o.setWaveform(Waveform::Saw);
  o.setFrequency(110.0);
  Ladder l;
  l.prepare(kRate);
  l.setResonance(0.85);
  l.setDrive(2.0);
  std::vector<float> out(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    l.setCutoff(300.0 + 2700.0 * i / double(kFrames));
    out[i] = l.process(o.process());
  }
  return out;
}

inline std::vector<float> adsr() {
  Adsr e;
  e.prepare(kRate);
  e.set(0.01, 0.02, 0.5, 0.03);
  e.noteOn();
  std::vector<float> out(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    if (i == 2400) e.noteOff();
    out[i] = e.process();
  }
  return out;
}

struct Named { std::string name; std::function<std::vector<float>()> fn; };

inline std::vector<Named> all() {
  return {
      {"osc_saw_440", [] { return oscillator(Waveform::Saw, 440.0); }},
      {"osc_square_pw30_440", [] { return oscillator(Waveform::Square, 440.0, 0.3); }},
      {"osc_triangle_440", [] { return oscillator(Waveform::Triangle, 440.0); }},
      {"osc_sine_440", [] { return oscillator(Waveform::Sine, 440.0); }},
      {"svf_lp_sweep_q4", svfSweep},
      {"ladder_resonant_saw", ladderResonant},
      {"adsr_basic", adsr},
  };
}

}  // namespace renders
