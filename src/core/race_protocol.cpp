#include "core/race_protocol.h"

#include <stdio.h>
#include <string.h>

#include "core/crc32.h"

namespace RaceProtocol {

namespace {

// ---- little-endian helpers ---------------------------------------------------
void put8(uint8_t*& p, uint8_t v) { *p++ = v; }
void put16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v);
  *p++ = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t*& p, uint32_t v) {
  for (int i = 0; i < 4; i++) *p++ = static_cast<uint8_t>(v >> (8 * i));
}
void putStr(uint8_t*& p, const char* s, size_t width) {  // NUL-padded fixed width
  size_t n = strlen(s);
  if (n > width) n = width;
  memcpy(p, s, n);
  memset(p + n, 0, width - n);
  p += width;
}
uint8_t get8(const uint8_t*& p) { return *p++; }
uint16_t get16(const uint8_t*& p) {
  uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  return v;
}
uint32_t get32(const uint8_t*& p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  p += 4;
  return v;
}
void getStr(const uint8_t*& p, char* out, size_t width) {
  memcpy(out, p, width);
  out[width] = '\0';
  p += width;
}

bool isHexId(const char* s) {
  if (strlen(s) != kIdChars) return false;
  for (size_t i = 0; i < kIdChars; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}
bool isHexIdOrEmpty(const char* s) { return s[0] == '\0' || isHexId(s); }

uint32_t crcOf(const uint8_t* data, size_t len) { return Crc32::compute(data, len); }

const char kMagicCommit[4] = {'R', 'C', 'M', '1'};
const char kMagicSlot[4] = {'R', 'C', 'S', '1'};
const char kMagicMember[4] = {'R', 'M', 'B', '1'};
constexpr uint8_t kRecFmt = 1;
const char* const kSlotKeys[2] = {"rc_slot0", "rc_slot1"};
constexpr size_t kSlotRsOffset = 4 + 1 + 1 + 4 + kGroupLen + 4 + 4 + 4 + 2;  // 57

// timers (implementation constants)
constexpr uint32_t kResendFirstMs = 3000, kResendCapMs = 30000;
constexpr uint32_t kQueryFirstMs = 5000, kQueryCapMs = 30000;
constexpr uint32_t kFreshnessDelayMs = 3000, kFreshnessPeriodMs = 30000;
constexpr uint32_t kEchoFirstMs = 3000, kEchoCapMs = 30000;
constexpr uint32_t kHeartbeatMs = 60000;
constexpr uint32_t kRepublishMinMs = 5000;
constexpr uint32_t kReplyRateMs = 5000;
constexpr uint32_t kReconcileRateMs = 30000;
constexpr uint32_t kWaitingMs = 15000;
constexpr uint32_t kDiscoveryExpiryMs = 150000;
constexpr uint32_t kNewRoomWindowS = 120;
constexpr uint32_t kRecordRetryMs = 2000;  // retry of a failed member-record write

constexpr uint8_t kNoReply = 0xFF;

bool due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }
uint32_t nextBackoff(uint32_t cur, uint32_t first, uint32_t cap) {
  if (cur == 0) return first;
  uint32_t n = cur * 2;
  return n > cap ? cap : n;
}

}  // namespace

// =============================================================================
// Codecs
// =============================================================================
size_t encodeState(const RoomState& s, uint8_t* out, size_t cap) {
  if (s.count > kMaxEntries) return 0;
  size_t need = kStateFixedBytes + kEntryBytes * s.count;
  if (cap < need) return 0;
  uint8_t* p = out;
  put8(p, kFmt);
  putStr(p, s.coordinator, kIdChars);
  put32(p, s.n);
  put32(p, s.nonce);
  put32(p, s.version);
  put8(p, s.phase);
  put8(p, s.closeReason);
  put16(p, s.roundNo);
  put32(p, s.roundStart);
  memcpy(p, s.secret, 4);
  p += 4;
  putStr(p, s.startRight, kIdChars);
  put32(p, s.startRightVersion);
  put32(p, s.resultRound);
  putStr(p, s.resultWinner, kIdChars);
  put32(p, s.evictionFloor);
  put32(p, s.createdUnix);
  put8(p, s.count);
  for (uint8_t i = 0; i < s.count; i++) {
    const Entry& e = s.entries[i];
    putStr(p, e.id, kIdChars);
    put16(p, e.score);
    put8(p, e.flags);
    put32(p, e.lastSeq);
    put32(p, e.departedAt);
  }
  return static_cast<size_t>(p - out);
}

bool decodeState(const uint8_t* in, size_t len, RoomState* s) {
  if (in == nullptr || len < kStateFixedBytes) return false;
  const uint8_t* p = in;
  if (get8(p) != kFmt) return false;
  memset(s, 0, sizeof(*s));
  getStr(p, s->coordinator, kIdChars);
  s->n = get32(p);
  s->nonce = get32(p);
  s->version = get32(p);
  s->phase = get8(p);
  s->closeReason = get8(p);
  s->roundNo = get16(p);
  s->roundStart = get32(p);
  memcpy(s->secret, p, 4);
  p += 4;
  getStr(p, s->startRight, kIdChars);
  s->startRightVersion = get32(p);
  s->resultRound = get32(p);
  getStr(p, s->resultWinner, kIdChars);
  s->evictionFloor = get32(p);
  s->createdUnix = get32(p);
  s->count = get8(p);
  if (s->count > kMaxEntries || len != kStateFixedBytes + kEntryBytes * s->count) return false;
  if (s->phase > PHASE_CLOSED || s->closeReason > CLOSE_EXHAUSTED) return false;
  if (!isHexId(s->coordinator) || !isHexIdOrEmpty(s->startRight) || !isHexIdOrEmpty(s->resultWinner)) return false;
  if (s->n == 0 || s->version == 0) return false;
  for (uint8_t i = 0; i < s->count; i++) {
    Entry& e = s->entries[i];
    getStr(p, e.id, kIdChars);
    e.score = get16(p);
    e.flags = get8(p);
    e.lastSeq = get32(p);
    e.departedAt = get32(p);
    if (!isHexId(e.id) || e.flags > ENTRY_ACTIVE) return false;
  }
  return true;
}

size_t encodeRequest(const Request& r, uint8_t* out, size_t cap) {
  if (cap < kRequestBytes || r.op < OP_JOIN || r.op > OP_QUERY) return 0;
  uint8_t* p = out;
  put8(p, kFmt);
  put8(p, r.op);
  putStr(p, r.coordinator, kIdChars);
  put32(p, r.n);
  put32(p, r.nonce);
  putStr(p, r.sender, kIdChars);
  put32(p, r.seq);
  put32(p, r.basis);
  put32(p, r.roundStart);
  memcpy(p, r.guess, 4);
  p += 4;
  put32(p, r.entitlement);
  return static_cast<size_t>(p - out);
}

size_t encodeReply(const Reply& r, uint8_t* out, size_t cap) {
  if (cap < kReplyBytes) return 0;
  uint8_t* p = out;
  put8(p, kFmt);
  put8(p, OP_REPLY);
  putStr(p, r.coordinator, kIdChars);
  putStr(p, r.target, kIdChars);
  put32(p, r.seq);
  put8(p, r.code);
  put32(p, r.curN);
  put32(p, r.curNonce);
  put32(p, r.curVersion);
  return static_cast<size_t>(p - out);
}

