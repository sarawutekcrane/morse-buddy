#!/usr/bin/env python3
# HOST TEST (not production): targeted mutations of the radio fix
# (radio_transport.cpp, radio.cpp: radio_test) and of the Enigma zero-key
# display (enigma.cpp: enigma_test). Each mutant must make its suite fail.
# Usage: python3 mutate_radio.py [name-substring ...]
import concurrent.futures, os, shutil, subprocess, sys, tempfile, threading

H = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("REPO", os.path.abspath(os.path.join(H, "..", "..")))
R = os.path.join(REPO, "src", "core")
RT, RU, EN = "radio_transport.cpp", "radio.cpp", "enigma.cpp"
SL, SF = "sleep.cpp", "sound_facade.cpp"

# (file, name, old, new)
muts = [
 (RT, "claim timeout resets silently to Ready again (no failure, no RELEASE)",
  "      releaseAndResetPrivateCall();\n      g_lastFailure = f;", "      resetPrivateCall();\n      (void)f;"),
 (RT, "claim timeout sends no RELEASE (a late grant holds the receiver's channel)",
  "      releaseAndResetPrivateCall();\n      g_lastFailure = f;", "      resetPrivateCall();\n      g_lastFailure = f;"),
 (RT, "the failure outlives the PTT release",
  "  g_lastFailure = PrivateFailure::NONE;  // the failed attempt's status ends with the PTT press\n", ""),
 (RT, "a new press keeps the previous failure",
  "  g_lastFailure = PrivateFailure::NONE;\n  ensureUdpBegun();", "  ensureUdpBegun();"),
 (RT, "every CLAIM refused still reads as 'No response'",
  "                         : g_call.claimQueuedTicket != 0 ? PrivateFailure::SEND_TIMEOUT\n                                                         : PrivateFailure::SEND_FAILED;",
  "                         : g_call.claimQueuedTicket != 0 ? PrivateFailure::SEND_TIMEOUT\n                                                         : PrivateFailure::NO_RESPONSE;"),
 (RT, "a refused CLAIM is not counted as a send failure",
  "    if (claiming) g_call.claimSendFails++;\n    Serial.printf(\"[RADIO][CLAIM] publish failed", "    Serial.printf(\"[RADIO][CLAIM] publish failed"),
 # ---- Gate 1: CLAIM queued vs published ----
 (RT, "Gate 1: the reply window counted from the press again (a CLAIM published late times out at once)",
  "  ? g_call.claimPublishedMs + kClaimTotalTimeoutMs", "  ? g_call.claimFirstSentMs + kClaimTotalTimeoutMs"),
 (RT, "Gate 1: no queue limit (Claiming... for as long as the worker is stuck)",
  "? g_call.claimFirstSentMs + kClaimQueueMaxMs", "? g_call.claimFirstSentMs + 600000"),
 (RT, "Gate 1: a CLAIM still queued at the limit reads as 'No response'",
  "                         : g_call.claimQueuedTicket != 0 ? PrivateFailure::SEND_TIMEOUT\n",
  "                         : g_call.claimQueuedTicket != 0 ? PrivateFailure::NO_RESPONSE\n"),
 (RT, "Gate 1: retries stack behind a queued CLAIM",
  "  if (g_call.claimQueuedTicket != 0) return true;  // the previous CLAIM has not left yet\n", ""),
 (RT, "Gate 1: a queued CLAIM counted as published (starts the reply window)",
  "    g_call.claimQueuedTicket = ticket;\n    g_call.claimQueuedAtMs = millis();\n",
  "    g_call.claimQueuedTicket = ticket;\n    g_call.claimQueuedAtMs = millis();\n    if (g_call.claimPublishedMs == 0) g_call.claimPublishedMs = millis() | 1;\n"),
 (RT, "Gate 1: a worker failure after enqueue counted as published",
  "      } else {\n        g_call.claimSendFails++;\n      }",
  "      } else {\n        if (g_call.claimPublishedMs == 0) g_call.claimPublishedMs = millis() | 1;\n      }"),
 (RT, "Gate 1: CLAIM sent through publishBinary() again (queued reported as sent)",
  "  return MqttManager::publishControl(group_code, topicSuffix, wireBuf,\n                                     static_cast<uint16_t>(PacketCodec::kHeaderSize + len), done, ticket);",
  "  return MqttManager::publishBinary(group_code, topicSuffix, wireBuf, static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0) ? MqttManager::ControlOutcome::PUBLISHED : MqttManager::ControlOutcome::FAILED;"),
 (RT, "Gate 1: a RELEASE refused while its CLAIM is queued is not owed",
  "    if (o == MqttManager::ControlOutcome::FAILED && queuedClaim != 0) {", "    if (false) {"),
 (RT, "Gate 1: the owed RELEASE is never sent",
  "  if (ok) {\n    MqttManager::ControlOutcome o =\n        sendBusyPacket(r->group_code",
  "  if (false) {\n    MqttManager::ControlOutcome o =\n        sendBusyPacket(r->group_code"),
 (RT, "Gate 1: the owed RELEASE is sent although the queued CLAIM failed (never reached the receiver)",
  "  if (ok) {\n    MqttManager::ControlOutcome o =\n        sendBusyPacket(r->group_code",
  "  if (true) {\n    MqttManager::ControlOutcome o =\n        sendBusyPacket(r->group_code"),
 # ---- final review: several sessions abandoned while queued ----
 (RT, "multi-session: a new owed RELEASE drops the earlier sessions' (single slot again)",
  "        r->next = g_owedRelease;\n", "        r->next = nullptr;\n"),
 (RT, "multi-session: the owed entry is not matched to its own CLAIM (first entry taken)",
  "    if ((*pp)->claimTicket == ticket) {", "    if (true) {"),
 (RT, "multi-session: a used owed entry stays in the list",
  "      *pp = r->next;\n      return r;", "      return r;"),
 (RT, "multi-session: no bound on owed entries",
  "OwedRelease* r = owedReleaseCount() < kMaxOwedReleases ? new (std::nothrow) OwedRelease() : nullptr;",
  "OwedRelease* r = new (std::nothrow) OwedRelease();"),
 (RT, "Gate 1: a queued GRANT's result is not tracked",
  "    g_replyTicket = ticket;\n", ""),
 (RU, "Gate 1: Talk screen never shows 'Send timeout'",
  "  } else if (RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_TIMEOUT) {",
  "  } else if (false) {"),
 (RT, "the claim window lengthened (3 s) instead of fixed",
  "constexpr uint32_t kClaimTotalTimeoutMs = 1000;", "constexpr uint32_t kClaimTotalTimeoutMs = 3000;"),
 (RT, "a reply outside CLAIMING is acted on (duplicate GRANT restarts the negotiation)",
  "                    static_cast<unsigned long>(millis() - g_failedAtMs));\n    }\n    return;\n  }",
  "                    static_cast<unsigned long>(millis() - g_failedAtMs));\n    }\n  }"),
 (RT, "audio published synchronously again (waits for TCP space)",
  "  return MqttManager::publishBinaryNoWait(group_code, topic_suffix, wireBuf,\n                                          static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen));",
  "  return MqttManager::publishBinary(group_code, topic_suffix, wireBuf,\n                                    static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen), false, 0);"),
 (RT, "pre-buffer ignores the heap reserve",
  "  size_t byFree = freeBytes > kPreBufferHeapReserve ? (freeBytes - kPreBufferHeapReserve) / kFrameBytes : 0;",
  "  size_t byFree = freeBytes / kFrameBytes;"),
 (RT, "pre-buffer ignores the largest free block",
  "  size_t frames = byFree < byBlock ? byFree : byBlock;", "  size_t frames = byFree;"),
 (RT, "pre-buffer allocated however small",
  "  return frames < kPreBufferMinFrames ? 0 : static_cast<uint16_t>(frames);", "  return static_cast<uint16_t>(frames);"),
 (RT, "pre-buffer not capped at 2 s",
  "  if (frames > kPreBufferMaxFrames) frames = kPreBufferMaxFrames;\n", ""),
 (RT, "receiver grants before its group is ready (pre-existing rule)",
  "    if (!MqttManager::isGroupReady(group_code)) {\n      Serial.printf(", "    if (false) {\n      Serial.printf("),
 (RT, "receiver grants a second claimer while busy (pre-existing rule)",
  "    if (!g_receiverBusy.busy || sameClaimant) {", "    if (true) {"),
 (RT, "stale-session GRANT accepted (pre-existing rule)",
  "  if (strcmp(sessionId, g_call.session_id) != 0) return;  // stale GRANT/DENY for an earlier attempt", ""),
 (RU, "Talk screen shows Ready after a claim timeout (the hardware symptom)",
  "  } else if (RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NO_RESPONSE) {",
  "  } else if (false) {"),
 (RU, "Talk screen never shows 'Send failed'",
  "  } else if (RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_FAILED) {",
  "  } else if (false) {"),
 (EN, "Enigma: the zero-key display ignores the plugboard (unescapes plugboard ciphertext)",
  "  if (embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0) {\n    char decrypted",
  "  if (embedded.rotor_count == 0) {\n    char decrypted"),
 (EN, "Enigma: every locked ciphertext is unescaped (XQ/XX in rotor ciphertext read as escapes)",
  "  if (embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0) {\n    char decrypted",
  "  if (true) {\n    char decrypted"),
 (EN, "Enigma: no readable zero-key display (DXQEE shown)",
  "  if (embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0) {\n    char decrypted",
  "  if (false) {\n    char decrypted"),
 (EN, "Enigma: zero-key display skips the decrypt step (unescape of the ciphertext directly)",
  "    EnigmaCrypto::reverseEscape(decrypted, readable, sizeof(readable));",
  "    EnigmaCrypto::reverseEscape(ciphertext, readable, sizeof(readable));"),
 # ---- PTT sleep safety ----
 (SF, "sleep: a live voice session does not hold deep sleep off (the hardware risk)",
  "  Sleep::setInhibited(Sleep::InhibitReason::RADIO_AUDIO, active);\n", ""),
 (SL, "sleep: one shared flag again (clearing any reason resumes sleep for all)",
  "  g_inhibitMask &= static_cast<uint8_t>(~bit);", "  g_inhibitMask = 0;"),
 (SL, "sleep: the end of an inhibited stretch keeps the stale baseline (sleep right after the release)",
  "  if (wasSet) notifyActivity();\n}", "}"),
 (SL, "sleep: clearing a reason that was not set postpones sleep",
  "  if (wasSet) notifyActivity();\n}", "  notifyActivity();\n}"),
 (SL, "sleep: update() ignores the inhibit reasons",
  "  if (g_inhibitMask != 0) return;\n", ""),
 (SL, "sleep: OTA suspension mapped onto the Radio reason",
  "void setSuspended(bool suspended) { setInhibited(InhibitReason::OTA_MAINTENANCE, suspended); }",
  "void setSuspended(bool suspended) { setInhibited(InhibitReason::RADIO_AUDIO, suspended); }"),
 (RT, "sleep: a DENY (BUSY) leaves the voice session marked live (pre-existing clear)",
  "    g_call.denied = true;\n    setRadioAudioActive(false);", "    g_call.denied = true;"),
 (EN, "Gate 6: a zero-key message shows the red padlock again",
  "  if (isZeroKeyMessage(msg)) return MessageIconKind::NONE;  // Gate 6: nothing locked, no padlock\n", ""),
 (EN, "Gate 6: a zero-key message opens the key editor / Reveal again",
  "  if (isZeroKeyMessage(view)) return;\n", ""),
 (EN, "Gate 6: the DOT hold on a zero-key message shows [locked] again",
  "  if (embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0) {\n    char decrypted",
  "  if (embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0 && !(g_holdActive && refsEqual(msg.ref, g_heldRef))) {\n    char decrypted"),
 (EN, "Gate 6: the no-padlock / no-editor rule ignores the plugboard",
  "         embedded.rotor_count == 0 && embedded.plugboard_pair_count == 0;\n", "         embedded.rotor_count == 0;\n"),
 (EN, "Enigma: zero-key display without the unescape",
  "    EnigmaCrypto::reverseEscape(decrypted, readable, sizeof(readable));",
  "    snprintf(readable, sizeof(readable), \"%s\", decrypted);"),
]
equivalent = {
 "a new press keeps the previous failure": "redundant guard: a failure can only be set while the PTT is held, and every way the hold ends (the release, Back via leaveTalk()) runs stopPrivateCall(), which clears it; a new press always follows one of those, so the start finds it cleared already",
 "Enigma: zero-key display skips the decrypt step (unescape of the ciphertext directly)": "with no rotors and no plugboard pairs EnigmaCrypto::run() is the identity (enigma_crypto.cpp encryptChar(): zero-rotor path, plugboard applied once, no pairs), so decrypting first changes nothing for the only keys this branch takes; the order is kept to state the decode rule (decrypt, then reverseEscape once)",
}

