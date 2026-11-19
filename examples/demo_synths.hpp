// Demo synths written with the patching syntax. Three are ports of Tonic 1's "Demo Synths"
// (examples/Demo Synths in the Tonic repo); differences from the originals are noted inline.
// Each demo also provides an event script so it can be rendered offline.
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "tonic/dsl/mixer.hpp"
#include "tonic/dsl/patch.hpp"
#include "tonic/engine/events.hpp"

namespace demos {

using namespace tonic;
using engine::Event;

struct Demo {
  std::string name;
  std::function<void(dsl::Patch&)> build;
  // Events for an offline render at `sampleRate`; param ids follow declaration order in build().
  std::function<std::vector<Event>(double sampleRate)> script;
  double seconds;
};

// Mono synth: two oscillators on the played note, ladder with envelope + macro control.
inline void monoSynth(dsl::Patch& p) {
  auto cutoff = p.param("cutoff", 80.f, 12000.f, 500.f, 0.03f);
  auto resonance = p.param("resonance", 0.f, 1.f, 0.3f, 0.03f);
  auto bright = p.param("brightness", 0.f, 1.f, 0.f, 0.05f);
  p.macro(bright, {{cutoff, 0.f, 3000.f, 2.f}, {resonance, 0.f, 0.4f, 1.f}});
  auto env = p.adsr(0.005, 0.25, 0.4, 0.3);
  auto osc = p.saw(p.pitch()) + p.square(p.pitch() * 0.5f, 0.3f) * 0.6f;
  p.output(osc * env * 0.35f >> dsl::ladder(cutoff + env * 2500.f, resonance, 1.5) >> dsl::pan(0.f));
}

// Tonic 1 SineSumSynth: 10 sines spaced by fourths, all following one "pitch" control.
inline void sineSum(dsl::Patch& p) {
  constexpr int kSines = 10;
  auto pitch = p.param("pitch", 0.f, 1.f, 0.f, 0.05f);  // Tonic 1 used .smoothed() per sine; params smooth here
  std::vector<dsl::Signal> sines;
  for (int s = 0; s < kSines; ++s)
    sines.push_back(p.sine((pitch * 220.f + 220.f) * std::pow(2.f, (s - kSines / 2) * 5.f / 12.f)));
  p.output(p.mix(sines) * ((1.f / kSines) * 0.5f) >> dsl::pan(0.f));
}

// Tonic 1 FMDroneSynth. Difference: Tonic 1 drove the LFO rate with LFNoise (random, 0.5 Hz);
// this port uses a slow sine (0.37 Hz) so renders are deterministic.
inline void fmDrone(dsl::Patch& p) {
  auto volume = p.param("volume", -60.f, 0.f, -12.f);
  auto carrierPitch = p.param("carrierPitch", 20.f, 32.f, 28.f);
  auto modIndex = p.param("modIndex", 0.f, 1.f, 0.25f);
  auto lfoAmt = p.param("lfoAmt", 0.f, 1.f, 0.5f);
  auto carrier = p.mtof(carrierPitch);
  auto modFreq = carrier * 4.f;
  auto lfoRate = (p.sine(0.37f) + 1.f) * 2.f + 0.2f;
  auto depth = modIndex * (1.f + p.sine(lfoRate) * (lfoAmt * 0.5f));
  auto out = p.sine(carrier + p.sine(modFreq) * modFreq * depth) * p.dbToGain(volume) *
             ((p.sine(0.15f) + 1.f) * 0.75f + 0.25f);
  p.output(out >> dsl::pan(0.f));
}

// Tonic 1 FilteredNoiseSynth, simplified. Differences: white instead of pink noise; band centres are
// not floored to whole semitones; tremolo rates are fixed instead of random; the random-metro "swell"
// on the bass is a constant.
inline void filteredNoise(dsl::Patch& p) {
  const float midi[] = {-24.f, 0.f, 3.f, 7.f, 10.f};
  const float tremoloHz[] = {0.13f, 0.21f, 0.17f, 0.29f, 0.11f};
  auto cutoff = p.param("cutoff", 0.f, 1.f, 0.5f);
  auto q = p.param("Q", 0.7f, 20.f, 5.f);
  auto noise = p.noise(7);
  std::vector<dsl::Signal> bands;
  for (int i = 0; i < 5; ++i) {
    auto centre = p.mtof(cutoff * 100.f + midi[i]);
    auto tremolo = (p.sine(tremoloHz[i]) + 1.5f) * 0.3f;
    bands.push_back((noise >> dsl::bandpass(centre, q)) / q * tremolo);  // / q: normalise band gain
  }
  auto bassPw = 0.5f + 0.2f * (p.sine(0.013f) + 1.f);
  auto bass = p.square(p.mtof(cutoff * 100.f + (midi[0] + 12.f)), bassPw) >>
              dsl::lowpass(600.f + p.sine(10.f) * 250.f);
  p.output((p.mix(bands) * (1.f + q * 0.05f) + bass * 0.05f) >> dsl::pan(0.f));
}

// Polyphonic synth: 8 voices with 2-voice unison, per-voice filter envelope, shared cutoff param and a
// stereo delay on the summed voices.
inline void polySynth(dsl::Patch& p) {
  auto cutoff = p.param("cutoff", 100.f, 8000.f, 900.f, 0.05f);
  auto voices = p.voices({8, dsl::VoiceMode::Poly, 2, 9.f}, [&](dsl::Patch& v) {
    auto env = v.adsr(0.01, 0.4, 0.5, 0.6);
    auto osc = v.saw(v.pitch()) * 0.5f + v.triangle(v.pitch() * 2.f) * 0.2f;
    v.output(osc * env * v.velocity() * 0.12f >> dsl::lowpass(v.use(cutoff) + env * 2500.f, 1.2f));
  });
  p.output(voices >> dsl::pan(0.f) >> dsl::delay(0.33, 0.35f, 0.25f));
}

// A small mix: poly pad and mono bass in a "keys" group, a noise hat on its own strip, a delay return
// fed by pad and hat sends, and a "space" macro that raises the send and lowers the dry pad.
inline void mixedSong(dsl::Patch& p) {
  auto space = p.param("space", 0.f, 1.f, 0.2f, 0.1f);
  auto pad = p.voices({6, dsl::VoiceMode::Poly, 2, 7.f, /*channel=*/0}, [](dsl::Patch& v) {
    auto env = v.adsr(0.05, 0.5, 0.6, 0.8);
    v.output(v.saw(v.pitch()) * env * 0.08f >> dsl::lowpass(1500.f, 0.8f));
  });
  // Routing: the pad plays MIDI channel 1 (index 0), the legato bass channel 2 (index 1).
  auto bass = p.voices({1, dsl::VoiceMode::Legato, 1, 0.f, /*channel=*/1}, [](dsl::Patch& v) {
    auto env = v.adsr(0.003, 0.15, 0.7, 0.1);
    v.output(v.square(v.pitch(), 0.4f) * env * 0.25f >> dsl::ladder(350.f + env * 900.f, 0.35f, 1.5));
  });
  auto hatEnv = (p.sine(4.f) + 1.f) * 0.5f;  // 8th-note pulse at 120 bpm
  auto hat = (p.noise(3) >> dsl::highpass(7000.f, 0.9f)) * hatEnv * hatEnv * hatEnv * 0.05f;

  dsl::Mixer mix(p);
  auto echo = mix.fxReturn([](dsl::Signal in) { return in >> dsl::lowpass(3500.f) >> dsl::delay(0.375, 0.45f, 1.f); });
  auto keys = mix.group({.gainDb = -1.f});
  auto padStrip = mix.strip(pad, {.gainDb = 0.f - space * 4.f, .pan = -0.25f}, &keys);
  mix.strip(bass, {.gainDb = -2.f, .pan = 0.f}, &keys);
  auto hatStrip = mix.strip(hat, {.gainDb = -3.f, .pan = 0.35f});
  mix.send(padStrip, echo, space * 0.6f);
  mix.send(hatStrip, echo, 0.2f);
  p.output(mix.master({.gainDb = -1.f}));
}

inline std::vector<Demo> all() {
  return {
      {"mixed", mixedSong,
       [](double sr) {
         std::vector<Event> ev;
         const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
         const int bar = static_cast<int>(sr * 1.0);
         const int bassRoots[4] = {45, 41, 43, 40};
         for (int c = 0; c < 4; ++c) {
           for (int n : chords[c]) { ev.push_back(Event::noteOn(c * bar, n, 0.7f, 0)); ev.push_back(Event::noteOff(c * bar + bar - 2000, n, 0)); }
           for (int k = 0; k < 4; ++k) {  // bass: root on each beat, channel 2 (index 1)
             const int at = c * bar + k * bar / 4;
             ev.push_back(Event::noteOn(at, bassRoots[c] - (k == 2 ? 12 : 0), 0.9f, 1));
             ev.push_back(Event::noteOff(at + bar / 5, bassRoots[c] - (k == 2 ? 12 : 0), 1));
           }
         }
         ev.push_back(Event::param(2 * bar, 0, 0.9f));
         return ev;
       },
       5.0},
      {"poly", polySynth,
       [](double sr) {
         std::vector<Event> ev;
         const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
         const int bar = static_cast<int>(sr * 0.8);
         for (int c = 0; c < 4; ++c)
           for (int n : chords[c]) {
             ev.push_back(Event::noteOn(c * bar, n, 0.8f));
             ev.push_back(Event::noteOff(c * bar + bar * 7 / 8, n));
           }
         for (int k = 0; k <= 12; ++k) ev.push_back(Event::param(k * bar / 3, 0, 500.f + 200.f * k));
         return ev;
       },
       4.0},
      {"mono", monoSynth,
       [](double sr) {
         std::vector<Event> ev;
         const int notes[] = {45, 48, 52, 57, 55, 52, 48, 50};
         const int step = static_cast<int>(sr * 0.25);
         for (int i = 0; i < 8; ++i) {
           ev.push_back(Event::noteOn(i * step, notes[i], 0.9f));
           ev.push_back(Event::noteOff(i * step + step * 3 / 4, notes[i]));
         }
         for (int k = 0; k <= 16; ++k) ev.push_back(Event::param(k * step / 2, 2, k / 16.f));  // brightness
         return ev;
       },
       2.25},
      {"sinesum", sineSum,
       [](double sr) {
         std::vector<Event> ev;
         for (int k = 0; k <= 20; ++k) ev.push_back(Event::param(static_cast<int>(k * sr * 0.15), 0, (k % 7) / 6.f));
         return ev;
       },
       3.0},
      {"fmdrone", fmDrone,
       [](double sr) {
         return std::vector<Event>{Event::param(static_cast<int>(sr * 1.5), 2, 0.8f),
                                   Event::param(static_cast<int>(sr * 3.0), 1, 24.f)};
       },
       4.0},
      {"filterednoise", filteredNoise,
       [](double sr) {
         return std::vector<Event>{Event::param(static_cast<int>(sr * 1.0), 0, 0.62f),
                                   Event::param(static_cast<int>(sr * 2.0), 1, 12.f)};
       },
       3.0},
  };
}

}  // namespace demos
