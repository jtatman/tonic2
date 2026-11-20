// M5 tests: voice allocation, stealing, freeing, mono/legato, unison, per-note expressions, shared
// parameters, polyphony == sum of single voices, no audio-thread allocations, and a benchmark.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "alloc_tracker.hpp"
#include "tonic/dsl/patch.hpp"
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
float hz(float n) { return dsp::midiToHz(n); }

constexpr double kSr = 48000.0;
constexpr int kMax = 128;

using Build = std::function<void(dsl::Patch&)>;

// Voice graph built directly so tests can inspect the VoiceNode.
struct Rig {
  engine::Engine engine;
  graph::VoiceNode* voices = nullptr;
};
void makeRig(Rig& r, graph::VoiceConfig cfg, Build build, engine::Layout layout = {}) {
  auto factory = [build](double sr, int mb) {
    dsl::Patch v;
    build(v);
    return v.compile(sr, mb).graph;
  };
  graph::GraphBuilder b;
  auto node = std::make_unique<graph::VoiceNode>(cfg, 1, factory);
  r.voices = node.get();
  b.setOutput(b.add(std::move(node)));
  r.engine.prepare(kSr, kMax, std::move(layout));
  r.engine.setGraph(b.compile(kSr, kMax).graph);
}
std::vector<float> run(engine::Engine& e, int frames, std::vector<Event> ev) {
  return render::renderOffline(e, 1, frames, ev, kMax)[0];
}

// A voice whose output is its pitch while held (plus a little after release so it stays "sounding").
void pitchVoice(dsl::Patch& v) { v.output(v.pitch() * (v.gate() + 0.001f)); }
// A voice that goes silent immediately on release.
void gatedPitchVoice(dsl::Patch& v) { v.output(v.pitch() * v.gate()); }

void testPolyAllocationAndStealing() {
  Rig r;
  makeRig(r, {3, graph::VoiceMode::Poly}, pitchVoice);
  auto out = run(r.engine, 1000, {Event::noteOn(0, 60), Event::noteOn(10, 64), Event::noteOn(20, 67)});
  const float expect = (hz(60) + hz(64) + hz(67)) * 1.001f;
  check(std::fabs(out[500] - expect) < 0.01f && r.voices->activeVoices() == 3, "3 notes -> 3 voices",
        fmt(out[500]) + " vs " + fmt(expect));

  // 2 voices: 60, 64 held; 64 released (still sounding); 67 must steal the released voice, not 60.
  Rig s;
  makeRig(s, {2, graph::VoiceMode::Poly}, pitchVoice);
  auto st = run(s.engine, 600, {Event::noteOn(0, 60), Event::noteOn(10, 64), Event::noteOff(100, 64), Event::noteOn(200, 67)});
  const float good = (hz(60) + hz(67)) * 1.001f;
  check(std::fabs(st[400] - good) < 0.01f, "steals oldest released voice first", fmt(st[400]) + " vs " + fmt(good));

  // 2 voices, all held: 67 steals the oldest held (60).
  Rig h;
  makeRig(h, {2, graph::VoiceMode::Poly}, pitchVoice);
  auto ht = run(h.engine, 600, {Event::noteOn(0, 60), Event::noteOn(10, 64), Event::noteOn(200, 67)});
  const float held = (hz(64) + hz(67)) * 1.001f;
  check(std::fabs(ht[400] - held) < 0.01f, "then steals oldest held voice", fmt(ht[400]) + " vs " + fmt(held));
}

void testVoicesFreed() {
  Rig r;
  makeRig(r, {4, graph::VoiceMode::Poly}, [](dsl::Patch& v) { v.output(v.sine(v.pitch()) * v.adsr(0.001, 0.01, 0.5, 0.02)); });
  run(r.engine, 2000, {Event::noteOn(0, 60), Event::noteOn(0, 64)});
  const int during = r.voices->activeVoices();
  run(r.engine, 9600, {Event::noteOff(0, 60), Event::noteOff(0, 64)});
  check(during == 2 && r.voices->activeVoices() == 0, "voices freed after release ends",
        std::to_string(during) + " -> " + std::to_string(r.voices->activeVoices()));
}

