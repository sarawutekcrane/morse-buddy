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

  // Default member initializers would make this a non-aggregate under
  // C++11 (the standard PlatformIO's arduino-esp32 core builds with),
  // breaking brace-init call sites like InputEvent{type, value, dur} — so
  // this is a plain constructor instead.
  InputEvent(InputEventType t = InputEventType::NONE, int8_t v = 0, uint32_t d = 0)
      : type(t), value(v), durationMs(d) {}
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
