#!/usr/bin/env python3
# Targeted mutations of mqtt_manager.cpp's connection worker (MQTT fix): each
# mutant must make mqtt_test fail (manual and/or threads; mutants marked
# "tsan" are run under ThreadSanitizer instead). A timeout counts as caught.
# HOST ONLY. Usage: python3 mutate_mqtt.py [name-substring ...]
import concurrent.futures, os, shutil, subprocess, sys, tempfile, threading

H = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("REPO", os.path.abspath(os.path.join(H, "..", "..")))
R = os.path.join(REPO, "src", "core")
M = "mqtt_manager.cpp"

# (name, old, new, mode) -- mode: "asan" (manual + threads) or "tsan"
muts = [
 ("stale result adopted (no generation check)",
  "  bool current = c.state == SlotState::kActive && r.generation == g_slotGeneration[r.slot].load();",
  "  bool current = c.state == SlotState::kActive;", "asan"),
 ("any setup result releases the slot's job (two jobs per slot)",
  "  if (!c.jobInFlight || r.generation != c.jobGeneration) {\n    if (r.ok) disposeConn(r.conn, /*publishOffline=*/false);\n    return;\n  }",
  "  if (!c.jobInFlight) {\n    if (r.ok) disposeConn(r.conn, /*publishOffline=*/false);\n    return;\n  }", "asan"),
 ("a result replaces an adopted connection",
  "    if (current && c.conn == nullptr && !g_maintenanceMode",
  "    if (current && !g_maintenanceMode", "asan"),
 ("adopted with Wi-Fi down",
  " && !g_maintenanceMode && WifiManager::isConnected() && r.conn->mqtt.connected()) {",
  " && !g_maintenanceMode && r.conn->mqtt.connected()) {", "asan"),
 ("adopted during maintenance",
  "    if (current && c.conn == nullptr && !g_maintenanceMode && WifiManager::isConnected()",
  "    if (current && c.conn == nullptr && WifiManager::isConnected()", "asan"),
 ("setup queued while the same client id still drains",
  "    if (clientIdDraining(c.client_id)) continue;  // its cleanup (same client id) must run first\n",
  "", "asan"),
 ("cleanup queued while the setup job is out",
  "    if (c.state != SlotState::kDraining || c.jobInFlight || c.cleanupInFlight) continue;",
  "    if (c.state != SlotState::kDraining || c.cleanupInFlight) continue;", "asan"),
 ("a second cleanup queued while one is out",
  "    if (c.state != SlotState::kDraining || c.jobInFlight || c.cleanupInFlight) continue;",
  "    if (c.state != SlotState::kDraining || c.jobInFlight) continue;", "asan"),
 ("worker ignores cancellation",
  "  return g_workerSuspended.load() || g_slotGeneration[job.slot].load() != job.generation;",
  "  return false;", "asan"),
 ("worker ignores suspension only",
  "  return g_workerSuspended.load() || g_slotGeneration[job.slot].load() != job.generation;",
  "  return g_slotGeneration[job.slot].load() != job.generation;", "asan"),
 ("deletion does not cancel the setup",
  "      if (c == nullptr) break;\n      bumpGeneration(slotIndex(*c));",
  "      if (c == nullptr) break;", "asan"),
 ("Wi-Fi recovery does not cancel jobs in flight",
  "    if (c.jobInFlight) bumpGeneration(slotIndex(c));\n", "", "asan"),
 ("Wi-Fi recovery keeps the dead adopted connection",
  "    if (c.conn != nullptr) dropAdoptedConn(c, /*publishOffline=*/false);\n    if (c.jobInFlight)",
  "    if (c.jobInFlight)", "asan"),
 ("maintenance does not suspend the worker",
  "    g_maintenanceMode = true;\n    g_workerSuspended.store(true);", "    g_maintenanceMode = true;", "asan"),
 ("maintenance exit keeps the worker suspended",
  "    g_workerSuspended.store(false);\n    g_maintenanceMode = false;", "    g_maintenanceMode = false;", "asan"),
 ("maintenance leaves connections open",
  "      if (gc.conn != nullptr) dropAdoptedConn(gc, /*publishOffline=*/false);\n      gc.nextAttemptMs = 0;",
  "      gc.nextAttemptMs = 0;", "asan"),
 ("before-sleep publishes no OFFLINE",
  "void onBeforeSleep() {\n  publishOfflineAllGroupsBounded();", "void onBeforeSleep() {", "asan"),
 ("before-sleep does not stop setups",
  "  g_workerSuspended.store(true);\n  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) bumpGeneration(i);\n}",
  "}", "asan"),
 ("constant 2 s retry (no backoff)",
  "  uint32_t d = kRetryMinMs << shift;", "  uint32_t d = kRetryMinMs; (void)shift;", "asan"),
 ("failures not counted",
  "  if (c.consecutiveFailures < 255) c.consecutiveFailures++;\n  c.nextAttemptMs = deadlineAfter(millis(), retryDelayMs(c.consecutiveFailures));\n  char ip[16];",
  "  c.nextAttemptMs = deadlineAfter(millis(), retryDelayMs(c.consecutiveFailures));\n  char ip[16];", "asan"),
 ("no anti-thrash window after Ready",
  "  c.nextAttemptMs = deadlineAfter(millis(), kReadyAntiThrashMs);", "  c.nextAttemptMs = 0;", "asan"),
 ("always the answer's first address (no endpoint memory)",
  "      if (e != nullptr && !endpointAvoided(*e, now) && (pass == 1 || e->everOk)) return resolved[i];",
  "      if (e != nullptr && i == 0) return resolved[i];", "asan"),
 ("only the first address of a DNS answer is kept",
  "    for (struct addrinfo* a = res; a != nullptr && n < cap; a = a->ai_next) {",
  "    for (struct addrinfo* a = res; a != nullptr && n < 1; a = a->ai_next) {", "asan"),
 ("no fresh DNS answer after an endpoint failure",
  "  tcpip_callback(clearDnsCacheInTcpipThread, nullptr);\n", "", "asan"),
 ("a working address of the answer not preferred",
  "(pass == 1 || e->everOk)) return resolved[i];", "(pass == 1 || true)) return resolved[i];", "asan"),
 ("TCP failure does not mark the endpoint",
  "    r.lastError = LWMQTT_NETWORK_FAILED_CONNECT;\n    noteEndpointFailure(c.endpoint, millis());",
  "    r.lastError = LWMQTT_NETWORK_FAILED_CONNECT;", "asan"),
 ("success does not mark the endpoint good",
  "    noteEndpointOk(c->endpoint, millis());\n", "", "asan"),
 ("DNS down: no fallback endpoint",
  "    if (best == nullptr || static_cast<int32_t>(e.lastOkMs - best->lastOkMs) > 0) best = &e;\n  }\n  if (best != nullptr) return best->ip;",
  "    if (best == nullptr || static_cast<int32_t>(e.lastOkMs - best->lastOkMs) > 0) best = &e;\n  }\n  if (best != nullptr && n > 0) return best->ip;", "asan"),
 ("session cleanup skipped for maintenance is not redone",
  "    c.nextAttemptMs = 0;  // skipped for maintenance: redone after it, not an attempt\n    Serial.printf(\"[MQTT] cleanup group=%s deferred until maintenance ends\\n\", c.group_code);\n    return;",
  "    freeSlot(c);\n    return;", "asan"),
 ("slot freed when the cleanup is queued (client id released early)",
  "    c.conn = nullptr;  // the connection now belongs to the job\n    c.cleanupInFlight = true;",
  "    c.conn = nullptr;  // the connection now belongs to the job\n    c = GroupClient();", "asan"),
 ("failed session cleanup never retried",
  "  if (++c.cleanupAttempts >= kCleanupMaxAttempts) {", "  if (++c.cleanupAttempts >= 1) {", "asan"),
 ("failed session cleanup retried without bound",
  "  if (++c.cleanupAttempts >= kCleanupMaxAttempts) {", "  if (++c.cleanupAttempts >= 250) {", "asan"),
 ("deletion keeps the anti-thrash deadline for the cleanup",
  "      c->nextAttemptMs = 0;              // from now on: the cleanup's retry time\n", "", "asan"),
 ("handover keeps the 5 s handshake timeout",
  "    c->mqtt.setTimeout(kMqttCommandTimeoutMs);\n    noteEndpointOk", "    noteEndpointOk", "asan"),
 ("ONLINE not required for Ready",
  "        if (!published || !c->mqtt.connected()) {", "        if (false) {", "asan"),
 ("cleanup skips OFFLINE and record clear",
  "      c->mqtt.publish(selfTopic, job.willOrOffline, /*retained=*/true, /*qos=*/1);\n      c->mqtt.publish(selfTopic, \"\", /*retained=*/true, /*qos=*/1);  // zero-length retained: remove our record\n",
  "", "asan"),
 ("cleanup skips the cleanSession session clear",
  "  if (!g_workerSuspended.load()) {\n    Conn* t = new (std::nothrow) Conn();",
  "  if (false) {\n    Conn* t = new (std::nothrow) Conn();", "asan"),
 ("deleted group's queued messages dispatched",
  "  if (!groupHasActiveClient(q.group_code)) return;\n", "", "asan"),
 ("loss not freed on the main loop",
  "    if (!connectedAfter) dropAdoptedConn(gc, /*publishOffline=*/false);", "", "asan"),
 ("no slot retry for a group added while slots were full",
  "  ensureSlotsForConfiguredGroups();\n  postPendingCleanups();\n  if (!wifiConnected) return;",
  "  postPendingCleanups();\n  if (!wifiConnected) return;", "asan"),
 ("presence change during setup not republished",
  "  if (Crc32::computeStr(current) != c.onlineCrc) Presence::publishOnline(c.group_code);", "", "asan"),
 ("unbounded write (no budget)",
  "      if (static_cast<uint32_t>(millis()) - start >= kSendBudgetMs) break;", "", "asan"),
 ("will topic cut at 48 bytes again",
  "    char willTopic[kTopicBufLen];", "    char willTopic[48];", "asan"),
 ("worker creation never retried",
  "  ensureWorker();\n  // Results first", "  // Results first", "asan"),
 ("receive ring without its lock (enqueue)",
  "  portENTER_CRITICAL(&g_queueMux);\n  if (g_queueCount < kQueueCapacity) {",
  "  if (g_queueCount < kQueueCapacity) {", "tsan"),
]
EQUIV = {
 "a result replaces an adopted connection": "redundant guard: a setup job is only queued while the slot has no adopted connection, and only that job's own result (jobInFlight and its generation) gets this far",
 "adopted during maintenance": "redundant guard: maintenance entry bumps every occupied slot's generation and no setup is queued during maintenance, so no result is current then",
 "worker ignores suspension only": "redundant guard: every path that sets the suspension (maintenance entry, before-sleep) also bumps the generation of every slot that can have a job, so the generation test alone already cancels",
}

