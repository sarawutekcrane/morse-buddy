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
  freshDevice(); powerOn(); runFor(5);
  LittleFS.fakeUsedBytes = LittleFS.totalBytes() - 150 * 1024;  // 150 KB free: under watermark + margin + one append
  before = g_fs.usedBytesCalls;
  for (int i = 0; i < 10; i++) arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u + i * 60);
  walks = g_fs.usedBytesCalls - before;
  CHECK(walks == 10 && truth(kBob).files == 10, "10 appends near the watermark: %llu exact measurements (expected 10), %d records",
        static_cast<unsigned long long>(walks), truth(kBob).files);
  LittleFS.fakeUsedBytes = 200000;

  g_test = "SL4 the estimate is measured again after a change outside the store, and after 10 minutes";
  freshDevice(); powerOn(); runFor(5);
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000000u);  // measured
  before = g_fs.usedBytesCalls;
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000060u);  // estimated
  uint64_t estimated = g_fs.usedBytesCalls - before;
  MessageStore::notifyExternalStorageMutationAttempted();
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000120u);  // measured again
  uint64_t afterExternal = g_fs.usedBytesCalls - before;
  T::now += 11 * 60 * 1000;
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000180u);  // measured again (age)
  uint64_t afterAge = g_fs.usedBytesCalls - before;
  CHECK(estimated == 0 && afterExternal == 1 && afterAge == 2, "estimated %llu, after an outside change %llu, after 11 min %llu",
        static_cast<unsigned long long>(estimated), static_cast<unsigned long long>(afterExternal),
        static_cast<unsigned long long>(afterAge));
}
