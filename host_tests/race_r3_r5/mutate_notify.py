#!/usr/bin/env python3
# HOST TEST (not production): one mutation at a time on a copy of
# notifications.cpp, text_message.cpp, enigma.cpp or storage_messages.cpp;
# rebuilds notify_test (fake message store) and storage_test (the ACTUAL
# storage_messages.cpp on a host directory) against the mutated copy. Each
# mutation must fail at least one of them.
import os, subprocess, sys, shutil, tempfile
H = os.path.dirname(os.path.abspath(__file__)); REPO = os.environ.get("REPO", os.path.normpath(os.path.join(H, "..", ".."))); R = REPO + "/src/core"
NT, TX, EN, SM = "notifications.cpp", "text_message.cpp", "enigma.cpp", "storage_messages.cpp"
muts = [
 # ---- reading in the chats ----
 (TX, "Text: selection does not mark read", "  if (hadEvent) g_chatRenderDirty = true;\n  markSelectedHistoryRead();\n", "  if (hadEvent) g_chatRenderDirty = true;\n"),
 (EN, "Enigma: selection does not mark read", "  if (hadEvent) g_enigmaChatDirty = true;\n  markSelectedHistoryRead();\n", "  if (hadEvent) g_enigmaChatDirty = true;\n"),
 (TX, "Text: every message in the chat marked read on entry", "    g_readCheckDone = false;  // a selection after (re)entry is checked afresh\n",
  "    g_readCheckDone = false;  // a selection after (re)entry is checked afresh\n    for (uint16_t k = 0; k < MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey); k++) Notifications::readUpTo(g_selectedGroupCode, g_selectedContactKey, k, MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey));\n"),
 # ---- categories ----
 (NT, "GAME record does not light the Text badge", "    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0 || s.unread[SLOT_GAME] > 0)) return true;", "    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0)) return true;"),
 (NT, "categories merged again (any unread lights every badge)", "    if (s.state == SUMMARY_FREE) continue;  // COUNTED: counts; UNRESOLVED: category flags -- same test\n", "    if (s.state == SUMMARY_FREE) continue;\n    if (totalUnread(s) > 0 && (mask & (BADGE_TEXT | BADGE_ENIGMA))) return true;\n"),
 (NT, "clear decrements the wrong category", "  int slot = slotForType(view.envelope.message_type);\n  if (slot >= 0 && s->unread[slot] > 0) {", "  int slot = SLOT_GAME;\n  if (slot >= 0 && s->unread[slot] > 0) {"),
 (NT, "arrival counted under a guessed category when the stored type is known", "    int slot = loaded ? slotForType(view.envelope.message_type) : -1;", "    int slot = loaded ? SLOT_GAME : -1;"),
 # ---- read up to the selection (hardware finding 1 on 23293c5) ----
 (NT, "read-up ignores the message type", "  if (a.type != type || !sameConversation(", "  if (!sameConversation("),
 (NT, "read-up ignores the sequence (a later arrival with an older timestamp is read)", "  if (sequence > a.sequence) return false;\n", ""),
 (NT, "read-up reads newer messages too", "  if (sortTs != a.sortTs) return sortTs < a.sortTs;", "  if (sortTs != a.sortTs) return true;"),
 (NT, "read-up never saved (lost at reboot)", "  r.items[r.count++] = add;\n  readUpsChanged();\n  return true;", "  r.items[r.count++] = add;\n  return true;"),
 (NT, "read-up written to NVS on the encoder path", "  r.items[r.count++] = add;\n  readUpsChanged();\n  return true;", "  r.items[r.count++] = add;\n  writeReadUps();\n  return true;"),
 (NT, "read-up rewrites the flags on the encoder path", "  if (!addReadUp(r)) return;  // covered by an earlier selection\n",
  "  if (!addReadUp(r)) return;  // covered by an earlier selection\n  MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);\n"),
 (NT, "read-up attempt ends after one pass even if a rewrite failed", "  if (left && j.pass + 1 < kReadUpPasses) {", "  if (false) {"),
 (NT, "read-up marked done while a read or rewrite is still unsuccessful", "  if (left) {\n    ReadUp* x = findReadUp(j.readUp);", "  if (false) {\n    ReadUp* x = findReadUp(j.readUp);"),
 (NT, "a failing read-up is retried at once, every time (no backoff; starves the other work)", "      if (r.done || (r.retryAtMs != 0 && static_cast<int32_t>(now - r.retryAtMs) < 0)) continue;", "      if (r.done) continue;"),
 (NT, "a failing read-up is never retried", "      if (r.done || (r.retryAtMs != 0 && static_cast<int32_t>(now - r.retryAtMs) < 0)) continue;", "      if (r.done || r.retryAtMs != 0) continue;"),
 (NT, "badge waits for the background (no immediate count)", "  if (s->state == SUMMARY_COUNTED) {\n    if (s->unread[slot] == remaining) return;\n    s->unread[slot] = remaining;\n  } else {\n    s->unread[slot] = remaining > 0 ? 1 : 0;\n  }", "  (void)remaining;"),
 (NT, "recount ignores pending read-ups", "      if (slot >= 0 && !coveredByPendingReadUp(j.group_code, j.contact_key, view.header, view.envelope, ref.sequence))", "      if (slot >= 0)"),
 (NT, "a read-up does not restart a recount under way", "  conversationChanged(group_code, contact_key);  // a recount under way may have counted what is now read\n", ""),
 # ---- loading, migration, saving ----
 (NT, "migration stores an approximate count of zero (badges of the old mask lost)", "      markUnresolved(&s, mask != 0 ? mask : static_cast<uint8_t>(BADGE_TEXT | BADGE_ENIGMA));", "      s.state = SUMMARY_COUNTED; (void)mask;"),
 (NT, "legacy read failure taken as an empty migration", "  if (!migrateLegacyInto(dst)) {\n    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_RETRY_LEGACY;\n  }", "  if (!migrateLegacyInto(dst)) {\n    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_OK;\n  }"),
 (NT, "unreadable summary2 replaced from the legacy record", "    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_RETRY_CURRENT;\n  }", "    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    if (migrateLegacyInto(dst)) { *migrated = true; return LOAD_OK; }\n    return LOAD_RETRY_CURRENT;\n  }"),
 (NT, "saves while the stored record is unreadable", "  if (g_saveDirty && g_loadState == LOAD_OK && static_cast<int32_t>(now - g_saveDueMs) >= 0) {", "  if (g_saveDirty && static_cast<int32_t>(now - g_saveDueMs) >= 0) {"),
 (NT, "a failed save is not retried", "  Serial.println(\"[NOTIFY] unread summary not saved; retrying\");\n  g_saveDueMs = millis() + kRetryMs;", "  g_saveDirty = false;"),
 (NT, "a failing save is retried every step (starves the other work)", "  if (g_saveDirty && g_loadState == LOAD_OK && static_cast<int32_t>(now - g_saveDueMs) >= 0) {", "  if (g_saveDirty && g_loadState == LOAD_OK) {"),
 (NT, "conversations touched while unreadable are not merged", "      if (t != nullptr) markUnresolved(t, mask);", "      (void)t;"),
 (NT, "conversations the loaded record lacks are never added", "    if (set[i].state == SUMMARY_FREE || (set[i].state == SUMMARY_COUNTED && totalUnread(set[i]) == 0)) {", "    if (false) {"),
 (NT, "counts loaded at boot are trusted (not verified)", "  } else {\n    verifyLoaded(g_summaries);\n  }", "  }"),
 (NT, "counts loaded by a recovery retry are trusted (not verified)", "    if (!migrated) verifyLoaded(loaded);\n", ""),
 (NT, "verification drops the badges until the recount", "markUnresolved(&set[i], 0);", "{ memset(set[i].unread, 0, sizeof(set[i].unread)); set[i].state = SUMMARY_UNRESOLVED; }"),
 # ---- background work ----
 (NT, "UNRESOLVED conversations are never recounted", "      if (s.state != SUMMARY_UNRESOLVED) continue;\n      g_reconcileCursor", "      continue;\n      g_reconcileCursor"),
 (NT, "a recount that gave way is retried at once (no backoff)", "  if (g_recountFailures == 0 || static_cast<int32_t>(now - g_recountRetryMs) >= 0) {", "  if (true) {"),
 (NT, "a read in an UNRESOLVED conversation guesses a decrement", "  if (s == nullptr || s->state == SUMMARY_UNRESOLVED) return;  // an UNRESOLVED one is recounted in the background", "  if (s == nullptr) return;"),
 (NT, "eviction keeps the stale counts", "  if (s == nullptr) return;  // not listed: nothing to correct (the sweep covers the rest)\n  markUnresolved(s, 0);", "  if (s == nullptr) return;  // not listed: nothing to correct (the sweep covers the rest)"),
 (NT, "no sweep after a load at boot", "    verifyLoaded(g_summaries);\n  }\n  startSweep();", "    verifyLoaded(g_summaries);\n  }"),
 (NT, "no sweep after a recovery retry", "    g_loadState = LOAD_OK;\n    startSweep();", "    g_loadState = LOAD_OK;"),
 (NT, "the sweep skips every listed conversation (zero-count ones too)", "  if (r == MessageStore::WalkResult::FOUND) {\n", "  if (r == MessageStore::WalkResult::FOUND) {\n    if (findSummary(group, contact) != nullptr) return;\n"),
 (NT, "a failed sweep walk ends the sweep (no further pass)", "  if (r == MessageStore::WalkResult::FAILED) g_sweep->incomplete = true;", ""),
 (NT, "a pass left incomplete is not followed by another", "  if (!g_sweep->incomplete) {\n    delete g_sweep;", "  if (true) {\n    delete g_sweep;"),
 (NT, "the sweep only recounts (no trimming)", "    g_job = newJob(JOB_TRIM, group, contact);", "    g_job = newJob(JOB_RECOUNT, group, contact);"),
 (NT, "the sweep never adds an unlisted conversation", "  if (s == nullptr && total > 0) s = findOrCreateSummary(j.group_code, j.contact_key);\n", ""),
 (NT, "an unreliable listing is taken as complete", "    j.listed = reliable;\n", "    j.listed = true; reliable = true;\n"),
 # ---- trimming to the retained history (approved 300 -> 100) ----
 (NT, "trim deletes the newest records instead of the oldest", "    ref.sequence = j.seqs[j.pos];\n    StoredMessageView view;\n    if (!MessageStore::loadMessage(ref, &view)) {\n      // Not readable", "    ref.sequence = j.seqs[j.n - 1 - j.pos];\n    StoredMessageView view;\n    if (!MessageStore::loadMessage(ref, &view)) {\n      // Not readable"),
 (NT, "trim keeps one record too many", "                     ? static_cast<uint16_t>(j.n - MessageStore::kRetainedMessagesPerThread)", "                     ? static_cast<uint16_t>(j.n - MessageStore::kRetainedMessagesPerThread - 1)"),
 (NT, "trim deletes pending-outbox records", "    if (view.header.flags & MessageStore::FLAG_PENDING_OUTBOX) {\n      j.pos++;  // kept", "    if (false) {\n      j.pos++;  // kept"),
 (NT, "trim uncounts before the deletion succeeded", "    if (!MessageStore::deleteMessage(ref)) {", "    if (counted) { Summary* s0 = findSummary(j.group_code, j.contact_key); if (s0 != nullptr && s0->unread[slot] > 0) s0->unread[slot]--; counted = false; }\n    if (!MessageStore::deleteMessage(ref)) {"),
 (NT, "trim never uncounts a deleted unread record", "    if (counted) {  // uncounted only now that it is gone", "    if (false) {"),
 (NT, "a failed deletion is skipped (the next record deleted instead)", "      Serial.println(\"[NOTIFY] trim: a record could not be deleted; retried later\");\n      giveWay(j);\n      continueWithRecount(j);\n      return;", "      j.pos++;\n      return;"),
 (NT, "a failed deletion is not retried in a later pass", "      Serial.println(\"[NOTIFY] trim: a record could not be deleted; retried later\");\n      giveWay(j);", "      Serial.println(\"[NOTIFY] trim: a record could not be deleted; retried later\");"),
 (NT, "an unreadable record is skipped by the trim", "        j.restart = true;\n      }\n      return;", "        j.pos++;\n      }\n      return;"),
 (NT, "the open conversation is trimmed", "  if (TextMessage::isConversationOpen(j.group_code, j.contact_key)) {", "  if (false) {"),
 (NT, "several deletions per step", "    return;  // at most one deletion per step", "    continue;"),
 (NT, "the walk reads every directory entry in one step", "constexpr uint8_t kWalkEntriesPerStep = 16;", "constexpr uint8_t kWalkEntriesPerStep = 255;"),
 (NT, "an unreadable record is skipped (recount applied anyway)", "      j.unreliable = true;\n      continue;", "      continue;"),
 (NT, "an arrival does not restart a recount under way", "                      uint8_t badgeMask, bool conversationCurrentlyOpen) {\n  conversationChanged(group_code, contact_key);", "                      uint8_t badgeMask, bool conversationCurrentlyOpen) {"),
 (NT, "a whole conversation per step (unbounded)", "constexpr uint8_t kLoadsPerStep = 4;", "constexpr uint8_t kLoadsPerStep = 255;"),
 (NT, "several rewrites per step", "      j.rewrites++;\n      return;  // at most one rewrite per step", "      j.rewrites++;"),
 # ---- message store listing ----
 (SM, "appends keep the earlier 300-record retention", "  if (msgCount >= kRetainedMessagesPerThread) {", "  if (msgCount >= kMaxMessagesPerThread) {"),
 (SM, "a truncated listing is reported complete", "  if (outReliable != nullptr) *outReliable = !openFailed && !truncated;", "  if (outReliable != nullptr) *outReliable = !openFailed;"),
 (SM, "a truncated listing keeps the first entries found, not the oldest", "        if (seq < outSeqs[hi]) outSeqs[hi] = seq;", "        (void)hi;"),
 (SM, "a predicate scan of a truncated listing is reported complete", "    if (openFailed || truncated) reliable = false;", "    if (openFailed) reliable = false;"),
 (NT, "a truncated listing stops the trim", "    if (j.kind == JOB_TRIM && truncated) reliable = true;\n", ""),
 (SM, "the walk ignores its per-step bound", "  for (uint8_t n = 0; n < maxEntries; n++) {", "  for (uint16_t n = 0; n < 60000; n++) {"),
 (SM, "the walk stops at the first group", "      w->group.close();\n      w->inGroup = false;\n      continue;", "      w->group.close();\n      w->inGroup = false;\n      return WalkResult::END;"),
 (SM, "deleteMessage leaves the record on flash", "bool deleteMessage(const MessageRef& ref) {", "bool deleteMessage(const MessageRef& ref) {\n  (void)ref; return true;"),
 (SM, "listing counts an interrupted rewrite's .tmp as a second record", "    if (!isDir && len > 4 && strcmp(name + len - 4, \".msg\") == 0) {", "    if (!isDir) {"),
]
out = os.path.join(H, "out", "mutn")
_bad = [m[1] for m in muts if open(R + "/" + m[0]).read().count(m[2]) != 1]
if _bad:
    print("BAD MUTATION ANCHORS (source changed): " + "; ".join(_bad)); sys.exit(2)
