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

}  // namespace tonic::dsp