bool decodeRequestOrReply(const uint8_t* in, size_t len, bool* isReply, Request* req, Reply* rep) {
  if (in == nullptr || len < 2 || in[0] != kFmt) return false;
  const uint8_t* p = in + 2;
  uint8_t op = in[1];
  if (op == OP_REPLY) {
    if (len != kReplyBytes) return false;
    memset(rep, 0, sizeof(*rep));
    getStr(p, rep->coordinator, kIdChars);
    getStr(p, rep->target, kIdChars);
    rep->seq = get32(p);
    rep->code = get8(p);
    rep->curN = get32(p);
    rep->curNonce = get32(p);
    rep->curVersion = get32(p);
    if (!isHexId(rep->coordinator) || !isHexId(rep->target) || rep->code > CODE_CURRENT) return false;
    *isReply = true;
    return true;
  }
  if (op < OP_JOIN || op > OP_QUERY || len != kRequestBytes) return false;
  memset(req, 0, sizeof(*req));
  req->op = op;
  getStr(p, req->coordinator, kIdChars);
  req->n = get32(p);
  req->nonce = get32(p);
  getStr(p, req->sender, kIdChars);
  req->seq = get32(p);
  req->basis = get32(p);
  req->roundStart = get32(p);
  memcpy(req->guess, p, 4);
  p += 4;
  req->entitlement = get32(p);
  if (!isHexId(req->coordinator) || !isHexId(req->sender)) return false;
  *isReply = false;
  return true;
}

int findEntry(const RoomState& s, const char* id) {
  for (uint8_t i = 0; i < s.count; i++) {
    if (strcmp(s.entries[i].id, id) == 0) return i;
  }
  return -1;
}
bool isActive(const RoomState& s, const char* id) {
  int i = findEntry(s, id);
  return i >= 0 && (s.entries[i].flags & ENTRY_ACTIVE);
}
uint8_t activeCount(const RoomState& s) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < s.count; i++) {
    if (s.entries[i].flags & ENTRY_ACTIVE) n++;
  }
  return n;
}
const char* startHolder(const RoomState& s) { return s.startRight[0] ? s.startRight : s.coordinator; }

// =============================================================================
// Engine: basics
// =============================================================================
void Engine::init(const Platform& p) {
  p_ = p;
  memset(&room_, 0, sizeof(room_));
  memset(&commit_, 0, sizeof(commit_));
  coordDamaged_ = false;
  echoPending_ = false;
  echoRetryAtMs_ = echoBackoffMs_ = lastHeartbeatMs_ = lastRepublishMs_ = 0;
  republishScheduled_ = false;
  memset(replyRate_, 0, sizeof(replyRate_));
  memset(&rec_, 0, sizeof(rec_));
  attempt_ = 0;
  have_ = storagePaused_ = frozen_ = queryOutstanding_ = queryVerify_ = false;
  exitPending_ = recDirty_ = false;
  exitNotice_ = NOTICE_NONE;
  exitRetryAtMs_ = recRetryAtMs_ = 0;
  querySeq_ = 0;
  queryGroup_[0] = '\0';
  queryN_ = queryNonce_ = queryAttempt_ = 0;
  queryRetryAtMs_ = queryBackoffMs_ = resendAtMs_ = resendBackoffMs_ = 0;
  lastProgressMs_ = freshnessQueryAtMs_ = okVersion_ = 0;
  okAwait_ = wasReady_ = false;
  notice_ = NOTICE_NONE;
  haveFinal_ = false;
  memset(reconcile_, 0, sizeof(reconcile_));
  memset(disc_, 0, sizeof(disc_));
  newRoomNotify_ = false;
}

bool Engine::same(const char* a, const char* b) const { return strcmp(a, b) == 0; }

bool Engine::isCoordinator() const {
  return rec_.state != MS_NONE && commit_.valid && same(rec_.coordinator, p_.selfId) && rec_.n == commit_.n &&
         rec_.nonce == commit_.nonce && same(rec_.group, commit_.group);
}

const RoomState* Engine::view() const {
  if (rec_.state == MS_NONE) return nullptr;
  if (isCoordinator() || have_) return &room_;
  return nullptr;
}

bool Engine::isMeActive() const {
  const RoomState* v = view();
  return v != nullptr && isActive(*v, p_.selfId);
}

bool Engine::canStart() const {
  const RoomState* v = view();
  if (v == nullptr || frozen_ || rec_.state != MS_MEMBER || v->phase != PHASE_LOBBY) return false;
  return same(startHolder(*v), p_.selfId);
}

bool Engine::pendingOp(uint8_t* op) const {
  if (rec_.state == MS_NONE || rec_.pendingOp == 0) return false;
  if (op) *op = rec_.pendingOp;
  return true;
}

bool Engine::claimPending(uint32_t roundStart) const {
  return rec_.pendingOp == OP_CLAIM && rec_.roundStart == roundStart;
}

bool Engine::waitingForCoordinator() const {
  if (rec_.state == MS_NONE || isCoordinator()) return false;
  if (rec_.pendingOp == 0 && have_) return false;
  return static_cast<uint32_t>(now() - lastProgressMs_) >= kWaitingMs;
}

bool Engine::takeNewRoomNotification() {
  bool n = newRoomNotify_;
  newRoomNotify_ = false;
  return n;
}

// =============================================================================
// Durable records
// =============================================================================
bool Engine::persistMember() {
  uint8_t b[kMemberRecordBytes];
  uint8_t* p = b;
  memcpy(p, kMagicMember, 4);
  p += 4;
  put8(p, kRecFmt);
  putStr(p, rec_.group, kGroupLen);
  putStr(p, rec_.coordinator, kIdChars);
  put32(p, rec_.n);
  put32(p, rec_.nonce);
  put32(p, rec_.appliedV);
  put8(p, rec_.state);
  put8(p, rec_.pendingOp);
  put32(p, rec_.seq);
  put32(p, rec_.basis);
  put32(p, rec_.roundStart);
  memcpy(p, rec_.guess, 4);
  p += 4;
  put32(p, rec_.entitlement);
  put32(p, crcOf(b, static_cast<size_t>(p - b)));
  if (!p_.nvsWrite(p_.ctx, "rm_member", b, sizeof(b))) return false;
  recDirty_ = false;
  return true;
}

// For writes whose loss is recoverable (the stored record is an earlier,
// still-valid state: a completed request is resent identically and completes
// again; a coordinator's own record is re-derived at boot): retried until the
// stored record catches up.
void Engine::persistOrRetry() {
  if (persistMember()) return;
  recDirty_ = true;
  recRetryAtMs_ = now() + kRecordRetryMs;
}

bool Engine::reserveSeq(uint32_t* out) {
  if (p_.nextSeq(p_.ctx, out)) return true;
  notice_ = NOTICE_STORAGE_ERROR;
  return false;
}

namespace {
bool parseCommit(const uint8_t* b, size_t len, const char* self, char* group, uint32_t* n, uint32_t* nonce,
                 uint32_t* version, uint8_t* slotId, uint32_t* slotSeq, bool* closed, uint32_t* lastRoom) {
  if (len != Engine::kCommitRecordBytes || memcmp(b, kMagicCommit, 4) != 0) return false;
  const uint8_t* crcAt = b + len - 4;
  if (crcOf(b, len - 4) != get32(crcAt)) return false;
  const uint8_t* p = b + 4;
  if (get8(p) != kRecFmt) return false;
  getStr(p, group, kGroupLen - 1);
  p++;  // group field is 33 bytes (32 + terminator slot)
  char coord[kIdChars + 1];
  getStr(p, coord, kIdChars);
  if (strcmp(coord, self) != 0) return false;
  *n = get32(p);
  *nonce = get32(p);
  *version = get32(p);
  *slotId = get8(p);
  *slotSeq = get32(p);
  *closed = get8(p) != 0;
  *lastRoom = get32(p);
  return *slotId <= 1;
}

// Validates a slot record in `b` (section 4 binding, header part). `scratch`
// receives the decoded RS on success.
void parseSlot(const uint8_t* b, size_t len, uint8_t keySlot, const char* self, SlotInfo* out, RoomState* scratch) {
  out->valid = false;
  if (len != Engine::kSlotRecordBytes || memcmp(b, kMagicSlot, 4) != 0) return;
  const uint8_t* c = b + len - 4;
  if (crcOf(b, len - 4) != get32(c)) return;
  const uint8_t* p = b + 4;
  if (get8(p) != kRecFmt) return;
  if (get8(p) != keySlot) return;
  out->slotSeq = get32(p);
  getStr(p, out->group, kGroupLen - 1);
  p++;
  out->n = get32(p);
  out->nonce = get32(p);
  out->version = get32(p);
  uint16_t rsLen = get16(p);
  if (rsLen > kStateMaxBytes || !decodeState(p, rsLen, scratch)) return;
  if (strcmp(scratch->coordinator, self) != 0 || scratch->n != out->n || scratch->nonce != out->nonce ||
      scratch->version != out->version) {
    return;
  }
  out->phase = scratch->phase;
  out->valid = true;
}
}  // namespace

