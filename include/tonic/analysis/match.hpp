// Sound matching: find parameter values for a patch so that its render sounds like a target recording.
// The patch is rendered offline with a fixed event script (the notes to play); a CMA-ES search moves the
// chosen parameters through their normalised ranges (log-scaled where the ParamSpec says so) and keeps
// the values with the smallest multi-resolution STFT distance to the target.
//
// Every evaluation compiles a fresh graph and engine, so no state leaks between candidates and the
// search can evaluate in parallel. Offline / control thread only.
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../dsl/patch.hpp"
#include "../render/offline.hpp"
#include "../ui/surface.hpp"
#include "cmaes.hpp"
#include "spectral.hpp"

namespace tonic::analysis {

struct MatchSpec {
  std::function<void(dsl::Patch&)> build;
  std::vector<engine::Event> events;  // any order; Param events for searched parameters are ignored
  double sampleRate = 48000.0;
  int frames = 48000;
  int maxBlock = 512;
  std::vector<int> paramIds;          // parameters to search; empty = all of them
};

// The patch's parameter layout (names, ranges, defaults), compiled once.
inline engine::Layout layoutOf(const std::function<void(dsl::Patch&)>& build, double sampleRate = 48000.0) {
  dsl::Patch p;
  build(p);
  return p.compile(sampleRate, 64).layout;
}

// Parameter id by name, or -1.
inline int paramId(const engine::Layout& layout, const std::string& name) {
  for (size_t i = 0; i < layout.params.size(); ++i)
    if (layout.params[i].name == name) return static_cast<int>(i);
  return -1;
}

// Renders the spec with parameters `ids` set to `values` (in parameter units) from the first sample.
// Returns the mono mix, or an empty vector if the patch does not compile.
inline std::vector<float> renderWith(const MatchSpec& spec, const std::vector<int>& ids, const std::vector<float>& values) {
  dsl::Patch patch;
  spec.build(patch);
  auto compiled = patch.compile(spec.sampleRate, spec.maxBlock);
  if (!compiled.graph) return {};
  for (size_t k = 0; k < ids.size() && k < values.size(); ++k) {
    auto& s = compiled.layout.params[static_cast<size_t>(ids[k])];
    s.def = std::clamp(values[k], s.min, s.max);  // the engine starts every parameter at its default: no ramp
  }
  std::vector<engine::Event> ev;
  for (const auto& e : spec.events)
    if (e.type != engine::Event::Type::Param || std::find(ids.begin(), ids.end(), e.key) == ids.end()) ev.push_back(e);
  std::stable_sort(ev.begin(), ev.end(), [](const engine::Event& a, const engine::Event& b) { return a.frame < b.frame; });
  engine::Engine eng;
  eng.prepare(spec.sampleRate, spec.maxBlock, compiled.layout);
  eng.setGraph(std::move(compiled.graph));
  return monoMix(render::renderOffline(eng, 2, spec.frames, ev));
}

struct MatchResult {
  engine::Layout layout;       // the patch's layout, with def = matched value for searched parameters
  std::vector<int> ids;        // searched parameters
  std::vector<float> values;   // matched values, parameter units, same order as ids
  double distance = 0.0;       // distance of the matched render
  double startDistance = 0.0;  // distance with the patch defaults
  CmaesResult search;
};

inline MatchResult matchSound(const MatchSpec& spec, const SpectralTarget& target, CmaesOptions options = {},
                              const std::function<void(int, double)>& progress = {}) {
  MatchResult r;
  r.layout = layoutOf(spec.build, spec.sampleRate);
  r.ids = spec.paramIds;
  if (r.ids.empty())
    for (size_t i = 0; i < r.layout.params.size(); ++i) r.ids.push_back(static_cast<int>(i));

  auto toValues = [&](const std::vector<double>& x) {
    std::vector<float> v(r.ids.size());
    for (size_t k = 0; k < r.ids.size(); ++k)
      v[k] = ui::fromNormalized(r.layout.params[static_cast<size_t>(r.ids[k])], static_cast<float>(x[k]));
    return v;
  };
  auto objective = [&](const std::vector<double>& x) {
    const auto audio = renderWith(spec, r.ids, toValues(x));
    return audio.empty() ? 1e9 : stftDistance(target, audio);
  };
  std::vector<double> x0(r.ids.size());
  for (size_t k = 0; k < r.ids.size(); ++k) {
    const auto& s = r.layout.params[static_cast<size_t>(r.ids[k])];
    x0[k] = ui::toNormalized(s, s.def);
  }
  r.search = minimizeCmaes(x0, objective, options, progress);
  r.startDistance = objective(x0);
  r.values = toValues(r.search.best);
  r.distance = r.search.bestValue;
  for (size_t k = 0; k < r.ids.size(); ++k) r.layout.params[static_cast<size_t>(r.ids[k])].def = r.values[k];
  return r;
}

}  // namespace tonic::analysis
