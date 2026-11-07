// Included into pm_tests.inc (round 8): unread markers on the Text and Enigma
// group and recipient (friend) lists. Real Menu, ListMenu, chats,
// Notifications, Presence and message store.
{
  // A star drawn on the screen row of a visible item whose text starts with `label`.
  auto rowY = [](const std::string& label) -> int {
    for (auto& d : T::screen)
      if (d.text.rfind(label, 0) == 0 && d.x > 2) return d.y;
    return -1;
  };
  auto starOnRow = [](int y) {
    for (auto& d : T::screen)
      if (y >= 0 && d.y == y && (d.text == "*" || d.text == " *")) return true;
    return false;
  };
  auto starred = [&](const std::string& label) { return starOnRow(rowY(label)); };
  auto presence = [](const char* group, const char* id, const char* name) {
    char p[120];
    snprintf(p, sizeof(p), "MBP1|%s|%s|ONLINE|0|%lu|255", id, name, (unsigned long)T::unixTime);
    Presence::handleIncoming(group, p, static_cast<uint16_t>(strlen(p)));
  };
  // A TEXT record in any group (storeText() writes GROUP1).
  auto storeTextIn = [](const char* group, const char* contact, const char* sender, bool received, bool unread, uint32_t ts) {
    PacketCodec::MessageEnvelope env = envOf(sender, PacketCodec::MSG_TYPE_TEXT, ts);
    strncpy(env.group_code, group, sizeof(env.group_code) - 1);
    uint8_t payload[32];
    size_t pn = PacketCodec::encodeTextPayload("E", payload, sizeof(payload));
    return writeLegacyRecordBytes(group, contact, received ? MessageStore::Direction::RECEIVED : MessageStore::Direction::SENT,
                                  unread ? MessageStore::FLAG_UNREAD : 0, ts, env, payload, static_cast<uint16_t>(pn));
  };
  auto storeEnigmaIn = [](const char* group, const char* contact, bool unread, uint32_t ts) {
    PacketCodec::MessageEnvelope env = envOf(contact, PacketCodec::MSG_TYPE_ENIGMA, ts);
    strncpy(env.group_code, group, sizeof(env.group_code) - 1);
    return writeLegacyRecord(group, contact, MessageStore::Direction::RECEIVED, unread ? MessageStore::FLAG_UNREAD : 0, ts, env,
                             "XQZV");
  };
  // Opens the Text or Enigma mode as the Main Menu does.
  auto openMode = [&](bool enigma) {
    Menu::pushScreen(getModeHandler(enigma ? Modes::ENIGMA : Modes::TEXT));
    loopPass(4);
  };
  auto confirm = [&]() {  // DOT tap: opens the highlighted row
    T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, T::now));
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 80, T::now + 80));
    loopPass(4);
  };
  auto toMain = [&]() {  // back to the Main Menu; no Back left queued for the next screen
    for (int i = 0; i < 6 && g_stackSize > 1; i++) back();
    loopPass(2);
    T::raw.clear();
    T::inputQ.clear();
  };
  const char* kDave = "DA7E00000001";
  // (Presence device ids are 12 characters: a longer key would name another conversation.)
  const char* kCarl = "CA2100000001";
  TypeCount none;

  g_test = "LM1 recipient rows: a star on each friend (and Everyone) with unread messages of the list's category; none on others; entering, highlighting and opening read nothing";
  {
    freshDevice();
    storeTextIn(kG, kBob, kBob, true, true, kDay06 + 60);                       // Bob: Text unread
    storeTextIn(kG, kCarl, kCarl, true, false, kDay06 + 60);                  // Carol: read only
    storeTextIn(kG, kDave, "DEADBEEFAAAA", false, false, kDay06 + 60);          // Dave: sent only
    storeEnigmaIn(kG, MessageStore::kEveryone, true, kDay06 + 120);             // Everyone: Enigma unread
    powerOn(); runFor(30);
    presence(kG, kBob, "Bob");
    presence(kG, kCarl, "Carol");
    presence(kG, kDave, "Dave");
    TypeCount bob0 = truth(kBob), all0 = truth(MessageStore::kEveryone);
    openMode(false);
    bool textBob = starred("Bob"), textCarol = starred("Carol"), textDave = starred("Dave"), textAll = starred("Everyone");
    CHECK(textBob && !textCarol && !textDave && !textAll, "Text list: Bob %d Carol %d Dave %d Everyone %d (expected 1 0 0 0)", textBob, textCarol,
          textDave, textAll);
    for (int i = 0; i < 4; i++) rot(1);  // highlight every row
    confirm();                           // opens the highlighted row's chat
    loopPass(5);
    toMain();
    openMode(true);
    bool enBob = starred("Bob"), enAll = starred("Everyone");
    CHECK(!enBob && enAll, "Enigma list: Bob %d Everyone %d (expected 0 1)", enBob, enAll);
    for (int i = 0; i < 4; i++) rot(1);
    toMain();
    TypeCount bob1 = truth(kBob), all1 = truth(MessageStore::kEveryone);
    CHECK(bob1.text == bob0.text && all1.enigma == all0.enigma && bob1.text == 1 && all1.enigma == 1,
          "entering, highlighting and opening read something: Bob %d -> %d, Everyone %d -> %d", bob0.text, bob1.text, all0.enigma, all1.enigma);
    CHECK(textBadge() == textBob && enigmaBadge() == enAll, "Main Menu badges (text %d enigma %d) disagree with the rows", textBadge(),
          enigmaBadge());
  }

  g_test = "LM2 group rows aggregate exactly their own group's conversations (Everyone included); the same contact in two groups stays apart";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    T::groupCount = 2;
    if (enigma) {
      storeEnigmaIn("GROUP2", kBob, true, kDay06 + 60);                 // Bob, in Work only: unread
      storeEnigmaIn(kG, kBob, false, kDay06 + 60);                      // Bob, in Fam: read
      storeEnigmaIn(kG, MessageStore::kEveryone, true, kDay06 + 90);    // Fam's Everyone: unread
    } else {
      storeTextIn("GROUP2", kBob, kBob, true, true, kDay06 + 60);    // Bob, in Work only: unread
      storeTextIn(kG, kBob, kBob, true, false, kDay06 + 60);         // Bob, in Fam: read
      storeTextIn(kG, MessageStore::kEveryone, kCarl, true, true, kDay06 + 90);  // Fam's Everyone: unread
    }
    powerOn(); runFor(60);
    presence(kG, kBob, "Bob");
    presence("GROUP2", kBob, "Bob");
    openMode(enigma);  // two groups: the group list
    bool fam = starred("Fam"), work = starred("Work");
    CHECK(fam && work, "%s: group list: Fam %d (its Everyone) Work %d (its Bob)", kChatName[enigma], fam, work);
    confirm();  // Fam
    bool famBob = starred("Bob"), famAll = starred("Everyone");
    back();
    loopPass(2);
    openMode(enigma);
    rot(1);  // Work
    confirm();
    bool workBob = starred("Bob"), workAll = starred("Everyone");
    CHECK(!famBob && famAll && workBob && !workAll, "%s: Fam: Bob %d Everyone %d; Work: Bob %d Everyone %d (expected 0 1 1 0)", kChatName[enigma], famBob, famAll, workBob,
          workAll);
    toMain();
    // reading Fam's Everyone clears Fam's group star, not Work's
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    waitChatDrawn();
    rot(-1);
    runFor(2);
    toMain();
    openMode(enigma);
    bool fam2 = starred("Fam"), work2 = starred("Work");
    CHECK(!fam2 && work2, "%s: after reading Fam's Everyone: Fam %d Work %d (expected 0 1)", kChatName[enigma], fam2, work2);
    toMain();
    T::groupCount = 1;
  }

  g_test = "LM3 a friend's star stays while any of its unread messages is unread; after the last read it clears only once the write succeeded, on the list already shown";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    std::vector<MessageRef> bob = unreadRun(kBob, enigma, 2, kDay06 + 600);
    powerOn(); runFor(30);
    presence(kG, kBob, "Bob");
    openMode(enigma);
    bool s0 = starred("Bob");
    confirm();                 // into Bob's chat (Enigma: the contact menu, then Chat)
    if (enigma) confirm();
    loopPass(3);
    rot(-1);                   // bob[1]
    runFor(2);
    back();                    // to the list (Enigma: the contact menu first)
    if (enigma) back();
    loopPass(3);
    bool s1 = starred("Bob");  // bob[0] still unread
    static bool failing;
    failing = true;
    LittleFS.failOpen = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
    confirm();
    if (enigma) confirm();
    loopPass(3);
    rot(-1); rot(-1);          // bob[1] (read), bob[0]: its write fails
    loopPass(2);
    back();
    if (enigma) back();
    loopPass(3);
    bool listShown = rowY("Bob") >= 0;
    bool s2 = starred("Bob");  // still unread on flash
    runFor(5);
    bool s3 = starred("Bob");
    failing = false;
    LittleFS.failOpen = nullptr;
    bool cleared = false;
    for (int i = 0; i < 6000 && !cleared; i++) {
      loopPass();
      cleared = !starred("Bob") && rowY("Bob") >= 0;
    }
    CHECK(s0 && s1 && listShown && s2 && s3 && cleared && unreadOf(bob).empty(),
          "%s: star at entry %d, after reading 1 of 2 %d; on the shown list while the last write fails %d %d; cleared there after it succeeded %d",
          kChatName[enigma], s0, s1, s2, s3, cleared);
    toMain();
  }

  g_test = "LM4 a new arrival lights the star of its friend on the list already shown (and only that one)";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    storeTextIn(kG, kCarl, kCarl, true, false, kDay06 + 60);
    powerOn(); runFor(30);
    presence(kG, kBob, "Bob");
    presence(kG, kCarl, "Carol");
    openMode(enigma);
    bool before = starred("Carol") || starred("Bob");
    arrive(kCarl, ofType(enigma), kDay06 + 900);
    loopPass(3);
    bool carol = starred("Carol"), bobS = starred("Bob");
    arrive(kBob, ofType(!enigma), kDay06 + 960);  // the other category
    loopPass(3);
    bool bobOther = starred("Bob");
    CHECK(!before && carol && !bobS && !bobOther, "%s: before %d; after Carol's arrival Carol %d Bob %d; after Bob's arrival of the other type Bob %d",
          kChatName[enigma], before, carol, bobS, bobOther);
    toMain();
  }

  g_test = "LM5 an UNRESOLVED conversation (counts unknown) keeps its category's star lit, conservatively; the other category stays dark";
  {
    freshDevice();
    storeTextIn(kG, kBob, kBob, true, false, kDay06 + 60);  // nothing unread on flash
    powerOn(); runFor(30);
    presence(kG, kBob, "Bob");
    Notifications::Summary* s = Notifications::findOrCreateSummary(kG, kBob);
    if (s != nullptr) Notifications::markUnresolved(s, Notifications::BADGE_TEXT);  // as after an unreadable arrival announced as Text
    openMode(false);
    bool t = starred("Bob");
    toMain();
    openMode(true);
    bool e = starred("Bob");
    toMain();
    CHECK(t && !e, "UNRESOLVED (Text flagged): Text list %d, Enigma list %d (expected 1 0)", t, e);
  }

  g_test = "LM6 long names keep their star visible in a reserved column; the selected row and scrolling redraw stars exactly";
  {
    freshDevice();
    T::realGlyphs = true;
    // Presence keeps 16 characters of a name: the widest is 16 wide glyphs
    // (about 270 px: more than the 240 px row).
    const char* longName = "WWWWWWWWWWWWWWWW";
    char ids[8][16];
    for (int i = 0; i < 8; i++) snprintf(ids[i], sizeof(ids[i]), "C0FFEE00000%d", i);
    for (int i = 0; i < 8; i++) storeTextIn(kG, ids[i], ids[i], true, i % 2 == 0, kDay06 + 60);  // even ones unread
    powerOn(); runFor(60);
    T::realGlyphs = true;
    char names[8][32];
    for (int i = 0; i < 8; i++) {
      if (i == 0) snprintf(names[i], sizeof(names[i]), "%s", longName);
      else snprintf(names[i], sizeof(names[i]), "Friend%d", i);
      presence(kG, ids[i], names[i]);
    }
    openMode(false);
    // The rows are listed online first in presence order; check every row visible at each scroll position.
    bool exact = true, inColumn = true, longChecked = false;
    int checkedRows = 0;
    for (int step = 0; step < 24; step++) {  // the whole list (sorted by name: the long one last), and around
      if (rowY("WWWW") >= 0) longChecked = true;
      for (int i = 0; i < 8; i++) {
        std::string prefix = i == 0 ? std::string("WWWW") : std::string(names[i]);
        int y = rowY(prefix);
        if (y < 0) continue;
        checkedRows++;
        bool st = starOnRow(y);
        if (st != (i % 2 == 0)) exact = false;
        for (auto& d : T::screen) {
          if (d.y != y) continue;
          if (d.text == "*" || d.text == " *") {
            if (d.x + Display::textWidth(d.text.c_str()) > Display::kScreenWidth) inColumn = false;
            for (auto& l : T::screen)  // the (clipped) label ends before the star cell
              if (l.y == y && l.text.rfind(prefix.substr(0, 4), 0) == 0 && l.x + Display::textWidth(l.text.c_str()) > d.x) inColumn = false;
          }
        }
      }
      rot(1);
    }
    T::realGlyphs = false;
    CHECK(exact && inColumn && longChecked && checkedRows >= 16,
          "scrolling through 8 friends: stars exact %d, in their own column %d, the long name's row checked %d (%d rows checked)", exact, inColumn,
          longChecked, checkedRows);
    toMain();
  }

  g_test = "LM7 showing, scrolling and refreshing the lists' stars does no storage work (no file opens, listings or NVS writes in the UI passes)";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    T::groupCount = 2;
    for (int i = 0; i < 3; i++) storeTextIn(kG, kBob, kBob, true, true, kDay06 + i * 60);
    storeTextIn("GROUP2", kCarl, kCarl, true, true, kDay06 + 60);
    powerOn(); runFor(60);
    presence(kG, kBob, "Bob");
    presence("GROUP2", kCarl, "Carol");
    openMode(enigma);  // the group list
    loopPass(5);
    g_uiMax = PassMax();
    FsStats a = g_fs;
    for (int i = 0; i < 6; i++) rot(1);
    loopPass(20);
    confirm();  // a group's recipient list
    loopPass(5);
    // only the list ticks from here: measured alone
    FsStats b = g_fs;
    int nb = g_notifyPrefs.writes;
    for (int i = 0; i < 20; i++) {
      T::now += 20;
      if (i % 3 == 0) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, 1, 0, 0));
      Menu::tick();
    }
    uint64_t opens = g_fs.fileOpens - b.fileOpens, entries = g_fs.dirEntries - b.dirEntries;
    int nvs = g_notifyPrefs.writes - nb;
    (void)a;
    CHECK(opens == 0 && entries == 0 && nvs == 0 && g_uiMax.opens == 0 && g_uiMax.dirEntries == 0 && g_uiMax.nvs == 0,
          "%s: list ticks: %llu opens, %llu directory entries, %d NVS writes (UI passes max opens %llu entries %llu nvs %d)", kChatName[enigma],
          (unsigned long long)opens, (unsigned long long)entries, nvs, (unsigned long long)g_uiMax.opens, (unsigned long long)g_uiMax.dirEntries,
          g_uiMax.nvs);
    toMain();
    T::groupCount = 1;
  }
  g_test = "LM8 an idle recipient list longer than 8 rows draws nothing tick after tick; a star change redraws only that badge cell";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    char ids[12][16];
    for (int i = 0; i < 12; i++) snprintf(ids[i], sizeof(ids[i]), "F00D0000000%c", 'A' + i);
    storeTextIn(kG, ids[0], ids[0], true, false, kDay06 + 60);
    powerOn(); runFor(30);
    for (int i = 0; i < 12; i++) {
      char name[16];
      snprintf(name, sizeof(name), "AAPal%02d", i);  // (sorted first: earlier tests' contacts stay online)
      presence(kG, ids[i], name);
    }
    openMode(enigma);
    loopPass(5);
    size_t d0 = T::drawLog.size();
    for (int i = 0; i < 30; i++) {
      T::now += 20;
      Menu::tick();
    }
    size_t idle = T::drawLog.size() - d0;
    arrive(ids[0], ofType(enigma), kDay06 + 900);  // AAPal00, on screen
    size_t d1 = T::drawLog.size();
    T::now += 20;
    Menu::tick();
    size_t onFlip = T::drawLog.size() - d1;
    bool lit = starred("AAPal00");
    CHECK(idle == 0 && lit && onFlip >= 1 && onFlip <= 2, "%s: %zu draws in 30 idle ticks (expected 0); the star change drew %zu items (lit %d)",
          kChatName[enigma], idle, onFlip, lit);
    toMain();
  }
  (void)none;
}
