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
// jumps to a non-adjacent or repeated state) yields 0. CW/CCW direction
// semantics (+1/-1) and kDetentTransitions (4 valid transitions per
// logical detent) are unchanged from before.
//
// kQuadratureTable is DRAM_ATTR (not left in flash-mapped .rodata) and
// the ISR itself is IRAM_ATTR, so both the code and the data it reads
// remain safely accessible even if the ISR fires while flash access is
// briefly unavailable (e.g. during an NVS commit) -- standard ESP32
// GPIO-ISR safety practice.
//
// Hardware Fix #4.5 (TEMPORARY validation): real-hardware raw diagnostic
// captures (see the Fix4.3a/#4.3 raw-transition ring, parent commit
// baeab293) proved the original free-running +/-4 accumulator had two
// bugs: (1) a genuine physical detent can finish with a sampled two-bit
// "skipped" transition straight back to REST (observed: 0->3, tableDir
// ==0) that the old decoder simply discarded, silently dropping a real
// click; (2) because the accumulator was only ever reset when IT ITSELF
// reached +/-4, that dropped click's partial count survived into the
// NEXT physical detent and corrupted it. The decoder below is now
// detent-bound instead of free-running: a candidate detent only exists
// between leaving the measured rest state (CLK=HIGH, DT=HIGH == state 3)
// and returning to it, and returning to rest -- by any means, including
// the confirmed skipped-transition case -- is an unconditional boundary
// that always resets the accumulator to 0, so a corrupted or partial
// detent can never bleed into the next one. This changes only how those
// 4 transitions are grouped into a detent; the table, the CW=+1/CCW=-1
// convention, and kDetentTransitions itself are unchanged.
//
// Hardware Fix #4.5b (TEMPORARY validation): a real-hardware run of
// Fix4.5a (20 physical CW detents, WPM 15->30) still only accepted 15/20,
// with the raw diagnostic showing the misses as two-bit INVALID DESYNC
// jumps (3->0, 2->1) rather than as the expected single-bit steps. The
// SAME ISR was previously attached to both CLK and DT and read BOTH pins
// on every firing to build the combined 2-bit state; if both physical
// edges of one intermediate quadrature step occurred before that shared
// ISR ran, it could observe the two pins' ALREADY-final combined level
// and collapse an intermediate state entirely (3->1->0 sampled as a
// direct 3->0). This is a raw CAPTURE change only, testing whether a
// separate per-pin ISR -- one for CLK, one for DT, each updating only
// its own bit of the cached state -- preserves the intermediate state
// the shared-ISR read was potentially losing. The Fix4.5a decoder logic
// itself (thresholds, peak tracking, recovery rule, reset rule) is
// unchanged; only how g_encIsrLastState's next value is computed differs.
// ---------------------------------------------------------------------------
DRAM_ATTR const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};
constexpr int8_t kDetentTransitions = 4;
constexpr uint8_t kEncoderRestState = 3;    // measured rest/detent state: CLK HIGH, DT HIGH
constexpr uint8_t kEncoderClkBit = 0x02;    // state = (CLK<<1)|DT -- CLK owns bit 1
constexpr uint8_t kEncoderDtBit = 0x01;     // DT owns bit 0
constexpr int8_t kRecoveryMinEvidence = 2;  // min final |subAccum| trusted to recover a skipped-transition return-to-rest
// Hardware Fix #4.5a (TEMPORARY): the final subAccum alone cannot tell a
// real missed detent (measured peak evidence +3, final +2 after bounce)
// apart from a partial-turn-and-abort that the ISR happened to observe
// as a skipped return (e.g. 3->1->0->3, which never actually reached the
// next detent and only ever accumulated +2). Requiring the CYCLE'S PEAK
// evidence to have reached kDetentTransitions-1 makes that distinction.
constexpr int8_t kRecoveryMinPeakEvidence = 3;

