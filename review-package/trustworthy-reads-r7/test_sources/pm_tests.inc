// Included into storage_test.cpp's main() (round 5): per-message read in the
// Text and Enigma chats, date headings in their histories, and the old
// "readup" NVS record. Real store, real Notifications, real chats.
{
  // ---- helpers -------------------------------------------------------------
  auto markers = []() {
    int n = 0;
    for (auto& d : T::screen) n += d.text == "*";
    return n;
  };
  auto shownY = [](const std::string& s) -> int {
    for (auto& d : T::screen)
      if (d.text == s) return d.y;
    return -1;
  };
  auto shownCount = [](const std::string& s) {
    int n = 0;
    for (auto& d : T::screen) n += d.text == s;
    return n;
  };
  auto cursorY = []() -> int {
    for (auto& d : T::screen)
      if (d.text == "#CURSOR") return d.y;
    return -1;
  };
  // Whether a "*" is drawn on screen row y.
  auto markerOnRow = [](int y) {
    for (auto& d : T::screen)
      if (d.text == "*" && d.y == y) return true;
    return false;
  };
  auto countsMatch = [](const char* contact) {
    TypeCount t = truth(contact);
    return Notifications::getUnreadCount(kG, contact) == t.text + t.enigma + t.game;
  };
  // The unread ones of `refs`, as indices into it.
  auto unreadOf = [](const std::vector<MessageRef>& refs) {
    std::vector<int> v;
    for (size_t i = 0; i < refs.size(); i++)
      if (isUnread(refs[i])) v.push_back(static_cast<int>(i));
    return v;
  };
  auto range = [](int from, int to) {  // [from, to)
    std::vector<int> v;
    for (int i = from; i < to; i++) v.push_back(i);
    return v;
  };
  auto open = [&](bool enigma, const char* contact) {
    if (enigma) openEnigma(contact);
    else openText(contact);
  };
  auto ofType = [](bool enigma) { return enigma ? PacketCodec::MSG_TYPE_ENIGMA : PacketCodec::MSG_TYPE_TEXT; };
  // n received unread messages of a type, one minute apart from ts0 (Text: "E").
  auto unreadRun = [&](const char* contact, bool enigma, int n, uint32_t ts0) {
    std::vector<MessageRef> refs;
    for (int i = 0; i < n; i++) {
      if (enigma) refs.push_back(storeOnly(contact, PacketCodec::MSG_TYPE_ENIGMA, true, ts0 + i * 60));
      else refs.push_back(storeText(contact, contact, true, true, ts0 + i * 60, "E"));
    }
    return refs;
  };
  const char* kChatName[2] = {"Text", "Enigma"};
  // A received message as the radio delivers it: through the ACTUAL
  // PK_MESSAGE dispatcher and the type's own arrival handler (Text's or
  // Enigma's), which tell Notifications whether its chat is open.
  auto arriveLive = [&](const char* contact, uint8_t type, uint32_t ts) {
    PacketCodec::MessageEnvelope env = envOf(contact, type, ts);
    // An id never used before in this run: the store's duplicate filter (RAM)
    // outlives freshDevice(), which restarts envOf()'s numbering.
    static uint32_t liveNo = 0;
    snprintf(env.message_id, sizeof(env.message_id), "LIVE%019u", ++liveNo);
    uint8_t payload[96];
    size_t pn = PacketCodec::encodeTextPayload("E", payload, sizeof(payload));
    uint8_t wire[256];
    size_t n = PacketCodec::encodeMessagePacket(env, payload, static_cast<uint16_t>(pn), wire, sizeof(wire));
    PacketCodec::Header h;
    const uint8_t* body = nullptr;
    PacketCodec::decodeHeader(wire, n, &h, &body);
    handleIncomingMessagePacket(kG, "direct", body, h.payload_length);
    MessageRef ref{};
    MessageStore::findMessageById(kG, contact, env.message_id, &ref);
    return ref;
  };
  constexpr uint32_t kDay06 = 1793898000u;  // 06/11/2026 00:00:00 in Thailand (UTC+7) = 2026-11-05T17:00:00Z
  // First rows of received messages on screen (their sender name, "Bob ...").
  auto firstRows = []() {
    int n = 0;
    for (auto& d : T::screen) n += d.text.rfind("Bob", 0) == 0 && d.color != 0xFFFF;  // the name (coloured), not its divider
    return n;
  };
  // The screen as drawn (texts, positions, cursor): unchanged by a move that
  // did nothing.
  auto screenKey = []() {
    std::string k;
    for (auto& d : T::screen) k += d.text + "@" + std::to_string(d.x) + "," + std::to_string(d.y) + "|";
    return k;
  };

  g_test = "PM1 entering, rendering, a cold stepped load, a cached re-entry and a parked restore read nothing; each shown unread message has one marker";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 6, kDay06 + 600);
    storeText(kCarol, kCarol, true, false, kDay06 + 100, "T");
    powerOn(); runFor(30);
    coldIndex();
    open(enigma, kBob); runFor(5);
    int shownMarkers = markers(), shownFirst = firstRows();
    CHECK(unreadOf(bob) == range(0, 6) && Notifications::getUnreadCount(kG, kBob) == 6, "%s: cold entry read something (%zu unread, count %u)",
          kChatName[enigma], unreadOf(bob).size(), Notifications::getUnreadCount(kG, kBob));
    CHECK(shownMarkers >= 2 && shownMarkers == shownFirst, "%s: %d markers for the %d unread messages whose first row is shown", kChatName[enigma],
          shownMarkers, shownFirst);
    back();
    open(enigma, kBob); runFor(5);  // cached
    CHECK(unreadOf(bob) == range(0, 6) && markers() == shownMarkers, "%s: cached re-entry read something", kChatName[enigma]);
    back();
    open(enigma, kCarol); runFor(2); back();
    open(enigma, kBob); runFor(5);  // parked index swapped back
    CHECK(unreadOf(bob) == range(0, 6) && Notifications::getUnreadCount(kG, kBob) == 6 && (enigma ? enigmaBadge() : textBadge()),
          "%s: parked restore read something", kChatName[enigma]);
    back();
  }

  g_test = "PM1b restoring a selection is not a move: the selected message deleted, the selection falls on an unread neighbour, which stays unread";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 6, kDay06 + 600);
    powerOn(); runFor(30);
    open(enigma, kBob);
    rot(-1); rot(-1); rot(-1);  // bob[5], bob[4], bob[3] read
    runFor(2);
    MessageStore::updateLocalFlags(bob[4], MessageStore::FLAG_UNREAD, 0);  // bob[4] unread again (as if never read)
    Notifications::onMessageArrived(kG, kBob, bob[4], enigma ? Notifications::BADGE_ENIGMA : Notifications::BADGE_TEXT, false);
    MessageStore::deleteMessage(bob[3]);  // the selected message: the selection keeps its row, now bob[4]
    runFor(5);
    CHECK(isUnread(bob[4]) && countsMatch(kBob), "%s: the neighbour the selection fell on was read (unread %d)", kChatName[enigma], isUnread(bob[4]));
    back();
  }

  g_test = "PM2 moving onto one unread message reads exactly that one; the others on screen keep their markers";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 6, kDay06 + 600);
    powerOn(); runFor(30);
    open(enigma, kBob);
    int before = markers();
    rot(-1);  // the newest
    int selY = cursorY();
    runFor(1);
    CHECK(unreadOf(bob) == range(0, 5), "%s: unread after one move: %zu (expected the 5 older)", kChatName[enigma], unreadOf(bob).size());
    CHECK(markers() == before - 1 && selY >= 0 && !markerOnRow(selY), "%s: markers %d -> %d; the read one still marked %d", kChatName[enigma],
          before, markers(), markerOnRow(selY));
    CHECK(Notifications::getUnreadCount(kG, kBob) == 5 && countsMatch(kBob), "%s: count %u", kChatName[enigma],
          Notifications::getUnreadCount(kG, kBob));
    back();
  }

  g_test = "PM3 forward and reverse moves, a batch of queued moves and moves held while loading read exactly the messages moved onto";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 10, kDay06 + 600);
    powerOn(); runFor(30);
    open(enigma, kBob);
    rot(-1); rot(-1); rot(-1);  // 9, 8, 7
    rot(1);                     // back to 8 (already read)
    runFor(1);
    CHECK(unreadOf(bob) == range(0, 7), "%s: after 3 up and 1 down: %zu unread", kChatName[enigma], unreadOf(bob).size());
    rot(-1);  // 7 again
    for (int i = 0; i < 3; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass();  // one batch: 6, 5, 4
    runFor(2);
    CHECK(unreadOf(bob) == range(0, 4), "%s: after a batch of 3: %zu unread (expected 0..3)", kChatName[enigma], unreadOf(bob).size());
    back();
    // Held while the chat loads (cold, device-cost model), replayed in order.
    coldIndex();
    LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
    if (enigma) Enigma::navigateToChatDirect(kG, kBob);
    else TextMessage::navigateToChatDirect(kG, kBob);
    loopPass();
    bool loading = T::visibleJoined().find("Loading messages") != std::string::npos;
    for (int i = 0; i < 7; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));  // 9..4 (read), 3
    loopPass();
    for (int i = 0; i < 3000 && T::visibleJoined().find("Loading messages") != std::string::npos; i++) loopPass();
    runFor(2);
    LittleFS.onFileOpen = nullptr;
    CHECK(loading && unreadOf(bob) == range(0, 3), "%s: loading %d; after 7 held moves: %zu unread (expected 0..2)", kChatName[enigma], loading,
          unreadOf(bob).size());
    back();
  }

  g_test = "PM4 isolation: conversations, and the other type in the same history; a sent message gets no marker and no write";
  {
    freshDevice();
    MessageRef t0 = storeText(kBob, kBob, true, true, kDay06 + 100, "E");
    MessageRef e1 = storeOnly(kBob, PacketCodec::MSG_TYPE_ENIGMA, true, kDay06 + 200);
    MessageRef t2 = storeText(kBob, kBob, true, true, kDay06 + 300, "E");
    MessageRef s3 = storeText(kBob, "DEADBEEFAAAA", false, false, kDay06 + 400, "T");
    MessageRef c0 = storeText(kCarol, kCarol, true, true, kDay06 + 100, "E");
    MessageRef v0 = storeText(MessageStore::kEveryone, kCarol, true, true, kDay06 + 100, "E");
    powerOn(); runFor(30);
    openText(kBob);
    int m0 = markers();
    FsStats a = g_fs;
    rot(-1);  // s3, sent
    int sentY = cursorY();
    runFor(1);
    CHECK(g_fs.renames == a.renames && g_fs.writes == a.writes && sentY >= 0 && !markerOnRow(sentY) && markers() == m0,
          "a sent message: %llu renames, marker on its row %d", (unsigned long long)(g_fs.renames - a.renames), markerOnRow(sentY));
    rot(-1);  // t2
    runFor(1);
    CHECK(!isUnread(t2) && isUnread(t0) && isUnread(e1) && isUnread(c0) && isUnread(v0),
          "Text chat, t2: t2 %d t0 %d e1 %d carol %d everyone %d", isUnread(t2), isUnread(t0), isUnread(e1), isUnread(c0), isUnread(v0));
    CHECK(textBadge() && enigmaBadge(), "badges: text %d enigma %d", textBadge(), enigmaBadge());
    back();
    openEnigma(kBob);
    rot(-1); rot(-1); rot(-1);  // s3, t2 (read), e1
    runFor(1);
    CHECK(!isUnread(e1) && isUnread(t0) && isUnread(c0) && !enigmaBadge() && textBadge(),
          "Enigma chat, e1: e1 %d t0 %d carol %d; enigma badge %d text badge %d", isUnread(e1), isUnread(t0), isUnread(c0), enigmaBadge(),
          textBadge());
    CHECK(countsMatch(kBob) && countsMatch(kCarol) && countsMatch(MessageStore::kEveryone), "counts differ from flash");
    back();
    (void)s3;
  }

  g_test = "PM5 the badge stays lit until the last qualifying unread message is read";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 3, kDay06 + 600);
    powerOn(); runFor(30);
    auto badge = [&]() { return enigma ? enigmaBadge() : textBadge(); };
    open(enigma, kBob);
    rot(-1); runFor(1);
    bool lit1 = badge();
    rot(-1); runFor(1);
    bool lit2 = badge();
    rot(-1); runFor(1);
    bool lit3 = badge();
    CHECK(lit1 && lit2 && !lit3 && unreadOf(bob).empty() && Notifications::getUnreadCount(kG, kBob) == 0,
          "%s: badge after 1, 2, 3 reads: %d %d %d", kChatName[enigma], lit1, lit2, lit3);
    back();
  }

  g_test = "PM5b a read while the conversation is UNRESOLVED keeps the badge lit while another unread message remains";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 2, kDay06 + 600);
    powerOn(); runFor(30);
    auto badge = [&]() { return enigma ? enigmaBadge() : textBadge(); };
    open(enigma, kBob);
    // UNRESOLVED (as after an unreadable record or an eviction): its count is a
    // category flag until recounted -- and the read is applied before that.
    Notifications::Summary* sum = Notifications::findSummary(kG, kBob);
    if (sum != nullptr) Notifications::markUnresolved(sum, 0);
    bool unresolved = sum != nullptr && sum->state == Notifications::SUMMARY_UNRESOLVED;
    rot(-1);
    bool alwaysLit = true;
    for (int i = 0; i < 3000; i++) {
      loopPass();
      alwaysLit = alwaysLit && badge();
    }
    CHECK(unresolved && alwaysLit && !isUnread(bob[1]) && isUnread(bob[0]) && Notifications::getUnreadCount(kG, kBob) == 1,
          "%s: unresolved at the read %d; badge always lit %d; count %u", kChatName[enigma], unresolved, alwaysLit,
          Notifications::getUnreadCount(kG, kBob));
    back();
  }

  g_test = "PM11 a read is applied promptly while long background work runs (the boot sweep over 8 x 300 records)";
  {
    freshDevice();
    char contact[16];
    for (int c = 0; c < 8; c++) {
      snprintf(contact, sizeof(contact), "C%011d", c);
      for (int i = 0; i < 300; i++) storeOnly(contact, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
    }
    std::vector<MessageRef> bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(5);
    openText(kBob);
    bool sweeping = Notifications::g_sweep != nullptr;
    rot(-1);
    loopPass(50);  // 1 s
    CHECK(sweeping && Notifications::g_sweep != nullptr && !isUnread(bob[2]),
          "sweep running %d (still %d); the read applied within 1 s %d", sweeping, Notifications::g_sweep != nullptr, !isUnread(bob[2]));
    back();
  }

  g_test = "PM6 a failing rewrite leaves the message unread (marker, count, badge) and is retried; reboot before the rewrite loses the read, after it keeps it";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 3, kDay06 + 600);
    powerOn(); runFor(30);
    static bool failing;
    failing = true;
    LittleFS.failOpen = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
    open(enigma, kBob);
    rot(-1);
    int selY = cursorY();
    bool everMismatch = false;
    for (int i = 0; i < 500; i++) {  // 10 s of failing writes
      loopPass();
      everMismatch = everMismatch || !countsMatch(kBob);
    }
    CHECK(isUnread(bob[2]) && markerOnRow(selY) && Notifications::getUnreadCount(kG, kBob) == 3 && (enigma ? enigmaBadge() : textBadge()),
          "%s: while failing: unread %d, marker %d, count %u", kChatName[enigma], isUnread(bob[2]), markerOnRow(selY),
          Notifications::getUnreadCount(kG, kBob));
    // The explicit DOT read of the same message, writes still failing: not counted as read.
    T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, T::now));
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 60, T::now + 60));
    loopPass(2);
    CHECK(isUnread(bob[2]) && Notifications::getUnreadCount(kG, kBob) == 3 && countsMatch(kBob),
          "%s: explicit DOT read with writes failing: unread %d, count %u", kChatName[enigma], isUnread(bob[2]),
          Notifications::getUnreadCount(kG, kBob));
    failing = false;
    for (int i = 0; i < 4000 && isUnread(bob[2]); i++) {
      loopPass();
      everMismatch = everMismatch || !countsMatch(kBob);
    }
    runFor(1);
    CHECK(unreadOf(bob) == range(0, 2) && !markerOnRow(selY) && !everMismatch && Notifications::getUnreadCount(kG, kBob) == 2,
          "%s: after writes recover: %zu unread, marker %d, count ever differed from flash %d", kChatName[enigma], unreadOf(bob).size(),
          markerOnRow(selY), everMismatch);
    LittleFS.failOpen = nullptr;
    // Power lost right after the move, before the background rewrite.
    rot(-1);  // bob[1]: the move is handled; its rewrite is the next pass's work
    reboot(); TextMessage::clearOpenConversation(); runFor(30);  // power lost: the chat's RAM too
    CHECK(isUnread(bob[1]) && countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 2,
          "%s: reboot before the rewrite: unread %d count %u", kChatName[enigma], isUnread(bob[1]), Notifications::getUnreadCount(kG, kBob));
    // After the rewrite: kept.
    open(enigma, kBob);
    rot(-1); rot(-1);  // bob[2] (read), bob[1]
    loopPass(2);
    reboot(); TextMessage::clearOpenConversation(); runFor(30);
    CHECK(!isUnread(bob[1]) && isUnread(bob[0]) && countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 1,
          "%s: reboot after the rewrite: bob[1] unread %d, bob[0] unread %d, count %u", kChatName[enigma], isUnread(bob[1]),
          isUnread(bob[0]), Notifications::getUnreadCount(kG, kBob));
  }

  g_test = "PM7 an arrival in the open chat is not read: unread on flash, counted, marked; the selection does not follow it";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    storeText(kBob, kBob, true, false, kDay06 + 100, "T");
    powerOn(); runFor(30);
    open(enigma, kBob);
    rot(-1);  // the history is focused on the (read) message
    int m0 = markers();
    MessageRef late = arriveLive(kBob, ofType(enigma), kDay06 + 900);
    runFor(2);
    CHECK(isUnread(late) && Notifications::getUnreadCount(kG, kBob) == 1 && (enigma ? enigmaBadge() : textBadge()) && markers() == m0 + 1,
          "%s: arrival: unread %d count %u markers %d -> %d", kChatName[enigma], isUnread(late), Notifications::getUnreadCount(kG, kBob), m0,
          markers());
    back();
  }

  g_test = "PM8 input retained while loading: only the kept moves read; moves the full queue dropped read nothing; Back with both queues full reads nothing";
  for (int enigma = 0; enigma < 2; enigma++) {
    for (int variant = 0; variant < 2; variant++) {
      freshDevice();
      std::vector<MessageRef> refs;
      for (int i = 0; i < 40; i++)
        refs.push_back(storeText(MessageStore::kEveryone, kBob, true, true, kDay06 + 600 + i * 60, "E"));
      powerOn(); runFor(60);
      coldIndex();
      LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
      T::inputCap = 32;
      if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
      else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
      loopPass();
      // Pass 1: 28 key events and 4 moves: all held (32).
      std::vector<InputEvent> keys;
      morseEvents(std::string(30, 'E').c_str(), keys);  // 60 events
      for (int i = 0; i < 28; i++) T::raw.push_back(keys[i]);
      for (int i = 0; i < 4; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
      loopPass();
      // Pass 2: 32 more key events fill the queue; 3 moves dropped (and, variant 1, a Back).
      for (int i = 28; i < 60; i++) T::raw.push_back(keys[i]);
      for (int i = 0; i < 3; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
      if (variant == 1) T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
      loopPass();
      bool left = !TextMessage::isAnyConversationOpen();
      for (int i = 0; i < 3000 && T::visibleJoined().find("Loading messages") != std::string::npos; i++) loopPass();
      runFor(3);
      LittleFS.onFileOpen = nullptr;
      T::inputCap = 0;
      if (variant == 0) {
        CHECK(unreadOf(refs) == range(0, 36) && countsMatch(MessageStore::kEveryone),
              "%s: %zu unread (expected the 36 older: the 4 kept moves read, the 3 dropped not)", kChatName[enigma], unreadOf(refs).size());
      } else {
        CHECK(left && unreadOf(refs) == range(0, 40), "%s: Back with the queues full: left %d, %zu unread (expected 40)", kChatName[enigma], left,
              unreadOf(refs).size());
      }
      if (TextMessage::isAnyConversationOpen()) back();
      loopPass(3);
    }
  }

  g_test = "PM9 more moves in one batch (40) than the pending list holds (32): the first 32 are accepted and read in the background; the other 8 are refused, left unread and reported; no rewrite in the UI pass (round 6; was: 8 rewritten synchronously)";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 45, kDay06 + 600);
    powerOn(); runFor(30);
    open(enigma, kBob);
    g_uiMax = PassMax();
    for (int i = 0; i < 40; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass();
    uint64_t uiRenames = g_uiMax.renames, uiWrites = g_uiMax.writes, uiDir = g_uiMax.dirEntries;
    g_svcMax = PassMax();
    runFor(10);
    CHECK(unreadOf(bob) == range(0, 13) && Notifications::getUnreadCount(kG, kBob) == 13 && countsMatch(kBob),
          "%s: %zu unread after 40 moves (expected 0..12: the 8 refused and the 5 never moved onto), count %u", kChatName[enigma],
          unreadOf(bob).size(), Notifications::getUnreadCount(kG, kBob));
    CHECK(uiRenames == 0 && uiWrites == 0 && uiDir == 0, "%s: the batch pass wrote %llu, renamed %llu, listed %llu entries", kChatName[enigma],
          (unsigned long long)uiWrites, (unsigned long long)uiRenames, (unsigned long long)uiDir);
    CHECK(g_svcMax.renames <= 1 && g_svcMax.nvs <= 1, "%s: the 32 queued reads: up to %llu rewrites and %d NVS writes in one service pass",
          kChatName[enigma], (unsigned long long)g_svcMax.renames, g_svcMax.nvs);
    back();
  }

  g_test = "PM10 exact identity: a pending read whose record was replaced under the same sequence by another message does not read it";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 3, kDay06 + 600);
    powerOn(); runFor(30);
    static int tmpFailures;
    tmpFailures = 0;
    LittleFS.failOpen = [](const std::string& p) { return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0 && tmpFailures++ < 1; };
    open(enigma, kBob);
    rot(-1);      // bob[2]
    loopPass(2);  // its first rewrite fails: retried in about 2 s
    LittleFS.failOpen = nullptr;
    back();
    MessageStore::deleteMessage(bob[2]);                              // gone without Notifications seeing it
    MessageRef newcomer = arrive(kBob, ofType(enigma), kDay06 + 5000);  // takes the same sequence
    runFor(10);
    CHECK(tmpFailures == 1 && newcomer.sequence == bob[2].sequence && isUnread(newcomer),
          "%s: the newcomer (sequence %u, the read one's %u) was read: unread %d", kChatName[enigma], newcomer.sequence, bob[2].sequence,
          isUnread(newcomer));
    runFor(60);
    CHECK(countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 3, "%s: count %u (expected 3, as on flash)", kChatName[enigma],
          Notifications::getUnreadCount(kG, kBob));
  }

  g_test = "DS1 date headings: one per Thai (UTC+7) calendar day, before its first message; the UTC midnight does not split a day";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    storeText(kBob, kBob, true, false, kDay06 - 1, "E");     // 05/11/2026 23:59:59 (+7)
    storeText(kBob, kBob, true, false, kDay06, "E");         // 06/11/2026 00:00:00 (+7)
    storeText(kBob, "DEADBEEFAAAA", false, false, kDay06 + 60, "T");  // same day, sent
    storeText(kCarol, kCarol, true, false, 1793923199u, "E");  // 2026-11-05T23:59:59Z = 06/11 06:59:59 (+7)
    storeText(kCarol, kCarol, true, false, 1793923200u, "E");  // 2026-11-06T00:00:00Z = 06/11 07:00:00 (+7)
    storeText(kCarol, "DEADBEEFAAAA", false, false, 1793923260u, "T");
    powerOn(); runFor(30);
    open(enigma, kCarol);
    bool one = true;
    for (int i = 0; i < 3; i++) {  // s2, c1, c0: at every step at most one heading, never 05/11
      rot(-1);
      one = one && shownCount("06/11/2026") <= 1 && shownCount("05/11/2026") == 0;
    }
    CHECK(one && shownY("06/11/2026") == 16 && cursorY() == 16 + 23, "%s: Carol (one Thai day across the UTC midnight): 06/11 at y %d, cursor at y %d",
          kChatName[enigma], shownY("06/11/2026"), cursorY());
    back();
    open(enigma, kBob);
    rot(-1); rot(-1);  // s2, m1: its heading right above it
    int y06 = shownY("06/11/2026"), cy1 = cursorY();
    rot(-1);  // m0
    int y05 = shownY("05/11/2026"), cy0 = cursorY();
    CHECK(y06 >= 16 && y06 == cy1 - 23 && y05 == 16 && cy0 == 16 + 23 && shownCount("05/11/2026") == 1,
          "%s: Bob: 06/11 at y %d above m1 at %d; 05/11 at y %d above m0 at %d", kChatName[enigma], y06, cy1, y05, cy0);
    back();
  }

  g_test = "DS2 untrustworthy timestamps get an unknown-date heading; a wrapped message has one heading and one marker; headings are never selected";
  {
    int textMoves = -1;
    for (int enigma = 0; enigma < 2; enigma++) {
      freshDevice();
      storeText(kBob, kBob, true, false, 0, "E");          // unsynced clock
      storeText(kBob, kBob, true, false, 1000u, "E");      // 1970: not trusted
      std::string longText(24, 'T');  // "- - ...": two rows
      MessageRef wrapped = storeText(kBob, kBob, true, true, kDay06 + 60, longText.c_str());  // wraps
      T::realWrap = true;  // (reset by the next freshDevice())
      powerOn(); runFor(30);
      open(enigma, kBob);
      // On entry (bottom-anchored) its heading and both rows are shown: one
      // marker, on its first row only.
      CHECK(markers() == 1 && shownCount("06/11/2026") == 1 && markerOnRow(shownY("06/11/2026") + 23),
            "%s: the wrapped unread message: %d markers, heading x%d, marker on its first row %d", kChatName[enigma], markers(),
            shownCount("06/11/2026"), markerOnRow(shownY("06/11/2026") + 23));
      int wrappedMarkers = -1;
      // From the compose row up to the very first row: no move ever puts the
      // cursor on a heading row; a move that changes nothing ends it.
      bool onHeading = false;
      int moves = 0;
      for (int i = 0; i < 40; i++) {
        std::string k0 = screenKey();
        rot(-1);
        runFor(1);
        if (screenKey() == k0) break;
        moves++;
        int cy = cursorY();
        for (auto& d : T::screen)
          if ((d.text == "Unknown date" || d.text == "06/11/2026") && d.y == cy) onHeading = true;
        if (shownCount("06/11/2026") == 1 && wrappedMarkers < 0) wrappedMarkers = markers();  // its first row is in view
      }
      CHECK(wrappedMarkers == 0, "%s: the wrapped message (read when moved onto) shows %d markers", kChatName[enigma], wrappedMarkers);
      CHECK(!onHeading && shownCount("Unknown date") == 1 && shownY("Unknown date") == 16 && cursorY() == 16 + 23 &&
                T::visibleJoined().find("1970") == std::string::npos,
            "%s: on a heading %d after %d moves; Unknown date x%d at y %d, cursor at y %d", kChatName[enigma], onHeading, moves,
            shownCount("Unknown date"), shownY("Unknown date"), cursorY());
      if (!enigma) {
        textMoves = moves;
        CHECK(g_historyCursor == 0 && g_historyRowOffset == 0 && moves >= 4, "Text: ended at %u/%u after %d moves (the wrapped one not wrapping?)",
              g_historyCursor, g_historyRowOffset, moves);
      } else {
        CHECK(moves == textMoves, "Enigma: %d moves (Text: %d): the same rows, headings skipped", moves, textMoves);
      }
      CHECK(!isUnread(wrapped) && markers() == 0, "%s: moving onto the wrapped message did not read it (or a marker is left)", kChatName[enigma]);
      back();
    }
  }

  g_test = "DS4 a heading per message (one per day): on entry the newest message is on the bottom row; scrolling up and down keeps the cursor on screen, on a message row";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    for (int i = 0; i < 10; i++) storeText(kBob, kBob, true, false, kDay06 + i * 86400, i % 2 ? "E" : "T");
    powerOn(); runFor(30);
    open(enigma, kBob);
    int composeY = cursorY();
    // The newest day's heading and, right below it, the newest message (the
    // last history row: Enigma keeps a status row above its compose row).
    const char* lastDay = "15/11/2026";  // kDay06 + 9 days
    int headY = shownY(lastDay);
    bool newestShown = false, belowIt = false;
    for (auto& d : T::screen) {
      if (headY >= 16 && d.y == headY + 23 && d.text.find('/') == std::string::npos && d.text[0] != '#') newestShown = true;
      if (headY >= 16 && d.y > headY + 23 && d.y < composeY && !d.text.empty()) belowIt = true;
    }
    CHECK(newestShown && !belowIt, "%s: on entry: heading %s at y %d, the newest below it %d, history rows below that %d (compose at %d)",
          kChatName[enigma], lastDay, headY, newestShown, belowIt, composeY);
    bool visible = true;
    int moves = 0;
    auto cursorOk = [&]() {
      int cy = cursorY();
      bool ok = cy >= 16 && cy < composeY;
      for (auto& d : T::screen)
        if (d.y == cy && d.text.find('/') != std::string::npos) ok = false;  // a heading row
      return ok;
    };
    for (int i = 0; i < 10; i++, moves++) { rot(-1); visible = visible && cursorOk(); }
    for (int i = 0; i < 9; i++, moves++) { rot(1); visible = visible && cursorOk(); }
    CHECK(visible, "%s: the cursor left the screen (or sat on a heading) during %d moves", kChatName[enigma], moves);
    back();
  }

  g_test = "DS5 scrolling up inside a message taller than the screen (with a heading): the focused row becomes the top row; its first row shows its heading above it";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    storeText(kBob, kBob, true, false, kDay06 - 86400, "E");   // 05/11
    std::string tall(90, 'T');                                  // 06/11: several rows
    storeText(kBob, kBob, true, false, kDay06 + 60, tall.c_str());
    T::realWrap = true;
    powerOn(); runFor(30);
    open(enigma, kBob);
    std::vector<int> ys;
    for (int i = 0; i < 30; i++) {
      rot(-1);
      ys.push_back(cursorY());
      if (shownY("06/11/2026") >= 16 && cursorY() == shownY("06/11/2026") + 23) break;  // its first row, its heading above
    }
    size_t n = ys.size();
    bool reached = shownY("06/11/2026") == 16 && n >= 3;
    CHECK(reached && ys[n - 2] == 16, "%s: reached the first row %d after %zu moves; the cursor on its second row at y %d (expected 16, the top row)",
          kChatName[enigma], reached, n, n >= 2 ? ys[n - 2] : -1);
    back();
  }

  g_test = "DS2b retention cut into a day: the first remaining message still gets its day's heading";
  {
    freshDevice();
    for (int i = 0; i < 3; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, false, kDay06 - 86400 * 2 + i * 60);  // 04/11
    for (int i = 0; i < 100; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, false, kDay06 + i * 60);           // 06/11
    powerOn(); runFor(600);  // trimmed to the latest 100: the 04/11 messages are gone
    // An appended one evicts the first 06/11 message too (the cap keeps 100).
    arrive(kBob, PacketCodec::MSG_TYPE_TEXT, kDay06 + 86400 + 60);  // 07/11
    runFor(30);
    openText(kBob);
    for (int i = 0; i < 120 && !(g_historyCursor == 0 && g_historyRowOffset == 0); i++) rot(-1);
    CHECK(onFlash(kG, kBob).size() == 100 && shownCount("06/11/2026") == 1 && shownY("06/11/2026") == 16 && shownCount("04/11/2026") == 0,
          "kept %zu; at the top: 06/11 x%d at y %d, 04/11 x%d", onFlash(kG, kBob).size(), shownCount("06/11/2026"), shownY("06/11/2026"),
          shownCount("04/11/2026"));
    back();
  }

  g_test = "DS3 arrivals and eviction at the 100 cap while the chat is open: headings follow, the arrival stays unread, counts follow flash";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    for (int i = 0; i < 100; i++) storeText(kBob, kBob, true, i == 0, kDay06 + i * 60, "E");  // the oldest unread
    powerOn(); runFor(120);
    open(enigma, kBob);
    CHECK(shownCount("07/11/2026") == 0, "%s: precondition", kChatName[enigma]);
    MessageRef late = arriveLive(kBob, ofType(enigma), kDay06 + 86400 + 30);  // 07/11, evicts the unread oldest
    runFor(3);
    CHECK(shownCount("07/11/2026") == 1 && isUnread(late) && onFlash(kG, kBob).size() == 100, "%s: 07/11 heading x%d; arrival unread %d; kept %zu",
          kChatName[enigma], shownCount("07/11/2026"), isUnread(late), onFlash(kG, kBob).size());
    runFor(60);
    CHECK(countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 1, "%s: count %u", kChatName[enigma],
          Notifications::getUnreadCount(kG, kBob));
    back();
  }

  g_test = "PM12 an old pending read-up record is discarded unapplied (erased once), never reinterpreted; already-read messages stay read";
  for (int variant = 0; variant < 2; variant++) {
    freshDevice();
    std::vector<MessageRef> refs;
    for (int i = 0; i < 10; i++) refs.push_back(storeText(kBob, kBob, true, i >= 5, kDay06 + i * 60, "E"));  // 0..4 read already
    // The earlier firmware's record (notifications.cpp at 511d13e): a done
    // read-up up to refs[4] and a pending one up to refs[7].
    struct OldReadUp {
      char group_code[33];
      char contact_key[MessageStore::kContactKeyLen];
      uint8_t type;
      uint8_t done;
      uint32_t sortTs;
      char messageId[PacketCodec::kMessageIdLen];
      uint32_t sequence;
      uint8_t failures;
      uint32_t retryAtMs;
    };
    struct OldReadUpRecord {
      uint8_t count;
      OldReadUp items[8];
    };
    static_assert(sizeof(OldReadUp) == 92 && sizeof(OldReadUpRecord) == 740, "layout as stored by 511d13e");
    OldReadUpRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.count = 2;
    int upTo[2] = {4, 7};
    for (int k = 0; k < 2; k++) {
      StoredMessageView v;
      MessageStore::loadMessage(refs[upTo[k]], &v);
      OldReadUp& r = rec.items[k];
      strncpy(r.group_code, kG, sizeof(r.group_code) - 1);
      strncpy(r.contact_key, kBob, sizeof(r.contact_key) - 1);
      r.type = PacketCodec::MSG_TYPE_TEXT;
      r.done = k == 0;
      r.sortTs = v.header.effective_sort_timestamp;
      memcpy(r.messageId, v.envelope.message_id, sizeof(r.messageId));
      r.sequence = refs[upTo[k]].sequence;
    }
    g_notifyPrefs.putBytes("readup", &rec, sizeof(rec));
    if (variant == 1) g_notifyPrefs.failRemove = [](const char*) { return true; };
    powerOn(); runFor(60);
    CHECK(unreadOf(refs) == range(5, 10) && Notifications::getUnreadCount(kG, kBob) == 5 && countsMatch(kBob) && textBadge(),
          "variant %d: %zu unread (expected 5..9, nothing applied), count %u", variant, unreadOf(refs).size(),
          Notifications::getUnreadCount(kG, kBob));
    CHECK(g_notifyPrefs.isKey("readup") == (variant == 1), "variant %d: the old record %s", variant,
          g_notifyPrefs.isKey("readup") ? "is still stored" : "was erased");
    if (variant == 1) {
      g_notifyPrefs.failRemove = nullptr;
      reboot(); runFor(30);
      CHECK(!g_notifyPrefs.isKey("readup") && unreadOf(refs) == range(5, 10), "a later boot: erased %d, %zu unread", !g_notifyPrefs.isKey("readup"),
            unreadOf(refs).size());
    }
    openText(kBob);
    CHECK(markers() == 4, "variant %d: %d markers on the 4 shown unread messages", variant, markers());
    back();
  }

