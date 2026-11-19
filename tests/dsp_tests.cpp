// M1 DSP kernel tests: measurable behavior plus golden-file comparisons. No framework needed.
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "renders.hpp"
#include "tonic/dsp/smoother.hpp"

using namespace tonic::dsp;

namespace {

int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4g", v); return b; }

int zeroCrossings(const std::vector<float>& b) {
  int zc = 0;
  for (size_t i = 1; i < b.size(); ++i) if ((b[i - 1] < 0) != (b[i] < 0)) ++zc;
  return zc;
}

// In-place radix-2 FFT.
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

// Fraction of spectral energy (dB) that is NOT near a true harmonic of f0 (i.e. aliasing).
double aliasDb(const std::vector<float>& x, double f0, double sr) {
  const size_t n = x.size();
  std::vector<std::complex<double>> a(n);
  for (size_t i = 0; i < n; ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(kTwoPi * i / (n - 1)));
  fft(a);
  double harm = 0, alias = 0;
  for (size_t k = 1; k < n / 2; ++k) {
    const double f = k * sr / n, p = std::norm(a[k]);
    const double h = f / f0, d = std::fabs(h - std::round(h)) * f0;  // Hz to nearest harmonic
    (d < 4.0 * sr / n ? harm : alias) += p;
  }
  return 10.0 * std::log10(alias / harm);
}

std::vector<float> naiveSaw(double f0, double sr, size_t n) {
  std::vector<float> y(n);
  double p = 0;
  for (auto& v : y) { v = float(2 * p - 1); p += f0 / sr; if (p >= 1) p -= 1; }
  return y;
}

// Steady-state gain of a filter at a frequency, measured with a sine.
template <class F>
double gainAt(F&& filt, double hz, double sr) {
  Oscillator o; o.prepare(sr); o.setWaveform(Waveform::Sine); o.setFrequency(hz);
  double in2 = 0, out2 = 0;
  const int settle = int(sr * 0.2), n = int(sr * 0.2);
  for (int i = 0; i < settle + n; ++i) {
    float x = o.process(), y = filt(x);
    if (i >= settle) { in2 += x * x; out2 += y * y; }
  }
  return 10.0 * std::log10(out2 / in2);
}

void testOscillatorFrequency() {
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    for (auto w : {Waveform::Sine, Waveform::Saw, Waveform::Square, Waveform::Triangle}) {
      Oscillator o; o.prepare(sr); o.setWaveform(w); o.setFrequency(1000.0);
      o.reset(0.123);  // avoid starting exactly on a zero
      std::vector<float> b(static_cast<size_t>(sr));
      o.process(b.data(), int(b.size()));
      int zc = zeroCrossings(b);
      check(std::abs(zc - 2000) <= 2, "osc freq w=" + std::to_string(int(w)) + " sr=" + fmt(sr),
            "zero crossings=" + std::to_string(zc));
    }
  }
}

void testAliasing() {
  const double sr = 48000, f0 = 3517.3;
  const size_t n = 16384;
  const double naive = aliasDb(naiveSaw(f0, sr, n), f0, sr);
  Oscillator o; o.prepare(sr); o.setWaveform(Waveform::Saw); o.setFrequency(f0);
  std::vector<float> b(n); o.process(b.data(), int(n));
  const double blep = aliasDb(b, f0, sr);
  check(blep < naive - 10.0, "saw aliasing PolyBLEP vs naive", fmt(blep) + " dB vs " + fmt(naive) + " dB");
  // Thresholds reflect 2-sample PolyBLEP/BLAMP (steps ~-25 dB, corners ~-40 dB at 3.5 kHz);
  // a future minBLEP oscillator should tighten these.
  o.setWaveform(Waveform::Square); o.reset();
  o.process(b.data(), int(n));
  const double sq = aliasDb(b, f0, sr);
  check(sq < -22.0, "square aliasing @3.5 kHz", fmt(sq) + " dB");
  o.setWaveform(Waveform::Triangle); o.reset();
  o.process(b.data(), int(n));
  const double tri = aliasDb(b, f0, sr);
  check(tri < -35.0, "triangle aliasing @3.5 kHz", fmt(tri) + " dB");
}

void testSvf() {
  const double sr = 48000;
  Svf f; f.prepare(sr); f.setCutoff(1000); f.setQ(0.70710678);
  auto lp = [&](float x) { return f.processLow(x); };
  const double atFc = gainAt(lp, 1000, sr);
  f.reset();
  const double octUp = gainAt(lp, 2000, sr);
  f.reset();
  const double pass = gainAt(lp, 100, sr);
  check(std::fabs(atFc + 3.01) < 0.3, "SVF LP -3 dB at cutoff", fmt(atFc) + " dB");
  check(std::fabs(pass) < 0.1, "SVF LP passband flat", fmt(pass) + " dB");
  check(octUp < -11.0 && octUp > -14.5, "SVF LP 12 dB/oct", "2 kHz: " + fmt(octUp) + " dB");
}

