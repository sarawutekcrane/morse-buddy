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
constexpr uint32_t kButtonDebounceMs = 10;      // Encoder SW only (Hardware Fix #4.7b: DOT/DASH now uses its own independent esp_timer sampler, see below)
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
  // usable with direct-list-init like DebouncedButton g_encSw{Pins::kEncoderSw}
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

DebouncedButton g_encSw{Pins::kEncoderSw};

// ---------------------------------------------------------------------------
// DOT/DASH reliable capture (Hardware Fix #4.7b).
//
// Real-hardware testing found 3 of 10 deliberate short DOT presses (at both
// WPM 10 and WPM 8) produced no character at all -- not misclassified as
// DASH, simply missing. The previous mechanism (updateDebounce() polled
// once per Input::update() call, exactly the same shape of bug the
// quadrature encoder already had before Hardware Fix #4.6) meant a
// complete short press+release could occur entirely between two loop
// iterations and never be observed.
//
// This applies the same fix already validated for the encoder: a periodic
// esp_timer (ESP_TIMER_TASK dispatch, its own independent timer -- NOT
// shared with the encoder's, so this has zero code/state overlap with the
// validated Hardware Fix #4.6 quadrature sampler/decoder) samples the pin
// every kButtonSamplePeriodUs and only accepts a new stable level after
// kButtonStableSamples consecutive identical samples (~10ms at 1ms
// sampling, matching the previous kButtonDebounceMs). Unlike the encoder
// (which only needs the latest stable STATE), a button press/release is a
// pair of discrete EVENTS whose durations matter, so every accepted
// PRESS/RELEASE transition is timestamped and queued -- if a complete
// press+release both become stable while Input::update() is not running,
// BOTH are still in the queue, in order, with their own real timestamps,
// the next time it runs; nothing can be missed just because the level was
// only sampled once.
//
// The encoder switch (Pins::kEncoderSw) deliberately keeps its existing
// updateDebounce()/polled mechanism unchanged -- it was never reported as
// having this problem, and migrating only the pin that actually needs it
// keeps this fix as small as it can be.
constexpr uint32_t kButtonSamplePeriodUs = 1000;  // 1ms sample period, independent of the encoder's own timer
constexpr uint8_t kButtonStableSamples = 10;      // ~10ms debounce at 1ms sampling, matching kButtonDebounceMs
constexpr uint8_t kButtonEdgeQueueCap = 16;       // bounded: at ~10ms/edge minimum spacing, holds >150ms of edges

struct ButtonEdgeRecord {
  bool pressed;     // true = accepted stable PRESS, false = accepted stable RELEASE
  uint32_t atMs;    // millis() at the moment this edge was accepted (debounce-completion time, not drain time)
};

portMUX_TYPE g_buttonMux = portMUX_INITIALIZER_UNLOCKED;
ButtonEdgeRecord g_dotEdgeQueue[kButtonEdgeQueueCap];  // contents only ever touched under g_buttonMux
uint8_t g_dotEdgeHead = 0;    // next slot to read; g_buttonMux-protected
uint8_t g_dotEdgeTail = 0;    // next slot to write; g_buttonMux-protected
uint8_t g_dotEdgeCount = 0;   // queued, undrained edges; g_buttonMux-protected
uint32_t g_dotEdgeDropped = 0;  // overflow count (see buttonSampleTimerCallback()); g_buttonMux-protected

// Stable-state sampler bookkeeping, touched ONLY from
// buttonSampleTimerCallback() (ESP_TIMER_TASK dispatch guarantees ordinary
// sequential, non-reentrant calls on esp_timer's own task) -- no locking
// needed, identical reasoning to the encoder's own sampler state.
bool g_dotRawStable = false;
bool g_dotCandidateState = false;
uint8_t g_dotCandidateCount = 0;

esp_timer_handle_t g_buttonSampleTimer = nullptr;
bool g_buttonSamplerFailed = false;  // set if esp_timer create/start fails; DOT/DASH simply produces no events in that case

