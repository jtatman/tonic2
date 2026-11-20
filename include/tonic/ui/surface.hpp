// UI-independent control-surface logic: which parameters a minimal UI (or a small MIDI controller)
// shows up front, how knob positions map to values, and how values read. No GUI toolkit here, so this
// is shared by the DPF UI, tests, and future front-ends.
//
// Main surface (up to 8 controls), in this order:
//   1. parameters marked UiPlace::Main
//   2. macros (UiPlace::Auto)
//   3. parameters bound to a MIDI CC (UiPlace::Auto)
// Everything else that is not Hidden goes to the detail page.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../engine/params.hpp"

namespace tonic::ui {

struct Surface {
  std::vector<int> main;    // parameter ids for the main knobs, in display order
  std::vector<int> detail;  // parameter ids for the detail page
};

inline Surface buildSurface(const engine::Layout& L, int maxMain = 8) {
  Surface s;
  const int n = static_cast<int>(L.params.size());
  auto isMacro = [&](int id) {
    return std::any_of(L.macros.begin(), L.macros.end(), [&](const engine::MacroSpec& m) { return m.param == id; });
  };
  auto hasCc = [&](int id) {
    return std::any_of(L.ccBindings.begin(), L.ccBindings.end(), [&](const engine::CcBinding& b) { return b.param == id; });
  };
  auto place = [&](int id) { return L.params[static_cast<size_t>(id)].ui; };
  auto take = [&](auto pred) {
    for (int id = 0; id < n && static_cast<int>(s.main.size()) < maxMain; ++id)
      if (pred(id) && std::find(s.main.begin(), s.main.end(), id) == s.main.end()) s.main.push_back(id);
  };
  take([&](int id) { return place(id) == engine::UiPlace::Main; });
  take([&](int id) { return place(id) == engine::UiPlace::Auto && isMacro(id); });
  take([&](int id) { return place(id) == engine::UiPlace::Auto && hasCc(id); });
  for (int id = 0; id < n; ++id)
    if (place(id) != engine::UiPlace::Hidden && std::find(s.main.begin(), s.main.end(), id) == s.main.end())
      s.detail.push_back(id);
  return s;
}

// Knob position 0..1 <-> plain value (logarithmic when spec.log and min > 0).
inline float toNormalized(const engine::ParamSpec& p, float v) {
  if (p.max <= p.min) return 0.f;
  v = std::clamp(v, p.min, p.max);
  if (p.log && p.min > 0.f) return std::log(v / p.min) / std::log(p.max / p.min);
  return (v - p.min) / (p.max - p.min);
}
inline float fromNormalized(const engine::ParamSpec& p, float t) {
  t = std::clamp(t, 0.f, 1.f);
  if (p.log && p.min > 0.f) return p.min * std::pow(p.max / p.min, t);
  return p.min + t * (p.max - p.min);
}

// Short readout: "1.20 kHz", "-6.0 dB", "45 %", "0.35".
inline std::string formatValue(const engine::ParamSpec& p, float v) {
  char b[48];
  if (p.unit == "Hz" && std::fabs(v) >= 1000.f) std::snprintf(b, sizeof b, "%.2f kHz", v / 1000.f);
  else if (p.unit == "Hz") std::snprintf(b, sizeof b, "%.0f Hz", v);
  else if (p.unit == "dB") std::snprintf(b, sizeof b, "%.1f dB", v);
  else if (p.unit == "%") std::snprintf(b, sizeof b, "%.0f %%", v);
  else if (p.unit == "s") std::snprintf(b, sizeof b, v < 1.f ? "%.0f ms" : "%.2f s", v < 1.f ? v * 1000.f : v);
  else if (!p.unit.empty()) std::snprintf(b, sizeof b, "%.2f %s", v, p.unit.c_str());
  else std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

// MIDI CC bound to a parameter in the layout, or -1.
inline int ccFor(const engine::Layout& L, int id) {
  for (const auto& b : L.ccBindings) if (b.param == id) return b.cc;
  return -1;
}

inline bool isMacro(const engine::Layout& L, int id) {
  return std::any_of(L.macros.begin(), L.macros.end(), [&](const engine::MacroSpec& m) { return m.param == id; });
}

}  // namespace tonic::ui