portMUX_TYPE g_encMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t g_encIsrLastState = 0;
volatile int8_t g_encIsrSubAccum = 0;        // directional evidence accumulated within the CURRENT cycle only
volatile int32_t g_encIsrPendingDetents = 0;  // net whole detents not yet drained by update()
volatile bool g_encSynced = false;        // true once REST has been observed at least once since boot
volatile bool g_encCycleActive = false;   // true while away from REST inside a candidate detent
volatile bool g_encCycleInvalid = false;  // true once this cycle has seen an unrelated away-from-rest invalid jump
volatile int8_t g_encCyclePeakPositive = 0;  // highest subAccum reached this cycle (>=0), reset at every REST boundary
volatile int8_t g_encCyclePeakNegative = 0;  // lowest subAccum reached this cycle (<=0), reset at every REST boundary

// ---------------------------------------------------------------------------
// TEMPORARY encoder raw-transition diagnostic (observation only).
//
// Records every raw quadrature transition the ISR observes into a small
// fixed ring buffer, purely so Input::update() (normal task context) can
// print it over Serial afterwards for hardware analysis. This is wired
// in PARALLEL to the production dir/sub-accum/pending-detent logic below
// -- it reads the same locals the production logic already computed
// after that logic has fully run, and does not alter, gate, delay, or
// feed back into any of it in any way. Remove this whole block (and its
// two call sites) once the mechanical encoder investigation is done.
//
// The ISR side only writes plain fields into a fixed array and bumps a
// count under the same critical section already used above -- no
// Serial/millis()/delay()/heap/Display/MQTT/NVS calls.
// ---------------------------------------------------------------------------
// Which pin's own ISR generated this record (Fix4.5b TEMPORARY): lets the
// drained log show whether the per-pin capture preserves physical edge
// order, without the decoder itself depending on this value in any way.
enum class EncoderDiagSource : uint8_t { CLK = 0, DT = 1 };

struct EncoderDiagRecord {
  uint8_t prevState;
  uint8_t currState;
  int8_t tableDir;
  int8_t subAccumAfter;
  int8_t wholeDetent;  // -1, 0, +1
  uint8_t flags;       // kDiagFlagRecovered / kDiagFlagDesync (Fix4.5 TEMPORARY)
  int8_t peakPositive;  // this cycle's peak subAccum (>=0) at the moment of this transition (Fix4.5a)
  int8_t peakNegative;  // this cycle's trough subAccum (<=0) at the moment of this transition (Fix4.5a)
  EncoderDiagSource source;  // which pin's ISR produced this record (Fix4.5b)
};

constexpr uint8_t kDiagFlagRecovered = 0x01;  // detent emitted via skipped-transition recovery (rule 4)
constexpr uint8_t kDiagFlagDesync = 0x02;     // invalid jump away from rest desynchronized the cycle (rule 5)

constexpr uint8_t kEncoderDiagCap = 128;
EncoderDiagRecord g_encDiagBuf[kEncoderDiagCap];  // contents only ever touched under g_encMux
volatile uint8_t g_encDiagHead = 0;               // next slot the ISR will write
volatile uint8_t g_encDiagCount = 0;              // unread records waiting to be drained
volatile uint32_t g_encDiagDropped = 0;           // records lost because the ring was full