// Periodic stable-state sampler for DOT/DASH: runs every
// kButtonSamplePeriodUs from its own ESP_TIMER_TASK callback, independent
// of Input::update()/the Arduino loop and independent of the encoder's
// timer. No Serial/Display/MQTT/NVS/LittleFS/heap/delay/pushEvent()/
// Sleep::notifyActivity()/application-callback calls here -- only a
// digitalRead(), the stability filter, and (on an accepted transition) a
// millis() timestamp read and a plain struct write into the bounded queue
// under the critical section. Input::update() (via drainButtonEdges())
// remains solely responsible for turning accepted edges into InputEvents
// and for Sleep::notifyActivity().
void buttonSampleTimerCallback(void* /*arg*/) {
  bool raw = digitalRead(Pins::kDotDash) == LOW;  // active LOW: pressed == LOW

  if (raw == g_dotRawStable) {
    // Back at the currently accepted level -- discard any in-progress
    // candidate.
    g_dotCandidateState = g_dotRawStable;
    g_dotCandidateCount = 0;
    return;
  }
  if (raw != g_dotCandidateState) {
    // A possible new level appeared, replacing any previous candidate.
    g_dotCandidateState = raw;
    g_dotCandidateCount = 1;
    return;
  }
  g_dotCandidateCount++;
  if (g_dotCandidateCount < kButtonStableSamples) return;  // not stable yet

  g_dotRawStable = g_dotCandidateState;
  g_dotCandidateCount = 0;
  uint32_t atMs = millis();  // a plain hardware-timer read, safe from ESP_TIMER_TASK context

  portENTER_CRITICAL(&g_buttonMux);
  if (g_dotEdgeCount < kButtonEdgeQueueCap) {
    g_dotEdgeQueue[g_dotEdgeTail] = ButtonEdgeRecord{g_dotRawStable, atMs};
    g_dotEdgeTail = static_cast<uint8_t>((g_dotEdgeTail + 1) % kButtonEdgeQueueCap);
    g_dotEdgeCount++;
  } else {
    // Bounded queue, deterministic overflow policy: the new edge is
    // dropped (not the oldest already-queued one) and counted. At 16
    // slots and a ~10ms minimum spacing between accepted edges, this would
    // require Input::update() to go unserviced for >150ms while DOT/DASH
    // is being actively pressed -- far longer than any single screen
    // redraw -- before a single edge could ever be lost this way.
    g_dotEdgeDropped++;
  }
  portEXIT_CRITICAL(&g_buttonMux);
}

// Drains accepted DOT/DASH edges in the order they were captured, each
// with its own real acceptance timestamp, and runs them through the exact
// same press-state-machine logic updateDotStateMachine() used to run
// polled every tick -- now driven by discrete events instead of a live
// level, so a complete press+release captured while this wasn't running
// still produces both events with an accurate duration.
void processDotEdge(bool pressed, uint32_t atMs);

void drainButtonEdges() {
  ButtonEdgeRecord local[kButtonEdgeQueueCap];
  uint8_t count;
  portENTER_CRITICAL(&g_buttonMux);
  count = g_dotEdgeCount;
  for (uint8_t i = 0; i < count; i++) {
    local[i] = g_dotEdgeQueue[(g_dotEdgeHead + i) % kButtonEdgeQueueCap];
  }
  g_dotEdgeHead = static_cast<uint8_t>((g_dotEdgeHead + count) % kButtonEdgeQueueCap);
  g_dotEdgeCount = 0;
  portEXIT_CRITICAL(&g_buttonMux);

  for (uint8_t i = 0; i < count; i++) {
    processDotEdge(local[i].pressed, local[i].atMs);
  }
}

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

