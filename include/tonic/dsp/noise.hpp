// Deterministic white noise (xorshift32). Same seed -> same output on every platform and port.
#pragma once

#include <cstdint>

namespace tonic::dsp {

struct WhiteNoise {
  explicit WhiteNoise(uint32_t seed = 0x9E3779B9u) : state_(seed ? seed : 1u) {}
  void seed(uint32_t s) { state_ = s ? s : 1u; }
  float process() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    // Top 24 bits -> [-1, 1)
    return static_cast<float>(static_cast<int32_t>(state_ >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
  }

 private:
  uint32_t state_;
};

// Pink noise (-3 dB/octave), Paul Kellet's refined filter over deterministic white noise.
struct PinkNoise {
  explicit PinkNoise(uint32_t seed = 0x2545F491u) : white_(seed) {}
  void seed(uint32_t s) { white_.seed(s); b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = 0.f; }
  float process() {
    const float w = white_.process();
    b0_ = 0.99886f * b0_ + w * 0.0555179f;
    b1_ = 0.99332f * b1_ + w * 0.0750759f;
    b2_ = 0.96900f * b2_ + w * 0.1538520f;
    b3_ = 0.86650f * b3_ + w * 0.3104856f;
    b4_ = 0.55000f * b4_ + w * 0.5329522f;
    b5_ = -0.7616f * b5_ - w * 0.0168980f;
    const float out = b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + w * 0.5362f;
    b6_ = w * 0.115926f;
    return out * 0.11f;  // roughly unity peak
  }

 private:
  WhiteNoise white_;
  float b0_ = 0, b1_ = 0, b2_ = 0, b3_ = 0, b4_ = 0, b5_ = 0, b6_ = 0;
};

}  // namespace tonic::dsp
