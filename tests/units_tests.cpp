// Ported Tonic 1 units: pink noise, metro, stepper, sample & hold, LFNoise, delay lines, ping-pong delay,
// bitcrusher. Each check measures a defining property.
#include <cmath>
#include <complex>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "tonic/dsp/delay.hpp"
#include "tonic/dsp/sequencing.hpp"

using namespace tonic::dsp;

namespace {
int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4g", v); return b; }
constexpr double kSr = 48000.0;

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const auto w = std::polar(1.0, -kTwoPi / double(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> wn(1.0);
      for (size_t k = 0; k < len / 2; ++k, wn *= w) {
        auto u = a[i + k], v = a[i + k + len / 2] * wn;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
      }
    }
  }
}

// Average band energy (dB) of a noise source over several FFT frames.
template <class Gen>
double bandDb(Gen&& gen, double lo, double hi) {
  const size_t n = 8192;
  double e = 0;
  for (int frame = 0; frame < 16; ++frame) {
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = gen() * (0.5 - 0.5 * std::cos(kTwoPi * i / (n - 1)));
    fft(a);
    for (size_t k = 1; k < n / 2; ++k) {
      const double f = k * kSr / n;
      if (f >= lo && f < hi) e += std::norm(a[k]);
    }
  }
  return 10.0 * std::log10(e);
}

void testPinkNoise() {
  PinkNoise pink(1);
  WhiteNoise white(1);
  auto p = [&] { return double(pink.process()); };
  auto w = [&] { return double(white.process()); };
  const double pinkDiff = bandDb(p, 3200, 6400) - bandDb(p, 200, 400);
  const double whiteDiff = bandDb(w, 3200, 6400) - bandDb(w, 200, 400);
  check(std::fabs(pinkDiff) < 2.0, "pink noise: equal energy per octave (200-400 vs 3.2k-6.4k Hz)", fmt(pinkDiff) + " dB");
  check(std::fabs(whiteDiff - 12.0) < 1.5, "white noise reference: +3 dB per octave band", fmt(whiteDiff) + " dB");
}

void testMetroStepperSampleHold() {
  Metro m; m.prepare(kSr); m.reset();
  std::vector<int> ticks;
  for (int i = 0; i < 96000; ++i) if (m.process(120.0) > 0.f) ticks.push_back(i);
  check(ticks == std::vector<int>({0, 24000, 48000, 72000}), "metro: 120 bpm ticks every 0.5 s",
        std::to_string(ticks.size()) + " ticks");
  Metro s; s.prepare(kSr); s.reset();
  int n16 = 0;
  for (int i = 0; i < 48000; ++i) n16 += s.process(120.0, 4.0) > 0.f;
  check(n16 == 8, "metro: division 4 = sixteenths (8 per second at 120 bpm)", std::to_string(n16));

  Stepper st; st.setSteps({1.f, 2.f, 3.f}); st.reset();
  std::vector<float> seq;
  for (int i = 0; i < 4; ++i) { seq.push_back(st.process(1.f)); st.process(0.f); }
  check(seq == std::vector<float>({1.f, 2.f, 3.f, 1.f}), "stepper advances on triggers and wraps", "1 2 3 1");

  SampleHold a(7), b(7);
  bool inRange = true, held = true, same = true;
  float prev = a.process(1.f, -2.f, 5.f);
  b.process(1.f, -2.f, 5.f);
  for (int i = 1; i < 2000; ++i) {
    const float trig = (i % 100 == 0) ? 1.f : 0.f;
    const float v = a.process(trig, -2.f, 5.f), w = b.process(trig, -2.f, 5.f);
    inRange &= v >= -2.f && v <= 5.f;
    if (trig == 0.f) held &= v == prev;
    same &= v == w;
    prev = v;
  }
  check(inRange && held && same, "sample & hold: in range, holds between triggers, deterministic per seed", "ok");
}

