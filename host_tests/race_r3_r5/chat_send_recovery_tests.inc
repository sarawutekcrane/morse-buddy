// Chat send crash consistency (core/chat_send.h, store first): ACTUAL
// text_message.cpp, storage_messages.cpp (host directory) and outbox.cpp.
// Included from pm_tests.inc.
{
  auto keyAndSend = [] {
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 80, T::now));
    loopPass(2);
    T::now += 300;
    T::raw.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 80, T::now));
    loopPass(3);
  };
  auto chatPublishes = [](const std::vector<uint8_t>& wire) {
    int n = 0;
    for (auto& p : T::pubs) n += (p.data == wire) ? 1 : 0;
    return n;
  };
  auto pendingOnFlash = [](const MessageRef& ref) {
    StoredMessageView v;
    return MessageStore::loadMessage(ref, &v) && (v.header.flags & MessageStore::FLAG_PENDING_OUTBOX) != 0;
  };

  g_test = "CR1 power lost while a chat message's publish is out: after the reboot the Outbox publishes the SAME stored bytes once and clears the flag";
  {
    freshDevice();
    powerOn(); runFor(5);
    T::asyncPublish = true;  // the publish starts and its result never arrives (the reset comes first)
    TextMessage::navigateToChatDirect(kG, kBob); loopPass(3);
    keyAndSend();
    bool started = T::asyncPending.size() == 1;
    std::vector<uint8_t> sent = started ? T::asyncPending.front().data : std::vector<uint8_t>();
    MessageRef ref{};
    // The record on flash: pending, with the published bytes.
    uint32_t seqs[8]; bool rel = false;
    uint16_t n = MessageStore::listMessageSequences(kG, kBob, seqs, 8, &rel, nullptr);
    if (n == 1) { strncpy(ref.group_code, kG, sizeof(ref.group_code) - 1); strncpy(ref.contact_key, kBob, sizeof(ref.contact_key) - 1); ref.sequence = seqs[0]; }
    bool durable = n == 1 && pendingOnFlash(ref);
    reboot();
    T::asyncPublish = false;  // after the reboot: the default model (published, result next service round)
    T::pubs.clear();
    runFor(15);  // the Outbox's boot pass finds the pending record
    int published = chatPublishes(sent);
    bool cleared = !pendingOnFlash(ref);
    runFor(15);
    CHECK(started && durable && published == 1 && cleared && chatPublishes(sent) == 1,
          "started %d, durable+pending before the result %d, republished after reboot %d (want 1), flag cleared %d, no repeat %d",
          started, durable, published, cleared, chatPublishes(sent));
  }

  g_test = "CR2 publish failed after leaving the chat: the record stays pending and the Outbox delivers it later (no draft needed, nothing lost)";
  {
    freshDevice();
    powerOn(); runFor(5);
    T::asyncPublish = true;
    TextMessage::navigateToChatDirect(kG, kBob); loopPass(3);
    keyAndSend();
    std::vector<uint8_t> sent = T::asyncPending.empty() ? std::vector<uint8_t>() : T::asyncPending.front().data;
    T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 500, T::now)); loopPass(3);  // leave the chat
    TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone); loopPass(3);
    T::completeAsync(false);  // no PUBACK
    T::asyncPublish = false;
    T::pubs.clear();
    runFor(12);  // the Outbox retry (kRetryMs 5 s)
    uint32_t seqs[8]; bool rel = false;
    uint16_t n = MessageStore::listMessageSequences(kG, kBob, seqs, 8, &rel, nullptr);
    MessageRef ref{};
    strncpy(ref.group_code, kG, sizeof(ref.group_code) - 1); strncpy(ref.contact_key, kBob, sizeof(ref.contact_key) - 1);
    ref.sequence = n == 1 ? seqs[0] : 0;
    CHECK(!sent.empty() && n == 1 && chatPublishes(sent) == 1 && !pendingOnFlash(ref),
          "records %u, delivered by the Outbox %d, flag cleared %d", static_cast<unsigned>(n), chatPublishes(sent), !pendingOnFlash(ref));
  }

  g_test = "CR4 the Outbox has one publish out at a time; a refused one (connection busy) is retried, not dropped";
  {
    freshDevice();
    powerOn(); runFor(5);
    T::connected = false;  // two offline sends: both pending for the Outbox
    TextMessage::navigateToChatDirect(kG, kBob); loopPass(3);
    keyAndSend();
    keyAndSend();
    T::connected = true;
    T::asyncPublish = true;
    T::asyncChainCap = 0;  // the connection's chain full: every publish refused for now
    runFor(8);
    bool refusedKept = T::asyncPending.empty();
    T::asyncChainCap = 4;
    runFor(8);  // past the Outbox retry
    size_t outAtOnce = T::asyncPending.size();
    T::completeAsync(true);
    runFor(1);
    size_t next = T::asyncPending.size();
    T::completeAsync(true);
    runFor(8);
    uint32_t seqs[8]; bool rel = false;
    uint16_t n = MessageStore::listMessageSequences(kG, kBob, seqs, 8, &rel, nullptr);
    int stillPending = 0;
    for (uint16_t i = 0; i < n; i++) {
      MessageRef ref{};
      strncpy(ref.group_code, kG, sizeof(ref.group_code) - 1); strncpy(ref.contact_key, kBob, sizeof(ref.contact_key) - 1);
      ref.sequence = seqs[i];
      stillPending += pendingOnFlash(ref) ? 1 : 0;
    }
    CHECK(refusedKept && outAtOnce == 1 && next == 1 && n == 2 && stillPending == 0,
          "nothing started while refused %d, Outbox publishes out at once %zu (want 1), then the next %zu, records %u, still pending %d",
          refusedKept, outAtOnce, next, static_cast<unsigned>(n), stillPending);
    T::asyncPublish = false;
  }

  g_test = "CR3 while a chat publish is out the Outbox does not publish the same record (no duplicate); after success it drops it";
  {
    freshDevice();
    powerOn(); runFor(5);
    T::asyncPublish = true;
    TextMessage::navigateToChatDirect(kG, kBob); loopPass(3);
    keyAndSend();
    size_t pendingNow = T::asyncPending.size();
    runFor(12);  // Outbox steps run meanwhile
    size_t stillOne = T::asyncPending.size();
    T::completeAsync(true);
    runFor(12);
    CHECK(pendingNow == 1 && stillOne == 1 && T::asyncPending.empty() && T::pubs.empty(),
          "publishes started: at send %zu, after 12 s %zu (want 1: the Outbox skipped it), after success %zu, sync %zu", pendingNow,
          stillOne, T::asyncPending.size(), T::pubs.size());
  }
}
