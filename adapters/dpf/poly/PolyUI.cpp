// Tonic Poly UI: the generic macro surface over the poly demo's parameters.
#include "../TonicDpfUI.hpp"
#include "demo_synths.hpp"

START_NAMESPACE_DISTRHO

UI* createUI() { return new TonicDpfUI(demos::polySynth, "TONIC POLY"); }

END_NAMESPACE_DISTRHO
