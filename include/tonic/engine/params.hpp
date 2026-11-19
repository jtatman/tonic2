// Parameter and macro layout. Parameters belong to the engine (not to a graph), so their values
// survive graph swaps. Ids are indices into Layout::params.
#pragma once

#include <string>
#include <vector>

namespace tonic::engine {

struct ParamSpec {
  std::string name;
  float min = 0.f, max = 1.f, def = 0.f;
  float smoothSeconds = 0.02f;  // 0 = jump immediately (still sample-accurate)
};

// One macro target: adds an offset in the target's plain units,
// offset = lo + (hi - lo) * macro^curve, then the result is clamped to the target's range.
struct MacroTarget {
  int param = 0;
  float lo = 0.f, hi = 0.f;
  float curve = 1.f;
};

// A macro is itself a parameter (normally 0..1) that drives several targets. This is what a small
// MIDI controller's knobs map to; detailed parameters stay reachable but out of the way.
struct MacroSpec {
  int param = 0;
  std::vector<MacroTarget> targets;
};

// A MIDI CC bound to a parameter: CC 0..127 maps linearly onto the parameter's min..max (bind a macro to
// drive several parameters from one knob).
struct CcBinding {
  int cc = 0;
  int param = 0;
};

struct Layout {
  std::vector<ParamSpec> params;
  std::vector<MacroSpec> macros;
  std::vector<CcBinding> ccBindings;  // initial bindings; MIDI learn and mapCc() can change them at runtime
  int add(ParamSpec p) { params.push_back(std::move(p)); return static_cast<int>(params.size()) - 1; }
};

}  // namespace tonic::engine
