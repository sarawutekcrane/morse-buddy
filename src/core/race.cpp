#include "core/race.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <new>

#include "core/display.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/mqtt_manager.h"
#include "core/notifications.h"
#include "core/number_guessing.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/race_protocol.h"
#include "core/radio_audio.h"
#include "core/radio_transport.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"
#include "core/wifi_manager.h"

namespace {

using NumberGuessing::DigitEntryState;

constexpr size_t kDeviceIdLen = PacketCodec::kSenderDeviceIdLen;  // 16
constexpr size_t kIdLen = PacketCodec::kMessageIdLen;             // 24

// ---- forward declarations ---------------------------------------------------
void screenNoFamilyGroups();
void screenGroupSelect();
void screenRaceRoom();
void screenRaceGuess();
void enterGuessScreen();
void stopRoomPtt();
void invalidateGuessOutcome();
bool isRoundActive();
void setGuessNotice(const char* text);


// =============================================================================
// Room state (design r8): the room itself -- membership, rounds, scores, the
// start right -- lives in the RaceProtocol engine (race_protocol.h), which
// this file drives and renders. Nothing here decides room state.
// =============================================================================
// Allocated once, at service init, for the device's lifetime (not static:
// static DRAM has no room for it). nullptr if that allocation failed: Race is
// then unavailable for this boot, every path below checks for it, and the
// saved Race records are neither read nor written.
RaceProtocol::Engine* g_engine = nullptr;
char g_groupCode[33] = {0};  // the group whose Room is being viewed
// Key of the round the Guess screen, its history and its outcome belong to
// ("<C>/<nonce>/<roundStart>"; empty = no live round), and that round's
// secret, kept after it ends for the "Solved by X: 1234" outcome.
constexpr size_t kRoundKeyLen = 40;
char g_roundId[kRoundKeyLen] = {0};
uint8_t g_secret[4] = {0};
uint32_t g_roundStart = 0;
bool g_isRoomScreenActive = false;
bool g_pendingAutoEnterGuess = false;


// Fix Phase 1C (round 4): transient status shown on the Room screen's
// action-label row when a user's Invite/Start Round press is refused for
// not being Ready yet -- mirrors screenRaceGuess()'s g_guessResultText/
// g_guessResultUntilMs timed-display convention below. Reset implicitly by
// its own expiry (screenRaceRoom() clears it once millis() passes
// g_roomActionStatusUntilMs); a fresh refusal simply overwrites both
// fields, no separate reset-on-entry is needed since NO_LOBBY/LOBBY_WAITING
// re-enter this same computed label every tick regardless.
const char* g_roomActionStatusText = nullptr;
uint32_t g_roomActionStatusUntilMs = 0;

// Fix Phase 1C (round 4 correction): clears the refusal status from every
// place that can make it stale -- a fresh Ready-approved action about to
// proceed (so a retry within the notice window doesn't keep showing the
// old refusal once it's no longer true), and leaving/resetting the room or
// switching which room/group is being viewed (so a refusal from one room
// can never bleed into another). A bare `g_roomActionStatusText = nullptr`
// inline at each call site would work the same way but this makes the
// intent explicit at each of those otherwise-unrelated call sites.
void clearRoomActionStatus() {
  g_roomActionStatusText = nullptr;
  g_roomActionStatusUntilMs = 0;
}

// Phase 2A: rollover-safe "deadline not yet reached" (same signed-difference
// technique as the Room action status check in screenRaceRoom()).
bool beforeDeadline(uint32_t nowMs, uint32_t untilMs) { return static_cast<int32_t>(nowMs - untilMs) < 0; }

// =============================================================================
// Phase 2C-1: replayed-Short protection across Race actions and screens.
//
// Race actions block synchronously (QoS1 publishes, Identity::nextId()'s
// NVS write, and -- with Mute-outside-Radio on -- presence republishes on
// Guess/Room entry). Encoder Shorts pressed meanwhile are captured by
// input.cpp's sampler and replayed later, carrying their RELEASE time as
// eventMs. Without protection a replayed Short turns Invite into Start
// Round (C1), a local solve into the next round (C2), Start Round into
// Continue, a Continue into a second Guess push, and Continue/automatic
// entry into Guess's "Exit this game?" prompt (H3).
//
// One module-wide window, shared by the Room and Guess screens (so it
// survives the Room <-> Guess transitions it exists to cover -- there is
// deliberately NO reset on screen entry):
//  - Opened by an accepted Room action (Invite / Start Round / Join /
//    Continue) or a solving Guess Short, with start = that Short's own
//    eventMs; or by automatic entry into Guess, with start = the eventMs
//    of the last Short a Race screen popped (encoder FIFO: every Short
//    popped later is >= it), or, if none was ever popped, a half-range
//    interval ending at end.
//  - OPEN until the end of the tick that opened it: every Short popped
//    meanwhile is dropped (it is queued behind the opening one).
//  - BOUNDED once g_raceShortWindowEndMs is recorded at that tick's end
//    (by the screenRaceRoom()/screenRaceGuess() wrappers, whichever screen
//    the tick ended on): a Short is dropped iff its release time lies in
//    [start, end] (unsigned modular interval: rollover, zero and equal
//    timestamps need no special case).
//  - CLOSED on the first popped Short released after end (processed
//    normally), on leaving Race (the Room stops handling that batch as soon
//    as it leaves), on group change, and on Race entry. Not on
//    a completed drain or elapsed time: encSwSampleTimerCallback() stamps
//    atMs before enqueueing, so a stale release can become visible late;
//    the guarantee used is that ENCODER_SHORT comes only from that single
//    serialized producer through FIFO queues, so popped Shorts arrive in
//    non-decreasing release order (see text_message.cpp's SendWindow
//    comment for the full audit).
//  - Deliberately NOT closed by a room-state-driven change (a round ending,
//    the room closing): the window only drops Shorts released before its
//    end, so it cannot swallow any press made in the new context, while
//    clearing it would let a stale Short trigger Invite in the changed room.
// Only ENCODER_SHORT is affected; Back, rotation and DOT/Morse input are
// untouched. Residual (as in 2B): a genuinely later press ~49.7 days after
// start can alias into the interval; that one press is dropped.
// =============================================================================
enum class RaceShortWindow : uint8_t { CLOSED, OPEN, BOUNDED };
RaceShortWindow g_raceShortWindow = RaceShortWindow::CLOSED;
uint32_t g_raceShortWindowStartMs = 0;
uint32_t g_raceShortWindowEndMs = 0;
bool g_raceShortWindowStartKnown = false;
bool g_raceShortWindowOpenedThisTick = false;
uint32_t g_raceShortWatermarkMs = 0;  // eventMs of the last Short a Race screen popped
bool g_raceShortWatermarkValid = false;

void openRaceShortWindow(uint32_t startMs) {
  g_raceShortWindow = RaceShortWindow::OPEN;
  g_raceShortWindowStartMs = startMs;
  g_raceShortWindowStartKnown = true;
  g_raceShortWindowOpenedThisTick = true;
}

// Automatic entry has no accepted Short of its own: bound it below by the
// last Short a Race screen popped (encoder FIFO ordering), if any.
void openRaceShortWindowFromWatermark() {
  openRaceShortWindow(g_raceShortWatermarkMs);
  g_raceShortWindowStartKnown = g_raceShortWatermarkValid;
}

void closeRaceShortWindow() {
  g_raceShortWindow = RaceShortWindow::CLOSED;
  g_raceShortWindowOpenedThisTick = false;
}

// Called by both screen wrappers after their tick body.
void finishRaceScreenTick() {
  if (g_raceShortWindowOpenedThisTick && g_raceShortWindow == RaceShortWindow::OPEN) {
    g_raceShortWindowEndMs = millis();
    if (!g_raceShortWindowStartKnown) g_raceShortWindowStartMs = g_raceShortWindowEndMs - 0x7FFFFFFFu;
    g_raceShortWindow = RaceShortWindow::BOUNDED;
  }
  g_raceShortWindowOpenedThisTick = false;
}

// True if `e` is a Short replayed from the current window (drop it).
// Retires the window on the first Short released after its end.
bool isReplayedRaceShort(const InputEvent& e) {
  if (e.type != InputEventType::ENCODER_SHORT) return false;
  g_raceShortWatermarkMs = e.eventMs;
  g_raceShortWatermarkValid = true;
  if (g_raceShortWindow == RaceShortWindow::OPEN) return true;
  if (g_raceShortWindow != RaceShortWindow::BOUNDED) return false;
  if (static_cast<uint32_t>(e.eventMs - g_raceShortWindowStartMs) <=
      static_cast<uint32_t>(g_raceShortWindowEndMs - g_raceShortWindowStartMs)) {
    return true;
  }
  g_raceShortWindow = RaceShortWindow::CLOSED;
  return false;
}

// Phase 2A: Race guess history -- Solo-style "1 2 3 4   2A2B" rows for THIS
// device's own submitted guesses in the current round. Local RAM only
// (never persisted, never published), bounded: once full the oldest entry
// is dropped, which only ever affects rows already scrolled off screen
// (the Guess screen shows at most ~5 at PRIMARY line height). Bound to the
// exact round it belongs to (RoundBinding below), so a stale result can
// never be shown against a different room's or round's secret. It survives
// the view of that round briefly disappearing (Leave and rejoin, joining,
// Reconnecting) within this boot, and is cleared once the room shows any
// other round or none, on a local solve, and on a group change.
struct RaceGuessEntry {
  uint16_t guessValue;
  uint8_t a;
  uint8_t b;
};
constexpr uint8_t kRaceHistoryCap = 8;
RaceGuessEntry g_raceHistory[kRaceHistoryCap];  // oldest first
uint8_t g_raceHistoryCount = 0;
// The room's full identity (group, coordinator, n, nonce) plus the round's
// start version; valid == false: the history belongs to no round.
struct RoundBinding {
  bool valid;
  char group[RaceProtocol::kGroupLen];
  char coordinator[RaceProtocol::kIdChars + 1];
  uint32_t n, nonce, roundStart;
};
RoundBinding g_raceHistoryRound = {};
// Bumped on every append/clear so the Guess screen can tell "history
// changed" apart from a plain digit-entry change (Solo uses historyCount
// for this, which stops changing once this bounded buffer is full).
uint16_t g_raceHistoryVersion = 0;

// Same-screen history browsing over this round's retained guesses (the
// kRaceHistoryCap most recent; older ones were dropped and are not
// browsable). Any history reset ends browsing.
NumberGuessing::HistoryBrowseState g_raceBrowse;

void clearRaceGuessHistory() {
  g_raceHistoryCount = 0;
  g_raceHistoryRound.valid = false;
  g_raceHistoryVersion++;
  NumberGuessing::resetHistoryBrowse(&g_raceBrowse);
}

void appendRaceGuess(const uint8_t digits[NumberGuessing::kSecretDigits], NumberGuessing::GuessResult r) {
  if (g_raceHistoryCount == kRaceHistoryCap) {
    memmove(&g_raceHistory[0], &g_raceHistory[1], sizeof(g_raceHistory[0]) * (kRaceHistoryCap - 1));
    g_raceHistoryCount--;
  }
  RaceGuessEntry& e = g_raceHistory[g_raceHistoryCount++];
  e.guessValue = static_cast<uint16_t>(digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3]);
  e.a = r.a;
  e.b = r.b;
  g_raceHistoryVersion++;
}

