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
// Quadrature decoder (Hardware Fix #4.3 issue A).
//
// The previous architecture polled CLK/DT once per Input::update() call
// with a 2ms stable-state debounce. On real hardware that made capture
// depend on how often update() actually ran -- if the main/UI loop was
// busy (e.g. mid screen redraw) for longer than a detent's transition
// window, edges were simply never sampled and the detent was lost
// (measured: up to 2/20 CW and 4/20 CCW physical detents missed).
//
// This replaces polling with GPIO CHANGE interrupts on both CLK and DT:
// every physical edge is captured the instant it happens, independent of
// loop()/redraw timing. The ISR does only minimal edge/state accounting
// (read two pins, look up direction in the same 2-bit transition table as
// before, accumulate) -- no Display, Sleep, millis(), Serial, MQTT, heap,
// NVS, pushEvent(), or Sleep::notifyActivity() calls inside it, per the
// requirement. Input::update() still owns everything else: it atomically
// drains whatever whole detents the ISR accumulated (via a FreeRTOS
// spinlock, the standard ESP32 ISR<->task synchronization primitive) and
// converts them into the same ENCODER_ROTATE events / Sleep::
// notifyActivity() calls as before, in normal task context.
//
// Debounce: rather than a time-based debounce (which an ISR cannot safely
// do without calling millis()/delay()), mechanical bounce is rejected
// structurally by the transition table itself -- kQuadratureTable only
// returns +-1 for the two valid forward/backward Gray-code steps from
// each state; any other observed transition (including a bounce that
// jumps to a non-adjacent or repeated state) yields 0 and is ignored, so
// a single physical detent's bounce cannot accumulate into a second
// logical step. Grouping 4 valid transitions into one logical detent
// (kDetentTransitions) is unchanged from before, preserving the existing
// CW/CCW direction semantics and one-physical-detent-per-logical-step
// behavior.
//
// kQuadratureTable is DRAM_ATTR (not left in flash-mapped .rodata) and
// the ISR itself is IRAM_ATTR, so both the code and the data it reads
// remain safely accessible even if the ISR fires while flash access is
// briefly unavailable (e.g. during an NVS commit) -- standard ESP32
// GPIO-ISR safety practice.
// ---------------------------------------------------------------------------
DRAM_ATTR const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};
constexpr int8_t kDetentTransitions = 4;

portMUX_TYPE g_encMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t g_encIsrLastState = 0;
volatile int8_t g_encIsrSubAccum = 0;        // raw transition-table accumulation between whole detents
volatile int32_t g_encIsrPendingDetents = 0;  // net whole detents not yet drained by update()

void IRAM_ATTR onEncoderChangeIsr() {
  uint8_t clk = digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0;
  uint8_t dt = digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0;
  uint8_t curr = static_cast<uint8_t>((clk << 1) | dt);

  portENTER_CRITICAL_ISR(&g_encMux);
  uint8_t idx = static_cast<uint8_t>((g_encIsrLastState << 2) | curr);
  g_encIsrLastState = curr;
  int8_t dir = kQuadratureTable[idx & 0x0F];
  if (dir != 0) {
    g_encIsrSubAccum = static_cast<int8_t>(g_encIsrSubAccum + dir);
    if (g_encIsrSubAccum >= kDetentTransitions) {
      g_encIsrSubAccum = 0;
      g_encIsrPendingDetents++;
    } else if (g_encIsrSubAccum <= -kDetentTransitions) {
      g_encIsrSubAccum = 0;
      g_encIsrPendingDetents--;
    }
  }
  portEXIT_CRITICAL_ISR(&g_encMux);
}

// Drains whatever whole detents the ISR has accumulated since the last
// call and converts each into the same ENCODER_ROTATE event (and
// Sleep::notifyActivity() call) the old polled path produced -- this is
// the only place that touches g_encIsrPendingDetents from task context.
void drainQuadrature() {
  int32_t pending;
  portENTER_CRITICAL(&g_encMux);
  pending = g_encIsrPendingDetents;
  g_encIsrPendingDetents = 0;
  portEXIT_CRITICAL(&g_encMux);

  while (pending > 0) {
    pushEvent(InputEventType::ENCODER_ROTATE, +1);
    Sleep::notifyActivity();
    pending--;
  }
  while (pending < 0) {
    pushEvent(InputEventType::ENCODER_ROTATE, -1);
    Sleep::notifyActivity();
    pending++;
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

  g_encIsrLastState = static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0) << 1 |
                                            (digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0));
  g_encIsrSubAccum = 0;
  g_encIsrPendingDetents = 0;

  // Either pin's edge can be the first or second half of a valid
  // transition, so both are watched by the same ISR (Hardware Fix #4.3
  // issue A).
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderClk), onEncoderChangeIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderDt), onEncoderChangeIsr, CHANGE);
}

void update() {
  uint32_t now = millis();

  updateDebounce(g_dot, now);
  updateDebounce(g_encSw, now);
  drainQuadrature();

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
