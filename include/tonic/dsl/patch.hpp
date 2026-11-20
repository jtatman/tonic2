// Patching syntax: write synths as expressions, the way Tonic 1 did, on top of the realtime-safe
// graph and engine. Everything here runs on the control thread and only builds a graph description;
// compile() turns it into a CompiledGraph plus the parameter Layout the engine needs.
//
//   dsl::Patch p;
//   auto cutoff = p.param("cutoff", 100, 8000, 800);
//   auto env    = p.adsr(0.005, 0.2, 0.6, 0.3);
//   auto osc    = p.saw(p.pitch()) + p.square(p.pitch() * 0.5f) * 0.5f;
//   p.output(osc * env >> dsl::ladder(cutoff + env * 3000, 0.4) >> dsl::pan(0.2f));
//   auto c = p.compile(48000, 512);   // c.graph, c.layout, c.error
//
// Precedence: `>>` binds looser than + and *, so `a + b >> fx` applies fx to the sum. Write
// `(a + b) >> fx` to make that explicit (GCC warns otherwise).
// Signals are cheap handles to node outputs; using one twice fans out (the node runs once).
// A float anywhere a signal is accepted becomes a constant. Settings of effects are signals too, so
// any of them can be modulated at audio rate.
#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../engine/params.hpp"
#include "../graph/graph.hpp"
#include "../graph/nodes.hpp"
#include "../graph/signal_nodes.hpp"
#include "../graph/voice_node.hpp"

namespace tonic::dsl {

class Patch;

class Signal {
 public:
  Signal() = default;
  bool valid() const { return patch_ != nullptr; }
  int channels() const { return channels_; }
  Patch* patch() const { return patch_; }

 private:
  friend class Patch;
  Signal(Patch* p, graph::NodeId n, int port, int ch) : patch_(p), node_(n), port_(port), channels_(ch) {}
  Patch* patch_ = nullptr;
  graph::NodeId node_ = -1;
  int port_ = 0;
  int channels_ = 1;
};

// A parameter is a signal that also knows its engine parameter id.
struct Param : Signal {
  int id = -1;
  float def = 0.f;
};

using graph::VoiceConfig;
using graph::VoiceMode;

// A constant or a signal (accepted wherever a setting can be modulated).
struct Value {
  Value(float v) : constant(v) {}
  Value(double v) : constant(static_cast<float>(v)) {}
  Value(int v) : constant(static_cast<float>(v)) {}
  Value(Signal s) : isSignal(true), signal(s) {}
  bool isSignal = false;
  float constant = 0.f;
  Signal signal;
};

struct MacroTarget {
  Param target;
  float lo = 0.f, hi = 0.f, curve = 1.f;
};

struct Compiled {
  std::unique_ptr<graph::CompiledGraph> graph;  // null on error
  engine::Layout layout;
  std::string error;
};

class Patch {
 public:
  Patch() = default;
  Patch(const Patch&) = delete;  // signals point at their patch
  Patch& operator=(const Patch&) = delete;

  // --- parameters and macros ---
  Param param(std::string name, float min, float max, float def, float smoothSeconds = 0.02f) {
    engine::ParamSpec spec;
    spec.name = std::move(name);
    spec.min = min;
    spec.max = max;
    spec.def = def;
    spec.smoothSeconds = smoothSeconds;
    return param(std::move(spec));
  }
  // Full spec: unit, log scale and UI placement as well.
  Param param(engine::ParamSpec spec) {
    if (isVoice_) { fail("declare parameters on the outer patch and read them in a voice with use()"); return {}; }
    Param p;
    p.def = spec.def;
    const float def = spec.def;
    p.id = layout_.add(std::move(spec));
    static_cast<Signal&>(p) = make(std::make_unique<graph::ParamSignalNode>(p.id, def), 1);
    return p;
  }
  // Bind MIDI CC `cc` to a parameter (often a macro): CC 0..127 sweeps the parameter's range.
  void midiCc(int cc, const Param& target) {
    if (isVoice_) { fail("declare MIDI CC bindings on the outer patch"); return; }
    layout_.ccBindings.push_back({cc, target.id});
  }
  void macro(const Param& m, std::vector<MacroTarget> targets) {
    if (isVoice_) { fail("declare macros on the outer patch"); return; }
    engine::MacroSpec spec{m.id, {}};
    for (const auto& t : targets) spec.targets.push_back({t.target.id, t.lo, t.hi, t.curve});
    layout_.macros.push_back(std::move(spec));
  }

