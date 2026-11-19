// M4 tests: patching syntax builds the same graphs as hand-built ones, arithmetic and modulation work,
// errors are reported at compile time, and the demo synths render cleanly without audio-thread allocations.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "alloc_tracker.hpp"
#include "demo_synths.hpp"
#include "tonic/render/offline.hpp"

using namespace tonic;
using engine::Event;

namespace {

int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.6g", v); return b; }

constexpr double kSr = 48000.0;
constexpr int kMax = 256;

std::vector<std::vector<float>> renderPatch(dsl::Patch& p, int channels, int frames, std::vector<Event> ev = {},
                                            std::string* err = nullptr) {
  auto c = p.compile(kSr, kMax);
  if (!c.graph) { if (err) *err = c.error; return {}; }
  engine::Engine e;
  e.prepare(kSr, kMax, c.layout);
  e.setGraph(std::move(c.graph));
  return render::renderOffline(e, channels, frames, ev);
}

void testArithmetic() {
  dsl::Patch p;
  auto two = p.constant(2.f);
  p.output((two * 3.f + 1.f) / 2.f - 0.5f + (10.f - two) * 0.f + 1.f / two);  // 3.5 - 0.5 + 0 + 0.5 = 3.5
  auto out = renderPatch(p, 1, 64);
  check(!out.empty() && out[0][0] == 3.5f && out[0][63] == 3.5f, "arithmetic on signals and floats",
        out.empty() ? "compile failed" : fmt(out[0][0]));
}

void testMatchesHandBuiltGraph() {
  dsl::Patch p;
  p.output(p.saw(110.f) >> dsl::ladder(900.f, 0.5f, 1.5));
  auto viaDsl = renderPatch(p, 1, 8000);

  graph::GraphBuilder b;
  auto o = b.add<graph::OscillatorNode>(dsp::Waveform::Saw, 110.0);
  auto l = b.add<graph::LadderNode>(1, 900.0, 0.5, 1.5);
  b.connect(o, 0, l, 0);
  b.setOutput(l);
  engine::Engine e;
  e.prepare(kSr, kMax, {});
  e.setGraph(b.compile(kSr, kMax).graph);
  auto direct = render::renderOffline(e, 1, 8000, {});
  check(!viaDsl.empty() && viaDsl[0] == direct[0], "DSL saw >> ladder == hand-built graph", "bit-exact over 8000 frames");
}

void testFanOutSharesNodes() {
  dsl::Patch p;
  auto s = p.saw(220.f);
  p.output(s + s);
  auto out = renderPatch(p, 1, 2000);
  dsl::Patch q;
  q.output(q.saw(220.f) * 2.f);
  auto ref = renderPatch(q, 1, 2000);
  check(!out.empty() && out[0] == ref[0], "reusing a signal fans out (s + s == 2s)", "bit-exact");
}

void testParamsAndMacrosThroughDsl() {
  dsl::Patch p;
  auto level = p.param("level", 0.f, 1.f, 0.25f, 0.f);
  auto m = p.param("macro", 0.f, 1.f, 0.f, 0.f);
  p.macro(m, {{level, 0.f, 0.5f, 1.f}});
  p.output(level + 0.f);
  auto out = renderPatch(p, 1, 400, {Event::param(100, 0, 0.5f), Event::param(200, 1, 1.f)});
  check(!out.empty() && out[0][99] == 0.25f && out[0][100] == 0.5f && out[0][200] == 1.f,
        "DSL params: events and macro reach the signal", out.empty() ? "?" : fmt(out[0][200]));
}

void testFmDepthZeroIsPlainSine() {
  dsl::Patch p;
  auto depth = p.param("depth", 0.f, 100.f, 0.f, 0.f);
  p.output(p.sine(440.f + p.sine(660.f) * depth));
  auto fm = renderPatch(p, 1, 4000);
  dsl::Patch q;
  q.output(q.sine(440.f));
  auto plain = renderPatch(q, 1, 4000);
  check(!fm.empty() && fm[0] == plain[0], "FM with depth 0 equals a plain sine", "bit-exact");
}

void testEnvelopeModulatesCutoff() {
  // Brighter while the envelope is high: compare high-frequency content early vs late in the note.
  dsl::Patch p;
  auto env = p.adsr(0.001, 0.15, 0.0, 0.1);
  p.output(p.saw(110.f) >> dsl::ladder(200.f + env * 6000.f, 0.f));
  auto out = renderPatch(p, 1, 24000, {Event::noteOn(0, 45)});
  auto hfEnergy = [&](int from, int to) {
    double e = 0;
    for (int i = from + 1; i < to; ++i) { double d = out[0][i] - out[0][i - 1]; e += d * d; }
    return e / (to - from);
  };
  const double early = hfEnergy(100, 2500), late = hfEnergy(20000, 23900);
  check(!out.empty() && early > 4 * late, "envelope opens the filter (audio-rate modulation)", fmt(early / late) + "x");
}

void testErrors() {
  {
    dsl::Patch p;
    auto stereo = p.saw(100.f) >> dsl::pan(0.f);
    auto six = p.constant(1.f);  // mono is fine with stereo...
    p.output(stereo * six);
    auto ok = p.compile(kSr, kMax);
    check(ok.graph != nullptr, "mono * stereo broadcasts", ok.error.empty() ? "ok" : ok.error);
  }
  {
    dsl::Patch p;
    auto stereo = p.saw(100.f) >> dsl::pan(0.f);
    p.output(stereo >> dsl::pan(0.f));
    auto r = p.compile(kSr, kMax);
    check(!r.graph && r.error.find("mono") != std::string::npos, "pan of stereo rejected", r.error);
  }
  {
    dsl::Patch a, b;
    a.output(a.saw(100.f) + b.saw(100.f));
    auto r = a.compile(kSr, kMax);
    check(!r.graph && r.error.find("different patch") != std::string::npos, "mixing patches rejected", r.error);
  }
  {
    dsl::Patch p;
    p.saw(100.f);
    auto r = p.compile(kSr, kMax);
    check(!r.graph && r.error == "no output set", "missing output rejected", r.error);
  }
}

void testDemoSynths() {
  for (const auto& d : demos::all()) {
    dsl::Patch p;
    d.build(p);
    auto c = p.compile(kSr, kMax);
    if (!c.graph) { check(false, "demo " + d.name, c.error); continue; }
    engine::Engine e;
    e.prepare(kSr, kMax, c.layout);
    e.setGraph(std::move(c.graph));
    auto ev = d.script(kSr);
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
    const int frames = static_cast<int>(kSr * d.seconds);
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(frames)));
    std::vector<Event> blockEvents;
    blockEvents.reserve(ev.size());
    size_t next = 0;
    g_audioAllocs = 0;  // buffers above are allocated up front; count only inside process()
    tl_audio = true;
    for (int off = 0; off < frames; off += kMax) {
      const int n = std::min(kMax, frames - off);
      blockEvents.clear();
      while (next < ev.size() && ev[next].frame < off + n) { Event x = ev[next++]; x.frame -= off; blockEvents.push_back(x); }
      float* ptrs[2] = {out[0].data() + off, out[1].data() + off};
      e.process(nullptr, 0, ptrs, 2, n, blockEvents.data(), static_cast<int>(blockEvents.size()));
    }
    tl_audio = false;
    double peak = 0, sum = 0;
    bool finite = true;
    for (const auto& ch : out)
      for (float v : ch) { finite &= std::isfinite(v); peak = std::max(peak, double(std::fabs(v))); sum += double(v) * v; }
    const double rms = std::sqrt(sum / (2.0 * frames));
    check(finite && peak < 1.0 && rms > 1e-3 && g_audioAllocs == 0, "demo " + d.name + " renders cleanly",
          "peak " + fmt(peak) + ", rms " + fmt(rms) + ", " + std::to_string(g_audioAllocs.load()) + " allocs");
  }
}

}  // namespace

int main() {
  testArithmetic();
  testMatchesHandBuiltGraph();
  testFanOutSharesNodes();
  testParamsAndMacrosThroughDsl();
  testFmDepthZeroIsPlainSine();
  testEnvelopeModulatesCutoff();
  testErrors();
  testDemoSynths();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
