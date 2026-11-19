// Signal nodes: every setting is an input port, so anything can modulate anything (the Tonic 1
// model). These are what the patching syntax (tonic/dsl) builds. Inputs that carry settings are mono;
// coefficients are recomputed only when the incoming value changes, so constant settings cost nothing.
#pragma once

#include <algorithm>
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
  enum class Op { Add, Sub, Mul, Div };
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

// Ladder: input 0 = signal (N ch), 1 = cutoff Hz, 2 = resonance 0..1.
class ModLadderNode final : public Node {
 public:
  ModLadderNode(int channels, double drive) : Node({channels, 1, 1}, {channels}), drive_(drive), f_(static_cast<size_t>(channels)) {}
  void prepare(double sr, int) override {
    for (auto& f : f_) { f.prepare(sr); f.setDrive(drive_); f.reset(); }
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

// ADSR: input 0 = gate, input 1 = trigger (retrigger while the gate stays open).
class ModEnvelopeNode final : public Node {
 public:
  ModEnvelopeNode(double a, double d, double s, double r) : Node({1, 1}, {1}), a_(a), d_(d), s_(s), r_(r) {}
  void prepare(double sr, int) override { env_.prepare(sr); env_.set(a_, d_, s_, r_); env_.reset(); open_ = false; }
  void process(const ProcessArgs& a) override {
    const float *gate = a.input(0, 0), *trig = a.input(1, 0);
    float* out = a.output(0, 0);
    for (int i = 0; i < a.numFrames; ++i) {
      const bool g = gate[i] > 0.f;
      if (trig[i] > 0.f || (g && !open_)) env_.noteOn();
      else if (!g && open_) env_.noteOff();
      open_ = g;
      out[i] = env_.process();
    }
  }
 private:
  dsp::Adsr env_;
  double a_, d_, s_, r_;
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

}  // namespace tonic::graph
