// Tonic Poly: the poly demo synth (8 voices, 2-voice unison, filter envelope, shared delay) as a plugin.
#include "../TonicDpfPlugin.hpp"
#include "demo_synths.hpp"

START_NAMESPACE_DISTRHO

Plugin* createPlugin() {
  return new TonicDpfPlugin(demos::polySynth,
                            {"TonicPoly", "Polyphonic synth built with tonic2 (example plugin).", "tonic2",
                             "https://github.com/jtatman/tonic2", "Unlicense", d_version(0, 1, 0),
                             d_cconst('T', 'n', 'P', 'l')});
}

END_NAMESPACE_DISTRHO
