// Included into pm_tests.inc (round 7): an accepted read is cancelled only by
// a trustworthy event (its record's deletion or eviction known to have
// succeeded, the record loaded and found read or holding another message);
// storage failures that cannot be told from absence keep it pending, retried
// with backoff, occupying one slot. Faults are tied to the read's own
// attempts: the target record is taken out of the record cache by reading
// other records (no outside-change signal, which would start recounts and
// other listings), and its opens are the read's.
{
  // The record on flash, read from the host file itself (never through the store).
  auto rawState = [](const char* contact, uint32_t seq, bool* unread) {
    auto f = onFlash(kG, contact);
    auto it = f.find(seq);
    if (it == f.end() || it->second.size() < 3) return false;
    uint16_t flags = static_cast<uint16_t>(static_cast<uint8_t>(it->second[1]) | (static_cast<uint8_t>(it->second[2]) << 8));
    *unread = (flags & MessageStore::FLAG_UNREAD) != 0;
    return true;
  };
  auto evictFromCache = [](const std::vector<MessageRef>& others) {
    for (auto& r : others) {
      StoredMessageView v;
      MessageStore::loadMessage(r, &v);
    }
  };
  auto pathOf = [](const char* contact, uint32_t seq) {
    char p[160];
    snprintf(p, sizeof(p), "/messages/%s/%s/%08lu.msg", kG, contact, static_cast<unsigned long>(seq));
    return std::string(p);
  };
  static std::string target, targetTmp;
  static int attempts;
  static uint32_t lastAttempt;
  static bool faulty;

  g_test = "PO7 two (and many) consecutive premature listings while the record's opens fail: the record stays on flash, unread; the read stays pending throughout and is applied on recovery without another visit";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 13, kDay06 + 600);  // target bob[12]; 12 others to push it out of the cache
    powerOn(); runFor(30);
    target = pathOf(kBob, bob[12].sequence);
    targetTmp = target + ".tmp";
    attempts = 0;
    lastAttempt = 0;
    faulty = true;
    // The first attempt fails on the rewrite (the record stays readable and
    // unread): the read is then pending, with the chat already left.
    LittleFS.failOpen = [](const std::string& q) { return faulty && q == targetTmp; };
    open(enigma, kBob);
    rot(-1);
    loopPass(2);
    back();
    std::vector<MessageRef> others(bob.begin(), bob.begin() + 12);
    evictFromCache(others);
    // From now on, every read attempt: the open of the record fails, and the
    // listing in that same step ends at once (a readdir error looks like the end).
    LittleFS.failOpen = [](const std::string& q) {
      if (faulty && q == target && T::now != lastAttempt) {
        attempts++;
        lastAttempt = T::now;
      }
      return faulty && (q == target || q == targetTmp);
    };
    LittleFS.endListing = [](const std::string& d, size_t) { return faulty && d.find(kBob) != std::string::npos && T::now == lastAttempt; };
    bool alwaysPending = true, alwaysOnFlash = true;
    int pendingAt2 = -1;
    for (int i = 0; i < 12000 && attempts < 6; i++) {  // six failed attempts (2+4+8+16+32+60 s of backoff)
      loopPass();
      bool u = false;
      alwaysPending = alwaysPending && pendingReads() == 1;
      alwaysOnFlash = alwaysOnFlash && rawState(kBob, bob[12].sequence, &u) && u;
      if (attempts == 2 && pendingAt2 < 0) pendingAt2 = pendingReads();
    }
    CHECK(attempts >= 6 && pendingAt2 == 1 && alwaysPending && alwaysOnFlash,
          "%s: %d failed attempts; pending after two consecutive empty listings %d; pending throughout %d; on flash and unread throughout %d",
          kChatName[enigma], attempts, pendingAt2, alwaysPending, alwaysOnFlash);
    faulty = false;
    LittleFS.failOpen = nullptr;
    LittleFS.endListing = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    bool u = true;
    CHECK(rawState(kBob, bob[12].sequence, &u) && !u && unreadOf(bob) == range(0, 12) && countsMatch(kBob),
          "%s: after recovery: bob[12] read on flash %d; %zu unread (expected 12)", kChatName[enigma], !u, unreadOf(bob).size());
  }

  g_test = "PO8 a known successful eviction cancels exactly its request (another stays); a failed deletion does not cancel";
  for (int variant = 0; variant < 3; variant++) {  // 0 evicted, 1 eviction fails, 2 evicted with its header unreadable
    freshDevice();
    std::vector<MessageRef> bob;
    for (int i = 0; i < 100; i++) bob.push_back(storeText(kBob, kBob, true, i == 0 || i == 50, kDay06 + i * 60, "E"));
    powerOn(); runFor(60);
    // Requests for the oldest (the next to be evicted) and for bob[50],
    // through the production API; their rewrites fail, so both stay pending.
    target = pathOf(kBob, bob[0].sequence);
    targetTmp = target + ".tmp";
    static std::string tmp50;
    tmp50 = pathOf(kBob, bob[50].sequence) + ".tmp";
    LittleFS.failOpen = [](const std::string& q) { return q == targetTmp || q == tmp50; };
    for (int k : {0, 50}) {
      StoredMessageView v;
      MessageStore::loadMessage(bob[k], &v);
      Notifications::readMovedOnto(kG, kBob, bob[k].sequence, v.envelope.message_id);
    }
    runFor(1);
    int before = pendingReads();
    if (variant == 1) LittleFS.failRemove = [](const std::string& q) { return q == target; };
    static int targetOpens;
    targetOpens = 0;
    if (variant == 2) {
      // The eviction reads the oldest record's header twice: to choose it
      // (succeeds), then in the removal itself (fails here) -- removed, its
      // flags unknown.
      evictFromCache(std::vector<MessageRef>(bob.begin() + 1, bob.begin() + 13));
      LittleFS.failOpen = [](const std::string& q) {
        if (q == target) targetOpens++;
        return q == targetTmp || q == tmp50 || (q == target && targetOpens == 2);
      };
    }
    arrive(kBob, PacketCodec::MSG_TYPE_TEXT, kDay06 + 86400);  // the 101st: evicts bob[0] (variant 1: its removal fails)
    bool u = false;
    bool stillThere = rawState(kBob, bob[0].sequence, &u);
    int after = pendingReads();
    LittleFS.failRemove = nullptr;
    if (variant == 2) CHECK(targetOpens >= 2, "variant 2: the removal's header read was not reached (%d opens)", targetOpens);
    if (variant != 1) {
      CHECK(before == 2 && !stillThere && after == 1, "variant %d evicted: %d pending before, record still on flash %d, %d pending after (expected 1)",
            variant, before, stillThere, after);
    } else {
      CHECK(before == 2 && stillThere && after == 2, "eviction failed: %d pending before, record on flash %d, %d pending after (expected 2)", before,
            stillThere, after);
    }
    LittleFS.failOpen = nullptr;
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(!isUnread(bob[50]) && pendingReads() == 0, "variant %d: bob[50] read after recovery %d", variant, !isUnread(bob[50]));
    runFor(60);
    CHECK(countsMatch(kBob), "variant %d: count %u differs from flash", variant, Notifications::getUnreadCount(kG, kBob));
  }

  g_test = "PO9 a record gone without any notification: its request stays pending (one slot), retried with backoff; other reads and other work go on";
  {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, false, 3, kDay06 + 600);
    std::vector<MessageRef> carol = unreadRun(kCarol, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    target = pathOf(kBob, bob[2].sequence);
    targetTmp = target + ".tmp";
    LittleFS.failOpen = [](const std::string& q) { return q == targetTmp; };
    openText(kBob);
    rot(-1);
    loopPass(2);
    back();
    LittleFS.failOpen = nullptr;
    // Gone from flash behind the store's back (no deletion it could report).
    std::error_code ec;
    std::filesystem::remove(LittleFS.root + target, ec);
    evictFromCache(std::vector<MessageRef>(bob.begin(), bob.begin() + 2));
    evictFromCache(carol);
    evictFromCache(unreadRun(kCarol, false, 10, kDay06 + 9000));  // (also more Carol messages, unread)
    attempts = 0;
    lastAttempt = 0;
    LittleFS.failOpen = [](const std::string& q) {
      if (q == target && T::now != lastAttempt) {
        attempts++;
        lastAttempt = T::now;
      }
      return false;
    };
    // Meanwhile: another message of the same conversation (the index still
    // lists the vanished one: moving over it is a duplicate request), reads
    // in another conversation, and a recount.
    openText(kBob);
    rot(-1); rot(-1);  // bob[2] (already pending), bob[1]
    back();
    openText(kCarol);
    rot(-1); rot(-1);
    back();
    Notifications::Summary* cs = Notifications::findSummary(kG, kCarol);
    if (cs != nullptr) Notifications::markUnresolved(cs, 0);
    g_svcMax = PassMax();
    runFor(300);
    PassMax svc = g_svcMax;
    LittleFS.failOpen = nullptr;
    cs = Notifications::findSummary(kG, kCarol);
    CHECK(pendingReads() == 1 && attempts >= 4 && attempts <= 12, "the request: %d pending (expected 1, kept), %d attempts in 300 s (backoff)",
          pendingReads(), attempts);
    CHECK(!isUnread(bob[1]) && pendingReads() == 1, "the same conversation: bob[1] read %d; %d pending (the uncertain one only)", !isUnread(bob[1]),
          pendingReads());
    CHECK(truth(kCarol).text == 11 && cs != nullptr && cs->state == Notifications::SUMMARY_COUNTED && countsMatch(kCarol),
          "other work: Carol %d unread on flash (expected 11), summary state %d", truth(kCarol).text, cs ? cs->state : -1);
    CHECK(svc.renames <= 1 && svc.dirEntries <= 300 && svc.opens <= 8, "per background pass: %llu rewrites, %llu entries listed, %llu opens",
          (unsigned long long)svc.renames, (unsigned long long)svc.dirEntries, (unsigned long long)svc.opens);
    printf("   PO9 measured (300 s, one uncertain request): %d attempts; background max per pass opens %llu writes %llu dirEntries %llu\n", attempts,
           (unsigned long long)svc.opens, (unsigned long long)svc.writes, (unsigned long long)svc.dirEntries);
  }

  g_test = "PO10 a rewrite whose rename fails after the old file was removed loses the record: the request stays pending (uncertain), the count is recounted to match flash";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 3, kDay06 + 600);
    powerOn(); runFor(30);
    target = pathOf(kBob, bob[2].sequence);
    LittleFS.failRename = [](const std::string& q) { return q == target; };
    open(enigma, kBob);
    rot(-1);
    loopPass(2);  // the rewrite: old file removed, rename failed -- the record is gone
    LittleFS.failRename = nullptr;
    back();
    bool u = false;
    bool gone = !rawState(kBob, bob[2].sequence, &u);
    runFor(120);
    TypeCount t = truth(kBob);
    CHECK(gone && pendingReads() == 1 && t.text + t.enigma == 2 && countsMatch(kBob) && Notifications::getUnreadCount(kG, kBob) == 2,
          "%s: record gone %d; %d pending (expected 1); flash %d unread, count %u", kChatName[enigma], gone, pendingReads(), t.text + t.enigma,
          Notifications::getUnreadCount(kG, kBob));
  }

  g_test = "PO11 the same message read by its explicit DOT action ends its pending request at once (exact identity); the others stay";
  {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    static int tmpFailures;
    tmpFailures = 0;
    LittleFS.failOpen = [](const std::string& q) { return q.size() > 4 && q.compare(q.size() - 4, 4, ".tmp") == 0 && tmpFailures++ < 2; };
    openText(kBob);
    rot(-1); rot(-1);  // bob[2], bob[1]: both first rewrites fail
    loopPass(3);
    LittleFS.failOpen = nullptr;
    int before = pendingReads();
    T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, T::now));  // on bob[1], selected
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 60, T::now + 60));
    T::now += 20;
    Menu::tick();  // the UI only: the DOT read, no background step
    int after = pendingReads();
    CHECK(before == 2 && after == 1 && !isUnread(bob[1]) && isUnread(bob[2]), "pending %d -> %d (expected 2 -> 1); bob[1] read %d, bob[2] unread %d",
          before, after, !isUnread(bob[1]), isUnread(bob[2]));
    back();
    for (int i = 0; i < 6000 && pendingReads() > 0; i++) loopPass();
    CHECK(!isUnread(bob[2]) && countsMatch(kBob), "bob[2] read after the retry %d", !isUnread(bob[2]));
  }

  g_test = "PO12 a pending read of a group no longer configured (deleted) ends at its next attempt; its record is not rewritten";
  {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, false, 3, kDay06 + 600);
    powerOn(); runFor(30);
    target = pathOf(kBob, bob[2].sequence);
    targetTmp = target + ".tmp";
    LittleFS.failOpen = [](const std::string& q) { return q == targetTmp; };
    openText(kBob);
    rot(-1);
    loopPass(2);
    back();
    LittleFS.failOpen = nullptr;
    int before = pendingReads();
    T::groupConfigured = false;  // as Settings::deleteGroup() leaves it
    FsStats a = g_fs;
    runFor(10);
    uint64_t renames = g_fs.renames - a.renames;
    T::groupConfigured = true;
    bool u = false;
    CHECK(before == 1 && pendingReads() == 0 && renames == 0 && rawState(kBob, bob[2].sequence, &u) && u,
          "pending %d -> %d; %llu rewrites; record unread %d", before, pendingReads(), (unsigned long long)renames, u);
  }
}
