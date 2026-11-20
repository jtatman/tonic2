// Time-stamped events delivered with an Engine::process() call.
#pragma once

#include <cstdint>

namespace tonic::engine {

struct Event {
  enum class Type : uint8_t { NoteOn, NoteOff, Param, Expression, ControlChange, PitchWheel };
  enum class Expr : uint8_t { PitchBend, Pressure, Timbre };  // per-note (CLAP note expressions / MPE)
  Type type = Type::Param;
  int frame = 0;      // sample offset inside the process() call
  int key = 0;        // MIDI note (NoteOn/NoteOff/Expression), parameter id (Param) or CC number (ControlChange)
  float value = 0.f;  // velocity 0..1 (NoteOn), plain parameter value (Param), expression value, or CC 0..1
  Expr expr = Expr::PitchBend;  // Expression only: bend in semitones, pressure and timbre 0..1
  int channel = -1;   // MIDI channel 0..15, or -1 = unspecified (reaches every voice node)

  static Event noteOn(int frame, int note, float velocity = 1.f, int channel = -1) {
    return {Type::NoteOn, frame, note, velocity, Expr::PitchBend, channel};
  }
  static Event noteOff(int frame, int note, int channel = -1) { return {Type::NoteOff, frame, note, 0.f, Expr::PitchBend, channel}; }
  // Pitch wheel: value -1..1 (scaled by the voice node's bend range, 2 semitones in the engine's mono path).
  static Event pitchWheel(int frame, float value, int channel = -1) {
    return {Type::PitchWheel, frame, 0, value, Expr::PitchBend, channel};
  }
  // MIDI CC: value normalised 0..1 (MIDI 0..127 / 127). Mapped to parameters by the engine.
  static Event cc(int frame, int number, float value, int channel = -1) {
    return {Type::ControlChange, frame, number, value, Expr::PitchBend, channel};
  }
  static Event param(int frame, int id, float value) { return {Type::Param, frame, id, value, Expr::PitchBend, -1}; }
  static Event expression(int frame, int note, Expr e, float value, int channel = -1) {
    return {Type::Expression, frame, note, value, e, channel};
  }
};

}  // namespace tonic::engine
