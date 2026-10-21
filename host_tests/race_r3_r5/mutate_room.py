#!/usr/bin/env python3
# HOST TEST (not production): one mutation at a time on a copy of race.cpp or
# race_protocol.cpp; rebuilds the protocol suite (proto_test), the Room UI
# suite (room_test) and the history-browsing suite (race_test) against the
# mutated copy. Each mutation must fail some suite.
import os, subprocess, sys, shutil
H = os.path.dirname(os.path.abspath(__file__)); REPO = os.environ.get("REPO", os.path.normpath(os.path.join(H, "..", ".."))); R = REPO + "/src/core"
UI, PR, ID = "race.cpp", "race_protocol.cpp", "identity.cpp"
muts = [
 # ---- race.cpp: R1 / R2 (carried over) ----
 (UI, "R2 removed: Start Round stays on the Room", "    if (isRoundActive()) {  // the coordinator's own Start", "    if (false) {  // the coordinator's own Start"),
 (UI, "R2: navigation not reported (rest of batch handled by the Room)", "      enterGuessScreen();\n      return true;\n    }\n    return false;  // a member's START", "      enterGuessScreen();\n      return false;\n    }\n    return false;  // a member's START"),
 (UI, "R2: no replay window for Start Round", "    // The window also covers Start Round -> Guess, as for Continue below.\n    openRaceShortWindow(e.eventMs);\n", ""),
 (UI, "R1: rotation ignored", "      moveRoomFocus(e.value);\n", "      (void)0;\n"),
 (UI, "R1: Short on a participant performs the action", "      bool away = (row >= 0) ? handleRoomRowShort(row, e) : handleRoomAction(e);", "      bool away = handleRoomAction(e); (void)row;"),
 (UI, "R1: rotation wraps past the last row", "  if (next >= static_cast<int16_t>(g_participantCount)) next = static_cast<int16_t>(g_participantCount) - 1;", "  if (next >= static_cast<int16_t>(g_participantCount)) next = -1;"),
 (UI, "R1: window does not follow downward focus", "  if (focus >= 0 && rows > 0 && focus >= g_roomListTop + rows) g_roomListTop = static_cast<uint8_t>(focus - rows + 1);\n", ""),
 (UI, "R1: window does not follow upward focus", "  if (focus >= 0 && focus < g_roomListTop) g_roomListTop = static_cast<uint8_t>(focus);\n", ""),
 (UI, "R1: focus kept by index, not identity", "    if (strcmp(g_participants[i], g_roomFocusId) == 0) {\n      g_roomFocusIndex = i;\n      return i;\n    }", "    if (i == g_roomFocusIndex) {\n      strncpy(g_roomFocusId, g_participants[i], kDeviceIdLen - 1);\n      return i;\n    }"),
 (UI, "R1: missing row drops focus instead of clamping", "  uint8_t i = (g_roomFocusIndex < g_participantCount) ? g_roomFocusIndex : static_cast<uint8_t>(g_participantCount - 1);\n  strncpy", "  resetRoomFocus();\n  return -1;\n  uint8_t i = (g_roomFocusIndex < g_participantCount) ? g_roomFocusIndex : static_cast<uint8_t>(g_participantCount - 1);\n  strncpy"),
 (UI, "R1: focus not reset on Room entry", "    resetRoomFocus();  // every Room visit starts on the action row\n", ""),
 (UI, "R1: action keeps its marker while a row is focused", "  if (focus >= 0) hasAction = false;\n", ""),
 (UI, "R1: no range on the title", "  if (g_participantCount > rows && visibleCount > 0 && listed > 0) {", "  if (false) {"),
 (UI, "R1: row marker not drawn", "  const char* marker = focused ? \"> \" : \"\";", "  const char* marker = \"\"; (void)focused;"),
 (UI, "R1: rows not capped by the available area", "  if (visibleCount > rows) visibleCount = rows;\n", "  if (visibleCount > kMaxRoomRows) visibleCount = kMaxRoomRows;\n"),
 # ---- race.cpp: R3-R5 UI ----
 (UI, "Back changes membership (Leave on Back)", "  closeRaceShortWindow();  // Phase 2C-1: Race is left", "  g_engine->leave();\n  closeRaceShortWindow();  // Phase 2C-1: Race is left"),
 (UI, "Leave row acts without confirmation", "    startLeaveConfirm();\n    return true;", "    leaveConfirmYes();\n    return false;"),
 (UI, "owner prompt does not say it closes the room", "\"Leave room?\", \"Closes it for everyone\"", "\"Leave room?\", nullptr"),
 (UI, "scores not drawn beside names", "  bool scored = g_rowKind[i] == RoomRowKind::PARTICIPANT || g_rowKind[i] == RoomRowKind::FINAL;", "  bool scored = false;"),
 (UI, "no Leave row", "    addRow(kLeaveRowId, RoomRowKind::LEAVE, 0);\n", ""),
 (UI, "frozen state not shown", "    if (g_engine->frozen()) return \"Room data inconsistent\";\n", ""),
 (UI, "storage pause not shown", "    if (g_engine->storagePaused()) return \"Storage error - room paused\";\n", ""),
 (UI, "reconnecting not shown on the status row", "    if (g_engine->reconnecting()) return \"Reconnecting...\";\n", ""),
 (UI, "waiting-for-coordinator not shown", "    if (g_engine->waitingForCoordinator()) {", "    if (false) {"),
 (UI, "non-holder offered Start Round", "    if (g_engine->canStart()) {\n      *hasAction = true;", "    if (true) {\n      *hasAction = true;"),
 (UI, "a frozen member may still guess", "  if (g_engine == nullptr || v == nullptr || g_roundId[0] == '\\0' || g_engine->state() != RaceProtocol::MS_MEMBER ||\n      !g_engine->canAct()) {", "  if (g_engine == nullptr || v == nullptr || g_roundId[0] == '\\0' || g_engine->state() != RaceProtocol::MS_MEMBER) {"),
 (UI, "no automatic Guess entry on a new round", "    if (g_engine->state() == RaceProtocol::MS_MEMBER && g_engine->canAct()) g_pendingAutoEnterGuess = true;\n", ""),
 (UI, "another member's win not shown", "      g_pendingExternalSolve = true;\n", ""),
 (UI, "discovered row does not join", "    if (!g_engine->join(g_groupCode, coordinator)) setRoomActionUnconfirmed(\"Couldn't join\");", "    setRoomActionUnconfirmed(\"Couldn't join\");"),
 (UI, "no notification for a new room", "    Notifications::setRaceInvitePending(true);\n", ""),
 (UI, "engine notices never shown", "  const char* t = noticeText(n);", "  const char* t = nullptr; (void)noticeText;"),
 (UI, "Ready gate removed", "  if (MqttManager::isGroupReady(g_groupCode)) return true;", "  return true;"),
 (UI, "a correct guess sends no claim", "  if (!g_engine->submitCorrectGuess(digits)) {", "  if (false) {"),
 (UI, "the result never reaches the Guess screen (no sync there)", "  syncRound();  // a result or a new round confirmed since the last tick\n", ""),
 # ---- race_protocol.cpp ----
 (PR, "model-11: equal-version restore writes NVS", "    if (s.version == rec_.appliedV) {       // restore at the floor: already durable, no write\n      applyState(s);\n      return;\n    }\n", ""),
 (PR, "model-12: freeze not enforced (later RS applied)", "  if (frozen_ || exitPending_) return;  // Leave only\n  if (!same(group, rec_.group)) return;", "  if (exitPending_) return;  // Leave only\n  if (!same(group, rec_.group)) return;"),
 (PR, "same-version conflict not detected", "      if (n != len || memcmp(buf_, raw, len) != 0) frozen_ = true;  // same version, different content", "      (void)n;"),
 (PR, "G2: applied despite a failed appliedV write", "    rec_.appliedV = old;\n    storagePaused_ = true;\n    return;", "    rec_.appliedV = old;\n    storagePaused_ = true;"),
 (PR, "G1: identity mismatch decided from the snapshot", "  if (s.n != rec_.n || s.nonce != rec_.nonce) {  // identity mismatch: verify, never decide from a snapshot\n    startQuery();", "  if (s.n != rec_.n || s.nonce != rec_.nonce) {\n    setTerminal(NOTICE_ROOM_CLOSED);"),
 (PR, "QUERY binding ignored", "    if (!same(queryGroup_, rec_.group) || queryN_ != rec_.n || queryNonce_ != rec_.nonce ||\n        queryAttempt_ != attempt_) {", "    if (false) {"),
 (PR, "QUERY not invalidated by Join", "  attempt_ = seq;\n  invalidateQuery();\n  have_ = false;", "  attempt_ = seq;\n  have_ = false;"),
 (PR, "STALE_BASIS JOIN reissued", "      if (op == OP_JOIN) setTerminal(NOTICE_COULD_NOT_JOIN);", "      if (op == OP_JOIN) resendPending();"),
 (PR, "no duplicate gate (seq <= lastSeq)", "  if (ei >= 0 && r.seq <= room_.entries[ei].lastSeq) {", "  if (false) {"),
 (PR, "non-member LEAVE does not raise the floor", "      if (ei < 0) {\n        next.evictionFloor = next.version;", "      if (ei < 0) {\n        (void)0;"),
 (PR, "START entitlement not checked", "          r.entitlement != room_.startRightVersion) {", "          false) {"),
 (PR, "winner Leave keeps the start right", "          next.startRight[0] = '\\0';\n", ""),
 (PR, "eviction ignores departedAt order", "    if (best < 0 || s->entries[i].departedAt < s->entries[best].departedAt) best = i;", "    best = i;"),
 (PR, "commit record not written (slot only)", "  if (!writeCommit(c)) {", "  if (false) {"),
 (PR, "FC-A writes no DATA_LOST", "    commitTerminal(CLOSE_DATA_LOST, false);  // at V+1 (at most 0xFFFFFFFF)\n", ""),
 (PR, "FC-B1 without the consecutive check", "    if (consecutive && (caseA || caseB)) {", "    if (caseA || caseB) {"),
 (PR, "4b: own open room does not repair rm_member", "  if (ownOpen()) {\n    if (!(rec_.state == MS_MEMBER && namesOwn", "  if (false) {\n    if (!(rec_.state == MS_MEMBER && namesOwn"),
 (PR, "DATA_LOST replaces the member's final scores", "  if (s.phase == PHASE_CLOSED && s.count == 0 && have_) {", "  if (false) {"),
 (PR, "exhaustion uses the reserved version", "  if (room_.version >= kFinalVersion - 1) {", "  if (room_.version >= kFinalVersion) {"),
 (PR, "reply's current room not checked", "  if (!curIsMine || r.code == CODE_UNKNOWN_ROOM || r.code == CODE_CLOSED) {", "  if (r.code == CODE_UNKNOWN_ROOM || r.code == CODE_CLOSED) {"),
 (PR, "reply group not checked", "  if (!same(group, rec_.group) || !same(r.coordinator, rec_.coordinator)) return;", "  if (!same(r.coordinator, rec_.coordinator)) return;"),
 (PR, "discovery not refreshed while a member (soak finding)", "      onDiscovered(group, s);\n    } else {", "    } else {"),
 (PR, "echo not awaited (no republish)", "      publishCommitted();\n      echoBackoffMs_ = nextBackoff(echoBackoffMs_, kEchoFirstMs, kEchoCapMs);", "      echoBackoffMs_ = nextBackoff(echoBackoffMs_, kEchoFirstMs, kEchoCapMs);"),
 (PR, "pending request not resent", "  if (rec_.pendingOp && due(t, resendAtMs_)) resendPending();\n", ""),
 # ---- review findings (durable numbers, freeze, durable exits) ----
 (ID, "F1: identity counter write result ignored", "  if (p.putULong(\"raceSeq\", next) != sizeof(uint32_t)) return false;  // not durably reserved", "  p.putULong(\"raceSeq\", next);"),
 (ID, "F4: Race shares nextId()'s counter again (no isolation)", "  if (!readStored(p, \"raceSeq\", &current)) return false;  // unreadable: never restart from a default", "  if (!readStored(p, \"idCounter\", &current)) return false;  // shared counter"),
 (ID, "F4: Race writes nextId()'s counter again (no isolation)", "  if (p.putULong(\"raceSeq\", next) != sizeof(uint32_t)) return false;  // not durably reserved", "  if (p.putULong(\"idCounter\", next) != sizeof(uint32_t)) return false;"),
 (UI, "F7: repeated init allocates a new engine", "  if (g_engine != nullptr) return;\n  g_engine = new (std::nothrow) RaceProtocol::Engine();", "  g_engine = new (std::nothrow) RaceProtocol::Engine();"),
 (UI, "F7: repeated init re-boots the existing engine", "  if (g_engine != nullptr) return;\n  g_engine = new (std::nothrow) RaceProtocol::Engine();\n  if (g_engine == nullptr) return;", "  if (g_engine == nullptr) g_engine = new (std::nothrow) RaceProtocol::Engine();\n  if (g_engine == nullptr) return;"),
 (UI, "F7: no guard in the room-state handler", "  if (g_engine == nullptr) return;\n  g_engine->onPacket(group_code, topic, RaceProtocol::kKindState", "  g_engine->onPacket(group_code, topic, RaceProtocol::kKindState"),
 (UI, "F7: no guard in the request handler", "  if (g_engine == nullptr) return;\n  g_engine->onPacket(group_code, topic, RaceProtocol::kKindRequest", "  g_engine->onPacket(group_code, topic, RaceProtocol::kKindRequest"),
 (UI, "F7: no guard in the service tick", "  if (g_engine == nullptr) return;\n  g_engine->tick();", "  g_engine->tick();"),
 (UI, "F7: no guard in syncRound", "void syncRound() {\n  if (g_engine == nullptr) return;", "void syncRound() {"),
 (UI, "F7: no guard building the rows", "  g_participantCount = 0;\n  if (g_engine == nullptr) return;", "  g_participantCount = 0;"),
 (UI, "F7: no guard in the room action", "  if (g_engine == nullptr) return false;  // \"Race unavailable\": no action\n", ""),
 (UI, "F7: no 'Race unavailable' text", "  if (g_engine == nullptr) return \"Race unavailable\";", "  if (g_engine == nullptr) return \"Invite to Play\";"),
 (UI, "F7: no guard in shownRoom", "const RaceProtocol::RoomState* shownRoom() {\n  if (g_engine == nullptr) return nullptr;", "const RaceProtocol::RoomState* shownRoom() {"),
 (ID, "F6: readStored accepts disagreeing reads", "  if (a != b) return false;\n", ""),
 (ID, "F6: readStored infers failure from the defaults (previous helper)", "  uint32_t b = p.getULong(key, 0xFFFFFFFFu);\n  if (a != b) return false;", "  if (a == 0 && p.getULong(key, 0xFFFFFFFFu) == 0xFFFFFFFFu) return false;"),
 (ID, "F5: no migration at boot (lazy only)", "  raceFloorStored(Storage::core());  // migrate before anything can call nextId(); retried there if it fails\n", ""),
 (ID, "F5: nextId() changes idCounter before the floor is stored", "  if (raceFloorStored(p)) p.putULong(\"idCounter\", counter);", "  raceFloorStored(p);\n  p.putULong(\"idCounter\", counter);"),
 (ID, "F5: migration from an unreadable idCounter (default 0)", "  if (p.isKey(\"idCounter\") && !readStored(p, \"idCounter\", &floor)) return false;  // unreadable: retry later", "  if (p.isKey(\"idCounter\")) floor = p.getULong(\"idCounter\", 0);"),
 (ID, "F5: unreadable raceSeq restarts at 0", "  if (!readStored(p, \"raceSeq\", &current)) return false;  // unreadable: never restart from a default", "  current = p.getULong(\"raceSeq\", 0);"),
 (ID, "F5: reservation without the stored floor", "  if (!raceFloorStored(p)) return false;  // migration not stored yet (retried on every call)", "  raceFloorStored(p);"),
 (PR, "F1: engine ignores a failed reservation (requests)", "  if (p_.nextSeq(p_.ctx, out)) return true;\n  notice_ = NOTICE_STORAGE_ERROR;\n  return false;", "  p_.nextSeq(p_.ctx, out);\n  return true;"),
 (PR, "F1: QUERY sent with an unreserved number", "  if (!p_.nextSeq(p_.ctx, &seq)) return;  // nothing is sent with an unreserved number", "  p_.nextSeq(p_.ctx, &seq);"),
 (PR, "F2: replies processed while frozen", "  if (frozen_ || exitPending_) return;  // Leave only: no reply completes or ends anything", "  if (exitPending_) return;"),
 (PR, "F2: timer activity while frozen", "  if (frozen_) return;  // Leave only: no resend, no query\n", ""),
 (PR, "F3: terminal shown before NONE is stored", "  if (!persistMember()) {\n    rec_ = saved;\n    exitPending_ = true;", "  if (!persistMember()) {\n    finishTerminal(n);\n    return true;\n    rec_ = saved;\n    exitPending_ = true;"),
 (PR, "F3: pending exit never retried", "    if (due(t, exitRetryAtMs_)) setTerminal(exitNotice_);  // re-arms itself on another failure", "    (void)exitRetryAtMs_;"),
 (PR, "F3: frozen Leave not stored as LEAVING first", "    if (!persistMember()) {\n      rec_ = saved;\n      notice_ = NOTICE_STORAGE_ERROR;\n      return false;\n    }\n    resendPending();  // the one LEAVE", "    resendPending();  // the one LEAVE"),
 (PR, "F3: stale member record never retried", "  } else if (recDirty_ && due(t, recRetryAtMs_)) {\n    persistOrRetry();\n  }", "  }"),
 (PR, "F3: completion write failure not marked for retry", "  if (persistMember()) return;\n  recDirty_ = true;", "  if (persistMember()) return;\n  return;\n  recDirty_ = true;"),
 (UI, "F3: refused Leave not reported", "  if (!ok) setRoomActionUnconfirmed(\"Storage error - try again\");", "  (void)ok;"),
 (UI, "F1: platform reservation ignores Identity's result", "bool platformNextSeq(void*, uint32_t* out) { return Identity::reserveCounter(out); }", "bool platformNextSeq(void*, uint32_t* out) { Identity::reserveCounter(out); return true; }"),
 (PR, "member state applied without a durable floor write", "  rec_.appliedV = s.version;\n  if (!persistMember()) {", "  rec_.appliedV = s.version;\n  if (false) {"),
]
# Redundant guards (defence in depth required by design r8 section 6.2): every
# change of the member record already clears the outstanding QUERY
# (setTerminal, leave, boot/init), and join() is reachable only from NONE,
# where no QUERY is outstanding. Assessed equivalent from source, not proven.
# "F5: reservation without the stored floor": the migration call still runs,
# and with raceSeq absent readStored() reports it unreadable, so the
# reservation is refused anyway (the explicit check is redundant).
equivalent = {"QUERY binding ignored", "QUERY not invalidated by Join", "F5: reservation without the stored floor"}
out = os.path.join(H, "out", "mutr"); caught = 0; survivors = []
# Unmutated objects are compiled once.
objdir = os.path.join(H, "out", "mutobj"); os.makedirs(objdir, exist_ok=True)
common = {}
build_errors = 0
for src in (H + "/stubs.cpp", H + "/race_stubs.cpp", R + "/number_guessing.cpp", R + "/packet_codec.cpp", R + "/presence.cpp", R + "/crc32.cpp"):
    o = os.path.join(objdir, os.path.basename(src) + ".o")
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-c", "-DREAL_IDENTITY", "-I", H, "-I", H + "/fake_sys", "-I", H + "/fake", "-I", REPO + "/src", src, "-o", o], capture_output=True, text=True)
    if b.returncode: print("COMMON BUILD FAILED " + src + "\n" + b.stderr[:500]); sys.exit(2)
    common[os.path.basename(src) + ".o"] = o