void testMonoAndLegato() {
  auto triggers = [](graph::VoiceMode mode) {
    Rig r;
    makeRig(r, {1, mode}, [](dsl::Patch& v) { v.output(v.trigger() + 0.f); });
    auto out = run(r.engine, 600, {Event::noteOn(0, 60), Event::noteOn(200, 64), Event::noteOff(400, 64)});
    return static_cast<int>(std::count_if(out.begin(), out.end(), [](float x) { return x > 0.f; }));
  };
  const int mono = triggers(graph::VoiceMode::Mono), legato = triggers(graph::VoiceMode::Legato);
  check(mono == 2 && legato == 1, "mono retriggers, legato does not", std::to_string(mono) + " / " + std::to_string(legato));

  Rig r;
  makeRig(r, {1, graph::VoiceMode::Mono}, gatedPitchVoice);
  auto out = run(r.engine, 800, {Event::noteOn(0, 60), Event::noteOn(200, 64), Event::noteOff(400, 64), Event::noteOff(600, 60)});
  check(std::fabs(out[300] - hz(64)) < 0.01f && std::fabs(out[500] - hz(60)) < 0.01f && out[700] == 0.f,
        "mono: last-note priority with fallback", fmt(out[300]) + ", " + fmt(out[500]) + ", " + fmt(out[700]));
}

void testUnison() {
  Rig r;
  makeRig(r, {6, graph::VoiceMode::Poly, 3, 10.f}, gatedPitchVoice);
  auto out = run(r.engine, 400, {Event::noteOn(0, 69)});
  const float expect = 440.f * (std::exp2(-10.f / 1200.f) + 1.f + std::exp2(10.f / 1200.f));
  check(std::fabs(out[200] - expect) < 0.01f && r.voices->activeVoices() == 3, "unison 3 voices, +/-10 cents",
        fmt(out[200]) + " vs " + fmt(expect));
}

void testExpressions() {
  Rig r;
  makeRig(r, {4, graph::VoiceMode::Poly}, [](dsl::Patch& v) { v.output(v.pitch() * v.gate() + v.pressure() * 1000.f); });
  auto out = run(r.engine, 600, {Event::noteOn(0, 60), Event::noteOn(0, 64),
                                 Event::expression(100, 64, Event::Expr::PitchBend, 2.f),
                                 Event::expression(300, 60, Event::Expr::Pressure, 0.5f)});
  check(std::fabs(out[200] - (hz(60) + hz(66))) < 0.01f, "per-note pitch bend affects only that note", fmt(out[200]));
  check(std::fabs(out[400] - (hz(60) + hz(66) + 500.f)) < 0.01f, "per-note pressure", fmt(out[400]));
}

void testSharedParamsAndDsl() {
  dsl::Patch p;
  auto level = p.param("level", 0.f, 1.f, 0.5f, 0.f);
  auto synth = p.voices({4}, [&](dsl::Patch& v) { v.output(v.gate() * v.use(level)); });
  p.output(synth + 0.f);
  auto c = p.compile(kSr, kMax);
  engine::Engine e;
  e.prepare(kSr, kMax, c.layout);
  e.setGraph(std::move(c.graph));
  auto out = run(e, 400, {Event::noteOn(0, 60), Event::noteOn(0, 64), Event::param(200, level.id, 0.25f)});
  check(out[100] == 1.f && out[300] == 0.5f, "outer param shared by all voices", fmt(out[100]) + " -> " + fmt(out[300]));

  dsl::Patch bad;
  bad.output(bad.voices({2}, [](dsl::Patch& v) { v.output(v.constant(0.f) + v.param("x", 0.f, 1.f, 0.f)); }));
  auto r = bad.compile(kSr, kMax);
  check(!r.graph && r.error.find("outer patch") != std::string::npos, "param declared inside a voice rejected", r.error);
}

