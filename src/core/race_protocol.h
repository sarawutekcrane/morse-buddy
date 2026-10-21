#pragma once
// Race room protocol engine (design revision 8): fixed coordinator, room
// identity (C, n, nonce), durable coordinator commit (two slots + commit
// record) and member record, identity verification, request processing.
//
// Instance-based with all I/O injected (RaceProtocol::Platform), so the
// firmware runs exactly one Engine (race.cpp) while host tests run several
// against a simulated broker and a fault-injecting NVS. The engine never
// touches the display, input or MQTT directly.

#include <stddef.h>
#include <stdint.h>

namespace RaceProtocol {

constexpr size_t kIdChars = 12;         // device id: 12 hex chars
constexpr size_t kGroupLen = 33;        // group code + NUL
constexpr uint8_t kMaxEntries = 20;
constexpr uint8_t kFmt = 1;
constexpr uint8_t kKindState = 12;      // PacketCodec kinds (packet_codec.h)
constexpr uint8_t kKindRequest = 13;
constexpr uint32_t kFinalVersion = 0xFFFFFFFFu;  // reserved for a terminal CLOSED

constexpr size_t kStateFixedBytes = 78;
constexpr size_t kEntryBytes = 23;
constexpr size_t kStateMaxBytes = kStateFixedBytes + kEntryBytes * kMaxEntries;  // 538
constexpr size_t kRequestBytes = 54;
constexpr size_t kReplyBytes = 43;

enum Phase : uint8_t { PHASE_LOBBY = 0, PHASE_ROUND = 1, PHASE_CLOSED = 2 };
enum CloseReason : uint8_t { CLOSE_NONE = 0, CLOSE_OWNER = 1, CLOSE_DATA_LOST = 2, CLOSE_EXHAUSTED = 3 };
enum Op : uint8_t { OP_JOIN = 1, OP_LEAVE = 2, OP_START = 3, OP_CLAIM = 4, OP_QUERY = 5, OP_REPLY = 6 };
enum Code : uint8_t {
  CODE_OK = 0, CODE_FULL, CODE_NOT_MEMBER, CODE_NOT_ALLOWED, CODE_STALE_BASIS, CODE_STALE_ROUND,
  CODE_LATE, CODE_BAD_GUESS, CODE_CLOSED, CODE_UNKNOWN_ROOM, CODE_CURRENT
};
enum EntryFlag : uint8_t { ENTRY_ACTIVE = 1 };

struct Entry {
  char id[kIdChars + 1];
  uint16_t score;
  uint8_t flags;
  uint32_t lastSeq;
  uint32_t departedAt;
};

struct RoomState {
  char coordinator[kIdChars + 1];
  uint32_t n;
  uint32_t nonce;
  uint32_t version;
  uint8_t phase;
  uint8_t closeReason;
  uint16_t roundNo;
  uint32_t roundStart;
  uint8_t secret[4];
  char startRight[kIdChars + 1];  // empty: the coordinator holds the start right
  uint32_t startRightVersion;
  uint32_t resultRound;
  char resultWinner[kIdChars + 1];
  uint32_t evictionFloor;
  uint32_t createdUnix;
  uint8_t count;
  Entry entries[kMaxEntries];
};

struct Request {
  uint8_t op;
  char coordinator[kIdChars + 1];
  uint32_t n;
  uint32_t nonce;
  char sender[kIdChars + 1];
  uint32_t seq;
  uint32_t basis;
  uint32_t roundStart;
  uint8_t guess[4];
  uint32_t entitlement;
};

struct Reply {
  char coordinator[kIdChars + 1];
  char target[kIdChars + 1];
  uint32_t seq;
  uint8_t code;
  uint32_t curN, curNonce, curVersion;  // the coordinator's CURRENT room (all zero = none)
};

// ---- Codecs (payload only; the 6-byte PacketCodec header is added by the
// engine when publishing). Exact sizes; every decode is bounds-checked.
size_t encodeState(const RoomState& s, uint8_t* out, size_t cap);
bool decodeState(const uint8_t* in, size_t len, RoomState* out);
size_t encodeRequest(const Request& r, uint8_t* out, size_t cap);
size_t encodeReply(const Reply& r, uint8_t* out, size_t cap);
// Decodes a kind-13 payload into either a request or a reply (by op).
bool decodeRequestOrReply(const uint8_t* in, size_t len, bool* isReply, Request* req, Reply* rep);

int findEntry(const RoomState& s, const char* id);
bool isActive(const RoomState& s, const char* id);
uint8_t activeCount(const RoomState& s);
const char* startHolder(const RoomState& s);  // startRight, or the coordinator

// ---- Injected platform ------------------------------------------------------
struct Platform {
  void* ctx;
  const char* selfId;  // 12 hex chars
  // Publishes payload with the given PacketCodec kind (header added inside).
  bool (*publish)(void* ctx, const char* group, const char* topicSuffix, uint8_t kind, const uint8_t* payload,
                  uint16_t len, bool retained);
  bool (*groupReady)(void* ctx, const char* group);
  // Durable storage (Preferences semantics: write returns false on failure;
  // read returns the stored length, 0 if absent/unreadable).
  bool (*nvsWrite)(void* ctx, const char* key, const void* data, size_t len);
  size_t (*nvsRead)(void* ctx, const char* key, void* buf, size_t cap);
  uint32_t (*millis)(void* ctx);
  uint32_t (*random32)(void* ctx);
  // Reserves the next durable, monotonic sequence number (Identity's
  // counter). False -- and no number -- if it could not be stored or is
  // exhausted; nothing may be sent with a number that was not reserved.
  bool (*nextSeq)(void* ctx, uint32_t* out);
  uint32_t (*unixTime)(void* ctx);
};

enum MemberState : uint8_t { MS_NONE = 0, MS_JOINING = 1, MS_MEMBER = 2, MS_LEAVING = 3 };

// Short, user-visible outcomes of the last terminal transition (for the
// Room's status row). Cleared by the UI once shown.
enum Notice : uint8_t {
  NOTICE_NONE = 0, NOTICE_ROOM_FULL, NOTICE_COULD_NOT_JOIN, NOTICE_ROOM_CLOSED, NOTICE_NO_LONGER_IN_ROOM,
  NOTICE_START_REFUSED, NOTICE_LATE, NOTICE_STORAGE_ERROR, NOTICE_DATA_DAMAGED, NOTICE_NUMBERS_EXHAUSTED,
  NOTICE_STAYED_LISTED
};

struct DiscoveredRoom {
  bool used;
  char group[kGroupLen];
  char coordinator[kIdChars + 1];
  uint32_t n, nonce, version;
  uint8_t phase;
  uint8_t active;
  uint32_t createdUnix;
  uint32_t lastSeenMs;
};
constexpr uint8_t kMaxDiscovered = 4;

// Header fields of a validated coordinator slot record (internal).
struct SlotInfo {
  bool valid;
  char group[kGroupLen];
  uint32_t slotSeq, n, nonce, version;
  uint8_t phase;
};

class Engine {
 public:
  void init(const Platform& p);
  // Loads coordinator and member records and applies the boot rules.
  void boot();
  // Packet entry point: `suffix` is the topic after the group
  // ("r2/<C>" or "r2q/<X>"); `group` is the connection's group.
  void onPacket(const char* group, const char* suffix, uint8_t kind, const uint8_t* payload, size_t len);
  // Timers: request resend, verification retry, freshness query, publish
  // retry, heartbeat. Call often (every loop tick).
  void tick();

