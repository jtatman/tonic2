// M6 tests: channel strip (pan laws, gain, click-free mute), solo logic, sends (pre/post), groups,
// FX returns, meters (incl. read from another thread), FX matrix routing, and no audio-thread allocation.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "alloc_tracker.hpp"
#include "tonic/dsl/mixer.hpp"
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
bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) < tol; }

constexpr double kSr = 48000.0;
constexpr int kMax = 128;

struct Rendered {
  std::vector<std::vector<float>> out;
  std::string error;
};
Rendered render(dsl::Patch& p, int frames, std::vector<Event> ev = {}) {
  Rendered r;
  auto c = p.compile(kSr, kMax);
  if (!c.graph) { r.error = c.error; return r; }
  engine::Engine e;
  e.prepare(kSr, kMax, c.layout);
  e.setGraph(std::move(c.graph));
  r.out = render::renderOffline(e, 2, frames, ev);
  return r;
}

void testPanLawsAndGain() {
  struct Case { dsl::PanLaw law; float pan; float l, r; const char* name; };
  const Case cases[] = {
      {dsl::PanLaw::ConstantPower, 0.f, 0.70710678f, 0.70710678f, "constant power centre -3 dB"},
      {dsl::PanLaw::Linear, 0.f, 0.5f, 0.5f, "linear centre -6 dB"},
      {dsl::PanLaw::Compromise, 0.f, 0.59460356f, 0.59460356f, "compromise centre -4.5 dB"},
      {dsl::PanLaw::ConstantPower, -1.f, 1.f, 0.f, "hard left"},
  };
  for (const Case& c : cases) {
    dsl::Patch p;
    dsl::Mixer mix(p);
    mix.strip(p.constant(1.f), {.pan = c.pan, .law = c.law});
    p.output(mix.master());
    auto r = render(p, 64);
    check(r.error.empty() && near(r.out[0][10], c.l) && near(r.out[1][10], c.r), c.name,
          r.error.empty() ? fmt(r.out[0][10]) + " / " + fmt(r.out[1][10]) : r.error);
  }
  dsl::Patch p;
  dsl::Mixer mix(p);
  mix.strip(p.constant(1.f), {.gainDb = -6.0206f, .pan = -1.f});
  p.output(mix.master({.gainDb = -6.0206f}));
  auto r = render(p, 64);
  check(near(r.out[0][10], 0.25f), "strip and master gain in dB", fmt(r.out[0][10]));

  dsl::Patch s;
  dsl::Mixer sm(s);
  sm.strip(s.constant(1.f) >> dsl::pan(-1.f), {.pan = 0.5f});  // stereo input: pan = balance
  s.output(sm.master());
  auto sr = render(s, 64);
  check(near(sr.out[0][10], 0.5f) && near(sr.out[1][10], 0.f), "stereo input: pan acts as balance",
        fmt(sr.out[0][10]) + " / " + fmt(sr.out[1][10]));
}

void testMuteIsSmooth() {
  dsl::Patch p;
  auto mute = p.param("mute", 0.f, 1.f, 0.f, 0.f);  // unsmoothed: the strip itself must fade
  dsl::Mixer mix(p);
  mix.strip(p.constant(1.f), {.pan = -1.f, .mute = mute});
  p.output(mix.master());
  auto r = render(p, 4000, {Event::param(1000, mute.id, 1.f)});
  const float before = r.out[0][999], first = r.out[0][1000], after = r.out[0][1000 + 2400];
  check(before == 1.f && first < 1.f && first > 0.9f && after < 1e-3f, "mute fades over ~5 ms (no click)",
        fmt(before) + " -> " + fmt(first) + " -> " + fmt(after));
}

