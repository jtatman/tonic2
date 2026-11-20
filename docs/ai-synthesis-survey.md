# AI in sound synthesis: survey and tonic2 integration points

Status: survey, November 2026. Goal (b1) of the modernization plan: find out where machine learning is
actually used in synthesis today, and decide which parts are worth building into tonic2.

The short answer: the useful work splits into two kinds.

- **Offline work** (finding presets, matching sounds, describing patches in words). This needs a
  synth with a small, explicit parameter space and a fast offline renderer. tonic2 already has both.
- **Real-time work** (neural oscillators and effects inside the audio graph). This needs an inference
  library that keeps to the real-time rules tonic2 already enforces: no allocation, no locks, no
  exceptions on the audio thread.

The AI never needs to sit in tonic2's core. It sits beside it, as a tool that writes `Layout` values or
DSL patches, or as one more graph node.

## 1. Audio-to-preset (sound matching)

The question here: given a recording, find synth parameters that reproduce it.

**Products**

- *Synplant 2* (Sonic Charge): its Genopatch feature grows patches toward a dropped-in sample, then
  lets you keep breeding variations.
- *MicroMusic*: generates Vital presets from audio samples.
- *Audial*: turns a one-shot into an editable Serum 2 preset file.
- *Chatmu Sound*: an AI synth plugin that recreates a given sound.
- *Jamahook*: retrieval rather than synthesis. It searches a sound library for sounds that suit your track.

**Research and open source**

- *InverSynth* (2018): CNN on spectrograms predicts the parameters of an FM subtractive synth.
- *Dexed sound-matching study* (Yee-King et al., IEEE TETCI 2018) compares hill climbing, a genetic algorithm and
  three neural nets on a DX7 model. A bidirectional LSTM did best, matching closely in about 25% of cases.
- *preset-gen-vae* (Le Vaillant et al.): spectral VAE over a 30k-preset Dexed dataset. It does inference,
  generation and morphing between presets.
- *Syntheon*: parameter inference for Vital and Dexed.
- *torchsynth*: a GPU modular synth in PyTorch, about 16,000× real time, built to make training data.
- *Masuda & Saito*, "Synthesizer Sound Matching with Differentiable DSP" (ISMIR 2021, IEEE follow-up
  2023). A differentiable synth lets the loss be measured on audio, not only on parameters, which
  generalises better to sounds the synth cannot reproduce exactly.
- *Tensynth* (OCAD): audio-to-parameters with fully editable output.

**What it means for tonic2**

This is the best fit. Every approach above needs three things:

1. A known parameter list with ranges.
2. A deterministic renderer.
3. A distance between two sounds.

tonic2 has 1 and 2 already: `ParamSpec`/`Layout` (name, range, log flag, default) and `render/offline.hpp`.
Golden renders are already bit-identical across compilers, which makes the training data reproducible.
Missing is item 3, a spectral distance (multi-resolution STFT plus MFCC), and a search loop.

The cheapest first version needs no neural net. Run CMA-ES or a genetic algorithm over the macro or
parameter space with `tonic2_render` in the loop. That reproduces the Dexed study's baselines and
gives a dataset and an evaluation harness for a learned model later.

## 2. Text-to-patch (language models)

The question here: given a sentence like "screaming acid lead", produce an editable patch.

**Products:** *Tilichu* (Butterlamp Audio, August 2026) is a wavetable synth with a built-in
text-to-preset generator. *Sonarch* showed a text-to-patch generator at Synthfest UK. *SynthGPT* is an
earlier plugin of the same kind.

**Research**

- *CTAG* (MIT): a virtual modular synth with only 78 parameters, optimised iteratively against a
  text-audio similarity model. The result is abstract, "sketch-like" sounds that stay inspectable and tweakable.
- *AutoSynth* (arXiv 2610.10774, 2026): learns to write editable synth *programs* (parameters,
  modulation routes and MIDI performance) from text or audio, trained on augmented native presets.