  // ---- user actions (return false if refused; see notice()) ----
  bool createRoom(const char* group);
  bool join(const char* group, const char* coordinator);  // from the discovery list
  bool leave();          // member: LEAVE request; coordinator: close the room (owner Leave)
  bool leaveAnyway();    // member LEAVING: stop waiting (warned)
  bool startRound();     // coordinator (local) or member (START request)
  // A locally verified correct guess for the current round.
  bool submitCorrectGuess(const uint8_t guess[4]);

  // ---- view ----
  MemberState state() const { return static_cast<MemberState>(rec_.state); }
  bool hasRoom() const { return rec_.state != MS_NONE; }
  bool isCoordinator() const;          // this device coordinates the recorded room
  const RoomState* view() const;       // confirmed room state, or nullptr
  const char* roomGroup() const { return rec_.group; }
  bool reconnecting() const { return rec_.state != MS_NONE && !isCoordinator() && !have_; }
  bool verifying() const { return queryOutstanding_ && queryVerify_; }
  bool storagePaused() const { return storagePaused_ || exitPending_; }
  // A terminal transition (room ended, Leave completed, Leave anyway) whose
  // NONE record could not be written yet: the room is kept (Leave only, no
  // further processing) and the write is retried; the exit completes, with
  // its notice, only once it is durable.
  bool exitPending() const { return exitPending_; }
  bool frozen() const { return frozen_; }
  bool waitingForCoordinator() const;
  bool pendingOp(uint8_t* op) const;
  bool claimPending(uint32_t roundStart) const;
  bool canAct() const { return !frozen_ && !storagePaused_ && !exitPending_; }  // otherwise Leave only
  bool isMeActive() const;
  bool canStart() const;               // this device holds the start right now
  Notice notice() const { return notice_; }
  void clearNotice() { notice_ = NOTICE_NONE; }
  bool coordinatorDamaged() const { return coordDamaged_; }
  const DiscoveredRoom* discovered(uint8_t i) const { return (i < kMaxDiscovered && disc_[i].used) ? &disc_[i] : nullptr; }
  // A room appeared recently enough to notify about (consumed).
  bool takeNewRoomNotification();
  // Last closed room's final snapshot (kept for display after NONE).
  const RoomState* finalView() const { return (haveFinal_ && rec_.state == MS_NONE) ? &room_ : nullptr; }

