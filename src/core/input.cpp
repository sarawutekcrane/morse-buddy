#include "core/input.h"

#include <Arduino.h>

#include "core/morse.h"
#include "core/pins.h"
#include "core/sleep.h"

namespace {

// ---------------------------------------------------------------------------
// Tunables (Addendum section 1.6 / 9).
// ---------------------------------------------------------------------------
constexpr uint32_t kButtonDebounceMs = 10;      // DOT/DASH and Encoder SW
constexpr uint32_t kQuadratureDebounceMs = 2;    // Encoder CLK/DT edges
constexpr uint32_t kEncoderLongPressMs = 500;
constexpr uint32_t kCombinedWindowMs = 200;
constexpr uint32_t kCombinedRevealMs = Morse::kSpecialCommandMs;  // 2000ms

// ---------------------------------------------------------------------------
// Event queue.
// ---------------------------------------------------------------------------
constexpr uint8_t kQueueSize = 8;
InputEvent g_queue[kQueueSize];
uint8_t g_queueHead = 0;
uint8_t g_queueTail = 0;
uint8_t g_queueCount = 0;

void pushEvent(InputEventType type, int8_t value = 0, uint32_t durationMs = 0) {
  if (g_queueCount >= kQueueSize) return;  // drop if the consumer is not draining fast enough
  g_queue[g_queueTail] = InputEvent{type, value, durationMs};
  g_queueTail = static_cast<uint8_t>((g_queueTail + 1) % kQueueSize);
  g_queueCount++;
}

// ---------------------------------------------------------------------------
// Debounced digital button (active LOW: pressed == digitalRead() == LOW).
// ---------------------------------------------------------------------------
struct DebouncedButton {
  int pin;
  bool stablePressed;
  bool lastRaw;
  uint32_t lastChangeMs;

  // Plain constructor (not default member initializers) so this stays
  // usable with direct-list-init like DebouncedButton g_dot{Pins::kDotDash}
  // under C++11.
  explicit DebouncedButton(int p) : pin(p), stablePressed(false), lastRaw(false), lastChangeMs(0) {}
};

void updateDebounce(DebouncedButton& b, uint32_t now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.lastChangeMs = now;
  } else if (now - b.lastChangeMs >= kButtonDebounceMs) {
    b.stablePressed = raw;
  }
}

DebouncedButton g_dot{Pins::kDotDash};
DebouncedButton g_encSw{Pins::kEncoderSw};

// ---------------------------------------------------------------------------
// Quadrature decoder (direct decode, no third-party library).
// Standard 2-bit-state transition table indexed by (prevState<<2 | currState).
// ---------------------------------------------------------------------------
const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

uint8_t g_encRawState = 0;
uint32_t g_encRawChangeMs = 0;
uint8_t g_encStableState = 0;
int8_t g_encAccum = 0;

void updateQuadrature(uint32_t now) {
  uint8_t clk = digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0;
  uint8_t dt = digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0;
  uint8_t curr = static_cast<uint8_t>((clk << 1) | dt);

  if (curr != g_encRawState) {
    g_encRawState = curr;
    g_encRawChangeMs = now;
    return;
  }
  if (now - g_encRawChangeMs < kQuadratureDebounceMs) return;
  if (curr == g_encStableState) return;

  uint8_t idx = static_cast<uint8_t>((g_encStableState << 2) | curr);
  int8_t dir = kQuadratureTable[idx & 0x0F];
  g_encStableState = curr;
  if (dir == 0) return;

  g_encAccum = static_cast<int8_t>(g_encAccum + dir);
  if (g_encAccum >= 4) {
    g_encAccum = 0;
    pushEvent(InputEventType::ENCODER_ROTATE, +1);
    Sleep::notifyActivity();
  } else if (g_encAccum <= -4) {
    g_encAccum = 0;
    pushEvent(InputEventType::ENCODER_ROTATE, -1);
    Sleep::notifyActivity();
  }
}

// ---------------------------------------------------------------------------
// DOT/DASH + Encoder-switch press state machines, with combined-gesture
// arbitration (Addendum section 9.8 / "Combined Gesture").
// ---------------------------------------------------------------------------
enum class PressState : uint8_t { IDLE, INDIVIDUAL, COMBINED };

PressState g_dotPressState = PressState::IDLE;
uint32_t g_dotDownMs = 0;

PressState g_encPressState = PressState::IDLE;
uint32_t g_encDownMs = 0;
bool g_encLongFired = false;

bool g_combinedActive = false;
uint32_t g_combinedStartMs = 0;
bool g_combinedRevealFired = false;

