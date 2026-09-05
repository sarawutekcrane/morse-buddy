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
}

void publishReset() {
  uint8_t payload[kIdLen];
  memset(payload, 0, sizeof(payload));
  strncpy(reinterpret_cast<char*>(payload), g_inviteId, kIdLen - 1);
  publishRacePacket(PacketCodec::PK_RACE_RESET, payload, sizeof(payload), "race/reset", false);
}

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

void publishStartRound() {
  char roundId[kIdLen];
  Identity::nextId(roundId, sizeof(roundId));
  strncpy(g_roundId, roundId, sizeof(g_roundId) - 1);
  generateRaceSecret(g_secret);
  g_hasHadFirstRound = true;
  g_phase = RoomPhase::ROUND_ACTIVE;

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
  MqttManager::publishRaw(g_groupCode, "race/round", "", true, 1);  // zero-length retained: clears it
  incrementScore(Identity::deviceId());
  strncpy(g_ownerDeviceId, Identity::deviceId(), sizeof(g_ownerDeviceId) - 1);
  g_phase = RoomPhase::LOBBY_WAITING;
  g_roundId[0] = '\0';
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

void enterGuessScreen() {
  NumberGuessing::resetDigitEntry(&g_digitEntry);
  g_guessResultText = nullptr;
  g_isRoomScreenActive = false;
  stopRoomPtt();
  RadioTransport::setUiContext(RadioTransport::UiContext::NONE);
  if (Settings::getMuteRadioOutsideRadio()) {
    Presence::setOwnRadioAvailable(false);
    Presence::republishOwnPresenceAllGroups();
  }
  Menu::pushScreen(screenRaceGuess);
}

void screenRaceGuess() {
  if (Menu::consumeJustEntered()) NumberGuessing::resetDigitEntry(&g_digitEntry);

  if (g_pendingExternalSolve) {
    static char buf[40];
    snprintf(buf, sizeof(buf), "Solved by %s: %d%d%d%d", g_pendingWinner, g_secret[0], g_secret[1], g_secret[2],
             g_secret[3]);
    g_guessResultText = buf;
    g_guessResultUntilMs = millis() + 1500;
    g_pendingExternalSolve = false;
  }

  if (g_guessResultText != nullptr && millis() >= g_guessResultUntilMs) {
    g_guessResultText = nullptr;
    Menu::goBack();
    return;
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (g_guessResultText != nullptr) continue;  // freeze input while showing the outcome
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (g_digitEntry.count == 4) {
        NumberGuessing::GuessResult r = NumberGuessing::evaluate(g_secret, g_digitEntry.digits);
        if (r.a == 4) {
          onLocalSolve();
          return;
        }
        NumberGuessing::resetDigitEntry(&g_digitEntry);
      } else {
        ConfirmPromptConfig cfg{"Exit this game?", nullptr, false, exitGuessConfirmYes, nullptr};
        Menu::startConfirmPrompt(cfg);
        Menu::pushScreen(Menu::confirmPromptScreen);
      }
    } else {
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
    }
  }
  NumberGuessing::tickDigitEntry(&g_digitEntry);

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  if (g_guessResultText != nullptr) {
    tft.setCursor(2, Display::kStatusBarHeight + 20);
    tft.print(g_guessResultText);
    return;
  }

  char guessLine[16] = "____";
  for (uint8_t i = 0; i < g_digitEntry.count; i++) guessLine[i] = static_cast<char>('0' + g_digitEntry.digits[i]);
  if (g_digitEntry.count < 4) guessLine[g_digitEntry.count] = static_cast<char>('0' + g_digitEntry.previewDigit);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print("Guess: ");
  tft.print(guessLine);
}

// =============================================================================
// Room screen
// =============================================================================
void handleRoomAction() {
  if (g_phase == RoomPhase::NO_LOBBY) {
    publishInvite();
  } else if (g_phase == RoomPhase::LOBBY_WAITING) {
    if (strcmp(g_ownerDeviceId, Identity::deviceId()) == 0) {
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

void screenRaceRoom() {
  if (Menu::consumeJustEntered()) {
    g_isRoomScreenActive = true;
    RadioTransport::setUiContext(RadioTransport::UiContext::RACE_ROOM);
    setRoomAvailability(true);
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
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  const char* actionLabel;
  if (g_phase == RoomPhase::NO_LOBBY) {
    actionLabel = "Short: Invite to Play";
  } else if (g_phase == RoomPhase::LOBBY_WAITING) {
    actionLabel = (strcmp(g_ownerDeviceId, Identity::deviceId()) == 0)
                      ? "Short: Start Round"
                      : (g_haveJoined ? "Waiting for start..." : "Short: Join");
  } else {
    actionLabel = g_haveJoined ? "Short: Continue" : "Short: Join";
  }
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print("Race Room");
  tft.setCursor(2, Display::kStatusBarHeight + 16);
  tft.print(actionLabel);

  char line[40];
  snprintf(line, sizeof(line), "Score: %u", getScore(Identity::deviceId()));
  tft.setCursor(2, Display::kStatusBarHeight + 30);
  tft.print(line);

  Presence::OnlineContact online[6];
  uint8_t n = Presence::getOnlineContacts(g_groupCode, online, 6);
  int16_t y = Display::kStatusBarHeight + 44;
  for (uint8_t i = 0; i < n && i < 4; i++) {
    tft.setCursor(2, y);
    tft.print(online[i].display_name);
    y += 10;
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

void screenNoFamilyGroups() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }
  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 60);
  tft.print("No Family Groups");
  tft.setCursor(6, 76);
  tft.print("Add one in Settings");
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
}

void handleRaceResetPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)group_code;
  (void)topic;
  (void)payload;
  (void)payloadLen;
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