  // Test/diagnostic access.
  uint32_t durableAppliedV() const { return rec_.appliedV; }
  uint32_t outstandingQuerySeq() const { return queryOutstanding_ ? querySeq_ : 0; }
  uint32_t pendingSeq() const { return rec_.pendingOp ? rec_.seq : 0; }

  // Coordinator record bytes, for size checks.
  static constexpr size_t kCommitRecordBytes = 76;
  static constexpr size_t kSlotRecordBytes = 599;
  static constexpr size_t kMemberRecordBytes = 88;

 private:
  struct MemberRecord {
    char group[kGroupLen];
    char coordinator[kIdChars + 1];
    uint32_t n, nonce, appliedV;
    uint8_t state;
    uint8_t pendingOp;
    uint32_t seq, basis, roundStart;
    uint8_t guess[4];
    uint32_t entitlement;
  };
  struct CommitRecord {
    bool valid;
    char group[kGroupLen];
    uint32_t n, nonce, version;
    uint8_t slotId;
    uint32_t slotSeq;
    bool closed;
    uint32_t lastRoomCounter;
  };

  // The one room held in RAM (section 9): C's own open room, else the
  // applied member state, else the final snapshot of the last room. An own
  // terminal room is published from its slot on demand.
  RoomState room_;

  // coordinator side
  CommitRecord commit_;          // mirror of rc_commit (identity of C's own room)
  bool coordDamaged_ = false;    // FC-B2
  bool echoPending_ = false;
  uint32_t echoRetryAtMs_ = 0;
  uint32_t echoBackoffMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  uint32_t lastRepublishMs_ = 0;
  bool republishScheduled_ = false;
  struct ReplyRate { char id[kIdChars + 1]; uint32_t atMs; };
  ReplyRate replyRate_[8];

