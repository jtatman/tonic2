// Signal nodes: every setting is an input port, so anything can modulate anything (the Tonic 1
// model). These are what the patching syntax (tonic/dsl) builds. Inputs that carry settings are mono;
// coefficients are recomputed only when the incoming value changes, so constant settings cost nothing.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../dsp/envelope.hpp"
#include "../dsp/filters.hpp"
#include "../dsp/noise.hpp"
#include "../dsp/oscillator.hpp"
#include "node.hpp"

namespace tonic::graph {

class ConstantNode final : public Node {
 public:
  explicit ConstantNode(float v) : Node({}, {1}), v_(v) {}
  void process(const ProcessArgs& a) override { std::fill(a.output(0, 0), a.output(0, 0) + a.numFrames, v_); }
 private:
  float v_;
};

// Outputs an engine parameter's per-sample values (or `fallback` if the engine has none).
class ParamSignalNode final : public Node {
 public:
  ParamSignalNode(int id, float fallback) : Node({}, {1}), id_(id), fallback_(fallback) {}
  void process(const ProcessArgs& a) override {
    const float* p = a.param(id_);
    float* out = a.output(0, 0);
    if (p) std::copy(p, p + a.numFrames, out);
    else std::fill(out, out + a.numFrames, fallback_);
  }
 private:
  int id_;
  float fallback_;
};

// Outputs one of the engine's note control signals.
class ControlSignalNode final : public Node {
 public:
  enum class Kind { Gate, Trigger, Pitch, Velocity, Pressure, Timbre };
  ControlSignalNode(Kind k, float fallback) : Node({}, {1}), kind_(k), fallback_(fallback) {}
  void process(const ProcessArgs& a) override {
    const float* src = nullptr;
    if (a.control) {
      switch (kind_) {
        case Kind::Gate: src = a.control->gate; break;
        case Kind::Trigger: src = a.control->trigger; break;
        case Kind::Pitch: src = a.control->pitchHz; break;
        case Kind::Velocity: src = a.control->velocity; break;
        case Kind::Pressure: src = a.control->pressure; break;
        case Kind::Timbre: src = a.control->timbre; break;
      }
    }
    float* out = a.output(0, 0);
    if (src) std::copy(src, src + a.numFrames, out);
    else std::fill(out, out + a.numFrames, fallback_);
  }
 private:
  Kind kind_;
  float fallback_;
};

// Element-wise a (op) b over N channels (mono inputs are broadcast by the graph).
class BinaryOpNode final : public Node {
 public:
  enum class Op { Add, Sub, Mul, Div, Min, Max };
  BinaryOpNode(Op op, int channels) : Node({channels, channels}, {channels}), op_(op) {}
  void process(const ProcessArgs& a) override {
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float *x = a.input(0, c), *y = a.input(1, c);
      float* o = a.output(0, c);
      const int n = a.numFrames;
      switch (op_) {
        case Op::Add: for (int i = 0; i < n; ++i) o[i] = x[i] + y[i]; break;
        case Op::Sub: for (int i = 0; i < n; ++i) o[i] = x[i] - y[i]; break;
        case Op::Mul: for (int i = 0; i < n; ++i) o[i] = x[i] * y[i]; break;
        case Op::Div: for (int i = 0; i < n; ++i) o[i] = y[i] != 0.f ? x[i] / y[i] : 0.f; break;
        case Op::Min: for (int i = 0; i < n; ++i) o[i] = std::min(x[i], y[i]); break;
        case Op::Max: for (int i = 0; i < n; ++i) o[i] = std::max(x[i], y[i]); break;
      }
    }
  }
 private:
  Op op_;
};

// Element-wise functions over N channels.
class UnaryOpNode final : public Node {
 public:
  enum class Op { MidiToHz, DbToGain, Abs, Tanh };
  UnaryOpNode(Op op, int channels) : Node({channels}, {channels}), op_(op) {}
  void process(const ProcessArgs& a) override {
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float* x = a.input(0, c);
      float* o = a.output(0, c);
      for (int i = 0; i < a.numFrames; ++i) {
        switch (op_) {
          case Op::MidiToHz: o[i] = dsp::midiToHz(x[i]); break;
          case Op::DbToGain: o[i] = dsp::dbToGain(x[i]); break;
          case Op::Abs: o[i] = std::fabs(x[i]); break;
          case Op::Tanh: o[i] = std::tanh(x[i]); break;
        }
      }
    }
  }
 private:
  Op op_;
};

// Oscillator with frequency (Hz) on input 0 and pulse width on input 1 (square only).
class ModOscillatorNode final : public Node {
 public:
  explicit ModOscillatorNode(dsp::Waveform w) : Node({1, 1}, {1}), wave_(w) {}
  void prepare(double sr, int) override { osc_.prepare(sr); osc_.setWaveform(wave_); osc_.reset(); }
  void process(const ProcessArgs& a) override {
    const float *f = a.input(0, 0), *pw = a.input(1, 0);
    float* out = a.output(0, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      osc_.setFrequency(f[i]);
      osc_.setPulseWidth(pw[i]);
      out[i] = osc_.process();
    }
  }
 private:
  dsp::Oscillator osc_;
  dsp::Waveform wave_;
};

