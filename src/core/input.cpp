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
// Hardware Fix #4.5b (TEMPORARY validation, SUPERSEDED by Fix4.6 below):
// a real-hardware run of Fix4.5a (20 physical CW detents, WPM 15->30)
// still only accepted 15/20, with the raw diagnostic showing the misses
// as two-bit INVALID DESYNC jumps (3->0, 2->1) rather than as the
// expected single-bit steps. The SAME ISR was previously attached to
// both CLK and DT and read BOTH pins on every firing to build the
// combined 2-bit state; if both physical edges of one intermediate
// quadrature step occurred before that shared ISR ran, it could observe
// the two pins' ALREADY-final combined level and collapse an
// intermediate state entirely (3->1->0 sampled as a direct 3->0). Fix4.5b
// tried a separate per-pin ISR -- one for CLK, one for DT, each updating
// only its own bit of the cached state -- to preserve that intermediate
// state. On real hardware this DID remove most of the two-bit collapses,
// but it now followed heavy mechanical contact bounce far too literally
// (rapid 3<->1<->3<->1 oscillation, huge REPEAT counts) and the accepted
// rate got WORSE (15/20 -> 11/20, WPM 15->26). Edge interrupts of any
// granularity -- shared or per-pin -- follow whatever the contact is
// doing at the instant it fires, with no way to tell a real settling
// transition from bounce-in-progress. That per-pin ISR architecture
// (onEncoderClkChangeIsr/onEncoderDtChangeIsr/processEncoderPinSampleIsr/
// EncoderDiagSource) is fully removed in Fix4.6 below.
//
// Hardware Fix #4.6 (TEMPORARY validation): replaces ALL encoder GPIO
// edge interrupts with a periodic STABLE-STATE sampler, independent of
// both interrupt timing and the Arduino loop/UI redraw. A dedicated
// esp_timer (ESP_TIMER_TASK dispatch, so it runs as a normal FreeRTOS
// task callback, not inside interrupt context) samples both pins every
// kEncoderSamplePeriodUs and only PRESENTS a new raw state to the
// unchanged Fix4.5a detent-bound decoder once that state has been
// observed for kEncoderStableSamples consecutive samples in a row --
// see encoderSampleTimerCallback()/processEncoderStableTransition()
// below. This directly targets what Fix4.5b's real-hardware run
// exposed: edge interrupts have no concept of "settled," only "changed
// just now"; a fixed 1ms/2-sample stability window waits out contact
// bounce structurally, the same way Fix4.3's transition table structurally
// rejects it, without adding any new edge-triggered heuristics. The
// decoder itself (kEncoderRestState, kDetentTransitions, peak tracking,
// recovery thresholds, DESYNC handling, the unconditional REST-boundary
// reset, CW=+1/CCW=-1) is completely unchanged from Fix4.5a; only how
// (and how often) it is FED a (prevState, currState) pair differs.
// ---------------------------------------------------------------------------
DRAM_ATTR const int8_t kQuadratureTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};
constexpr int8_t kDetentTransitions = 4;
constexpr uint8_t kEncoderRestState = 3;    // measured rest/detent state: CLK HIGH, DT HIGH
constexpr int8_t kRecoveryMinEvidence = 2;  // min final |subAccum| trusted to recover a skipped-transition return-to-rest
// Hardware Fix #4.5a (TEMPORARY): the final subAccum alone cannot tell a
// real missed detent (measured peak evidence +3, final +2 after bounce)
// apart from a partial-turn-and-abort that the ISR happened to observe
// as a skipped return (e.g. 3->1->0->3, which never actually reached the
// next detent and only ever accumulated +2). Requiring the CYCLE'S PEAK
// evidence to have reached kDetentTransitions-1 makes that distinction.
constexpr int8_t kRecoveryMinPeakEvidence = 3;

// Hardware Fix #4.6 (TEMPORARY): periodic stable-state sampler tunables.
// Do NOT change these during this validation task -- see the Fix4.6
// comment block above.
constexpr uint32_t kEncoderSamplePeriodUs = 1000;  // 1ms sample period
constexpr uint8_t kEncoderStableSamples = 2;       // consecutive identical non-stable samples required to accept

