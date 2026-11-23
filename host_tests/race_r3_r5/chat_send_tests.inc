// Asynchronous chat send (core/chat_send.h; hardware device A: CHAT_SEND
// total=1195 ms, of which the QoS1 PUBACK wait was 605 ms on the main task).
// Included from notify_test.cpp's main(): ACTUAL text_message.cpp and
// enigma.cpp against the stub store; T::asyncPublish models
// MqttManager::publishBinaryAsync(), T::completeAsync() its result delivery.
{
  auto keyE = [] { press(InputEventType::DOT_RELEASE); tick(); };
  auto keyT = [] { press(InputEventType::DOT_RELEASE, 300); tick(); };
  auto sendShort = [] { T::now += 300; press(InputEventType::ENCODER_SHORT); tick(3); };
  // ChatSend's in-flight records live in the firmware module, not in T::: a
  // test that leaves a send in flight completes it before the next reset.
  auto fresh = [] {
    while (!T::asyncPending.empty()) T::completeAsync(true);
    reset();
  };
  auto count = [](const char* what) { int n = 0; for (auto& e : T::seq) n += e == what ? 1 : 0; return n; };
  auto status = [] { return std::string(g_sendStatus ? g_sendStatus : "(none)"); };

  g_test = "AS1 Text, connected: the publish is started, nothing is stored or blocked until its result";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort();
  CHECK(T::asyncPending.size() == 1 && T::pubs.empty() && T::appendCalls == 0,
        "pending %zu sync publishes %zu appends %d", T::asyncPending.size(), T::pubs.size(), T::appendCalls);
  CHECK(!T::visible("15/11/2023"), "history shows the message before it is stored");
  CHECK(g_composeLen == 0 && status() == "Sending...", "draft len %zu status %s", static_cast<size_t>(g_composeLen),
        status().c_str());
  std::vector<uint8_t> sentWire = T::asyncPending.empty() ? std::vector<uint8_t>() : T::asyncPending.front().data;
  CHECK(!T::asyncPending.empty() && T::asyncPending.front().topic == std::string("msg/") + kBob, "topic %s",
        T::asyncPending.empty() ? "-" : T::asyncPending.front().topic.c_str());
  g_test = "AS1b the result stores the SAME wire bytes once, flags 0, and shows the same outcome wording";
  T::completeAsync(true); tick(2);
  CHECK(T::appendCalls == 1 && T::store.size() == 1, "appends %d", T::appendCalls);
  CHECK(!T::store.empty() && T::store.back().wire == sentWire && T::store.back().hdr.flags == 0 &&
            T::store.back().hdr.direction == MessageStore::Direction::SENT && !strcmp(T::store.back().ref.contact_key, kBob),
        "record differs from what was published");
  CHECK(!T::store.empty() && T::store.back().hdr.effective_sort_timestamp == T::store.back().env.timestamp,
        "sort timestamp is not the envelope's");
  CHECK(status() == "Server acknowledged", "status %s", status().c_str());
  CHECK(T::visible("15/11/2023"), "history not refreshed after the store: %s", T::visibleJoined().c_str());

  g_test = "AS2 publish failed: stored once with FLAG_PENDING_OUTBOX (the Outbox retries it), draft stays cleared";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); T::completeAsync(false); tick(2);
  CHECK(T::appendCalls == 1 && !T::store.empty() && (T::store.back().hdr.flags & MessageStore::FLAG_PENDING_OUTBOX),
        "appends %d", T::appendCalls);
  CHECK(g_composeLen == 0 && status() == "Unconfirmed: queued", "draft %zu status %s",
        static_cast<size_t>(g_composeLen), status().c_str());

  g_test = "AS3 publish AND store failed: the draft text comes back (Phase 2B-1 retention)";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); T::appendResult = false; T::completeAsync(false); tick(2);
  CHECK(!strcmp(g_composeText, "E") && g_composeLen == 1, "draft '%s'", g_composeText);
  CHECK(status() == "Unconfirmed; not saved", "status %s", status().c_str());
  g_test = "AS3b ...but never over a new draft the user started meanwhile";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); keyT(); T::now += 1000; tick(2);  // a new letter, finalized by the idle gap
  T::appendResult = false; T::completeAsync(false); tick(2);
  CHECK(!strcmp(g_composeText, "T") && g_composeLen == 1, "new draft overwritten: '%s'", g_composeText);

  g_test = "AS4 a second send while the first is in flight is refused; its draft stays; order is kept";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); keyE(); sendShort();
  CHECK(T::asyncPending.size() == 1 && T::pubs.empty() && T::appendCalls == 0 && count("nextId") == 1,
        "pending %zu sync %zu appends %d ids %d", T::asyncPending.size(), T::pubs.size(), T::appendCalls, count("nextId"));
  CHECK(!strcmp(g_composeText, "E") && status() == "Still sending", "draft '%s' status %s", g_composeText,
        status().c_str());
  T::completeAsync(true); tick(2); sendShort();
  CHECK(T::store.size() == 1 && T::asyncPending.size() == 1, "first not stored before the second started");

  g_test = "AS5 group deleted while in flight: nothing is stored into it";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); T::groupCount = 0; T::completeAsync(true); tick(2);
  CHECK(T::appendCalls == 0, "appends %d", T::appendCalls);
  CHECK(status() == "Group removed; not saved", "status %s", status().c_str());

  g_test = "AS6 asynchronous publish refused: the synchronous path runs exactly as before";
  fresh(); T::asyncPublish = false; openText();
  keyE(); sendShort();
  CHECK(T::pubs.size() == 1 && T::appendCalls == 1 && !T::store.empty() && T::store.back().hdr.flags == 0,
        "pubs %zu appends %d", T::pubs.size(), T::appendCalls);
  CHECK(status() == "Server acknowledged" && g_composeLen == 0, "status %s", status().c_str());
  g_test = "AS6b offline: no asynchronous attempt, stored for the Outbox as before";
  fresh(); T::asyncPublish = true; T::connected = false; openText();
  keyE(); sendShort();
  CHECK(T::asyncPending.empty() && T::appendCalls == 1 && !T::store.empty() &&
            (T::store.back().hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) && status() == "Offline: queued",
        "pending %zu appends %d status %s", T::asyncPending.size(), T::appendCalls, status().c_str());

  g_test = "AS7 the result arrives after leaving the chat: stored in the ORIGINAL conversation, no restore elsewhere";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); back();
  TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone); tick(3);
  T::completeAsync(true); tick(2);
  CHECK(T::store.size() == 1 && !strcmp(T::store.back().ref.contact_key, kBob), "stored under %s",
        T::store.empty() ? "-" : T::store.back().ref.contact_key);
  CHECK(status() == "(none)", "outcome shown in another conversation: %s", status().c_str());

  g_test = "AS8 Enigma: same flow; the stored record carries the same wire bytes and local payload as the sync path";
  fresh(); T::asyncPublish = false; openEnigma();
  keyE(); sendShort();
  std::vector<uint8_t> syncLocal = T::store.empty() ? std::vector<uint8_t>() : T::store.back().local;
  fresh(); T::asyncPublish = true; openEnigma();
  keyE(); sendShort();
  CHECK(T::asyncPending.size() == 1 && T::appendCalls == 0 && T::pubs.empty(), "pending %zu appends %d",
        T::asyncPending.size(), T::appendCalls);
  CHECK(!T::visible("E"), "draft not cleared or message shown before it is stored: %s", T::visibleJoined().c_str());
  std::vector<uint8_t> enigmaWire = T::asyncPending.empty() ? std::vector<uint8_t>() : T::asyncPending.front().data;
  T::completeAsync(true); tick(2);
  CHECK(T::appendCalls == 1 && !T::store.empty() && T::store.back().wire == enigmaWire &&
            T::store.back().hdr.flags == 0 && !syncLocal.empty() && T::store.back().local == syncLocal,
        "enigma record differs (appends %d)", T::appendCalls);
  CHECK(T::visible("E"), "history not refreshed after the store: %s", T::visibleJoined().c_str());
  CHECK(T::visible("Server acknowledged"), "enigma outcome not shown: %s", T::visibleJoined().c_str());
  g_test = "AS8b Enigma total failure: the draft comes back (a new Send sends it again)";
  T::appendResult = false; keyE(); sendShort(); T::completeAsync(false); tick(2);
  T::appendResult = true; sendShort();
  CHECK(T::asyncPending.size() == 1, "draft not restored: no new send (pending %zu)", T::asyncPending.size());
  g_test = "AS8c Enigma second send while in flight: refused, nothing published or stored";
  int appendsBefore = T::appendCalls;
  keyE(); sendShort();
  CHECK(T::asyncPending.size() == 1 && T::appendCalls == appendsBefore && T::visible("Still sending"),
        "pending %zu appends %d", T::asyncPending.size(), T::appendCalls);
  T::completeAsync(true); tick(2);
}