void testSoloAndSends() {
  dsl::Patch p;
  auto soloA = p.param("soloA", 0.f, 1.f, 0.f, 0.f);
  dsl::Mixer mix(p);
  auto ret = mix.fxReturn([](dsl::Signal in) { return in * 1.f; });  // pass-through return
  auto a = mix.strip(p.constant(1.f), {.pan = -1.f, .solo = soloA});
  auto b = mix.strip(p.constant(1.f), {.pan = 1.f});
  mix.send(b, ret, 0.5f);  // post-fader send of B (hard right) to the return
  p.output(mix.master());
  auto r = render(p, 6000, {Event::param(1000, soloA.id, 1.f)});
  // Before solo: L = A (1), R = B (1) + return (0.5). After solo A: B muted; the return is solo-safe but
  // is fed post-fader from B, so it falls silent too.
  check(near(r.out[0][500], 1.f) && near(r.out[1][500], 1.5f), "no solo: all strips + return", fmt(r.out[1][500]));
  check(near(r.out[0][5000], 1.f) && r.out[1][5000] < 1e-3f, "solo A mutes B (and its post-fader send)",
        fmt(r.out[1][5000]));
  (void)a;

  dsl::Patch q;
  auto gainB = q.param("gainB", -120.f, 0.f, 0.f, 0.f);
  dsl::Mixer mq(q);
  auto retPre = mq.fxReturn([](dsl::Signal in) { return in * 1.f; });
  auto retPost = mq.fxReturn([](dsl::Signal in) { return in * 1.f; }, {.pan = 0.f});
  auto sb = mq.strip(q.constant(1.f), {.gainDb = gainB, .pan = 1.f});
  mq.send(sb, retPre, 1.f, /*preFader=*/true);
  mq.send(sb, retPost, 1.f);
  q.output(mq.master());
  auto rq = render(q, 2000, {Event::param(500, gainB.id, -120.f)});
  // After the fader closes only the pre-fader send remains: R = 1 (pre) from the hard-right strip.
  check(near(rq.out[1][200], 3.f) && near(rq.out[1][1500], 1.f, 1e-3f), "pre-fader send ignores the fader",
        fmt(rq.out[1][200]) + " -> " + fmt(rq.out[1][1500]));
}

void testGroupsAndMeters() {
  dsl::Patch p;
  dsl::Mixer mix(p);
  auto grp = mix.group({.gainDb = -6.0206f});
  auto s1 = mix.strip(p.constant(0.25f), {.pan = -1.f}, &grp);
  mix.strip(p.constant(0.75f), {.pan = -1.f}, &grp);
  p.output(mix.master());
  auto c = p.compile(kSr, kMax);
  engine::Engine e;
  e.prepare(kSr, kMax, c.layout);
  e.setGraph(std::move(c.graph));
  auto out = render::renderOffline(e, 2, 256, {});
  check(near(out[0][100], 0.5f), "group sums its strips and applies its gain", fmt(out[0][100]));
  const float m1 = s1.meter->readAndReset(), mg = grp.strip.meter->readAndReset(), mm = mix.masterMeter()->readAndReset();
  check(near(m1, 0.25f) && near(mg, 0.5f) && near(mm, 0.5f) && s1.meter->readAndReset() == 0.f,
        "meters report peaks and reset on read", fmt(m1) + ", " + fmt(mg) + ", " + fmt(mm));
}

