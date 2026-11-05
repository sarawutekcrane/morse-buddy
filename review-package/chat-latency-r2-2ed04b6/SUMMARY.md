# Chat entry, viewport reads, Outbox scan and held input: round 2 on 2ed04b6

- **Base:** `2ed04b687983bafb4d8739c085655e2e23b67968` (`test/hw-functional-ui-fix4`, unchanged).
- **Where it lives:** the candidate is on `wip/chat-latency-r2`, a separate WIP branch, not for merge. You approved that branch after the cloud container was reclaimed. The earlier uncommitted work, including the previous candidate, was lost then, and this round was rebuilt from the design notes and the recovered files.
- **Not done:** nothing flashed, erased or uploaded. No OTA or release change. `test/hw-functional-ui-fix4` was not pushed to.
- **Unchanged:** device A's history and device B. Handover research was not touched; the untracked `host_tests/race_r3_r5/model/` files were lost with the container.
- **No new persistent format.** The comparison, and a proposal for your approval, are in `STORAGE_CACHE_DESIGN.md`.

## Device A's capture (2ed04b6, starting with a reboot), split

| | measured on device A | what it was |
|---|---|---|
| Chat entry, first | `CHAT_INDEX` list 158 ms + read 4446 ms (70 records) | one LittleFS open per record, about 63 ms each |
| Chat entry, cached | about 864 ms (`historyViewport` 415 + `historyRender` 447) | the visible rows read twice |
| Outbox | `scan 5143 ms found=0` | every record loaded in one call, once a group connected |
| Startup | `[PERF][MQTT] setup/connect/subscribe`, OTA check | network startup, separate from chat entry. The trace does not show a Wi-Fi disconnect. |

## What changed (`production.diff`: 12 files, including the new `held_input.h`)

1. **Record cache** (`loadMessageForDisplay()`).
   - Holds 10 records, byte-exact; the slot table and the bytes are on the heap.
   - Every store write updates its cached copy (write-through). A removal, a failed write or a change made outside the store drops it.
   - The render no longer re-reads what the viewport read, and re-entry and rotation read nothing they already read.
2. **The shared index stays current.**
   - Mutations of other conversations leave it valid.
   - Its own conversation's writes and removals are patched in place.
   - A **parked** second index keeps the last chat, so Everyone → 1:1 → Everyone reads nothing.
3. **The boot sweep reads each record once and shares it.**
   - Everyone is visited first, and its recount feeds that chat's index.
   - While a chat is open, the recount feeds the parked index instead. It never replaces the index an open chat draws from.
   - A fed index is used only if every listed record was read, in listing order, and that conversation did not change meanwhile; otherwise it is dropped.
4. **Time-budgeted steps.**
   - At most 4 reads, and about 20 ms, per main-loop pass: about one open per pass on device A.
   - The Notifications sweep and the Outbox pass pause while a chat loads its index.
5. **Outbox.**
   - The blocking scan is replaced by an incremental pass of **header-only** reads, at most 4 per step.
   - The pass runs once per boot after connecting, and again after a change made outside the store or a pass that could not read everything.
   - A record written pending is queued directly, so a send never causes a scan.
   - Pending records stay durable on flash: the flag is cleared only after a successful publish.
6. **Input while a chat loads** (Text and Enigma).
   - "Loading messages..." is shown on entry; a reload during a visit keeps the history on screen.
   - Input is held in order (32 events, heap) and replayed once the index is ready.
   - **Back** leaves at once and drops what was held.
   - **Held-input limits** are tested explicitly (IN2, IN3; the allocation failure is injected through `operator new[](nothrow)`). On overflow or allocation failure, the index is completed in that pass and the input goes live, so nothing is dropped.
   - **The Send window.** `isReplayedSend()` applies at dispatch to held and live events alike. IN4, IN4b and IN4c (Text, Text across a reload, Enigma) each give exactly one send attempt for a Send plus its replayed Short. A mutant that lets held events bypass the check is caught.
   - **A Back already queued** skips the history-side events before it: IN6, 20 rotations then Back, leaves in one pass with 0 opens. On 2ed04b6 the same sequence costs 28 opens, about 1.8 s in the model. A Send queued before the rotation is still sent (IN6b).
   - **Morse.** IN5 shows one tested sequence (SOS), keyed while the chat loads, gives the same draft as keyed live. This is not a claim that all Morse input is lossless.
7. **The selection follows its message.** The chat re-syncs with the store every pass, and the selection is kept by sequence across patches and reloads. This matters because selecting a message reads it and the older ones.
   - **Found while rebuilding:** on 2ed04b6, any reload that inserts a message above the selection (for example an arrival with an older timestamp) silently moves the selection onto another message, which is then read.
   - CE23 covers it, and ST3c still passes.

## Measurements: host only (device-cost model, 63 ms per file open). Not device timing.

`storage_test` (the same tests on 2ed04b6 in `evidence/before_runs/`):

