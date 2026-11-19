# tonic2

A realtime-safe C++20 synthesis library: the successor core to [Tonic](https://github.com/jtatman/Tonic).
Released into the public domain under the [Unlicense](LICENSE).

**Status: M2 (graph + engine).** Parameters, events, voices, mixer and patching syntax come in later
milestones; see the [design document](https://github.com/jtatman/Tonic/blob/phase1-design/docs/phase1-core-design.md).

## DSP kernels (`include/tonic/dsp/`, header-only)

| Header | Contents |
|---|---|
| `oscillator.hpp` | Band-limited sine, saw, square/PWM, triangle (PolyBLEP / PolyBLAMP) |
| `filters.hpp` | ZDF state-variable filter (LP/BP/HP/notch), ZDF 4-pole ladder with saturation |
| `envelope.hpp` | Analog-style exponential ADSR with exact segment times |
| `smoother.hpp` | One-pole and linear parameter smoothers |
| `noise.hpp` | Deterministic xorshift white noise |
| `math.hpp` | Helpers (Pade tanh, dB, MIDI to Hz, TPT gain) |

Kernel rules: plain structs, no virtual functions, no allocation after `prepare()`, no global state,
no exceptions. They are meant to be ported line-for-line to Rust and Go.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests check measured behavior (oscillator frequency, aliasing, filter response, envelope timing,
smoother response, block-size independence, stability) and compare against golden renders in
`tests/golden/`. To accept an intentional change: `build/tonic2_golden --update tests/golden`.

## Graph and engine (`include/tonic/graph/`, `include/tonic/engine/`)

```cpp
using namespace tonic;
graph::GraphBuilder b;
auto osc = b.add<graph::OscillatorNode>(dsp::Waveform::Saw, 110.0);
auto lad = b.add<graph::LadderNode>(1, 1200.0, 0.5, 1.5);
auto pan = b.add<graph::PanNode>(0.3f);
b.connect(osc, 0, lad, 0);
b.connect(lad, 0, pan, 0);
b.setOutput(pan);
auto result = b.compile(48000.0, 512);     // control thread; result.error explains failures

engine::Engine e;
e.prepare(48000.0, 512);
e.setGraph(std::move(result.graph));       // control thread, any time (lock-free swap)
e.process(inputs, numIn, outputs, numOut, numFrames);   // audio thread, any block size
e.collectGarbage();                         // control thread, frees replaced graphs
```

- Compilation validates ports and channel counts, removes nodes that don't reach the output,
  rejects cycles, sums multiple connections into one input, broadcasts mono to N channels, and
  reuses buffers (a 50-node chain uses 2 buffers).
- `process()` never allocates, frees, locks or throws. Tests check this with an allocation counter
  while graphs are hot-swapped from another thread; CI also runs ThreadSanitizer and ASan/UBSan.