objdir = os.path.join(H, "out", "mutnobj"); shutil.rmtree(objdir, ignore_errors=True); os.makedirs(objdir)
def cc(src, extra, name):
    o = os.path.join(objdir, name + ".o")
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-c"] + extra + [src, "-o", o], capture_output=True, text=True)
    if b.returncode: print("COMMON BUILD FAILED " + src + "\n" + b.stderr[:500]); sys.exit(2)
    return o
base = ["-DREAL_NOTIFICATIONS", "-I", H, "-I", H + "/fake", "-I", REPO + "/src"]
common = [cc(s, base, os.path.basename(s)) for s in (R + "/number_guessing.cpp", R + "/packet_codec.cpp", R + "/morse.cpp", R + "/enigma_crypto.cpp",
                                                     R + "/enigma_keys.cpp", R + "/crc32.cpp", R + "/presence.cpp")]
stubsFake = cc(H + "/stubs.cpp", base, "stubs_fake")
fsBase = ["-DREAL_NOTIFICATIONS", "-DREAL_MESSAGE_STORE", "-I", H + "/fake_fs", "-I", H, "-I", H + "/fake", "-I", REPO + "/src"]
stubsReal = cc(H + "/stubs.cpp", fsBase, "stubs_real")
lfs = cc(H + "/fake_fs/LittleFS.cpp", ["-I", H + "/fake_fs"], "littlefs")
fsroot = tempfile.mkdtemp(prefix="mutn_fs_")

