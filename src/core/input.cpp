#include "core/input.h"

#include <Arduino.h>
#include "esp_timer.h"

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
// Quadrature decoder (Hardware Fix #4.6).
//
// History: the original architecture polled CLK/DT once per
// Input::update() call, so capture depended on how often update() ran --
// busy redraw periods lost detents outright. GPIO CHANGE interrupts on
// both pins fixed that dependency, but real-hardware testing showed edge
// interrupts have no concept of "settled," only "changed just now": a
// shared two-pin ISR could observe both pins' already-final level and
// collapse an intermediate quadrature state, and a per-pin ISR followed
// mechanical contact bounce too literally, both producing missed or
// corrupted detents.
//
// This replaces GPIO edge interrupts entirely with a periodic STABLE-
// STATE sampler, independent of both interrupt timing and the Arduino
// loop/UI redraw: a dedicated esp_timer (ESP_TIMER_TASK dispatch --
// encoderSampleTimerCallback() runs as an ordinary FreeRTOS task
// callback, not in interrupt context) samples both pins every
// kEncoderSamplePeriodUs and only presents a new raw state to the
// detent-bound decoder below once that state has been observed for
// kEncoderStableSamples consecutive samples in a row. This waits out
// contact bounce structurally, the same way the transition table
// structurally rejects invalid jumps, without any additional
// edge-triggered heuristics. Real-hardware validation (20/20 CW, 20/20
// CCW, partial-turn-abort, immediate-direction-reversal, and fast
// human-CW passes) confirmed the stable sampler presents only clean
// single-step Gray-code transitions to the decoder.
//
// Decoder: a candidate detent only exists between leaving the measured
// rest state (CLK=HIGH, DT=HIGH == state 3, kEncoderRestState) and
// returning to it. kQuadratureTable only returns +-1 for the two valid
// forward/backward Gray-code steps from each state; any other observed
// transition (an invalid two-bit jump such as 3->0/2->1/1->2/0->3)
// yields 0 and marks the current cycle invalid, so it cannot silently be
// mistaken for a real step. A candidate detent completes only when its
// accumulated directional evidence reaches kDetentTransitions valid
// steps AND the encoder has returned to rest; returning to rest by any
// other means (an invalid cycle, or a partial turn that reverses back to
// the same detent) always resets the accumulator and emits nothing, so a
// corrupted or partial detent can never bleed into the next one and a
// same-detent abort never produces a phantom click. CW=+1, CCW=-1.
// ---------------------------------------------------------------------------
DRAM_ATTR const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};
constexpr int8_t kDetentTransitions = 4;
constexpr uint8_t kEncoderRestState = 3;  // measured rest/detent state: CLK HIGH, DT HIGH

constexpr uint32_t kEncoderSamplePeriodUs = 1000;  // 1ms sample period
constexpr uint8_t kEncoderStableSamples = 2;       // consecutive identical non-rest samples required to accept a new state

portMUX_TYPE g_encMux = portMUX_INITIALIZER_UNLOCKED;
volatile int8_t g_encIsrSubAccum = 0;         // directional evidence accumulated within the CURRENT cycle only; g_encMux-protected
volatile int32_t g_encIsrPendingDetents = 0;  // net whole detents not yet drained by update(); g_encMux-protected
volatile bool g_encSynced = false;        // true once REST has been observed at least once since boot; g_encMux-protected
volatile bool g_encCycleActive = false;   // true while away from REST inside a candidate detent; g_encMux-protected
volatile bool g_encCycleInvalid = false;  // true once this cycle has seen an invalid transition; g_encMux-protected

// Stable-state sampler bookkeeping. Touched ONLY from
// encoderSampleTimerCallback(), which ESP_TIMER_TASK dispatch guarantees
// runs as ordinary sequential (non-reentrant) calls on esp_timer's own
// task -- no other code reads or writes these, so they need no locking.
uint8_t g_encStableState = 0;     // last ACCEPTED stable (CLK<<1|DT) state -- what the decoder is fed as prevStable
uint8_t g_encCandidateState = 0;  // a raw state currently being evaluated for stability
uint8_t g_encCandidateCount = 0;  // consecutive samples candidateState has been observed (not yet accepted)

esp_timer_handle_t g_encoderSampleTimer = nullptr;
bool g_encoderSamplerFailed = false;  // set if esp_timer create/start fails; the encoder simply produces no events in that case