// Phase 2A (D1): local-only "Solved! 4A0B" acknowledgement shown on the
// Room's status row after this device's own correct guess. It states the
// local evaluation result only -- the confirmed result arrives with the
// room state (a member's claim may still lose to an earlier one).
uint32_t g_roomSolvedNoticeUntilMs = 0;
bool g_roomSolvedNoticeActive = false;

void clearRoomSolvedNotice() {
  g_roomSolvedNoticeActive = false;
  g_roomSolvedNoticeUntilMs = 0;
}


// Rows below the Room's status row, rebuilt from the engine every tick:
// the room's ACTIVE participants (with scores) and a "Leave room" row while
// in a room; otherwise the rooms discovered in this group (Short = Join)
// followed by the last room's final scores, if any.
enum class RoomRowKind : uint8_t { PARTICIPANT, DISCOVERED, LEAVE, FINAL };
constexpr uint8_t kMaxParticipants = RaceProtocol::kMaxEntries + RaceProtocol::kMaxDiscovered + 1;
char g_participants[kMaxParticipants][kDeviceIdLen];  // row ids (device / coordinator id, "#leave")
RoomRowKind g_rowKind[kMaxParticipants];
uint16_t g_rowScore[kMaxParticipants];
uint8_t g_participantCount = 0;
const char* const kLeaveRowId = "#leave";

// Room participant list focus/scroll. Rotation moves the focus from the
// action row (g_roomFocusId empty) down through the rows; the focused row
// is tracked by its id so it stays put when others join, and is clamped to
// a neighbor if it is no longer listed. A Short on a participant only
// returns the focus to the action row; discovered-room and Leave rows act.
char g_roomFocusId[kDeviceIdLen] = {0};
uint8_t g_roomFocusIndex = 0;  // last resolved index of g_roomFocusId (clamp target)
uint8_t g_roomListTop = 0;     // first participant shown

void resetRoomFocus() {
  g_roomFocusId[0] = '\0';
  g_roomFocusIndex = 0;
  g_roomListTop = 0;
}


// ---- Availability helper (Phase 4 section 10) ------------------------------
void setRoomAvailability(bool available) {
  if (!Settings::getMuteRadioOutsideRadio()) return;  // Mute Off: availability just tracks ONLINE
  Presence::setOwnRadioAvailable(available);
  Presence::republishOwnPresenceAllGroups();
}

// ---- Heap diagnostics (serial only; hardware acceptance plan section 8) -----
// One compact line per reading of the byte-addressable internal heap (the
// capability the engine's new draws from on this board): free bytes now, the
// lowest free bytes since boot, and the largest free block. Formatted into a
// stack buffer (no allocation). Taken at engine allocation, on entering the
// Room, and otherwise at most once per kHeapLogPeriodMs from the service tick;
// never per loop or packet. Nothing about rooms, guesses or messages is logged.
constexpr uint32_t kHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kHeapLogPeriodMs = 60000;
constexpr uint32_t kHeapLogMinGapMs = 5000;  // Room entries closer than this to the last reading are not logged
uint32_t g_heapLogLastMs = 0;

void logRaceHeap(const char* event) {
  char line[160];
  uint32_t now = millis();
  snprintf(line, sizeof(line), "[RACE][HEAP] %s uptime_s=%lu free=%lu min_since_boot=%lu largest_block=%lu (bytes, internal 8-bit)",
           event, static_cast<unsigned long>(now / 1000),
           static_cast<unsigned long>(heap_caps_get_free_size(kHeapCaps)),
           static_cast<unsigned long>(heap_caps_get_minimum_free_size(kHeapCaps)),
           static_cast<unsigned long>(heap_caps_get_largest_free_block(kHeapCaps)));
  Serial.println(line);
  g_heapLogLastMs = now;
}

// =============================================================================
// Engine platform: MQTT (PacketCodec header + publishBinary, QoS 1), the
// mb_race NVS namespace, esp_random(), Identity's persistent counter.
// =============================================================================
bool platformPublish(void*, const char* group, const char* suffix, uint8_t kind, const uint8_t* payload, uint16_t len,
                     bool retained) {
  uint8_t wire[PacketCodec::kHeaderSize + RaceProtocol::kStateMaxBytes];  // 6 + 538
  if (len > RaceProtocol::kStateMaxBytes) return false;
  if (!PacketCodec::encodeHeader(kind, len, wire, sizeof(wire))) return false;
  memcpy(wire + PacketCodec::kHeaderSize, payload, len);
  return MqttManager::publishBinary(group, suffix, wire, static_cast<uint16_t>(PacketCodec::kHeaderSize + len),
                                    retained, 1);
}
bool platformGroupReady(void*, const char* group) { return MqttManager::isGroupReady(group); }
bool platformNvsWrite(void*, const char* key, const void* data, size_t len) {
  return Storage::race().putBytes(key, data, len) == len;
}
size_t platformNvsRead(void*, const char* key, void* buf, size_t cap) {
  if (!Storage::race().isKey(key)) return 0;
  return Storage::race().getBytes(key, buf, cap);
}
uint32_t platformMillis(void*) { return millis(); }
uint32_t platformRandom(void*) { return esp_random(); }
bool platformNextSeq(void*, uint32_t* out) { return Identity::reserveCounter(out); }
uint32_t platformUnixTime(void*) { return WifiManager::getUnixTime(); }

bool isSelf(const char* id) { return strcmp(id, Identity::deviceId()) == 0; }

// The engine's room is the one shown when it belongs to the viewed group.
const RaceProtocol::RoomState* shownRoom() {
  if (g_engine == nullptr) return nullptr;
  if (!g_engine->hasRoom() || strcmp(g_engine->roomGroup(), g_groupCode) != 0) return nullptr;
  return g_engine->view();
}

void roundKeyOf(const RaceProtocol::RoomState* v, char* out, size_t cap) {
  if (v == nullptr || v->phase != RaceProtocol::PHASE_ROUND) {
    out[0] = '\0';
    return;
  }
  snprintf(out, cap, "%s/%08lX/%lu", v->coordinator, static_cast<unsigned long>(v->nonce),
           static_cast<unsigned long>(v->roundStart));
}

