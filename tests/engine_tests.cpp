// M3 tests: parameters (events, UI, smoothing), macros, note gate/trigger/pitch, sample accuracy,
// host block-size independence, WAV round trip, and no allocation on the audio thread.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "alloc_tracker.hpp"
#include "tonic/graph/nodes.hpp"
#include "tonic/render/offline.hpp"
#include "tonic/render/wav.hpp"

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
constexpr int kMax = 128;

// Outputs a control signal so tests can observe exactly what nodes see.
class ProbeNode final : public graph::Node {
 public:
  enum class What { Param, Gate, Trigger, Pitch, Velocity };
  ProbeNode(What w, int id = 0) : Node({}, {1}), what_(w), id_(id) {}
  void process(const graph::ProcessArgs& a) override {
    const float* src = nullptr;
    if (a.control) {
      switch (what_) {
        case What::Param: src = a.param(id_); break;
        case What::Gate: src = a.control->gate; break;
        case What::Trigger: src = a.control->trigger; break;
        case What::Pitch: src = a.control->pitchHz; break;
        case What::Velocity: src = a.control->velocity; break;
      }
    }
    for (int i = 0; i < a.numFrames; ++i) a.output(0, 0)[i] = src ? src[i] : -1.f;
  }
 private:
  What what_;
  int id_;
};

std::unique_ptr<graph::CompiledGraph> probe(ProbeNode::What w, int id = 0) {
  graph::GraphBuilder b;
  b.setOutput(b.add<ProbeNode>(w, id));
  return b.compile(kSr, kMax).graph;
}

std::vector<float> run(engine::Engine& e, int frames, std::vector<Event> ev, int hostBlock = 256) {
  return render::renderOffline(e, 1, frames, ev, hostBlock)[0];
}

void testParamEventsSampleAccurate() {
  engine::Layout L;
  const int p = L.add("p", 0.f, 10.f, 1.f, 0.f);  // no smoothing
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(probe(ProbeNode::What::Param, p));
  auto out = run(e, 4096, {Event::param(1000, p, 7.f), Event::param(3001, p, 99.f)});
  check(out[999] == 1.f && out[1000] == 7.f, "param event lands on its exact frame", fmt(out[999]) + " -> " + fmt(out[1000]));
  check(out[3000] == 7.f && out[3001] == 10.f, "param event clamped to range", fmt(out[3001]));
}

void testSmoothingAndUi() {
  engine::Layout L;
  const int p = L.add("p", 0.f, 1.f, 0.f, 0.01f);  // 10 ms
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(probe(ProbeNode::What::Param, p));
  auto out = run(e, 2000, {Event::param(500, p, 1.f)});
  check(std::fabs(out[500 + 479] - 0.632f) < 0.01f && out[499] == 0.f, "smoothing: 63% after 10 ms", fmt(out[979]));

  e.setParam(p, 0.5f);  // UI thread path: applied at the next block, smoothed
  auto ui = run(e, 4800, {});
  check(std::fabs(ui[4799] - 0.5f) < 1e-3f && std::fabs(e.paramValue(p) - ui[4799]) < 1e-6f, "UI setParam + paramValue",
        fmt(ui[4799]));
}

void testMacros() {
  engine::Layout L;
  const int cutoff = L.add("cutoff", 0.f, 1000.f, 100.f, 0.f);
  const int macro = L.add("macro", 0.f, 1.f, 0.f, 0.f);
  L.macros.push_back({macro, {{cutoff, 0.f, 2000.f, 1.f}}});
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(probe(ProbeNode::What::Param, cutoff));
  auto out = run(e, 600, {Event::param(100, macro, 0.25f), Event::param(300, macro, 1.f), Event::param(500, cutoff, 0.f)});
  check(out[99] == 100.f && out[100] == 600.f, "macro offsets target (100 + 0.25*2000)", fmt(out[100]));
  check(out[300] == 1000.f, "macro result clamped to target range", fmt(out[300]));
  check(out[500] == 1000.f, "macro offset still applied after base change", fmt(out[500]));
}