- *SynthScribe* (ACM IUI 2024): multimodal retrieval and editing of synth sounds.
- Practitioner write-ups (e.g. Christian Tapp's Serum agent) use an LLM with tool calls to fill in a
  preset format, then render and check the result.

**What it means for tonic2**

Two designs fit, and both use structures tonic2 already exposes.

- **Parameter mode:** the model fills in a `Layout`, a short list of named, ranged parameters, as JSON. The
  8 macros are an even smaller target. This is CTAG's idea; small, named parameter spaces are what make it work.
- **Program mode:** the model writes a DSL patch (`dsl::Patch`, `voices`, `ladder`, `>>` effects). This is
  AutoSynth's idea and more expressive, but the output must be compiled and checked.
  `Patch::compile` already reports errors, so an agent loop can fix its own mistakes.

Either way the loop is: generate, compile, render, score with CLAP-style text-audio similarity, repeat.
The renderer and compiler are deterministic, so every candidate can be checked automatically.

## 3. Neural synthesis and effects

**Real-time neural audio**

- *RAVE* (IRCAM): a variational autoencoder that runs at 48 kHz on a CPU. Its licence comes from IRCAM, so check it
  before redistributing.
- *DDSP* (Google Magenta, DDSP-VST): a network predicts harmonic amplitudes and filtered noise from pitch
  and loudness. It is small, works in real time, and keeps the expressive detail that MIDI throws away.
- *Neutone* (Qosmo / Neutone Inc., Tokyo): a free plugin that hosts community RAVE and DDSP models, plus "neu."
  devices such as neural resynthesis from latent grains and real-time neural pitch estimation.
- *Neural Amp Modeler* / *TONE3000*: captures of amps and pedals as small networks (WaveNet- and LSTM-style),
  with a big community library of captures.

**What it means for tonic2**

DDSP is the closest match to tonic2's design. Its output stage is an additive oscillator bank plus
filtered noise, both easy as kernels. A network supplies only slow control signals (harmonic
amplitudes and a noise filter shape) at control rate. That suits tonic2's per-sample control
buffers: the network can run once per block, and the kernels interpolate between results.

RAVE and NAM run the network at audio rate. They need an inference node with fixed buffers and
reported latency.

## 4. AI mixing and mastering assistants

- *iZotope Neutron 5 / Ozone 12*: analyse tracks and suggest starting chains for EQ, compression and loudness;
  Ozone 12 adds stem-level control.
- *Sonible smart:bundle*: per-track smart EQ, compression, limiting, reverb and unmasking.
- *MixingGPT* and similar add a conversational layer that gives feedback rather than doing the processing.

Reviewers agree: these tools give a starting point, and a human still makes the decisions.

**What it means for tonic2:** low priority. The relevant pieces are the mixer strips and the FX matrix
(M6). An assistant would need loudness and spectral meters first (RMS/LUFS meters are already an
open bead), then a suggestion step that writes strip parameters. Treat it as a later demo, not a core feature.

## 5. Real-time inference tooling

| Library | What it is | Fit for tonic2 |
|-|-|-|
| **RTNeural** (Chowdhury) | Header-friendly C++ inference with compile-time-sized layers (Dense, GRU, LSTM, Conv1D) and no allocation in the forward pass; already used in shipping plugins and as a SuperCollider UGen | Best first choice: matches the "plain struct, no allocation after prepare" rule |
| **ANIRA** (arXiv 2506.12665) | Architecture that runs ONNX Runtime, LibTorch or TFLite off the audio thread, with latency management and benchmarking | For bigger models (RAVE-size); costs one buffer of latency |
| **Cmajor ML tools** | Converts ONNX and RTNeural models to pure Cmajor code | Shows that small models can be compiled to plain DSP code, an option for a Go or Rust port |
| ONNX Runtime directly | General-purpose | Allocates and spins up threads; never call it on the audio thread |

The DAFx26 paper "Real-time neural audio on Apple Silicon" benchmarks RTNeural against anira-ONNX RT
under realistic DAW load. The two make different trade-offs: RTNeural runs inline, while ANIRA moves the
work to another thread and adds latency. That matches the split in this document: small models inline, large models off-thread.

## 6. Research directions worth tracking

- **Differentiable DSP** (review: Hayes et al., Frontiers in Signal Processing, 2023/2024). Writing
  synth kernels so gradients flow through them is what makes audio-domain losses work for sound matching.
  For tonic2 this would mean a Python/JAX or PyTorch *mirror* of a few kernels (oscillator, SVF, ADSR),
  checked against the C++ golden renders. The C++ core stays as it is.
- **Editable outputs over waveforms.** The newest work (CTAG, AutoSynth) deliberately produces synth
  programs instead of raw audio, because musicians want to keep editing. That is tonic2's niche.
- **Latency-aware hosting.** ANIRA-style reported latency ties in with the existing "host latency
  reporting" bead (the oversampler already has 31 samples of latency to report).

## 7. Proposed tonic2 work, in order

1. **Sound-matching harness** (`tools/match`): spectral distance plus a CMA-ES or genetic search over
   `Layout`/macros, driving the offline renderer. No ML dependencies. It creates the evaluation set and
   dataset generator that everything else reuses.
2. **Patch agent:** a small CLI that takes a text prompt and asks an LLM for a `Layout` JSON (later a DSL
   patch), then compiles, renders and scores it, looping on compile errors. Kept outside the core
   library, in `tools/` or a separate repo.
3. **Inference node** (`graph/neural_node.hpp`, optional, off by default): RTNeural-backed, fixed-size
   model, weights loaded in `prepare`, control-rate or audio-rate mode, latency reported. Test it with
   the allocation tracker and a golden render from a tiny bundled model.
4. **DDSP-style harmonic voice:** an additive oscillator bank plus a filtered-noise kernel driven by
   control signals. It is useful on its own, and it is the target for item 3 when run at control rate.
5. **Differentiable mirror** (Python) of oscillator, SVF and ADSR, verified against goldens. Only if
   item 1 shows the gradient-free search isn't good enough.

Port-friendliness (goal e): items 1, 2 and 4 are plain code with no ML runtime in the audio path, so they
port to Go and Rust directly. Item 3 is the only one tied to a C++ library. Keep it behind an interface
so a Go or Rust port can swap in its own backend (e.g. `tract` or `candle` for Rust) or code generated
Cmajor-style.

## Sources

- Synplant 2: soniccharge.com/synplant; MicroMusic: forum.vital.audio/t/ai-patch-creation-tool/13996
- Audial Serum 2 generator: audialmusic.ai/tools/serum-2-preset-generator; Jamahook: jamahook.com
- Chatmu Sound and Tilichu coverage: chatmu.io/blog/ai-synth-plugin-chatmu-sound; sonicstate.com (2026-08-26)
- AutoSynth: arxiv.org/abs/2610.10774; CTAG: ctag.media.mit.edu; SynthScribe: doi.org/10.1145/3640543.3645158
- Patch-generation link collection: gist.github.com/0xdevalias/5a06349b376d01b2a76ad27a86b08c1b
- InverSynth: arxiv.org/abs/1812.06349; Dexed study: ieeexplore.ieee.org/document/8323327
- preset-gen-vae: github.com/gwendal-lv/preset-gen-vae; Syntheon: github.com/gudgud96/syntheon; torchsynth: github.com/torchsynth/torchsynth
- Masuda & Saito: archives.ismir.net/ismir2021/paper/000053.pdf; IEEE follow-up doc 10017350
- DDSP review (Hayes et al.): arxiv.org/abs/2308.15422
- Neutone: neutone.jp; RAVE licensing note: old.qosmo.jp/en/projects/neutone-2; DDSP-VST: magenta.withgoogle.com/ddsp-vst
- RTNeural: github.com/jatinchowdhury18/RTNeural; ANIRA: arxiv.org/abs/2506.12665
- Cmajor ML: cmajor.dev/docs/Tools/MachineLearning; DAFx26 Apple Silicon benchmark: dafx26.mit.edu/assets/papers/DAFx26_paper_16.pdf
- Mixing assistants: musehub.com/learn/best-ai-plugins; mixinggpt.com/blog/best-ai-mixing-plugins-2026; sonible.com