| scenario | 2ed04b6 | this candidate |
|---|---|---|
| Everyone, 90 messages, after the boot sweep (CE1) | 98 opens in one pass | 4 opens, drawn in the first pass |
| Re-entry after arrivals elsewhere (CE2), after visiting a 1:1 chat (CE2b) | 98 opens each | 0 opens |
| Arrival in the open chat, with its eviction (CE3) | the index is not updated | 4 opens, index equal to a fresh load |
| Arrival during a cold stepped load (CE11) | 100 opens, one pass | 96 opens, no restart |
| Everyone opened 1 s after boot (DM1) | one blocking pass: 102 opens, 6446 ms | 4196 ms spread over 46 passes, at most 5 opens in a pass; input is held meanwhile |
| Everyone opened once boot work settled (DM2) | 6194 ms, one pass | 272 ms, one pass |
| Everyone opened 14 s after boot, 3 × 99 other messages (DM3) | 6194 ms | 272 ms, one pass |
| Boot passes before the chat (DM1–DM3) | up to 194 / 391 opens in one pass (the Outbox scan) | at most 2 opens per pass |
| Back after 20 queued rotations (IN6, IN6c) | 28 / 26 opens, about 1.8 s | 0 opens, one pass |
| 1:1 chat opened at boot, then Everyone (CE21) | 98–99 opens | 4 opens, drawn in the first pass |

Storage bench (`evidence/storage_bench/`; host counts × 63 ms):

| history | B2: Everyone opened after boot work (2ed04b6 → this) | B0b: last conversation opened during boot (2ed04b6 → this) |
|---|---|---|
| 1 × 70 | 544 → **40** ms | 9364 → **6062** ms |
| 40 × 20 | 1804 → **292** ms | 1804 → **1912** ms (+108 ms: cold load spread over passes) |
| 8 × 100 | 6844 → **292** ms | 57244 → **8552** ms |

## Validation

- **`storage_test`** (real store on a temp-dir LittleFS; real Notifications, Text, Enigma and Outbox):
  - this candidate: **137 passed, 0 failed** (`evidence/storage_test_this_candidate.txt`)
  - the same tests on 2ed04b6: 99 passed, 38 failed (`evidence/before_runs/`, shim listed there)
  - The previous candidate could not be re-run: its source was lost with the container.
- **Full `run_all.sh` suites** (steps 1–6b; the mutation step run separately below): all steps passed (`evidence/run_all_suites.txt`).
- **Targeted mutations** of the store, both chats, Notifications and the Outbox (`evidence/mutate_notify.txt`): **124/126 caught**, 0 unexplained survivors. The two survivors were assessed equivalent, with reasons in `mutate_notify.py`:
  - Without the explicit unreadable-record feed, the feed is dropped anyway (out of step).
  - Without the open-chat flag on the feed, the chat's per-pass re-sync reloads a stale index anyway, so the flag only saves cost.
  - The first run's 9 survivors led to new tests (CE11, CE22b, CE24, DM1b, OB5 after the boot pass, a stricter CE16) and to fixing two mutants that changed nothing.
- **Race mutations** (`mutate_room.py`, unchanged files): ROOMMUT (`evidence/mutate_room.txt`).
- **ESP32 build** (`evidence/esp32/`):
  - success: RAM 124,476 B, flash 1,206,349 B (83.7%)
  - static DRAM headroom **96 B** (232 B on 2ed04b6)
  - `firmware.bin` SHA-256 `bd635c6f1f56ca39abc4fb145031d7e1b6cfc97cc3e25dc5a9b87836512f9644`
  - The first link overflowed `dram0_0_seg` by 576 B, so the record cache's slot table was moved to the heap.

## Limitations

- **Device timing is not verified.** The figures above are host operation counts at 63 ms per open. `CAPTURE_WINDOWS.md` has a capture procedure that separates startup MQTT/OTA work from chat entry.
- **Cold entry stays one open per record.** A conversation opened before the boot sweep reached it (DM1, B0b) is spread over passes and stays responsive, but the total cost is unchanged. Removing that floor across a reboot needs the persistent index (design B), which is not implemented and needs your approval.
- **The Outbox's boot pass** still reads one header per stored record, once per boot after connecting, spread over passes (options C and D in the design).
- **Static DRAM headroom is 96 B.** New state is on the heap: record cache up to about 10 records plus the slot table, parked index about 4 KB, Outbox pass and queue while in use, held input while in use.
- **The Friend chat** still loads its index in one call.
- **Morse during loading:** one tested sequence only.
- **Behaviour changes to review:**
  - Events queued behind a Back are dropped when the chat is left. Before, the closed chat consumed them.
  - A second Send pressed while the chat loads falls inside the first Send's replay window and is dropped, as a press during a blocking send is.
  - A chat now reloads by itself after a change it could not follow (a failed write, a change made outside the store).
