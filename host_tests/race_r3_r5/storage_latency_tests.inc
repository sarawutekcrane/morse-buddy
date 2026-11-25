// Storage work on the append path (hardware, device A on b573ec7:
// "[PERF][STORE_APPEND] total=563 ms freeSpace=390 ms rewrite=156 ms" and
// "remove(): /messages/.../00000020.msg does not exists or is directory").
// Included from storage_test.cpp's main().
{
  g_test = "SL1 appends do not walk the whole filesystem each time: one exact free-space measurement per 32 appends at most";
  freshDevice(); powerOn(); runFor(5);
  uint64_t before = g_fs.usedBytesCalls;
  for (int i = 0; i < 40; i++) arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u + i * 60);
  uint64_t walks = g_fs.usedBytesCalls - before;
  CHECK(walks >= 1 && walks <= 2 && truth(kBob).files == 40, "40 appends: %llu filesystem walks (expected 1-2), %d records",
        static_cast<unsigned long long>(walks), truth(kBob).files);

  g_test = "SL2 a new record is written without removing a path that does not exist (the ESP32 error line); rewriting an existing one still replaces it";
  freshDevice(); powerOn(); runFor(5);
  uint64_t missing0 = g_fs.removesMissing;
  MessageRef last{};
  for (int i = 0; i < 20; i++) last = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u + i * 60);
  uint64_t missing = g_fs.removesMissing - missing0;
  bool updated = MessageStore::updateLocalFlags(last, 0, MessageStore::FLAG_UNREAD);  // an in-place rewrite
  StoredMessageView v;
  bool reread = MessageStore::loadMessage(last, &v) && !(v.header.flags & MessageStore::FLAG_UNREAD);
  CHECK(missing == 0 && updated && reread && truth(kBob).files == 20,
        "removes of missing paths %llu (expected 0); rewrite ok %d, re-read ok %d, %d records",
        static_cast<unsigned long long>(missing), updated, reread, truth(kBob).files);

  g_test = "SL3 close to the watermark every append measures exactly, as before (no estimate trusted there)";
  freshDevice();
  LittleFS.fakeUsedBytes = LittleFS.totalBytes() - 150 * 1024;  // 150 KB free at boot: under watermark + margin + one append
  powerOn(); runFor(5);
  before = g_fs.usedBytesCalls;
  for (int i = 0; i < 10; i++) arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u + i * 60);
  walks = g_fs.usedBytesCalls - before;
  CHECK(walks == 10 && truth(kBob).files == 10, "10 appends near the watermark: %llu exact measurements (expected 10), %d records",
        static_cast<unsigned long long>(walks), truth(kBob).files);
  LittleFS.fakeUsedBytes = 200000;

  g_test = "SL4 boot takes the exact free-space figure (device A on 827cd27: freeSpace=364 ms on the first send after boot); the first appends do not walk";
  freshDevice();
  uint64_t b0 = g_fs.usedBytesCalls;
  powerOn();
  uint64_t atBoot = g_fs.usedBytesCalls - b0;
  runFor(5);
  before = g_fs.usedBytesCalls;
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u);
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000060u);
  uint64_t firstAppends = g_fs.usedBytesCalls - before;
  CHECK(atBoot == 1 && firstAppends == 0, "walks at boot %llu (expected 1), during the first two appends %llu (expected 0)",
        static_cast<unsigned long long>(atBoot), static_cast<unsigned long long>(firstAppends));

  g_test = "SL4b a change outside the store makes the next append measure again; 11 quiet minutes do not (time cannot make the conservative estimate optimistic)";
  before = g_fs.usedBytesCalls;
  MessageStore::notifyExternalStorageMutationAttempted();
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000120u);  // measured again
  uint64_t afterExternal = g_fs.usedBytesCalls - before;
  T::now += 11 * 60 * 1000;
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000180u);  // estimated
  uint64_t afterQuiet = g_fs.usedBytesCalls - before;
  CHECK(afterExternal == 1 && afterQuiet == 1 && truth(kBob).files == 4,
        "after an outside change %llu (expected 1), after 11 quiet minutes %llu (expected still 1), %d records",
        static_cast<unsigned long long>(afterExternal), static_cast<unsigned long long>(afterQuiet), truth(kBob).files);

  g_test = "SL4c boot below the watermark: no estimate is trusted, every append measures (and evicts) exactly as before";
  freshDevice();
  LittleFS.fakeUsedBytes = LittleFS.totalBytes() - 64 * 1024;  // 64 KB free: below the 96 KB watermark
  powerOn(); runFor(5);
  before = g_fs.usedBytesCalls;
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u);
  CHECK(g_fs.usedBytesCalls - before >= 1 && truth(kBob).files == 1, "low-space append: %llu exact measurements, %d records",
        static_cast<unsigned long long>(g_fs.usedBytesCalls - before), truth(kBob).files);
  LittleFS.fakeUsedBytes = 200000;
}
