// Offline rendering: drive an Engine with a time-stamped event list (frames from the start of the
// render) and collect the output, in host-sized blocks exactly as a realtime host would.
#pragma once

#include <algorithm>
#include <vector>

#include "../engine/engine.hpp"

namespace tonic::render {

// Events must be sorted by frame. Returns channels[c][frame].
inline std::vector<std::vector<float>> renderOffline(engine::Engine& e, int channels, int frames,
                                                     const std::vector<engine::Event>& events, int hostBlock = 256) {
  std::vector<std::vector<float>> out(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(frames)));
  std::vector<float*> ptrs(static_cast<size_t>(channels));
  std::vector<engine::Event> blockEvents;
  size_t next = 0;
  for (int off = 0; off < frames; off += hostBlock) {
    const int n = std::min(hostBlock, frames - off);
    blockEvents.clear();
    while (next < events.size() && events[next].frame < off + n) {
      engine::Event ev = events[next++];
      ev.frame = std::max(0, ev.frame - off);
      blockEvents.push_back(ev);
    }
    for (int c = 0; c < channels; ++c) ptrs[static_cast<size_t>(c)] = out[static_cast<size_t>(c)].data() + off;
    e.process(nullptr, 0, ptrs.data(), channels, n, blockEvents.data(), static_cast<int>(blockEvents.size()));
  }
  return out;
}

}  // namespace tonic::render