bool Engine::writeCommit(const CommitRecord& c) {
  uint8_t b[kCommitRecordBytes];
  uint8_t* p = b;
  memcpy(p, kMagicCommit, 4);
  p += 4;
  put8(p, kRecFmt);
  putStr(p, c.group, kGroupLen);
  putStr(p, p_.selfId, kIdChars);
  put32(p, c.n);
  put32(p, c.nonce);
  put32(p, c.version);
  put8(p, c.slotId);
  put32(p, c.slotSeq);
  put8(p, c.closed ? 1 : 0);
  put32(p, c.lastRoomCounter);
  put32(p, crcOf(b, static_cast<size_t>(p - b)));
  return p_.nvsWrite(p_.ctx, "rc_commit", b, sizeof(b));
}

// Reads slot `id` into buf_ and, if it is valid, its RS into room_.
bool Engine::loadSlot(uint8_t id, SlotInfo* out) {
  size_t l = p_.nvsRead(p_.ctx, kSlotKeys[id], buf_, sizeof(buf_));
  parseSlot(buf_, l, id, p_.selfId, out, &room_);
  return l > 0;
}

bool Engine::slotBound(const SlotInfo& s) const {
  return s.valid && s.slotSeq == commit_.slotSeq && strcmp(s.group, commit_.group) == 0 && s.n == commit_.n &&
         s.nonce == commit_.nonce && s.version == commit_.version;
}

bool Engine::loadCoordinator() {
  uint8_t cb[kCommitRecordBytes];
  size_t clen = p_.nvsRead(p_.ctx, "rc_commit", cb, sizeof(cb));
  CommitRecord c;
  memset(&c, 0, sizeof(c));
  bool commitOk = clen > 0 && parseCommit(cb, clen, p_.selfId, c.group, &c.n, &c.nonce, &c.version, &c.slotId,
                                          &c.slotSeq, &c.closed, &c.lastRoomCounter);
  SlotInfo slots[2];
  bool slotPresent[2];
  for (uint8_t i = 0; i < 2; i++) slotPresent[i] = loadSlot(i, &slots[i]);
  if (clen == 0 && !slotPresent[0] && !slotPresent[1]) return false;  // row 5: never a coordinator

  if (commitOk) {
    c.valid = true;
    commit_ = c;
    if (slotBound(slots[c.slotId])) {  // row 1 (a newer other slot is uncommitted and ignored)
      SlotInfo again;
      loadSlot(c.slotId, &again);  // room_ = the committed RS
      echoPending_ = true;           // republished until the echo is seen (closed too)
      return true;
    }
    // row 2: FC-A
    if (c.closed) {  // terminal already committed, content damaged: nothing is written or published
      echoPending_ = false;
      notice_ = NOTICE_DATA_DAMAGED;
      return true;
    }
    memset(&room_, 0, sizeof(room_));
    strncpy(room_.coordinator, p_.selfId, kIdChars);
    room_.n = c.n;
    room_.nonce = c.nonce;
    room_.version = c.version;
    room_.phase = PHASE_LOBBY;
    room_.startRightVersion = 1;
    commitTerminal(CLOSE_DATA_LOST, false);  // at V+1 (at most 0xFFFFFFFF)
    notice_ = NOTICE_DATA_DAMAGED;
    return true;
  }

  // commit record invalid or missing while slots exist
  if (slots[0].valid && slots[1].valid) {
    uint8_t newerId = slots[1].slotSeq > slots[0].slotSeq ? 1 : 0;
    const SlotInfo& newer = slots[newerId];
    const SlotInfo& older = slots[1 - newerId];
    bool consecutive = newer.slotSeq == older.slotSeq + 1;
    bool caseA = strcmp(newer.group, older.group) == 0 && newer.n == older.n && newer.nonce == older.nonce &&
                 newer.version == older.version + 1;
    bool caseB = newer.version == 1 && (newer.n != older.n || newer.nonce != older.nonce) &&
                 older.phase == PHASE_CLOSED;
    if (consecutive && (caseA || caseB)) {  // row 3: FC-B1 -- adopt the newer slot, no new version
      memset(&c, 0, sizeof(c));
      c.valid = true;
      strncpy(c.group, newer.group, kGroupLen - 1);
      c.n = newer.n;
      c.nonce = newer.nonce;
      c.version = newer.version;
      c.slotId = newerId;
      c.slotSeq = newer.slotSeq;
      c.closed = newer.phase == PHASE_CLOSED;
      c.lastRoomCounter = newer.n > older.n ? newer.n : older.n;
      commit_ = c;
      writeCommit(c);
      SlotInfo again;
      loadSlot(newerId, &again);
      echoPending_ = true;
      return true;
    }
  }
  // row 4: FC-B2 -- nothing is written for that room.
  coordDamaged_ = true;
  memset(&commit_, 0, sizeof(commit_));
  uint32_t lastN = 0;
  for (uint8_t i = 0; i < 2; i++) {
    if (slots[i].valid && slots[i].n > lastN) lastN = slots[i].n;
  }
  commit_.lastRoomCounter = lastN;
  notice_ = NOTICE_DATA_DAMAGED;
  return true;
}

void Engine::boot() {
  loadCoordinator();
  // member record
  uint8_t b[kMemberRecordBytes];
  size_t len = p_.nvsRead(p_.ctx, "rm_member", b, sizeof(b));
  memset(&rec_, 0, sizeof(rec_));
  bool memberOk = false;
  if (len == kMemberRecordBytes && memcmp(b, kMagicMember, 4) == 0) {
    const uint8_t* c = b + len - 4;
    if (crcOf(b, len - 4) == get32(c)) {
      const uint8_t* p = b + 4;
      if (get8(p) == kRecFmt) {
        getStr(p, rec_.group, kGroupLen - 1);
        p++;
        getStr(p, rec_.coordinator, kIdChars);
        rec_.n = get32(p);
        rec_.nonce = get32(p);
        rec_.appliedV = get32(p);
        rec_.state = get8(p);
        rec_.pendingOp = get8(p);
        rec_.seq = get32(p);
        rec_.basis = get32(p);
        rec_.roundStart = get32(p);
        memcpy(rec_.guess, p, 4);
        p += 4;
        rec_.entitlement = get32(p);
        memberOk = rec_.state <= MS_LEAVING;
      }
    }
  }
  if (!memberOk) memset(&rec_, 0, sizeof(rec_));

  // 4b: the coordinator record takes precedence for C's own room.
  bool namesOwn = rec_.state != MS_NONE && same(rec_.coordinator, p_.selfId);
  if (ownOpen()) {
    if (!(rec_.state == MS_MEMBER && namesOwn && rec_.n == commit_.n && rec_.nonce == commit_.nonce)) {
      memset(&rec_, 0, sizeof(rec_));
      strncpy(rec_.group, commit_.group, kGroupLen - 1);
      strncpy(rec_.coordinator, p_.selfId, kIdChars);
      rec_.n = commit_.n;
      rec_.nonce = commit_.nonce;
      rec_.appliedV = commit_.version;
      rec_.state = MS_MEMBER;
      persistOrRetry();  // derived from the coordinator record: repeated at every boot until stored
    }
  } else if (namesOwn) {
    memset(&rec_, 0, sizeof(rec_));
    persistOrRetry();  // likewise (row 2 at the next boot)
  }
  attempt_ = rec_.seq;  // re-derived per boot; any verification is invalidated anyway
  have_ = false;
  queryOutstanding_ = false;
  lastProgressMs_ = now();
  if (rec_.pendingOp) resendAtMs_ = now();  // durable pending request: resent identically once ready
  resendBackoffMs_ = 0;
  freshnessQueryAtMs_ = now() + kFreshnessDelayMs;
}