void enterCombined(uint32_t now) {
  g_dotPressState = PressState::COMBINED;
  g_encPressState = PressState::COMBINED;
  g_combinedActive = true;
  g_combinedStartMs = now;
  g_combinedRevealFired = false;
  pushEvent(InputEventType::COMBINED_START);
}

void endCombined(uint32_t now) {
  uint32_t duration = now - g_combinedStartMs;
  pushEvent(InputEventType::COMBINED_RELEASE, 0, duration);
  g_combinedActive = false;
  g_dotPressState = PressState::IDLE;
  g_encPressState = PressState::IDLE;
  g_encLongFired = false;
}

void updateDotStateMachine(uint32_t now) {
  bool pressed = g_dot.stablePressed;

  if (pressed && g_dotPressState == PressState::IDLE) {
    g_dotDownMs = now;
    Sleep::notifyActivity();
    if (g_encPressState == PressState::INDIVIDUAL && (now - g_encDownMs) < kCombinedWindowMs) {
      enterCombined(now);
    } else {
      g_dotPressState = PressState::INDIVIDUAL;
      pushEvent(InputEventType::DOT_PRESS_START);
    }
    return;
  }

  if (pressed && g_dotPressState == PressState::INDIVIDUAL) {
    Sleep::notifyActivity();
    return;
  }

  if (!pressed && g_dotPressState == PressState::INDIVIDUAL) {
    uint32_t duration = now - g_dotDownMs;
    pushEvent(InputEventType::DOT_RELEASE, 0, duration);
    g_dotPressState = PressState::IDLE;
    return;
  }

  if (!pressed && g_combinedActive && g_dotPressState == PressState::COMBINED) {
    endCombined(now);
    return;
  }
}

void updateEncoderSwStateMachine(uint32_t now) {
  bool pressed = g_encSw.stablePressed;

  if (pressed && g_encPressState == PressState::IDLE) {
    g_encDownMs = now;
    g_encLongFired = false;
    Sleep::notifyActivity();
    if (g_dotPressState == PressState::INDIVIDUAL && (now - g_dotDownMs) < kCombinedWindowMs) {
      enterCombined(now);
    } else {
      g_encPressState = PressState::INDIVIDUAL;
    }
    return;
  }

  if (pressed && g_encPressState == PressState::INDIVIDUAL) {
    Sleep::notifyActivity();
    if (!g_encLongFired && (now - g_encDownMs) >= kEncoderLongPressMs) {
      g_encLongFired = true;
      pushEvent(InputEventType::ENCODER_LONG);
    }
    return;
  }

  if (!pressed && g_encPressState == PressState::INDIVIDUAL) {
    if (!g_encLongFired) {
      pushEvent(InputEventType::ENCODER_SHORT);
    }
    g_encPressState = PressState::IDLE;
    return;
  }

  if (!pressed && g_combinedActive && g_encPressState == PressState::COMBINED) {
    endCombined(now);
    return;
  }
}

void updateCombinedHold(uint32_t now) {
  if (!g_combinedActive || g_combinedRevealFired) return;
  if (now - g_combinedStartMs >= kCombinedRevealMs) {
    g_combinedRevealFired = true;
    pushEvent(InputEventType::COMBINED_REVEAL);
  }
}

}  // namespace

namespace Input {

void init() {
  pinMode(Pins::kDotDash, INPUT_PULLUP);
  pinMode(Pins::kEncoderSw, INPUT_PULLUP);
  pinMode(Pins::kEncoderClk, INPUT_PULLUP);
  pinMode(Pins::kEncoderDt, INPUT_PULLUP);

  uint32_t now = millis();
  g_encRawState = static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0) << 1 |
                                        (digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0));
  g_encStableState = g_encRawState;
  g_encRawChangeMs = now;
}

void update() {
  uint32_t now = millis();

  updateDebounce(g_dot, now);
  updateDebounce(g_encSw, now);
  updateQuadrature(now);

  updateDotStateMachine(now);
  updateEncoderSwStateMachine(now);
  updateCombinedHold(now);
}

bool popEvent(InputEvent& outEvent) {
  if (g_queueCount == 0) {
    outEvent = InputEvent{};
    return false;
  }
  outEvent = g_queue[g_queueHead];
  g_queueHead = static_cast<uint8_t>((g_queueHead + 1) % kQueueSize);
  g_queueCount--;
  return true;
}

bool isMenuConfirm(const InputEvent& e) {
  return e.type == InputEventType::DOT_RELEASE && e.durationMs < Morse::kSpecialCommandMs;
}

bool isBack(const InputEvent& e) {
  return e.type == InputEventType::ENCODER_LONG;
}

}  // namespace Input
