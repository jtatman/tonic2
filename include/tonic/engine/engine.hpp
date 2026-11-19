// Engine: runs the current CompiledGraph on the audio thread and swaps in new graphs without locks.
//
// Threads:
//  - control thread: prepare(), setGraph(), collectGarbage()
//  - audio thread:   process()
// setGraph() publishes through an atomic pointer. At the start of process() the audio thread adopts
// the pending graph and hands the old one to a lock-free retire queue; collectGarbage() frees it on
// the control thread. The audio thread never allocates, frees, locks, or throws.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "../graph/graph.hpp"
#include "spsc_queue.hpp"

namespace tonic::engine {

class Engine {
 public:
  Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  // The audio thread must be stopped before destruction.
  ~Engine() {
    delete pending_.exchange(nullptr);
    delete active_;
    collectGarbage();
  }

  // Control thread, audio stopped. Graphs must be compiled with the same settings.
  void prepare(double sampleRate, int maxBlock) { sampleRate_ = sampleRate; maxBlock_ = maxBlock; }
  double sampleRate() const { return sampleRate_; }
  int maxBlock() const { return maxBlock_; }

  // Control thread. Returns false (and keeps the current graph) if the graph's settings don't match.
  bool setGraph(std::unique_ptr<graph::CompiledGraph> g) {
    if (!g || g->sampleRate() != sampleRate_ || g->maxBlock() != maxBlock_) return false;
    // Anything still pending was never seen by the audio thread, so it can be freed here.
    delete pending_.exchange(g.release(), std::memory_order_acq_rel);
    return true;
  }

  // Control thread. Frees graphs the audio thread has finished with.
  void collectGarbage() {
    graph::CompiledGraph* g = nullptr;
    while (retired_.pop(g)) delete g;
  }

  // Audio thread. Any numFrames; blocks larger than maxBlock are processed in chunks.
  void process(const float* const* in, int numIn, float* const* out, int numOut, int numFrames) {
    adoptPending();
    if (!active_) {
      for (int c = 0; c < numOut; ++c) if (out[c]) std::memset(out[c], 0, sizeof(float) * numFrames);
      return;
    }
    for (int offset = 0; offset < numFrames;) {
      const int n = std::min(maxBlock_, numFrames - offset);
      active_->process(n, in, numIn, offset, out, numOut, offset);
      offset += n;
    }
  }

  // Audio thread (or when stopped): true once a graph is running.
  bool hasActiveGraph() const { return active_ != nullptr; }

 private:
  void adoptPending() {
    if (!pending_.load(std::memory_order_relaxed)) return;
    if (active_ && retired_.full()) return;  // try again next block, once garbage is collected
    graph::CompiledGraph* g = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!g) return;
    if (active_) retired_.push(active_);  // cannot fail: checked full() above, single producer
    active_ = g;
  }

  double sampleRate_ = 48000.0;
  int maxBlock_ = 512;
  std::atomic<graph::CompiledGraph*> pending_{nullptr};
  graph::CompiledGraph* active_ = nullptr;  // owned by the audio thread
  SpscQueue<graph::CompiledGraph*, 64> retired_;
};

}  // namespace tonic::engine