void testRouting() {
  auto render2 = [](graph::VoiceConfig a, graph::VoiceConfig b, std::vector<Event> ev) {
    dsl::Patch p;
    auto na = p.voices(a, [](dsl::Patch& v) { v.output(v.pitch() * v.gate()); });
    auto nb = p.voices(b, [](dsl::Patch& v) { v.output(v.pitch() * v.gate()); });
    p.output(na + nb * 10.f);  // tell the two nodes apart
    auto c = p.compile(kSr, kMax);
    engine::Engine e;
    e.prepare(kSr, kMax, c.layout);
    e.setGraph(std::move(c.graph));
    return run(e, 400, ev);
  };
  graph::VoiceConfig ch0{2, graph::VoiceMode::Poly, 1, 0.f, 0}, ch1{2, graph::VoiceMode::Poly, 1, 0.f, 1};
  auto byChannel = render2(ch0, ch1, {Event::noteOn(0, 60, 1.f, 0), Event::noteOn(0, 64, 1.f, 1)});
  check(std::fabs(byChannel[200] - (hz(60) + 10.f * hz(64))) < 0.05f, "channel routing: each node plays its channel",
        fmt(byChannel[200]));
  auto omni = render2(ch0, ch1, {Event::noteOn(0, 67)});
  check(std::fabs(omni[200] - 11.f * hz(67)) < 0.05f, "notes without a channel reach every node", fmt(omni[200]));
  auto wrong = render2(ch0, ch1, {Event::noteOn(0, 60, 1.f, 5)});
  check(wrong[200] == 0.f, "notes on other channels are ignored", fmt(wrong[200]));
  graph::VoiceConfig low{2}, high{2};
  low.highKey = 59;
  high.lowKey = 60;
  auto split = render2(low, high, {Event::noteOn(0, 48), Event::noteOn(0, 72), Event::noteOff(200, 48)});
  check(std::fabs(split[100] - (hz(48) + 10.f * hz(72))) < 0.05f && std::fabs(split[300] - 10.f * hz(72)) < 0.05f,
        "key split: low and high nodes", fmt(split[100]) + " -> " + fmt(split[300]));
}

void testGlideAndWheel() {
  graph::VoiceConfig mono{1, graph::VoiceMode::Legato};
  mono.glideSeconds = 0.1f;
  Rig g;
  makeRig(g, mono, gatedPitchVoice);
  auto out = run(g.engine, 60000, {Event::noteOn(0, 60), Event::noteOn(1000, 72)});  // ~12 time constants
  const float semi = 60.f + 12.f * (1.f - std::exp(-1.f));  // one time constant after the second note
  check(std::fabs(out[10] - hz(60)) < 0.01f, "first note from silence does not glide", fmt(out[10]));
  check(std::fabs(out[1000 + 4800] - hz(semi)) < 0.5f, "legato glide: 63% of the way after one time constant",
        fmt(out[5800]) + " vs " + fmt(hz(semi)));
  check(std::fabs(out[59999] - hz(72)) < 0.01f, "glide arrives", fmt(out[59999]));

  Rig w;
  makeRig(w, {2, graph::VoiceMode::Poly}, gatedPitchVoice);
  auto wh = run(w.engine, 600, {Event::noteOn(0, 60), Event::pitchWheel(200, 1.f), Event::pitchWheel(400, -0.5f)});
  check(std::fabs(wh[300] - hz(62)) < 0.01f && std::fabs(wh[500] - hz(59)) < 0.01f, "pitch wheel, 2-semitone range",
        fmt(wh[300]) + ", " + fmt(wh[500]));

  graph::VoiceConfig wide{1, graph::VoiceMode::Mono};
  wide.bendRange = 12.f;
  wide.glideSeconds = 0.5f;
  Rig ww;
  makeRig(ww, wide, gatedPitchVoice);
  auto wo = run(ww.engine, 400, {Event::noteOn(0, 60), Event::pitchWheel(200, 0.5f)});
  check(std::fabs(wo[200] - hz(66)) < 0.01f, "wheel applies instantly even with glide on (12-semitone range)", fmt(wo[200]));
}