// =============================================================================
// Publishing helpers
// =============================================================================
bool Engine::sendRequest(const char* group, const Request& r) {
  uint8_t b[kRequestBytes];
  size_t n = encodeRequest(r, b, sizeof(b));
  if (n == 0) return false;
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "r2q/%s", r.coordinator);
  return p_.publish(p_.ctx, group, suffix, kKindRequest, b, static_cast<uint16_t>(n), false);
}

bool Engine::sendReply(const char* group, const char* target, uint32_t seq, uint8_t code) {
  Reply r;
  memset(&r, 0, sizeof(r));
  strncpy(r.coordinator, p_.selfId, kIdChars);
  strncpy(r.target, target, kIdChars);
  r.seq = seq;
  r.code = code;
  if (ownOpen()) {
    r.curN = commit_.n;
    r.curNonce = commit_.nonce;
    r.curVersion = commit_.version;
  }
  uint8_t b[kReplyBytes];
  size_t n = encodeReply(r, b, sizeof(b));
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "r2q/%s", target);
  return p_.publish(p_.ctx, group, suffix, kKindRequest, b, static_cast<uint16_t>(n), false);
}

bool Engine::publishCommitted() {
  if (!commit_.valid || coordDamaged_) return false;
  const uint8_t* rs = buf_;
  size_t n = 0;
  if (ownOpen()) {
    n = encodeState(room_, buf_, sizeof(buf_));
  } else {
    // own terminal room: loaded on demand from its committed slot (room_ may hold another room)
    size_t l = p_.nvsRead(p_.ctx, kSlotKeys[commit_.slotId], buf_, sizeof(buf_));
    if (l != kSlotRecordBytes || memcmp(buf_, kMagicSlot, 4) != 0) return false;
    const uint8_t* c = buf_ + kSlotRecordBytes - 4;
    if (crcOf(buf_, kSlotRecordBytes - 4) != get32(c)) return false;
    const uint8_t* p = buf_ + 6;
    if (get32(p) != commit_.slotSeq) return false;
    p = buf_ + kSlotRsOffset - 2;
    n = get16(p);
    if (n > kStateMaxBytes) return false;
    rs = buf_ + kSlotRsOffset;
  }
  if (n == 0) return false;
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "r2/%s", p_.selfId);
  lastRepublishMs_ = now();
  return p_.publish(p_.ctx, commit_.group, suffix, kKindState, rs, static_cast<uint16_t>(n), true);
}

// =============================================================================
// Coordinator: commit procedure
// =============================================================================
bool Engine::commitState(const RoomState& next) {
  uint8_t slotId = commit_.valid ? static_cast<uint8_t>(1 - commit_.slotId) : 0;
  uint32_t slotSeq = commit_.valid ? commit_.slotSeq + 1 : 1;
  // 1-2: slot record (staged in the shared buffer)
  memset(buf_, 0, sizeof(buf_));
  uint8_t* p = buf_;
  memcpy(p, kMagicSlot, 4);
  p += 4;
  put8(p, kRecFmt);
  put8(p, slotId);
  put32(p, slotSeq);
  putStr(p, commit_.group, kGroupLen);  // the room's group (set by createRoom for a new room)
  put32(p, next.n);
  put32(p, next.nonce);
  put32(p, next.version);
  size_t rsLen = encodeState(next, buf_ + kSlotRsOffset, kStateMaxBytes);
  if (rsLen == 0) return false;
  put16(p, static_cast<uint16_t>(rsLen));
  uint8_t* crcAt = buf_ + kSlotRecordBytes - 4;
  uint32_t crc = crcOf(buf_, kSlotRecordBytes - 4);
  put32(crcAt, crc);
  if (!p_.nvsWrite(p_.ctx, kSlotKeys[slotId], buf_, kSlotRecordBytes)) {
    notice_ = NOTICE_STORAGE_ERROR;
    return false;
  }
  // 3: commit record
  CommitRecord c = commit_;
  c.valid = true;
  c.n = next.n;
  c.nonce = next.nonce;
  c.version = next.version;
  c.slotId = slotId;
  c.slotSeq = slotSeq;
  c.closed = next.phase == PHASE_CLOSED;
  if (next.n > c.lastRoomCounter) c.lastRoomCounter = next.n;
  if (!writeCommit(c)) {
    notice_ = NOTICE_STORAGE_ERROR;
    return false;  // the slot write is uncommitted and never published
  }
  // 4: committed
  commit_ = c;
  room_ = next;
  coordDamaged_ = false;
  publishCommitted();
  echoPending_ = true;
  echoBackoffMs_ = kEchoFirstMs;
  echoRetryAtMs_ = now() + echoBackoffMs_;
  return true;
}

bool Engine::beginChange(RoomState* next) {
  if (!ownOpen()) return false;
  if (room_.version >= kFinalVersion - 1) {  // the next normal version would be the reserved one
    if (commitTerminal(CLOSE_EXHAUSTED, true)) ownClosed(NOTICE_ROOM_CLOSED);
    return false;
  }
  *next = room_;
  next->version = room_.version + 1;
  return true;
}

void Engine::ownClosed(Notice n) {
  // C was a member of its own room: it ends locally like any member, keeping the final snapshot.
  if (rec_.state != MS_NONE && same(rec_.coordinator, p_.selfId)) {
    memset(&rec_, 0, sizeof(rec_));
    // The closure is durable in the coordinator record (committed CLOSED);
    // a stored rm_member still naming this room is cleared by boot row 2.
    persistOrRetry();
    haveFinal_ = true;
    invalidateQuery();
    notice_ = n;
  }
}

bool Engine::commitTerminal(uint8_t reason, bool keepEntries) {
  if (room_.version == kFinalVersion) return false;  // already final
  RoomState next = room_;
  next.version = room_.version + 1;
  next.phase = PHASE_CLOSED;
  next.closeReason = reason;
  if (!keepEntries) next.count = 0;
  return commitState(next);
}

int Engine::allocEntry(RoomState* s, bool* evicted) {
  *evicted = false;
  if (s->count < kMaxEntries) {
    int i = s->count++;
    memset(&s->entries[i], 0, sizeof(Entry));
    return i;
  }
  int best = -1;
  for (uint8_t i = 0; i < s->count; i++) {
    if (s->entries[i].flags & ENTRY_ACTIVE) continue;
    if (best < 0 || s->entries[i].departedAt < s->entries[best].departedAt) best = i;
  }
  if (best < 0) return -1;
  memset(&s->entries[best], 0, sizeof(Entry));
  *evicted = true;
  return best;
}

bool Engine::replyAllowed(const char* id) {
  uint32_t t = now();
  int freeSlot = -1, oldest = 0;
  for (int i = 0; i < 8; i++) {
    if (replyRate_[i].id[0] == '\0') {
      if (freeSlot < 0) freeSlot = i;
      continue;
    }
    if (same(replyRate_[i].id, id)) {
      if (static_cast<uint32_t>(t - replyRate_[i].atMs) < kReplyRateMs) return false;
      replyRate_[i].atMs = t;
      return true;
    }
    if (static_cast<int32_t>(replyRate_[i].atMs - replyRate_[oldest].atMs) < 0) oldest = i;
  }
  int slot = freeSlot >= 0 ? freeSlot : oldest;
  strncpy(replyRate_[slot].id, id, kIdChars);
  replyRate_[slot].id[kIdChars] = '\0';
  replyRate_[slot].atMs = t;
  return true;
}

namespace {
void newSecret(uint32_t (*rnd)(void*), void* ctx, uint8_t out[4]) {
  bool used[10] = {false};
  for (int i = 0; i < 4; i++) {
    uint8_t d;
    do {
      d = static_cast<uint8_t>(rnd(ctx) % 10);
    } while (used[d]);
    used[d] = true;
    out[i] = d;
  }
}
}  // namespace