#include "pm_overflow_tests.inc"
#include "pm_uncertain_tests.inc"

  // ---- measurements (printed, not checked): device-cost model, 63 ms per open ----
  {
    auto measure = [&](const char* what, std::function<void()> act) {
      FsStats a = g_fs;
      int na = g_notifyPrefs.writes;
      g_svcMax = PassMax(); g_uiMax = PassMax();
      uint32_t t0 = T::now;
      act();
      printf("MEASURE %-34s opens %4llu writes %3llu renames %3llu removes %3llu dirEntries %4llu nvs %3d | per pass max: ui opens %llu writes %llu renames %llu; services opens %llu writes %llu renames %llu nvs %d | model %u ms\n",
             what, (unsigned long long)(g_fs.fileOpens - a.fileOpens), (unsigned long long)(g_fs.writes - a.writes),
             (unsigned long long)(g_fs.renames - a.renames), (unsigned long long)(g_fs.removes - a.removes),
             (unsigned long long)(g_fs.dirEntries - a.dirEntries), g_notifyPrefs.writes - na, (unsigned long long)g_uiMax.opens,
             (unsigned long long)g_uiMax.writes, (unsigned long long)g_uiMax.renames, (unsigned long long)g_svcMax.opens,
             (unsigned long long)g_svcMax.writes, (unsigned long long)g_svcMax.renames, g_svcMax.nvs, T::now - t0);
    };
    for (int enigma = 0; enigma < 2; enigma++) {
      freshDevice();
      for (int i = 0; i < 90; i++) {
        bool received = i % 3 != 0;
        storeText(MessageStore::kEveryone, received ? kBob : "DEADBEEFAAAA", received, received && i >= 50, kDay06 + i * 600, "E");
      }
      powerOn(); runFor(120);
      LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
      printf("MEASURE -- %s chat, Everyone: 90 messages (27 received unread among the newest 40), several days\n", kChatName[enigma]);
      coldIndex();
      measure("cold entry (until drawn)", [&]() { open(enigma, MessageStore::kEveryone); });
      back();
      measure("cached re-entry (until drawn)", [&]() { open(enigma, MessageStore::kEveryone); });
      back();
      // scrolling: the same history with every message read
      LittleFS.onFileOpen = nullptr;
      freshDevice();
      for (int i = 0; i < 90; i++) {
        bool received = i % 3 != 0;
        storeText(MessageStore::kEveryone, received ? kBob : "DEADBEEFAAAA", received, false, kDay06 + i * 600, "E");
      }
      powerOn(); runFor(120);
      open(enigma, MessageStore::kEveryone);
      LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
      measure("scroll: 40 single moves, all read", [&]() {
        for (int i = 0; i < 40; i++) rot(-1);
      });
      back();
      // reading: fresh flags again
      LittleFS.onFileOpen = nullptr;
      freshDevice();
      for (int i = 0; i < 90; i++) {
        bool received = i % 3 != 0;
        storeText(MessageStore::kEveryone, received ? kBob : "DEADBEEFAAAA", received, received && i >= 50, kDay06 + i * 600, "E");
      }
      powerOn(); runFor(120);
      open(enigma, MessageStore::kEveryone);
      LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
      measure("read: 20 moves over the newest (14 unread), then 10 s", [&]() {
        for (int i = 0; i < 20; i++) rot(-1);
        runFor(10);
      });
      printf("MEASURE   flash: %d unread left of 27 (count %u)\n", truth(MessageStore::kEveryone).text,
             Notifications::getUnreadCount(kG, MessageStore::kEveryone));
      LittleFS.onFileOpen = nullptr;
      back();
    }
  }
}