void testNotes() {
  engine::Engine e;
  e.prepare(kSr, kMax, {});
  e.setGraph(probe(ProbeNode::What::Gate));
  auto gate = run(e, 1000, {Event::noteOn(300, 60), Event::noteOff(700, 60)});
  check(gate[299] == 0.f && gate[300] == 1.f && gate[699] == 1.f && gate[700] == 0.f, "gate opens/closes on exact frames", "ok");

  engine::Engine t;
  t.prepare(kSr, kMax, {});
  t.setGraph(probe(ProbeNode::What::Trigger));
  auto trig = run(t, 400, {Event::noteOn(0, 60), Event::noteOn(100, 64), Event::noteOff(200, 64)});
  int count = 0;
  for (float v : trig) count += v > 0.f;
  check(trig[0] == 1.f && trig[100] == 1.f && count == 2, "trigger once per note-on (incl. legato)", std::to_string(count) + " triggers");

  engine::Engine p;
  p.prepare(kSr, kMax, {});
  p.setGraph(probe(ProbeNode::What::Pitch));
  auto pitch = run(p, 400, {Event::noteOn(0, 60), Event::noteOn(100, 64), Event::noteOff(200, 64)});
  check(std::fabs(pitch[150] - dsp::midiToHz(64)) < 1e-3f && std::fabs(pitch[250] - dsp::midiToHz(60)) < 1e-3f,
        "last-note priority falls back to held note", fmt(pitch[150]) + " / " + fmt(pitch[250]));

  engine::Engine z;
  z.prepare(kSr, kMax, {});
  z.setGraph(probe(ProbeNode::What::Gate));
  auto zg = run(z, 300, {Event::noteOn(0, 60), Event::noteOn(100, 60, 0.f)});
  check(zg[99] == 1.f && zg[100] == 0.f, "note-on with velocity 0 acts as note-off", "ok");
}

void testMidiCc() {
  engine::Layout L;
  const int cut = L.add("cutoff", 100.f, 1100.f, 100.f, 0.f);
  const int m = L.add("macro", 0.f, 1.f, 0.f, 0.f);
  const int res = L.add("res", 0.f, 1.f, 0.f, 0.f);
  L.macros.push_back({m, {{res, 0.f, 0.8f, 1.f}}});
  L.ccBindings.push_back({74, cut});
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(probe(ProbeNode::What::Param, cut));
  auto out = run(e, 600, {Event::cc(300, 74, 0.5f, 0)});
  check(out[299] == 100.f && out[300] == 600.f, "bound CC maps 0..1 onto the parameter range on its exact frame",
        fmt(out[299]) + " -> " + fmt(out[300]));
  auto unbound = run(e, 200, {Event::cc(10, 20, 1.f)});
  check(unbound[150] == 600.f && e.ccMapping(20) == -1, "unbound CC is ignored", fmt(unbound[150]));

  e.mapCc(20, cut);
  auto remapped = run(e, 200, {Event::cc(10, 20, 1.f)});
  check(remapped[150] == 1100.f, "mapCc rebinds at runtime", fmt(remapped[150]));

  e.learnCc(m);  // MIDI learn: the next CC that arrives (CC 7) gets bound to the macro
  check(e.isLearning(), "learn mode armed", "ok");
  e.setGraph(probe(ProbeNode::What::Param, res));
  auto learned = run(e, 300, {Event::cc(100, 7, 0.5f)});
  e.collectGarbage();
  check(!e.isLearning() && e.ccMapping(7) == m && learned[99] == 0.f && std::fabs(learned[100] - 0.4f) < 1e-6f,
        "MIDI learn binds the next CC; CC drives a macro and its targets", fmt(learned[100]));
}

void testParamsSurviveGraphSwap() {
  engine::Layout L;
  const int p = L.add("p", 0.f, 1.f, 0.f, 0.f);
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(probe(ProbeNode::What::Param, p));
  run(e, 256, {Event::param(0, p, 0.75f)});
  e.setGraph(probe(ProbeNode::What::Param, p));
  e.collectGarbage();
  auto out = run(e, 256, {});
  check(out[0] == 0.75f, "parameter values survive a graph swap", fmt(out[0]));
}

