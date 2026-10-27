#!/usr/bin/env python3
# HOST TEST (not production): one mutation at a time on a copy of
# notifications.cpp, text_message.cpp or enigma.cpp; rebuilds notify_test
# (ACTUAL menu.cpp + notifications.cpp + text_message.cpp, linked enigma.cpp)
# against the mutated copy. Each mutation must fail it.
import os, subprocess, sys, shutil
H = os.path.dirname(os.path.abspath(__file__)); REPO = os.environ.get("REPO", os.path.normpath(os.path.join(H, "..", ".."))); R = REPO + "/src/core"
NT, TX, EN = "notifications.cpp", "text_message.cpp", "enigma.cpp"
muts = [
 (TX, "Text: selection does not mark read", "  if (hadEvent) g_chatRenderDirty = true;\n  markSelectedHistoryRead();\n", "  if (hadEvent) g_chatRenderDirty = true;\n"),
 (EN, "Enigma: selection does not mark read", "  if (hadEvent) g_enigmaChatDirty = true;\n  markSelectedHistoryRead();\n", "  if (hadEvent) g_enigmaChatDirty = true;\n"),
 (TX, "Text: every message in the chat marked read on entry", "    g_readCheckDone = false;  // a selection after (re)entry is checked afresh\n",
  "    g_readCheckDone = false;  // a selection after (re)entry is checked afresh\n    for (auto& x : T::store) if (!strcmp(x.ref.contact_key, g_selectedContactKey)) Notifications::clearUnread(g_selectedGroupCode, g_selectedContactKey, x.ref);\n"),
 (NT, "GAME record does not light the Text badge", "    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0 || s.unread[SLOT_GAME] > 0)) return true;", "    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0)) return true;"),
 (NT, "categories merged again (any unread lights every badge)", "    if (s.state == SUMMARY_FREE) continue;  // COUNTED: counts; UNRESOLVED: category flags -- same test\n", "    if (s.state == SUMMARY_FREE) continue;\n    if (totalUnread(s) > 0 && (mask & (BADGE_TEXT | BADGE_ENIGMA))) return true;\n"),
 (NT, "clear decrements the wrong category", "  int slot = slotForType(view.envelope.message_type);\n  if (slot >= 0 && s->unread[slot] > 0) {", "  int slot = SLOT_GAME;\n  if (slot >= 0 && s->unread[slot] > 0) {"),
 (NT, "arrival counted under a guessed category when the stored type is known", "    int slot = loaded ? slotForType(view.envelope.message_type) : -1;", "    int slot = loaded ? SLOT_GAME : -1;"),
 (NT, "migration skips the recount (old merged count kept as UNRESOLVED)", "      if (!recountFromStore(&s)) {", "      if (true) {"),
 (NT, "migration fallback stores an approximate GAME count (first candidate)", "        markUnresolved(&s, mask != 0 ? mask : static_cast<uint8_t>(BADGE_TEXT | BADGE_ENIGMA));", "        s.state = SUMMARY_COUNTED; s.unread[SLOT_GAME] = l.unread_count; (void)mask;"),
 (NT, "legacy read failure taken as an empty migration", "  if (!migrateLegacyInto(dst)) {\n    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_RETRY_LEGACY;\n  }", "  if (!migrateLegacyInto(dst)) {\n    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_OK;\n  }"),
 (NT, "unreadable summary2 replaced from the legacy record", "    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    return LOAD_RETRY_CURRENT;\n  }", "    memset(dst, 0, sizeof(Summary) * kMaxSummaries);\n    if (migrateLegacyInto(dst)) { *migrated = true; return LOAD_OK; }\n    return LOAD_RETRY_CURRENT;\n  }"),
 (NT, "saves while the stored record is unreadable", "  if (g_loadState != LOAD_OK) {  // the stored record could not be read: never overwrite it\n    g_saveDirty = true;\n    return false;\n  }\n", ""),
 (NT, "a failed save is not retried", "  if (!g_saveDirty) Serial.println(\"[NOTIFY] unread summary not saved; retrying\");\n  g_saveDirty = true;\n  return false;", "  return false;"),
 (NT, "conversations touched while unreadable are not merged", "      if (t != nullptr) markUnresolved(t, mask);", "      (void)t;"),
 (NT, "UNRESOLVED conversations are never reconciled", "    if (recountFromStore(&s)) saveSummaries();\n    return true;", "    return true;"),
 (NT, "a read in an UNRESOLVED conversation guesses a decrement", "  if (s->state == SUMMARY_UNRESOLVED) {\n    if (recountFromStore(s)) saveSummaries();  // else it stays lit until a reliable recount\n    return;\n  }", "  if (s->state == SUMMARY_UNRESOLVED) s->state = SUMMARY_COUNTED;"),
 (NT, "eviction with a failed scan keeps the stale counts", "  if (!recountFromStore(s)) markUnresolved(s, 0);", "  recountFromStore(s);"),
 (NT, "counts loaded at boot are trusted (not verified)", "  } else {\n    verifyLoaded(g_summaries);\n  }", "  }"),
 (NT, "counts loaded by a recovery retry are trusted (not verified)", "    if (!migrated) verifyLoaded(loaded);\n", ""),
 (NT, "verification drops the badges until the recount", "markUnresolved(&set[i], 0);", "{ memset(set[i].unread, 0, sizeof(set[i].unread)); set[i].state = SUMMARY_UNRESOLVED; }"),
 (NT, "verification resets counts to zero (stale value replaced by a guess)", "markUnresolved(&set[i], 0);", "memset(set[i].unread, 0, sizeof(set[i].unread));"),
 (NT, "no sweep after a load at boot", "    verifyLoaded(g_summaries);\n  }\n  startSweep();", "    verifyLoaded(g_summaries);\n  }"),
 (NT, "no sweep after a recovery retry", "    g_loadState = LOAD_OK;\n    startSweep();", "    g_loadState = LOAD_OK;"),
 (NT, "the sweep checks only unlisted conversations (skips zero-count ones)", "  if (listed == nullptr || listed->state == SUMMARY_COUNTED) {", "  if (listed == nullptr) {"),
 (NT, "the sweep never adds an unlisted conversation", "    if (s == nullptr && totalUnread(t) > 0) s = findOrCreateSummary(pick.group_code, pick.contact_key);\n", ""),
 (NT, "the sweep stops after the first conversation", "  g_sweep->started = true;\n", "  g_sweep->started = true;\n  delete g_sweep; g_sweep = nullptr; return;\n"),
 (NT, "the sweep advances past an unreliable recount", "    if (!recountFromStore(&t)) return;\n    Summary* s = listed;", "    if (!recountFromStore(&t)) memset(t.unread, 0, sizeof(t.unread));\n    Summary* s = listed;"),
 (NT, "an unreliable listing ends the sweep", "  if (!MessageStore::listConversations(pickNextConversation, &pick)) return;", "  MessageStore::listConversations(pickNextConversation, &pick);"),
 (NT, "a failing save starves the recount", "    if (g_saveDirty) saveSummaries();\n    if (!reconcileOne() && g_sweep != nullptr) sweepOne();", "    if (g_saveDirty) { saveSummaries(); return; }\n    if (!reconcileOne() && g_sweep != nullptr) sweepOne();"),
 (NT, "conversations the loaded record lacks are never added (touched or pending)", "    if (set[i].state == SUMMARY_FREE || (set[i].state == SUMMARY_COUNTED && totalUnread(set[i]) == 0)) {", "    if (false) {"),
]
out = os.path.join(H, "out", "mutn")
_bad = [m[1] for m in muts if open(R + "/" + m[0]).read().count(m[2]) != 1]
if _bad:
    print("BAD MUTATION ANCHORS (source changed): " + "; ".join(_bad)); sys.exit(2)
