// Time-stamped events delivered with an Engine::process() call.
#pragma once

#include <cstdint>

namespace tonic::engine {

struct Event {
  enum class Type : uint8_t { NoteOn, NoteOff, Param, Expression };
  enum class Expr : uint8_t { PitchBend, Pressure, Timbre };  // per-note (CLAP note expressions / MPE)
  Type type = Type::Param;
  int frame = 0;      // sample offset inside the process() call
  int key = 0;        // MIDI note (NoteOn/NoteOff/Expression) or parameter id (Param)
  float value = 0.f;  // velocity 0..1 (NoteOn), plain parameter value (Param), or expression value
  Expr expr = Expr::PitchBend;  // Expression only: bend in semitones, pressure and timbre 0..1

  static Event noteOn(int frame, int note, float velocity = 1.f) { return {Type::NoteOn, frame, note, velocity}; }
  static Event noteOff(int frame, int note) { return {Type::NoteOff, frame, note, 0.f}; }
  static Event param(int frame, int id, float value) { return {Type::Param, frame, id, value}; }
  static Event expression(int frame, int note, Expr e, float value) { return {Type::Expression, frame, note, value, e}; }
};

}  // namespace tonic::engine
