#include "core/race.h"

#include <Arduino.h>
#include <string.h>

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
#include "core/radio_audio.h"
#include "core/radio_transport.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/sound_facade.h"

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

// =============================================================================
// Room / round state (Phase 4 sections 11-18)
// =============================================================================
enum class RoomPhase : uint8_t { NO_LOBBY, LOBBY_WAITING, ROUND_ACTIVE };
RoomPhase g_phase = RoomPhase::NO_LOBBY;
char g_groupCode[33] = {0};
char g_inviteId[kIdLen] = {0};
char g_roundId[kIdLen] = {0};
char g_ownerDeviceId[kDeviceIdLen] = {0};
uint8_t g_secret[4] = {0};
bool g_haveJoined = false;
bool g_hasHadFirstRound = false;
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

// Phase 2A: Race guess history -- Solo-style "1 2 3 4   2A2B" rows for THIS
// device's own submitted guesses in the current round. Local RAM only
// (never persisted, never published), bounded: once full the oldest entry
// is dropped, which only ever affects rows already scrolled off screen
// (the Guess screen shows at most ~5 at PRIMARY line height). Keyed by the
// round it belongs to, so a stale result can never be shown against a
// different round's secret; cleared on every round end/reset/group change.
struct RaceGuessEntry {
  uint16_t guessValue;
  uint8_t a;
  uint8_t b;
};
constexpr uint8_t kRaceHistoryCap = 8;
RaceGuessEntry g_raceHistory[kRaceHistoryCap];  // oldest first
uint8_t g_raceHistoryCount = 0;
char g_raceHistoryRoundId[kIdLen] = {0};
// Bumped on every append/clear so the Guess screen can tell "history
// changed" apart from a plain digit-entry change (Solo uses historyCount
// for this, which stops changing once this bounded buffer is full).
uint16_t g_raceHistoryVersion = 0;

void clearRaceGuessHistory() {
  g_raceHistoryCount = 0;
  g_raceHistoryRoundId[0] = '\0';
  g_raceHistoryVersion++;
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
// Room's score row after this device's own correct guess. It states the
// local evaluation result only -- NOT that the SOLVED packet reached the
// broker or any peer (publishBinary()'s result is still discarded).
uint32_t g_roomSolvedNoticeUntilMs = 0;
bool g_roomSolvedNoticeActive = false;

void clearRoomSolvedNotice() {
  g_roomSolvedNoticeActive = false;
  g_roomSolvedNoticeUntilMs = 0;
}

constexpr uint8_t kMaxParticipants = 20;
char g_participants[kMaxParticipants][kDeviceIdLen];
uint8_t g_participantCount = 0;


void addParticipant(const char* deviceId) {
  for (uint8_t i = 0; i < g_participantCount; i++) {
    if (strcmp(g_participants[i], deviceId) == 0) return;
  }
  if (g_participantCount < kMaxParticipants) {
    strncpy(g_participants[g_participantCount], deviceId, kDeviceIdLen - 1);
    g_participants[g_participantCount][kDeviceIdLen - 1] = '\0';
    g_participantCount++;
  }
}

// ---- Scores: local RAM only (Phase 4 section 16) ---------------------------
struct ScoreEntry {
  char device_id[kDeviceIdLen];
  uint16_t score;
};
constexpr uint8_t kMaxScoreEntries = 20;
ScoreEntry g_scores[kMaxScoreEntries];
uint8_t g_scoreCount = 0;

ScoreEntry* findOrCreateScoreEntry(const char* deviceId) {
  for (uint8_t i = 0; i < g_scoreCount; i++) {
    if (strcmp(g_scores[i].device_id, deviceId) == 0) return &g_scores[i];
  }
  if (g_scoreCount < kMaxScoreEntries) {
    strncpy(g_scores[g_scoreCount].device_id, deviceId, kDeviceIdLen - 1);
    g_scores[g_scoreCount].device_id[kDeviceIdLen - 1] = '\0';
    g_scores[g_scoreCount].score = 0;
    return &g_scores[g_scoreCount++];
  }
  return nullptr;
}
void incrementScore(const char* deviceId) {
  ScoreEntry* e = findOrCreateScoreEntry(deviceId);
  if (e != nullptr) e->score++;
}
uint16_t getScore(const char* deviceId) {
  for (uint8_t i = 0; i < g_scoreCount; i++) {
    if (strcmp(g_scores[i].device_id, deviceId) == 0) return g_scores[i].score;
  }
  return 0;
}

// ---- Availability helper (Phase 4 section 10) ------------------------------
void setRoomAvailability(bool available) {
  if (!Settings::getMuteRadioOutsideRadio()) return;  // Mute Off: availability just tracks ONLINE
  Presence::setOwnRadioAvailable(available);
  Presence::republishOwnPresenceAllGroups();
}

// =============================================================================
// Wire codecs — no numeric fields beyond the raw 4-byte secret, so no
// endianness helpers are needed here.
// =============================================================================
size_t buildInvitePayload(const char* inviteId, const char* initiatorId, uint8_t* out, size_t cap) {
  size_t need = kIdLen + kDeviceIdLen;
  if (cap < need) return 0;
  memset(out, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out), inviteId, kIdLen - 1);
  memset(out + kIdLen, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + kIdLen), initiatorId, kDeviceIdLen - 1);
  return need;
}
bool parseInvitePayload(const uint8_t* data, uint16_t len, char* outInviteId, char* outInitiatorId) {
  if (data == nullptr || len < kIdLen + kDeviceIdLen) return false;
  memcpy(outInviteId, data, kIdLen);
  outInviteId[kIdLen - 1] = '\0';
  memcpy(outInitiatorId, data + kIdLen, kDeviceIdLen);
  outInitiatorId[kDeviceIdLen - 1] = '\0';
  return true;
}