bool Engine::claimAccept(const char* who, const uint8_t guess[4], uint32_t roundStart, uint32_t seq, uint8_t* code) {
  if (room_.phase == PHASE_LOBBY && room_.resultRound == roundStart && roundStart != 0) {
    *code = CODE_LATE;
    return false;
  }
  if (room_.phase != PHASE_ROUND || roundStart != room_.roundStart) {
    *code = CODE_STALE_ROUND;
    return false;
  }
  if (memcmp(guess, room_.secret, 4) != 0) {
    *code = CODE_BAD_GUESS;
    return false;
  }
  RoomState next;
  if (!beginChange(&next)) {
    *code = CODE_CLOSED;
    return false;
  }
  int i = findEntry(next, who);
  if (i < 0) {
    *code = CODE_NOT_MEMBER;
    return false;
  }
  if (next.entries[i].score < 0xFFFF) next.entries[i].score++;
  if (seq) next.entries[i].lastSeq = seq;
  next.phase = PHASE_LOBBY;
  next.resultRound = room_.roundStart;
  strncpy(next.resultWinner, who, kIdChars);
  strncpy(next.startRight, who, kIdChars);
  next.startRightVersion = next.version;
  if (!commitState(next)) {
    *code = kNoReply;  // storage failure: nothing committed, the claimant resends
    return false;
  }
  *code = CODE_OK;
  return true;
}

void Engine::processRequest(const char* group, const Request& r) {
  if (!same(r.coordinator, p_.selfId)) return;
  if (commit_.valid && !same(group, commit_.group)) return;  // group isolation
  if (r.op == OP_QUERY) {
    if (replyAllowed(r.sender)) {
      sendReply(group, r.sender, r.seq, CODE_CURRENT);
      if (commit_.valid) republishScheduled_ = true;
    }
    return;
  }
  if (!commit_.valid || coordDamaged_ || r.n != commit_.n || r.nonce != commit_.nonce) {
    sendReply(group, r.sender, r.seq, CODE_UNKNOWN_ROOM);
    return;
  }
  if (commit_.closed) {
    sendReply(group, r.sender, r.seq, CODE_CLOSED);
    republishScheduled_ = true;
    return;
  }
  int ei = findEntry(room_, r.sender);
  if (ei >= 0 && r.seq <= room_.entries[ei].lastSeq) {
    sendReply(group, r.sender, r.seq, CODE_OK);  // duplicate: no effect
    return;
  }
  RoomState next;
  switch (r.op) {
    case OP_JOIN: {
      if (ei < 0 && r.basis < room_.evictionFloor) {
        sendReply(group, r.sender, r.seq, CODE_STALE_BASIS);
        return;
      }
      if (ei >= 0 && (room_.entries[ei].flags & ENTRY_ACTIVE)) {
        sendReply(group, r.sender, r.seq, CODE_OK);
        return;
      }
      if (!beginChange(&next)) {
        sendReply(group, r.sender, r.seq, CODE_CLOSED);
        return;
      }
      int i = ei;
      bool evicted = false;
      if (i < 0) i = allocEntry(&next, &evicted);
      if (i < 0) {  // FULL: durably ordered
        next.evictionFloor = next.version;
        if (commitState(next)) sendReply(group, r.sender, r.seq, CODE_FULL);
        return;
      }
      if (ei < 0) strncpy(next.entries[i].id, r.sender, kIdChars);
      next.entries[i].flags = ENTRY_ACTIVE;
      next.entries[i].lastSeq = r.seq;
      if (evicted) next.evictionFloor = next.version;
      if (commitState(next)) sendReply(group, r.sender, r.seq, CODE_OK);
      return;
    }
    case OP_LEAVE: {
      if (ei < 0 && r.basis < room_.evictionFloor) {
        sendReply(group, r.sender, r.seq, CODE_OK);  // covered by an earlier floor bump
        return;
      }
      if (!beginChange(&next)) {
        sendReply(group, r.sender, r.seq, CODE_CLOSED);
        return;
      }
      if (ei < 0) {
        next.evictionFloor = next.version;
      } else {
        Entry& e = next.entries[ei];
        if (e.flags & ENTRY_ACTIVE) {
          e.flags = 0;
          e.departedAt = next.version;
        }
        e.lastSeq = r.seq;
        if (same(next.startRight, r.sender)) {
          next.startRight[0] = '\0';
          next.startRightVersion = next.version;
        }
      }
      if (commitState(next)) sendReply(group, r.sender, r.seq, CODE_OK);
      return;
    }
    case OP_START: {
      if (ei < 0 || !(room_.entries[ei].flags & ENTRY_ACTIVE)) {
        sendReply(group, r.sender, r.seq, CODE_NOT_MEMBER);
        return;
      }
      if (room_.phase != PHASE_LOBBY || !same(startHolder(room_), r.sender) ||
          r.entitlement != room_.startRightVersion) {
        sendReply(group, r.sender, r.seq, CODE_NOT_ALLOWED);
        return;
      }
      if (!beginChange(&next)) {
        sendReply(group, r.sender, r.seq, CODE_CLOSED);
        return;
      }
      next.phase = PHASE_ROUND;
      next.roundStart = next.version;
      next.roundNo++;
      newSecret(p_.random32, p_.ctx, next.secret);
      next.entries[ei].lastSeq = r.seq;
      if (commitState(next)) sendReply(group, r.sender, r.seq, CODE_OK);
      return;
    }
    case OP_CLAIM: {
      if (ei < 0 || !(room_.entries[ei].flags & ENTRY_ACTIVE)) {
        sendReply(group, r.sender, r.seq, CODE_NOT_MEMBER);
        return;
      }
      uint8_t code = CODE_OK;
      claimAccept(r.sender, r.guess, r.roundStart, r.seq, &code);
      if (code != kNoReply) sendReply(group, r.sender, r.seq, code);  // no reply if the commit failed
      return;
    }
    default:
      return;
  }
}

void Engine::onOwnSlot(const char* group, const RoomState& s, const uint8_t* raw, size_t len) {
  (void)raw;
  (void)len;
  if (!commit_.valid || coordDamaged_ || !same(group, commit_.group)) return;
  if (s.n == commit_.n && s.nonce == commit_.nonce && s.version >= commit_.version) {
    echoPending_ = false;  // own copy observed
    return;
  }
  republishScheduled_ = true;  // an older or foreign copy occupies the slot: republish the current one
}

// =============================================================================
// Member side
// =============================================================================
void Engine::clearPending() {
  rec_.pendingOp = 0;
  okAwait_ = false;
  resendBackoffMs_ = 0;
}

void Engine::invalidateQuery() { queryOutstanding_ = false; }

// A terminal transition is complete only once NONE is stored. Until then the
// membership is kept (in RAM and on storage), nothing further is processed
// (Leave only) and the write is retried; a reboot meanwhile resumes from the
// stored record, which re-derives the same ending (the closed room's retained
// state, a resent LEAVE, a repeated verification).
bool Engine::setTerminal(Notice n) {
  MemberRecord saved = rec_;
  memset(&rec_, 0, sizeof(rec_));
  if (!persistMember()) {
    rec_ = saved;
    exitPending_ = true;
    exitNotice_ = n;
    exitRetryAtMs_ = now() + kRecordRetryMs;
    invalidateQuery();
    okAwait_ = false;
    return false;
  }
  finishTerminal(n);
  return true;
}

void Engine::finishTerminal(Notice n) {
  memset(&rec_, 0, sizeof(rec_));
  haveFinal_ = have_;  // room_ keeps the last applied snapshot as final
  have_ = false;
  frozen_ = false;
  storagePaused_ = false;
  exitPending_ = false;
  recDirty_ = false;
  invalidateQuery();
  okAwait_ = false;
  notice_ = n;
}