// The binding of the live round in the engine's room (invalid when there is
// no room view or no round in progress).
RoundBinding bindingOf(const RaceProtocol::RoomState* v) {
  RoundBinding b = {};
  if (g_engine == nullptr || v == nullptr || v->phase != RaceProtocol::PHASE_ROUND) return b;
  b.valid = true;
  strncpy(b.group, g_engine->roomGroup(), sizeof(b.group) - 1);
  strncpy(b.coordinator, v->coordinator, sizeof(b.coordinator) - 1);
  b.n = v->n;
  b.nonce = v->nonce;
  b.roundStart = v->roundStart;
  return b;
}

bool sameBinding(const RoundBinding& a, const RoundBinding& b) {
  return a.valid && b.valid && strcmp(a.group, b.group) == 0 && strcmp(a.coordinator, b.coordinator) == 0 &&
         a.n == b.n && a.nonce == b.nonce && a.roundStart == b.roundStart;
}

// Applies the engine's view to the guess history. No view (Leave completed,
// rejoining, Reconnecting) says nothing about the bound round, so the
// history is kept; any view other than exactly the bound round (a later
// round, the lobby or a result, a closed room, another room) discards it.
void reconcileRaceHistory(const RaceProtocol::RoomState* v) {
  if (v == nullptr || !g_raceHistoryRound.valid) return;
  if (!sameBinding(bindingOf(v), g_raceHistoryRound)) clearRaceGuessHistory();
}

// =============================================================================
// Room voice (Phase 4 section 17): MQTT broadcast transport, scoped by the
// room's identity ("<C><nonce>", 20 chars, within the 23-char room key),
// gated on actually viewing this Room right now.
// =============================================================================
uint16_t g_roomVoiceSeq = 0;
bool g_roomPttActive = false;

// False when no room of the viewed group is shown.
bool roomVoiceKey(char* out, size_t cap) {
  const RaceProtocol::RoomState* v = shownRoom();
  if (v == nullptr) return false;
  snprintf(out, cap, "%s%08lX", v->coordinator, static_cast<unsigned long>(v->nonce));
  return true;
}

void onRoomMicFrame(const int16_t* samples, size_t count) {
  char key[kIdLen];
  if (!roomVoiceKey(key, sizeof(key))) return;
  RadioTransport::publishAudioPacket(g_groupCode, "radio/audio/broadcast", RadioTransport::AUDIO_RACE_ROOM, key,
                                     g_roomVoiceSeq++, samples, count);
}

void startRoomPtt() {
  char key[kIdLen];
  if (g_roomPttActive || !roomVoiceKey(key, sizeof(key))) return;
  g_roomPttActive = true;
  g_roomVoiceSeq = 0;
  setRadioAudioActive(true);
  RadioAudio::startCapture(onRoomMicFrame);
}
void stopRoomPtt() {
  if (!g_roomPttActive) return;
  RadioAudio::stopCapture();
  setRadioAudioActive(false);
  g_roomPttActive = false;
}

void handleRoomAudioScope(const char* group_code, const char* sender, const char* roomKey, uint16_t sequence,
                          const int16_t* samples, size_t sampleCount) {
  (void)group_code;
  (void)sender;
  (void)sequence;
  if (!g_isRoomScreenActive) return;                 // only Room participants currently viewing it
  char key[kIdLen];
  if (!roomVoiceKey(key, sizeof(key)) || strcmp(roomKey, key) != 0) return;  // not this room
  RadioAudio::playFrame(samples, sampleCount);
  Sleep::notifyActivity();
}

// =============================================================================
// Guess screen (Phase 4 section 14; reuses Phase 3's shared evaluator)
// =============================================================================
DigitEntryState g_digitEntry;
const char* g_guessResultText = nullptr;
uint32_t g_guessResultUntilMs = 0;
bool g_pendingExternalSolve = false;
char g_pendingWinner[kDeviceIdLen] = {0};

// Phase 2C-1 (review round 2): nonblocking local notice on the Guess screen
// for an action whose outcome is only known once Guess is already the
// destination -- today only a Join-before-Continue whose publish this
// device could not confirm. Shown on the top content row while the Guess
// row and history stay usable (unlike g_guessResultText, it never freezes
// input or navigates). Visible for kGuessNoticeVisibleMs from when it is
// first actually drawn; cleared by invalidateGuessOutcome() (reset, leave,
// group change, new round).
constexpr uint32_t kGuessNoticeVisibleMs = 1500;
const char* g_guessNoticeText = nullptr;
bool g_guessNoticeDrawn = false;
uint32_t g_guessNoticeShownMs = 0;

void setGuessNotice(const char* text) {
  g_guessNoticeText = text;
  g_guessNoticeDrawn = false;
}

void exitGuessConfirmYes() { Menu::goBack(); }

// Phase 2A (D1) notice: a local correct guess, shown until the result.
void showLocalSolve() {
  g_roomSolvedNoticeActive = true;
  g_roomSolvedNoticeUntilMs = millis() + 1500;
}

// Follows the engine's confirmed state: binds a newly started round (its
// history, secret and automatic Guess entry) and turns the end of the bound
// round into its outcome ("Solved by X" on Guess, or "Round ended").
void syncRound() {
  if (g_engine == nullptr) return;
  const RaceProtocol::RoomState* v = g_engine->view();
  char key[kRoundKeyLen];
  roundKeyOf(v, key, sizeof(key));
  reconcileRaceHistory(v);  // on every view, also when the round key is unchanged (e.g. rejoining into the lobby)
  if (strcmp(key, g_roundId) == 0) return;
  if (g_roundId[0] != '\0') {  // the bound round ended
    if (v != nullptr && v->resultRound == g_roundStart && v->resultWinner[0] != '\0' &&
        !isSelf(v->resultWinner)) {
      Presence::resolveDisplayName(g_engine->roomGroup(), v->resultWinner, g_pendingWinner, sizeof(g_pendingWinner));
      g_pendingExternalSolve = true;
    }
    g_roundId[0] = '\0';
  }
  if (key[0] != '\0') {  // a newly bound round: supersedes the previous one's notices and outcome
    strncpy(g_roundId, key, sizeof(g_roundId) - 1);
    g_roundId[sizeof(g_roundId) - 1] = '\0';
    memcpy(g_secret, v->secret, 4);
    g_roundStart = v->roundStart;
    clearRoomSolvedNotice();
    invalidateGuessOutcome();
    clearRoomActionStatus();
    if (g_engine->state() == RaceProtocol::MS_MEMBER && g_engine->canAct()) g_pendingAutoEnterGuess = true;
  }
}

// A correct guess (4A0B) evaluated locally against the shown round: the
// coordinator commits it directly, a member sends a durable CLAIM. Either
// way the result reaches the Room through the confirmed state.
void onLocalSolve(const uint8_t digits[4]) {
  if (g_engine == nullptr) return;  // unreachable: no round is active without an engine
  if (!g_engine->submitCorrectGuess(digits)) {
    setGuessNotice("Not sent - try again");
    return;
  }
  clearRaceGuessHistory();
  showLocalSolve();
  syncRound();
  Menu::goBack();  // back to Room
}


// Phase 2A (D2): a guess may only be evaluated against a live round: the
// confirmed state's current round, played by an active member with no claim
// for it outstanding.
bool isRoundActive() {
  const RaceProtocol::RoomState* v = shownRoom();
  if (g_engine == nullptr || v == nullptr || g_roundId[0] == '\0' || g_engine->state() != RaceProtocol::MS_MEMBER ||
      !g_engine->canAct()) {
    return false;
  }
  char key[kRoundKeyLen];
  roundKeyOf(v, key, sizeof(key));
  return strcmp(key, g_roundId) == 0 && g_engine->isMeActive() && !g_engine->claimPending(v->roundStart);
}

// Phase 2A: binds the guess history and the in-progress digit entry to the
// current round. When the round differs from the one the history belongs
// to (room identity or round), both are discarded, so neither a previous
// round's results nor its half-typed digits can carry over into (or be
// submitted against) another round. Same round -> no-op (history survives
// Room <-> Guess re-entry, and Leave and rejoin of this round).
// Returns true if anything was reset.
bool syncGuessRound() {
  RoundBinding b = bindingOf(g_engine != nullptr ? g_engine->view() : nullptr);
  if (sameBinding(b, g_raceHistoryRound) || (!b.valid && !g_raceHistoryRound.valid)) return false;
  clearRaceGuessHistory();
  g_raceHistoryRound = b;
  NumberGuessing::resetDigitEntry(&g_digitEntry);
  return true;
}