  // --- note control signals (mono until M5 voices) ---
  Signal pitch() { return control(graph::ControlSignalNode::Kind::Pitch, 440.f); }
  Signal gate() { return control(graph::ControlSignalNode::Kind::Gate, 0.f); }
  Signal trigger() { return control(graph::ControlSignalNode::Kind::Trigger, 0.f); }
  Signal velocity() { return control(graph::ControlSignalNode::Kind::Velocity, 0.f); }
  Signal pressure() { return control(graph::ControlSignalNode::Kind::Pressure, 0.f); }  // per-note, inside voices
  Signal timbre() { return control(graph::ControlSignalNode::Kind::Timbre, 0.f); }      // per-note, inside voices

  // --- voices ---
  // Calls `build` once per voice right away (so lambdas may capture locals by reference) and sums
  // the voices; the voice patches are compiled when the outer patch is compiled. Inside the voice, pitch(),
  // gate(), trigger(), velocity(), pressure() and timbre() are per voice; read outer parameters with
  // use(param). Effects applied to the returned signal are shared by all voices.
  Signal voices(VoiceConfig cfg, std::function<void(Patch&)> build) {
    if (isVoice_) { fail("voices cannot be nested"); return constant(0.f); }
    auto pool = std::make_shared<std::vector<std::unique_ptr<Patch>>>();
    for (int i = 0; i < std::max(1, cfg.voices); ++i) {
      pool->push_back(std::unique_ptr<Patch>(new Patch(VoiceTag{})));
      build(*pool->back());
    }
    const Patch& first = *pool->front();
    if (!first.error_.empty()) { fail("in voice: " + first.error_); return constant(0.f); }
    if (!first.hasOutput_) { fail("voice has no output"); return constant(0.f); }
    const int ch = first.outChannels_;
    auto next = std::make_shared<size_t>(0);
    auto factory = [pool, next](double sr, int maxBlock) -> std::unique_ptr<graph::CompiledGraph> {
      if (*next >= pool->size()) return nullptr;
      return (*pool)[(*next)++]->compile(sr, maxBlock).graph;
    };
    return make(std::make_unique<graph::VoiceNode>(cfg, ch, std::move(factory)), ch);
  }
  // Inside a voice: a per-sample signal for a parameter declared on the outer patch.
  Signal use(const Param& p) { return make(std::make_unique<graph::ParamSignalNode>(p.id, p.def), 1); }

  // --- sources ---
  Signal constant(float v) { return make(std::make_unique<graph::ConstantNode>(v), 1); }
  Signal sine(Value hz) { return osc(dsp::Waveform::Sine, hz, 0.5f); }
  Signal saw(Value hz) { return osc(dsp::Waveform::Saw, hz, 0.5f); }
  Signal square(Value hz, Value pulseWidth = 0.5f) { return osc(dsp::Waveform::Square, hz, pulseWidth); }
  Signal triangle(Value hz) { return osc(dsp::Waveform::Triangle, hz, 0.5f); }
  Signal noise(uint32_t seed = 1) { return make(std::make_unique<graph::NoiseNode>(seed), 1); }
  // Envelope driven by the played notes (gate + retrigger).
  Signal adsr(double attack, double decay, double sustain, double release) {
    return adsr(attack, decay, sustain, release, gate(), trigger());
  }
  Signal adsr(double attack, double decay, double sustain, double release, Signal gateSig, Signal triggerSig) {
    Signal e = make(std::make_unique<graph::ModEnvelopeNode>(attack, decay, sustain, release), 1);
    connect(gateSig, e, 0);
    connect(triggerSig, e, 1);
    return e;
  }

  // --- functions and mixing ---
  Signal mtof(Signal midi) { return unary(graph::UnaryOpNode::Op::MidiToHz, midi); }
  Signal dbToGain(Signal db) { return unary(graph::UnaryOpNode::Op::DbToGain, db); }
  Signal tanh(Signal x) { return unary(graph::UnaryOpNode::Op::Tanh, x); }
  // Sum of signals (one summing input, no chain of adders).
  Signal mix(const std::vector<Signal>& signals) {
    int ch = 1;
    for (const auto& s : signals) ch = std::max(ch, s.channels());
    Signal m = make(std::make_unique<graph::GainNode>(ch, 1.f), ch);
    for (const auto& s : signals) connect(s, m, 0);
    return m;
  }

