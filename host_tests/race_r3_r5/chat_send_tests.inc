// Chat send, store first then publish asynchronously (core/chat_send.h;
// hardware device A: CHAT_SEND total=1195 ms, of which the QoS1 PUBACK wait
// was 605 ms on the main task). Included from notify_test.cpp's main():
// ACTUAL text_message.cpp and enigma.cpp against the stub store;
// T::asyncPublish models MqttManager::publishBinaryAsync() (a per-group chain
// of T::asyncChainCap), T::completeAsync() its result delivery.
{
  auto keyE = [] { press(InputEventType::DOT_RELEASE); tick(); };
  auto keyT = [] { press(InputEventType::DOT_RELEASE, 300); tick(); };
  auto sendShort = [] { T::now += 300; press(InputEventType::ENCODER_SHORT); tick(3); };
  // ChatSend's in-flight records live in the firmware module, not in T::: a
  // test that leaves a send in flight completes it before the next reset.
  auto fresh = [] {
    T::appendResult = true;
    T::groupCount = 1;
    while (!T::asyncPending.empty()) T::completeAsync(false);
    reset();
  };
  auto status = [] { return std::string(g_sendStatus ? g_sendStatus : "(none)"); };
  auto pending = [](const T::Rec& r) { return (r.hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) != 0; };

  g_test = "AS1 Text, connected: the record is stored durably (pending) BEFORE the publish starts; nothing waits for the network";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort();
  CHECK(T::appendCalls == 1 && T::store.size() == 1 && pending(T::store.back()) && T::asyncPending.size() == 1 &&
            T::pubs.empty(),
        "appends %d pending %d async %zu sync publishes %zu", T::appendCalls, !T::store.empty() && pending(T::store.back()),
        T::asyncPending.size(), T::pubs.size());
  CHECK(!T::store.empty() && !T::asyncPending.empty() && T::asyncPending.front().data == T::store.back().wire &&
            T::asyncPending.front().topic == std::string("msg/") + kBob && !strcmp(T::store.back().ref.contact_key, kBob) &&
            T::store.back().hdr.direction == MessageStore::Direction::SENT &&
            T::store.back().hdr.effective_sort_timestamp == T::store.back().env.timestamp,
        "the published bytes differ from the stored record");
  int iApp = -1, iStart = -1;
  for (size_t i = 0; i < T::seq.size(); i++) {
    if (T::seq[i] == "append" && iApp < 0) iApp = static_cast<int>(i);
    if (T::seq[i] == "async:start" && iStart < 0) iStart = static_cast<int>(i);
  }
  CHECK(iApp >= 0 && iApp < iStart, "store (%d) not before the publish start (%d)", iApp, iStart);
  CHECK(g_composeLen == 0 && status() == "Sending..." && T::visible("15/11/2023"),
        "draft %zu status %s, history shows the stored message %d", static_cast<size_t>(g_composeLen), status().c_str(),
        T::visible("15/11/2023"));
  g_test = "AS1b success: the pending flag is cleared on the same record; nothing stored twice; the outcome wording is unchanged";
  int updatesBefore = T::updateCalls;
  T::completeAsync(true); tick(2);
  CHECK(T::store.size() == 1 && T::appendCalls == 1 && T::updateCalls == updatesBefore + 1 && !pending(T::store.back()),
        "records %zu appends %d updates %d pending %d", T::store.size(), T::appendCalls, T::updateCalls - updatesBefore,
        !T::store.empty() && pending(T::store.back()));
  CHECK(status() == "Server acknowledged", "status %s", status().c_str());

  g_test = "AS2 publish failed: the record stays pending (the Outbox retries it); no second store, no flag rewrite";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort();
  updatesBefore = T::updateCalls;
  T::completeAsync(false); tick(2);
  CHECK(T::appendCalls == 1 && T::updateCalls == updatesBefore && !T::store.empty() && pending(T::store.back()),
        "appends %d updates %d", T::appendCalls, T::updateCalls - updatesBefore);
  CHECK(status() == "Unconfirmed: queued", "status %s", status().c_str());

  g_test = "AS3 reset or power loss while the publish is out: the message is already on flash, pending (what survives a reset)";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort();
  // No result ever arrives (the device resets now): the durable state is the store.
  CHECK(T::store.size() == 1 && pending(T::store.back()) && !T::store.back().wire.empty(),
        "not durable before the result: records %zu", T::store.size());

  g_test = "AS4 two sends in a row: both stored in order and both published on the group's chain; no refusal, no draft kept";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); keyT(); sendShort();
  CHECK(T::store.size() == 2 && T::asyncPending.size() == 2 && T::store[0].ref.sequence < T::store[1].ref.sequence &&
            T::asyncPending[0].data == T::store[0].wire && T::asyncPending[1].data == T::store[1].wire && g_composeLen == 0,
        "records %zu publishes %zu draft %zu", T::store.size(), T::asyncPending.size(), static_cast<size_t>(g_composeLen));
  T::completeAsync(true); T::completeAsync(true); tick(2);
  CHECK(!pending(T::store[0]) && !pending(T::store[1]), "flags not both cleared");
  g_test = "AS4b the group's chain full: the record is stored pending and left to the Outbox ('Queued'), nothing waits";
  fresh(); T::asyncPublish = true; T::asyncChainCap = 1; openText();
  keyE(); sendShort(); keyT(); sendShort();
  CHECK(T::store.size() == 2 && T::asyncPending.size() == 1 && pending(T::store[1]) && T::pubs.empty() &&
            status() == "Queued" && g_composeLen == 0,
        "records %zu publishes %zu sync %zu status %s", T::store.size(), T::asyncPending.size(), T::pubs.size(),
        status().c_str());

  g_test = "AS5 group deleted while the publish is out: no flag rewrite into a deleted conversation, no status";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort();
  updatesBefore = T::updateCalls;
  T::groupCount = 0; T::completeAsync(true); tick(2);
  CHECK(T::updateCalls == updatesBefore && status() == "Sending...", "updates %d status %s", T::updateCalls - updatesBefore,
        status().c_str());

  g_test = "AS6 the store failed: the synchronous path decides as before -- published: draft cleared ('Server acked; not saved')";
  fresh(); T::asyncPublish = true; T::appendResult = false; openText();
  keyE(); sendShort();
  CHECK(T::asyncPending.empty() && T::pubs.size() == 1 && g_composeLen == 0 && status() == "Server acked; not saved",
        "async %zu sync %zu draft %zu status %s", T::asyncPending.size(), T::pubs.size(), static_cast<size_t>(g_composeLen),
        status().c_str());
  g_test = "AS6b the store AND the publish failed: the draft is kept (Phase 2B-1), nothing lost";
  fresh(); T::asyncPublish = true; T::appendResult = false; T::publishScript = {false}; openText();
  keyE(); sendShort();
  CHECK(!strcmp(g_composeText, "E") && status() == "Unconfirmed; not saved", "draft '%s' status %s", g_composeText,
        status().c_str());
  g_test = "AS6c offline: stored pending for the Outbox, no publish attempt";
  fresh(); T::asyncPublish = true; T::connected = false; openText();
  keyE(); sendShort();
  CHECK(T::asyncPending.empty() && T::pubs.empty() && T::store.size() == 1 && pending(T::store.back()) &&
            status() == "Offline: queued",
        "async %zu sync %zu status %s", T::asyncPending.size(), T::pubs.size(), status().c_str());

  g_test = "AS7 the result arrives after moving to another conversation and starting a draft there: the original record is the one updated; the new draft and status are untouched";
  fresh(); T::asyncPublish = true; openText();
  keyE(); sendShort(); back();
  TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone); tick(3);
  keyT(); T::now += 1000; tick(2);
  T::completeAsync(false); tick(2);
  CHECK(T::store.size() == 1 && !strcmp(T::store.back().ref.contact_key, kBob) && pending(T::store.back()),
        "record %s pending %d", T::store.empty() ? "-" : T::store.back().ref.contact_key,
        !T::store.empty() && pending(T::store.back()));
  CHECK(!strcmp(g_composeText, "T") && status() == "(none)", "draft '%s' status %s", g_composeText, status().c_str());

  g_test = "AS8 Enigma: stored first (pending, with the same local payload as an offline send), the same bytes published";
  fresh(); T::connected = false; openEnigma();
  keyE(); sendShort();
  std::vector<uint8_t> offlineLocal = T::store.empty() ? std::vector<uint8_t>() : T::store.back().local;
  fresh(); T::asyncPublish = true; openEnigma();
  keyE(); sendShort();
  CHECK(T::store.size() == 1 && pending(T::store.back()) && T::asyncPending.size() == 1 &&
            T::asyncPending.front().data == T::store.back().wire && !offlineLocal.empty() &&
            T::store.back().local == offlineLocal && T::pubs.empty(),
        "records %zu async %zu local equal %d", T::store.size(), T::asyncPending.size(),
        !T::store.empty() && T::store.back().local == offlineLocal);
  CHECK(T::visible("Sending...") && T::visible("E"), "draft not cleared into the history: %s", T::visibleJoined().c_str());
  g_test = "AS8b Enigma success: the flag cleared, outcome shown";
  T::completeAsync(true); tick(2);
  CHECK(!T::store.empty() && !pending(T::store.back()) && T::visible("Server acknowledged"), "flag %d screen %s",
        !T::store.empty() && pending(T::store.back()), T::visibleJoined().c_str());
  g_test = "AS8c Enigma store and publish failed: the draft is kept (a new Send sends it again)";
  fresh(); T::asyncPublish = true; T::appendResult = false; T::publishScript = {false}; openEnigma();
  keyE(); sendShort();
  T::appendResult = true; T::connected = true; sendShort();  // (the failed publish closed the connection; reconnected)
  CHECK(T::store.size() == 1 && T::asyncPending.size() == 1, "draft not kept: records %zu async %zu", T::store.size(),
        T::asyncPending.size());
  fresh();
}
