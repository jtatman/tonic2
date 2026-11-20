// FX kernel tests (reverb, compressor): measurable behavior. No framework needed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tonic/dsp/dynamics.hpp"
#include "tonic/dsp/reverb.hpp"

using namespace tonic::dsp;

namespace {

int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4g", v); return b; }

constexpr double kSr = 48000.0;

struct IR { std::vector<double> l, r; };

IR reverbIR(float decay, float damping, double seconds) {
  Reverb rv;
  rv.prepare(kSr);
  rv.setDecay(decay);
  rv.setDamping(damping);
  IR ir;
  const size_t n = static_cast<size_t>(seconds * kSr);
  for (size_t i = 0; i < n; ++i) {
    float l, r;
    rv.process(i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f, l, r);
    ir.l.push_back(l);
    ir.r.push_back(r);
  }
  return ir;
}

// Schroeder backward integration, line fit of -5..-25 dB, extrapolated to -60 dB.
double t60(const std::vector<double>& h) {
  std::vector<double> edc(h.size());
  double acc = 0.0;
  for (size_t i = h.size(); i-- > 0;) { acc += h[i] * h[i]; edc[i] = acc; }
  double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < h.size(); ++i) {
    const double db = 10.0 * std::log10(edc[i] / edc[0] + 1e-300);
    if (db > -5.0 || db < -25.0) continue;
    const double x = double(i) / kSr;
    n += 1; sx += x; sy += db; sxx += x * x; sxy += x * db;
  }
  const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  return -60.0 / slope;
}

double hfRatio(const std::vector<double>& h) {  // first-difference (high-pass) energy / total
  double e = 0, hp = 0;
  for (size_t i = 1; i < h.size(); ++i) { e += h[i] * h[i]; const double d = h[i] - h[i - 1]; hp += d * d; }
  return hp / e;
}

void testReverb() {
  for (float decay : {1.0f, 2.5f}) {
    const IR ir = reverbIR(decay, 8000.0f, 10.0);
    const double t = t60(ir.l);
    check(std::fabs(t - decay) < 0.25 * decay, "reverb T60 " + fmt(decay) + "s", "measured " + fmt(t));
  }
  const IR ir = reverbIR(2.0f, 8000.0f, 5.0);
  bool ok = true;
  double peak = 0;
  for (size_t i = 0; i < ir.l.size(); ++i) {
    ok = ok && std::isfinite(ir.l[i]) && std::isfinite(ir.r[i]);
    peak = std::max({peak, std::fabs(ir.l[i]), std::fabs(ir.r[i])});
  }
  check(ok && peak < 4.0, "reverb finite and bounded", "peak " + fmt(peak));

  double sll = 0, srr = 0, slr = 0;
  for (size_t i = 0; i < ir.l.size(); ++i) { sll += ir.l[i] * ir.l[i]; srr += ir.r[i] * ir.r[i]; slr += ir.l[i] * ir.r[i]; }
  const double corr = slr / std::sqrt(sll * srr);
  check(std::fabs(corr) < 0.5, "reverb L/R decorrelated", "corr " + fmt(corr));

  const double dark = hfRatio(reverbIR(2.0f, 2000.0f, 3.0).l);
  const double bright = hfRatio(reverbIR(2.0f, 16000.0f, 3.0).l);
  check(dark < 0.5 * bright, "reverb damping lowers HF", "2k " + fmt(dark) + " vs 16k " + fmt(bright));
}

// Sine at `db` dBFS peak through the compressor; returns peak over the last 0.1 s of `seconds`.
double sinePeakDb(Compressor& c, double db, double seconds) {
  const double a = std::pow(10.0, db / 20.0);
  const size_t n = static_cast<size_t>(seconds * kSr), tail = static_cast<size_t>(0.1 * kSr);
  double pk = 0;
  for (size_t i = 0; i < n; ++i) {
    const float y = c.process(static_cast<float>(a * std::sin(kTwoPi * 1000.0 * double(i) / kSr)));
    if (i >= n - tail) pk = std::max(pk, std::fabs(double(y)));
  }
  return 20.0 * std::log10(pk);
}

Compressor makeComp(float thr, float ratio, float knee, float attack) {
  Compressor c;
  c.prepare(kSr);
  c.setThreshold(thr); c.setRatio(ratio); c.setKnee(knee); c.setAttack(attack); c.setRelease(0.1f); c.setMakeup(0.0f);
  return c;
}

void testCompressor() {
  {
    Compressor c = makeComp(-20.0f, 4.0f, 0.0f, 0.005f);
    const double o = sinePeakDb(c, -6.0, 1.0);
    check(std::fabs(o - (-16.5)) < 0.75, "comp static curve", "out " + fmt(o) + " dB, want -16.5");
  }
  {
    Compressor c = makeComp(-20.0f, 4.0f, 0.0f, 0.005f);
    const double o = sinePeakDb(c, -30.0, 1.0);
    check(std::fabs(o + 30.0) < 0.1, "comp below threshold unity", "out " + fmt(o) + " dB");
  }
  {
    Compressor c = makeComp(-20.0f, 1000.0f, 0.0f, 0.001f);
    const double o = sinePeakDb(c, -6.0, 1.0);
    check(o <= -20.0 + 0.5, "comp limiter ceiling", "out " + fmt(o) + " dB, ceiling -20");
  }
  {
    const float attack = 0.005f;
    Compressor c = makeComp(-20.0f, 4.0f, 0.0f, attack);
    sinePeakDb(c, -40.0, 0.5);
    Compressor ref = c;
    sinePeakDb(ref, -6.0, 1.0);
    const double finalRed = ref.gainReductionDb();
    double tHalf = -1;
    const double a = std::pow(10.0, -6.0 / 20.0);
    for (size_t i = 0; i < static_cast<size_t>(0.1 * kSr); ++i) {
      c.process(static_cast<float>(a * std::sin(kTwoPi * 1000.0 * double(i) / kSr)));
      if (c.gainReductionDb() >= 0.5 * finalRed) { tHalf = double(i + 1) / kSr; break; }
    }
    check(tHalf >= 0.3 * attack && tHalf <= 3.0 * attack, "comp attack time",
          "50% at " + fmt(tHalf * 1000) + " ms, final " + fmt(finalRed) + " dB, attack 5 ms");
  }
  {
    Compressor c = makeComp(-20.0f, 4.0f, 12.0f, 0.0001f);
    float g = 1.0f;
    const float dc = std::pow(10.0f, -20.0f / 20.0f);
    for (int i = 0; i < 48000; ++i) g = c.gainFor(dc);
    const double red = -20.0 * std::log10(double(g));
    check(red > 0.5 && red < 2.0, "comp soft knee at threshold", "reduction " + fmt(red) + " dB");
  }
}

}  // namespace

int main() {
  testReverb();
  testCompressor();
  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