objdir = os.path.join(H, "out", "mutnobj"); shutil.rmtree(objdir, ignore_errors=True); os.makedirs(objdir)
common = []
for src in (H + "/stubs.cpp", R + "/number_guessing.cpp", R + "/packet_codec.cpp", R + "/morse.cpp", R + "/enigma_crypto.cpp",
            R + "/enigma_keys.cpp", R + "/crc32.cpp", R + "/presence.cpp"):
    o = os.path.join(objdir, os.path.basename(src) + ".o")
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-c", "-DREAL_NOTIFICATIONS", "-I", H, "-I", H + "/fake", "-I", REPO + "/src", src, "-o", o], capture_output=True, text=True)
    if b.returncode: print("COMMON BUILD FAILED " + src + "\n" + b.stderr[:500]); sys.exit(2)
    common.append(o)
caught, survivors, build_errors = 0, [], 0
only = sys.argv[1:]
for target, name, old, new in muts:
    if only and not any(o in name for o in only): continue
    shutil.rmtree(out, ignore_errors=True); os.makedirs(out + "/core")
    for x in (NT, TX, EN, "notifications.h"): shutil.copy(R + "/" + x, out + "/core/" + x)
    p = out + "/core/" + target; s = open(p).read()
    open(p, "w").write(s.replace(old, new))
    inc = sum([["-I", i] for i in (H, H + "/fake", out, REPO + "/src")], [])
    b = subprocess.run(["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS"] + inc + [H + "/notify_test.cpp", out + "/core/" + EN] + common + ["-o", out + "/n"], capture_output=True, text=True)
    if b.returncode: print(f"  COMPILE-ERR  {name}: {b.stderr[:300]}"); build_errors += 1; continue
    try:
        r = subprocess.run([out + "/n"], capture_output=True, text=True, timeout=120)
        rc, fails = r.returncode, [l for l in r.stdout.splitlines() if "FAIL" in l]
    except subprocess.TimeoutExpired:
        rc, fails = 124, ["TIMEOUT"]
    ok = rc != 0; caught += ok
    if not ok: survivors.append(name)
    print(f"  {'CAUGHT ' if ok else 'SURVIVED'} [notify:{len(fails)}]  {target}: {name}   (first: {fails[0][:100] if fails else '-'})", flush=True)
n = len([m for m in muts if not only or any(o in m[1] for o in only)])
print(f"{caught}/{n} mutations caught; {len(survivors)} survivor(s); {build_errors} build error(s)")
sys.exit(1 if survivors or build_errors else 0)
