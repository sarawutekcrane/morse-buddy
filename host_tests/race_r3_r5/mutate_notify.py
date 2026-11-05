#!/usr/bin/env python3
# HOST TEST (not production): one mutation at a time on a copy of
# notifications.cpp, text_message.cpp, enigma.cpp or storage_messages.cpp;
# rebuilds notify_test (fake message store) and storage_test (the ACTUAL
# storage_messages.cpp on a host directory) against the mutated copy. Each
# mutation must fail at least one of them.
import os, subprocess, sys, shutil, tempfile
H = os.path.dirname(os.path.abspath(__file__)); REPO = os.environ.get("REPO", os.path.normpath(os.path.join(H, "..", ".."))); R = REPO + "/src/core"
NT, TX, EN, SM, OB = "notifications.cpp", "text_message.cpp", "enigma.cpp", "storage_messages.cpp", "outbox.cpp"
muts = [
 # ---- reading in the chats ----
 (TX, "Text: selection does not mark read", "  markSelectedHistoryRead();  // against the complete index\n", "\n"),
 (EN, "Enigma: selection does not mark read", "  markSelectedHistoryRead();  // against the complete index\n", "\n"),
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
 (NT, "an unreadable record is skipped (recount applied anyway)", "      j.unreliable = true;\n      if (j.kind == JOB_RECOUNT) MessageStore::feedIndexRecord(", "      if (j.kind == JOB_RECOUNT) MessageStore::feedIndexRecord("),
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
 # ---- chat entry, round 2: record cache ----
 (SM, "a store write is not written through to the record cache", "void writeThroughCachedRecord(const MessageRef& ref, bool ok) {\n", "void writeThroughCachedRecord(const MessageRef& ref, bool ok) {\n  return;\n"),
 (SM, "a removed record stays in the record cache", "  dropCachedRecord(group_code, contact_key, seq);\n  afterRecordRemove(", "  afterRecordRemove("),
 (SM, "an external change keeps the record cache", "  dropAllCachedRecords();\n  g_externalMutations++;", "  g_externalMutations++;"),
 (SM, "the record cache ignores the conversation", "    if (c.valid && c.seq == seq && strcmp(c.contact, contact_key) == 0 && strcmp(c.group, group_code) == 0) return &c;", "    if (c.valid && c.seq == seq && strcmp(c.group, group_code) == 0) return &c;"),
 (SM, "drawn rows are not kept (no record cache)", "  if (!fromCache) keepCachedRecord(ref, g_loadBuffer, len);\n", ""),
 (SM, "a failed write keeps the cached bytes", "  if (!ok || g_rewriteLen == 0 || !fillCachedRecord(*c, g_rewriteScratch, g_rewriteLen)) c->valid = false;", "  if (ok && g_rewriteLen > 0 && !fillCachedRecord(*c, g_rewriteScratch, g_rewriteLen)) c->valid = false;"),
 # ---- the shared index kept current ----
 (SM, "a mutation elsewhere makes the shared index stale", "  if (keepWorking && !target) g_indexCacheGeneration = g_storageChangeGeneration;\n", ""),
 (SM, "the open conversation's own writes are not patched in (its index goes stale)", "  if (p.working && patchIndexWrite(g_index, &g_indexCount, kMaxMessagesPerThread, e)) g_indexCacheGeneration = p.generation;\n", ""),
 (SM, "a removal is not patched out of the shared index", "  if (p.working) {\n    patchIndexRemove(g_index, &g_indexCount, seq);\n    g_indexCacheGeneration = p.generation;", "  if (p.working) {\n    g_indexCacheGeneration = p.generation;"),
 (SM, "a patched entry is appended instead of sorted in", "  uint16_t at = *count;\n  for (uint16_t i = 0; i < *count; i++) {\n    if (indexEntryAfter(arr[i], e)) {", "  uint16_t at = *count;\n  for (uint16_t i = 0; i < 0; i++) {\n    if (indexEntryAfter(arr[i], e)) {"),
 (SM, "a failed write is patched in as if it succeeded", "  if (!ok || !p.active || p.generation != g_storageChangeGeneration) return;  // stale: reloaded at the next use", "  if (!p.active || p.generation != g_storageChangeGeneration) return;"),
 (SM, "a write during a stepped load is not followed", "    if (fine) g_loader.generation = p.generation;\n", ""),
 (SM, "an external change keeps every index current", "  bool known = group_code != nullptr && contact_key != nullptr;", "  bool known = true;\n  if (group_code == nullptr) { group_code = \"\"; contact_key = \"\"; }"),
 # ---- the parked index ----
 (SM, "the parked index is never swapped back", "  return g_parked != nullptr && g_parked->valid && parkedHolds(group_code, contact_key);", "  (void)group_code; (void)contact_key; return false;"),
 (SM, "the outgoing index is not parked", "  if (!isIndexTarget(group_code, contact_key)) parkWorkingIndex();\n", ""),
 (SM, "a mutation elsewhere drops the parked index", "  if (keepParked) g_parked->generation = g_storageChangeGeneration;\n", ""),
 (SM, "the parked conversation's own writes are not patched in", "  if (p.parked && patchIndexWrite(g_parked->entries, &g_parked->count, kParkedCapacity, e)) g_parked->generation = p.generation;\n", ""),
 (SM, "a parked removal is not patched out", "    patchIndexRemove(g_parked->entries, &g_parked->count, seq);\n    g_parked->generation = p.generation;", "    g_parked->generation = p.generation;"),
 # ---- the recount feeds the index ----
 (NT, "the recount does not feed the chat index", "      MessageStore::beginIndexFeed(j.group_code, j.contact_key, j.seqs, j.n, true, TextMessage::isAnyConversationOpen());\n", ""),
 (NT, "the feed ignores the open chat (replaces the index it draws from)", "      MessageStore::beginIndexFeed(j.group_code, j.contact_key, j.seqs, j.n, true, TextMessage::isAnyConversationOpen());\n", "      MessageStore::beginIndexFeed(j.group_code, j.contact_key, j.seqs, j.n, true, false);\n"),
 (NT, "an unreadable record is fed as read", "      if (j.kind == JOB_RECOUNT) MessageStore::feedIndexRecord(ref.group_code, ref.contact_key, ref.sequence, nullptr, nullptr);\n", ""),
 (SM, "a feed overwrites a current cached index", "  if (sharedInUse || (g_indexCacheValid && g_indexCacheGeneration == now) ||", "  if (sharedInUse ||"),
 (SM, "a fed record out of step is accepted", "  if (g_loader.pos >= g_loader.n || g_loaderSeqs[g_loader.pos] != seq ||\n      g_loader.generation != getStorageChangeGeneration()) {", "  if (g_loader.pos >= g_loader.n) {"),
 (SM, "no parked feed while a chat holds the shared index", "    return beginParkedFeed(group_code, contact_key, seqs, n, listingReliable);", "    return false;"),
 (SM, "a mutation elsewhere drops the parked feed", "  if (keepParkedFeed) g_parked->feedGeneration = g_storageChangeGeneration;\n", ""),
 (SM, "a mutation of the fed conversation keeps the parked feed", "  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before &&\n                        !parkedHolds(group_code, contact_key);", "  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before;"),
 (SM, "an unreadable record still completes the parked feed", "    p.valid = p.feedReliable;", "    p.valid = true;"),
 (SM, "an out-of-step record is fed to the parked index", "  if (p.feedPos >= p.feedN || p.feedSeqs[p.feedPos] != seq || p.feedGeneration != getStorageChangeGeneration()) {", "  if (p.feedPos >= p.feedN) {"),
 (SM, "the parked feed overwrites a current parked index", "  if (g_parked != nullptr && g_parked->valid && g_parked->generation == now) return false;  // current: kept\n", ""),
 (SM, "records are not fed to the parked index", "    feedParkedRecord(seq, hdr, message_id);\n    return;", "    return;"),
 # ---- bounded, time-budgeted steps; Everyone first ----
 (SM, "an index load reads a whole conversation per step", "    if (k > 0 && millis() - stepStart >= kIndexStepBudgetMs) break;\n    uint32_t seq = g_loaderSeqs[g_loader.pos++];", "    uint32_t seq = g_loaderSeqs[g_loader.pos++];"),
 (SM, "a stepped load is never cached", "  if (g_loader.reliable && g_loader.generation == g_storageChangeGeneration) {\n    g_indexCacheGeneration", "  if (false) {\n    g_indexCacheGeneration"),
 (SM, "a load that met an unreadable record is cached", "      g_loader.reliable = false;  // left out, as the one-call load did; not cached\n", ""),
 (SM, "background work never waits for a loading chat", "  return g_loader.active && !g_loader.fed && millis() - g_loader.lastStepMs < kChatLoadingMs;", "  return false;"),
 (NT, "Notifications does not pause while a chat loads", "  if (MessageStore::chatIndexLoading()) return;\n  if (g_loadState", "  if (g_loadState"),
 (NT, "a recount step ignores the time budget", "  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {\n    if (loads > 0 && millis() - stepStart >= kStepBudgetMs) return;\n    MessageRef ref;", "  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {\n    MessageRef ref;"),
 (NT, "Everyone is not visited first", "    if (!g_sweep->everyoneDone) {", "    if (false) {"),
 (NT, "the walk visits Everyone again", "    if (strcmp(contact, MessageStore::kEveryone) == 0 && isConfiguredGroup(group)) return;  // visited first\n", ""),
 # ---- input while the index loads ----
 (TX, "Text: held events bypass isReplayedSend()", "    if (isReplayedSend(e)) continue;  // Phase 2B-2: Short replayed from the Send window\n    if (backQueued", "    if (!fromHeld && isReplayedSend(e)) continue;\n    if (backQueued"),
 (TX, "Text: a queued Back waits for the rotations before it", "    if (backQueued && e.type != InputEventType::ENCODER_LONG &&\n        (droppingBeforeBack || e.type == InputEventType::ENCODER_ROTATE || g_historyCursor != kNoHistoryCursor)) {\n      droppingBeforeBack = true;\n      continue;", "    if (false) {\n      droppingBeforeBack = true;\n      continue;"),
 (TX, "Text: the selection does not follow its message", "void followCursorMessage() {\n", "void followCursorMessage() {\n  return;\n"),
 (TX, "Text: an index the store could not keep current is not reloaded", "  if (!MessageStore::isIndexCurrent(g_selectedGroupCode, g_selectedContactKey)) {\n    markIndexDirty();\n    return true;\n  }", "  if (false) {\n  }"),
 (EN, "Enigma: replayed Shorts not dropped at dispatch", "    if (isReplayedSend(e)) continue;  // Phase 2B-2: Short replayed from the Send window\n    if (backQueued", "    if (backQueued"),
 (EN, "Enigma: a queued Back waits for the rotations before it", "      droppingBeforeBack = true;  // a Back is waiting: history-side work before it is skipped\n      continue;", "      droppingBeforeBack = true;"),
 # ---- Outbox: incremental, header-only, durable ----
 (OB, "a pending write is not queued directly", "void serviceInit() { MessageStore::setOnPendingWrittenCallback(onPendingWritten); }", "void serviceInit() {}"),
 (OB, "an unreadable record does not make the pass incomplete", "        p.incomplete = true;  // unreadable (or removed since the listing): the next pass looks again\n", ""),
 (OB, "a full queue is not remembered (the rest never found)", "  if (g_queue->count >= kQueueSize) {\n    g_queueOverflowed = true;", "  if (g_queue->count >= kQueueSize) {"),
 (OB, "a failed publish clears the pending flag", "  if (!ok) {\n    g_publishRetryAtMs = millis() + kRetryMs;\n    return true;\n  }", "  if (!ok) {\n    MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_PENDING_OUTBOX);\n    dropFront();\n    return true;\n  }"),
 (OB, "a whole pass in one step (unbounded)", "constexpr uint8_t kReadsPerStep = 4;", "constexpr uint8_t kReadsPerStep = 255;"),
 (OB, "the pass skips records it found pending", "      if (hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) {\n        p.found++;\n        enqueue(ref);", "      if (hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) {\n        p.found++;"),
 # ---- review round: held-input overflow without a synchronous load ----
 (TX, "Text: input while loading is dropped, not held", "  while (!g_haveCarried && Input::popEvent(q)) {\n    if (!g_heldInput.hold(q)) {", "  while (!g_haveCarried && Input::popEvent(q)) {\n    if (true) continue;\n    if (!g_heldInput.hold(q)) {"),
 (TX, "Text: Back while loading waits for the load", "bool takeInputWhileLoading() {\n  if (Input::hasQueuedEvent(InputEventType::ENCODER_LONG)) return false;", "bool takeInputWhileLoading() {"),
 (TX, "Text: input beyond the held buffer is dropped (not carried)", "    if (!g_heldInput.hold(q)) {\n      g_carriedEvent = q;\n      g_haveCarried = true;\n    }", "    if (!g_heldInput.hold(q)) {\n    }"),
 (TX, "Text: a full input queue is not drained (a Back behind it is lost)", "  if (Input::queueFull()) {\n    // Nothing more", "  if (false) {\n    // Nothing more"),
 (TX, "Text: input dropped by the full queue is not reported", "  if (Input::droppedEventCount() != g_inputDropsSeen) g_inputLost = true;  // dropped by the full queue\n", ""),
 (TX, "Text: the loss is never shown", "    setSendStatus({\"Input lost while loading\", true});", "    (void)0;"),
 (TX, "Text: overflow completes the index synchronously (as before)", "      g_carriedEvent = q;\n      g_haveCarried = true;\n    }\n  }\n  if (Input::queueFull()) {\n    // Nothing more", "      g_carriedEvent = q;\n      g_haveCarried = true;\n      MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey);\n    }\n  }\n  if (Input::queueFull()) {\n    // Nothing more"),
 (TX, "Text: a mid-batch reload is completed synchronously", "      refreshIndexIfNeeded();\n      indexRefreshElapsed += millis() - midRefreshStart;", "      while (g_indexDirty) refreshIndexIfNeeded();\n      indexRefreshElapsed += millis() - midRefreshStart;"),
 (TX, "Text: the final reload is completed synchronously", "  refreshIndexIfNeeded();  // one bounded step\n  indexRefreshElapsed += millis() - finalRefreshStart;", "  while (g_indexDirty) refreshIndexIfNeeded();\n  indexRefreshElapsed += millis() - finalRefreshStart;"),
 (TX, "Text: an event met by a mid-batch reload is lost (not retained)", "      if (g_indexDirty) {\n        retain(e, fromHeld);\n        break;", "      if (g_indexDirty) {\n        break;"),
 (EN, "Enigma: input while loading is dropped, not held", "  while (!g_haveCarried && Input::popEvent(q)) {\n    if (!g_heldInput.hold(q)) {", "  while (!g_haveCarried && Input::popEvent(q)) {\n    if (true) continue;\n    if (!g_heldInput.hold(q)) {"),
 (EN, "Enigma: Back while loading waits for the load", "bool takeInputWhileLoading() {\n  if (Input::hasQueuedEvent(InputEventType::ENCODER_LONG)) return false;", "bool takeInputWhileLoading() {"),
 (EN, "Enigma: input beyond the held buffer is dropped (not carried)", "    if (!g_heldInput.hold(q)) {\n      g_carriedEvent = q;\n      g_haveCarried = true;\n    }", "    if (!g_heldInput.hold(q)) {\n    }"),
 (EN, "Enigma: a full input queue is not drained (a Back behind it is lost)", "  if (Input::queueFull()) {\n    while (Input::popEvent(q)) {", "  if (false) {\n    while (Input::popEvent(q)) {"),
 (EN, "Enigma: the loss is never shown", "    setSendStatus({\"Input lost while loading\", true});", "    (void)0;"),
 (EN, "Enigma: the final reload is completed synchronously", "  refreshIndexIfNeeded();  // one bounded step\n  if (g_indexDirty) {\n    Display::drawStatusBar();", "  while (g_indexDirty) refreshIndexIfNeeded();\n  if (g_indexDirty) {\n    Display::drawStatusBar();"),
 (EN, "Enigma: an event met by a mid-batch reload is lost (not retained)", "      if (g_indexDirty) {\n        retain(e);\n        break;", "      if (g_indexDirty) {\n        break;"),
 # ---- review round: Outbox publication of a record it cannot read ----
 (OB, "an unreadable queued record is dropped (as before)", "    if (confirmedAbsent(ref)) {\n      removeAt(k);", "    if (true) {\n      removeAt(k);"),
 (OB, "a record confirmed gone is retried forever", "    if (confirmedAbsent(ref)) {\n      removeAt(k);", "    if (false) {\n      removeAt(k);"),
 (OB, "a failed listing is taken as confirmed absence", "  return reliable && !listed;", "  return !listed;"),
 (OB, "an unreadable queued record is retried without backoff", "    e.retryAtMs = millis() + backoff;", "    e.retryAtMs = millis();"),
 (OB, "the pass runs again and again (not once per boot)", "    bool due = externalChange || g_queueOverflowed || (!g_passDone && !g_passAgain) ||", "    bool due = true || externalChange || g_queueOverflowed || (!g_passDone && !g_passAgain) ||"),
 (OB, "the pass ignores changes made outside the store", "    bool externalChange = MessageStore::getExternalMutationCount() != g_externalAtPassStart;", "    bool externalChange = false;"),
 (OB, "an incomplete first pass is repeated at once (no pause)", "    bool due = externalChange || g_queueOverflowed || (!g_passDone && !g_passAgain) ||", "    bool due = externalChange || g_queueOverflowed || !g_passDone ||"),
 (OB, "an incomplete pass is repeated without a growing pause", "    g_passRetryMs = g_passRetryMs * 2 < kPassRetryMaxMs ? g_passRetryMs * 2 : kPassRetryMaxMs;\n", ""),
 (OB, "a record queued while offline is dropped", "    if (g_queue->count > 1) moveToBack(k);\n    g_publishRetryAtMs = millis() + kRetryMs;\n    return false;", "    removeAt(k);\n    g_publishRetryAtMs = millis() + kRetryMs;\n    return false;"),
]
# Survivors assessed equivalent (no behaviour a test can observe), with the reason.
equivalent = {
 "the feed ignores the open chat (replaces the index it draws from)": "a current open-chat index is still protected by the cache check, and a stale one is reloaded by the chat's per-pass re-sync (CE23) anyway: the flag only saves that reload (cost, not correctness)",
 "an unreadable record is fed as read": "without the explicit nullptr feed the next fed record is out of step, which drops the feed; either way the fed index is never used and the chat loads itself",
}
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