portMUX_TYPE g_encMux = portMUX_INITIALIZER_UNLOCKED;
volatile int8_t g_encIsrSubAccum = 0;        // directional evidence accumulated within the CURRENT cycle only
volatile int32_t g_encIsrPendingDetents = 0;  // net whole detents not yet drained by update()
volatile bool g_encSynced = false;        // true once REST has been observed at least once since boot
volatile bool g_encCycleActive = false;   // true while away from REST inside a candidate detent
volatile bool g_encCycleInvalid = false;  // true once this cycle has seen an unrelated away-from-rest invalid jump
volatile int8_t g_encCyclePeakPositive = 0;  // highest subAccum reached this cycle (>=0), reset at every REST boundary
volatile int8_t g_encCyclePeakNegative = 0;  // lowest subAccum reached this cycle (<=0), reset at every REST boundary

// Hardware Fix #4.6 (TEMPORARY): periodic stable-state sampler state.
// Touched ONLY from encoderSampleTimerCallback(), which the ESP_TIMER_TASK
// dispatch mode guarantees runs as ordinary sequential (non-reentrant,
// non-overlapping) calls on esp_timer's own task -- no other code ever
// reads or writes these, so unlike the decoder state below they need
// neither `volatile` nor g_encMux protection.
uint8_t g_encStableState = 0;     // last ACCEPTED stable (CLK<<1|DT) state -- what the decoder is fed as prevStable
uint8_t g_encCandidateState = 0;  // a raw state currently being evaluated for stability
uint8_t g_encCandidateCount = 0;  // consecutive samples candidateState has been observed (not yet accepted)
uint8_t g_encNoiseCount = 0;      // candidate starts rejected since the last accepted stable transition (diagnostic only; saturates, never wraps)

esp_timer_handle_t g_encoderSampleTimer = nullptr;
volatile bool g_encoderSamplerInitFailed = false;  // set on create/start failure; drained+printed once from task context

// ---------------------------------------------------------------------------
// TEMPORARY encoder raw-transition diagnostic (observation only).
//
// Records each ACCEPTED STABLE transition (Fix4.6) -- never a raw 1ms
// sample -- into a small fixed ring buffer, purely so Input::update()
// (normal task context) can print it over Serial afterwards for hardware
// analysis. This is wired in PARALLEL to the production decoder logic
// below -- it reads the same locals the production logic already
// computed after that logic has fully run, and does not alter, gate,
// delay, or feed back into any of it in any way. Remove this whole block
// (and its call sites) once the mechanical encoder investigation is done.
//
// Fix4.6 removed the Fix4.5b per-pin CLK/DT `EncoderDiagSource` field --
// every record now originates from the single stable-state sampler, so a
// source label is no longer meaningful -- and replaced it with a `noise`
// count (see below), keeping the record at the same 9 bytes.
// ---------------------------------------------------------------------------
struct EncoderDiagRecord {
  uint8_t prevState;
  uint8_t currState;
  int8_t tableDir;
  int8_t subAccumAfter;
  int8_t wholeDetent;  // -1, 0, +1
  uint8_t flags;       // kDiagFlagRecovered / kDiagFlagDesync (Fix4.5 TEMPORARY)
  int8_t peakPositive;  // this cycle's peak subAccum (>=0) at the moment of this transition (Fix4.5a)
  int8_t peakNegative;  // this cycle's trough subAccum (<=0) at the moment of this transition (Fix4.5a)
  uint8_t noise;  // rejected candidate starts since the previous accepted stable transition (Fix4.6, saturates at 255)
};

constexpr uint8_t kDiagFlagRecovered = 0x01;  // detent emitted via skipped-transition recovery (rule 4)
constexpr uint8_t kDiagFlagDesync = 0x02;     // invalid jump away from rest desynchronized the cycle (rule 5)

constexpr uint8_t kEncoderDiagCap = 128;
EncoderDiagRecord g_encDiagBuf[kEncoderDiagCap];  // contents only ever touched under g_encMux
volatile uint8_t g_encDiagHead = 0;               // next slot the ISR will write
volatile uint8_t g_encDiagCount = 0;              // unread records waiting to be drained
volatile uint32_t g_encDiagDropped = 0;           // records lost because the ring was full

