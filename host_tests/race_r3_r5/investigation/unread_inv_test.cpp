// INVESTIGATION REPRO (host only, not production; not part of run_all.sh).
// Defect 3: the unread "*" stays after opening a received message.
// Compiles the ACTUAL src/core/notifications.cpp and src/core/text_message.cpp
// into this translation unit, with the ACTUAL menu.cpp. Build with the shared
// stubs minus their Notifications stub (setup in investigation/README.md).
#include "stubs.h"
#include "core/menu.cpp"
#include "core/notifications.cpp"
#include "core/text_message.cpp"
#include <cstdio>
#include <string>

// ---- the few MessageStore/Storage hooks notifications.cpp needs (fake store) ----
namespace MessageStore {
void updateLocalFlags(const MessageRef& ref, uint16_t setMask, uint16_t clearMask) {
  for (auto& r : T::store)
    if (r.ref.sequence == ref.sequence && !strcmp(r.ref.group_code, ref.group_code) && !strcmp(r.ref.contact_key, ref.contact_key))
      r.hdr.flags = static_cast<uint16_t>((r.hdr.flags | setMask) & ~clearMask);
}
void setOnEvictedCallback(EvictedCallback) {}
}  // namespace MessageStore
namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static Preferences g_notifyPrefs;
namespace Storage { Preferences& notify() { return g_notifyPrefs; } }

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)
static void tick(int n = 1) { for (int i = 0; i < n; i++) { Menu::tick(); T::now += 5; } }
static void press(InputEventType t, uint32_t dur = 80) { T::raw.push_back(InputEvent(t, 0, dur, T::now)); }
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
void T_initServices();

static const char* kG = "GROUP1";
static const char* kBob = "B0B000000001";
static MessageRef arriveText(const char* id, const char* text) {
  PacketCodec::MessageEnvelope env{};
  strncpy(env.message_id, id, sizeof(env.message_id) - 1);
  env.schema_version = 1; env.message_type = PacketCodec::MSG_TYPE_TEXT;
  strncpy(env.sender_device_id, kBob, sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, "Bob", sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, kG, sizeof(env.group_code) - 1);
  env.timestamp = 1700000100u;
  handleTextArrival(kG, kBob, env, env.timestamp, reinterpret_cast<const uint8_t*>(text), static_cast<uint16_t>(strlen(text)));
  return T::store.back().ref;
}
static bool unreadFlag(const MessageRef& ref) {
  for (auto& r : T::store) if (r.ref.sequence == ref.sequence) return (r.hdr.flags & MessageStore::FLAG_UNREAD) != 0;
  return false;
}
static void mainMenuStub() {}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  T::reset();
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  T_initServices();  // notifications.cpp serviceInit(): loads summaries, registers the Main Menu badges
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));

  g_test = "N1 a Text message to a closed conversation lights the Text badge";
  MessageRef m = arriveText("MSG00000000000000000001", "HELLO");
  CHECK(Notifications::hasAnyUnreadText() && unreadFlag(m), "badge %d flag %d", Notifications::hasAnyUnreadText(), unreadFlag(m));

  g_test = "N2 open the chat, move the cursor onto the message, press Encoder Short, go Back: the badge stays";
  Menu::pushScreen(mainMenuStub);
  TextMessage::navigateToChatDirect(kG, kBob); tick(3);
  printf("  N2: chat open=%d unread=%d badge=%d\n", TextMessage::isConversationOpen(kG, kBob), unreadFlag(m),
         Notifications::hasAnyUnreadText());
  rot(-1); tick(2);
  printf("  N2: cursor on index %u (message landed on): unread=%d badge=%d\n", g_historyCursor, unreadFlag(m),
         Notifications::hasAnyUnreadText());
  press(InputEventType::ENCODER_SHORT); tick(2);
  printf("  N2: after Short: chat open=%d, cursor=%u, unread=%d badge=%d\n", TextMessage::isConversationOpen(kG, kBob),
         g_historyCursor, unreadFlag(m), Notifications::hasAnyUnreadText());
  T::now += 1000; press(InputEventType::ENCODER_LONG); tick(3);
  printf("  N2: after Short and Back: chat still open=%d, stack size %u, unread=%d badge=%d\n",
         TextMessage::isConversationOpen(kG, kBob), g_stackSize, unreadFlag(m), Notifications::hasAnyUnreadText());
  CHECK(!TextMessage::isConversationOpen(kG, kBob), "Back left the chat");
  CHECK(Notifications::hasAnyUnreadText() && unreadFlag(m), "CURRENT BEHAVIOUR: still unread after viewing it");

  g_test = "N3 the only read path: DOT press/release while that message is focused";
  TextMessage::navigateToChatDirect(kG, kBob); tick(3);
  rot(-1); tick(2);
  press(InputEventType::DOT_PRESS_START); tick(); press(InputEventType::DOT_RELEASE); tick(2);
  CHECK(!Notifications::hasAnyUnreadText() && !unreadFlag(m), "DOT on the line clears: badge %d flag %d",
        Notifications::hasAnyUnreadText(), unreadFlag(m));
  press(InputEventType::ENCODER_LONG); tick(2);

  g_test = "N4 one Friend result (badge mask TEXT|ENIGMA, as number_guessing_friend.cpp passes) lights BOTH badges";
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  MessageRef c = arriveText("MSG00000000000000000002", "CHALLENGE STAND-IN");
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  Notifications::onMessageArrived(kG, kBob, c, Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA, false);
  CHECK(Notifications::hasAnyUnreadText() && Notifications::hasAnyUnreadEnigma(), "text %d enigma %d",
        Notifications::hasAnyUnreadText(), Notifications::hasAnyUnreadEnigma());

  g_test = "N5 the per-conversation summary merges categories: reading the Text message leaves BOTH badges while an Enigma one is unread";
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  for (auto& r : T::store) r.hdr.flags = 0;
  MessageRef t1 = arriveText("MSG00000000000000000003", "TEXT ONE");
  MessageRef e1 = arriveText("MSG00000000000000000004", "ENIGMA STAND-IN");
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  Notifications::onMessageArrived(kG, kBob, t1, Notifications::BADGE_TEXT, false);
  Notifications::onMessageArrived(kG, kBob, e1, Notifications::BADGE_ENIGMA, false);
  Notifications::clearUnread(kG, kBob, t1);  // the Text message is read
  printf("  N5: after reading the Text message: text badge=%d enigma badge=%d unread_count=%u\n",
         Notifications::hasAnyUnreadText(), Notifications::hasAnyUnreadEnigma(), Notifications::getUnreadCount(kG, kBob));
  CHECK(Notifications::hasAnyUnreadText(), "CURRENT BEHAVIOUR: the Text badge stays (summary mask still has TEXT, count 1)");

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
