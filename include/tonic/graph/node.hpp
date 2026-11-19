// Graph node interface. Nodes wrap DSP kernels and declare their audio ports.
// Virtual dispatch happens once per node per block, never per sample.
#pragma once

#include <utility>
#include <vector>

#include "../engine/events.hpp"

namespace tonic::graph {

// Per-sample control signals computed by the engine for the current block (sample-accurate).
// Any pointer may be null when the engine has nothing to supply (e.g. a graph rendered directly).
struct ControlBuffers {
  const float* const* params = nullptr;  // params[id][frame], smoothed, macros applied
  int numParams = 0;
  const float* gate = nullptr;     // 1 while a note is held (engine: mono last-note priority; voices: per voice)
  const float* trigger = nullptr;  // 1 on the frame a note starts
  const float* pitchHz = nullptr;  // frequency of the current note
  const float* velocity = nullptr; // velocity of the current note
  const float* pressure = nullptr; // per-note pressure (set inside voices)
  const float* timbre = nullptr;   // per-note timbre (set inside voices)
  // The process() call's events that fall in this block. Frames are relative to the process() call;
  // subtract eventOffset for a frame inside this block. Used by voice nodes.
  const engine::Event* events = nullptr;
  int numEvents = 0;
  int eventOffset = 0;
};

// Buffers for one process call. input(port, ch) / output(port, ch) point at numFrames samples.
// Inputs are read-only and may alias each other (fan-out, mono broadcast); outputs never alias inputs.
struct ProcessArgs {
  int numFrames = 0;
  const float* const* const* inputs = nullptr;
  float* const* const* outputs = nullptr;
  const ControlBuffers* control = nullptr;
  const float* input(int port, int ch) const { return inputs[port][ch]; }
  float* output(int port, int ch) const { return outputs[port][ch]; }
  // Per-sample values of a parameter, or null if unavailable.
  const float* param(int id) const {
    return control && id >= 0 && id < control->numParams ? control->params[id] : nullptr;
  }
};

class Node {
 public:
  // Channel count of each input port and each output port.
  Node(std::vector<int> inputChannels, std::vector<int> outputChannels)
      : inputChannels_(std::move(inputChannels)), outputChannels_(std::move(outputChannels)) {}
  virtual ~Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  const std::vector<int>& inputChannels() const { return inputChannels_; }
  const std::vector<int>& outputChannels() const { return outputChannels_; }

  // Called off the audio thread when the graph is compiled. May allocate.
  virtual void prepare(double /*sampleRate*/, int /*maxBlock*/) {}
  // Audio thread. Must not allocate, lock, or throw. Must write every output sample.
  virtual void process(const ProcessArgs& args) = 0;

 private:
  std::vector<int> inputChannels_, outputChannels_;
};

}  // namespace tonic::graph
