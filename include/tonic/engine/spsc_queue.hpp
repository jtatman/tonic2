// Fixed-capacity lock-free single-producer / single-consumer queue. No allocation after construction.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace tonic::engine {

template <class T, size_t Capacity>
class SpscQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  // Producer thread only.
  bool push(const T& v) {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == Capacity) return false;
    buf_[h & (Capacity - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool full() const { return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire) == Capacity; }
  // Consumer thread only.
  bool pop(T& out) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = buf_[t & (Capacity - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  std::array<T, Capacity> buf_{};
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace tonic::engine