size_t buildJoinPayload(const char* inviteId, const char* deviceId, uint8_t* out, size_t cap) {
  size_t need = kIdLen + kDeviceIdLen;
  if (cap < need) return 0;
  memset(out, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out), inviteId, kIdLen - 1);
  memset(out + kIdLen, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + kIdLen), deviceId, kDeviceIdLen - 1);
  return need;
}
bool parseJoinPayload(const uint8_t* data, uint16_t len, char* outInviteId, char* outDeviceId) {
  if (data == nullptr || len < kIdLen + kDeviceIdLen) return false;
  memcpy(outInviteId, data, kIdLen);
  outInviteId[kIdLen - 1] = '\0';
  memcpy(outDeviceId, data + kIdLen, kDeviceIdLen);
  outDeviceId[kDeviceIdLen - 1] = '\0';
  return true;
}

size_t buildRoundPayload(const char* inviteId, const char* roundId, const char* starterId, const uint8_t secret[4],
                         uint8_t* out, size_t cap) {
  size_t need = kIdLen + kIdLen + kDeviceIdLen + 4;
  if (cap < need) return 0;
  size_t pos = 0;
  memset(out + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), inviteId, kIdLen - 1);
  pos += kIdLen;
  memset(out + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), roundId, kIdLen - 1);
  pos += kIdLen;
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), starterId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  memcpy(out + pos, secret, 4);
  pos += 4;
  return pos;
}
bool parseRoundPayload(const uint8_t* data, uint16_t len, char* outInviteId, char* outRoundId, char* outStarterId,
                       uint8_t outSecret[4]) {
  size_t need = kIdLen + kIdLen + kDeviceIdLen + 4;
  if (data == nullptr || len < need) return false;
  size_t pos = 0;
  memcpy(outInviteId, data + pos, kIdLen);
  outInviteId[kIdLen - 1] = '\0';
  pos += kIdLen;
  memcpy(outRoundId, data + pos, kIdLen);
  outRoundId[kIdLen - 1] = '\0';
  pos += kIdLen;
  memcpy(outStarterId, data + pos, kDeviceIdLen);
  outStarterId[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  memcpy(outSecret, data + pos, 4);
  return true;
}

size_t buildSolvedPayload(const char* roundId, const char* winnerId, uint8_t* out, size_t cap) {
  size_t need = kIdLen + kDeviceIdLen;
  if (cap < need) return 0;
  memset(out, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out), roundId, kIdLen - 1);
  memset(out + kIdLen, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + kIdLen), winnerId, kDeviceIdLen - 1);
  return need;
}
bool parseSolvedPayload(const uint8_t* data, uint16_t len, char* outRoundId, char* outWinnerId) {
  if (data == nullptr || len < kIdLen + kDeviceIdLen) return false;
  memcpy(outRoundId, data, kIdLen);
  outRoundId[kIdLen - 1] = '\0';
  memcpy(outWinnerId, data + kIdLen, kDeviceIdLen);
  outWinnerId[kDeviceIdLen - 1] = '\0';
  return true;
}

// Fix Phase 1C (corrected): publishBinary()'s bool is discarded here for
// every caller, unchanged from before this candidate -- an earlier revision
// of this candidate added a guarded/retried special case for the
// "race/round" retained clear in onLocalSolve(); that mechanism has been
// REMOVED (a delayed, unconditional retained clear can overwrite a newer
// round a PEER has since published -- a roundId-bearing payload does not
// fully solve this either, since the clear publish still overwrites the
// broker's retained value for any later subscriber regardless of whether
// currently-online peers reject it locally) and is not reintroduced here.
//
// This means the following are OPEN, UNRESOLVED issues, not accepted
// limitations and not "safe" by design:
//  - publishStartRound()'s retained "race/round" publish (via this
//    function) can silently fail to reach the broker if not connected,
//    leaving a newcomer with no retained round to adopt.
//  - onLocalSolve()'s retained clear (below, NOT via this function -- it
//    calls MqttManager::publishRaw() directly, unconditionally, exactly as
//    before this candidate) can equally silently fail, leaving a *finished*
//    round's payload retained indefinitely; a later newcomer's
//    handleRaceRoundPacket() has no way to distinguish that from a real
//    still-active round.
// Neither failure mode is new, and neither is fixed by this candidate.
//
// Does staged MQTT setup make either of these WORSE? Every call through
// this function (and onLocalSolve()'s direct publishRaw() call) still gates
// purely on transport (MqttManager::isGroupConnected(), unchanged in
// meaning and evaluated the same way pre- and post-candidate) -- staged
// setup does not add a new failure trigger to whether a Race publish goes
// out. The one genuine difference: under the OLD atomic connectGroupIfNeeded(),
// once a group's transport reported connected(), its 13 subscriptions had
// (by construction) already finished too, synchronously, before anything
// else could run -- under staged setup, "connected" can now be observed
// true for a stretch of wall-clock time while subscriptions are still
// incomplete. This function's own gating is unaffected either way: every
// call through it stays on isGroupConnected() only, exactly as before.
// Two of its callers (Invite, Start Round) DO depend on this device's own
// subscriptions being live to see their expected reply -- that dependency
// is not handled here, in this send-only function, but one layer up, at
// the user-action gate in handleRoomAction() (see publishInvite()'s and
// publishStartRound()'s own comments below). Every other caller through
// this function (Join, Solved, Reset) really is send-only with no such
// dependency. What CAN change here is the total wall-clock time a Race
// group's transport takes to reconnect after a drop when it is competing
// with OTHER groups for the shared one-operation-per-tick setup budget
// (see mqtt_manager.cpp's serviceTick()) -- previously, one group's ENTIRE
// atomic reconnect finished within the single tick it was picked;
// under staged setup, several simultaneously-reconnecting groups now share
// that budget round-robin, so any one of them (Race's group included) can
// take longer in aggregate to become connected again. That widens the
// EXISTING window during which a Race publish silently fails for being
// disconnected -- it does not create a new kind of failure.
//
// Race/radio integration is NOT claimed complete by this candidate.
void publishRacePacket(uint8_t kind, const uint8_t* payload, size_t payloadLen, const char* topicSuffix,
                       bool retained) {
  uint8_t wireBuf[PacketCodec::kHeaderSize + 96];
  if (payloadLen + PacketCodec::kHeaderSize > sizeof(wireBuf)) return;
  if (!PacketCodec::encodeHeader(kind, static_cast<uint16_t>(payloadLen), wireBuf, sizeof(wireBuf))) return;
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, payloadLen);
  MqttManager::publishBinary(g_groupCode, topicSuffix, wireBuf,
                             static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen), retained, 1);
}

