python3 - <<'EOF'
p='storage_messages.cpp'; s=open(p).read()
def rep(old,new,cnt=1):
    global s
    assert s.count(old)==cnt, (s.count(old), old[:90]); s=s.replace(old,new)
rep('''struct ParkedIndex {
  bool valid;
  char group[PacketCodec::kGroupCodeLen];
  char contact[kContactKeyLen];
  uint32_t generation;
  uint16_t count;
  ConversationIndexEntry entries[kParkedCapacity];
};
ParkedIndex* g_parked = nullptr;

bool isParked(const char* group_code, const char* contact_key) {
  return g_parked != nullptr && g_parked->valid && strcmp(g_parked->group, group_code) == 0 &&
         strcmp(g_parked->contact, contact_key) == 0;
}''','''// While the shared index is held by a chat, the boot sweep's recount feeds
// the parked one instead (feeding: not usable until complete).
struct ParkedIndex {
  bool valid;
  char group[PacketCodec::kGroupCodeLen];
  char contact[kContactKeyLen];
  uint32_t generation;
  uint16_t count;
  ConversationIndexEntry entries[kParkedCapacity];
  bool feeding, feedReliable;
  uint16_t feedN, feedPos;
  uint32_t feedGeneration;
  uint32_t feedSeqs[kParkedCapacity];
};
ParkedIndex* g_parked = nullptr;

bool parkedHolds(const char* group_code, const char* contact_key) {
  return g_parked != nullptr && strcmp(g_parked->group, group_code) == 0 && strcmp(g_parked->contact, contact_key) == 0;
}

bool isParked(const char* group_code, const char* contact_key) {
  return g_parked != nullptr && g_parked->valid && parkedHolds(group_code, contact_key);
}''')
rep('''  bool keepParked = known && g_parked != nullptr && g_parked->valid && g_parked->generation == before &&
                    !isParked(group_code, contact_key);
  g_storageChangeGeneration++;
  if (keepParked) g_parked->generation = g_storageChangeGeneration;''','''  bool keepParked = known && g_parked != nullptr && g_parked->valid && g_parked->generation == before &&
                    !isParked(group_code, contact_key);
  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before &&
                        !parkedHolds(group_code, contact_key);
  g_storageChangeGeneration++;
  if (keepParked) g_parked->generation = g_storageChangeGeneration;
  if (keepParkedFeed) g_parked->feedGeneration = g_storageChangeGeneration;''')
rep('''  memcpy(g_parked->entries, g_index, sizeof(ConversationIndexEntry) * g_indexCount);
  g_parked->count = g_indexCount;''','''  memcpy(g_parked->entries, g_index, sizeof(ConversationIndexEntry) * g_indexCount);
  g_parked->feeding = false;
  g_parked->count = g_indexCount;''')
rep('''    g_parked = new (std::nothrow) ParkedIndex;
    if (g_parked == nullptr) return;
    g_parked->valid = false;
  }''','''    g_parked = new (std::nothrow) ParkedIndex;
    if (g_parked == nullptr) return;
    g_parked->valid = false;
    g_parked->feeding = false;
  }''')