void testStealIsContinuous() {
  // Oscillators keep their phase and the envelope retriggers from its current level, so stealing a sounding
  // voice must not produce a jump bigger than the signal's own steepest slope.
  Rig r;
  makeRig(r, {1, graph::VoiceMode::Poly}, [](dsl::Patch& v) { v.output(v.sine(v.pitch()) * v.adsr(0.002, 0.05, 0.8, 0.05)); });
  auto out = run(r.engine, 6000, {Event::noteOn(0, 60), Event::noteOn(2000, 84)});
  float steal = 0.f;
  for (int i = 1990; i < 2010; ++i) steal = std::max(steal, std::fabs(out[i] - out[i - 1]));
  const float maxSlope = static_cast<float>(dsp::kTwoPi * hz(84) / kSr);  // steepest step of a full-scale sine at the new pitch
  check(steal <= maxSlope, "stealing a sounding voice is click-free (no jump beyond a full-scale sine slope)",
        "max step at steal " + fmt(steal) + " vs limit " + fmt(maxSlope));
}

void synthVoice(dsl::Patch& v) {
  auto env = v.adsr(0.005, 0.2, 0.5, 0.2);
  auto osc = v.saw(v.pitch()) + v.square(v.pitch() * 0.5f) * 0.5f;
  v.output(osc * env * 0.2f >> dsl::ladder(400.f + env * 3000.f, 0.3f, 1.5));
}

void testPolyEqualsSumOfVoices() {
  // Three notes in a 3-voice synth == the three notes rendered alone and summed in allocation order.
  const int notes[] = {57, 60, 64};
  Rig all;
  makeRig(all, {3, graph::VoiceMode::Poly}, synthVoice);
  auto together = run(all.engine, 12000, {Event::noteOn(0, notes[0]), Event::noteOn(0, notes[1]), Event::noteOn(0, notes[2])});
  std::vector<float> sum(12000, 0.f);
  for (int n : notes) {
    Rig one;
    makeRig(one, {3, graph::VoiceMode::Poly}, synthVoice);
    auto alone = run(one.engine, 12000, {Event::noteOn(0, n)});
    for (size_t i = 0; i < sum.size(); ++i) sum[i] += alone[i];
  }
  check(together == sum, "poly output == sum of single-voice renders", "bit-exact over 12000 frames");
}

void testNoAllocationsAndBenchmark() {
  Rig r;
  makeRig(r, {16, graph::VoiceMode::Poly, 1, 0.f}, synthVoice);
  std::vector<Event> ev;
  for (int k = 0; k < 64; ++k) {  // more notes than voices -> constant stealing
    ev.push_back(Event::noteOn(k * 300, 40 + (k * 7) % 36, 0.8f));
    ev.push_back(Event::noteOff(k * 300 + 4000, 40 + (k * 7) % 36));
  }
  std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
  const int frames = 48000 * 2;
  std::vector<float> out(static_cast<size_t>(frames));
  std::vector<Event> block;
  block.reserve(ev.size());
  size_t next = 0;
  g_audioAllocs = 0;
  tl_audio = true;
  const auto t0 = std::chrono::steady_clock::now();
  for (int off = 0; off < frames; off += kMax) {
    const int n = std::min(kMax, frames - off);
    block.clear();
    while (next < ev.size() && ev[next].frame < off + n) { Event x = ev[next++]; x.frame -= off; block.push_back(x); }
    float* ptr = out.data() + off;
    r.engine.process(nullptr, 0, &ptr, 1, n, block.data(), static_cast<int>(block.size()));
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  tl_audio = false;
  bool finite = std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); });
  check(g_audioAllocs == 0 && finite, "no allocation with 16 voices and constant stealing",
        std::to_string(g_audioAllocs.load()) + " allocs");
  const double rt = 2.0 / secs;
  std::printf("[INFO] benchmark: 16-voice saw+square+ladder synth at 48 kHz runs at %.1fx realtime (one core)\n", rt);
  check(rt > 1.0, "16-voice synth faster than realtime", fmt(rt) + "x");
}

}  // namespace

int main() {
  testPolyAllocationAndStealing();
  testVoicesFreed();
  testMonoAndLegato();
  testUnison();
  testExpressions();
  testSharedParamsAndDsl();
  testRouting();
  testGlideAndWheel();
  testStealIsContinuous();
  testPolyEqualsSumOfVoices();
  testNoAllocationsAndBenchmark();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
