#pragma once
// Semantic input events for DOT/DASH button + rotary encoder (Addendum
// sections 1.6 and 9). Screen code must consume only these events; it must
// never read GPIOs or millis()-based timing itself.

#include <stdint.h>

enum class InputEventType : uint8_t {
  NONE = 0,
  ENCODER_ROTATE,     // value = +1 (CW) or -1 (CCW)
  ENCODER_LONG,       // fires once, edge-triggered, at exactly 500ms while still held
  ENCODER_SHORT,      // release before 500ms (no ENCODER_LONG preceded it)
  DOT_PRESS_START,    // DOT/DASH just went down (for instant hold-preview screens)
  DOT_RELEASE,        // DOT/DASH released; durationMs holds the press length
  COMBINED_START,     // DOT/DASH + Encoder press recognized as one combined gesture
  COMBINED_REVEAL,    // combined gesture held to >=2000ms, edge-triggered, still held
  COMBINED_RELEASE,   // combined gesture ended; durationMs holds total hold length
};

struct InputEvent {
  InputEventType type;
  int8_t value;
  uint32_t durationMs;
  // Hardware Fix #4.7d: the physical/debounced accepted event time
  // (millis()) at the moment this event's edge was actually accepted --
  // NOT when Input::popEvent() happened to be called for it. Fix #4.7b's
  // independent DOT/DASH timer already captures this instant in
  // ButtonEdgeRecord::atMs; this field is how it survives into the
  // semantic InputEvent so a busy main loop that only gets around to
  // draining several queued edges later can still reconstruct the user's
  // real key rhythm instead of the loop's own processing time. 0 means
  // "no physical time known" (e.g. a non-DOT/DASH event, or one synthesized
  // without one) -- every Morse-timing consumer must fall back to
  // millis() in that case (eventMs != 0 ? eventMs : millis()) so
  // synthetic/legacy/default-constructed events stay safe.
  uint32_t eventMs;

  // Default member initializers would make this a non-aggregate under
  // C++11 (the standard PlatformIO's arduino-esp32 core builds with),
  // breaking brace-init call sites like InputEvent{type, value, dur} — so
  // this is a plain constructor instead. The new eventMs parameter is
  // appended last with a 0 default so every existing 1/2/3-argument
  // construction/brace-init call site keeps compiling unchanged.
  InputEvent(InputEventType t = InputEventType::NONE, int8_t v = 0, uint32_t d = 0, uint32_t when = 0)
      : type(t), value(v), durationMs(d), eventMs(when) {}
};

namespace Input {

// One-time GPIO setup. Call from setup().
void init();

// Updates debounce/timing state machines and enqueues any semantic events
// completed since the last call. Must be called every loop() iteration
// (and is the single place that ever touches raw GPIO/millis() timing).
void update();

// Pops one queued event. Call in a loop after update() to drain everything
// generated this tick. Returns false (type NONE) when the queue is empty.
bool popEvent(InputEvent& outEvent);

// True if `e` is a general-navigation confirm: DOT/DASH released before the
// 2000ms special-command threshold (Addendum section 9.3).
bool isMenuConfirm(const InputEvent& e);

// True if `e` is a general-navigation "back one level" (Addendum section
// "General Confirm Principle": Encoder long, >=500ms).
bool isBack(const InputEvent& e);

}  // namespace Input