void testLfNoise() {
  LfNoise step(3); step.prepare(kSr); step.setSmooth(false);
  std::set<float> values;
  for (int i = 0; i < 48000; ++i) values.insert(step.process(10.0));
  check(values.size() >= 9 && values.size() <= 11, "LFNoise stepped: ~10 values per second at 10 Hz", std::to_string(values.size()));
  LfNoise smooth(3); smooth.prepare(kSr);
  float prev = smooth.process(10.0), maxStep = 0.f;
  for (int i = 1; i < 48000; ++i) { const float v = smooth.process(10.0); maxStep = std::max(maxStep, std::fabs(v - prev)); prev = v; }
  check(maxStep < 2.0f * 10.0f / 48000.f * 1.01f, "LFNoise smooth: ramps (no jumps)", "max step " + fmt(maxStep));
}

void testDelay() {
  DelayLine d; d.prepare(kSr, 0.1);
  std::vector<float> out(400);
  for (int i = 0; i < 400; ++i) { out[static_cast<size_t>(i)] = d.read(100.0); d.write(i == 0 ? 1.f : 0.f); }
  check(out[100] == 1.f && out[99] == 0.f && out[101] == 0.f, "delay line: integer delay is exact", "ok");
  DelayLine h; h.prepare(kSr, 0.1);
  std::vector<float> half(400);
  for (int i = 0; i < 400; ++i) { half[static_cast<size_t>(i)] = h.read(100.5); h.write(i == 0 ? 1.f : 0.f); }
  check(std::fabs(half[100] - half[101]) < 1e-6f && half[100] > 0.5f, "delay line: half-sample delay splits the impulse", fmt(half[100]));

  DelayLine m; m.prepare(kSr, 0.1);
  float prev = 0.f, maxStep = 0.f;
  for (int i = 0; i < 48000; ++i) {
    const float x = static_cast<float>(std::sin(kTwoPi * 220.0 * i / kSr));
    const double t = (0.02 + 0.005 * std::sin(kTwoPi * 0.5 * i / kSr)) * kSr;  // chorus-like modulation
    const float y = m.read(t);
    m.write(x);
    if (i > 2000) maxStep = std::max(maxStep, std::fabs(y - prev));
    prev = y;
  }
  check(maxStep < 0.04f, "delay line: modulated time stays smooth (no zipper)", "max step " + fmt(maxStep));

  StereoDelay pp; pp.prepare(kSr, 1.0); pp.setPingPong(true);
  std::vector<float> L(30000), R(30000);
  for (int i = 0; i < 30000; ++i) pp.process(i == 0 ? 1.f : 0.f, 0.f, 0.1, 0.1, 0.5f, L[static_cast<size_t>(i)], R[static_cast<size_t>(i)]);
  check(L[4800] > 0.4f && std::fabs(R[4800]) < 1e-6f && R[9600] > 0.2f && std::fabs(L[9600]) < 1e-6f,
        "ping-pong: first echo left, second right", fmt(L[4800]) + " / " + fmt(R[9600]));
}

void testBitCrusher() {
  BitCrusher c; c.prepare(kSr);
  std::set<float> levels;
  for (int i = 0; i < 48000; ++i) levels.insert(c.process(static_cast<float>(std::sin(kTwoPi * 100.0 * i / kSr)), 4.f, kSr));
  check(levels.size() <= 17, "bitcrusher 4 bits: at most 2^4 + 1 levels", std::to_string(levels.size()));
  BitCrusher r; r.prepare(kSr);
  float prev = 0.f;
  int changes = 0;
  for (int i = 0; i < 48000; ++i) {
    const float v = r.process(static_cast<float>(std::sin(kTwoPi * 3000.0 * i / kSr)), 24.f, 1000.0);
    changes += v != prev;
    prev = v;
  }
  check(changes <= 1001, "bitcrusher rate 1 kHz: output changes at most 1000 times per second", std::to_string(changes));
}
}  // namespace

int main() {
  testPinkNoise();
  testMetroStepperSampleHold();
  testLfNoise();
  testDelay();
  testBitCrusher();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