rep('''bool beginIndexFeed(const char* group_code, const char* contact_key, const uint32_t* seqs, uint16_t n,
                    bool listingReliable) {
  // Only into an empty index: never over one that is cached and current, or
  // being loaded for a chat (a stale one is reloaded at its next use anyway).
  uint32_t now = getStorageChangeGeneration();
  if (g_indexCacheValid && g_indexCacheGeneration == now) return false;
  if (g_loader.active && !g_loader.fed && g_loader.generation == now) return false;''','''namespace {
// The shared index is in use: the feed builds the parked one instead -- not
// over a current parked index of another conversation (the last chat).
bool beginParkedFeed(const char* group_code, const char* contact_key, const uint32_t* seqs, uint16_t n,
                     bool listingReliable) {
  uint32_t now = getStorageChangeGeneration();
  if (!listingReliable || n > kParkedCapacity) return false;
  if (strlen(group_code) >= PacketCodec::kGroupCodeLen || strlen(contact_key) >= kContactKeyLen) return false;
  if (g_parked != nullptr && g_parked->valid && g_parked->generation == now) return false;  // current: kept
  if (g_parked == nullptr) {
    g_parked = new (std::nothrow) ParkedIndex;
    if (g_parked == nullptr) return false;
  }
  g_parked->valid = false;
  strncpy(g_parked->group, group_code, sizeof(g_parked->group) - 1);
  g_parked->group[sizeof(g_parked->group) - 1] = '\\0';
  strncpy(g_parked->contact, contact_key, sizeof(g_parked->contact) - 1);
  g_parked->contact[sizeof(g_parked->contact) - 1] = '\\0';
  g_parked->count = 0;
  g_parked->feeding = true;
  g_parked->feedReliable = true;
  g_parked->feedN = n;
  g_parked->feedPos = 0;
  g_parked->feedGeneration = now;
  memcpy(g_parked->feedSeqs, seqs, sizeof(uint32_t) * n);
  if (n == 0) {
    g_parked->feeding = false;
    g_parked->valid = true;
    g_parked->generation = now;
  }
  return true;
}

void feedParkedRecord(uint32_t seq, const StoredHeader* hdr, const char* message_id) {
  ParkedIndex& p = *g_parked;
  if (p.feedPos >= p.feedN || p.feedSeqs[p.feedPos] != seq || p.feedGeneration != getStorageChangeGeneration()) {
    p.feeding = false;  // out of step, or the conversation changed: dropped
    return;
  }
  p.feedPos++;
  if (hdr == nullptr) {
    p.feedReliable = false;
  } else {
    ConversationIndexEntry e;
    e.sequence = seq;
    e.effective_sort_timestamp = hdr->effective_sort_timestamp;
    strncpy(e.message_id, message_id, PacketCodec::kMessageIdLen - 1);
    e.message_id[PacketCodec::kMessageIdLen - 1] = '\\0';
    e.flags = hdr->flags;
    if (!patchIndexWrite(p.entries, &p.count, kParkedCapacity, e)) p.feedReliable = false;
  }
  if (p.feedPos == p.feedN) {
    p.feeding = false;
    p.valid = p.feedReliable;
    p.generation = p.feedGeneration;
  }
}
}  // namespace

bool beginIndexFeed(const char* group_code, const char* contact_key, const uint32_t* seqs, uint16_t n,
                    bool listingReliable) {
  // Into the shared index only when it holds nothing current and no chat is
  // loading it; otherwise into the parked one.
  uint32_t now = getStorageChangeGeneration();
  if ((g_indexCacheValid && g_indexCacheGeneration == now) || (g_loader.active && !g_loader.fed && g_loader.generation == now)) {
    if (isIndexTarget(group_code, contact_key)) return false;  // that conversation is already the shared index
    return beginParkedFeed(group_code, contact_key, seqs, n, listingReliable);
  }''')
rep('''void feedIndexRecord(const char* group_code, const char* contact_key, uint32_t seq, const StoredHeader* hdr,
                     const char* message_id) {
  if (!g_loader.active || !g_loader.fed || !isIndexTarget(group_code, contact_key)) return;''','''void feedIndexRecord(const char* group_code, const char* contact_key, uint32_t seq, const StoredHeader* hdr,
                     const char* message_id) {
  if (g_parked != nullptr && g_parked->feeding && parkedHolds(group_code, contact_key)) {
    feedParkedRecord(seq, hdr, message_id);
    return;
  }
  if (!g_loader.active || !g_loader.fed || !isIndexTarget(group_code, contact_key)) return;''')
open(p,'w').write(s)
EOF
grep -n "^bool patchIndexWrite" storage_messages.cpp; grep -n "^bool beginParkedFeed\|^void feedParkedRecord" storage_messages.cpp