// UI-independent surface logic: main/detail selection, knob mapping, value readouts.
#include <cmath>
#include <cstdio>
#include <string>

#include "demo_synths.hpp"
#include "tonic/ui/surface.hpp"

using namespace tonic;

namespace {
int failures = 0;
void check(bool ok, const std::string& name, const std::string& detail) {
  std::printf("[%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  if (!ok) ++failures;
}
std::string ids(const std::vector<int>& v) {
  std::string s;
  for (int i : v) s += std::to_string(i) + " ";
  return s;
}
}  // namespace

int main() {
  dsl::Patch p;
  demos::polySynth(p);
  const auto layout = p.compile(48000.0, 256).layout;
  // params: 0 cutoff (CC 74), 1 resonance (Detail), 2 env amount (Detail), 3 level (Main), 4 tone, 5 bite (macros)
  const auto s = ui::buildSurface(layout);
  check(s.main == std::vector<int>({3, 4, 5, 0}), "main surface: Main, then macros, then CC-bound", ids(s.main));
  check(s.detail == std::vector<int>({1, 2}), "detail page: the rest", ids(s.detail));
  check(ui::ccFor(layout, 0) == 74 && ui::ccFor(layout, 4) == 71 && ui::ccFor(layout, 1) == -1, "CC lookup", "ok");

  const auto& cutoff = layout.params[0];
  const float mid = ui::fromNormalized(cutoff, 0.5f);
  check(std::fabs(mid - std::sqrt(100.f * 8000.f)) < 0.5f, "log knob: middle is the geometric mean", std::to_string(mid));
  check(std::fabs(ui::toNormalized(cutoff, mid) - 0.5f) < 1e-5f, "log knob round trip", "ok");
  const auto& level = layout.params[3];
  check(ui::toNormalized(level, -18.f) == 0.5f, "linear knob", "ok");
  check(ui::formatValue(cutoff, 1200.f) == "1.20 kHz" && ui::formatValue(cutoff, 900.f) == "900 Hz" &&
            ui::formatValue(level, -6.f) == "-6.0 dB",
        "value readouts", ui::formatValue(cutoff, 1200.f) + ", " + ui::formatValue(level, -6.f));

  engine::Layout many;
  for (int i = 0; i < 12; ++i) {
    engine::ParamSpec sp;
    sp.name = "p" + std::to_string(i);
    sp.ui = engine::UiPlace::Main;
    many.params.push_back(sp);
  }
  many.params[11].ui = engine::UiPlace::Hidden;
  const auto sm = ui::buildSurface(many);
  check(sm.main.size() == 8 && sm.detail.size() == 3, "at most 8 main controls; hidden params omitted",
        std::to_string(sm.main.size()) + " main, " + std::to_string(sm.detail.size()) + " detail");
  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