void Engine::sendQuery(bool verify) {
  queryVerify_ = verify;
  queryBackoffMs_ = nextBackoff(queryBackoffMs_, kQueryFirstMs, kQueryCapMs);
  queryRetryAtMs_ = now() + queryBackoffMs_;
  queryOutstanding_ = true;  // retried by tick() even if no number could be reserved now
  querySeq_ = 0;             // 0 matches no reply
  uint32_t seq = 0;
  if (!p_.nextSeq(p_.ctx, &seq)) return;  // nothing is sent with an unreserved number
  querySeq_ = seq;
  strncpy(queryGroup_, rec_.group, kGroupLen - 1);
  queryGroup_[kGroupLen - 1] = '\0';
  queryN_ = rec_.n;
  queryNonce_ = rec_.nonce;
  queryAttempt_ = attempt_;
  Request q;
  memset(&q, 0, sizeof(q));
  q.op = OP_QUERY;
  strncpy(q.coordinator, rec_.coordinator, kIdChars);
  q.n = rec_.n;
  q.nonce = rec_.nonce;
  strncpy(q.sender, p_.selfId, kIdChars);
  q.seq = querySeq_;
  sendRequest(rec_.group, q);
}

void Engine::startQuery() {
  if (!queryOutstanding_) {
    queryBackoffMs_ = 0;
    sendQuery(true);
  } else {
    queryVerify_ = true;  // an outstanding freshness query now also verifies identity
  }
}

void Engine::resendPending() {
  if (rec_.pendingOp == 0) return;
  Request r;
  memset(&r, 0, sizeof(r));
  r.op = rec_.pendingOp;
  strncpy(r.coordinator, rec_.coordinator, kIdChars);
  r.n = rec_.n;
  r.nonce = rec_.nonce;
  strncpy(r.sender, p_.selfId, kIdChars);
  r.seq = rec_.seq;
  r.basis = rec_.basis;
  r.roundStart = rec_.roundStart;
  memcpy(r.guess, rec_.guess, 4);
  r.entitlement = rec_.entitlement;
  sendRequest(rec_.group, r);
  resendBackoffMs_ = nextBackoff(resendBackoffMs_, kResendFirstMs, kResendCapMs);
  resendAtMs_ = now() + resendBackoffMs_;
}

void Engine::onMemberState(const char* group, const RoomState& s, const uint8_t* raw, size_t len) {
  if (frozen_ || exitPending_) return;  // Leave only
  if (!same(group, rec_.group)) return;
  if (s.n != rec_.n || s.nonce != rec_.nonce) {  // identity mismatch: verify, never decide from a snapshot
    startQuery();
    return;
  }
  lastProgressMs_ = now();
  if (have_) {
    if (s.version < room_.version) return;  // stale
    if (s.version == room_.version) {
      size_t n = encodeState(room_, buf_, sizeof(buf_));
      if (n != len || memcmp(buf_, raw, len) != 0) frozen_ = true;  // same version, different content
      return;
    }
  } else {
    if (s.version < rec_.appliedV) return;  // stale below the durable floor
    if (s.version == rec_.appliedV) {       // restore at the floor: already durable, no write
      applyState(s);
      return;
    }
  }
  // apply only after the floor is durable
  uint32_t old = rec_.appliedV;
  rec_.appliedV = s.version;
  if (!persistMember()) {
    rec_.appliedV = old;
    storagePaused_ = true;
    return;
  }
  applyState(s);
}

void Engine::applyState(const RoomState& s) {
  if (s.phase == PHASE_CLOSED && s.count == 0 && have_) {
    // DATA_LOST carries no entries: keep the last applied entries as final.
    room_.phase = PHASE_CLOSED;
    room_.closeReason = s.closeReason;
    setTerminal(NOTICE_ROOM_CLOSED);
    return;
  }
  room_ = s;
  have_ = true;
  storagePaused_ = false;
  lastProgressMs_ = now();
  if (okAwait_ && s.version >= okVersion_) okAwait_ = false;
  if (s.phase == PHASE_CLOSED) {
    setTerminal(NOTICE_ROOM_CLOSED);
    return;
  }
  completeFromState();
}

void Engine::completeFromState() {
  const RoomState& s = room_;
  int ei = findEntry(s, p_.selfId);
  const Entry* e = ei >= 0 ? &s.entries[ei] : nullptr;
  bool active = e && (e->flags & ENTRY_ACTIVE);
  bool changed = false;
  switch (rec_.pendingOp) {
    case OP_JOIN:
      if (active && e->lastSeq >= rec_.seq) {
        rec_.state = MS_MEMBER;
        clearPending();
        changed = true;
      } else if ((e == nullptr && s.evictionFloor > rec_.basis) || (e && !active && e->lastSeq >= rec_.seq)) {
        setTerminal(activeCount(s) >= kMaxEntries ? NOTICE_ROOM_FULL : NOTICE_COULD_NOT_JOIN);
        return;
      }
      break;
    case OP_LEAVE:
      if ((e && !active && e->lastSeq >= rec_.seq) || (e == nullptr && s.evictionFloor > rec_.basis)) {
        setTerminal(NOTICE_NONE);
        return;
      }
      break;
    case OP_START:
      if (s.roundStart > rec_.entitlement || s.resultRound > rec_.entitlement || (e && e->lastSeq >= rec_.seq)) {
        clearPending();
        changed = true;
      } else if (s.startRightVersion > rec_.entitlement && s.roundStart < rec_.entitlement) {
        clearPending();
        notice_ = NOTICE_START_REFUSED;
        changed = true;
      }
      break;
    case OP_CLAIM:
      if (s.resultRound >= rec_.roundStart || (e && e->lastSeq >= rec_.seq)) {
        clearPending();
        changed = true;
      }
      break;
    default:
      break;
  }
  if (rec_.state == MS_MEMBER && rec_.pendingOp != OP_LEAVE && !active) {
    setTerminal(NOTICE_NO_LONGER_IN_ROOM);
    return;
  }
  if (changed) persistOrRetry();
}

void Engine::onReply(const char* group, const Reply& r) {
  if (frozen_ || exitPending_) return;  // Leave only: no reply completes or ends anything
  if (rec_.state == MS_NONE || !same(r.target, p_.selfId)) return;
  if (!same(group, rec_.group) || !same(r.coordinator, rec_.coordinator)) return;
  bool curIsMine = r.curN == rec_.n && r.curNonce == rec_.nonce && r.curN != 0;
  if (r.code == CODE_CURRENT) {
    if (!queryOutstanding_ || querySeq_ == 0 || r.seq != querySeq_) return;
    if (!same(queryGroup_, rec_.group) || queryN_ != rec_.n || queryNonce_ != rec_.nonce ||
        queryAttempt_ != attempt_) {
      return;  // bound to an earlier membership
    }
    queryOutstanding_ = false;
    lastProgressMs_ = now();
    if (!curIsMine) setTerminal(NOTICE_ROOM_CLOSED);
    return;
  }
  if (rec_.pendingOp == 0 || r.seq != rec_.seq) return;  // only the durable pending request
  lastProgressMs_ = now();
  if (!curIsMine || r.code == CODE_UNKNOWN_ROOM || r.code == CODE_CLOSED) {
    setTerminal(NOTICE_ROOM_CLOSED);
    return;
  }
  uint8_t op = rec_.pendingOp;
  switch (r.code) {
    case CODE_OK:
      okAwait_ = true;
      okVersion_ = r.curVersion;
      if (have_ && room_.version >= okVersion_) {
        okAwait_ = false;
        completeFromState();
      } else {
        freshnessQueryAtMs_ = now() + kFreshnessDelayMs;
      }
      return;
    case CODE_FULL:
      if (op == OP_JOIN) setTerminal(NOTICE_ROOM_FULL);
      return;
    case CODE_STALE_BASIS:
      if (op == OP_JOIN) setTerminal(NOTICE_COULD_NOT_JOIN);
      return;
    case CODE_NOT_ALLOWED:
      if (op == OP_JOIN) {
        setTerminal(NOTICE_ROOM_CLOSED);
      } else {
        clearPending();
        if (op == OP_START) notice_ = NOTICE_START_REFUSED;
        persistOrRetry();
      }
      return;
    case CODE_LATE:
    case CODE_STALE_ROUND:
    case CODE_BAD_GUESS:
      if (op == OP_CLAIM) {
        clearPending();
        notice_ = NOTICE_LATE;
        persistOrRetry();
      }
      return;
    case CODE_NOT_MEMBER:
      if (op == OP_START || op == OP_CLAIM) {
        clearPending();
        persistOrRetry();
        freshnessQueryAtMs_ = now();
      }
      return;
    default:
      return;
  }
}