only = sys.argv[1:]
todo = [m for m in muts if not only or any(o in m[0] for o in only)]
bad = [m[0] for m in todo if open(os.path.join(R, M)).read().count(m[1]) != 1]
if bad:
    print("BAD MUTATION ANCHORS (source changed): " + "; ".join(bad)); sys.exit(2)
if any(m[0] == "receive ring without its lock (enqueue)" for m in todo):
    pass
lock = threading.Lock()

def build(src_dir, out, tsan):
    san = ["-g", "-O1", "-fsanitize=thread"] if tsan else ["-g", "-fsanitize=address,undefined"]
    cmd = ["g++", "-std=gnu++17"] + san + ["-I", os.path.join(H, "fake_net"), "-I", os.path.join(H, "fake"),
           "-I", src_dir, "-I", os.path.join(REPO, "src"), os.path.join(src_dir, "mqtt_test.cpp"),
           os.path.join(R, "crc32.cpp"), os.path.join(R, "packet_codec.cpp"), "-o", out, "-lpthread"]
    return subprocess.run(cmd, capture_output=True, text=True)

def run(binary, args, timeout):
    try:
        r = subprocess.run([binary] + args, capture_output=True, text=True, timeout=timeout)
        return r.returncode, [l for l in (r.stdout + r.stderr).splitlines() if "FAIL" in l or "ThreadSanitizer" in l]
    except subprocess.TimeoutExpired:
        return 124, ["TIMEOUT"]