// Detent-bound decoder: called only with an already-accepted STABLE
// (prevStable, currStable) transition from encoderSampleTimerCallback()
// below, never reads GPIO itself. Runs in the esp_timer ESP_TIMER_TASK
// context (an ordinary FreeRTOS task, not interrupt context), so it uses
// the normal task-context critical section, never the _ISR variants.
void processEncoderStableTransition(uint8_t prevStable, uint8_t currStable) {
  portENTER_CRITICAL(&g_encMux);
  uint8_t idx = static_cast<uint8_t>((prevStable << 2) | currStable);
  int8_t dir = kQuadratureTable[idx & 0x0F];

  if (prevStable == currStable) {
    // The sampler only ever calls this with a genuine state change.
  } else if (!g_encSynced) {
    // Never fabricate a detent before REST has been observed at least
    // once since startup.
    if (currStable == kEncoderRestState) {
      g_encSynced = true;
      g_encIsrSubAccum = 0;
      g_encCycleActive = false;
      g_encCycleInvalid = false;
    }
  } else if (currStable == kEncoderRestState) {
    // Returning to REST is an absolute cycle boundary. Only a clean
    // (never-desynchronized) cycle's evidence is evaluated for
    // completion; either way, everything is unconditionally reset below
    // so no partial accumulation can ever survive past this point.
    if (g_encCycleActive && !g_encCycleInvalid && dir != 0) {
      g_encIsrSubAccum = static_cast<int8_t>(g_encIsrSubAccum + dir);
      if (g_encIsrSubAccum >= kDetentTransitions) {
        g_encIsrPendingDetents++;
      } else if (g_encIsrSubAccum <= -kDetentTransitions) {
        g_encIsrPendingDetents--;
      }
    }
    g_encIsrSubAccum = 0;
    g_encCycleActive = false;
    g_encCycleInvalid = false;
  } else if (dir != 0) {
    // Away from rest, valid Gray-code step.
    if (!g_encCycleActive) {
      if (prevStable == kEncoderRestState) {
        // A new cycle may start only when leaving REST.
        g_encCycleActive = true;
        g_encCycleInvalid = false;
        g_encIsrSubAccum = dir;
      } else {
        // A valid step observed while not already in a cycle and not
        // leaving from REST should not happen if the invariants above
        // hold; don't guess -- mark the cycle invalid instead.
        g_encCycleActive = true;
        g_encCycleInvalid = true;
        g_encIsrSubAccum = 0;
      }
    } else if (!g_encCycleInvalid) {
      // Keep accumulating; mechanical bounce naturally cancels via its
      // own opposite-signed contribution. Only a return to REST can
      // finalize a detent.
      g_encIsrSubAccum = static_cast<int8_t>(g_encIsrSubAccum + dir);
    }
    // else: this cycle is already invalid -- ignore further evidence
    // until REST is observed.
  } else {
    // Invalid two-bit jump away from rest (e.g. 3->0, 2->1, 1->2, 0->3).
    // Never guess a direction -- mark the cycle invalid and require a
    // return to REST before a new cycle can start.
    g_encCycleActive = true;
    g_encCycleInvalid = true;
    g_encIsrSubAccum = 0;
  }
  portEXIT_CRITICAL(&g_encMux);
}

// Periodic stable-state sampler: runs every kEncoderSamplePeriodUs from
// the esp_timer ESP_TIMER_TASK, independent of Input::update()/the
// Arduino loop. No Serial/Display/MQTT/NVS/LittleFS/heap/delay/
// pushEvent()/Sleep::notifyActivity() calls here; those stay task-context
// responsibilities in Input::update().
void encoderSampleTimerCallback(void* /*arg*/) {
  uint8_t clk = digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0;
  uint8_t dt = digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0;
  uint8_t rawState = static_cast<uint8_t>((clk << 1) | dt);

  if (rawState == g_encStableState) {
    // Back at the currently accepted state -- discard any in-progress
    // candidate; do not call the decoder.
    g_encCandidateState = g_encStableState;
    g_encCandidateCount = 0;
    return;
  }

  if (rawState != g_encCandidateState) {
    // A possible new state appeared, replacing any previous candidate.
    // Not accepted yet.
    g_encCandidateState = rawState;
    g_encCandidateCount = 1;
    return;
  }

  // rawState == candidateState != stableState.
  g_encCandidateCount++;
  if (g_encCandidateCount < kEncoderStableSamples) return;  // not stable yet

  uint8_t prevStable = g_encStableState;
  uint8_t newStable = g_encCandidateState;
  g_encStableState = newStable;
  g_encCandidateCount = 0;

  processEncoderStableTransition(prevStable, newStable);
}

// Drains whatever whole detents the decoder has accumulated since the
// last call and converts each into an ENCODER_ROTATE event (and
// Sleep::notifyActivity() call) -- this is the only place that touches
// g_encIsrPendingDetents from the Arduino loop task.
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

  uint8_t initialRaw = static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0) << 1 |
                                             (digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0));
  g_encStableState = initialRaw;
  g_encCandidateState = initialRaw;
  g_encCandidateCount = 0;

  g_encIsrSubAccum = 0;
  g_encIsrPendingDetents = 0;
  // Only trust the boot pin state as a synchronized rest position if it
  // actually IS rest; otherwise wait for the sampler to accept REST for
  // the first time before decoding any cycle, rather than fabricating a
  // detent from an unknown starting position.
  g_encSynced = (g_encStableState == kEncoderRestState);
  g_encCycleActive = false;
  g_encCycleInvalid = false;

  // No GPIO edge interrupts for CLK/DT (Hardware Fix #4.6) -- a periodic
  // esp_timer replaces them entirely; see the comment block above
  // encoderSampleTimerCallback(). Guarded so calling init() more than
  // once never creates a second periodic timer.
  if (g_encoderSampleTimer == nullptr) {
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &encoderSampleTimerCallback;
    timerArgs.arg = nullptr;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "enc_sample";
    esp_err_t createErr = esp_timer_create(&timerArgs, &g_encoderSampleTimer);
    if (createErr != ESP_OK) {
      g_encoderSampleTimer = nullptr;
      g_encoderSamplerFailed = true;
    } else {
      esp_err_t startErr = esp_timer_start_periodic(g_encoderSampleTimer, kEncoderSamplePeriodUs);
      if (startErr != ESP_OK) {
        g_encoderSamplerFailed = true;
      }
    }
  }
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