SUITE = {
 RT: "radio", RU: "radio", EN: "enigma", SL: "radio", SF: "radio",
}
only = sys.argv[1:]
todo = [m for m in muts if not only or any(o in m[1] for o in only)]
bad = [m[1] for m in todo if open(os.path.join(R, m[0])).read().count(m[2]) != 1]
if bad:
    print("BAD MUTATION ANCHORS (source changed): " + "; ".join(bad)); sys.exit(2)
lock = threading.Lock()

def build(suite, d, out):
    if suite == "radio":
        src = open(os.path.join(H, "radio_test.cpp")).read()
        src = src.replace('#include "core/radio_transport.cpp"', '#include "%s"' % os.path.join(d, "core", RT))
        src = src.replace('#include "core/radio.cpp"', '#include "%s"' % os.path.join(d, "core", RU))
        src = src.replace('#include "core/sleep.cpp"', '#include "%s"' % os.path.join(d, "core", SL))
        src = src.replace('#include "core/sound_facade.cpp"', '#include "%s"' % os.path.join(d, "core", SF))
        open(os.path.join(d, "t.cpp"), "w").write(src)
        cmd = ["g++", "-std=gnu++17", "-O1", "-I", H, "-I", os.path.join(H, "fake_sleep"), "-I", os.path.join(H, "fake_radio"), "-I", os.path.join(H, "fake"),
               "-I", os.path.join(REPO, "src"), os.path.join(d, "t.cpp"), os.path.join(H, "stubs.cpp")] + \
              [os.path.join(R, f) for f in ("packet_codec.cpp", "presence.cpp", "crc32.cpp", "morse.cpp")] + ["-o", out]
    else:
        src = open(os.path.join(H, "enigma_test.cpp")).read()
        src = src.replace('#include "core/enigma.cpp"', '#include "%s"' % os.path.join(d, "core", EN))
        open(os.path.join(d, "t.cpp"), "w").write(src)
        cmd = ["g++", "-std=gnu++17", "-O1", "-DREAL_NOTIFICATIONS", "-I", H, "-I", os.path.join(H, "fake"),
               "-I", os.path.join(REPO, "src"), os.path.join(d, "t.cpp"), os.path.join(H, "stubs.cpp")] + \
              [os.path.join(R, f) for f in ("menu.cpp", "notifications.cpp", "text_message.cpp", "number_guessing.cpp", "packet_codec.cpp",
                                            "morse.cpp", "enigma_crypto.cpp", "enigma_keys.cpp", "crc32.cpp", "presence.cpp")] + ["-o", out]
    return subprocess.run(cmd, capture_output=True, text=True)

