// Engine: runs the current CompiledGraph on the audio thread, swaps in new graphs without locks, and
// turns parameters, macros and note events into sample-accurate per-sample control buffers.
//
// Threads:
//  - control thread: prepare() (audio stopped), setGraph(), collectGarbage(), setParam(), paramValue()
//  - audio thread:   process()
// setGraph() publishes through an atomic pointer; the audio thread adopts it at the start of a block and
// hands the old graph to a lock-free retire queue, freed by collectGarbage(). setParam() goes through
// per-parameter atomics. The audio thread never allocates, frees, locks, or throws.
//
// Timing: events carry a frame offset inside the process() call and take effect exactly on that frame.
// Nodes read per-sample parameter, gate, trigger, pitch and velocity buffers, so the graph itself does
// not need to be split at event boundaries. Events must be sorted by frame.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../dsp/math.hpp"
#include "../dsp/smoother.hpp"
#include "../graph/graph.hpp"
#include "events.hpp"
#include "params.hpp"
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

  // Control thread, audio stopped. Graphs must be compiled with the same sample rate and maxBlock.
  void prepare(double sampleRate, int maxBlock, Layout layout = {}) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    layout_ = std::move(layout);
    const size_t n = layout_.params.size();
    smoothers_.assign(n, {});
    base_.assign(n, 0.f);
    paramStore_.assign(n * static_cast<size_t>(maxBlock), 0.f);
    paramPtrs_.resize(n);
    uiValue_ = std::make_unique<std::atomic<float>[]>(n);
    reported_ = std::make_unique<std::atomic<float>[]>(n);
    uiVersion_ = std::make_unique<std::atomic<uint32_t>[]>(n);
    seenVersion_.assign(n, 0);
    for (size_t p = 0; p < n; ++p) {
      const ParamSpec& s = layout_.params[p];
      smoothers_[p].prepare(sampleRate);
      smoothers_[p].setTime(s.smoothSeconds);
      smoothers_[p].reset(s.def);
      base_[p] = s.def;
      uiValue_[p].store(s.def);
      reported_[p].store(s.def);
      uiVersion_[p].store(0);
      paramPtrs_[p] = paramStore_.data() + p * static_cast<size_t>(maxBlock);
    }
    // Drop macro entries that reference unknown parameters; remember which params macros touch.
    macroTargets_.clear();
    for (auto& m : layout_.macros) {
      std::erase_if(m.targets, [&](const MacroTarget& t) { return t.param < 0 || t.param >= static_cast<int>(n); });
      for (const auto& t : m.targets)
        if (std::find(macroTargets_.begin(), macroTargets_.end(), t.param) == macroTargets_.end())
          macroTargets_.push_back(t.param);
    }
    std::erase_if(layout_.macros, [&](const MacroSpec& m) { return m.param < 0 || m.param >= static_cast<int>(n); });
    for (auto* v : {&gate_, &trigger_, &pitch_, &velocity_}) v->assign(static_cast<size_t>(maxBlock), 0.f);
    noteCount_ = 0;
    pendingTrigger_ = false;
    control_ = {paramPtrs_.data(), static_cast<int>(n), gate_.data(), trigger_.data(), pitch_.data(), velocity_.data()};
  }
  double sampleRate() const { return sampleRate_; }
  int maxBlock() const { return maxBlock_; }
  const Layout& layout() const { return layout_; }

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

  // Control thread (UI, MIDI learn, presets). Takes effect at the start of the next block, smoothed.
  void setParam(int id, float value) {
    if (id < 0 || id >= static_cast<int>(layout_.params.size())) return;
    const ParamSpec& s = layout_.params[static_cast<size_t>(id)];
    uiValue_[static_cast<size_t>(id)].store(std::clamp(value, s.min, s.max), std::memory_order_relaxed);
    uiVersion_[static_cast<size_t>(id)].fetch_add(1, std::memory_order_release);
  }
  // Any thread. Last value the audio thread produced (after smoothing and macros).
  float paramValue(int id) const {
    if (id < 0 || id >= static_cast<int>(layout_.params.size())) return 0.f;
    return reported_[static_cast<size_t>(id)].load(std::memory_order_relaxed);
  }

  // Audio thread. Any numFrames; blocks larger than maxBlock are processed in chunks.
  void process(const float* const* in, int numIn, float* const* out, int numOut, int numFrames,
               const Event* events = nullptr, int numEvents = 0) {
    if (gate_.empty()) {  // prepare() was never called: output silence rather than touch unsized buffers
      for (int c = 0; c < numOut; ++c) if (out[c]) std::memset(out[c], 0, sizeof(float) * numFrames);
      return;
    }
    adoptPending();
    pullUiChanges();
    int e = 0;
    for (int offset = 0; offset < numFrames;) {
      const int n = std::min(maxBlock_, numFrames - offset);
      for (int i = 0; i < n; ++i) {
        while (e < numEvents && events[e].frame <= offset + i) apply(events[e++]);
        computeFrame(i);
      }
      if (active_) {
        active_->process(n, in, numIn, offset, out, numOut, offset, &control_);
      } else {
        for (int c = 0; c < numOut; ++c) if (out[c]) std::memset(out[c] + offset, 0, sizeof(float) * n);
      }
      if (offset + n >= numFrames)
        for (size_t p = 0; p < paramPtrs_.size(); ++p) reported_[p].store(paramPtrs_[p][n - 1], std::memory_order_relaxed);
      offset += n;
    }
    while (e < numEvents) apply(events[e++]);  // events past the end of the block
  }

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

  void pullUiChanges() {
    for (size_t p = 0; p < base_.size(); ++p) {
      const uint32_t v = uiVersion_[p].load(std::memory_order_acquire);
      if (v == seenVersion_[p]) continue;
      seenVersion_[p] = v;
      base_[p] = uiValue_[p].load(std::memory_order_relaxed);
      smoothers_[p].setTarget(base_[p]);
    }
  }

  void apply(const Event& ev) {
    switch (ev.type) {
      case Event::Type::Param: {
        if (ev.key < 0 || ev.key >= static_cast<int>(base_.size())) break;
        const ParamSpec& s = layout_.params[static_cast<size_t>(ev.key)];
        base_[static_cast<size_t>(ev.key)] = std::clamp(ev.value, s.min, s.max);
        smoothers_[static_cast<size_t>(ev.key)].setTarget(base_[static_cast<size_t>(ev.key)]);
        break;
      }
      case Event::Type::NoteOn:
        if (ev.value <= 0.f) { noteOff(ev.key); break; }  // MIDI: velocity 0 means note off
        removeNote(ev.key);
        if (noteCount_ == kMaxNotes) { std::memmove(notes_, notes_ + 1, sizeof(int) * (kMaxNotes - 1)); --noteCount_; }
        notes_[noteCount_++] = ev.key;
        pitchHz_ = dsp::midiToHz(static_cast<float>(ev.key));
        velocityValue_ = ev.value;
        pendingTrigger_ = true;
        break;
      case Event::Type::NoteOff:
        noteOff(ev.key);
        break;
    }
  }
  void removeNote(int key) {
    for (int i = 0; i < noteCount_; ++i)
      if (notes_[i] == key) { std::memmove(notes_ + i, notes_ + i + 1, sizeof(int) * (noteCount_ - i - 1)); --noteCount_; return; }
  }
  // Last-note priority: releasing the newest note falls back to the previous held one (legato).
  void noteOff(int key) {
    removeNote(key);
    if (noteCount_ > 0) pitchHz_ = dsp::midiToHz(static_cast<float>(notes_[noteCount_ - 1]));
  }

  void computeFrame(int i) {
    gate_[i] = noteCount_ > 0 ? 1.f : 0.f;
    trigger_[i] = pendingTrigger_ ? 1.f : 0.f;
    pendingTrigger_ = false;
    pitch_[i] = pitchHz_;
    velocity_[i] = velocityValue_;
    for (size_t p = 0; p < smoothers_.size(); ++p) paramPtrs_[p][i] = smoothers_[p].next();
    if (layout_.macros.empty()) return;
    for (const MacroSpec& m : layout_.macros) {
      const float mv = std::max(paramPtrs_[m.param][i], 0.f);
      for (const MacroTarget& t : m.targets) {
        const float shaped = t.curve == 1.f ? mv : std::pow(mv, t.curve);
        paramPtrs_[t.param][i] += t.lo + (t.hi - t.lo) * shaped;
      }
    }
    for (int p : macroTargets_) {
      const ParamSpec& s = layout_.params[static_cast<size_t>(p)];
      paramPtrs_[p][i] = std::clamp(paramPtrs_[p][i], s.min, s.max);
    }
  }

  static constexpr int kMaxNotes = 16;
  double sampleRate_ = 48000.0;
  int maxBlock_ = 512;
  Layout layout_;
  std::vector<dsp::OnePoleSmoother> smoothers_;
  std::vector<float> base_, paramStore_;
  std::vector<float*> paramPtrs_;
  std::unique_ptr<std::atomic<float>[]> uiValue_, reported_;
  std::unique_ptr<std::atomic<uint32_t>[]> uiVersion_;
  std::vector<uint32_t> seenVersion_;
  std::vector<int> macroTargets_;
  std::vector<float> gate_, trigger_, pitch_, velocity_;
  graph::ControlBuffers control_;
  int notes_[kMaxNotes] = {};
  int noteCount_ = 0;
  float pitchHz_ = 440.f, velocityValue_ = 0.f;
  bool pendingTrigger_ = false;

  std::atomic<graph::CompiledGraph*> pending_{nullptr};
  graph::CompiledGraph* active_ = nullptr;  // owned by the audio thread
  SpscQueue<graph::CompiledGraph*, 64> retired_;
};

}  // namespace tonic::engine
