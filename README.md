# tonic2

A realtime-safe C++20 synthesis library: the successor core to [Tonic](https://github.com/jtatman/Tonic).
Released into the public domain under the [Unlicense](LICENSE).

**Status: M1 (DSP kernels).** The graph, engine, voices, mixer and patching syntax come in later
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