def build_run(srcs, incs, outbin, env=None):
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_IDENTITY"] + sum([["-I", i] for i in incs], []) + srcs + ["-o", outbin], capture_output=True, text=True)
    if b.returncode: return None, ["COMPILE-ERR " + b.stderr[:300]]
    try:
        r = subprocess.run([outbin], capture_output=True, text=True, timeout=120, env=env)
    except subprocess.TimeoutExpired:
        return 124, ["TIMEOUT"]
    return r.returncode, [l for l in r.stdout.splitlines() if "FAIL" in l]
# Every anchor must match exactly once before anything is built.
_bad = [m[1] for m in muts if open(R + "/" + m[0]).read().count(m[2]) != 1]
if _bad:
    print("BAD MUTATION ANCHORS (source changed): " + "; ".join(_bad)); sys.exit(2)
only = sys.argv[1:]
for target, name, old, new in muts:
    if only and not any(o in name for o in only): continue
    if os.path.exists(out): shutil.rmtree(out)
    os.makedirs(out + "/core")
    for x in ("race.cpp", "race_protocol.cpp", "race_protocol.h", "menu.cpp", "menu.h", "identity.cpp", "identity.h"): shutil.copy(R + "/" + x, out + "/core/" + x)
    p = out + "/core/" + target; s = open(p).read()
    if s.count(old) != 1: print(f"BAD MUTATION (anchor x{s.count(old)}): {name}"); sys.exit(2)
    open(p, "w").write(s.replace(old, new))
    inc = [H, H + "/fake_sys", H + "/fake", out, REPO + "/src"]
    P = out + "/core/race_protocol.cpp"
    po = out + "/race_protocol.o"
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-c", "-I", out, "-I", REPO + "/src", P, "-o", po], capture_output=True, text=True)
    if b.returncode: print(f"  COMPILE-ERR  {name}: {b.stderr[:200]}"); build_errors += 1; continue
    io = out + "/identity.o"
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-c", "-I", H + "/fake_sys", "-I", H + "/fake", "-I", out, "-I", REPO + "/src", out + "/core/identity.cpp", "-o", io], capture_output=True, text=True)
    if b.returncode: print(f"  COMPILE-ERR  {name}: {b.stderr[:200]}"); build_errors += 1; continue
    ui = [common[k] for k in ("stubs.cpp.o", "race_stubs.cpp.o", "number_guessing.cpp.o", "packet_codec.cpp.o", "presence.cpp.o", "crc32.cpp.o")] + [po, io]
    res = {
      "proto": build_run([H + "/proto_test.cpp", po, common["crc32.cpp.o"]], [out, REPO + "/src"], out + "/p"),
      "room": build_run([H + "/room_test.cpp"] + ui, inc, out + "/m"),
      "browse": build_run([H + "/race_test.cpp"] + ui, inc, out + "/b"),
      "identity": build_run([H + "/identity_test.cpp", out + "/core/identity.cpp"], [H + "/fake_sys", H + "/fake", out, REPO + "/src"], out + "/i"),
    }
    if any(rc is None for rc, _ in res.values()):
        print(f"  COMPILE-ERR  {name}: " + " | ".join(k + ":" + fl[0][:200] for k, (rc, fl) in res.items() if rc is None)); build_errors += 1; continue
    ok = any(rc != 0 for rc, _ in res.values()); caught += ok
    if not ok: survivors.append(name)
    summary = " ".join(f"{k}:{len(fl)}" for k, (rc, fl) in res.items())
    first = next((fl[0][:100] for rc, fl in res.values() if fl), "-")
    tag = 'CAUGHT ' if ok else ('SURVIVED (assessed equivalent: redundant guard)' if name in equivalent else 'SURVIVED')
    print(f"  {tag} [{summary}]  {target}: {name}   (first: {first})", flush=True)
n = len([m for m in muts if not only or any(o in m[1] for o in only)])
eq = len([x for x in survivors if x in equivalent])
print(f"{caught}/{n} mutations caught; {eq} survivor(s) assessed equivalent; {len(survivors) - eq} unexplained survivor(s); {build_errors} build error(s)")
sys.exit(1 if (len(survivors) - eq) or build_errors else 0)
