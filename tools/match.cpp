// tonic2_match: find parameter values for a demo patch that make it sound like a target WAV.
//
//   tonic2_match --demo mono --target t.wav [--params cutoff,resonance] [--evals 400] [--seed 1]
//                [--threads N] [--out best.wav] [--json best.json]
//   tonic2_match --demo mono --truth cutoff=2400,resonance=0.7 [--params ...]   (synthetic target)
//
// The demo's own event script supplies the notes. The target must be a float WAV written by tonic2 tools
// (tonic2_render, --out); its sample rate is used for rendering and its length sets the compared span.
// Prints progress and the matched values; --json writes them as {"params": {"name": value, ...}}.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "demo_synths.hpp"
#include "tonic/analysis/match.hpp"
#include "tonic/render/wav.hpp"

using namespace tonic;

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, sep);)
    if (!item.empty()) out.push_back(item);
  return out;
}

int usage() {
  std::fprintf(stderr,
               "usage: tonic2_match --demo NAME (--target FILE.wav | --truth name=value,...) [--params a,b,...]\n"
               "                    [--evals N] [--seed S] [--threads N] [--seconds S] [--out best.wav] [--json best.json]\n"
               "demos:");
  for (const auto& d : demos::all()) std::fprintf(stderr, " %s", d.name.c_str());
  std::fprintf(stderr, "\n");
  return 2;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  std::string demoName, targetPath, truthArg, paramsArg, outPath, jsonPath;
  analysis::CmaesOptions opt;
  opt.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  double seconds = 0.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--demo") demoName = next();
    else if (a == "--target") targetPath = next();
    else if (a == "--truth") truthArg = next();
    else if (a == "--params") paramsArg = next();
    else if (a == "--evals") opt.maxEvals = std::atoi(next().c_str());
    else if (a == "--seed") opt.seed = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--threads") opt.threads = std::max(1, std::atoi(next().c_str()));
    else if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--out") outPath = next();
    else if (a == "--json") jsonPath = next();
    else return usage();
  }
  const auto demoList = demos::all();
  const demos::Demo* demo = nullptr;
  for (const auto& d : demoList) if (d.name == demoName) demo = &d;
  if (!demo || (targetPath.empty() == truthArg.empty())) return usage();

  analysis::MatchSpec spec;
  spec.build = demo->build;
  render::WavData target;
  if (!targetPath.empty()) {
    if (!render::readWavFloat(targetPath, target) || target.channels.empty()) {
      std::fprintf(stderr, "could not read %s (float WAV from a tonic2 tool expected)\n", targetPath.c_str());
      return 1;
    }
    spec.sampleRate = target.sampleRate;
  }
  spec.events = demo->script(spec.sampleRate);
  const double span = seconds > 0 ? seconds : demo->seconds;
  spec.frames = static_cast<int>(span * spec.sampleRate);

  const auto layout = analysis::layoutOf(spec.build, spec.sampleRate);
  auto lookup = [&](const std::string& name) {
    const int id = analysis::paramId(layout, name);
    if (id < 0) {
      std::fprintf(stderr, "demo '%s' has no parameter '%s'; it has:", demo->name.c_str(), name.c_str());
      for (const auto& p : layout.params) std::fprintf(stderr, " %s", p.name.c_str());
      std::fprintf(stderr, "\n");
      std::exit(2);
    }
    return id;
  };
  for (const auto& name : split(paramsArg, ',')) spec.paramIds.push_back(lookup(name));

  std::vector<float> targetMono;
  if (!truthArg.empty()) {  // synthetic target: the demo with the given values
    std::vector<int> ids;
    std::vector<float> vals;
    for (const auto& kv : split(truthArg, ',')) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) return usage();
      ids.push_back(lookup(kv.substr(0, eq)));
      vals.push_back(static_cast<float>(std::atof(kv.substr(eq + 1).c_str())));
    }
    targetMono = analysis::renderWith(spec, ids, vals);
  } else {
    targetMono = analysis::monoMix(target.channels);
    if (seconds <= 0) spec.frames = static_cast<int>(targetMono.size());
    targetMono.resize(static_cast<size_t>(spec.frames), 0.f);
  }

  std::printf("matching '%s' (%zu parameters, %d evals, %d threads, %.2f s at %.0f Hz)\n", demo->name.c_str(),
              spec.paramIds.empty() ? layout.params.size() : spec.paramIds.size(), opt.maxEvals, opt.threads,
              spec.frames / spec.sampleRate, spec.sampleRate);
  const auto t0 = std::chrono::steady_clock::now();
  int lastReport = 0;
  const auto r = analysis::matchSound(spec, analysis::analyse(targetMono), opt, [&](int evals, double best) {
    if (evals - lastReport >= 50 || evals + 20 > opt.maxEvals) {
      std::printf("  %4d evals  distance %.5f\n", evals, best);
      std::fflush(stdout);
      lastReport = evals;
    }
  });
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("distance %.5f (defaults %.5f), %d evals in %.1f s\n", r.distance, r.startDistance, r.search.evals, secs);
  for (size_t k = 0; k < r.ids.size(); ++k) {
    const auto& s = r.layout.params[static_cast<size_t>(r.ids[k])];
    std::printf("  %-14s %s\n", s.name.c_str(), ui::formatValue(s, r.values[k]).c_str());
  }

  if (!jsonPath.empty()) {
    FILE* fp = std::fopen(jsonPath.c_str(), "w");
    if (!fp) { std::perror(jsonPath.c_str()); return 1; }
    std::fprintf(fp, "{\n  \"demo\": \"%s\",\n  \"distance\": %.6f,\n  \"start_distance\": %.6f,\n  \"params\": {",
                 jsonEscape(demo->name).c_str(), r.distance, r.startDistance);
    for (size_t k = 0; k < r.ids.size(); ++k)
      std::fprintf(fp, "%s\n    \"%s\": %.6g", k ? "," : "", jsonEscape(r.layout.params[static_cast<size_t>(r.ids[k])].name).c_str(),
                   r.values[k]);
    std::fprintf(fp, "\n  }\n}\n");
    std::fclose(fp);
  }
  if (!outPath.empty()) {
    const auto best = analysis::renderWith(spec, r.ids, r.values);
    if (!render::writeWavFloat(outPath, {best, best}, static_cast<int>(spec.sampleRate))) { std::perror(outPath.c_str()); return 1; }
    std::printf("wrote %s\n", outPath.c_str());
  }
  return 0;
}
