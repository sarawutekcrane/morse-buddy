# Chat entry, viewport reads, Outbox scan and held input: round 2 on 2ed04b6 (uncommitted candidate)

- **Base:** `2ed04b687983bafb4d8739c085655e2e23b67968` (`test/hw-functional-ui-fix4`).
- **Not done:** nothing committed, pushed, flashed, erased or uploaded. No OTA or release change.
- **Unchanged:** device A's history and device B. Handover research is not touched.
- **No new persistent format.** The comparison and a proposal for approval are in `STORAGE_CACHE_DESIGN.md`.

## Device A's capture (2ed04b6, starts with a reboot), split

| | measured on device A | what it was |
|---|---|---|
| Chat entry, first | `CHAT_INDEX` list 158 ms + read 4446 ms (70 records) | one LittleFS open per record, about 63 ms each |
| Chat entry, cached | about 864 ms (`historyViewport` 415 + `historyRender` 447) | the visible rows read twice |
| Outbox | `scan 5143 ms found=0` | every record loaded in one call, after connecting |
| Startup | `[PERF][MQTT] setup/connect/subscribe`, OTA check | network startup, separate from chat entry; the trace does not show a Wi-Fi disconnect |

## What changed (`production.diff`: 9 files changed, plus the new `held_input.h`)

1. **Record cache.** 10 heap slots hold exact record bytes.
   - Every store write updates its slot (write-through). A removal or an external change drops it.
   - The render no longer re-reads what the viewport read. A re-entry or a rotation reads nothing it has already read.
2. **The index is kept across mutations of other conversations.**
   - Its own writes and removals are patched in place.
   - A **parked second index** (heap) keeps the last chat. Everyone → 1:1 → Everyone reads nothing.
3. **The boot sweep reads each record once and shares it.**
   - Everyone is visited first. Its recount feeds the chat index.
   - If a chat already holds the shared index (a 1:1 chat opened right after boot), the recount feeds the parked index instead. This is new since the last package.
   - The fed index is used only if:
     - every listed record was read;
     - each record matched the listing;
     - nothing changed that conversation during the feed.
   
   Otherwise it is dropped and the chat loads normally.
4. **Time-budgeted steps.**
   - About one open per main-loop pass on device A (a 20 ms budget).
   - The Notifications sweep and the Outbox pass pause while a chat loads its index.
5. **Outbox.**
   - The blocking 5.1 s scan is gone. In its place is an incremental pass of **header-only** reads, at most 4 per step within the 20 ms budget.
   - The pass runs once per boot after a group connects, and again after a change made outside the store or a pass that could not read everything.
   - A record written pending is queued directly, so a send never causes a scan.
   - Pending records stay durable on flash: the flag is cleared only after a successful publish (as before). A queue overflow is found by the next pass.
6. **Input while loading** (Text and Enigma):
   - **Back** leaves at once. Held input is dropped, and history-side events queued before a Back are not processed.
   - **Held-input limits** (32 events, heap):
     - On overflow or allocation failure, the index is completed in that pass and the input is processed live, so nothing is dropped.
     - Overflow and allocation failure are tested explicitly (IN2, IN3; the allocation failure is injected through `operator new[](nothrow)`).
   - **Send window.** `isReplayedSend()` now applies at dispatch to held and live events alike.
     - IN4, IN4b and IN4c show a Send and its replayed Short give one send attempt.
     - IN6b shows a Send queued before a rotation and a Back is still sent.
   - **Morse.** IN5 shows one tested sequence (SOS) keyed during loading gives the same draft as keyed live. It is not a proof that all Morse input is lossless.

## Measurements: host only (device-cost model, 63 ms per open). Not device timing.

| scenario | 2ed04b6 | this candidate |
|---|---|---|
| Everyone, 90 messages, after the boot sweep (CE1) | 98 opens in one pass | 4 opens, drawn in the first pass |
| Re-entry after arrivals elsewhere (CE2) / after visiting a 1:1 chat (CE2b) | full reload | 0 opens |
| Everyone opened 1 s after boot (DM1) | — | 5.7 s simulated, spread over passes; at most 5 opens in a pass |
| Everyone opened once boot work settled (DM2) | — | 272 ms simulated, drawn in one pass |
| Everyone opened 14 s after boot with 3 × 99 other messages (DM3) | — | 335 ms simulated, one pass |
| Back after 20 queued rotations (IN6) | — | leaves in the same pass, 20 ms simulated, 0 opens |
| 1:1 chat at boot, then Everyone (CE21) | — | Everyone: 4 opens, drawn in one pass |

Storage bench, simulated time until the chat is drawn (`evidence/storage_bench/`; host counts × 63 ms):

| history | B2 (opened after boot work) 2ed04b6 / previous candidate / this | B0b (opened during boot) 2ed04b6 / previous / this |
|---|---|---|
| 1 × 70 | 544 / 544 / **40** ms | 9364 / 13252 / **6102** ms |
| 40 × 20 | 1804 / 1904 / **292** ms | 1804 / 2156 / **1952** ms |
| 8 × 100 | 6844 / 7344 / **292** ms | 57244 / 63036 / **8592** ms |

## Validation

- `storage_test` (real store over a temp-dir LittleFS, real Notifications, Text, Enigma and Outbox):
  - this candidate: **166 passed, 0 failed**
  - the same tests on 2ed04b6: BEFORE_2ED
  - the same tests on the previous candidate: BEFORE_PREV

  (`evidence/before_runs/`, with the shims listed there.)
- Full `run_all.sh`, including targeted mutations: RUN_ALL (`evidence/run_all.txt`).
- ESP32 build: ESP32_LINE (`evidence/esp32_build_log.txt`, `evidence/esp32_binary_hashes.txt`).

## Limitations

- **Device timing is not verified.** The figures above are host operation counts at 63 ms per open. `CAPTURE_WINDOWS.md` has a capture procedure that separates startup MQTT/OTA work from chat entry.
- **Cold entry stays one open per record.** A conversation opened before the boot sweep reached it (DM1) is spread over passes, but the total cost is unchanged. Removing that floor across a reboot needs the persistent index (design B), which is not implemented and needs your approval.
- **Static DRAM headroom** is 128 B (232 B on 2ed04b6). New state is on the heap: record cache up to about 4.5 KB, parked index about 4 KB, Outbox pass and queue while in use, held input while in use.
- **The Friend chat** still loads its index in one call.
- **Morse during loading:** one tested sequence only (above).