void testLadder() {
  const double sr = 48000;
  Ladder l; l.prepare(sr); l.setCutoff(1000); l.setResonance(0.0); l.setDrive(0.1);  // linear region
  auto f = [&](float x) { return l.process(x); };
  const double oct1 = gainAt(f, 2000, sr); l.reset();
  const double oct2 = gainAt(f, 4000, sr); l.reset();
  const double pass = gainAt(f, 100, sr);
  const double slope = oct2 - oct1;
  check(slope < -20.0 && slope > -26.0, "ladder ~24 dB/oct", "2k->4k: " + fmt(slope) + " dB");
  check(pass > -1.0, "ladder passband", fmt(pass) + " dB");
  // Stability: full resonance, hot input, fast cutoff modulation -> finite and bounded.
  Ladder h; h.prepare(sr); h.setResonance(1.0); h.setDrive(4.0);
  WhiteNoise n(7);
  float peak = 0; bool finite = true;
  for (int i = 0; i < 48000; ++i) {
    h.setCutoff(50 + 15000 * (0.5 + 0.5 * std::sin(i * 0.01)));
    float y = h.process(n.process());
    finite &= std::isfinite(y); peak = std::max(peak, std::fabs(y));
  }
  check(finite && peak < 10.0f, "ladder stable at full resonance + modulation", "peak=" + fmt(peak));
}

void testAdsr() {
  for (double sr : {44100.0, 96000.0}) {
    Adsr e; e.prepare(sr); e.set(0.5, 0.1, 0.5, 0.25); e.noteOn();
    int i = 0;
    while (e.stage() == Adsr::Stage::Attack && i < sr * 2) { e.process(); ++i; }
    check(std::abs(i - sr * 0.5) <= 2, "ADSR attack time sr=" + fmt(sr), std::to_string(i) + " samples");
    for (int k = 0; k < sr; ++k) e.process();
    check(std::fabs(e.process() - 0.5f) < 1e-6f, "ADSR sustain level", "ok");
    e.noteOff();
    int r = 0;
    while (e.isActive() && r < sr * 2) { e.process(); ++r; }
    // Release time is defined from 1.0; from sustain 0.5 it scales by log((L+r)/r)/log((1+r)/r).
    const double ratio = 0.0001, expect = sr * 0.25 * std::log((0.5 + ratio) / ratio) / std::log((1 + ratio) / ratio);
    check(std::abs(r - expect) <= 2, "ADSR release time sr=" + fmt(sr),
          std::to_string(r) + " samples, expect " + fmt(expect));
  }
}

void testSmoother() {
  OnePoleSmoother s; s.prepare(48000); s.setTime(0.01); s.reset(0); s.setTarget(1);
  float v = 0;
  for (int i = 0; i < 480; ++i) v = s.next();
  check(std::fabs(v - 0.632f) < 0.01f, "one-pole smoother 63% at tau", fmt(v));
  LinearRamp r; r.reset(0); r.setTarget(1, 100);
  for (int i = 0; i < 99; ++i) r.next();
  check(r.isRamping() && r.next() == 1.0f && !r.isRamping(), "linear ramp exact arrival", "ok");
}

void testBlockEquivalence() {
  Oscillator a, b;
  for (auto* o : {&a, &b}) { o->prepare(48000); o->setWaveform(Waveform::Square); o->setFrequency(777); }
  std::vector<float> x(10000), y(10000);
  for (auto& v : x) v = a.process();
  int pos = 0, sizes[] = {1, 7, 64, 513, 3, 128};
  for (int k = 0; pos < 10000; ++k) {
    int n = std::min(sizes[k % 6], 10000 - pos);
    b.process(y.data() + pos, n);
    pos += n;
  }
  check(x == y, "per-sample == variable block sizes", "bit-exact");
}

void testGolden() {
  for (const auto& r : renders::all()) {
    const std::string path = std::string(TONIC2_GOLDEN_DIR) + "/" + r.name + ".f32";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { check(false, "golden " + r.name, "missing " + path + " (run tonic2_golden --update)"); continue; }
    std::vector<float> ref(renders::kFrames);
    const size_t got = std::fread(ref.data(), sizeof(float), ref.size(), f);
    std::fclose(f);
    const auto out = r.fn();
    double maxDiff = got == ref.size() ? 0 : 1e9;
    for (size_t i = 0; i < out.size() && got == ref.size(); ++i)
      {
        double d = std::fabs(out[i] - ref[i]);
        if (r.shiftTolerant) {
          if (i > 0) d = std::min(d, double(std::fabs(out[i] - ref[i - 1])));
          if (i + 1 < ref.size()) d = std::min(d, double(std::fabs(out[i] - ref[i + 1])));
        }
        maxDiff = std::max(maxDiff, d);
      }
    check(maxDiff < 1e-4, "golden " + r.name, "max diff=" + fmt(maxDiff));
  }
}

}  // namespace

int main() {
  testOscillatorFrequency();
  testAliasing();
  testSvf();
  testLadder();
  testAdsr();
  testSmoother();
  testBlockEquivalence();
  testGolden();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
