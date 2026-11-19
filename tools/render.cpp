// Renders a short demo phrase through a small mono synth to a WAV file.
// Usage: tonic2_render out.wav
// The patch shows the M3 pieces: note pitch and gate, an envelope, a smoothed cutoff parameter
// automated by events, and a "brightness" macro that moves cutoff and resonance together.
#include <cmath>
#include <cstdio>

#include "tonic/graph/nodes.hpp"
#include "tonic/render/offline.hpp"
#include "tonic/render/wav.hpp"

using namespace tonic;

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: %s out.wav\n", argv[0]); return 2; }
  const double sr = 48000.0;
  const int maxBlock = 512;

  engine::Layout layout;
  const int cutoff = layout.add({"cutoff", 80.f, 12000.f, 600.f, 0.03f});
  const int resonance = layout.add({"resonance", 0.f, 1.f, 0.3f, 0.03f});
  const int bright = layout.add({"brightness", 0.f, 1.f, 0.f, 0.05f});
  layout.macros.push_back({bright, {{cutoff, 0.f, 4000.f, 2.f}, {resonance, 0.f, 0.4f, 1.f}}});

  engine::Engine e;
  e.prepare(sr, maxBlock, layout);

  graph::GraphBuilder b;
  auto o1 = b.add<graph::OscillatorNode>(dsp::Waveform::Saw, graph::FreqSource::note(1.0));
  auto o2 = b.add<graph::OscillatorNode>(dsp::Waveform::Square, graph::FreqSource::note(0.5));
  auto lad = b.add<graph::LadderNode>(1, 600.0, 0.3, 1.5, cutoff, resonance);
  auto env = b.add<graph::EnvelopeNode>(0.005, 0.25, 0.4, 0.3);
  auto vca = b.add<graph::VcaNode>(1);
  auto gain = b.add<graph::GainNode>(1, 0.4f);
  auto pan = b.add<graph::PanNode>(0.f);
  b.connect(o1, 0, lad, 0); b.connect(o2, 0, lad, 0);
  b.connect(lad, 0, vca, 0); b.connect(env, 0, vca, 1);
  b.connect(vca, 0, gain, 0); b.connect(gain, 0, pan, 0);
  b.setOutput(pan);
  auto compiled = b.compile(sr, maxBlock);
  if (!compiled.graph) { std::fprintf(stderr, "compile failed: %s\n", compiled.error.c_str()); return 1; }
  e.setGraph(std::move(compiled.graph));

  std::vector<engine::Event> ev;
  const int notes[] = {45, 48, 52, 57, 55, 52, 48, 50};
  const int step = static_cast<int>(sr * 0.25);
  for (int i = 0; i < 8; ++i) {
    ev.push_back(engine::Event::noteOn(i * step, notes[i], 0.9f));
    ev.push_back(engine::Event::noteOff(i * step + step * 3 / 4, notes[i]));
  }
  // Automation: macro sweeps up over the phrase.
  for (int k = 0; k <= 16; ++k) ev.push_back(engine::Event::param(k * step / 2, bright, k / 16.f));
  std::sort(ev.begin(), ev.end(), [](const engine::Event& a, const engine::Event& b) { return a.frame < b.frame; });

  const int frames = 9 * step;
  auto out = render::renderOffline(e, 2, frames, ev);
  if (!render::writeWavFloat(argv[1], out, static_cast<int>(sr))) { std::perror(argv[1]); return 1; }
  double peak = 0, sum = 0;
  for (const auto& c : out)
    for (float v : c) { peak = std::max(peak, double(std::fabs(v))); sum += double(v) * v; }
  const double rms = std::sqrt(sum / (2.0 * frames));
  std::printf("wrote %s: %d frames, 2 channels, %.0f Hz, peak %.2f dBFS, rms %.2f dBFS\n", argv[1], frames, sr,
              20 * std::log10(peak + 1e-12), 20 * std::log10(rms + 1e-12));
  return 0;
}
