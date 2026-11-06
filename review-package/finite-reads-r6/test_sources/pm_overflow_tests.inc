// Included into pm_tests.inc (round 6): the finite pending-read list. No
// read is ever written on the cursor-movement path; an accepted read is
// kept by exact identity and retried until written (or its record is
// confirmed gone); one that cannot be accepted (the list full, or no memory
// for it) leaves its message unread and is reported ("Read not saved
// (busy)"). Real store, real Notifications, real chats; Text and Enigma.
{
  const char* kBusy = "Read not saved (busy)";
  auto busyShown = [&]() { return T::visibleJoined().find(kBusy) != std::string::npos; };
  static bool failingWrites;
  auto failWrites = [](bool on) {
    failingWrites = on;
    LittleFS.failOpen = on ? [](const std::string& p) {
      return failingWrites && p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0;
    } : std::function<bool(const std::string&)>();
  };
  // Moves onto messages, one per pass, recording the UI's work in each pass.
  auto movesUp = [&](int n) {
    for (int i = 0; i < n; i++) rot(-1);
  };
  // Indices [from, to) of refs, as a vector.
  auto idx = [](int from, int to) {
    std::vector<int> v;
    for (int i = from; i < to; i++) v.push_back(i);
    return v;
  };
  auto cat = [](std::vector<int> a, const std::vector<int>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };

  g_test = "PO1 a replay burst of 40 distinct unread selections: 32 accepted (read later, exact), 8 refused and shown unread; no write or listing in the UI pass; one rewrite per background pass";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 45, kDay06 + 600);
    powerOn(); runFor(30);
    // Held while the chat loads (cold), replayed in one pass once it is ready.
    coldIndex();
    if (enigma) Enigma::navigateToChatDirect(kG, kBob);
    else TextMessage::navigateToChatDirect(kG, kBob);
    loopPass();
    bool loading = T::visibleJoined().find("Loading messages") != std::string::npos;
    // 40 moves while loading: 32 held, 8 left in the input queue; all replayed
    // in the pass the load completes.
    for (int i = 0; i < 40; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    g_uiMax = PassMax();
    int passes = 0;
    do loopPass(); while (T::visibleJoined().find("Loading messages") != std::string::npos && passes++ < 3000);
    PassMax ui = g_uiMax;
    int pendingAfter = pendingReads();
    bool unreadAfter = unreadOf(bob).size() == 45;
    bool shown = busyShown();
    CHECK(loading && pendingAfter == 32 && unreadAfter && shown,
          "%s: loading %d; right after the moves: %d pending, %zu unread, busy shown %d", kChatName[enigma], loading, pendingAfter,
          unreadOf(bob).size(), shown);
    CHECK(ui.writes == 0 && ui.renames == 0 && ui.removes == 0 && ui.dirEntries == 0 && ui.nvs == 0,
          "%s: UI passes (replay and the batch): writes %llu renames %llu removes %llu dirEntries %llu nvs %d", kChatName[enigma],
          (unsigned long long)ui.writes, (unsigned long long)ui.renames, (unsigned long long)ui.removes, (unsigned long long)ui.dirEntries, ui.nvs);
    g_svcMax = PassMax();
    bool mismatch = false;
    for (int i = 0; i < 500; i++) {
      loopPass();
      mismatch = mismatch || !countsMatch(kBob);
    }
    // moved onto: bob[44] .. bob[5]; accepted the first 32 (44..13), refused 12..5
    CHECK(unreadOf(bob) == idx(0, 13) && !mismatch && pendingReads() == 0,
          "%s: %zu unread (expected 0..12), counts ever differed from flash %d, %d still pending", kChatName[enigma], unreadOf(bob).size(), mismatch,
          pendingReads());
    CHECK(g_svcMax.renames <= 1 && g_svcMax.opens <= 8 && g_svcMax.nvs <= 1, "%s: per background pass: %llu rewrites, %llu opens, %d NVS",
          kChatName[enigma], (unsigned long long)g_svcMax.renames, (unsigned long long)g_svcMax.opens, g_svcMax.nvs);
    printf("   PO1 %s measured: UI passes max opens %llu writes %llu dirEntries %llu; background max per pass opens %llu rewrites %llu dirEntries %llu nvs %d\n",
           kChatName[enigma], (unsigned long long)ui.opens, (unsigned long long)ui.writes, (unsigned long long)ui.dirEntries,
           (unsigned long long)g_svcMax.opens, (unsigned long long)g_svcMax.renames, (unsigned long long)g_svcMax.dirEntries, g_svcMax.nvs);
    back();
  }

  g_test = "PO2 the list full from earlier passes while writes keep failing: later selections refused (unread, reported), unrelated work goes on; on recovery every accepted read is written without another visit";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 45, kDay06 + 600);
    for (int i = 0; i < 5; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, kDay06 + i * 60);
    powerOn(); runFor(30);
    failWrites(true);
    open(enigma, kBob);
    g_uiMax = PassMax();
    // A failed rewrite leaves the open chat's index stale: it reloads (a
    // stepped listing) and the moves wait for it, in order.
    movesUp(32);  // all accepted, all failing
    runFor(3);
    int pending32 = pendingReads();
    bool shownAt32 = busyShown();
    movesUp(5);   // refused
    runFor(3);
    PassMax ui = g_uiMax;
    printf("   PO2 %s measured: UI passes while writes fail: max opens %llu, dirEntries %llu (the chat's own reload), writes %llu\n", kChatName[enigma],
           (unsigned long long)ui.opens, (unsigned long long)ui.dirEntries, (unsigned long long)ui.writes);
    CHECK(pending32 == 32 && !shownAt32 && pendingReads() == 32 && busyShown() && unreadOf(bob).size() == 45,
          "%s: pending %d after 32 moves (busy shown %d); after 5 more: pending %d, busy shown %d, %zu unread", kChatName[enigma], pending32,
          shownAt32, pendingReads(), busyShown(), unreadOf(bob).size());
    CHECK(ui.renames == 0 && ui.writes == 0 && ui.dirEntries <= 45 && ui.opens <= 24 && ui.nvs == 0,  // (24: ST6's bound for a rotation pass)
          "%s: UI passes: renamed %llu, wrote %llu, listed %llu entries (at most the chat's own reload, one listing), %llu opens, %d NVS",
          kChatName[enigma], (unsigned long long)ui.renames, (unsigned long long)ui.writes, (unsigned long long)ui.dirEntries,
          (unsigned long long)ui.opens, ui.nvs);
    // Unrelated background work while every read keeps failing: Carol recounted.
    Notifications::Summary* carol = Notifications::findSummary(kG, kCarol);
    if (carol != nullptr) Notifications::markUnresolved(carol, 0);
    g_svcMax = PassMax();
    bool mismatch = false;
    for (int i = 0; i < 3000; i++) {  // 60 s
      loopPass();
      mismatch = mismatch || !countsMatch(kBob);
    }
    carol = Notifications::findSummary(kG, kCarol);
    CHECK(carol != nullptr && carol->state == Notifications::SUMMARY_COUNTED && Notifications::getUnreadCount(kG, kCarol) == 5,
          "%s: unrelated work stalled behind the failing reads (Carol state %d)", kChatName[enigma], carol ? carol->state : -1);
    CHECK(pendingReads() == 32 && unreadOf(bob).size() == 45 && !mismatch && g_svcMax.renames == 0 && g_svcMax.dirEntries <= 300 &&
              g_svcMax.opens <= 8,
          "%s: while failing: %d pending, %zu unread, mismatch %d; per background pass opens %llu dirEntries %llu", kChatName[enigma],
          pendingReads(), unreadOf(bob).size(), mismatch, (unsigned long long)g_svcMax.opens, (unsigned long long)g_svcMax.dirEntries);
    printf("   PO2 %s measured (60 s, writes failing, 32 pending): background max per pass opens %llu writes %llu dirEntries %llu nvs %d\n",
           kChatName[enigma], (unsigned long long)g_svcMax.opens, (unsigned long long)g_svcMax.writes, (unsigned long long)g_svcMax.dirEntries,
           g_svcMax.nvs);
    back();  // left the chat: the accepted reads are not the screen's
    failWrites(false);
    g_svcMax = PassMax();
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) {
      loopPass();
      mismatch = mismatch || !countsMatch(kBob);
    }
    // moved onto bob[44]..bob[8]; accepted 44..13; refused 12..8
    CHECK(unreadOf(bob) == idx(0, 13) && !mismatch && g_svcMax.renames <= 1,
          "%s: after recovery: %zu unread (expected 0..12), mismatch %d, up to %llu rewrites per pass", kChatName[enigma], unreadOf(bob).size(),
          mismatch, (unsigned long long)g_svcMax.renames);
  }

  g_test = "PO3 no memory for the pending list: the selection is refused (unread, reported), no write; once memory returns, a selection is accepted and read";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 3, kDay06 + 600);
    powerOn(); runFor(30);
    open(enigma, kBob);
    T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    FsStats before = g_fs;
    g_failNothrowNew = true;  // only for this pass's UI: the list cannot be allocated
    T::now += 20;
    Menu::tick();
    g_failNothrowNew = false;
    uint64_t uiRenames = g_fs.renames - before.renames, uiWrites = g_fs.writes - before.writes;
    bool refused = pendingReads() == 0 && busyShown();
    runFor(5);
    CHECK(refused && isUnread(bob[2]) && Notifications::getUnreadCount(kG, kBob) == 3 && uiRenames == 0 && uiWrites == 0,
          "%s: refused %d (pending %d, busy shown %d); bob[2] unread %d; count %u", kChatName[enigma], refused, pendingReads(), busyShown(),
          isUnread(bob[2]), Notifications::getUnreadCount(kG, kBob));
    rot(-1);  // bob[1]: memory is back
    runFor(2);
    rot(1);   // back onto bob[2]
    runFor(2);
    CHECK(!isUnread(bob[1]) && !isUnread(bob[2]) && isUnread(bob[0]) && countsMatch(kBob),
          "%s: after recovery: bob[2] %d bob[1] %d bob[0] %d", kChatName[enigma], isUnread(bob[2]), isUnread(bob[1]), isUnread(bob[0]));
    back();
  }

  g_test = "PO4 Back while the pending-read list and both input queues are full leaves at once; nothing more is read; the accepted reads are written after recovery";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 40, kDay06 + 600);
    std::vector<MessageRef> all;
    for (int i = 0; i < 40; i++) all.push_back(storeText(MessageStore::kEveryone, kBob, true, true, kDay06 + 600 + i * 60, "E"));
    powerOn(); runFor(60);
    failWrites(true);
    open(enigma, kBob);
    movesUp(32);
    runFor(3);
    back();
    int full = pendingReads();
    coldIndex();
    LittleFS.onFileOpen = [](const std::string&) { T::now += 63; };
    T::inputCap = 32;
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();
    for (int i = 0; i < 32; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass();  // held: 32
    for (int i = 0; i < 32; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));  // the queue full; the Back displaces one
    FsStats a = g_fs;
    loopPass();
    bool left = !TextMessage::isAnyConversationOpen();
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    LittleFS.onFileOpen = nullptr;
    T::inputCap = 0;
    CHECK(full == 32 && left && opens <= 6 && pendingReads() == 32 && unreadOf(all).size() == 40,
          "%s: pending list %d; left in that pass %d (%llu opens); pending %d; Everyone unread %zu", kChatName[enigma], full, left,
          (unsigned long long)opens, pendingReads(), unreadOf(all).size());
    if (TextMessage::isAnyConversationOpen()) back();
    failWrites(false);
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(unreadOf(bob) == idx(0, 8) && unreadOf(all).size() == 40 && countsMatch(kBob) && countsMatch(MessageStore::kEveryone),
          "%s: after recovery: Bob %zu unread (expected 0..7), Everyone %zu unread", kChatName[enigma], unreadOf(bob).size(), unreadOf(all).size());
    loopPass(3);
  }

  g_test = "PO5 moves in two conversations while writes fail, then leaving: exactly the messages moved onto are read after recovery, in each conversation";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 5, kDay06 + 600);
    std::vector<MessageRef> carol = unreadRun(kCarol, enigma, 5, kDay06 + 600);
    powerOn(); runFor(30);
    failWrites(true);
    open(enigma, kBob);
    movesUp(2);  // bob[4], bob[3]
    runFor(3);
    back();
    open(enigma, kCarol);
    movesUp(1);  // carol[4]
    runFor(3);
    back();
    runFor(5);
    bool none = unreadOf(bob).size() == 5 && unreadOf(carol).size() == 5 && pendingReads() == 3;
    failWrites(false);
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(none && unreadOf(bob) == idx(0, 3) && unreadOf(carol) == idx(0, 4) && countsMatch(kBob) && countsMatch(kCarol),
          "%s: before recovery nothing read %d; Bob %zu unread (expected 0..2), Carol %zu (expected 0..3)", kChatName[enigma], none,
          unreadOf(bob).size(), unreadOf(carol).size());
  }

  g_test = "PO6 absence must be confirmed: a deleted record is dropped only after two complete listings without it; an unreadable record, a listing cut short once, or no memory for the listing keep the read pending";
  {
    // (a) deleted
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    static std::string target;
    static int attempts;
    char p[160];
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[2].sequence));
    target = p;
    failWrites(true);
    openText(kBob);
    rot(-1);
    loopPass(2);  // first rewrite failed: retried later
    back();
    failWrites(false);
    MessageStore::deleteMessage(bob[2]);  // gone, Notifications not told
    attempts = 0;
    LittleFS.failOpen = [](const std::string& q) { attempts += q == target; return false; };  // counts open attempts only
    int pendingAfterFirst = -1;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) {
      loopPass();
      if (attempts == 1 && pendingAfterFirst < 0) pendingAfterFirst = pendingReads();
    }
    LittleFS.failOpen = nullptr;
    runFor(60);
    CHECK(pendingAfterFirst == 1 && attempts == 2 && pendingReads() == 0 && countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 2,
          "deleted: pending after the first absent listing %d (expected 1), read attempts %d (expected 2), pending now %d, count %u", pendingAfterFirst,
          attempts, pendingReads(), Notifications::getUnreadCount(kG, kBob));

    // (b) unreadable for a minute, then readable: read without another visit
    freshDevice();
    bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[2].sequence));
    target = p;
    openText(kBob);
    rot(-1);
    coldIndex();  // nothing cached: the record is read from flash (and fails)
    LittleFS.failOpen = [](const std::string& q) { return q == target; };
    back();
    runFor(60);
    bool stillPending = pendingReads() == 1;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(stillPending && !isUnread(bob[2]) && countsMatch(kBob), "unreadable: pending while unreadable %d; read after %d", stillPending,
          !isUnread(bob[2]));

    // (c) a listing cut short once (a readdir error looks like the end): not taken as absence
    freshDevice();
    bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[2].sequence));
    target = p;
    static int cuts;
    cuts = 0;
    openText(kBob);
    rot(-1);
    coldIndex();
    LittleFS.failOpen = [](const std::string& q) { return q == target; };
    LittleFS.endListing = [](const std::string& d, size_t) { return d.find(kBob) != std::string::npos && cuts++ < 1; };
    back();
    runFor(30);
    bool keptAfterCut = pendingReads() == 1 && cuts >= 1;
    LittleFS.endListing = nullptr;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(keptAfterCut && !isUnread(bob[2]) && countsMatch(kBob), "cut listing: kept %d (cuts %d); read after %d", keptAfterCut, cuts,
          !isUnread(bob[2]));

    // (c2) absent, then shown, then absent again (the read's listing cut short
    // on its attempts 1 and 3): two absent listings that are not consecutive
    // do not confirm absence. No outside change is signalled (that would start
    // a recount listing the same directory): the record leaves the record
    // cache (10 slots) by reading 12 others.
    freshDevice();
    bob = unreadRun(kBob, false, 13, kDay06 + 600);
    powerOn(); runFor(30);
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[12].sequence));
    target = p;
    static int readAttempts;
    static uint32_t lastFail;
    readAttempts = 0;
    lastFail = 0;
    failWrites(true);  // the first attempt fails on its write: the read stays pending
    openText(kBob);
    rot(-1);
    loopPass(2);
    back();             // left: no chat reload lists the directory from here on
    failWrites(false);
    for (int i = 0; i < 12; i++) {
      StoredMessageView v;
      MessageStore::loadMessage(bob[i], &v);
    }
    LittleFS.failOpen = [](const std::string& q) {
      if (q == target && T::now != lastFail) {
        readAttempts++;
        lastFail = T::now;
      }
      return q == target;
    };
    LittleFS.endListing = [](const std::string& d, size_t pos) {  // the listing in the same step as the failed read
      return d.find(kBob) != std::string::npos && pos == 0 && T::now == lastFail && (readAttempts == 1 || readAttempts == 3);
    };
    runFor(60);
    bool keptAfterTwoCuts = pendingReads() == 1 && readAttempts >= 4;
    LittleFS.endListing = nullptr;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(keptAfterTwoCuts && !isUnread(bob[12]) && countsMatch(kBob), "absent, present, absent: kept %d (read attempts %d); read after %d",
          keptAfterTwoCuts, readAttempts, !isUnread(bob[12]));

    // (d) no memory for the listing: kept
    freshDevice();
    bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[2].sequence));
    target = p;
    openText(kBob);
    rot(-1);
    coldIndex();
    LittleFS.failOpen = [](const std::string& q) { return q == target; };
    g_failNothrowArrayNew = true;
    back();
    runFor(30);
    g_failNothrowArrayNew = false;
    bool keptNoMem = pendingReads() == 1;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(keptNoMem && !isUnread(bob[2]) && countsMatch(kBob), "no memory for the listing: kept %d; read after %d", keptNoMem, !isUnread(bob[2]));

    // (f) the conversation directory cannot be opened (no listing at all): kept
    freshDevice();
    bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kBob, static_cast<unsigned long>(bob[2].sequence));
    target = p;
    openText(kBob);
    rot(-1);
    coldIndex();
    LittleFS.failOpen = [](const std::string& q) {
      return q == target || q == target.substr(0, target.rfind('/'));  // the record, and its directory
    };
    back();
    runFor(30);
    bool keptNoDir = pendingReads() == 1;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(keptNoDir && !isUnread(bob[2]) && countsMatch(kBob), "no directory listing: kept %d; read after %d", keptNoDir, !isUnread(bob[2]));

    // (e) an incomplete (truncated) listing: more records than a listing holds
    // (300); the pending record is outside the lowest 300 returned. Requested
    // through the production API (the chat's index holds the lowest 300 too).
    freshDevice();
    for (int i = 0; i < 300; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, false, kDay06 + i * 60);
    MessageRef last = storeText(kCarol, kCarol, true, true, kDay06 + 400 * 60, "E");  // sequence 301
    powerOn();
    StoredMessageView v;
    MessageStore::loadMessage(last, &v);
    coldIndex();  // then nothing cached
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, kCarol, static_cast<unsigned long>(last.sequence));
    target = p;
    attempts = 0;
    LittleFS.failOpen = [](const std::string& q) { attempts += q == target; return q == target; };
    TextMessage::setOpenConversation(kG, kCarol);  // (keeps the sweep from trimming it meanwhile)
    Notifications::readMovedOnto(kG, kCarol, last.sequence, v.envelope.message_id);
    for (int i = 0; i < 400 && attempts < 3; i++) loopPass();
    bool keptTruncated = pendingReads() == 1 && attempts >= 3 && onFlash(kG, kCarol).size() == 301;
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    TextMessage::clearOpenConversation();
    CHECK(keptTruncated && !isUnread(last), "truncated listing: kept %d (attempts %d, %zu records); read after %d", keptTruncated, attempts,
          onFlash(kG, kCarol).size(), !isUnread(last));
  }
}
