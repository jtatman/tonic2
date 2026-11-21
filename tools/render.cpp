// Renders a demo synth (examples/demo_synths.hpp) with its event script to a WAV file.
//
//   tonic2_render <demo> out.wav [--set "name=value,..."] [--seconds S] [--features]
//   tonic2_render --layout <demo>
//
// --set     starts the named parameters at the given values (the script's own changes to them are dropped)
// --features prints one JSON object with audio descriptors instead of the summary line
// --layout  prints the demo's description and parameters (ranges, units, macros) as JSON
// These three are the interface the text-to-patch agent (tools/patch_agent) drives.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "demo_synths.hpp"
#include "tonic/analysis/features.hpp"
#include "tonic/analysis/match.hpp"
#include "tonic/render/wav.hpp"

using namespace tonic;

namespace {

std::string jsonString(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

int usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <demo> out.wav [--set \"name=value,...\"] [--seconds S] [--features]\n       %s --layout <demo>\ndemos:",
               argv0, argv0);
  for (const auto& d : demos::all()) std::fprintf(stderr, " %s", d.name.c_str());
  std::fprintf(stderr, "\n");
  return 2;
}

void printLayout(const demos::Demo& demo, const engine::Layout& layout) {
  std::printf("{\n  \"demo\": %s,\n  \"description\": %s,\n  \"seconds\": %.3f,\n  \"params\": [", jsonString(demo.name).c_str(),
              jsonString(demo.description).c_str(), demo.seconds);
  for (size_t i = 0; i < layout.params.size(); ++i) {
    const auto& p = layout.params[i];
    std::string targets;
    for (const auto& m : layout.macros)
      if (m.param == static_cast<int>(i))
        for (const auto& t : m.targets) {
          if (!targets.empty()) targets += ", ";
          targets += jsonString(layout.params[static_cast<size_t>(t.param)].name);
        }
    std::printf("%s\n    {\"name\": %s, \"min\": %g, \"max\": %g, \"default\": %g, \"unit\": %s, \"log\": %s, \"macro_targets\": [%s]}",
                i ? "," : "", jsonString(p.name).c_str(), p.min, p.max, p.def, jsonString(p.unit).c_str(), p.log ? "true" : "false",
                targets.c_str());
  }
  std::printf("\n  ]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string demoName, outPath, setArg;
  bool layoutOnly = false, features = false;
  double seconds = 0.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--layout") { layoutOnly = true; demoName = next(); }
    else if (a == "--set") setArg = next();
    else if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--features") features = true;
    else if (!a.empty() && a[0] == '-') return usage(argv[0]);
    else if (demoName.empty()) demoName = a;
    else if (outPath.empty()) outPath = a;
    else return usage(argv[0]);
  }
  const auto demoList = demos::all();
  const demos::Demo* demo = nullptr;
  for (const auto& d : demoList) if (d.name == demoName) demo = &d;
  if (!demo || (!layoutOnly && outPath.empty())) return usage(argv[0]);

  const double sr = 48000.0;
  const auto layout = analysis::layoutOf(demo->build, sr);
  if (layoutOnly) { printLayout(*demo, layout); return 0; }

  analysis::MatchSpec spec;
  spec.build = demo->build;
  spec.sampleRate = sr;
  spec.events = demo->script(sr);
  spec.frames = static_cast<int>((seconds > 0 ? seconds : demo->seconds) * sr);
  std::vector<int> ids;
  std::vector<float> values;
  std::stringstream ss(setArg);
  for (std::string kv; std::getline(ss, kv, ',');) {
    if (kv.empty()) continue;
    const auto eq = kv.find('=');
    const int id = eq == std::string::npos ? -1 : analysis::paramId(layout, kv.substr(0, eq));
    if (id < 0) {
      std::fprintf(stderr, "bad --set entry '%s'; parameters:", kv.c_str());
      for (const auto& p : layout.params) std::fprintf(stderr, " '%s'", p.name.c_str());
      std::fprintf(stderr, "\n");
      return 2;
    }
    ids.push_back(id);
    values.push_back(static_cast<float>(std::atof(kv.substr(eq + 1).c_str())));
  }

  const auto out = analysis::renderStereoWith(spec, ids, values);
  if (out.empty()) { std::fprintf(stderr, "compile failed\n"); return 1; }
  if (!render::writeWavFloat(outPath, out, static_cast<int>(sr))) { std::perror(outPath.c_str()); return 1; }
  if (features) {
    std::printf("%s\n", analysis::toJson(analysis::computeFeatures(analysis::monoMix(out), sr)).c_str());
    return 0;
  }
  double peak = 0, sum = 0;
  for (const auto& c : out)
    for (float v : c) { peak = std::max(peak, double(std::fabs(v))); sum += double(v) * v; }
  const double rms = std::sqrt(sum / (2.0 * spec.frames));
  std::printf("wrote %s (%s): %d frames, 2 ch, %.0f Hz, peak %.2f dBFS, rms %.2f dBFS\n", outPath.c_str(), demo->name.c_str(),
              spec.frames, sr, 20 * std::log10(peak + 1e-12), 20 * std::log10(rms + 1e-12));
  return 0;
}
