cd /home/user/morse-buddy/host_tests/race_r3_r5; python3 - <<'EOF'
p='storage_test.cpp'; s=open(p).read()
old='''    if (variant < 2) {
      CHECK(p.passesToContent > 0 && p.passesToContent <= 2 && opens <= 8, "variant %d: content after %d passes, %llu opens (no reload)",
            variant, p.passesToContent, (unsigned long long)opens);
    } else {
      CHECK(opens >= 90, "variant 2: the dropped feed was not used (%llu opens: a reload)", (unsigned long long)opens);
    }
  }
'''
new='''    CHECK(p.passesToContent > 0 && p.passesToContent <= 2 && opens <= 8, "variant %d: content after %d passes, %llu opens (no reload)",
          variant, p.passesToContent, (unsigned long long)opens);
  }

  g_test = "CE22 the parked feed, step by step: a mutation of its conversation, an unreadable record or an out-of-step record drops it; a mutation elsewhere does not";
  for (int variant = 0; variant < 5; variant++) {  // 0 clean, 1 arrival elsewhere, 2 arrival in Everyone, 3 unreadable, 4 out of step
    freshDevice();
    everyoneHistory(30);
    for (int i = 0; i < 10; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    MessageStore::loadConversationIndex(kG, kBob);  // the shared index is current for Bob
    uint32_t seqs[MessageStore::kMaxMessagesPerThread];
    bool reliable = false;
    uint16_t n = MessageStore::listMessageSequences(kG, MessageStore::kEveryone, seqs, MessageStore::kMaxMessagesPerThread, &reliable);
    bool began = MessageStore::beginIndexFeed(kG, MessageStore::kEveryone, seqs, n, reliable);
    for (uint16_t i = 0; i < n; i++) {
      if (i == 15 && variant == 1) arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      if (i == 15 && variant == 2) arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      MessageRef ref{};
      strcpy(ref.group_code, kG);
      strcpy(ref.contact_key, MessageStore::kEveryone);
      ref.sequence = seqs[variant == 4 && i == 15 ? 16 : i];
      StoredMessageView view;
      bool ok = MessageStore::loadMessage(ref, &view) && !(variant == 3 && i == 15);
      MessageStore::feedIndexRecord(kG, MessageStore::kEveryone, ref.sequence, ok ? &view.header : nullptr,
                                    ok ? view.envelope.message_id : nullptr);
    }
    // Leaving Bob parks Bob over the fed index only once Everyone is loaded: ask Everyone with no reads allowed.
    uint16_t c = 0;
    FsStats a = g_fs;
    bool swapped = MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 0, &c, nullptr, nullptr);
    uint64_t reads = g_fs.fileOpens - a.fileOpens;
    bool expectUsable = variant <= 1;
    std::vector<std::tuple<uint32_t, uint32_t, std::string, uint16_t>> kept, fresh;
    if (swapped) {
      for (uint16_t i = 0; i < c; i++) {
        const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
        kept.emplace_back(e->sequence, e->effective_sort_timestamp, e->message_id, e->flags);
      }
    }
    coldIndex();
    uint16_t m = MessageStore::loadConversationIndex(kG, MessageStore::kEveryone);
    for (uint16_t i = 0; i < m; i++) {
      const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
      fresh.emplace_back(e->sequence, e->effective_sort_timestamp, e->message_id, e->flags);
    }
    CHECK(began && swapped == expectUsable && reads == 0 && (!swapped || kept == fresh),
          "variant %d: feed began %d, used %d (expected %d) with %llu reads, equal to a fresh load %d", variant, began, swapped,
          expectUsable, (unsigned long long)reads, kept == fresh);
  }
'''
assert s.count(old)==1; s=s.replace(old,new); open(p,'w').write(s)
EOF
S=/tmp/claude-0/-home-user-morse-buddy/6c983299-def3-5558-ac59-7534d8753181/scratchpad
g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -DREAL_NOTIFICATIONS -DREAL_MESSAGE_STORE -I fake_fs -I . -I fake -I ../../src storage_test.cpp stubs.cpp $(cat $S/fn2.txt) fake_fs/LittleFS.cpp -o $S/stt 2>&1 | grep -E "error" | head; $S/stt $S/fs_ce21 2>&1 | grep -E "CE2[12]|FAIL|passed"