// =============================================================================
// Lobby/round actions
// =============================================================================
void applyReset() {
  g_phase = RoomPhase::NO_LOBBY;
  g_inviteId[0] = '\0';
  g_roundId[0] = '\0';
  g_ownerDeviceId[0] = '\0';
  g_haveJoined = false;
  g_hasHadFirstRound = false;
  g_participantCount = 0;
  Notifications::setRaceInvitePending(false);
  // Fix Phase 1C (round 4 correction): applyReset() is the shared reset
  // point for leaving Race Mode (leaveRaceMode()) and for the
  // owner-went-offline reset (checkOwnerOnline()) -- a refusal notice from
  // before either of those must not survive into whatever this room's
  // state becomes next.
  clearRoomActionStatus();
  // Phase 2A: same reasoning for this round's guess history, any
  // pending "Solved!" notice, and any pending/visible Guess outcome.
  clearRaceGuessHistory();
  clearRoomSolvedNotice();
  invalidateGuessOutcome();
}

void publishReset() {
  uint8_t payload[kIdLen];
  memset(payload, 0, sizeof(payload));
  strncpy(reinterpret_cast<char*>(payload), g_inviteId, kIdLen - 1);
  publishRacePacket(PacketCodec::PK_RACE_RESET, payload, sizeof(payload), "race/reset", false);
}

// Fix Phase 1C (re-audited per review point 6): this is NOT a pure
// send-only action, and classifying it as one would be wrong -- inviting
// implicitly anticipates receiving PK_RACE_JOIN replies (race/join, one of
// the 13 subscribed topics) from whoever accepts, on THIS device's own
// subscription. If that subscription is not yet active when a peer's JOIN
// is sent (this device's group still mid-staged-setup -- a real
// possibility precisely because "just entered Race Mode and pressed
// Invite" is one of the more likely moments for a group to still be
// reconnecting, e.g. right after a WiFi drop or app cold-boot), plain MQTT
// does not retroactively deliver a message published before the matching
// subscribe() took effect -- that JOIN is lost, not delayed, and this
// device would never learn that peer tried to join.
//
// Fix Phase 1C (round 4 correction): the risk above, AT THE MOMENT OF THIS
// DEVICE'S OWN INVITE PRESS, is now closed -- handleRoomAction() (see its
// own comment) only enters this function once
// MqttManager::isGroupReady(g_groupCode) is true, meaning this device's
// race/join subscription is confirmed already active, checked
// synchronously immediately before this call with no tick boundary in
// between where it could regress. This is a real gate, not a retry: a
// press while not yet Ready is refused with a visible status (see
// handleRoomAction()), never silently discarded and never auto-resent.
//
// What remains open: once Invite succeeds and this device sits in
// LOBBY_WAITING waiting for a peer's JOIN, a LATER loss of this device's
// own subscription (e.g. a WiFi drop well after the press) is not covered
// by a one-time, press-time gate -- a JOIN arriving during that later
// window is still lost the same way described above. Fixing that would
// mean either continuously polling own-readiness for the whole waiting
// period or a retry/resend protocol on the JOIN side, both out of this
// round's scope. publishStartRound() below has the analogous dependency
// and the same residual limitation.
void publishInvite() {
  char inviteId[kIdLen];
  Identity::nextId(inviteId, sizeof(inviteId));
  strncpy(g_inviteId, inviteId, sizeof(g_inviteId) - 1);
  strncpy(g_ownerDeviceId, Identity::deviceId(), sizeof(g_ownerDeviceId) - 1);
  g_haveJoined = true;
  g_hasHadFirstRound = false;
  g_phase = RoomPhase::LOBBY_WAITING;
  g_participantCount = 0;
  addParticipant(Identity::deviceId());

  uint8_t payload[kIdLen + kDeviceIdLen];
  size_t len = buildInvitePayload(g_inviteId, Identity::deviceId(), payload, sizeof(payload));
  if (len > 0) publishRacePacket(PacketCodec::PK_RACE_INVITE, payload, len, "race/invite", false);
}

void publishJoin() {
  g_haveJoined = true;
  addParticipant(Identity::deviceId());
  uint8_t payload[kIdLen + kDeviceIdLen];
  size_t len = buildJoinPayload(g_inviteId, Identity::deviceId(), payload, sizeof(payload));
  if (len > 0) publishRacePacket(PacketCodec::PK_RACE_JOIN, payload, len, "race/join", false);
}

void generateRaceSecret(uint8_t out[4]) {
  bool used[10] = {false};
  for (int i = 0; i < 4; i++) {
    int d;
    do {
      d = static_cast<int>(random(10));
    } while (used[d]);
    used[d] = true;
    out[i] = static_cast<uint8_t>(d);
  }
}