def one(m):
    target, name, old, new = m
    d = tempfile.mkdtemp(prefix="mutr_")
    os.makedirs(os.path.join(d, "core"))
    for f in (RT, RU, EN, SL, SF):
        shutil.copy(os.path.join(R, f), os.path.join(d, "core", f))
    p = os.path.join(d, "core", target)
    src = open(p).read().replace(old, new)
    open(p, "w").write(src)
    b = build(SUITE[target], d, os.path.join(d, "t"))
    if b.returncode:
        with lock: print("  COMPILE-ERR  %s: %s" % (name, b.stderr[:300]), flush=True)
        shutil.rmtree(d, ignore_errors=True); return name, None
    try:
        r = subprocess.run([os.path.join(d, "t")], capture_output=True, text=True, timeout=120)
        rc, fails = r.returncode, [l for l in r.stdout.splitlines() if "FAIL" in l]
    except subprocess.TimeoutExpired:
        rc, fails = 124, ["TIMEOUT"]
    shutil.rmtree(d, ignore_errors=True)
    ok = rc != 0
    tag = "CAUGHT " if ok else ("SURVIVED (assessed equivalent: %s)" % equivalent[name] if name in equivalent else "SURVIVED")
    with lock: print("  %s [%s:%d] %s: %s  (first: %s)" % (tag, SUITE[target], len(fails), target, name, (fails or ["-"])[0][:100]), flush=True)
    return name, ok

caught, surv, errs = 0, [], 0
with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("MUT_JOBS", "3"))) as ex:
    for name, ok in ex.map(one, todo):
        if ok is None: errs += 1
        elif ok: caught += 1
        else: surv.append(name)
eq = len([s for s in surv if s in equivalent])
print("%d/%d mutations caught; %d survivor(s) assessed equivalent; %d unexplained survivor(s); %d build error(s)" %
      (caught, len(todo), eq, len(surv) - eq, errs))
sys.exit(1 if (len(surv) - eq) or errs else 0)