void enterGuessScreen() {
  NumberGuessing::resetDigitEntry(&g_digitEntry);
  g_guessResultText = nullptr;
  if (isRoundActive()) syncGuessRound();
  g_isRoomScreenActive = false;
  stopRoomPtt();
  RadioTransport::setUiContext(RadioTransport::UiContext::NONE);
  if (Settings::getMuteRadioOutsideRadio()) {
    Presence::setOwnRadioAvailable(false);
    Presence::republishOwnPresenceAllGroups();
  }
  Menu::pushScreen(screenRaceGuess);
}

bool g_raceGuessDirty = true;
bool g_raceGuessNeedsFullRedraw = true;
bool g_raceGuessLastShowingResult = false;
bool g_raceGuessLastShowingNotice = false;
NumberGuessing::DigitRowRenderState g_raceGuessRowState;
uint16_t g_raceGuessLastHistoryVersion = 0;
bool g_raceGuessLastBrowsing = false;
int16_t g_raceGuessLastSelected = -1;
uint16_t g_raceGuessLastScroll = 0;

// Phase 2A: drops every pending AND visible Guess-screen outcome ("Solved by
// X" pending or shown, "Round ended" shown, its deadline) plus any not-yet-
// consumed auto-enter request. Called at every transition that supersedes
// the round those belonged to: reset/leave (applyReset()), group change,
// and local or remote new round. Forces a full Guess redraw so a notice
// that was on screen is replaced by the entry layout on the next tick.
void invalidateGuessOutcome() {
  g_guessNoticeText = nullptr;  // Phase 2C-1: its context is gone too
  g_guessNoticeDrawn = false;
  g_pendingExternalSolve = false;
  g_guessResultText = nullptr;
  g_guessResultUntilMs = 0;
  g_pendingAutoEnterGuess = false;
  g_raceGuessDirty = true;
  g_raceGuessNeedsFullRedraw = true;
}

// Phase 2A: layout matches Play Solo's screenSoloGuess() exactly -- Guess
// row pinned to the bottom of the content area, this round's history rows
// filling upward from it (most recent immediately above Guess), every
// history row drawn by the same NumberGuessing::drawGuessHistoryRow() Solo
// uses, so A/B meaning and presentation are identical. During an active
// round the only things ever drawn are this device's own submitted guesses,
// their A/B results, and the live digit-entry row -- never g_secret.
void screenRaceGuessTick();

// Phase 2C-1: records the replayed-Short window's end at the end of the tick
// that opened it (a solving Short ends this tick on the Room) -- see
// RaceShortWindow.
void screenRaceGuess() {
  screenRaceGuessTick();
  finishRaceScreenTick();
}

void screenRaceGuessTick() {
  if (Menu::consumeJustEntered()) {
    NumberGuessing::resetDigitEntry(&g_digitEntry);
    g_raceGuessDirty = true;
    g_raceGuessNeedsFullRedraw = true;
    NumberGuessing::resetDigitRowRenderState(&g_raceGuessRowState);
    NumberGuessing::resetHistoryBrowse(&g_raceBrowse);
  }
  syncRound();  // a result or a new round confirmed since the last tick

  if (g_pendingExternalSolve) {
    static char buf[40];
    snprintf(buf, sizeof(buf), "Solved by %s: %d%d%d%d", g_pendingWinner, g_secret[0], g_secret[1], g_secret[2],
             g_secret[3]);
    g_guessResultText = buf;
    g_guessResultUntilMs = millis() + 1500;
    g_pendingExternalSolve = false;
    g_raceGuessDirty = true;
  }

  // Phase 2A (D3): rollover-safe expiry (was a plain `millis() >= UntilMs`,
  // which expires immediately if UntilMs wrapped past UINT32_MAX).
  if (g_guessResultText != nullptr && !beforeDeadline(millis(), g_guessResultUntilMs)) {
    g_guessResultText = nullptr;
    Menu::goBack();
    return;
  }

  // Phase 2A (D2): the round this screen was opened for is gone (solved
  // elsewhere, the room closed or paused, or this device no longer in it) with no
  // external-solve notice to show instead -- stop accepting guesses for it.
  // Reuses the timed-notice path above: input freezes, then back to Room.
  if (g_guessResultText == nullptr && !isRoundActive()) {
    reconcileRaceHistory(g_engine != nullptr ? g_engine->view() : nullptr);
    NumberGuessing::resetHistoryBrowse(&g_raceBrowse);  // browsing ends with the screen, kept history or not
    g_guessResultText = "Round ended";
    g_guessResultUntilMs = millis() + 1500;
    g_raceGuessDirty = true;
  }

  // Phase 2C-1: the Guess notice expires kGuessNoticeVisibleMs after it was
  // first drawn; removing it gives its row back to history (full redraw).
  if (g_guessNoticeText != nullptr && g_guessNoticeDrawn &&
      static_cast<uint32_t>(millis() - g_guessNoticeShownMs) >= kGuessNoticeVisibleMs) {
    g_guessNoticeText = nullptr;
    g_guessNoticeDrawn = false;
    g_raceGuessDirty = true;
  }

  // A different round arrived while this screen was open: drop the old
  // round's history and half-typed digits before any input is processed.
  // The user is already on this round's Guess screen, so a pending
  // auto-enter request for it is satisfied (otherwise the next Back would
  // bounce straight back in from the Room).
  if (g_guessResultText == nullptr) {
    if (syncGuessRound()) g_raceGuessDirty = true;
    g_pendingAutoEnterGuess = false;
  }

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    if (isReplayedRaceShort(e)) continue;  // Phase 2C-1: Short replayed from a Race action window
    hadEvent = true;
    if (Input::isBack(e)) {
      Menu::goBack();
      // Phase 2C-1 (review round 2): Guess no longer owns input; the rest
      // of the batch stays queued for the Room (whose own Back handler runs
      // leaveRaceMode()) instead of being handled here -- a second Back
      // processed here would pop past the Room without that cleanup.
      break;
    }
    if (g_guessResultText != nullptr) continue;  // freeze input while showing the outcome
    if (NumberGuessing::handleHistoryBrowseEvent(&g_raceBrowse, g_digitEntry, e, g_raceHistoryCount) !=
        NumberGuessing::BrowseResult::PASS) {
      continue;  // same-screen history browsing (a browsing Short returns to digit editing)
    }
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (g_digitEntry.count == 4) {
        if (!isRoundActive()) continue;  // D2: never evaluate/score/publish a stale round
        NumberGuessing::GuessResult r = NumberGuessing::evaluate(g_secret, g_digitEntry.digits);
        if (r.a == 4) {
          openRaceShortWindow(e.eventMs);  // Phase 2C-1: covers the solve's blocking and Guess -> Room
          onLocalSolve(g_digitEntry.digits);
          return;
        }
        appendRaceGuess(g_digitEntry.digits, r);
        NumberGuessing::resetDigitEntry(&g_digitEntry);
      } else {
        ConfirmPromptConfig cfg{"Exit this game?", "Guess needs 4 digits", false, exitGuessConfirmYes, nullptr};
        Menu::startConfirmPrompt(cfg);
        Menu::pushScreen(Menu::confirmPromptScreen);
        break;  // Phase 2C-1: the prompt now owns the remaining input
      }
    } else {
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
    }
  }
  if (hadEvent) g_raceGuessDirty = true;

  uint8_t countBefore = g_digitEntry.count;
  NumberGuessing::tickDigitEntry(&g_digitEntry);
  if (g_digitEntry.count != countBefore) g_raceGuessDirty = true;  // hold-to-delete fired

  Display::drawStatusBar();
  if (!g_raceGuessDirty) return;
  g_raceGuessDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t guessY = static_cast<int16_t>(Display::kScreenHeight - lh);
  static const char* const kGuessLabel = "Guess: ";

  // Showing the transient "Solved by X" / "Round ended" outcome is a
  // genuine content-type change from the guess-entry layout, so it forces a
  // full redraw same as first entry (Global Invariant 12's spirit); it is
  // otherwise static while shown, so no further per-tick diffing is needed.
  bool showingResult = (g_guessResultText != nullptr);
  bool phaseChanged = (showingResult != g_raceGuessLastShowingResult);
  // Phase 2C-1: the notice appearing/disappearing moves the history area's
  // top edge, so it is a full redraw like any other layout change.
  bool showingNotice = !showingResult && g_guessNoticeText != nullptr;
  bool noticeChanged = (showingNotice != g_raceGuessLastShowingNotice);
  bool forceFull = g_raceGuessNeedsFullRedraw || phaseChanged || noticeChanged;
  int16_t historyTop = static_cast<int16_t>(contentTop + (showingNotice ? lh : 0));

  if (showingResult) {
    if (forceFull) {
      Display::clearContentArea();
      Display::printLine(2, contentTop, g_guessResultText);
      g_raceGuessNeedsFullRedraw = false;
    }
  } else {
    // Same three-way split as screenSoloGuess(): full draw on entry/phase
    // change, history region only when a guess was recorded or history was
    // cleared, and otherwise only renderDigitRow()'s own per-cell diffing.
    bool browseChanged = g_raceBrowse.browsing != g_raceGuessLastBrowsing ||
                         g_raceBrowse.selected != g_raceGuessLastSelected || g_raceBrowse.scroll != g_raceGuessLastScroll;
    bool historyChanged = forceFull || browseChanged || (g_raceHistoryVersion != g_raceGuessLastHistoryVersion);
    if (forceFull) {
      Display::clearContentArea();
      NumberGuessing::resetDigitRowRenderState(&g_raceGuessRowState);
      g_raceGuessNeedsFullRedraw = false;
      if (showingNotice) {
        Display::printLine(2, contentTop, g_guessNoticeText);
        if (!g_guessNoticeDrawn) {  // visible duration starts at the first actual draw
          g_guessNoticeDrawn = true;
          g_guessNoticeShownMs = millis();
        }
      }
    } else if (historyChanged) {
      int16_t regionH = static_cast<int16_t>(guessY - historyTop);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, historyTop, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    if (historyChanged) {
      int16_t availableHistoryHeight = static_cast<int16_t>(guessY - historyTop);
      uint8_t maxRows = (availableHistoryHeight > 0) ? static_cast<uint8_t>(availableHistoryHeight / lh) : 0;
      // A notice appearing/expiring changes maxRows: clamping keeps the
      // selected row inside the window.
      NumberGuessing::clampHistoryBrowse(&g_raceBrowse, g_raceHistoryCount, maxRows);
      uint8_t end = static_cast<uint8_t>(g_raceHistoryCount - g_raceBrowse.scroll);
      uint8_t shown = (end < maxRows) ? end : maxRows;
      uint8_t startIdx = static_cast<uint8_t>(end - shown);
      for (uint8_t i = startIdx; i < end; i++) {
        uint8_t rowIndex = static_cast<uint8_t>(i - startIdx);
        int16_t rowY = static_cast<int16_t>(guessY - (shown - rowIndex) * lh);
        const RaceGuessEntry& g = g_raceHistory[i];
        NumberGuessing::drawGuessHistoryRow(2, rowY, kGuessLabel, g.guessValue, g.a, g.b);
        if (g_raceBrowse.browsing && g_raceBrowse.selected == static_cast<int16_t>(g_raceHistoryCount - 1 - i)) {
          NumberGuessing::drawHistorySelectionMarker(2, rowY);
        }
      }
      g_raceGuessLastHistoryVersion = g_raceHistoryVersion;
    }
    NumberGuessing::renderDigitRow(&g_raceGuessRowState, 2, guessY, kGuessLabel, g_digitEntry);
    bool frontNow = g_raceBrowse.browsing && g_raceBrowse.selected < 0;
    bool frontBefore = g_raceGuessLastBrowsing && g_raceGuessLastSelected < 0;
    if (frontNow != frontBefore || (forceFull && frontNow)) {
      NumberGuessing::drawGuessLabel(2, guessY, kGuessLabel, frontNow);
    }
    g_raceGuessLastBrowsing = g_raceBrowse.browsing;
    g_raceGuessLastSelected = g_raceBrowse.selected;
    g_raceGuessLastScroll = g_raceBrowse.scroll;
  }

  g_raceGuessLastShowingResult = showingResult;
  g_raceGuessLastShowingNotice = showingNotice;
}


