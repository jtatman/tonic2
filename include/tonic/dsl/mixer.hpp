// Mixer and FX matrix on top of the patching syntax. Both build ordinary graph nodes, so every
// setting (gain, pan, mute, solo, send level, matrix cell) can be a parameter, macro or signal.
//
//   dsl::Mixer mix(p);
//   auto fx    = mix.fxReturn([](dsl::Signal in) { return in >> dsl::delay(0.3, 0.4f, 1.f); });
//   auto keys  = mix.group({.gainDb = -3.f});
//   auto pad   = mix.strip(padSig,  {.gainDb = -6.f, .pan = -0.3f}, &keys);
//   auto lead  = mix.strip(leadSig, {.gainDb = gainParam, .solo = soloParam});
//   mix.send(lead, fx, 0.3f);
//   p.output(mix.master());
//
// Signal flow per strip: input -> pan (law for mono, balance for stereo) -> [pre-fader send tap] ->
// gain (dB) and mute (5 ms fade) -> [post-fader sends] -> meter -> group or master.
// Solo: if any strip or group is soloed, the others are muted (smoothly). FX returns are solo-safe.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "../graph/mixer_nodes.hpp"
#include "patch.hpp"

namespace tonic::dsl {

using graph::Meter;
using graph::PanLaw;

struct StripSettings {
  Value gainDb = 0.f;
  Value pan = 0.f;
  Value mute = 0.f;  // 0..1 (1 = muted)
  Value solo = 0.f;  // 0..1 (1 = soloed)
  PanLaw law = PanLaw::ConstantPower;
};

class Mixer {
 public:
  struct Strip {
    Signal post;  // post-fader stereo (after meter)
    Signal pre;   // pre-fader stereo (panned)
    std::shared_ptr<Meter> meter;
  };
  struct Bus {
    Signal input;  // summing point (stereo)
    Strip strip;
  };

  explicit Mixer(Patch& p) : p_(p) {
    masterSum_ = p_.summingPoint(2);
    soloSum_ = p_.summingPoint(1);
    anySolo_ = min(soloSum_, 1.f);
  }

  // A channel strip feeding `dest` (a group) or the master.
  Strip strip(Signal in, const StripSettings& s = {}, const Bus* dest = nullptr) {
    Strip st = build(in, s, /*soloable=*/true);
    p_.route(st.post, dest ? dest->input : masterSum_, 0);
    return st;
  }

  // A group bus: strips routed to it are summed, then pass through its own strip (soloable).
  Bus group(const StripSettings& s = {}, const Bus* dest = nullptr) {
    Bus b;
    b.input = p_.summingPoint(2);
    b.strip = build(b.input, s, true);
    p_.route(b.strip.post, dest ? dest->input : masterSum_, 0);
    return b;
  }

  // An effect return: sends are summed into it, run through `effect`, then a solo-safe strip.
  Bus fxReturn(const std::function<Signal(Signal)>& effect, const StripSettings& s = {}) {
    Bus b;
    b.input = p_.summingPoint(2);
    Signal wet = effect(b.input);
    if (wet.channels() != 2) wet = wet >> pan(0.f);  // mono effect output -> centre
    b.strip = build(wet, s, false);
    p_.route(b.strip.post, masterSum_, 0);
    return b;
  }

  void send(const Strip& from, const Bus& to, Value level, bool preFader = false) {
    p_.route((preFader ? from.pre : from.post) * p_.toSignal(level), to.input, 0);
  }

  // The master strip (no solo). Call once, after all strips; route the result to the patch output.
  Signal master(const StripSettings& s = {}) {
    StripSettings m = s;
    m.solo = 0.f;
    masterStrip_ = build(masterSum_, m, false);
    return masterStrip_.post;
  }
  std::shared_ptr<Meter> masterMeter() const { return masterStrip_.meter; }

 private:
  Strip build(Signal in, const StripSettings& s, bool soloable) {
    if (in.channels() != 1 && in.channels() != 2) {
      p_.fail("mixer strips take mono or stereo signals");
      in = p_.constant(0.f);
    }
    Signal mute = p_.toSignal(s.mute);
    if (soloable) {
      Signal solo = p_.toSignal(s.solo);
      p_.route(solo, soloSum_, 0);
      // Muted if muted, or if something else is soloed and this strip isn't.
      mute = max(mute, anySolo_ * (1.f - solo));
    }
    Signal strip = p_.addNode(std::make_unique<graph::StripNode>(in.channels(), s.law), 2);
    p_.route(in, strip, 0);
    p_.route(p_.dbToGain(p_.toSignal(s.gainDb)), strip, 1);
    p_.route(p_.toSignal(s.pan), strip, 2);
    p_.route(mute, strip, 3);
    Strip out;
    out.pre = p_.port(strip, 1, 2);
    out.meter = std::make_shared<Meter>();
    out.post = p_.addNode(std::make_unique<graph::MeterNode>(2, out.meter), 2);
    p_.route(p_.port(strip, 0, 2), out.post, 0);
    return out;
  }

  Patch& p_;
  Signal masterSum_, soloSum_, anySolo_;
  Strip masterStrip_;
};

// ---- FX matrix: K effect slots with a feed-forward routing matrix ----
// Each route is (from, to, gain): from = kFxIn or a slot index, to = a slot index or kFxOut.
// Routes may only go forward (slot i -> slot j with j > i), so any serial/parallel mix is possible
// without feedback loops. Gains are Values: drive them from parameters for click-free crossfades.
inline constexpr int kFxIn = -1;
inline constexpr int kFxOut = -2;

struct FxRoute {
  int from;
  int to;
  Value gain = 1.f;
};

inline Signal fxMatrix(Signal in, const std::vector<std::function<Signal(Signal)>>& slots,
                       const std::vector<FxRoute>& routes) {
  Patch* p = in.patch();
  if (!p) return in;
  const int k = static_cast<int>(slots.size());
  for (const FxRoute& r : routes) {
    const bool fromOk = r.from == kFxIn || (r.from >= 0 && r.from < k);
    const bool toOk = r.to == kFxOut || (r.to >= 0 && r.to < k);
    if (!fromOk || !toOk) { p->fail("fxMatrix: route references an unknown slot"); return in; }
    if (r.from >= 0 && r.to >= 0 && r.to <= r.from) {
      p->fail("fxMatrix: routes must go forward (slot i -> slot j with j > i)");
      return in;
    }
  }
  const int ch = in.channels();
  std::vector<Signal> outs(static_cast<size_t>(k));
  for (int j = 0; j < k; ++j) {
    Signal sum = p->summingPoint(ch);
    bool fed = false;
    for (const FxRoute& r : routes) {
      if (r.to != j) continue;
      const Signal& src = r.from == kFxIn ? in : outs[static_cast<size_t>(r.from)];
      if (!src.valid()) { p->fail("fxMatrix: route from a slot that has no input"); return in; }
      p->route(src * p->toSignal(r.gain), sum, 0);
      fed = true;
    }
    if (!fed) continue;  // unused slot: not built
    outs[static_cast<size_t>(j)] = slots[static_cast<size_t>(j)](sum);
  }
  Signal out = p->summingPoint(ch);
  for (const FxRoute& r : routes) {
    if (r.to != kFxOut) continue;
    const Signal& src = r.from == kFxIn ? in : outs[static_cast<size_t>(r.from)];
    if (!src.valid()) { p->fail("fxMatrix: routes to the output from a slot that has no input"); return in; }
    p->route(src * p->toSignal(r.gain), out, 0);
  }
  return out;
}

}  // namespace tonic::dsl
