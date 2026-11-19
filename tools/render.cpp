// Renders a demo synth (examples/demo_synths.hpp) with its event script to a WAV file.
// Usage: tonic2_render <demo> out.wav      demos: mono, sinesum, fmdrone, filterednoise
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "demo_synths.hpp"
#include "tonic/render/offline.hpp"
#include "tonic/render/wav.hpp"

using namespace tonic;

int main(int argc, char** argv) {
  const auto demoList = demos::all();
  const demos::Demo* demo = nullptr;
  if (argc == 3)
    for (const auto& d : demoList) if (d.name == argv[1]) demo = &d;
  if (!demo) {
    std::fprintf(stderr, "usage: %s <demo> out.wav\ndemos:", argv[0]);
    for (const auto& d : demoList) std::fprintf(stderr, " %s", d.name.c_str());
    std::fprintf(stderr, "\n");
    return 2;
  }
  const double sr = 48000.0;
  const int maxBlock = 512;
  dsl::Patch patch;
  demo->build(patch);
  auto compiled = patch.compile(sr, maxBlock);
  if (!compiled.graph) { std::fprintf(stderr, "compile failed: %s\n", compiled.error.c_str()); return 1; }

  engine::Engine e;
  e.prepare(sr, maxBlock, compiled.layout);
  e.setGraph(std::move(compiled.graph));
  auto ev = demo->script(sr);
  std::stable_sort(ev.begin(), ev.end(), [](const engine::Event& a, const engine::Event& b) { return a.frame < b.frame; });

  const int frames = static_cast<int>(demo->seconds * sr);
  auto out = render::renderOffline(e, 2, frames, ev);
  if (!render::writeWavFloat(argv[2], out, static_cast<int>(sr))) { std::perror(argv[2]); return 1; }
  double peak = 0, sum = 0;
  for (const auto& c : out)
    for (float v : c) { peak = std::max(peak, double(std::fabs(v))); sum += double(v) * v; }
  const double rms = std::sqrt(sum / (2.0 * frames));
  std::printf("wrote %s (%s): %d frames, 2 ch, %.0f Hz, peak %.2f dBFS, rms %.2f dBFS\n", argv[2], demo->name.c_str(),
              frames, sr, 20 * std::log10(peak + 1e-12), 20 * std::log10(rms + 1e-12));
  return 0;
}