// =============================================================================
// Room screen
// =============================================================================
// Every row is recomputed from the engine each tick and diffed (Hardware
// Fix #3): the room changes through confirmed room states arriving at any
// time, as much as through local input.
void drawRoomActionNow(const char* text);
bool handleRoomAction(const InputEvent& e);

// Phase 2C-1: timed local notice on the action row (1500ms, rollover-safe,
// cleared by group change and new rounds).
void setRoomActionUnconfirmed(const char* text) {
  g_roomActionStatusText = text;
  g_roomActionStatusUntilMs = millis() + 1500;
}

// Fix Phase 1C (round 4): an action that sends anything needs this
// device's own subscriptions (r2/+, r2q/<own id>) live, so a press while
// the group is not Ready is refused with a visible status, never queued.
bool roomReady() {
  if (MqttManager::isGroupReady(g_groupCode)) return true;
  setRoomActionUnconfirmed("Connecting, try again");
  return false;
}

const char* noticeText(RaceProtocol::Notice n) {
  switch (n) {
    case RaceProtocol::NOTICE_ROOM_FULL: return "Room full";
    case RaceProtocol::NOTICE_COULD_NOT_JOIN: return "Couldn't join - try again";
    case RaceProtocol::NOTICE_ROOM_CLOSED: return "Room closed";
    case RaceProtocol::NOTICE_NO_LONGER_IN_ROOM: return "You're no longer in this room";
    case RaceProtocol::NOTICE_START_REFUSED: return "Start not allowed";
    case RaceProtocol::NOTICE_LATE: return "Already solved";
    case RaceProtocol::NOTICE_STORAGE_ERROR: return "Storage error";
    case RaceProtocol::NOTICE_DATA_DAMAGED: return "Room data damaged";
    case RaceProtocol::NOTICE_NUMBERS_EXHAUSTED: return "Can't create a room";
    case RaceProtocol::NOTICE_STAYED_LISTED: return "You may stay listed";
    default: return nullptr;
  }
}

// Engine notices are shown on the status row for kRoomNoticeMs from their
// first display, then cleared.
constexpr uint32_t kRoomNoticeMs = 4000;
RaceProtocol::Notice g_roomNoticeShown = RaceProtocol::NOTICE_NONE;
uint32_t g_roomNoticeSinceMs = 0;

// A Leave that could not be stored is refused, never shown as done; one whose
// final write is still being retried shows "Leaving..." with the storage
// error until it completes.
void leaveConfirmYes() {
  if (g_engine == nullptr) return;
  bool ok;
  if (g_engine->state() == RaceProtocol::MS_LEAVING) {
    ok = g_engine->leaveAnyway() || g_engine->exitPending();
  } else {
    ok = g_engine->leave();
  }
  if (!ok) setRoomActionUnconfirmed("Storage error - try again");
}

void startLeaveConfirm() {
  if (g_engine == nullptr) return;
  ConfirmPromptConfig cfg;
  if (g_engine->state() == RaceProtocol::MS_LEAVING) {
    cfg = ConfirmPromptConfig{"Leave anyway?", "You may stay listed", false, leaveConfirmYes, nullptr};
  } else if (g_engine->isCoordinator()) {
    cfg = ConfirmPromptConfig{"Leave room?", "Closes it for everyone", false, leaveConfirmYes, nullptr};
  } else {
    cfg = ConfirmPromptConfig{"Leave room?", nullptr, false, leaveConfirmYes, nullptr};
  }
  Menu::startConfirmPrompt(cfg);
  Menu::pushScreen(Menu::confirmPromptScreen);
}

void addRow(const char* id, RoomRowKind kind, uint16_t score) {
  if (g_participantCount >= kMaxParticipants) return;
  strncpy(g_participants[g_participantCount], id, kDeviceIdLen - 1);
  g_participants[g_participantCount][kDeviceIdLen - 1] = '\0';
  g_rowKind[g_participantCount] = kind;
  g_rowScore[g_participantCount] = score;
  g_participantCount++;
}

void buildRoomRows() {
  g_participantCount = 0;
  if (g_engine == nullptr) return;
  const RaceProtocol::RoomState* v = shownRoom();
  if (g_engine->hasRoom()) {
    if (strcmp(g_engine->roomGroup(), g_groupCode) != 0) return;  // in a room of another group
    if (v != nullptr) {
      for (uint8_t i = 0; i < v->count; i++) {
        if (v->entries[i].flags & RaceProtocol::ENTRY_ACTIVE) {
          addRow(v->entries[i].id, RoomRowKind::PARTICIPANT, v->entries[i].score);
        }
      }
    }
    addRow(kLeaveRowId, RoomRowKind::LEAVE, 0);
    return;
  }
  for (uint8_t i = 0; i < RaceProtocol::kMaxDiscovered; i++) {
    const RaceProtocol::DiscoveredRoom* d = g_engine->discovered(i);
    if (d != nullptr && strcmp(d->group, g_groupCode) == 0 && d->phase != RaceProtocol::PHASE_CLOSED) {
      addRow(d->coordinator, RoomRowKind::DISCOVERED, d->active);
    }
  }
  const RaceProtocol::RoomState* f = g_engine->finalView();
  if (f != nullptr && strcmp(g_engine->roomGroup(), g_groupCode) == 0) {
    for (uint8_t i = 0; i < f->count; i++) addRow(f->entries[i].id, RoomRowKind::FINAL, f->entries[i].score);
  }
}