  void output(Signal s) {
    if (!own(s)) return;
    builder_.setOutput(s.node_, s.port_);
    hasOutput_ = true;
    outChannels_ = s.channels();
  }

  Compiled compile(double sampleRate, int maxBlock) {
    Compiled c;
    if (compiled_) { c.error = "patch already compiled"; return c; }
    compiled_ = true;
    if (!error_.empty()) { c.error = error_; return c; }
    if (!hasOutput_) { c.error = "no output set"; return c; }
    auto r = builder_.compile(sampleRate, maxBlock);
    c.graph = std::move(r.graph);
    c.error = std::move(r.error);
    c.layout = layout_;
    return c;
  }

  const std::string& error() const { return error_; }

  // --- used by operators and effects ---
  Signal toSignal(const Value& v) { return v.isSignal ? (own(v.signal) ? v.signal : constant(0.f)) : constant(v.constant); }
  Signal binary(graph::BinaryOpNode::Op op, Signal a, Signal b) {
    if (!own(a) || !own(b)) return a;
    if (a.channels() != b.channels() && a.channels() != 1 && b.channels() != 1) {
      fail("channel mismatch in arithmetic (" + std::to_string(a.channels()) + " vs " + std::to_string(b.channels()) + ")");
      return a;
    }
    const int ch = std::max(a.channels(), b.channels());
    Signal o = make(std::make_unique<graph::BinaryOpNode>(op, ch), ch);
    connect(a, o, 0);
    connect(b, o, 1);
    return o;
  }
  // Adds a node whose input 0 is `in` and whose inputs 1.. are mono settings.
  Signal effect(std::unique_ptr<graph::Node> node, Signal in, std::initializer_list<Value> settings, int outChannels) {
    if (!own(in)) return in;
    Signal o = make(std::move(node), outChannels);
    connect(in, o, 0);
    int port = 1;
    for (const Value& v : settings) {
      Signal s = toSignal(v);
      if (s.channels() != 1) { fail("effect settings must be mono signals"); return in; }
      connect(s, o, port++);
    }
    return o;
  }
  void fail(const std::string& msg) { if (error_.empty()) error_ = msg; }

  // --- advanced: building blocks for Mixer, fxMatrix and custom nodes ---
  Signal addNode(std::unique_ptr<graph::Node> n, int channels) { return make(std::move(n), channels); }
  // Another output port of the node behind `s`.
  Signal port(const Signal& s, int portIndex, int channels) { return own(s) ? Signal(this, s.node_, portIndex, channels) : s; }
  // Connect `from` into input `portIndex` of the node behind `to` (several connections are summed).
  void route(const Signal& from, const Signal& to, int portIndex) { if (own(to)) connect(from, to, portIndex); }
  // A summing point: route any number of signals into it with route(sig, sum, 0).
  Signal summingPoint(int channels) { return make(std::make_unique<graph::GainNode>(channels, 1.f), channels); }

 private:
  struct VoiceTag {};
  explicit Patch(VoiceTag) : isVoice_(true) {}

  bool own(const Signal& s) {
    if (s.patch_ == this) return true;
    fail(s.valid() ? "signal belongs to a different patch" : "use of an empty signal");
    return false;
  }
  Signal make(std::unique_ptr<graph::Node> n, int channels) {
    const graph::NodeId id = builder_.add(std::move(n));
    return Signal(this, id, 0, channels);
  }
  void connect(const Signal& from, const Signal& to, int port) {
    if (own(from)) builder_.connect(from.node_, from.port_, to.node_, port);
  }
  Signal control(graph::ControlSignalNode::Kind k, float fallback) {
    Signal& cached = controls_[static_cast<int>(k)];
    if (!cached.valid()) cached = make(std::make_unique<graph::ControlSignalNode>(k, fallback), 1);
    return cached;
  }
  Signal osc(dsp::Waveform w, const Value& hz, const Value& pw) {
    Signal o = make(std::make_unique<graph::ModOscillatorNode>(w), 1);
    Signal f = toSignal(hz), p = toSignal(pw);
    if (f.channels() != 1 || p.channels() != 1) fail("oscillator settings must be mono signals");
    connect(f, o, 0);
    connect(p, o, 1);
    return o;
  }
  Signal unary(graph::UnaryOpNode::Op op, Signal x) {
    if (!own(x)) return x;
    Signal o = make(std::make_unique<graph::UnaryOpNode>(op, x.channels()), x.channels());
    connect(x, o, 0);
    return o;
  }

