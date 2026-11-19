// Graph builder (control thread) and compiled graph (audio thread).
//
// Build a graph by adding nodes and connections, then compile() it. Compilation (off the audio thread):
//  - validates ids, ports and channel counts (equal, or a mono source broadcast to N channels)
//  - drops nodes that don't reach the output, topologically sorts the rest, rejects cycles
//  - assigns buffers, reusing them once their last reader has run; several connections into one
//    input port are summed into a dedicated mix buffer
// The CompiledGraph then runs with no allocation, locks or exceptions.
#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "node.hpp"

namespace tonic::graph {

using NodeId = int;

// Source node for an external input bus (audio from the host / device).
class InputNode final : public Node {
 public:
  explicit InputNode(int channels) : Node({}, {channels}) {}
  void bind(const float* const* ext, int numExt, int firstChannel, int offset) {
    ext_ = ext; numExt_ = numExt; first_ = firstChannel; offset_ = offset;
  }
  void process(const ProcessArgs& a) override {
    const int ch = outputChannels()[0];
    for (int c = 0; c < ch; ++c) {
      float* out = a.output(0, c);
      const int e = first_ + c;
      if (ext_ && e < numExt_ && ext_[e]) std::memcpy(out, ext_[e] + offset_, sizeof(float) * a.numFrames);
      else std::memset(out, 0, sizeof(float) * a.numFrames);
    }
  }

 private:
  const float* const* ext_ = nullptr;
  int numExt_ = 0, first_ = 0, offset_ = 0;
};

class CompiledGraph {
 public:
  double sampleRate() const { return sampleRate_; }
  int maxBlock() const { return maxBlock_; }
  int numBuffers() const { return numBuffers_; }
  int numSteps() const { return static_cast<int>(steps_.size()); }
  int numOutputChannels() const { return static_cast<int>(outPtrs_.size()); }
  int numInputChannels() const { return numInputChannels_; }

  // Audio thread. numFrames <= maxBlock(). External channel pointers are read at `offset`;
  // output channels are written at `outOffset`. A mono graph output feeds every output channel.
  void process(int numFrames, const float* const* in, int numIn, int inOffset, float* const* out, int numOut,
               int outOffset) {
    for (size_t i = 0; i < inputs_.size(); ++i) inputs_[i]->bind(in, numIn, inputFirst_[i], inOffset);
    for (Step& s : steps_) {
      for (Mix& m : s.mixes) {
        std::memcpy(m.dstPtr, m.srcPtrs[0], sizeof(float) * numFrames);
        for (size_t k = 1; k < m.srcPtrs.size(); ++k)
          for (int f = 0; f < numFrames; ++f) m.dstPtr[f] += m.srcPtrs[k][f];
      }
      ProcessArgs a;
      a.numFrames = numFrames;
      a.inputs = s.inPorts.data();
      a.outputs = s.outPorts.data();
      s.node->process(a);
    }
    const int gch = numOutputChannels();
    for (int c = 0; c < numOut; ++c) {
      if (!out[c]) continue;
      if (gch == 0 || (c >= gch && gch != 1)) { std::memset(out[c] + outOffset, 0, sizeof(float) * numFrames); continue; }
      std::memcpy(out[c] + outOffset, outPtrs_[gch == 1 ? 0 : c], sizeof(float) * numFrames);
    }
  }

 private:
  friend class GraphBuilder;
  struct Mix {
    int dst = 0;
    std::vector<int> srcs;
    float* dstPtr = nullptr;
    std::vector<const float*> srcPtrs;
  };
  struct Step {
    Node* node = nullptr;
    std::vector<std::vector<int>> inIdx, outIdx;  // buffer index per port/channel, -1 = silence
    std::vector<Mix> mixes;
    std::vector<std::vector<const float*>> inPtrs;
    std::vector<std::vector<float*>> outPtrs;
    std::vector<const float* const*> inPorts;
    std::vector<float* const*> outPorts;
  };