def run(binary, args):
    try:
        r = subprocess.run([binary] + args, capture_output=True, text=True, timeout=300)
        return r.returncode, [l for l in r.stdout.splitlines() if "FAIL" in l]
    except subprocess.TimeoutExpired:
        return 124, ["TIMEOUT"]

caught, survivors, build_errors = 0, [], 0
only = sys.argv[1:]
for target, name, old, new in muts:
    if only and not any(o in name for o in only): continue
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out + "/core")
    for x in (NT, TX, EN, SM, "notifications.h", "storage_messages.h"): shutil.copy(R + "/" + x, out + "/core/" + x)
    p = out + "/core/" + target; s = open(p).read()
    open(p, "w").write(s.replace(old, new))
    inc = sum([["-I", i] for i in (H, H + "/fake", out, REPO + "/src")], [])
    b1 = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS"] + inc + [H + "/notify_test.cpp", out + "/core/" + EN, stubsFake] + common + ["-o", out + "/n"], capture_output=True, text=True)
    incfs = sum([["-I", i] for i in (H + "/fake_fs", H, H + "/fake", out, REPO + "/src")], [])
    b2 = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS", "-DREAL_MESSAGE_STORE"] + incfs + [H + "/storage_test.cpp", out + "/core/" + EN, out + "/core/" + SM, stubsReal, lfs] + common + ["-o", out + "/s"], capture_output=True, text=True)
    if b1.returncode or b2.returncode:
        print(f"  COMPILE-ERR  {name}: {(b1.stderr or b2.stderr)[:300]}"); build_errors += 1; continue
    rc1, f1 = run(out + "/n", [])
    rc2, f2 = run(out + "/s", [fsroot])
    ok = rc1 != 0 or rc2 != 0; caught += ok
    if not ok: survivors.append(name)
    first = (f1 or f2 or ["-"])[0][:100]
    print(f"  {'CAUGHT ' if ok else 'SURVIVED'} [notify:{len(f1)} storage:{len(f2)}]  {target}: {name}   (first: {first})", flush=True)
shutil.rmtree(fsroot, ignore_errors=True)
n = len([m for m in muts if not only or any(o in m[1] for o in only)])
print(f"{caught}/{n} mutations caught; {len(survivors)} survivor(s); {build_errors} build error(s)")
sys.exit(1 if survivors or build_errors else 0)
