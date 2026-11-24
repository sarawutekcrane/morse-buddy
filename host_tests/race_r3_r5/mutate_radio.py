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
  "PrivateFailure f = (g_call.claimSends > 0 && g_call.claimSendFails == g_call.claimSends)",
  "PrivateFailure f = (false)"),
 (RT, "a refused CLAIM is not counted as a send failure",
  "    if (!ok) g_call.claimSendFails++;\n", ""),
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
 (EN, "Enigma: zero-key display without the unescape",
  "    EnigmaCrypto::reverseEscape(decrypted, readable, sizeof(readable));",
  "    snprintf(readable, sizeof(readable), \"%s\", decrypted);"),
]
equivalent = {
 "a new press keeps the previous failure": "redundant guard: a failure can only be set while the PTT is held, and every way the hold ends (the release, Back via leaveTalk()) runs stopPrivateCall(), which clears it; a new press always follows one of those, so the start finds it cleared already",
 "Enigma: zero-key display skips the decrypt step (unescape of the ciphertext directly)": "with no rotors and no plugboard pairs EnigmaCrypto::run() is the identity (enigma_crypto.cpp encryptChar(): zero-rotor path, plugboard applied once, no pairs), so decrypting first changes nothing for the only keys this branch takes; the order is kept to state the decode rule (decrypt, then reverseEscape once)",
}

SUITE = {
 RT: "radio", RU: "radio", EN: "enigma",
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
        open(os.path.join(d, "t.cpp"), "w").write(src)
        cmd = ["g++", "-std=gnu++17", "-O1", "-I", H, "-I", os.path.join(H, "fake_radio"), "-I", os.path.join(H, "fake"),
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
    for f in (RT, RU, EN):
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