// Replaces the old polled updateDotStateMachine(): driven by one accepted
// DOT/DASH edge (from drainButtonEdges(), Hardware Fix #4.7b) at a time,
// using that edge's own captured timestamp rather than the current
// millis() -- so duration/combined-gesture timing is computed from the
// real debounce-acceptance instant, not from whenever Input::update()
// happened to run. Behavior is otherwise identical to the polled version:
// a redundant same-level edge (pressed while already INDIVIDUAL, or a
// release outside INDIVIDUAL/COMBINED) is simply not observed here in the
// first place, since only genuine accepted transitions are ever queued.
void processDotEdge(bool pressed, uint32_t atMs) {
  if (pressed) {
    if (g_dotPressState == PressState::IDLE) {
      g_dotDownMs = atMs;
      Sleep::notifyActivity();
      if (g_encPressState == PressState::INDIVIDUAL && (atMs - g_encDownMs) < kCombinedWindowMs) {
        enterCombined(atMs);
      } else {
        g_dotPressState = PressState::INDIVIDUAL;
        pushEvent(InputEventType::DOT_PRESS_START);
      }
    }
    return;
  }

  if (g_dotPressState == PressState::INDIVIDUAL) {
    uint32_t duration = atMs - g_dotDownMs;
    pushEvent(InputEventType::DOT_RELEASE, 0, duration);
    g_dotPressState = PressState::IDLE;
    return;
  }

  if (g_combinedActive && g_dotPressState == PressState::COMBINED) {
    endCombined(atMs);
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

  // DOT/DASH reliable capture (Hardware Fix #4.7b) -- a fully independent
  // periodic esp_timer, separate from the encoder's above (its own
  // g_buttonSampleTimer, its own g_buttonMux, zero shared state), fixing
  // the same class of "missed while Input::update() wasn't running" bug
  // real-hardware testing found on DOT/DASH. See the comment block above
  // buttonSampleTimerCallback().
  bool initialDotRaw = digitalRead(Pins::kDotDash) == LOW;
  g_dotRawStable = initialDotRaw;
  g_dotCandidateState = initialDotRaw;
  g_dotCandidateCount = 0;

  portENTER_CRITICAL(&g_buttonMux);
  g_dotEdgeHead = 0;
  g_dotEdgeTail = 0;
  g_dotEdgeCount = 0;
  g_dotEdgeDropped = 0;
  portEXIT_CRITICAL(&g_buttonMux);

  // Guarded so calling init() more than once never creates a second
  // periodic timer, exactly like the encoder timer above.
  if (g_buttonSampleTimer == nullptr) {
    esp_timer_create_args_t buttonTimerArgs = {};
    buttonTimerArgs.callback = &buttonSampleTimerCallback;
    buttonTimerArgs.arg = nullptr;
    buttonTimerArgs.dispatch_method = ESP_TIMER_TASK;
    buttonTimerArgs.name = "btn_sample";
    esp_err_t buttonCreateErr = esp_timer_create(&buttonTimerArgs, &g_buttonSampleTimer);
    if (buttonCreateErr != ESP_OK) {
      g_buttonSampleTimer = nullptr;
      g_buttonSamplerFailed = true;
    } else {
      esp_err_t buttonStartErr = esp_timer_start_periodic(g_buttonSampleTimer, kButtonSamplePeriodUs);
      if (buttonStartErr != ESP_OK) {
        g_buttonSamplerFailed = true;
      }
    }
  }
}

void update() {
  uint32_t now = millis();

  updateDebounce(g_encSw, now);
  drainQuadrature();
  // Hardware Fix #4.7b: DOT/DASH is no longer polled here at all -- it is
  // continuously sampled and debounced by its own independent esp_timer
  // (buttonSampleTimerCallback()) regardless of how often update() runs;
  // drainButtonEdges() only replays whatever accepted press/release edges
  // have accumulated since the last call, in order, each with its own
  // real timestamp. This preserves the original relative ordering (DOT
  // edges processed before the encoder-switch state machine) so combined-
  // gesture tie-breaking behavior is unchanged.
  drainButtonEdges();

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
