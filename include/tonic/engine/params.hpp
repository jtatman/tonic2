// Parameter and macro layout. Parameters belong to the engine (not to a graph), so their values
// survive graph swaps. Ids are indices into Layout::params.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tonic::engine {

// Where a parameter appears in a plugin UI (see tonic/ui/surface.hpp).
enum class UiPlace : uint8_t {
  Auto,    // macros and CC-bound parameters go on the main surface first, the rest to the detail page
  Main,    // always on the main surface (up to 8 controls)
  Detail,  // detail page only
  Hidden,  // not shown (still automatable)
};

struct ParamSpec {
  std::string name;
  float min = 0.f, max = 1.f, def = 0.f;
  float smoothSeconds = 0.02f;  // 0 = jump immediately (still sample-accurate)
  std::string unit;             // display only, e.g. "Hz", "dB", "%" (empty = plain number)
  bool log = false;             // UI controls sweep logarithmically (needs min > 0)
  UiPlace ui = UiPlace::Auto;
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
  int add(std::string name, float min, float max, float def, float smoothSeconds = 0.02f) {
    ParamSpec p;
    p.name = std::move(name);
    p.min = min;
    p.max = max;
    p.def = def;
    p.smoothSeconds = smoothSeconds;
    return add(std::move(p));
  }
};

}  // namespace tonic::engine