// Fix Phase 1C (round 4 correction): same class of dependency as
// publishInvite() above -- starting a round anticipates seeing a
// PK_RACE_SOLVED from whichever participant solves it first, on this
// device's own race/solved subscription -- and the same fix applies: the
// risk AT THE MOMENT OF THIS DEVICE'S OWN Start Round press is now closed
// by handleRoomAction()'s Ready gate, checked synchronously immediately
// before this call. A press while not yet Ready is refused with a visible
// status, never silently discarded, never auto-resent. The exposure
// window was already narrower in practice than Invite's (the owner
// reaching this action already implies an earlier successful INVITE or
// JOIN on this same group, so the group is less likely to still be
// mid-setup than at the very first Invite press), and the same residual
// limitation as publishInvite() above applies: a LATER loss of this
// device's own subscription, after Start Round has already succeeded and
// while waiting for a SOLVED, is not covered by this one-time, press-time
// gate.
void publishStartRound() {
  char roundId[kIdLen];
  Identity::nextId(roundId, sizeof(roundId));
  strncpy(g_roundId, roundId, sizeof(g_roundId) - 1);
  generateRaceSecret(g_secret);
  g_hasHadFirstRound = true;
  g_phase = RoomPhase::ROUND_ACTIVE;
  // Phase 2A: a new round supersedes the previous round's history, its
  // notices, and any pending/visible Guess outcome.
  clearRaceGuessHistory();
  clearRoomSolvedNotice();
  invalidateGuessOutcome();

  uint8_t payload[kIdLen + kIdLen + kDeviceIdLen + 4];
  size_t len = buildRoundPayload(g_inviteId, g_roundId, Identity::deviceId(), g_secret, payload, sizeof(payload));
  if (len > 0) publishRacePacket(PacketCodec::PK_RACE_ROUND, payload, len, "race/round", true);
}

void publishSolved() {
  uint8_t payload[kIdLen + kDeviceIdLen];
  size_t len = buildSolvedPayload(g_roundId, Identity::deviceId(), payload, sizeof(payload));
  if (len > 0) publishRacePacket(PacketCodec::PK_RACE_SOLVED, payload, len, "race/solved", false);
}

void onLocalSolve() {
  publishSolved();
  // Reverted to the pre-candidate, unconditional form: a delayed retained
  // clear risks overwriting a newer round a peer has since published (see
  // publishRacePacket()'s comment above) -- no guard, no retry. This is a
  // known, OPEN issue: if this publish fails (not connected), the
  // *finished* round's payload can stay retained on the broker
  // indefinitely, with no mechanism in this codebase to reconcile it.
  MqttManager::publishRaw(g_groupCode, "race/round", "", true, 1);  // zero-length retained: clears it
  incrementScore(Identity::deviceId());
  strncpy(g_ownerDeviceId, Identity::deviceId(), sizeof(g_ownerDeviceId) - 1);
  g_phase = RoomPhase::LOBBY_WAITING;
  g_roundId[0] = '\0';
  // Phase 2A (D1): the round is over -- drop its history and show a
  // local-only, nonblocking acknowledgement on the Room screen (expires on
  // its own; never gates any Room action). A correct guess is 4A0B by
  // definition, so the text is fixed.
  clearRaceGuessHistory();
  g_roomSolvedNoticeActive = true;
  g_roomSolvedNoticeUntilMs = millis() + 1500;
  Menu::goBack();  // back to Room
}

// =============================================================================
// Room voice (Phase 4 section 17): MQTT broadcast transport, scoped by
// invite_id, gated on actually viewing this Room right now.
// =============================================================================
uint16_t g_roomVoiceSeq = 0;
bool g_roomPttActive = false;

void onRoomMicFrame(const int16_t* samples, size_t count) {
  RadioTransport::publishAudioPacket(g_groupCode, "radio/audio/broadcast", RadioTransport::AUDIO_RACE_ROOM,
                                     g_inviteId, g_roomVoiceSeq++, samples, count);
}

