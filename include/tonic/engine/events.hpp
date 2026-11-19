// Time-stamped events delivered with an Engine::process() call.
#pragma once

#include <cstdint>

namespace tonic::engine {

struct Event {
  enum class Type : uint8_t { NoteOn, NoteOff, Param };
  Type type = Type::Param;
  int frame = 0;      // sample offset inside the process() call
  int key = 0;        // MIDI note (NoteOn/NoteOff) or parameter id (Param)
  float value = 0.f;  // velocity 0..1 (NoteOn) or plain parameter value (Param)

  static Event noteOn(int frame, int note, float velocity = 1.f) { return {Type::NoteOn, frame, note, velocity}; }
  static Event noteOff(int frame, int note) { return {Type::NoteOff, frame, note, 0.f}; }
  static Event param(int frame, int id, float value) { return {Type::Param, frame, id, value}; }
};

}  // namespace tonic::engine
