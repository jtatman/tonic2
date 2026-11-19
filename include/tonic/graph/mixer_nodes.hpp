// Mixer building blocks: a channel strip (gain, pan, mute with click-free fades) and a meter tap.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "../dsp/math.hpp"
#include "node.hpp"

namespace tonic::graph {

enum class PanLaw {
  ConstantPower,  // -3 dB at centre (sin/cos)
  Linear,         // -6 dB at centre
  Compromise,     // -4.5 dB at centre (geometric mean of the two above)
};

// Channel strip. Inputs: 0 = signal (mono or stereo), 1 = linear gain, 2 = pan -1..1, 3 = mute 0..1.
// Outputs: 0 = post-fader stereo, 1 = pre-fader stereo (panned, before gain and mute).
// Mono inputs are panned with the chosen law; stereo inputs use pan as balance.
// Mute is smoothed over ~5 ms so muting and soloing never click.
class StripNode final : public Node {
 public:
  StripNode(int inChannels, PanLaw law) : Node({inChannels, 1, 1, 1}, {2, 2}), law_(law) {}
  void prepare(double sr, int) override {
    muteCoef_ = 1.f - static_cast<float>(std::exp(-1.0 / (0.005 * sr)));
    muteGain_ = 1.f;
    lastPan_ = -2.f;  // forces the first computation
  }
  void process(const ProcessArgs& a) override {
    const bool stereo = inputChannels()[0] == 2;
    const float* l = a.input(0, 0);
    const float* r = stereo ? a.input(0, 1) : l;
    const float *gain = a.input(1, 0), *pan = a.input(2, 0), *mute = a.input(3, 0);
    float *postL = a.output(0, 0), *postR = a.output(0, 1), *preL = a.output(1, 0), *preR = a.output(1, 1);
    for (int i = 0; i < a.numFrames; ++i) {
      if (pan[i] != lastPan_) {  // trig only when pan moves
        lastPan_ = pan[i];
        panGains(std::clamp(lastPan_, -1.f, 1.f), stereo, gl_, gr_);
      }
      preL[i] = l[i] * gl_;
      preR[i] = r[i] * gr_;
      const float target = 1.f - std::clamp(mute[i], 0.f, 1.f);
      muteGain_ += muteCoef_ * (target - muteGain_);
      const float g = gain[i] * muteGain_;
      postL[i] = preL[i] * g;
      postR[i] = preR[i] * g;
    }
  }

 private:
  void panGains(float p, bool stereo, float& gl, float& gr) const {
    if (stereo) {  // balance
      gl = std::min(1.f, 1.f - p);
      gr = std::min(1.f, 1.f + p);
      return;
    }
    const float a = (p + 1.f) * 0.25f * static_cast<float>(dsp::kPi);
    const float cl = std::cos(a), cr = std::sin(a);
    const float ll = (1.f - p) * 0.5f, lr = (1.f + p) * 0.5f;
    switch (law_) {
      case PanLaw::ConstantPower: gl = cl; gr = cr; break;
      case PanLaw::Linear: gl = ll; gr = lr; break;
      case PanLaw::Compromise: gl = std::sqrt(cl * ll); gr = std::sqrt(cr * lr); break;
    }
  }
  PanLaw law_;
  float muteCoef_ = 1.f, muteGain_ = 1.f, lastPan_ = -2.f, gl_ = 0.f, gr_ = 0.f;
};

// Peak meter readable from any thread. The audio thread raises `peak`; the UI reads and resets it.
struct Meter {
  std::atomic<float> peak{0.f};
  float readAndReset() { return peak.exchange(0.f, std::memory_order_relaxed); }
};

// Passes audio through unchanged and records its peak into a shared Meter.
class MeterNode final : public Node {
 public:
  MeterNode(int channels, std::shared_ptr<Meter> meter) : Node({channels}, {channels}), meter_(std::move(meter)) {}
  void process(const ProcessArgs& a) override {
    float block = 0.f;
    for (int c = 0; c < outputChannels()[0]; ++c) {
      const float* in = a.input(0, c);
      float* out = a.output(0, c);
      for (int i = 0; i < a.numFrames; ++i) { out[i] = in[i]; block = std::max(block, std::fabs(in[i])); }
    }
    // Single writer: a lost update against the UI's reset only shows a slightly stale peak.
    const float prev = meter_->peak.load(std::memory_order_relaxed);
    if (block > prev) meter_->peak.store(block, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<Meter> meter_;
};

}  // namespace tonic::graph