void testFxMatrix() {
  auto build = [](std::vector<dsl::FxRoute> routes, std::string* err = nullptr) {
    dsl::Patch p;
    std::vector<std::function<dsl::Signal(dsl::Signal)>> slots = {
        [](dsl::Signal s) { return s * 2.f; }, [](dsl::Signal s) { return s * 3.f; }};
    p.output(dsl::fxMatrix(p.constant(1.f), slots, routes));
    auto r = render(p, 64);
    if (err) *err = r.error;
    return r.error.empty() ? r.out[0][10] : -1.f;
  };
  using dsl::kFxIn, dsl::kFxOut;
  check(build({{kFxIn, 0}, {0, 1}, {1, kFxOut}}) == 6.f, "fx matrix: serial chain", "x2 then x3");
  check(build({{kFxIn, 0}, {kFxIn, 1}, {0, kFxOut}, {1, kFxOut}}) == 5.f, "fx matrix: parallel", "x2 + x3");
  check(build({{kFxIn, kFxOut, 0.5f}, {kFxIn, 1}, {1, kFxOut, 0.5f}}) == 2.f, "fx matrix: dry + one slot, gains",
        "0.5 + 1.5");
  std::string err;
  build({{kFxIn, 1}, {1, 0}, {0, kFxOut}}, &err);
  check(err.find("forward") != std::string::npos, "fx matrix: backward route rejected", err);

  // Routing crossfade driven by a smoothed parameter: serial <-> parallel morph.
  dsl::Patch p;
  auto morph = p.param("morph", 0.f, 1.f, 0.f, 0.01f);
  std::vector<std::function<dsl::Signal(dsl::Signal)>> slots = {
      [](dsl::Signal s) { return s * 2.f; }, [](dsl::Signal s) { return s * 3.f; }};
  p.output(dsl::fxMatrix(p.constant(1.f), slots,
                         {{kFxIn, 0}, {0, 1, 1.f - morph}, {kFxIn, 1, morph}, {0, kFxOut, morph}, {1, kFxOut}}));
  auto r = render(p, 8000, {Event::param(1000, morph.id, 1.f)});
  // morph 0: 1*2*3 = 6; morph 1: 2 + 3 = 5; in between it glides (10 ms smoothing, checked ~14 tau later).
  const float mid = r.out[0][1000 + 300];
  check(r.error.empty() && r.out[0][900] == 6.f && near(r.out[0][7900], 5.f, 1e-4f) && mid < 6.f && mid > 5.f,
        "fx matrix routing crossfades with a smoothed param", fmt(r.out[0][900]) + " -> " + fmt(mid) + " -> " + fmt(r.out[0][7900]));
}

void testRealtimeAndMeterThreads() {
  dsl::Patch p;
  auto g = p.param("g", -60.f, 0.f, -6.f);
  dsl::Mixer mix(p);
  auto ret = mix.fxReturn([](dsl::Signal in) { return in >> dsl::delay(0.2, 0.4f, 1.f); });
  auto st = mix.strip(p.saw(110.f), {.gainDb = g, .pan = -0.4f});
  mix.strip(p.saw(165.f) >> dsl::lowpass(800.f), {.gainDb = -9.f, .pan = 0.4f});
  mix.send(st, ret, 0.3f);
  p.output(mix.master());
  auto c = p.compile(kSr, kMax);
  engine::Engine e;
  e.prepare(kSr, kMax, c.layout);
  e.setGraph(std::move(c.graph));
  std::vector<float> l(kMax), r(kMax);
  float* outs[2] = {l.data(), r.data()};
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::thread ui([&] {
    float peak = 0.f;
    while (!stop.load()) { peak = std::max(peak, st.meter->readAndReset()); ++reads; std::this_thread::yield(); }
    (void)peak;
  });
  g_audioAllocs = 0;
  tl_audio = true;
  for (int i = 0; i < 3000; ++i) {
    const Event ev[] = {Event::param(i % kMax, g.id, -6.f - static_cast<float>(i % 20))};
    e.process(nullptr, 0, outs, 2, kMax, ev, 1);
  }
  tl_audio = false;
  stop = true;
  ui.join();
  check(g_audioAllocs == 0 && reads > 0, "mixer + return: no audio allocations; meters read concurrently",
        std::to_string(g_audioAllocs.load()) + " allocs, " + std::to_string(reads.load()) + " UI reads");
}

}  // namespace

int main() {
  testPanLawsAndGain();
  testMuteIsSmooth();
  testSoloAndSends();
  testGroupsAndMeters();
  testFxMatrix();
  testRealtimeAndMeterThreads();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