void startRoomPtt() {
  if (g_roomPttActive || g_phase == RoomPhase::NO_LOBBY) return;
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
  if (strcmp(roomKey, g_inviteId) != 0) return;       // not this room
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

void exitGuessConfirmYes() { Menu::goBack(); }

// Phase 2A (D2): a guess may only be evaluated against a live round.
bool isRoundActive() { return g_phase == RoomPhase::ROUND_ACTIVE && g_roundId[0] != '\0'; }

// Phase 2A: binds the guess history and the in-progress digit entry to the
// current round. When the round differs from the one the history belongs
// to, both are discarded, so neither a previous round's results nor its
// half-typed digits can carry over into (or be submitted against) the new
// round. Same round -> no-op (history survives Room <-> Guess re-entry).
// Returns true if anything was reset.
bool syncGuessRound() {
  if (strcmp(g_raceHistoryRoundId, g_roundId) == 0) return false;
  clearRaceGuessHistory();
  strncpy(g_raceHistoryRoundId, g_roundId, sizeof(g_raceHistoryRoundId) - 1);
  g_raceHistoryRoundId[sizeof(g_raceHistoryRoundId) - 1] = '\0';
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
NumberGuessing::DigitRowRenderState g_raceGuessRowState;
uint16_t g_raceGuessLastHistoryVersion = 0;

// Phase 2A: drops every pending AND visible Guess-screen outcome ("Solved by
// X" pending or shown, "Round ended" shown, its deadline) plus any not-yet-
// consumed auto-enter request. Called at every transition that supersedes
// the round those belonged to: reset/leave (applyReset()), group change,
// and local or remote new round. Forces a full Guess redraw so a notice
// that was on screen is replaced by the entry layout on the next tick.
void invalidateGuessOutcome() {
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
void screenRaceGuess() {
  if (Menu::consumeJustEntered()) {
    NumberGuessing::resetDigitEntry(&g_digitEntry);
    g_raceGuessDirty = true;
    g_raceGuessNeedsFullRedraw = true;
    NumberGuessing::resetDigitRowRenderState(&g_raceGuessRowState);
  }

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

  // Phase 2A (D2): the round this screen was opened for is gone (RESET,
  // owner-offline reset, or otherwise no longer ROUND_ACTIVE) with no
  // external-solve notice to show instead -- stop accepting guesses for it.
  // Reuses the timed-notice path above: input freezes, then back to Room.
  if (g_guessResultText == nullptr && !isRoundActive()) {
    clearRaceGuessHistory();
    g_guessResultText = "Round ended";
    g_guessResultUntilMs = millis() + 1500;
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
    hadEvent = true;
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (g_guessResultText != nullptr) continue;  // freeze input while showing the outcome
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (g_digitEntry.count == 4) {
        if (!isRoundActive()) continue;  // D2: never evaluate/score/publish a stale round
        NumberGuessing::GuessResult r = NumberGuessing::evaluate(g_secret, g_digitEntry.digits);
        if (r.a == 4) {
          onLocalSolve();
          return;
        }
        appendRaceGuess(g_digitEntry.digits, r);
        NumberGuessing::resetDigitEntry(&g_digitEntry);
      } else {
        ConfirmPromptConfig cfg{"Exit this game?", "Guess needs 4 digits", false, exitGuessConfirmYes, nullptr};
        Menu::startConfirmPrompt(cfg);
        Menu::pushScreen(Menu::confirmPromptScreen);
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
  bool forceFull = g_raceGuessNeedsFullRedraw || phaseChanged;

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
    bool historyChanged = forceFull || (g_raceHistoryVersion != g_raceGuessLastHistoryVersion);
    if (forceFull) {
      Display::clearContentArea();
      NumberGuessing::resetDigitRowRenderState(&g_raceGuessRowState);
      g_raceGuessNeedsFullRedraw = false;
    } else if (historyChanged) {
      int16_t regionH = static_cast<int16_t>(guessY - contentTop);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, contentTop, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    if (historyChanged) {
      int16_t availableHistoryHeight = static_cast<int16_t>(guessY - contentTop);
      uint8_t maxRows = (availableHistoryHeight > 0) ? static_cast<uint8_t>(availableHistoryHeight / lh) : 0;
      uint8_t shown = (g_raceHistoryCount < maxRows) ? g_raceHistoryCount : maxRows;
      uint8_t startIdx = static_cast<uint8_t>(g_raceHistoryCount - shown);
      for (uint8_t i = startIdx; i < g_raceHistoryCount; i++) {
        uint8_t rowIndex = static_cast<uint8_t>(i - startIdx);
        int16_t rowY = static_cast<int16_t>(guessY - (shown - rowIndex) * lh);
        const RaceGuessEntry& g = g_raceHistory[i];
        NumberGuessing::drawGuessHistoryRow(2, rowY, kGuessLabel, g.guessValue, g.a, g.b);
      }
      g_raceGuessLastHistoryVersion = g_raceHistoryVersion;
    }
    NumberGuessing::renderDigitRow(&g_raceGuessRowState, 2, guessY, kGuessLabel, g_digitEntry);
  }

  g_raceGuessLastShowingResult = showingResult;
}

// =============================================================================
// Room screen
// =============================================================================
// Hardware Fix #4.3 issue D audit: a one-device Invite press was reported as
// "appearing to do nothing" on hardware. Traced end-to-end -- publishInvite()
// below is entirely local/synchronous (sets g_inviteId, owner=self,
// g_haveJoined, g_phase=LOBBY_WAITING, resets and re-adds self to
// g_participants) before it ever touches the network, and
// MqttManager::publishBinary() returns immediately (false, non-blocking) when
// no broker connection is up, so a missing/slow connection cannot stall or
// skip the state transition. screenRaceRoom() also has no top-level dirty
// gate (by design, so it keeps reflecting async network/presence events --
// see the Hardware Fix #3 comment above it); its action-label, score, and
// participant rows are unconditionally recomputed and diffed every tick, and
// that render happens in the same screenRaceRoom() call as the
// ENCODER_SHORT event that triggered handleRoomAction(), so the label
// ("Invite to Play" -> "Start Round") and the new "<name> (You)" row are
// guaranteed to be current on the very next frame. No logic or redraw defect
// was found in this path. The most likely explanation for the observed
// non-responsiveness is the encoder pushbutton press itself not registering
// a clean ENCODER_SHORT edge on that hardware unit (a separate GPIO/contact
// from the CLK/DT rotary pins fixed under issue A, and not touched by that
// fix), which is indistinguishable to a user from "the feature does
// nothing" since a dropped press currently produces no visible feedback
// either way. That finding is unaffected by the round-4 gate added below:
// this function's OWN early return (not publishInvite()/publishStartRound()
// stalling) is what now visibly declines the action when not Ready, with
// its own status text -- a dropped ENCODER_SHORT edge still shows nothing
// at all, same as before.
//
// Fix Phase 1C (round 4): audited both call sites below before placing
// this gate. handleRoomAction() is the SOLE caller of publishInvite() and
// publishStartRound() (no other path, automatic or otherwise, reaches
// them -- confirmed via a whole-file caller search), and this function
// does nothing after calling either besides returning, so the gate
// belongs here, at the point where the user's action either proceeds or
// is refused -- not as a bare early return buried inside
// publishInvite()/publishStartRound() themselves, which would work
// equivalently for this caller alone but would leave the "show a status,
// preserve state" behavior disconnected from the Room screen's own UI
// state that owns it. Gating here, before either function is entered,
// means NEITHER function's local state mutation nor its network publish
// ever runs on a refusal -- there is nothing to "undo".
void handleRoomAction() {
  if (g_phase == RoomPhase::NO_LOBBY) {
    if (!MqttManager::isGroupReady(g_groupCode)) {
      g_roomActionStatusText = "Connecting, try again";
      g_roomActionStatusUntilMs = millis() + 1500;
      return;  // no publish, no phase/participant change, selection untouched
    }
    // Fix Phase 1C (round 4 correction): a still-pending refusal notice
    // from an EARLIER press must not keep showing once this fresh,
    // Ready-approved attempt is about to actually proceed -- clear it
    // before entering publishInvite(), not after, so there is no tick in
    // between where the phase has already changed but the stale notice
    // could still be selected by screenRaceRoom() (its own status check
    // runs before recomputing the phase-driven label, so an un-cleared
    // notice would keep winning even though it's no longer accurate).
    clearRoomActionStatus();
    publishInvite();
  } else if (g_phase == RoomPhase::LOBBY_WAITING) {
    if (strcmp(g_ownerDeviceId, Identity::deviceId()) == 0) {
      if (!MqttManager::isGroupReady(g_groupCode)) {
        g_roomActionStatusText = "Connecting, try again";
        g_roomActionStatusUntilMs = millis() + 1500;
        return;  // no publish, no phase change, g_roundId/g_secret untouched
      }
      clearRoomActionStatus();  // same reasoning as the Invite branch above
      publishStartRound();
    } else if (!g_haveJoined) {
      publishJoin();
    }
  } else {  // ROUND_ACTIVE
    if (!g_haveJoined) publishJoin();
    enterGuessScreen();
  }
}

void checkOwnerOnline() {
  if (g_phase != RoomPhase::LOBBY_WAITING || g_ownerDeviceId[0] == '\0') return;
  if (strcmp(g_ownerDeviceId, Identity::deviceId()) == 0) return;
  if (Presence::isContactOnline(g_groupCode, g_ownerDeviceId)) return;

  if (!g_hasHadFirstRound) {
    for (uint8_t i = 0; i < g_participantCount; i++) {
      if (strcmp(g_participants[i], g_ownerDeviceId) == 0) continue;
      if (Presence::isContactOnline(g_groupCode, g_participants[i])) {
        strncpy(g_ownerDeviceId, g_participants[i], sizeof(g_ownerDeviceId) - 1);
        return;
      }
    }
  } else {
    publishReset();
    applyReset();
  }
}

void leaveRaceMode() {
  g_isRoomScreenActive = false;
  stopRoomPtt();
  RadioTransport::setUiContext(RadioTransport::UiContext::NONE);
  if (Settings::getMuteRadioOutsideRadio()) {
    Presence::setOwnRadioAvailable(false);
    Presence::republishOwnPresenceAllGroups();
  }
  g_scoreCount = 0;  // "Reset: only when exiting Race Mode to Training Game"
  applyReset();
}

// Hardware Fix #3: this screen is driven by async network/presence events
// (owner takeover, participants joining, score changes from remote
// solves) as much as local input. Rather than one combined "anything
// changed" snapshot that redraws the whole content area, each row (title,
// action label, score, and each participant slot) is diffed and
// redrawn independently -- a score change no longer repaints the
// participant list, an online-count change no longer repaints the score,
// and a single participant's name change no longer repaints its
// neighbors (this is the Race Mode Room participant-list screen flagged
// for extra overlap/clipping review under the larger PRIMARY font).
bool g_raceRoomNeedsFullRedraw = true;
char g_raceRoomLastActionLabel[24] = {0};
char g_raceRoomLastScoreLine[32] = {0};
uint8_t g_raceRoomLastOnlineCount = 0xFF;
char g_raceRoomLastOnlineNames[4][17] = {{0}};

void screenRaceRoom() {
  if (Menu::consumeJustEntered()) {
    g_isRoomScreenActive = true;
    RadioTransport::setUiContext(RadioTransport::UiContext::RACE_ROOM);
    setRoomAvailability(true);
    g_raceRoomNeedsFullRedraw = true;
  }

  checkOwnerOnline();
  if (g_pendingAutoEnterGuess) {
    g_pendingAutoEnterGuess = false;
    enterGuessScreen();
    return;
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      leaveRaceMode();
      Menu::goBack();
      continue;
    }
    if (e.type == InputEventType::DOT_PRESS_START) {
      startRoomPtt();
    } else if (e.type == InputEventType::DOT_RELEASE) {
      stopRoomPtt();
    } else if (e.type == InputEventType::ENCODER_SHORT) {
      handleRoomAction();
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
    Display::printLine(2, titleY, "Race Room");
    g_raceRoomNeedsFullRedraw = false;
  }

  // Hardware Fix #4 issue 9: a ">" marker (not the word "Short:") shows
  // when ENCODER_SHORT currently does something local; the one non-action
  // state ("Waiting for start...") gets no marker at all, so it reads as
  // unmistakably passive. Every actionText here is a categorically
  // different single-line status (never a same-label toggle where only a
  // marker should move), so diffing the combined marker+text line as one
  // string is the right redraw granularity, not the class of bug Hardware
  // Fix #3 fixed elsewhere.
  const char* actionText;
  bool hasAction;
  // Fix Phase 1C (round 4): a refused Invite/Start Round press overrides
  // the normal action label with a visible status for a short window,
  // similar in spirit to g_guessResultText's timed display in
  // screenRaceGuess() above -- expires on its own (no early-return dirty
  // gate needed here, since this screen already recomputes/diffs
  // actionLabel every tick by design, see the Hardware Fix #3 comment
  // above screenRaceRoom()).
  //
  // Fix Phase 1C (round 4 correction): unlike g_guessResultText's plain
  // `millis() >= UntilMs` check, this uses the rollover-safe signed-
  // difference comparison (same technique as mqtt_manager.cpp's
  // hasEligibleSetupOperation()) -- a plain `<`/`>=` against millis()
  // breaks exactly at the uint32_t wraparound: if `now` is within 1500ms
  // of UINT32_MAX when the notice is set, `now + 1500` wraps to a small
  // value, and a naive `millis() < UntilMs` would then read false (looks
  // already expired) immediately, even though the notice was just set.
  // (Phase 2A: g_guessResultText's expiry now uses the same technique, via
  // beforeDeadline().)
  if (g_roomActionStatusText != nullptr && static_cast<int32_t>(millis() - g_roomActionStatusUntilMs) < 0) {
    actionText = g_roomActionStatusText;
    hasAction = false;  // not a live action right now -- no ">" marker
  } else if (g_phase == RoomPhase::NO_LOBBY) {
    clearRoomActionStatus();
    actionText = "Invite to Play";
    hasAction = true;
  } else if (g_phase == RoomPhase::LOBBY_WAITING) {
    clearRoomActionStatus();
    if (strcmp(g_ownerDeviceId, Identity::deviceId()) == 0) {
      actionText = "Start Round";
      hasAction = true;
    } else if (g_haveJoined) {
      actionText = "Waiting for start...";
      hasAction = false;
    } else {
      actionText = "Join";
      hasAction = true;
    }
  } else {
    clearRoomActionStatus();
    actionText = g_haveJoined ? "Continue" : "Join";
    hasAction = true;
  }
  char actionLabel[32];
  snprintf(actionLabel, sizeof(actionLabel), "%s%s", hasAction ? "> " : "", actionText);
  if (firstDraw || strcmp(actionLabel, g_raceRoomLastActionLabel) != 0) {
    int16_t oldW = Display::textWidth(g_raceRoomLastActionLabel);
    int16_t newW = Display::textWidth(actionLabel);
    int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
    int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
    if (eraseW > maxW) eraseW = maxW;
    if (!firstDraw) Display::tft().fillRect(2, actionY, eraseW, lh, ST77XX_BLACK);
    Display::printLine(2, actionY, actionLabel);
    strncpy(g_raceRoomLastActionLabel, actionLabel, sizeof(g_raceRoomLastActionLabel) - 1);
    g_raceRoomLastActionLabel[sizeof(g_raceRoomLastActionLabel) - 1] = '\0';
  }

  uint16_t score = getScore(Identity::deviceId());
  // Phase 2A (D1): the local "Solved!" acknowledgement rides on the score
  // row (not the action row, which stays live so "> Start Round" remains
  // visible and usable immediately) and expires on its own.
  if (g_roomSolvedNoticeActive && !beforeDeadline(millis(), g_roomSolvedNoticeUntilMs)) clearRoomSolvedNotice();
  char scoreLine[32];
  snprintf(scoreLine, sizeof(scoreLine), "Score: %u%s", score, g_roomSolvedNoticeActive ? "  Solved! 4A0B" : "");
  if (firstDraw || strcmp(scoreLine, g_raceRoomLastScoreLine) != 0) {
    int16_t oldW = Display::textWidth(g_raceRoomLastScoreLine);
    int16_t newW = Display::textWidth(scoreLine);
    int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
    int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
    if (eraseW > maxW) eraseW = maxW;
    if (!firstDraw) Display::tft().fillRect(2, scoreY, eraseW, lh, ST77XX_BLACK);
    Display::printLine(2, scoreY, scoreLine);
    strncpy(g_raceRoomLastScoreLine, scoreLine, sizeof(g_raceRoomLastScoreLine) - 1);
    g_raceRoomLastScoreLine[sizeof(g_raceRoomLastScoreLine) - 1] = '\0';
  }

  // Hardware Fix #4 issue 10: race membership comes from g_participants,
  // the authoritative Race-protocol join list (populated by
  // publishInvite()/publishJoin()/handleRaceJoinPacket(), and always
  // includes self once invited/joined) -- never from generic group-online
  // contacts, which exclude self entirely and would also show group
  // members who are online but never joined this race. Presence is used
  // only to resolve a joined device_id into a nicer display name; it never
  // decides who counts as a participant.
  //
  // Dynamic viewport: at the larger PRIMARY line height fewer rows fit
  // than the old fixed 4-row/10px layout assumed, so size the participant
  // list to whatever vertical space is actually left instead of
  // hardcoding a row count (avoids off-screen/overlapping names, item 14).
  uint8_t shown = (g_participantCount < 4) ? g_participantCount : 4;
  char shownNames[4][17];
  for (uint8_t i = 0; i < shown; i++) {
    Presence::resolveDisplayName(g_groupCode, g_participants[i], shownNames[i], sizeof(shownNames[i]));
    if (strcmp(g_participants[i], Identity::deviceId()) == 0) {
      // Mark self compactly; Display::printLine() safely truncates if the
      // combined text would otherwise overflow the row.
      char withYou[24];
      snprintf(withYou, sizeof(withYou), "%s (You)", shownNames[i]);
      strncpy(shownNames[i], withYou, sizeof(shownNames[i]) - 1);
      shownNames[i][sizeof(shownNames[i]) - 1] = '\0';
    }
  }
  int16_t remaining = Display::kScreenHeight - rowsTopY;
  uint16_t rows = (remaining > 0) ? static_cast<uint16_t>(remaining / lh) : 0;
  uint8_t visibleCount = static_cast<uint8_t>((shown < rows) ? shown : rows);

  if (firstDraw || visibleCount != g_raceRoomLastOnlineCount) {
    // The set of visible participant slots itself changed (someone
    // joined/left, or first draw): a genuine layout change, so the
    // participant region is redrawn in full -- still never the title,
    // action, or score rows above it.
    if (!firstDraw) {
      int16_t regionH = static_cast<int16_t>(Display::kScreenHeight - rowsTopY);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, rowsTopY, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    for (uint8_t i = 0; i < visibleCount; i++) {
      Display::printLine(2, static_cast<int16_t>(rowsTopY + i * lh), shownNames[i]);
      strncpy(g_raceRoomLastOnlineNames[i], shownNames[i], sizeof(g_raceRoomLastOnlineNames[i]) - 1);
      g_raceRoomLastOnlineNames[i][sizeof(g_raceRoomLastOnlineNames[i]) - 1] = '\0';
    }
    g_raceRoomLastOnlineCount = visibleCount;
  } else {
    // Same visible slot count: diff each row independently so one
    // participant's name change never repaints the others.
    for (uint8_t i = 0; i < visibleCount; i++) {
      if (strcmp(shownNames[i], g_raceRoomLastOnlineNames[i]) == 0) continue;
      int16_t rowY = static_cast<int16_t>(rowsTopY + i * lh);
      Display::tft().fillRect(0, rowY, Display::kScreenWidth, lh, ST77XX_BLACK);
      Display::printLine(2, rowY, shownNames[i]);
      strncpy(g_raceRoomLastOnlineNames[i], shownNames[i], sizeof(g_raceRoomLastOnlineNames[i]) - 1);
      g_raceRoomLastOnlineNames[i][sizeof(g_raceRoomLastOnlineNames[i]) - 1] = '\0';
    }
  }
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
// Network packet handlers
// =============================================================================
void handleRaceInvitePacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  char inviteId[kIdLen], initiator[kDeviceIdLen];
  if (!parseInvitePayload(payload, static_cast<uint16_t>(payloadLen), inviteId, initiator)) return;
  if (strcmp(initiator, Identity::deviceId()) == 0) return;  // my own echo
  if (g_phase != RoomPhase::NO_LOBBY) return;                // already in a lobby/round

  strncpy(g_groupCode, group_code, sizeof(g_groupCode) - 1);
  strncpy(g_inviteId, inviteId, sizeof(g_inviteId) - 1);
  strncpy(g_ownerDeviceId, initiator, sizeof(g_ownerDeviceId) - 1);
  g_haveJoined = false;
  g_hasHadFirstRound = false;
  g_phase = RoomPhase::LOBBY_WAITING;
  g_participantCount = 0;
  addParticipant(initiator);

  Notifications::setRaceInvitePending(true);
  playTone(1000, 150, SOUND_NOTIFICATION);
}

void handleRaceJoinPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)group_code;
  (void)topic;
  char inviteId[kIdLen], deviceId[kDeviceIdLen];
  if (!parseJoinPayload(payload, static_cast<uint16_t>(payloadLen), inviteId, deviceId)) return;
  if (strcmp(deviceId, Identity::deviceId()) == 0) return;
  if (strcmp(inviteId, g_inviteId) != 0) return;
  addParticipant(deviceId);
}

void handleRaceRoundPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  if (payloadLen == 0) return;  // the zero-length "clear" publish — handled via handleRaceSolvedPacket instead
  char inviteId[kIdLen], roundId[kIdLen], starterId[kDeviceIdLen];
  uint8_t secret[4];
  if (!parseRoundPayload(payload, static_cast<uint16_t>(payloadLen), inviteId, roundId, starterId, secret)) return;

  if (g_phase == RoomPhase::NO_LOBBY) {
    // Mid-round newcomer / post-reboot: adopt this retained round's lobby
    // even though we never saw its (non-retained) invite (section 13, 18).
    strncpy(g_groupCode, group_code, sizeof(g_groupCode) - 1);
    strncpy(g_inviteId, inviteId, sizeof(g_inviteId) - 1);
    strncpy(g_ownerDeviceId, starterId, sizeof(g_ownerDeviceId) - 1);
    g_haveJoined = false;
    addParticipant(starterId);
  } else if (strcmp(inviteId, g_inviteId) != 0) {
    return;  // a round for a different lobby
  }
  if (strcmp(roundId, g_roundId) == 0) return;  // already have this exact round (e.g. we're the starter)

  strncpy(g_roundId, roundId, sizeof(g_roundId) - 1);
  memcpy(g_secret, secret, 4);
  g_hasHadFirstRound = true;
  g_phase = RoomPhase::ROUND_ACTIVE;
  // Phase 2A: a new round supersedes the previous one's history, its
  // "Solved!" notice, and any pending or visible Guess outcome for it (a
  // stale "Solved by X" would otherwise be formatted with THIS round's
  // secret, and a visible one would freeze input and later pop the screen
  // on its old deadline). Must run before the auto-enter flag is set below.
  clearRaceGuessHistory();
  clearRoomSolvedNotice();
  invalidateGuessOutcome();
  if (g_haveJoined) g_pendingAutoEnterGuess = true;
}

void handleRaceSolvedPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)group_code;
  (void)topic;
  char roundId[kIdLen], winner[kDeviceIdLen];
  if (!parseSolvedPayload(payload, static_cast<uint16_t>(payloadLen), roundId, winner)) return;
  if (strcmp(roundId, g_roundId) != 0) return;                // stale/foreign round
  if (strcmp(winner, Identity::deviceId()) == 0) return;      // our own, already handled locally

  incrementScore(winner);
  strncpy(g_ownerDeviceId, winner, sizeof(g_ownerDeviceId) - 1);
  g_phase = RoomPhase::LOBBY_WAITING;
  g_roundId[0] = '\0';
  strncpy(g_pendingWinner, winner, sizeof(g_pendingWinner) - 1);
  g_pendingExternalSolve = true;
  clearRaceGuessHistory();  // Phase 2A: round over
}

void handleRaceResetPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)group_code;
  (void)topic;
  // publishReset() carries the sender's own g_inviteId as the entire
  // payload; every other Race packet handler validates inviteId/roundId
  // against our own room before acting (a stale/foreign event is otherwise
  // ignored per Addendum "stale Race event"). This one didn't -- a RESET
  // published by a completely different, unrelated room in the same group
  // (e.g. its owner going offline) would wipe an uninvolved device's own
  // active room. Reject anything that isn't for our current room.
  if (payloadLen < kIdLen || g_phase == RoomPhase::NO_LOBBY) return;
  char inviteId[kIdLen];
  memcpy(inviteId, payload, kIdLen);
  inviteId[kIdLen - 1] = '\0';
  if (strcmp(inviteId, g_inviteId) != 0) return;
  applyReset();
}

struct Registrar {
  Registrar() {
    registerGameSubModeHandler(GameSubModes::RACE, screenRaceEntry);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_INVITE, handleRaceInvitePacket);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_JOIN, handleRaceJoinPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_ROUND, handleRaceRoundPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_SOLVED, handleRaceSolvedPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RACE_RESET, handleRaceResetPacket);
    RadioTransport::registerAudioScopeHandler(RadioTransport::AUDIO_RACE_ROOM, handleRoomAudioScope);
  }
};
Registrar g_registrar;

}  // namespace