// Shared ISR-safe decoder body (Fix4.5b TEMPORARY): computes the new
// cached state from the PREVIOUS cached state plus only the ONE bit the
// calling per-pin ISR observed changing, then runs the unmodified
// Fix4.5a detent-bound decoder on that (prev, curr) pair. Called only
// from onEncoderClkChangeIsr()/onEncoderDtChangeIsr(), so it must itself
// be IRAM_ATTR.
void IRAM_ATTR processEncoderPinSampleIsr(uint8_t bitMask, bool pinHigh, EncoderDiagSource source) {
  portENTER_CRITICAL_ISR(&g_encMux);
  uint8_t prevState = g_encIsrLastState;
  uint8_t curr = pinHigh ? static_cast<uint8_t>(prevState | bitMask) : static_cast<uint8_t>(prevState & ~bitMask);
  uint8_t idx = static_cast<uint8_t>((prevState << 2) | curr);
  g_encIsrLastState = curr;
  int8_t dir = kQuadratureTable[idx & 0x0F];
  int8_t wholeDetent = 0;
  uint8_t diagFlags = 0;
  int8_t diagPeakPos = 0;
  int8_t diagPeakNeg = 0;
  bool diagPeakCaptured = false;  // REST-arrival branch snapshots peaks BEFORE resetting them; all other branches read live values below

  if (prevState == curr) {
    // Rule 6 (REPEAT): both GPIO CHANGE interrupts can fire after the
    // combined pin state has already settled to the same value. Total
    // no-op -- must not touch subAccum, the peaks, the cycle state, or
    // emit.
  } else if (!g_encSynced) {
    // Boot resync: never fabricate a detent before REST has been
    // observed at least once since startup.
    if (curr == kEncoderRestState) {
      g_encSynced = true;
      g_encIsrSubAccum = 0;
      g_encCyclePeakPositive = 0;
      g_encCyclePeakNegative = 0;
      g_encCycleActive = false;
      g_encCycleInvalid = false;
    }
    // else: still unsynced: ignore this transition entirely and keep
    // waiting for REST.
  } else if (curr == kEncoderRestState) {
    // Rule 7: returning to REST is an absolute cycle boundary, whatever
    // path got us here (normal completion, recovered skipped-transition,
    // or an aborted/desynchronized cycle returning home).
    if (g_encCycleActive && !g_encCycleInvalid) {
      if (dir != 0) {
        // Rule 3: normal valid return to rest. Still tracks peaks (every
        // valid directional transition does), though peak evidence is
        // only ever CONSULTED for the skipped-transition recovery case
        // below, not for this normal-completion threshold.
        g_encIsrSubAccum = static_cast<int8_t>(g_encIsrSubAccum + dir);
        if (g_encIsrSubAccum > g_encCyclePeakPositive) g_encCyclePeakPositive = g_encIsrSubAccum;
        if (g_encIsrSubAccum < g_encCyclePeakNegative) g_encCyclePeakNegative = g_encIsrSubAccum;
        diagPeakPos = g_encCyclePeakPositive;
        diagPeakNeg = g_encCyclePeakNegative;
        diagPeakCaptured = true;
        if (g_encIsrSubAccum >= kDetentTransitions) {
          g_encIsrPendingDetents++;
          wholeDetent = 1;
        } else if (g_encIsrSubAccum <= -kDetentTransitions) {
          g_encIsrPendingDetents--;
          wholeDetent = -1;
        }
      } else {
        // Rule 4 (Fix4.5a): confirmed skipped-transition recovery (e.g.
        // 0->3). The final accumulator alone cannot tell a real missed
        // detent (measured peak +3, final +2 after bounce) apart from a
        // partial-turn-and-abort that only ever reached +2 -- requiring
        // the cycle's PEAK evidence to have reached kRecoveryMinPeak-
        // Evidence makes that distinction; a same-detent abort like
        // 3->1->0->3 peaks at +2 and is correctly rejected.
        diagPeakPos = g_encCyclePeakPositive;
        diagPeakNeg = g_encCyclePeakNegative;
        diagPeakCaptured = true;
        if (g_encIsrSubAccum >= kRecoveryMinEvidence && g_encCyclePeakPositive >= kRecoveryMinPeakEvidence) {
          g_encIsrPendingDetents++;
          wholeDetent = 1;
          diagFlags |= kDiagFlagRecovered;
        } else if (g_encIsrSubAccum <= -kRecoveryMinEvidence && g_encCyclePeakNegative <= -kRecoveryMinPeakEvidence) {
          g_encIsrPendingDetents--;
          wholeDetent = -1;
          diagFlags |= kDiagFlagRecovered;
        }
      }
    }
    // Unconditional reset at the rest boundary (rules 3/4/7): no stale
    // partial accumulation -- subAccum OR peaks -- may ever survive past
    // this point, whether or not a detent was just emitted.
    g_encIsrSubAccum = 0;
    g_encCyclePeakPositive = 0;
    g_encCyclePeakNegative = 0;
    g_encCycleActive = false;
    g_encCycleInvalid = false;
  } else if (dir != 0) {
    // Away from rest, valid Gray-code step.
    if (!g_encCycleActive) {
      if (prevState == kEncoderRestState) {
        // Rule 1: start a fresh candidate cycle. Never inherits a prior
        // cycle's accumulator or peaks -- both were already 0 from the
        // last rest-boundary reset, and are explicitly reset here too.
        g_encCycleActive = true;
        g_encCycleInvalid = false;
        g_encIsrSubAccum = dir;
        g_encCyclePeakPositive = 0;
        g_encCyclePeakNegative = 0;
        if (g_encIsrSubAccum > g_encCyclePeakPositive) g_encCyclePeakPositive = g_encIsrSubAccum;
        if (g_encIsrSubAccum < g_encCyclePeakNegative) g_encCyclePeakNegative = g_encIsrSubAccum;
      } else {
        // Defensive: a valid step observed while not already in a cycle
        // and not leaving from REST should not happen if the invariants
        // above hold, but if it ever does, don't guess -- desynchronize
        // rather than start a cycle from an unknown position.
        g_encCycleActive = true;
        g_encCycleInvalid = true;
        g_encIsrSubAccum = 0;
        g_encCyclePeakPositive = 0;
        g_encCyclePeakNegative = 0;
        diagFlags |= kDiagFlagDesync;
      }
    } else if (!g_encCycleInvalid) {
      // Rule 2: keep accumulating directional evidence for this cycle
      // and tracking its peak; mechanical bounce naturally cancels the
      // accumulator via its own opposite-signed contribution, but the
      // peak (a running max/min, never itself decremented by bounce)
      // remembers how far the cycle got before any bounce pulled it
      // back. Do NOT emit here even if this temporarily reaches
      // +/-kDetentTransitions -- only a return to REST finalizes a detent.
      g_encIsrSubAccum = static_cast<int8_t>(g_encIsrSubAccum + dir);
      if (g_encIsrSubAccum > g_encCyclePeakPositive) g_encCyclePeakPositive = g_encIsrSubAccum;
      if (g_encIsrSubAccum < g_encCyclePeakNegative) g_encCyclePeakNegative = g_encIsrSubAccum;
    }
    // else: this cycle is already desynchronized (rule 5) -- further
    // valid evidence does not recover it; ignore until REST is observed.
  } else {
    // Rule 5: invalid two-bit jump away from rest (prev != curr,
    // tableDir == 0, curr != REST). Never guess a direction here --
    // desynchronize the cycle and wait for REST before accepting a new
    // one. False positives are worse than dropping one corrupted detent.
    g_encCycleActive = true;
    g_encCycleInvalid = true;
    g_encIsrSubAccum = 0;
    g_encCyclePeakPositive = 0;
    g_encCyclePeakNegative = 0;
    diagFlags |= kDiagFlagDesync;
  }

  if (!diagPeakCaptured) {
    // No rest-boundary reset happened this call, so the live values
    // still reflect whatever this transition just did (or left alone).
    diagPeakPos = g_encCyclePeakPositive;
    diagPeakNeg = g_encCyclePeakNegative;
  }

  // Diagnostic capture only, appended after the production decision above
  // has already fully executed; it observes the outcome, it does not
  // participate in producing it.
  if (g_encDiagCount < kEncoderDiagCap) {
    uint8_t slot = g_encDiagHead;
    g_encDiagBuf[slot].prevState = prevState;
    g_encDiagBuf[slot].currState = curr;
    g_encDiagBuf[slot].tableDir = dir;
    g_encDiagBuf[slot].subAccumAfter = g_encIsrSubAccum;
    g_encDiagBuf[slot].wholeDetent = wholeDetent;
    g_encDiagBuf[slot].flags = diagFlags;
    g_encDiagBuf[slot].peakPositive = diagPeakPos;
    g_encDiagBuf[slot].peakNegative = diagPeakNeg;
    g_encDiagBuf[slot].source = source;
    g_encDiagHead = static_cast<uint8_t>((slot + 1) % kEncoderDiagCap);
    g_encDiagCount++;
  } else {
    g_encDiagDropped++;
  }
  portEXIT_CRITICAL_ISR(&g_encMux);
}