// Returns the focused row's index, or -1 for the action row.
int16_t resolveRoomFocus() {
  if (g_roomFocusId[0] == '\0') return -1;
  for (uint8_t i = 0; i < g_participantCount; i++) {
    if (strcmp(g_participants[i], g_roomFocusId) == 0) {
      g_roomFocusIndex = i;
      return i;
    }
  }
  if (g_participantCount == 0) {
    resetRoomFocus();
    return -1;
  }
  uint8_t i = (g_roomFocusIndex < g_participantCount) ? g_roomFocusIndex : static_cast<uint8_t>(g_participantCount - 1);
  strncpy(g_roomFocusId, g_participants[i], kDeviceIdLen - 1);
  g_roomFocusId[kDeviceIdLen - 1] = '\0';
  g_roomFocusIndex = i;
  return i;
}

// One detent: +1 moves down (towards later rows), -1 up towards the action
// row. No wraparound.
void moveRoomFocus(int8_t delta) {
  int16_t next = static_cast<int16_t>(resolveRoomFocus() + (delta > 0 ? 1 : -1));
  if (next >= static_cast<int16_t>(g_participantCount)) next = static_cast<int16_t>(g_participantCount) - 1;
  if (next < 0) {
    g_roomFocusId[0] = '\0';
    return;
  }
  strncpy(g_roomFocusId, g_participants[next], kDeviceIdLen - 1);
  g_roomFocusId[kDeviceIdLen - 1] = '\0';
  g_roomFocusIndex = static_cast<uint8_t>(next);
}

// A Short on a focused row. Returns true iff it navigated away.
bool handleRoomRowShort(int16_t row, const InputEvent& e) {
  if (g_engine == nullptr) return false;
  RoomRowKind kind = g_rowKind[row];
  if (kind == RoomRowKind::LEAVE) {
    startLeaveConfirm();
    return true;
  }
  if (kind == RoomRowKind::DISCOVERED) {
    if (!roomReady()) return false;
    clearRoomActionStatus();
    openRaceShortWindow(e.eventMs);
    drawRoomActionNow("Joining...");
    char coordinator[kDeviceIdLen];
    strncpy(coordinator, g_participants[row], sizeof(coordinator) - 1);
    coordinator[sizeof(coordinator) - 1] = '\0';
    if (!g_engine->join(g_groupCode, coordinator)) setRoomActionUnconfirmed("Couldn't join");
    resetRoomFocus();
    return false;
  }
  g_roomFocusId[0] = '\0';  // a Short on a participant returns focus to the action row
  return false;
}

// Phase 2C-1: `e` is the accepted Short. Each action that proceeds draws its
// in-progress text BEFORE its blocking work and opens the replayed-Short
// window (see RaceShortWindow above). A refusal or a no-op press does no
// blocking work and opens nothing. Returns true iff it navigated away from
// the Room (Start Round or Continue -> Guess).
bool handleRoomAction(const InputEvent& e) {
  if (g_engine == nullptr) return false;  // "Race unavailable": no action
  if (g_engine->hasRoom() && strcmp(g_engine->roomGroup(), g_groupCode) != 0) return false;
  const RaceProtocol::RoomState* v = shownRoom();
  RaceProtocol::MemberState st = g_engine->state();
  if (st == RaceProtocol::MS_NONE) {  // Invite to Play: create a room coordinated by this device
    if (!roomReady()) return false;
    clearRoomActionStatus();
    openRaceShortWindow(e.eventMs);
    drawRoomActionNow("Creating...");
    if (!g_engine->createRoom(g_groupCode)) setRoomActionUnconfirmed("Couldn't create room");
    return false;
  }
  if (st != RaceProtocol::MS_MEMBER || v == nullptr || !g_engine->canAct()) return false;
  if (v->phase == RaceProtocol::PHASE_LOBBY) {
    if (!g_engine->canStart()) return false;  // "Waiting for <holder>"
    if (!roomReady()) return false;
    clearRoomActionStatus();
    // The window also covers Start Round -> Guess, as for Continue below.
    openRaceShortWindow(e.eventMs);
    drawRoomActionNow("Starting...");
    if (!g_engine->startRound()) {
      setRoomActionUnconfirmed("Start failed");
      return false;
    }
    syncRound();
    if (isRoundActive()) {  // the coordinator's own Start: the round is committed, play it now (R2)
      g_pendingAutoEnterGuess = false;
      enterGuessScreen();
      return true;
    }
    return false;  // a member's START request: Guess opens when the round arrives
  }
  if (!isRoundActive()) return false;  // claim being confirmed
  // Continue: the window also covers the Room -> Guess transition, so a
  // replayed Short can neither push Guess again nor open Guess's Exit prompt.
  openRaceShortWindow(e.eventMs);
  enterGuessScreen();
  return true;
}

// Back is navigation only (B3): it never changes room membership.
void leaveRaceMode() {
  g_isRoomScreenActive = false;
  stopRoomPtt();
  RadioTransport::setUiContext(RadioTransport::UiContext::NONE);
  if (Settings::getMuteRadioOutsideRadio()) {
    Presence::setOwnRadioAvailable(false);
    Presence::republishOwnPresenceAllGroups();
  }
  closeRaceShortWindow();  // Phase 2C-1: Race is left (the Room stops handling this batch)
}

// Hardware Fix #3: each row (title, action label, status, and each
// participant slot) is diffed and redrawn independently.
bool g_raceRoomNeedsFullRedraw = true;
char g_raceRoomLastActionLabel[40] = {0};
char g_raceRoomLastScoreLine[40] = {0};
uint8_t g_raceRoomLastOnlineCount = 0xFF;
constexpr uint8_t kMaxRoomRows = 6;  // more than fit at any font; bounds the row caches
char g_raceRoomLastOnlineNames[kMaxRoomRows][40] = {{0}};
char g_raceRoomLastTitle[24] = {0};

// Phase 2C-1: synchronous in-progress text on the Room's action row, drawn
// before the action's blocking work. The row's diff cache is updated so the
// next render repaints it with whatever label/notice then applies. If the
// Room has not rendered since entry, the content area is cleared first
// (the pending full redraw repaints everything afterwards).
void drawRoomActionNow(const char* text) {
  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t actionY = static_cast<int16_t>(Display::kStatusBarHeight + 2 + lh);
  if (g_raceRoomNeedsFullRedraw) {
    Display::clearContentArea();
  } else {
    Display::tft().fillRect(0, actionY, Display::kScreenWidth, lh, ST77XX_BLACK);
  }
  Display::printLine(2, actionY, text);
  strncpy(g_raceRoomLastActionLabel, text, sizeof(g_raceRoomLastActionLabel) - 1);
  g_raceRoomLastActionLabel[sizeof(g_raceRoomLastActionLabel) - 1] = '\0';
}

// The action row's text, and whether a Short there does something.
const char* roomActionText(bool* hasAction, char* buf, size_t cap) {
  *hasAction = false;
  if (g_engine == nullptr) return "Race unavailable";
  if (g_engine->hasRoom() && strcmp(g_engine->roomGroup(), g_groupCode) != 0) return "In a room in another group";
  const RaceProtocol::RoomState* v = shownRoom();
  if (g_engine->exitPending()) return "Leaving...";
  switch (g_engine->state()) {
    case RaceProtocol::MS_NONE:
      *hasAction = true;
      return "Invite to Play";
    case RaceProtocol::MS_JOINING:
      return "Joining...";
    case RaceProtocol::MS_LEAVING:
      return "Leaving...";
    default:
      break;
  }
  if (v == nullptr) return "Reconnecting...";
  if (!g_engine->canAct()) return "Leave only";
  if (v->phase == RaceProtocol::PHASE_LOBBY) {
    uint8_t op = 0;
    if (g_engine->pendingOp(&op) && op == RaceProtocol::OP_START) return "Starting...";
    if (g_engine->canStart()) {
      *hasAction = true;
      return "Start Round";
    }
    char name[17];
    Presence::resolveDisplayName(g_groupCode, RaceProtocol::startHolder(*v), name, sizeof(name));
    snprintf(buf, cap, "Waiting for %s", name);
    return buf;
  }
  if (g_engine->claimPending(v->roundStart)) return "Confirming...";
  *hasAction = true;
  return "Continue";
}

