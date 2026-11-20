// Tests for sound matching: FFT, STFT distance, CMA-ES and recovery of known patch parameters.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "demo_synths.hpp"
#include "tonic/analysis/match.hpp"

using namespace tonic;
using namespace tonic::analysis;

namespace {
int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string fmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4g", v); return b; }
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double sr, int frames, float gain = 0.5f) {
  std::vector<float> x(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i)] = gain * static_cast<float>(std::sin(2.0 * kPi * hz * i / sr));
  return x;
}

void testFft() {
  NormalSource normal(7);
  std::vector<std::complex<double>> x(64);
  for (auto& v : x) v = {normal(), normal()};
  auto y = x;
  fft(y);
  double err = 0.0;
  for (size_t k = 0; k < x.size(); ++k) {
    std::complex<double> s = 0.0;
    for (size_t n = 0; n < x.size(); ++n) s += x[n] * std::polar(1.0, -2.0 * kPi * double(k * n) / double(x.size()));
    err = std::max(err, std::abs(s - y[k]));
  }
  check(err < 1e-9, "FFT matches a direct DFT (64 points)", "max error " + fmt(err));
}

void testDistance() {
  const double sr = 48000.0;
  const int n = 24000;
  const auto a = sine(440, sr, n);
  const double self = stftDistance(a, a);
  check(self == 0.0, "distance of a signal to itself is 0", fmt(self));
  const double d5 = stftDistance(a, sine(445, sr, n)), d80 = stftDistance(a, sine(520, sr, n)), d440 = stftDistance(a, sine(880, sr, n));
  check(d5 > 0.0 && d5 < d80 && d80 <= d440 * 1.05, "distance grows with detuning (445 < 520 <= 880 Hz vs 440)",
        fmt(d5) + " " + fmt(d80) + " " + fmt(d440));
  const double dGain = stftDistance(a, sine(440, sr, n, 0.25f));
  check(dGain > 0.0 && dGain < d80, "a 6 dB level change costs less than a pitch change", fmt(dGain) + " < " + fmt(d80));
  const double dShort = stftDistance(a, std::vector<float>(a.begin(), a.begin() + n / 2));
  check(dShort > dGain, "a signal cut short in half is far from the original", fmt(dShort));
}

void testCmaes() {
  const std::vector<double> centre{0.3, 0.7, 0.45, 0.6, 0.2};
  auto sphere = [&](const std::vector<double>& x) {
    double s = 0.0;
    for (size_t i = 0; i < x.size(); ++i) s += (x[i] - centre[i]) * (x[i] - centre[i]);
    return s;
  };
  CmaesOptions opt;
  opt.maxEvals = 3000;
  opt.seed = 3;
  const auto r = minimizeCmaes(std::vector<double>(5, 0.5), sphere, opt);
  check(r.bestValue < 1e-8, "CMA-ES finds the minimum of a 5-D sphere", fmt(r.bestValue) + " after " + std::to_string(r.evals) + " evals");

  // A corner minimum: the box repair must let the search sit on the boundary.
  auto corner = [](const std::vector<double>& x) { return (1.0 - x[0]) * (1.0 - x[0]) + x[1] * x[1]; };
  const auto rc = minimizeCmaes({0.5, 0.5}, corner, opt);
  check(rc.bestValue < 1e-8, "CMA-ES reaches a minimum on the box corner (1, 0)", fmt(rc.bestValue));

  // Rosenbrock (scaled into the box, minimum at (0.75, 0.75)): curved valley, needs adaptation.
  auto rosen = [](const std::vector<double>& x) {
    const double a = 4.0 * x[0] - 2.0, b = 4.0 * x[1] - 2.0;
    return (1.0 - a) * (1.0 - a) + 100.0 * (b - a * a) * (b - a * a);
  };
  const auto rr = minimizeCmaes({0.2, 0.2}, rosen, opt);
  check(rr.bestValue < 1e-6, "CMA-ES solves a 2-D Rosenbrock valley", fmt(rr.bestValue));

  const auto again = minimizeCmaes(std::vector<double>(5, 0.5), sphere, opt);
  CmaesOptions par = opt;
  par.threads = 4;
  const auto threaded = minimizeCmaes(std::vector<double>(5, 0.5), sphere, par);
  check(again.best == r.best && threaded.best == r.best && threaded.bestValue == r.bestValue,
        "CMA-ES is deterministic for a seed, with 1 or 4 threads", "");
}

// Render the mono demo with known cutoff and resonance, then search for them from the defaults.
void testRecovery() {
  const auto demoList = demos::all();
  const demos::Demo* mono = nullptr;
  for (const auto& d : demoList) if (d.name == "mono") mono = &d;
  MatchSpec spec;
  spec.build = mono->build;
  spec.sampleRate = 48000.0;
  spec.events = mono->script(spec.sampleRate);
  spec.frames = static_cast<int>(0.75 * spec.sampleRate);  // the first three notes
  const auto layout = layoutOf(spec.build);
  const int cutoff = paramId(layout, "cutoff"), resonance = paramId(layout, "resonance");
  check(cutoff >= 0 && resonance >= 0, "mono demo has cutoff and resonance parameters", "");
  spec.paramIds = {cutoff, resonance};

  const std::vector<float> truth{2400.f, 0.7f};
  const auto target = analyse(renderWith(spec, spec.paramIds, truth));
  const auto again = renderWith(spec, spec.paramIds, truth);
  check(stftDistance(target, again) == 0.0, "renders are deterministic (same values, distance 0)", "");

  CmaesOptions opt;
  opt.maxEvals = 200;
  opt.seed = 11;
  opt.threads = 4;
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = matchSound(spec, target, opt);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto& cs = layout.params[static_cast<size_t>(cutoff)];
  const double cutErr = std::fabs(ui::toNormalized(cs, r.values[0]) - ui::toNormalized(cs, truth[0]));
  const double resErr = std::fabs(r.values[1] - truth[1]);
  check(r.distance < 0.2 * r.startDistance, "matching cuts the distance to under 20% of the defaults' distance",
        fmt(r.startDistance) + " -> " + fmt(r.distance) + ", " + std::to_string(r.search.evals) + " evals in " + fmt(secs) + " s");
  check(cutErr < 0.05 && resErr < 0.1, "matching recovers cutoff and resonance",
        "cutoff " + fmt(r.values[0]) + " (truth 2400), resonance " + fmt(r.values[1]) + " (truth 0.7)");
  check(r.layout.params[static_cast<size_t>(cutoff)].def == r.values[0], "result layout carries the matched values as defaults", "");
}
}  // namespace

int main() {
  testFft();
  testDistance();
  testCmaes();
  testRecovery();
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