// Fix4.5a detent-bound decoder, now expressed as a helper that receives
// an already-ACCEPTED STABLE (prevStable, currStable) transition from
// encoderSampleTimerCallback() below, rather than reading GPIO or
// tracking its own "last state" itself -- every call is exactly one
// accepted stable transition, so the decoder logic itself is otherwise
// BYTE-FOR-BYTE IDENTICAL to Fix4.5a's. Runs in the esp_timer
// ESP_TIMER_TASK context (an ordinary FreeRTOS task, not interrupt
// context), so it uses the normal TASK-context critical section
// (portENTER_CRITICAL/portEXIT_CRITICAL), never the _ISR variants --
// Input::update()'s drainQuadrature()/drainEncoderDiagnostics() already
// used the same task-context macros, so this is consistent cross-task
// locking between the esp_timer task and the Arduino loop task.
void processEncoderStableTransition(uint8_t prevStable, uint8_t currStable, uint8_t noise) {
  portENTER_CRITICAL(&g_encMux);
  uint8_t idx = static_cast<uint8_t>((prevStable << 2) | currStable);
  int8_t dir = kQuadratureTable[idx & 0x0F];
  int8_t wholeDetent = 0;
  uint8_t diagFlags = 0;
  int8_t diagPeakPos = 0;
  int8_t diagPeakNeg = 0;
  bool diagPeakCaptured = false;  // REST-arrival branch snapshots peaks BEFORE resetting them; all other branches read live values below

  if (prevStable == currStable) {
    // Rule 6 (REPEAT): the stable-state sampler only ever calls this once
    // a genuinely NEW state has been accepted, so this should not fire in
    // practice -- kept as defense-in-depth, matching Fix4.5a's original
    // REPEAT handling exactly. Total no-op -- must not touch subAccum,
    // the peaks, the cycle state, or emit.
  } else if (!g_encSynced) {
    // Boot resync: never fabricate a detent before REST has been
    // observed at least once since startup.
    if (currStable == kEncoderRestState) {
      g_encSynced = true;
      g_encIsrSubAccum = 0;
      g_encCyclePeakPositive = 0;
      g_encCyclePeakNegative = 0;
      g_encCycleActive = false;
      g_encCycleInvalid = false;
    }
    // else: still unsynced: ignore this transition entirely and keep
    // waiting for REST.
  } else if (currStable == kEncoderRestState) {
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
      if (prevStable == kEncoderRestState) {
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
    g_encDiagBuf[slot].prevState = prevStable;
    g_encDiagBuf[slot].currState = currStable;
    g_encDiagBuf[slot].tableDir = dir;
    g_encDiagBuf[slot].subAccumAfter = g_encIsrSubAccum;
    g_encDiagBuf[slot].wholeDetent = wholeDetent;
    g_encDiagBuf[slot].flags = diagFlags;
    g_encDiagBuf[slot].peakPositive = diagPeakPos;
    g_encDiagBuf[slot].peakNegative = diagPeakNeg;
    g_encDiagBuf[slot].noise = noise;
    g_encDiagHead = static_cast<uint8_t>((slot + 1) % kEncoderDiagCap);
    g_encDiagCount++;
  } else {
    g_encDiagDropped++;
  }
  portEXIT_CRITICAL(&g_encMux);
}

// Periodic stable-state sampler (Fix4.6 TEMPORARY): runs every
// kEncoderSamplePeriodUs from the esp_timer ESP_TIMER_TASK, independent
// of Input::update()/the Arduino loop, replacing ALL encoder GPIO edge
// interrupts. Reads both pins once, then applies the stability filter
// (see the STABILITY ALGORITHM cases below) before ever presenting a
// transition to the decoder -- a raw sample on its own never reaches
// processEncoderStableTransition(). No Serial/Display/MQTT/NVS/LittleFS/
// heap/delay/pushEvent()/Sleep::notifyActivity() calls here; those stay
// task-context responsibilities in Input::update().
void encoderSampleTimerCallback(void* /*arg*/) {
  uint8_t clk = digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0;
  uint8_t dt = digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0;
  uint8_t rawState = static_cast<uint8_t>((clk << 1) | dt);

  if (rawState == g_encStableState) {
    // CASE A: back at the currently accepted state -- whatever candidate
    // was forming is discarded; do NOT call the decoder.
    g_encCandidateState = g_encStableState;
    g_encCandidateCount = 0;
    return;
  }

  if (rawState != g_encCandidateState) {
    // CASE B: a possible new state appeared, replacing whatever candidate
    // (if any) was previously forming -- that previous candidate never
    // reached kEncoderStableSamples, so count it as rejected/noise. Not
    // accepted yet.
    g_encCandidateState = rawState;
    g_encCandidateCount = 1;
    if (g_encNoiseCount < 255) g_encNoiseCount++;  // saturate, never wrap
    return;
  }

  // CASE C: rawState == candidateState != stableState.
  g_encCandidateCount++;
  if (g_encCandidateCount < kEncoderStableSamples) return;  // not stable yet -- two consecutive samples required

  uint8_t prevStable = g_encStableState;
  uint8_t newStable = g_encCandidateState;
  g_encStableState = newStable;
  g_encCandidateCount = 0;

  uint8_t noise = g_encNoiseCount;
  g_encNoiseCount = 0;  // reset the per-transition noise counter once a stable state is accepted

  processEncoderStableTransition(prevStable, newStable, noise);
}

// Drains and prints whatever accepted-stable-transition records were
// captured since the last call. Runs entirely in normal task context
// (called from Input::update()) -- the ring is snapshotted into a local
// buffer under the same critical section processEncoderStableTransition()
// uses, then Serial is written only after the section is released, so
// the esp_timer task is never blocked on Serial I/O. Observation only:
// never touches g_encIsrSubAccum/g_encIsrPendingDetents/g_encStableState.
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
    Serial.print("ENC[S] ");  // every record now comes from the single stable-state sampler (Fix4.6)
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
    Serial.print(" noise=");
    Serial.print(r.noise);
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

  uint8_t initialRaw = static_cast<uint8_t>((digitalRead(Pins::kEncoderClk) == HIGH ? 1 : 0) << 1 |
                                             (digitalRead(Pins::kEncoderDt) == HIGH ? 1 : 0));
  g_encStableState = initialRaw;
  g_encCandidateState = initialRaw;
  g_encCandidateCount = 0;
  g_encNoiseCount = 0;

  g_encIsrSubAccum = 0;
  g_encIsrPendingDetents = 0;
  // Hardware Fix #4.5 (TEMPORARY): only trust the boot pin state as a
  // synchronized rest position if it actually IS rest; otherwise wait
  // for the sampler to accept REST for the first time before decoding
  // any cycle, rather than fabricating a detent from an unknown starting
  // position.
  g_encSynced = (g_encStableState == kEncoderRestState);
  g_encCycleActive = false;
  g_encCycleInvalid = false;
  g_encCyclePeakPositive = 0;
  g_encCyclePeakNegative = 0;

  // Hardware Fix #4.6 (TEMPORARY): NO GPIO edge interrupts for CLK/DT --
  // see the Fix4.6 comment block above encoderSampleTimerCallback(). A
  // periodic esp_timer replaces them entirely. Guarded so calling init()
  // more than once never creates a second periodic timer (no repeated
  // heap churn); if one already exists it is left running as-is.
  if (g_encoderSampleTimer == nullptr) {
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &encoderSampleTimerCallback;
    timerArgs.arg = nullptr;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "enc_sample";
    esp_err_t createErr = esp_timer_create(&timerArgs, &g_encoderSampleTimer);
    if (createErr != ESP_OK) {
      g_encoderSampleTimer = nullptr;
      g_encoderSamplerInitFailed = true;
    } else {
      esp_err_t startErr = esp_timer_start_periodic(g_encoderSampleTimer, kEncoderSamplePeriodUs);
      if (startErr != ESP_OK) {
        g_encoderSamplerInitFailed = true;
      }
    }
  }
}

void update() {
  uint32_t now = millis();

  updateDebounce(g_dot, now);
  updateDebounce(g_encSw, now);
  drainQuadrature();
  drainEncoderDiagnostics();  // TEMPORARY: accepted-stable-transition diagnostic (observation only)
  if (g_encoderSamplerInitFailed) {
    // TEMPORARY (Fix4.6): report exactly once, from normal task context,
    // never from init() itself and never repeatedly -- the encoder simply
    // will not produce ENCODER_ROTATE events if this fires, since no
    // sampler is running to feed the decoder.
    g_encoderSamplerInitFailed = false;
    Serial.println("ENC_SAMPLER_INIT_FAILED");
  }

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