  // member side
  MemberRecord rec_;
  uint32_t attempt_ = 0;
  bool have_ = false;            // room_ holds the applied member state
  bool storagePaused_ = false;
  bool frozen_ = false;
  bool exitPending_ = false;     // terminal transition awaiting its NONE write
  Notice exitNotice_ = NOTICE_NONE;
  uint32_t exitRetryAtMs_ = 0;
  bool recDirty_ = false;        // rec_ differs from the stored record (a non-terminal write failed)
  uint32_t recRetryAtMs_ = 0;
  bool queryOutstanding_ = false;
  bool queryVerify_ = false;     // the outstanding query verifies a mismatched identity
  uint32_t querySeq_ = 0;
  char queryGroup_[kGroupLen];
  uint32_t queryN_ = 0, queryNonce_ = 0, queryAttempt_ = 0;
  uint32_t queryRetryAtMs_ = 0, queryBackoffMs_ = 0;
  uint32_t resendAtMs_ = 0, resendBackoffMs_ = 0;
  uint32_t lastProgressMs_ = 0;
  uint32_t freshnessQueryAtMs_ = 0;
  uint32_t okVersion_ = 0;       // REPLY OK curVersion awaiting an RS >= it
  bool okAwait_ = false;
  bool wasReady_ = false;
  Notice notice_ = NOTICE_NONE;
  bool haveFinal_ = false;       // room_ holds the final snapshot (state NONE)
  struct LeaveRate { char id[kIdChars + 1]; uint32_t atMs; };
  LeaveRate reconcile_[4];

  DiscoveredRoom disc_[kMaxDiscovered];
  bool newRoomNotify_ = false;

  Platform p_;
  uint8_t buf_[kSlotRecordBytes];  // shared: serialization, slot staging, on-demand slot load

  // helpers
  uint32_t now() const { return p_.millis(p_.ctx); }
  bool same(const char* a, const char* b) const;
  bool sendRequest(const char* group, const Request& r);
  bool sendReply(const char* group, const char* target, uint32_t seq, uint8_t code);
  bool publishCommitted();
  bool persistMember();
  void persistOrRetry();         // writes rec_; on failure marks it for retry (tick)
  bool reserveSeq(uint32_t* out);
  void clearPending();
  bool setTerminal(Notice n);    // false: exit pending (NONE not yet durable)
  void finishTerminal(Notice n);
  void invalidateQuery();
  void startQuery();
  void sendQuery(bool verify);
  void resendPending();

  // coordinator
  bool ownOpen() const { return commit_.valid && !commit_.closed && !coordDamaged_; }
  void ownClosed(Notice n);      // C's own room became terminal while C was its member
  bool loadCoordinator();
  bool loadSlot(uint8_t id, SlotInfo* out);
  bool slotBound(const SlotInfo& s) const;
  bool writeCommit(const CommitRecord& c);
  bool commitState(const RoomState& next);
  bool commitTerminal(uint8_t reason, bool keepEntries);
  bool beginChange(RoomState* next);  // copy + version bump (false if exhausted -> closed)
  void processRequest(const char* group, const Request& r);
  bool claimAccept(const char* who, const uint8_t guess[4], uint32_t roundStart, uint32_t seq, uint8_t* code);
  int allocEntry(RoomState* s, bool* evicted);
  void onOwnSlot(const char* group, const RoomState& s, const uint8_t* raw, size_t len);
  bool replyAllowed(const char* id);

  // member
  void onMemberState(const char* group, const RoomState& s, const uint8_t* raw, size_t len);
  void applyState(const RoomState& s);
  void completeFromState();
  void onReply(const char* group, const Reply& r);
  void onDiscovered(const char* group, const RoomState& s);
  void reconcile(const char* group, const RoomState& s);
};

}  // namespace RaceProtocol
