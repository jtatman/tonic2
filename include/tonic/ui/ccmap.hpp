// MIDI CC map <-> text, for saving learned bindings in plugin state (and anything else that persists them).
// Format: comma-separated "cc=param" pairs, e.g. "1=4,71=4,74=0". Parsing skips malformed entries, so a
// damaged or older state never breaks loading.
#pragma once

#include <array>
#include <cstdlib>
#include <string>

namespace tonic::ui {

using CcMap = std::array<int, 128>;  // parameter id per CC, -1 = unbound

inline CcMap emptyCcMap() {
  CcMap m;
  m.fill(-1);
  return m;
}

inline std::string serializeCcMap(const CcMap& m) {
  std::string s;
  for (int cc = 0; cc < 128; ++cc) {
    if (m[static_cast<size_t>(cc)] < 0) continue;
    if (!s.empty()) s += ',';
    s += std::to_string(cc) + '=' + std::to_string(m[static_cast<size_t>(cc)]);
  }
  return s;
}

// Returns the parsed map; entries with a CC outside 0..127 or a parameter outside 0..numParams-1 are ignored.
inline CcMap parseCcMap(const std::string& s, int numParams) {
  CcMap m = emptyCcMap();
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    const std::string item = s.substr(pos, end - pos);
    const size_t eq = item.find('=');
    if (eq != std::string::npos && eq > 0 && eq + 1 < item.size()) {
      char* e1 = nullptr;
      char* e2 = nullptr;
      const long cc = std::strtol(item.c_str(), &e1, 10);
      const long p = std::strtol(item.c_str() + eq + 1, &e2, 10);
      const bool clean = e1 == item.c_str() + eq && e2 == item.c_str() + item.size();
      if (clean && cc >= 0 && cc < 128 && p >= 0 && p < numParams) m[static_cast<size_t>(cc)] = static_cast<int>(p);
    }
    pos = end + 1;
  }
  return m;
}

}  // namespace tonic::ui