void Engine::onDiscovered(const char* group, const RoomState& s) {
  if (same(s.coordinator, p_.selfId)) return;
  int slot = -1, oldest = -1;
  for (uint8_t i = 0; i < kMaxDiscovered; i++) {
    if (disc_[i].used && same(disc_[i].coordinator, s.coordinator) && same(disc_[i].group, group)) {
      slot = i;
      break;
    }
  }
  if (slot >= 0) {
    DiscoveredRoom& d = disc_[slot];
    // per coordinator slot: keep the room with the higher (n), or newer version of the same identity
    bool sameId = d.n == s.n && d.nonce == s.nonce;
    if (sameId && s.version < d.version) return;
    if (!sameId && s.n < d.n) return;
  }
  if (s.phase == PHASE_CLOSED) {
    if (slot >= 0) disc_[slot].used = false;
    return;
  }
  bool fresh = slot < 0;
  if (slot < 0) {
    for (uint8_t i = 0; i < kMaxDiscovered; i++) {
      if (!disc_[i].used) {
        slot = i;
        break;
      }
      if (oldest < 0 || static_cast<int32_t>(disc_[i].lastSeenMs - disc_[oldest].lastSeenMs) < 0) oldest = i;
    }
    if (slot < 0) slot = oldest;
  }
  DiscoveredRoom& d = disc_[slot];
  d.used = true;
  strncpy(d.group, group, kGroupLen - 1);
  d.group[kGroupLen - 1] = '\0';
  strncpy(d.coordinator, s.coordinator, kIdChars);
  d.coordinator[kIdChars] = '\0';
  d.n = s.n;
  d.nonce = s.nonce;
  d.version = s.version;
  d.phase = s.phase;
  d.active = activeCount(s);
  d.createdUnix = s.createdUnix;
  d.lastSeenMs = now();
  if (fresh && rec_.state == MS_NONE && s.phase == PHASE_LOBBY) {
    uint32_t t = p_.unixTime(p_.ctx);
    if (t != 0 && s.createdUnix != 0 && t >= s.createdUnix && t - s.createdUnix <= kNewRoomWindowS) {
      newRoomNotify_ = true;
    }
  }
}

void Engine::reconcile(const char* group, const RoomState& s) {
  if (s.phase == PHASE_CLOSED || !isActive(s, p_.selfId) || same(s.coordinator, p_.selfId)) return;
  if (rec_.state != MS_NONE && same(rec_.coordinator, s.coordinator) && rec_.n == s.n && rec_.nonce == s.nonce) return;
  uint32_t t = now();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 4; i++) {
    if (same(reconcile_[i].id, s.coordinator)) {
      if (static_cast<uint32_t>(t - reconcile_[i].atMs) < kReconcileRateMs) return;
      slot = i;
      break;
    }
    if (reconcile_[i].id[0] == '\0' && slot < 0) slot = i;
    if (static_cast<int32_t>(reconcile_[i].atMs - reconcile_[oldest].atMs) < 0) oldest = i;
  }
  if (slot < 0) slot = oldest;
  uint32_t seq = 0;
  if (!p_.nextSeq(p_.ctx, &seq)) return;  // retried on a later state (no rate slot used)
  strncpy(reconcile_[slot].id, s.coordinator, kIdChars);
  reconcile_[slot].id[kIdChars] = '\0';
  reconcile_[slot].atMs = t;
  Request r;
  memset(&r, 0, sizeof(r));
  r.op = OP_LEAVE;
  strncpy(r.coordinator, s.coordinator, kIdChars);
  r.n = s.n;
  r.nonce = s.nonce;
  strncpy(r.sender, p_.selfId, kIdChars);
  r.seq = seq;
  r.basis = s.version;
  sendRequest(group, r);
}

// =============================================================================
// Packet entry
// =============================================================================
void Engine::onPacket(const char* group, const char* suffix, uint8_t kind, const uint8_t* payload, size_t len) {
  if (group == nullptr || suffix == nullptr) return;
  if (kind == kKindState) {
    if (strncmp(suffix, "r2/", 3) != 0) return;
    const char* topicC = suffix + 3;
    RoomState s;
    if (!decodeState(payload, len, &s) || !same(s.coordinator, topicC)) return;
    if (same(s.coordinator, p_.selfId)) {
      onOwnSlot(group, s, payload, len);
      return;
    }
    if (rec_.state != MS_NONE && same(rec_.coordinator, s.coordinator) && same(group, rec_.group)) {
      onMemberState(group, s, payload, len);
      // keep the discovery entry current too: a later re-join must not start below what was shown
      onDiscovered(group, s);
    } else {
      onDiscovered(group, s);
      reconcile(group, s);
    }
    return;
  }
  if (kind == kKindRequest) {
    if (strncmp(suffix, "r2q/", 4) != 0 || !same(suffix + 4, p_.selfId)) return;
    bool isReply = false;
    Request req;
    Reply rep;
    if (!decodeRequestOrReply(payload, len, &isReply, &req, &rep)) return;
    if (isReply) {
      onReply(group, rep);
    } else {
      processRequest(group, req);
    }
  }
}

// =============================================================================
// User actions
// =============================================================================
bool Engine::createRoom(const char* group) {
  if (rec_.state != MS_NONE || rec_.pendingOp) return false;
  if (ownOpen()) return false;
  uint32_t last = commit_.lastRoomCounter;
  if (last == kFinalVersion) {
    notice_ = NOTICE_NUMBERS_EXHAUSTED;
    return false;
  }
  uint32_t seq = 0;
  if (!reserveSeq(&seq)) return false;  // no room counter without a durably reserved number
  uint32_t n = last + 1 > seq ? last + 1 : seq;
  uint32_t nonce;
  do {
    nonce = p_.random32(p_.ctx);
  } while (nonce == 0);
  RoomState s;
  memset(&s, 0, sizeof(s));
  strncpy(s.coordinator, p_.selfId, kIdChars);
  s.n = n;
  s.nonce = nonce;
  s.version = 1;
  s.phase = PHASE_LOBBY;
  s.startRightVersion = 1;
  s.createdUnix = p_.unixTime(p_.ctx);
  s.count = 1;
  strncpy(s.entries[0].id, p_.selfId, kIdChars);
  s.entries[0].flags = ENTRY_ACTIVE;
  // creation is a new room: the commit record names it; slotSeq continues
  memset(&rec_, 0, sizeof(rec_));
  strncpy(rec_.group, group, kGroupLen - 1);
  char savedGroup[kGroupLen];
  strncpy(savedGroup, commit_.group, kGroupLen);
  strncpy(commit_.group, group, kGroupLen - 1);
  commit_.group[kGroupLen - 1] = '\0';
  bool wasDamaged = coordDamaged_;
  coordDamaged_ = false;
  if (!commitState(s)) {
    strncpy(commit_.group, savedGroup, kGroupLen);
    coordDamaged_ = wasDamaged;
    memset(&rec_, 0, sizeof(rec_));
    return false;
  }
  strncpy(rec_.coordinator, p_.selfId, kIdChars);
  rec_.n = n;
  rec_.nonce = nonce;
  rec_.appliedV = 1;
  rec_.state = MS_MEMBER;
  attempt_ = seq;
  persistOrRetry();  // derived from the committed room: boot row 1 repairs a lost write
  haveFinal_ = false;
  lastHeartbeatMs_ = now();
  return true;
}