class NoiseNode final : public Node {
 public:
  explicit NoiseNode(uint32_t seed) : Node({}, {1}), seed_(seed) {}
  void prepare(double, int) override { noise_.seed(seed_); }
  void process(const ProcessArgs& a) override {
    float* out = a.output(0, 0);
    for (int i = 0; i < a.numFrames; ++i) out[i] = noise_.process();
  }
 private:
  dsp::WhiteNoise noise_;
  uint32_t seed_;
};

// Ladder: input 0 = signal (N ch), 1 = cutoff Hz, 2 = resonance 0..1. oversample = 1, 2 or 4.
class ModLadderNode final : public Node {
 public:
  ModLadderNode(int channels, double drive, int oversample = 1)
      : Node({channels, 1, 1}, {channels}), drive_(drive), oversample_(oversample), f_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : f_) { f.prepare(sr); f.setOversampling(oversample_); f.setDrive(drive_); f.reset(); }
    lastFc_ = lastRes_ = -1.f;
  }
  void process(const ProcessArgs& a) override {
    const float *fc = a.input(1, 0), *rs = a.input(2, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      if (fc[i] != lastFc_) { lastFc_ = fc[i]; for (auto& f : f_) f.setCutoff(lastFc_); }
      if (rs[i] != lastRes_) { lastRes_ = rs[i]; for (auto& f : f_) f.setResonance(lastRes_); }
      for (size_t c = 0; c < f_.size(); ++c)
        a.output(0, static_cast<int>(c))[i] = f_[c].process(a.input(0, static_cast<int>(c))[i]);
    }
  }
 private:
  double drive_;
  int oversample_;
  std::vector<dsp::Ladder> f_;
  float lastFc_ = -1.f, lastRes_ = -1.f;
};

// State-variable filter: input 0 = signal (N ch), 1 = cutoff Hz, 2 = Q.
class ModSvfNode final : public Node {
 public:
  enum class Mode { Low, Band, High, Notch };
  ModSvfNode(int channels, Mode m) : Node({channels, 1, 1}, {channels}), mode_(m), f_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : f_) { f.prepare(sr); f.reset(); }
    lastFc_ = lastQ_ = -1.f;
  }
  void process(const ProcessArgs& a) override {
    const float *fc = a.input(1, 0), *q = a.input(2, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      if (fc[i] != lastFc_) { lastFc_ = fc[i]; for (auto& f : f_) f.setCutoff(lastFc_); }
      if (q[i] != lastQ_) { lastQ_ = q[i]; for (auto& f : f_) f.setQ(lastQ_); }
      for (size_t c = 0; c < f_.size(); ++c) {
        const auto o = f_[c].process(a.input(0, static_cast<int>(c))[i]);
        a.output(0, static_cast<int>(c))[i] =
            mode_ == Mode::Low ? o.low : mode_ == Mode::Band ? o.band : mode_ == Mode::High ? o.high : o.notch;
      }
    }
  }
 private:
  Mode mode_;
  std::vector<dsp::Svf> f_;
  float lastFc_ = -1.f, lastQ_ = -1.f;
};

// ADSR: inputs 0 = gate, 1 = trigger (retrigger while the gate stays open), 2..5 = attack, decay (s),
// sustain (0..1), release (s). Times may be modulated; coefficients are recomputed only when they change.
class ModEnvelopeNode final : public Node {
 public:
  ModEnvelopeNode() : Node({1, 1, 1, 1, 1, 1}, {1}) {}
  void prepare(double sr, int) override { env_.prepare(sr); env_.reset(); open_ = false; last_[0] = -1.f; }
  void process(const ProcessArgs& a) override {
    const float *gate = a.input(0, 0), *trig = a.input(1, 0);
    const float *at = a.input(2, 0), *de = a.input(3, 0), *su = a.input(4, 0), *re = a.input(5, 0);
    float* out = a.output(0, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      if (at[i] != last_[0] || de[i] != last_[1] || su[i] != last_[2] || re[i] != last_[3]) {
        last_[0] = at[i]; last_[1] = de[i]; last_[2] = su[i]; last_[3] = re[i];
        env_.set(std::max(0.f, at[i]), std::max(0.f, de[i]), std::clamp(su[i], 0.f, 1.f), std::max(0.f, re[i]));
      }
      const bool g = gate[i] > 0.f;
      if (trig[i] > 0.f || (g && !open_)) env_.noteOn();
      else if (!g && open_) env_.noteOff();
      open_ = g;
      out[i] = env_.process();
    }
  }
 private:
  dsp::Adsr env_;
  float last_[4] = {-1.f, -1.f, -1.f, -1.f};
  bool open_ = false;
};