def run(binary, args):
    try:
        r = subprocess.run([binary] + args, capture_output=True, text=True, timeout=300)
        return r.returncode, [l for l in r.stdout.splitlines() if "FAIL" in l]
    except subprocess.TimeoutExpired:
        return 124, ["TIMEOUT"]

import concurrent.futures, threading
only = sys.argv[1:]
todo = [m for m in muts if not only or any(o in m[1] for o in only)]
_print_lock = threading.Lock()

def one(k, mut):
    target, name, old, new = mut
    out = os.path.join(H, "out", "mutn%d" % k)
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out + "/core")
    for x in (NT, TX, EN, SM, OB, "notifications.h", "storage_messages.h", "held_input.h"): shutil.copy(R + "/" + x, out + "/core/" + x)
    p = out + "/core/" + target; src = open(p).read()
    open(p, "w").write(src.replace(old, new))
    inc = sum([["-I", i] for i in (H, H + "/fake", out, REPO + "/src")], [])
    b1 = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS"] + inc + [H + "/notify_test.cpp", out + "/core/" + EN, stubsFake] + common + ["-o", out + "/n"], capture_output=True, text=True)
    incfs = sum([["-I", i] for i in (H + "/fake_fs", H, H + "/fake", out, REPO + "/src")], [])
    b2 = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS", "-DREAL_MESSAGE_STORE"] + incfs + [H + "/storage_test.cpp", out + "/core/" + EN, out + "/core/" + SM, out + "/core/" + OB, stubsReal, lfs] + common + ["-o", out + "/s"], capture_output=True, text=True)
    if b1.returncode or b2.returncode:
        with _print_lock: print(f"  COMPILE-ERR  {name}: {(b1.stderr or b2.stderr)[:300]}", flush=True)
        shutil.rmtree(out, ignore_errors=True)
        return name, None
    fsroot = tempfile.mkdtemp(prefix="mutn_fs_")
    rc1, f1 = run(out + "/n", [])
    rc2, f2 = run(out + "/s", [fsroot])
    shutil.rmtree(fsroot, ignore_errors=True)
    shutil.rmtree(out, ignore_errors=True)
    ok = rc1 != 0 or rc2 != 0
    first = (f1 or f2 or ["-"])[0][:100]
    tag = 'CAUGHT ' if ok else (f'SURVIVED (assessed equivalent: {equivalent[name]})' if name in equivalent else 'SURVIVED')
    with _print_lock: print(f"  {tag} [notify:{len(f1)} storage:{len(f2)}]  {target}: {name}   (first: {first})", flush=True)
    return name, ok

caught, survivors, build_errors = 0, [], 0
workers = int(os.environ.get("MUT_JOBS", "3"))
with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
    for name, ok in ex.map(lambda km: one(*km), enumerate(todo)):
        if ok is None: build_errors += 1
        elif ok: caught += 1
        else: survivors.append(name)
n = len(todo)
eq = len([x for x in survivors if x in equivalent])
print(f"{caught}/{n} mutations caught; {eq} survivor(s) assessed equivalent; {len(survivors) - eq} unexplained survivor(s); {build_errors} build error(s)")
sys.exit(1 if (len(survivors) - eq) or build_errors else 0)