  double sampleRate_ = 0;
  int maxBlock_ = 0, numBuffers_ = 0, numInputChannels_ = 0;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<float> pool_;   // numBuffers_ * maxBlock_ samples, then one silent buffer
  std::vector<Step> steps_;
  std::vector<InputNode*> inputs_;
  std::vector<int> inputFirst_;
  std::vector<int> outIdx_;
  std::vector<const float*> outPtrs_;
};

struct CompileResult {
  std::unique_ptr<CompiledGraph> graph;  // null on error
  std::string error;
};

class GraphBuilder {
 public:
  template <class T, class... Args>
  NodeId add(Args&&... args) {
    return add(std::make_unique<T>(std::forward<Args>(args)...));
  }
  NodeId add(std::unique_ptr<Node> node) {
    nodes_.push_back(std::move(node));
    return static_cast<NodeId>(nodes_.size() - 1);
  }
  // External input bus; buses take consecutive external channels in the order they are added.
  NodeId addInput(int channels) {
    const NodeId id = add(std::make_unique<InputNode>(channels));
    inputIds_.push_back(id);
    return id;
  }
  void connect(NodeId src, int srcPort, NodeId dst, int dstPort) { edges_.push_back({src, srcPort, dst, dstPort}); }
  void setOutput(NodeId src, int srcPort = 0) { outNode_ = src; outPort_ = srcPort; }

  // Consumes the builder's nodes.
  CompileResult compile(double sampleRate, int maxBlock) {
    CompileResult r;
    if (std::string e = validate(maxBlock); !e.empty()) { r.error = e; return r; }
    const int n = static_cast<int>(nodes_.size());

    // Keep only nodes that reach the output.
    std::vector<char> live(n, 0);
    std::vector<int> stack{outNode_};
    live[outNode_] = 1;
    while (!stack.empty()) {
      const int v = stack.back(); stack.pop_back();
      for (const Edge& e : edges_)
        if (e.dst == v && !live[e.src]) { live[e.src] = 1; stack.push_back(e.src); }
    }
    // Kahn topological sort over live nodes.
    std::vector<int> indeg(n, 0), order;
    for (const Edge& e : edges_) if (live[e.src] && live[e.dst]) ++indeg[e.dst];
    for (int v = 0; v < n; ++v) if (live[v] && indeg[v] == 0) order.push_back(v);
    for (size_t i = 0; i < order.size(); ++i)
      for (const Edge& e : edges_)
        if (e.src == order[i] && live[e.dst] && --indeg[e.dst] == 0) order.push_back(e.dst);
    const int liveCount = static_cast<int>(std::count(live.begin(), live.end(), 1));
    if (static_cast<int>(order.size()) != liveCount) { r.error = "graph contains a cycle"; return r; }

    auto g = std::make_unique<CompiledGraph>();
    g->sampleRate_ = sampleRate;
    g->maxBlock_ = maxBlock;

    // Remaining readers per output port; the graph output is never released.
    std::vector<std::vector<int>> readers(n), bufOf(n);
    std::vector<std::vector<std::vector<int>>> portBufs(n);
    for (int v = 0; v < n; ++v) {
      readers[v].assign(nodes_[v]->outputChannels().size(), 0);
      portBufs[v].resize(nodes_[v]->outputChannels().size());
    }
    for (const Edge& e : edges_) if (live[e.dst]) ++readers[e.src][e.srcPort];
    readers[outNode_][outPort_] += 1 << 20;

    std::vector<int> freeList;
    int numBuffers = 0;
    auto alloc = [&]() { if (freeList.empty()) return numBuffers++; int b = freeList.back(); freeList.pop_back(); return b; };

    for (int v : order) {
      Node& node = *nodes_[v];
      CompiledGraph::Step s;
      s.node = &node;
      const auto& inCh = node.inputChannels();
      std::vector<int> mixBufs;
      for (int p = 0; p < static_cast<int>(inCh.size()); ++p) {
        std::vector<const Edge*> srcs;
        for (const Edge& e : edges_) if (e.dst == v && e.dstPort == p) srcs.push_back(&e);
        std::vector<int> idx(inCh[p], -1);
        auto srcBuf = [&](const Edge* e, int c) {
          const auto& b = portBufs[e->src][e->srcPort];
          return b.size() == 1 ? b[0] : b[c];
        };
        if (srcs.size() == 1) {
          for (int c = 0; c < inCh[p]; ++c) idx[c] = srcBuf(srcs[0], c);
        } else if (srcs.size() > 1) {
          for (int c = 0; c < inCh[p]; ++c) {
            CompiledGraph::Mix m;
            m.dst = alloc();
            mixBufs.push_back(m.dst);
            for (const Edge* e : srcs) m.srcs.push_back(srcBuf(e, c));
            idx[c] = m.dst;
            s.mixes.push_back(std::move(m));
          }
        }
        s.inIdx.push_back(std::move(idx));
      }
      // Outputs are allocated before any input is released, so they never alias an input.
      for (int p = 0; p < static_cast<int>(node.outputChannels().size()); ++p) {
        std::vector<int> idx(node.outputChannels()[p]);
        for (int& b : idx) b = alloc();
        portBufs[v][p] = idx;
        s.outIdx.push_back(std::move(idx));
      }
      // Release inputs whose last reader was this node, mix buffers, and unread outputs.
      for (const Edge& e : edges_)
        if (e.dst == v && --readers[e.src][e.srcPort] == 0)
          for (int b : portBufs[e.src][e.srcPort]) freeList.push_back(b);
      for (int b : mixBufs) freeList.push_back(b);
      for (int p = 0; p < static_cast<int>(node.outputChannels().size()); ++p)
        if (readers[v][p] == 0) for (int b : portBufs[v][p]) freeList.push_back(b);
      g->steps_.push_back(std::move(s));
    }

    // Resolve buffer indices to pointers now that the pool size is known.
    g->numBuffers_ = numBuffers;
    g->pool_.assign(static_cast<size_t>(numBuffers + 1) * maxBlock, 0.0f);
    float* base = g->pool_.data();
    const float* silence = base + static_cast<size_t>(numBuffers) * maxBlock;
    auto ptr = [&](int b) { return b < 0 ? const_cast<float*>(silence) : base + static_cast<size_t>(b) * maxBlock; };
    for (auto& s : g->steps_) {
      for (auto& m : s.mixes) {
        m.dstPtr = ptr(m.dst);
        for (int b : m.srcs) m.srcPtrs.push_back(ptr(b));
      }
      for (auto& port : s.inIdx) {
        s.inPtrs.emplace_back();
        for (int b : port) s.inPtrs.back().push_back(ptr(b));
      }
      for (auto& port : s.outIdx) {
        s.outPtrs.emplace_back();
        for (int b : port) s.outPtrs.back().push_back(ptr(b));
      }
      for (auto& p : s.inPtrs) s.inPorts.push_back(p.data());
      for (auto& p : s.outPtrs) s.outPorts.push_back(p.data());
    }
    for (int b : portBufs[outNode_][outPort_]) g->outPtrs_.push_back(ptr(b));

    int firstChannel = 0;
    for (NodeId id : inputIds_) {
      auto* in = static_cast<InputNode*>(nodes_[id].get());
      g->inputs_.push_back(in);
      g->inputFirst_.push_back(firstChannel);
      firstChannel += in->outputChannels()[0];
    }
    g->numInputChannels_ = firstChannel;
    for (auto& node : nodes_) node->prepare(sampleRate, maxBlock);
    g->nodes_ = std::move(nodes_);
    r.graph = std::move(g);
    return r;
  }

