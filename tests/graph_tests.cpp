// M2 tests: graph compilation, routing, buffer reuse, variable block sizes, and realtime safety
// (no allocation on the audio thread, including while graphs are hot-swapped).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "tonic/engine/engine.hpp"
#include "tonic/graph/nodes.hpp"

// ---- allocation tracking: counts allocations made while tl_audio is set on the current thread ----
namespace {
thread_local bool tl_audio = false;
std::atomic<long> g_audioAllocs{0};
}  // namespace
namespace {
void* trackedAlloc(std::size_t n) {
  if (tl_audio) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void trackedFree(void* p) noexcept {
  if (tl_audio && p) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);  // frees count too
  std::free(p);
}
}  // namespace
void* operator new(std::size_t n) { return trackedAlloc(n); }
void* operator new[](std::size_t n) { return trackedAlloc(n); }
void operator delete(void* p) noexcept { trackedFree(p); }
void operator delete[](void* p) noexcept { trackedFree(p); }
void operator delete(void* p, std::size_t) noexcept { trackedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { trackedFree(p); }

using namespace tonic;
using graph::GraphBuilder;

namespace {

int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4g", v); return b; }

constexpr double kSr = 48000.0;
constexpr int kMax = 256;

// Constant source node (handy for exact arithmetic checks).
class ConstNode final : public graph::Node {
 public:
  ConstNode(int channels, float v) : Node({}, {channels}), v_(v) {}
  void process(const graph::ProcessArgs& a) override {
    for (int c = 0; c < outputChannels()[0]; ++c)
      for (int i = 0; i < a.numFrames; ++i) a.output(0, c)[i] = v_ + static_cast<float>(c);
  }
 private:
  float v_;
};

std::vector<std::vector<float>> render(graph::CompiledGraph& g, int channels, int frames, int block) {
  std::vector<std::vector<float>> out(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(frames)));
  std::vector<float*> ptrs;
  for (auto& o : out) ptrs.push_back(o.data());
  for (int off = 0; off < frames; off += block) {
    const int n = std::min(block, frames - off);
    g.process(n, nullptr, 0, 0, ptrs.data(), channels, off);
  }
  return out;
}

void testCycleAndValidation() {
  {
    GraphBuilder b;
    auto a = b.add<graph::GainNode>(1, 1.f), c = b.add<graph::GainNode>(1, 1.f);
    b.connect(a, 0, c, 0); b.connect(c, 0, a, 0); b.setOutput(c);
    auto r = b.compile(kSr, kMax);
    check(!r.graph && r.error.find("cycle") != std::string::npos, "cycle rejected", r.error);
  }
  {
    GraphBuilder b;
    auto s = b.add<ConstNode>(2, 1.f), g = b.add<graph::GainNode>(3, 1.f);
    b.connect(s, 0, g, 0); b.setOutput(g);
    auto r = b.compile(kSr, kMax);
    check(!r.graph && r.error.find("mismatch") != std::string::npos, "channel mismatch rejected", r.error);
  }
  {
    GraphBuilder b;
    b.add<ConstNode>(1, 1.f);
    auto r = b.compile(kSr, kMax);
    check(!r.graph, "missing output rejected", r.error);
  }
}

void testFanOutAndSumming() {
  // const 0.5 -> gain x2 and gain x3, both into one gain x1 input (summed) -> 0.5*2 + 0.5*3 = 2.5
  GraphBuilder b;
  auto s = b.add<ConstNode>(1, 0.5f);
  auto g2 = b.add<graph::GainNode>(1, 2.f), g3 = b.add<graph::GainNode>(1, 3.f), sum = b.add<graph::GainNode>(1, 1.f);
  b.connect(s, 0, g2, 0); b.connect(s, 0, g3, 0);
  b.connect(g2, 0, sum, 0); b.connect(g3, 0, sum, 0);
  b.setOutput(sum);
  auto r = b.compile(kSr, kMax);
  auto out = render(*r.graph, 1, 100, 100);
  check(out[0][0] == 2.5f && out[0][99] == 2.5f, "fan-out + summing input", fmt(out[0][0]));
}

void testMonoBroadcastAndPan() {
  GraphBuilder b;
  auto s = b.add<ConstNode>(1, 1.f);
  auto st = b.add<graph::GainNode>(2, 1.f);  // mono -> stereo input: broadcast
  b.connect(s, 0, st, 0); b.setOutput(st);
  auto out = render(*b.compile(kSr, kMax).graph, 2, 10, 10);
  check(out[0][5] == 1.f && out[1][5] == 1.f, "mono broadcast to stereo port", fmt(out[1][5]));

  GraphBuilder p;
  auto ps = p.add<ConstNode>(1, 1.f);
  auto pan = p.add<graph::PanNode>(0.f);
  p.connect(ps, 0, pan, 0); p.setOutput(pan);
  auto po = render(*p.compile(kSr, kMax).graph, 2, 10, 10);
  const double pow = po[0][0] * po[0][0] + po[1][0] * po[1][0];
  check(std::fabs(pow - 1.0) < 1e-6 && std::fabs(po[0][0] - po[1][0]) < 1e-6, "equal-power center pan", fmt(pow));
}

void testUnconnectedInputIsSilent() {
  GraphBuilder b;
  auto g = b.add<graph::GainNode>(1, 5.f);
  b.setOutput(g);
  auto out = render(*b.compile(kSr, kMax).graph, 1, 64, 64);
  check(out[0][0] == 0.f && out[0][63] == 0.f, "unconnected input reads silence", "ok");
}

void testBufferReuseAndDeadNodes() {
  GraphBuilder b;
  auto prev = b.add<ConstNode>(1, 1.f);
  for (int i = 0; i < 50; ++i) {
    auto g = b.add<graph::GainNode>(1, 1.01f);
    b.connect(prev, 0, g, 0);
    prev = g;
  }
  b.add<graph::OscillatorNode>(dsp::Waveform::Saw, 100.0);  // dead: not connected to output
  b.setOutput(prev);
  auto r = b.compile(kSr, kMax);
  auto out = render(*r.graph, 1, 8, 8);
  const float expect = std::pow(1.01f, 50.f);
  check(r.graph->numBuffers() <= 2, "50-node chain reuses buffers", std::to_string(r.graph->numBuffers()) + " buffers");
  check(r.graph->numSteps() == 51, "dead node removed", std::to_string(r.graph->numSteps()) + " steps");
  check(std::fabs(out[0][0] - expect) < 1e-4f, "chain result", fmt(out[0][0]) + " vs " + fmt(expect));
}

void testMultichannel() {
  GraphBuilder b;
  auto s = b.add<ConstNode>(6, 1.f);  // channel c = 1 + c
  auto g = b.add<graph::GainNode>(6, 0.5f);
  b.connect(s, 0, g, 0); b.setOutput(g);
  auto out = render(*b.compile(kSr, kMax).graph, 6, 16, 16);
  bool ok = true;
  for (int c = 0; c < 6; ++c) ok &= out[static_cast<size_t>(c)][7] == 0.5f * (1.f + static_cast<float>(c));
  check(ok, "6-channel bus", fmt(out[5][7]));
}

std::unique_ptr<graph::CompiledGraph> synthPatch(double hz) {
  GraphBuilder b;
  auto o1 = b.add<graph::OscillatorNode>(dsp::Waveform::Saw, hz);
  auto o2 = b.add<graph::OscillatorNode>(dsp::Waveform::Square, hz * 1.005);
  auto lad = b.add<graph::LadderNode>(1, 1200.0, 0.5, 1.5);
  auto pan = b.add<graph::PanNode>(0.3f);
  auto svf = b.add<graph::SvfLowpassNode>(2, 6000.0, 0.7);
  b.connect(o1, 0, lad, 0); b.connect(o2, 0, lad, 0);
  b.connect(lad, 0, pan, 0); b.connect(pan, 0, svf, 0);
  b.setOutput(svf);
  return b.compile(kSr, kMax).graph;
}

void testVariableBlockSizes() {
  auto a = synthPatch(110.0), b = synthPatch(110.0);
  const int frames = 20000;
  auto ref = render(*a, 2, frames, kMax);
  // Engine with irregular host block sizes, including larger than maxBlock.
  engine::Engine e;
  e.prepare(kSr, kMax);
  e.setGraph(std::move(b));
  std::vector<std::vector<float>> out(2, std::vector<float>(frames));
  const int sizes[] = {1, 7, 256, 1000, 33, 64, 513, 2};
  for (int off = 0, k = 0; off < frames; ++k) {
    const int n = std::min(sizes[k % 8], frames - off);
    float* ptrs[2] = {out[0].data() + off, out[1].data() + off};
    e.process(nullptr, 0, ptrs, 2, n);
    off += n;
  }
  check(out == ref, "engine output independent of host block size", "bit-exact over 20000 frames");
}

void testExternalInput() {
  GraphBuilder b;
  auto in = b.addInput(2);
  auto g = b.add<graph::GainNode>(2, 0.25f);
  b.connect(in, 0, g, 0); b.setOutput(g);
  engine::Engine e;
  e.prepare(kSr, kMax);
  e.setGraph(b.compile(kSr, kMax).graph);
  std::vector<float> l(600, 1.f), r(600, -2.f), ol(600), orr(600);
  const float* ins[2] = {l.data(), r.data()};
  float* outs[2] = {ol.data(), orr.data()};
  e.process(ins, 2, outs, 2, 600);
  check(ol[599] == 0.25f && orr[300] == -0.5f, "external stereo input through graph", fmt(orr[300]));
}

void testRealtimeSafety() {
  // Sanity: the detector must see an allocation made on a flagged thread.
  g_audioAllocs = 0;
  tl_audio = true;
  { auto* probe = new std::vector<int>(4); delete probe; }
  tl_audio = false;
  check(g_audioAllocs >= 2, "allocation detector works", std::to_string(g_audioAllocs.load()) + " events");

  engine::Engine e;
  e.prepare(kSr, kMax);
  e.setGraph(synthPatch(220.0));
  std::vector<float> l(kMax), r(kMax);
  float* outs[2] = {l.data(), r.data()};

  g_audioAllocs = 0;
  tl_audio = true;
  for (int i = 0; i < 2000; ++i) e.process(nullptr, 0, outs, 2, kMax);
  tl_audio = false;
  check(g_audioAllocs == 0, "no allocation in process()", std::to_string(g_audioAllocs.load()) + " allocs");

  // Hot-swap stress: control thread publishes new graphs while the audio thread runs.
  std::atomic<bool> stop{false};
  std::atomic<int> swaps{0};
  g_audioAllocs = 0;
  std::thread control([&] {
    for (int i = 0; !stop.load(); ++i) {
      if (e.setGraph(synthPatch(100.0 + i % 200))) ++swaps;
      e.collectGarbage();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  bool finite = true;
  std::thread audio([&] {
    tl_audio = true;
    for (int i = 0; i < 20000; ++i) {
      e.process(nullptr, 0, outs, 2, 64 + (i % 4) * 48);
      finite &= std::isfinite(l[0]) && std::isfinite(r[0]);
    }
    tl_audio = false;
  });
  audio.join();
  stop = true;
  control.join();
  e.collectGarbage();
  check(g_audioAllocs == 0 && finite && swaps > 10, "no allocation during hot swaps",
        std::to_string(swaps.load()) + " swaps, " + std::to_string(g_audioAllocs.load()) + " audio allocs");

  // Mismatched settings are refused and the current graph keeps running.
  GraphBuilder b;
  auto o = b.add<graph::OscillatorNode>(dsp::Waveform::Sine, 440.0);
  b.setOutput(o);
  check(!e.setGraph(b.compile(44100.0, kMax).graph) && e.hasActiveGraph(), "mismatched graph refused", "ok");
}

}  // namespace

int main() {
  testCycleAndValidation();
  testFanOutAndSumming();
  testMonoBroadcastAndPan();
  testUnconnectedInputIsSilent();
  testBufferReuseAndDeadNodes();
  testMultichannel();
  testVariableBlockSizes();
  testExternalInput();
  testRealtimeSafety();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
