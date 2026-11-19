// Graph node interface. Nodes wrap DSP kernels and declare their audio ports.
// Virtual dispatch happens once per node per block, never per sample.
#pragma once

#include <utility>
#include <vector>

namespace tonic::graph {

// Buffers for one process call. input(port, ch) / output(port, ch) point at numFrames samples.
// Inputs are read-only and may alias each other (fan-out, mono broadcast); outputs never alias inputs.
struct ProcessArgs {
  int numFrames = 0;
  const float* const* const* inputs = nullptr;
  float* const* const* outputs = nullptr;
  const float* input(int port, int ch) const { return inputs[port][ch]; }
  float* output(int port, int ch) const { return outputs[port][ch]; }
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