// The status row (in place of the old local-only score row): problems
// first, then the local solve notice, then the engine's last notice.
const char* roomStatusText(char* buf, size_t cap) {
  if (g_engine == nullptr) return "";
  if (g_engine->hasRoom() && strcmp(g_engine->roomGroup(), g_groupCode) == 0) {
    if (g_engine->storagePaused()) return "Storage error - room paused";
    if (g_engine->frozen()) return "Room data inconsistent";
    if (g_engine->verifying()) return "Verifying room...";
    if (g_engine->reconnecting()) return "Reconnecting...";
    if (g_engine->waitingForCoordinator()) {
      char name[17];
      Presence::resolveDisplayName(g_groupCode, shownRoom() ? shownRoom()->coordinator : "", name, sizeof(name));
      snprintf(buf, cap, "Waiting for %s", name);
      return buf;
    }
  }
  if (g_roomSolvedNoticeActive && !beforeDeadline(millis(), g_roomSolvedNoticeUntilMs)) clearRoomSolvedNotice();
  if (g_roomSolvedNoticeActive) return "Solved! 4A0B";
  RaceProtocol::Notice n = g_engine->notice();
  if (n != g_roomNoticeShown) {
    g_roomNoticeShown = n;
    g_roomNoticeSinceMs = millis();
  }
  if (n != RaceProtocol::NOTICE_NONE && static_cast<uint32_t>(millis() - g_roomNoticeSinceMs) >= kRoomNoticeMs) {
    g_engine->clearNotice();
    g_roomNoticeShown = RaceProtocol::NOTICE_NONE;
    n = RaceProtocol::NOTICE_NONE;
  }
  const char* t = noticeText(n);
  return t != nullptr ? t : "";
}

void diffRow(int16_t y, const char* text, char* cache, size_t cacheSize, bool firstDraw, int16_t lh) {
  if (!firstDraw && strcmp(text, cache) == 0) return;
  if (!firstDraw) Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
  Display::printLine(2, y, text);
  strncpy(cache, text, cacheSize - 1);
  cache[cacheSize - 1] = '\0';
}

// A row's label: "> Name (You)", "Join Name (3)" (a discovered room: its
// coordinator and active participant count), "Leave room" (or "Leave anyway").
// Scores are drawn right-aligned beside participant names; the label is
// shortened so it never runs into the score.
void formatRoomRow(uint8_t i, bool focused, char* out, size_t cap) {
  if (g_engine == nullptr) {  // no rows are built without an engine
    out[0] = '\0';
    return;
  }
  const char* marker = focused ? "> " : "";
  char name[17];
  switch (g_rowKind[i]) {
    case RoomRowKind::LEAVE:
      snprintf(out, cap, "%s%s", marker, g_engine->state() == RaceProtocol::MS_LEAVING ? "Leave anyway" : "Leave room");
      return;
    case RoomRowKind::DISCOVERED:
      Presence::resolveDisplayName(g_groupCode, g_participants[i], name, sizeof(name));
      snprintf(out, cap, "%sJoin %s (%u)", marker, name, g_rowScore[i]);
      return;
    default:
      Presence::resolveDisplayName(g_groupCode, g_participants[i], name, sizeof(name));
      snprintf(out, cap, "%s%s%s", marker, name, isSelf(g_participants[i]) ? " (You)" : "");
      return;
  }
}

void drawRoomRow(int16_t y, uint8_t i, const char* label) {
  bool scored = g_rowKind[i] == RoomRowKind::PARTICIPANT || g_rowKind[i] == RoomRowKind::FINAL;
  if (!scored) {
    Display::printLine(2, y, label);
    return;
  }
  char score[8];
  snprintf(score, sizeof(score), "%u", g_rowScore[i]);
  // fitWidth(), the measure printLine() clips with: aligning by ink width
  // alone put a leading "1" (3 px left bearing) past the edge, so a score of
  // 1 was clipped away and 10-19 showed as "1".
  int16_t scoreW = Display::fitWidth(score);
  int16_t scoreX = static_cast<int16_t>(Display::kScreenWidth - 2 - scoreW);
  char shown[40];
  strncpy(shown, label, sizeof(shown) - 1);
  shown[sizeof(shown) - 1] = '\0';
  size_t n = strlen(shown);
  while (n > 0 && 2 + Display::textWidth(shown) + 8 > scoreX) shown[--n] = '\0';
  Display::printLine(2, y, shown);
  Display::printLine(scoreX, y, score);
}

void screenRaceRoomTick();

// Phase 2C-1: see screenRaceGuess() -- same tick-end window handling.
void screenRaceRoom() {
  screenRaceRoomTick();
  finishRaceScreenTick();
}

void screenRaceRoomTick() {
  if (Menu::consumeJustEntered()) {
    g_isRoomScreenActive = true;
    RadioTransport::setUiContext(RadioTransport::UiContext::RACE_ROOM);
    setRoomAvailability(true);
    Notifications::setRaceInvitePending(false);
    g_raceRoomNeedsFullRedraw = true;
    resetRoomFocus();  // every Room visit starts on the action row
    if (millis() - g_heapLogLastMs >= kHeapLogMinGapMs) logRaceHeap("room");
  }

  syncRound();
  if (g_pendingAutoEnterGuess) {
    g_pendingAutoEnterGuess = false;
    if (isRoundActive()) {
      openRaceShortWindowFromWatermark();  // Phase 2C-1: covers entry blocking + Room -> Guess
      enterGuessScreen();
      return;
    }
  }

  buildRoomRows();
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (isReplayedRaceShort(e)) continue;  // Phase 2C-1: Short replayed from a Race action window
    if (Input::isBack(e)) {
      leaveRaceMode();
      Menu::goBack();
      // Phase 2C-1 (review round 2): Race is left; the rest of the batch
      // belongs to the screen below, not to Race (no post-exit actions).
      break;
    }
    if (e.type == InputEventType::DOT_PRESS_START) {
      startRoomPtt();
    } else if (e.type == InputEventType::DOT_RELEASE) {
      stopRoomPtt();
    } else if (e.type == InputEventType::ENCODER_ROTATE) {
      moveRoomFocus(e.value);
    } else if (e.type == InputEventType::ENCODER_SHORT) {
      int16_t row = resolveRoomFocus();
      bool away = (row >= 0) ? handleRoomRowShort(row, e) : handleRoomAction(e);
      if (away) break;  // Phase 2C-1: the next screen owns the remaining input
      buildRoomRows();
    }
  }

  Display::drawStatusBar();

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t titleY = Display::kStatusBarHeight + 2;
  int16_t actionY = static_cast<int16_t>(titleY + lh);
  int16_t scoreY = static_cast<int16_t>(actionY + lh);
  int16_t rowsTopY = static_cast<int16_t>(scoreY + lh);

  bool firstDraw = g_raceRoomNeedsFullRedraw;
  if (firstDraw) {
    Display::clearContentArea();
    g_raceRoomNeedsFullRedraw = false;
  }

  // Row window: as many rows as fit below the status row, scrolled so the
  // focused row is visible.
  int16_t remaining = Display::kScreenHeight - rowsTopY;
  uint8_t rows = (remaining > 0) ? static_cast<uint8_t>(remaining / lh) : 0;
  if (rows > kMaxRoomRows) rows = kMaxRoomRows;
  int16_t focus = resolveRoomFocus();
  uint8_t maxTop = (g_participantCount > rows) ? static_cast<uint8_t>(g_participantCount - rows) : 0;
  if (focus >= 0 && focus < g_roomListTop) g_roomListTop = static_cast<uint8_t>(focus);
  if (focus >= 0 && rows > 0 && focus >= g_roomListTop + rows) g_roomListTop = static_cast<uint8_t>(focus - rows + 1);
  if (g_roomListTop > maxTop) g_roomListTop = maxTop;
  uint8_t visibleCount = static_cast<uint8_t>(g_participantCount - g_roomListTop);
  if (visibleCount > rows) visibleCount = rows;

  // Title, with the visible range of listed rooms/players (not the Leave
  // row) once the list no longer fits.
  char title[24];
  uint8_t listed = g_participantCount;
  if (listed > 0 && g_rowKind[listed - 1] == RoomRowKind::LEAVE) listed--;
  if (g_participantCount > rows && visibleCount > 0 && listed > 0) {
    uint8_t first = g_roomListTop + 1 < listed ? static_cast<uint8_t>(g_roomListTop + 1) : listed;
    uint8_t last = g_roomListTop + visibleCount < listed ? static_cast<uint8_t>(g_roomListTop + visibleCount) : listed;
    snprintf(title, sizeof(title), "Race Room  %u-%u/%u", first, last, listed);
  } else {
    snprintf(title, sizeof(title), "Race Room");
  }
  diffRow(titleY, title, g_raceRoomLastTitle, sizeof(g_raceRoomLastTitle), firstDraw, lh);

  // Hardware Fix #4 issue 9: a ">" marker shows when ENCODER_SHORT on the
  // action row currently does something; passive states get no marker.
  // Fix Phase 1C (round 4): a refused/unconfirmed press overrides the label
  // with a visible status for a short, rollover-safe window.
  char actionBuf[32];
  const char* actionText;
  bool hasAction;
  if (g_roomActionStatusText != nullptr && beforeDeadline(millis(), g_roomActionStatusUntilMs)) {
    actionText = g_roomActionStatusText;
    hasAction = false;  // not a live action right now -- no ">" marker
  } else {
    clearRoomActionStatus();
    actionText = roomActionText(&hasAction, actionBuf, sizeof(actionBuf));
  }
  // The ">" also marks where the focus is: with a row focused, the action
  // shows no marker.
  if (focus >= 0) hasAction = false;
  char actionLabel[40];
  snprintf(actionLabel, sizeof(actionLabel), "%s%s", hasAction ? "> " : "", actionText);
  diffRow(actionY, actionLabel, g_raceRoomLastActionLabel, sizeof(g_raceRoomLastActionLabel), firstDraw, lh);

  char statusBuf[40];
  const char* status = roomStatusText(statusBuf, sizeof(statusBuf));
  diffRow(scoreY, status, g_raceRoomLastScoreLine, sizeof(g_raceRoomLastScoreLine), firstDraw, lh);

  // Rows (scrolled window; see above). The focused one carries the ">"
  // marker; the diff key includes the score so a score change repaints
  // only its own row.
  char shownNames[kMaxRoomRows][40];
  for (uint8_t r = 0; r < visibleCount; r++) {
    uint8_t i = static_cast<uint8_t>(g_roomListTop + r);
    char label[32];
    formatRoomRow(i, focus == i, label, sizeof(label));
    snprintf(shownNames[r], sizeof(shownNames[r]), "%s\t%u", label, g_rowScore[i]);
  }
  bool layoutChanged = firstDraw || visibleCount != g_raceRoomLastOnlineCount;
  if (layoutChanged && !firstDraw) {
    int16_t regionH = static_cast<int16_t>(Display::kScreenHeight - rowsTopY);
    if (regionH < 0) regionH = 0;
    Display::tft().fillRect(0, rowsTopY, Display::kScreenWidth, regionH, ST77XX_BLACK);
  }
  for (uint8_t r = 0; r < visibleCount; r++) {
    if (!layoutChanged && strcmp(shownNames[r], g_raceRoomLastOnlineNames[r]) == 0) continue;
    int16_t rowY = static_cast<int16_t>(rowsTopY + r * lh);
    if (!layoutChanged) Display::tft().fillRect(0, rowY, Display::kScreenWidth, lh, ST77XX_BLACK);
    uint8_t i = static_cast<uint8_t>(g_roomListTop + r);
    char label[32];
    formatRoomRow(i, focus == i, label, sizeof(label));
    drawRoomRow(rowY, i, label);
    strncpy(g_raceRoomLastOnlineNames[r], shownNames[r], sizeof(g_raceRoomLastOnlineNames[r]) - 1);
    g_raceRoomLastOnlineNames[r][sizeof(g_raceRoomLastOnlineNames[r]) - 1] = '\0';
  }
  g_raceRoomLastOnlineCount = visibleCount;
}