std::unique_ptr<graph::CompiledGraph> synth(int cutoffParam) {
  graph::GraphBuilder b;
  auto o = b.add<graph::OscillatorNode>(dsp::Waveform::Saw, graph::FreqSource::note());
  auto l = b.add<graph::LadderNode>(1, 800.0, 0.4, 1.5, cutoffParam);
  auto env = b.add<graph::EnvelopeNode>(0.01, 0.1, 0.5, 0.2);
  auto v = b.add<graph::VcaNode>(1);
  b.connect(o, 0, l, 0); b.connect(l, 0, v, 0); b.connect(env, 0, v, 1);
  b.setOutput(v);
  return b.compile(kSr, kMax).graph;
}

std::vector<Event> phrase(int cutoff) {
  return {Event::noteOn(10, 57), Event::param(2000, cutoff, 3000.f), Event::noteOn(5000, 60),
          Event::noteOff(9000, 60), Event::noteOff(9500, 57), Event::param(12000, cutoff, 300.f)};
}

void testHostBlockIndependence() {
  engine::Layout L;
  const int cutoff = L.add("cutoff", 50.f, 10000.f, 800.f, 0.02f);
  std::vector<std::vector<float>> results;
  for (int hb : {64, 1000, 37}) {
    engine::Engine e;
    e.prepare(kSr, kMax, L);
    e.setGraph(synth(cutoff));
    results.push_back(run(e, 20000, phrase(cutoff), hb));
  }
  double peak = 0;
  for (float v : results[0]) peak = std::max(peak, double(std::fabs(v)));
  check(results[0] == results[1] && results[0] == results[2] && peak > 0.05,
        "synth with events identical for host blocks 64/1000/37", "peak=" + fmt(peak));
}

void testWavRoundTrip() {
  std::vector<std::vector<float>> ch = {{0.f, 0.5f, -1.f, 0.25f}, {1.f, -0.5f, 0.125f, 0.f}};
  const std::string path = "tonic2_test_roundtrip.wav";
  render::WavData back;
  const bool ok = render::writeWavFloat(path, ch, 44100) && render::readWavFloat(path, back);
  std::remove(path.c_str());
  check(ok && back.sampleRate == 44100 && back.channels == ch, "WAV float32 write/read round trip", ok ? "ok" : "io error");
}

void testNoAllocations() {
  engine::Layout L;
  const int cutoff = L.add("cutoff", 50.f, 10000.f, 800.f, 0.02f);
  const int m = L.add("macro", 0.f, 1.f, 0.f, 0.02f);
  L.macros.push_back({m, {{cutoff, 0.f, 5000.f, 2.f}}});
  engine::Engine e;
  e.prepare(kSr, kMax, L);
  e.setGraph(synth(cutoff));
  std::vector<float> buf(1024);
  float* outs[1] = {buf.data()};
  const Event ev[] = {Event::noteOn(5, 60), Event::param(100, m, 0.7f), Event::param(300, cutoff, 2000.f), Event::noteOff(900, 60)};
  g_audioAllocs = 0;
  tl_audio = true;
  for (int i = 0; i < 500; ++i) {
    e.setParam(m, (i % 10) / 10.f);  // UI writes from the same thread here; still lock- and allocation-free
    e.process(nullptr, 0, outs, 1, 1024, ev, 4);
  }
  tl_audio = false;
  check(g_audioAllocs == 0, "no allocation with events, macros and UI params", std::to_string(g_audioAllocs.load()) + " allocs");
}

}  // namespace

int main() {
  testParamEventsSampleAccurate();
  testSmoothingAndUi();
  testMacros();
  testNotes();
  testMidiCc();
  testParamsSurviveGraphSwap();
  testHostBlockIndependence();
  testWavRoundTrip();
  testNoAllocations();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