 private:
  struct Edge { NodeId src; int srcPort; NodeId dst; int dstPort; };

  std::string validate(int maxBlock) const {
    const int n = static_cast<int>(nodes_.size());
    if (maxBlock <= 0) return "maxBlock must be positive";
    if (outNode_ < 0 || outNode_ >= n) return "no output set";
    if (outPort_ < 0 || outPort_ >= static_cast<int>(nodes_[outNode_]->outputChannels().size())) return "bad output port";
    for (const Edge& e : edges_) {
      if (e.src < 0 || e.src >= n || e.dst < 0 || e.dst >= n) return "connection references unknown node";
      const auto& so = nodes_[e.src]->outputChannels();
      const auto& di = nodes_[e.dst]->inputChannels();
      if (e.srcPort < 0 || e.srcPort >= static_cast<int>(so.size())) return "bad source port";
      if (e.dstPort < 0 || e.dstPort >= static_cast<int>(di.size())) return "bad destination port";
      if (so[e.srcPort] != di[e.dstPort] && so[e.srcPort] != 1)
        return "channel mismatch (" + std::to_string(so[e.srcPort]) + " -> " + std::to_string(di[e.dstPort]) + ")";
    }
    return {};
  }

  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Edge> edges_;
  std::vector<NodeId> inputIds_;
  NodeId outNode_ = -1;
  int outPort_ = 0;
};

}  // namespace tonic::graph