// =============================================================================
// Entry (Training Game > Number Guessing > Race Mode already routes here via
// settings.cpp's trampolineRace)
// =============================================================================
SettingItem g_groupSelectItems[Settings::kMaxGroups];
char g_groupSelectLabelBuf[Settings::kMaxGroups][21];
ListMenu g_groupSelectListMenu;

void groupSelectTrampoline() {
  uint8_t idx = g_groupSelectListMenu.selectedIndex();
  Settings::FamilyGroup g = Settings::getGroup(idx);
  strncpy(g_groupCode, g.code, sizeof(g_groupCode) - 1);
  g_groupCode[sizeof(g_groupCode) - 1] = '\0';
  // Fix Phase 1C (round 4 correction): a refusal notice is scoped to
  // whichever group's room it was shown for -- switching g_groupCode to a
  // different room's identity here must not let it appear in the new
  // room.
  clearRoomActionStatus();
  closeRaceShortWindow();  // Phase 2C-1: a different room is a new context
  // Phase 2A: likewise for guess history, the "Solved!" notice, and any
  // pending/visible Guess outcome.
  clearRaceGuessHistory();
  clearRoomSolvedNotice();
  invalidateGuessOutcome();
  Menu::goBack();
  Menu::pushScreen(screenRaceRoom);
}

void screenGroupSelect() {
  if (Menu::consumeJustEntered()) {
    uint8_t n = Settings::getGroupCount();
    for (uint8_t i = 0; i < n; i++) {
      Settings::FamilyGroup g = Settings::getGroup(i);
      strncpy(g_groupSelectLabelBuf[i], g.name, sizeof(g_groupSelectLabelBuf[i]) - 1);
      g_groupSelectLabelBuf[i][sizeof(g_groupSelectLabelBuf[i]) - 1] = '\0';
      g_groupSelectItems[i] = SettingItem{g_groupSelectLabelBuf[i], groupSelectTrampoline};
    }
    g_groupSelectListMenu.configure(g_groupSelectItems, n);
  }
  Display::drawStatusBar();
  g_groupSelectListMenu.tick("Select Group");
}

bool g_raceNoGroupsDirty = true;

void screenNoFamilyGroups() {
  bool justEntered = Menu::consumeJustEntered();
  if (justEntered) g_raceNoGroupsDirty = true;

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }
  Display::drawStatusBar();
  if (!g_raceNoGroupsDirty) return;
  g_raceNoGroupsDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();
  int16_t y = 60;
  Display::printLine(6, y, "No Family Groups");
  y += lh;
  Display::printLine(6, y, "Add one in Settings");
}

void screenRaceEntry() {
  closeRaceShortWindow();  // Phase 2C-1: every Race visit starts without a window
  uint8_t n = Settings::getGroupCount();
  ScreenHandlerFn target;
  if (n == 0) {
    target = screenNoFamilyGroups;
  } else if (n == 1) {
    Settings::FamilyGroup g = Settings::getGroup(0);
    strncpy(g_groupCode, g.code, sizeof(g_groupCode) - 1);
    g_groupCode[sizeof(g_groupCode) - 1] = '\0';
    target = screenRaceRoom;
  } else {
    target = screenGroupSelect;
  }
  Menu::goBack();
  Menu::pushScreen(target);
}

// =============================================================================
// Network packets and background service: the engine handles both kinds;
// it is booted once storage is up and ticked every loop.
// =============================================================================
void handleRaceStatePacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  if (g_engine == nullptr) return;
  g_engine->onPacket(group_code, topic, RaceProtocol::kKindState, payload, payloadLen);
}

void handleRaceRequestPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  if (g_engine == nullptr) return;
  g_engine->onPacket(group_code, topic, RaceProtocol::kKindRequest, payload, payloadLen);
}

// Allocates and boots the engine (the firmware calls this once per boot). A
// repeated call preserves an existing engine: it neither replaces nor re-boots
// it. Allocation failure does not abort: Race stays unavailable, its saved
// records untouched, and a later call retries the allocation.
void raceServiceInit() {
  if (g_engine != nullptr) return;
  logRaceHeap("pre-alloc");
  g_engine = new (std::nothrow) RaceProtocol::Engine();
  {
    char event[48];
    snprintf(event, sizeof(event), "engine-alloc-%s size=%lu", g_engine != nullptr ? "ok" : "FAILED",
             static_cast<unsigned long>(sizeof(RaceProtocol::Engine)));
    logRaceHeap(event);
  }
  if (g_engine == nullptr) return;
  RaceProtocol::Platform p{nullptr,          Identity::deviceId(), platformPublish, platformGroupReady,
                           platformNvsWrite, platformNvsRead,      platformMillis,  platformRandom,
                           platformNextSeq,  platformUnixTime};
  g_engine->init(p);
  g_engine->boot();
}

void raceServiceTick() {
  if (millis() - g_heapLogLastMs >= kHeapLogPeriodMs) logRaceHeap("periodic");
  if (g_engine == nullptr) return;
  g_engine->tick();
  syncRound();
  if (g_engine->takeNewRoomNotification()) {  // a room just created in one of my groups
    Notifications::setRaceInvitePending(true);
    playTone(1000, 150, SOUND_NOTIFICATION);
  }
}

struct Registrar {
  Registrar() {
    registerGameSubModeHandler(GameSubModes::RACE, screenRaceEntry);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_STATE, handleRaceStatePacket);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_REQUEST, handleRaceRequestPacket);
    RadioTransport::registerAudioScopeHandler(RadioTransport::AUDIO_RACE_ROOM, handleRoomAudioScope);
    AppService svc;
    svc.init = raceServiceInit;
    svc.tick = raceServiceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace
