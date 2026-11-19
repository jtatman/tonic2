// Tonic Probe: a test plugin whose output (both channels) equals its "level" parameter (unsmoothed).
// MIDI CC 1 (mod wheel) is bound to "level". Used by the CLAP host test to check exactly when
// automation and CC changes land.
#include "../TonicDpfPlugin.hpp"

START_NAMESPACE_DISTRHO

static void probePatch(tonic::dsl::Patch& p) {
  auto level = p.param("level", 0.f, 1.f, 0.f, 0.f);
  p.midiCc(1, level);
  p.output(level + 0.f);
}

Plugin* createPlugin() {
  return new TonicDpfPlugin(probePatch, {"TonicProbe", "Test plugin: output equals the level parameter.", "tonic2",
                                         "https://github.com/jtatman/tonic2", "Unlicense", d_version(0, 1, 0),
                                         d_cconst('T', 'n', 'P', 'r')});
}

END_NAMESPACE_DISTRHO