// Per-pin ISR entry points (Fix4.5b TEMPORARY): each reads ONLY its own
// GPIO -- the CLK ISR never reads DT, and the DT ISR never reads CLK --
// so the decoder-state bit each one owns is always taken from that pin's
// own most recent observation, never reconstructed by re-reading the
// other pin (which is what the previous shared-ISR architecture did and
// what the real-hardware evidence suggests could lose an intermediate
// quadrature state).
void IRAM_ATTR onEncoderClkChangeIsr() {
  bool clkHigh = digitalRead(Pins::kEncoderClk) == HIGH;
  processEncoderPinSampleIsr(kEncoderClkBit, clkHigh, EncoderDiagSource::CLK);
}

void IRAM_ATTR onEncoderDtChangeIsr() {
  bool dtHigh = digitalRead(Pins::kEncoderDt) == HIGH;
  processEncoderPinSampleIsr(kEncoderDtBit, dtHigh, EncoderDiagSource::DT);
}

// Drains and prints whatever raw transition records the ISR captured since
// the last call. Runs entirely in normal task context (called from
// Input::update()) -- the ring is snapshotted into a local buffer under
// the same critical section the ISR uses, then Serial is written only
// after the section is released, so the ISR is never blocked on Serial
// I/O. Observation only: never touches g_encIsrSubAccum/
// g_encIsrPendingDetents/g_encIsrLastState.
void drainEncoderDiagnostics() {
  EncoderDiagRecord localBuf[kEncoderDiagCap];
  uint8_t count;
  uint32_t dropped;

  portENTER_CRITICAL(&g_encMux);
  count = g_encDiagCount;
  uint8_t oldest = static_cast<uint8_t>((g_encDiagHead + kEncoderDiagCap - count) % kEncoderDiagCap);
  for (uint8_t i = 0; i < count; i++) {
    localBuf[i] = g_encDiagBuf[(oldest + i) % kEncoderDiagCap];
  }
  g_encDiagCount = 0;
  dropped = g_encDiagDropped;
  g_encDiagDropped = 0;
  portEXIT_CRITICAL(&g_encMux);

  for (uint8_t i = 0; i < count; i++) {
    const EncoderDiagRecord& r = localBuf[i];
    Serial.print(r.source == EncoderDiagSource::CLK ? "ENC[C] " : "ENC[D] ");
    Serial.print(r.prevState);
    Serial.print('>');
    Serial.print(r.currState);
    Serial.print(" dir=");
    if (r.tableDir > 0) Serial.print('+');
    Serial.print(r.tableDir);
    Serial.print(" sub=");
    if (r.subAccumAfter > 0) Serial.print('+');
    Serial.print(r.subAccumAfter);
    Serial.print(" peak+=");
    Serial.print(r.peakPositive);  // always >=0 by construction, no sign prefix needed
    Serial.print(" peak-=");
    Serial.print(r.peakNegative);  // always <=0 by construction, prints its own '-' when nonzero
    Serial.print(" step=");
    if (r.wholeDetent > 0) Serial.print('+');
    Serial.print(r.wholeDetent);
    if (r.prevState == r.currState) {
      Serial.print(" REPEAT");
    } else if (r.tableDir == 0) {
      Serial.print(" INVALID");
      if (r.flags & kDiagFlagRecovered) {
        Serial.print(" RECOVER");
      } else if (r.flags & kDiagFlagDesync) {
        Serial.print(" DESYNC");
      }
    }
    Serial.println();
  }

  if (dropped > 0) {
    Serial.print("ENC_DIAG_DROP=");
    Serial.println(dropped);
  }
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
  // Hardware Fix #4.5 (TEMPORARY): only trust the boot pin state as a
  // synchronized rest position if it actually IS rest; otherwise wait
  // for the ISR to observe REST for the first time before decoding any
  // cycle, rather than fabricating a detent from an unknown starting
  // position.
  g_encSynced = (g_encIsrLastState == kEncoderRestState);
  g_encCycleActive = false;
  g_encCycleInvalid = false;
  g_encCyclePeakPositive = 0;
  g_encCyclePeakNegative = 0;

  // Each pin gets its OWN ISR (Hardware Fix #4.5b TEMPORARY): CLK's ISR
  // updates only the CLK bit of the cached state, DT's ISR updates only
  // the DT bit, and neither re-reads the other pin -- see the per-pin
  // capture comment above onEncoderClkChangeIsr()/onEncoderDtChangeIsr().
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderClk), onEncoderClkChangeIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Pins::kEncoderDt), onEncoderDtChangeIsr, CHANGE);
}

void update() {
  uint32_t now = millis();

  updateDebounce(g_dot, now);
  updateDebounce(g_encSw, now);
  drainQuadrature();
  drainEncoderDiagnostics();  // TEMPORARY: raw quadrature diagnostic (observation only)

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