def one(k, m):
    name, old, new, mode = m
    d = tempfile.mkdtemp(prefix="mutq_")
    os.makedirs(os.path.join(d, "core"))
    for f in os.listdir(R):
        if f.endswith(".h"): shutil.copy(os.path.join(R, f), os.path.join(d, "core", f))
    src = open(os.path.join(R, M)).read().replace(old, new)
    open(os.path.join(d, "core", M), "w").write(src)
    t = open(os.path.join(H, "mqtt_test.cpp")).read().replace('#include "core/mqtt_manager.cpp"',
                                                              '#include "%s"' % os.path.join(d, "core", M))
    open(os.path.join(d, "mqtt_test.cpp"), "w").write(t)
    b = build(d, os.path.join(d, "t"), mode == "tsan")
    if b.returncode:
        with lock: print("  COMPILE-ERR  %s: %s" % (name, b.stderr[:300]), flush=True)
        shutil.rmtree(d, ignore_errors=True); return name, None
    if mode == "tsan":
        rc, f = run(os.path.join(d, "t"), ["threads", "100"], 600)
        rcs, fs = [rc], f
    else:
        rc1, f1 = run(os.path.join(d, "t"), ["manual"], 120)
        rc2, f2 = (0, [])
        if rc1 == 0: rc2, f2 = run(os.path.join(d, "t"), ["threads", "60"], 300)
        rcs, fs = [rc1, rc2], f1 + f2
    shutil.rmtree(d, ignore_errors=True)
    ok = any(x != 0 for x in rcs)
    tag = "CAUGHT " if ok else ("SURVIVED (assessed equivalent: %s)" % EQUIV[name] if name in EQUIV else "SURVIVED")
    with lock: print("  %s %s  (first: %s)" % (tag, name, (fs or ["-"])[0][:110]), flush=True)
    return name, ok

caught, surv, errs = 0, [], 0
with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("MUT_JOBS", "3"))) as ex:
    for name, ok in ex.map(lambda km: one(*km), enumerate(todo)):
        if ok is None: errs += 1
        elif ok: caught += 1
        else: surv.append(name)
eq = len([s for s in surv if s in EQUIV])
print("%d/%d mutations caught; %d survivor(s) assessed equivalent; %d unexplained survivor(s); %d build error(s)" %
      (caught, len(todo), eq, len(surv) - eq, errs))
sys.exit(1 if (len(surv) - eq) or errs else 0)
