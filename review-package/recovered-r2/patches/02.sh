cd /home/user/morse-buddy/host_tests/race_r3_r5; python3 - <<'EOF'
p='storage_test.cpp'; s=open(p).read()
anchor='''  printf("%d checks passed, %d failed\\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}'''
assert s.count(anchor)==1
new='''  g_test = "CE21 a 1:1 chat opened right after boot holds the shared index: the sweep's recount builds Everyone's parked index, entered without reloading and equal to a fresh load";
  for (int variant = 0; variant < 3; variant++) {  // 0: quiet, 1: an arrival elsewhere meanwhile, 2: an arrival in Everyone meanwhile
    freshDevice();
    everyoneHistory(90);
    for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    for (int i = 0; i < 20; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    powerOn();
    openText(kBob);  // before the sweep reached anything
    for (int i = 0; i < 2000; i++) {
      loopPass();
      if (variant > 0 && i == 40) arrive(variant == 1 ? kCarol : MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
    }
    back();
    FsStats a = g_fs;
    const char* expect = variant == 2 ? "" : kNewest;
    EntryProfile p = enterChat(false, MessageStore::kEveryone, expect);
    for (int i = 0; i < 2000 && g_indexDirty; i++) loopPass();
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    std::vector<std::tuple<uint32_t, uint32_t, std::string, uint16_t>> kept, fresh;
    for (uint16_t i = 0; i < g_indexTotal; i++) {
      const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
      kept.emplace_back(e->sequence, e->effective_sort_timestamp, e->message_id, e->flags);
    }
    back();
    coldIndex();
    uint16_t n = MessageStore::loadConversationIndex(kG, MessageStore::kEveryone);
    for (uint16_t i = 0; i < n; i++) {
      const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
      fresh.emplace_back(e->sequence, e->effective_sort_timestamp, e->message_id, e->flags);
    }
    printf("   CE21 variant %d measured: %llu opens on entry, content after %d passes\\n", variant, (unsigned long long)opens,
           p.passesToContent);
    CHECK(kept == fresh && n == (variant == 2 ? 91 : 90), "variant %d: index equal to a fresh load %d (%u entries)", variant,
          kept == fresh, n);
    if (variant < 2) {
      CHECK(p.passesToContent > 0 && p.passesToContent <= 2 && opens <= 8, "variant %d: content after %d passes, %llu opens (no reload)",
            variant, p.passesToContent, (unsigned long long)opens);
    } else {
      CHECK(opens >= 90, "variant 2: the dropped feed was not used (%llu opens: a reload)", (unsigned long long)opens);
    }
  }

'''
s=s.replace(anchor,new+anchor); open(p,'w').write(s)
EOF
S=/tmp/claude-0/-home-user-morse-buddy/6c983299-def3-5558-ac59-7534d8753181/scratchpad
g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -DREAL_NOTIFICATIONS -DREAL_MESSAGE_STORE -I fake_fs -I . -I fake -I ../../src storage_test.cpp stubs.cpp $(cat $S/fn2.txt) fake_fs/LittleFS.cpp -o $S/stt 2>&1 | grep -E "error" | head; $S/stt $S/fs_ce21 2>&1 | grep -E "CE21|FAIL|passed"