// Feedback delay with fixed time, N channels. mix = wet amount (0..1), dry = 1 - mix.
class DelayNode final : public Node {
 public:
  DelayNode(int channels, double seconds, float feedback, float mix)
      : Node({channels}, {channels}), seconds_(seconds), fb_(feedback), mix_(mix) {}
  void prepare(double sr, int) override {
    len_ = std::max<size_t>(1, static_cast<size_t>(seconds_ * sr));
    lines_.assign(static_cast<size_t>(outputChannels()[0]), std::vector<float>(len_, 0.f));
    pos_ = 0;
  }
  void process(const ProcessArgs& a) override {
    size_t pos = pos_;
    for (size_t c = 0; c < lines_.size(); ++c) {
      pos = pos_;
      const float* in = a.input(0, static_cast<int>(c));
      float* out = a.output(0, static_cast<int>(c));
      auto& line = lines_[c];
      for (int i = 0; i < a.numFrames; ++i) {
        const float wet = line[pos];
        line[pos] = in[i] + wet * fb_;
        out[i] = in[i] * (1.f - mix_) + wet * mix_;
        if (++pos == len_) pos = 0;
      }
    }
    pos_ = pos;
  }
 private:
  double seconds_;
  float fb_, mix_;
  size_t len_ = 1, pos_ = 0;
  std::vector<std::vector<float>> lines_;
};

// One-pole lag smoother (Tonic 1 .smoothed()). Inputs: 0 = signal (N ch), 1 = time constant (s).
class SmoothNode final : public Node {
 public:
  explicit SmoothNode(int channels) : Node({channels, 1}, {channels}), y_(static_cast<size_t>(channels), 0.f) {}
  void prepare(double sr, int) override { sr_ = sr; lastT_ = -1.f; primed_ = false; }
  void process(const ProcessArgs& a) override {
    const float* t = a.input(1, 0);
    if (!primed_) { for (size_t c = 0; c < y_.size(); ++c) y_[c] = a.input(0, static_cast<int>(c))[0]; primed_ = true; }
    for (int i = 0; i < a.numFrames; ++i) {
      if (t[i] != lastT_) { lastT_ = t[i]; k_ = t[i] > 0.f ? 1.f - static_cast<float>(std::exp(-1.0 / (t[i] * sr_))) : 1.f; }
      for (size_t c = 0; c < y_.size(); ++c) {
        y_[c] += k_ * (a.input(0, static_cast<int>(c))[i] - y_[c]);
        a.output(0, static_cast<int>(c))[i] = y_[c];
      }
    }
  }
 private:
  std::vector<float> y_;
  double sr_ = 48000.0;
  float lastT_ = -1.f, k_ = 1.f;
  bool primed_ = false;
};

// Snap a MIDI note signal to the nearest note of a scale (Tonic 1 ControlSnapToScale). Scale degrees are
// semitones 0..11 above `root`; ties go down. Input 0 = MIDI note (fractional allowed).
class SnapToScaleNode final : public Node {
 public:
  SnapToScaleNode(std::vector<int> degrees, int root) : Node({1}, {1}), root_(root) {
    for (int d : degrees) allowed_[static_cast<size_t>(((d % 12) + 12) % 12)] = true;
    bool any = false;
    for (bool b : allowed_) any |= b;
    if (!any) allowed_.fill(true);
  }
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = snap(a.input(0, 0)[i]);
  }
 private:
  float snap(float note) const {
    const int base = static_cast<int>(std::floor(note));
    float best = static_cast<float>(base), bestDist = 1e9f;
    for (int n = base - 6; n <= base + 7; ++n) {
      const int pc = (((n - root_) % 12) + 12) % 12;
      const float dist = std::fabs(note - static_cast<float>(n));
      if (allowed_[static_cast<size_t>(pc)] && dist < bestDist) { bestDist = dist; best = static_cast<float>(n); }
    }
    return best;
  }
  std::array<bool, 12> allowed_{};
  int root_;
};

// Select one of N mono inputs by index (Tonic 1 ControlSwitcher). Input 0 = index (rounded, clamped),
// inputs 1..N = choices.
class SelectNode final : public Node {
 public:
  explicit SelectNode(int n) : Node(std::vector<int>(static_cast<size_t>(n) + 1, 1), {1}), n_(n) {}
  void process(const ProcessArgs& a) override {
    for (int i = 0; i < a.numFrames; ++i) {
      const int k = std::clamp(static_cast<int>(std::lround(a.input(0, 0)[i])), 0, n_ - 1);
      a.output(0, 0)[i] = a.input(k + 1, 0)[i];
    }
  }
 private:
  int n_;
};

}  // namespace tonic::graph