bool Engine::join(const char* group, const char* coordinator) {
  if (rec_.state != MS_NONE || frozen_) return false;
  if (ownOpen()) return false;
  const DiscoveredRoom* d = nullptr;
  for (uint8_t i = 0; i < kMaxDiscovered; i++) {
    if (disc_[i].used && same(disc_[i].coordinator, coordinator) && same(disc_[i].group, group)) d = &disc_[i];
  }
  if (d == nullptr || d->phase == PHASE_CLOSED) return false;
  uint32_t seq = 0;
  if (!reserveSeq(&seq)) return false;
  memset(&rec_, 0, sizeof(rec_));
  strncpy(rec_.group, group, kGroupLen - 1);
  strncpy(rec_.coordinator, coordinator, kIdChars);
  rec_.n = d->n;
  rec_.nonce = d->nonce;
  rec_.appliedV = d->version;
  rec_.state = MS_JOINING;
  rec_.pendingOp = OP_JOIN;
  rec_.seq = seq;
  rec_.basis = d->version;
  if (!persistMember()) {
    memset(&rec_, 0, sizeof(rec_));
    notice_ = NOTICE_STORAGE_ERROR;
    return false;
  }
  attempt_ = seq;
  invalidateQuery();
  have_ = false;
  haveFinal_ = false;
  lastProgressMs_ = now();
  resendBackoffMs_ = 0;
  resendPending();
  return true;
}

bool Engine::leave() {
  if (rec_.state == MS_NONE) return false;
  if (exitPending_) return true;  // already ending; its NONE write is being retried
  if (isCoordinator()) {  // owner Leave: close the room for everyone
    if (!commitTerminal(CLOSE_OWNER, true)) return false;
    haveFinal_ = true;  // room_ is the committed CLOSED state
    memset(&rec_, 0, sizeof(rec_));
    // The closure is durable in the coordinator record; a stored rm_member
    // still naming this room is cleared by boot row 2 if this write is lost.
    persistOrRetry();
    invalidateQuery();
    return true;
  }
  uint32_t seq = 0;
  if (frozen_) {  // Leave only: one LEAVE, then local exit
    // The exit is stored as LEAVING (with its LEAVE) before anything is sent,
    // so a reboot before the NONE write resumes leaving instead of returning
    // to the room; the local exit itself completes only once NONE is stored.
    if (!reserveSeq(&seq)) return false;
    MemberRecord saved = rec_;
    rec_.state = MS_LEAVING;
    rec_.pendingOp = OP_LEAVE;
    rec_.seq = seq;
    rec_.basis = have_ ? room_.version : rec_.appliedV;
    if (!persistMember()) {
      rec_ = saved;
      notice_ = NOTICE_STORAGE_ERROR;
      return false;
    }
    resendPending();  // the one LEAVE (tick() sends nothing while frozen)
    setTerminal(NOTICE_NONE);
    return true;
  }
  if (rec_.state == MS_LEAVING) return true;
  if (!reserveSeq(&seq)) return false;
  MemberRecord saved = rec_;
  rec_.state = MS_LEAVING;
  rec_.pendingOp = OP_LEAVE;
  rec_.seq = seq;
  rec_.basis = have_ ? room_.version : rec_.appliedV;
  if (!persistMember()) {
    rec_ = saved;
    notice_ = NOTICE_STORAGE_ERROR;
    return false;
  }
  invalidateQuery();
  lastProgressMs_ = now();
  resendBackoffMs_ = 0;
  resendPending();
  return true;
}

bool Engine::leaveAnyway() {
  if (rec_.state != MS_LEAVING || exitPending_) return false;
  return setTerminal(NOTICE_STAYED_LISTED);  // false: pending until NONE is stored
}

bool Engine::startRound() {
  if (!canAct()) return false;
  if (isCoordinator()) {
    if (room_.phase != PHASE_LOBBY || !same(startHolder(room_), p_.selfId)) {
      notice_ = NOTICE_START_REFUSED;
      return false;
    }
    RoomState next;
    if (!beginChange(&next)) return false;
    next.phase = PHASE_ROUND;
    next.roundStart = next.version;
    next.roundNo++;
    newSecret(p_.random32, p_.ctx, next.secret);
    return commitState(next);
  }
  if (!canStart() || rec_.pendingOp) return false;
  uint32_t seq = 0;
  if (!reserveSeq(&seq)) return false;
  MemberRecord saved = rec_;
  rec_.pendingOp = OP_START;
  rec_.seq = seq;
  rec_.basis = room_.version;
  rec_.entitlement = room_.startRightVersion;
  if (!persistMember()) {
    rec_ = saved;
    notice_ = NOTICE_STORAGE_ERROR;
    return false;
  }
  lastProgressMs_ = now();
  resendBackoffMs_ = 0;
  resendPending();
  return true;
}

bool Engine::submitCorrectGuess(const uint8_t guess[4]) {
  if (!canAct()) return false;
  if (isCoordinator()) {
    uint8_t code = CODE_OK;
    return claimAccept(p_.selfId, guess, room_.roundStart, 0, &code);
  }
  const RoomState* v = view();
  if (v == nullptr || rec_.state != MS_MEMBER || v->phase != PHASE_ROUND || rec_.pendingOp) return false;
  uint32_t seq = 0;
  if (!reserveSeq(&seq)) return false;
  MemberRecord saved = rec_;
  rec_.pendingOp = OP_CLAIM;
  rec_.seq = seq;
  rec_.basis = room_.version;
  rec_.roundStart = room_.roundStart;
  memcpy(rec_.guess, guess, 4);
  if (!persistMember()) {
    rec_ = saved;
    notice_ = NOTICE_STORAGE_ERROR;
    return false;
  }
  lastProgressMs_ = now();
  resendBackoffMs_ = 0;
  resendPending();
  return true;
}

// =============================================================================
// Timers
// =============================================================================
void Engine::tick() {
  uint32_t t = now();
  const char* g = rec_.state != MS_NONE ? rec_.group : commit_.group;
  bool ready = g[0] != '\0' && p_.groupReady(p_.ctx, g);
  bool becameReady = ready && !wasReady_;
  wasReady_ = ready;

  // coordinator publishing
  if (commit_.valid && !coordDamaged_ && p_.groupReady(p_.ctx, commit_.group)) {
    bool open = !commit_.closed;
    if (becameReady && echoPending_) echoRetryAtMs_ = t;
    if (echoPending_ && due(t, echoRetryAtMs_)) {
      publishCommitted();
      echoBackoffMs_ = nextBackoff(echoBackoffMs_, kEchoFirstMs, kEchoCapMs);
      echoRetryAtMs_ = t + echoBackoffMs_;
    } else if (republishScheduled_ && static_cast<uint32_t>(t - lastRepublishMs_) >= kRepublishMinMs) {
      republishScheduled_ = false;
      publishCommitted();
    } else if (open && static_cast<uint32_t>(t - lastHeartbeatMs_) >= kHeartbeatMs) {
      lastHeartbeatMs_ = t;
      publishCommitted();
    }
  }

  // Member-record writes that failed (local storage; independent of the
  // connection): a pending exit is retried until its NONE is stored, which
  // completes it; otherwise a stale stored record catches up with rec_.
  if (exitPending_) {
    if (due(t, exitRetryAtMs_)) setTerminal(exitNotice_);  // re-arms itself on another failure
    return;  // Leave only: nothing else while the exit is pending
  } else if (recDirty_ && due(t, recRetryAtMs_)) {
    persistOrRetry();
  }
  if (rec_.state == MS_NONE || isCoordinator() || !ready) return;
  if (frozen_) return;  // Leave only: no resend, no query
  if (becameReady) {
    if (rec_.pendingOp) resendAtMs_ = t;
    freshnessQueryAtMs_ = t + kFreshnessDelayMs;
  }
  if (rec_.pendingOp && due(t, resendAtMs_)) resendPending();
  if (queryOutstanding_ && due(t, queryRetryAtMs_)) sendQuery(queryVerify_);  // replaces the outstanding query
  bool needFresh = !have_ || okAwait_ || rec_.pendingOp != 0;
  if (needFresh && !queryOutstanding_ && due(t, freshnessQueryAtMs_)) {
    queryBackoffMs_ = 0;
    sendQuery(false);
    freshnessQueryAtMs_ = t + kFreshnessPeriodMs;
  }
}

}  // namespace RaceProtocol