  graph::GraphBuilder builder_;
  engine::Layout layout_;
  Signal controls_[static_cast<int>(graph::ControlSignalNode::Kind::Timbre) + 1];  // one cached signal per kind
  std::string error_;
  bool hasOutput_ = false, compiled_ = false, isVoice_ = false;
  int outChannels_ = 1;
};

// ---- arithmetic ----
inline Signal operator+(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Add, a, b) : a; }
inline Signal operator-(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Sub, a, b) : a; }
inline Signal operator*(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Mul, a, b) : a; }
inline Signal operator/(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Div, a, b) : a; }
inline Signal operator+(Signal a, float b) { return a.patch() ? a + a.patch()->constant(b) : a; }
inline Signal operator-(Signal a, float b) { return a.patch() ? a - a.patch()->constant(b) : a; }
inline Signal operator*(Signal a, float b) { return a.patch() ? a * a.patch()->constant(b) : a; }
inline Signal operator/(Signal a, float b) { return a.patch() ? a / a.patch()->constant(b) : a; }
inline Signal operator+(float a, Signal b) { return b.patch() ? b.patch()->constant(a) + b : b; }
inline Signal operator-(float a, Signal b) { return b.patch() ? b.patch()->constant(a) - b : b; }
inline Signal operator*(float a, Signal b) { return b.patch() ? b.patch()->constant(a) * b : b; }
inline Signal operator/(float a, Signal b) { return b.patch() ? b.patch()->constant(a) / b : b; }
inline Signal operator-(Signal a) { return a * -1.f; }
inline Signal min(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Min, a, b) : a; }
inline Signal max(Signal a, Signal b) { return a.patch() ? a.patch()->binary(graph::BinaryOpNode::Op::Max, a, b) : a; }
inline Signal min(Signal a, float b) { return a.patch() ? min(a, a.patch()->constant(b)) : a; }
inline Signal max(Signal a, float b) { return a.patch() ? max(a, a.patch()->constant(b)) : a; }

// ---- effects: describe settings, then apply with `signal >> effect` ----
struct Ladder {
  Value cutoff, resonance;
  double drive;
  int oversample;
  Signal apply(Signal in) const {
    return in.patch()->effect(std::make_unique<graph::ModLadderNode>(in.channels(), drive, oversample), in, {cutoff, resonance},
                              in.channels());
  }
};
// oversample: 1, 2 or 4 (worth it when drive or resonance pushes the saturation; adds a little latency).
inline Ladder ladder(Value cutoff, Value resonance = 0.f, double drive = 1.0, int oversample = 1) {
  return {cutoff, resonance, drive, oversample};
}

struct Svf {
  graph::ModSvfNode::Mode mode;
  Value cutoff, q;
  Signal apply(Signal in) const {
    return in.patch()->effect(std::make_unique<graph::ModSvfNode>(in.channels(), mode), in, {cutoff, q}, in.channels());
  }
};
inline Svf lowpass(Value cutoff, Value q = 0.70710678f) { return {graph::ModSvfNode::Mode::Low, cutoff, q}; }
inline Svf highpass(Value cutoff, Value q = 0.70710678f) { return {graph::ModSvfNode::Mode::High, cutoff, q}; }
inline Svf bandpass(Value cutoff, Value q = 0.70710678f) { return {graph::ModSvfNode::Mode::Band, cutoff, q}; }
inline Svf notch(Value cutoff, Value q = 0.70710678f) { return {graph::ModSvfNode::Mode::Notch, cutoff, q}; }

struct Pan {
  float position;
  Signal apply(Signal in) const {
    if (in.channels() != 1) { in.patch()->fail("pan expects a mono signal"); return in; }
    return in.patch()->effect(std::make_unique<graph::PanNode>(position), in, {}, 2);
  }
};
inline Pan pan(float position) { return {position}; }

struct Delay {
  double seconds;
  float feedback, mix;
  Signal apply(Signal in) const {
    return in.patch()->effect(std::make_unique<graph::DelayNode>(in.channels(), seconds, feedback, mix), in, {}, in.channels());
  }
};
inline Delay delay(double seconds, float feedback = 0.3f, float mix = 0.3f) { return {seconds, feedback, mix}; }

template <class E>
concept Effect = requires(const E& e, Signal s) {
  { e.apply(s) } -> std::same_as<Signal>;
};

template <Effect E>
Signal operator>>(Signal in, const E& e) {
  return in.patch() ? e.apply(in) : in;
}

}  // namespace tonic::dsl
