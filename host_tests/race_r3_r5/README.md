# Race R3–R5 (room protocol, design revision 8): host tests (HOST ONLY)

These are not firmware, ESP32 timing, memory, flash/NVS hardware, broker or
radio validation. The firmware build is recorded separately (status below);
these host tests don't depend on it.

**Run:** `./run_all.sh` from this directory (the repository root is found
from here; or set `REPO=/path/to/morse-buddy`).
- Needs g++ with ASan/UBSan and python3. Build outputs go to `out/`, which
  is ignored.
- `SKIP_MUTATIONS=1` skips step 7 (about an hour).
- `SOAK_SEEDS=n` widens the randomized soak in step 1 (default 20; 500 was
  also run).

**Layout:**
- test sources, fixtures (`fake/`, `fake_sys/`) and `run_all.sh` here;
- the executable design model in `model/` (`python3 -I model/race_model.py`,
  exit 0; output in `model/model_output.txt`);
- `model/handover_model.py`: an exhaustive model of the **proposed**
  coordinator handover (design only, nothing implemented; not run by
  `run_all.sh`, since a full run takes hours). Output in
  `model/handover_model_output.txt`;
- recorded results and build logs in `evidence/`.

## Status and remaining limitations

- **ESP32 build: succeeded.** Actual `pio run -e morse_buddy`, exit 0, no
  compiler warnings (`evidence/esp32_build_log.txt`; the baseline build is in
  `evidence/esp32_baseline_build_log.txt`; sizes in
  `evidence/esp32_sizes.txt`).
  - Firmware RAM 124,212 B (37.9%), flash 1,190,921 B (82.6%).
  - Target `sizeof(Engine)` = 2,108 B, allocated on the heap at service init.
- **Static DRAM headroom is only 360 bytes:** 124,220 of 124,580 B are used
  in `dram0_0_seg` (the baseline had 160 B free). Any static addition
  anywhere in the firmware can overflow the link again.
- **Unverified (needs hardware):**
  - runtime heap availability for the engine at boot;
  - ESP32 timing and stack use;
  - real NVS behaviour on power loss;
  - broker ordering and retained behaviour;
  - multi-device play and room push-to-talk audio.

  These host tests are not hardware evidence.
- **Other known limitations:**
  - an exhausted Race counter shows "Storage error" rather than a message of
    its own;
  - `nextId()` still wraps its own `idCounter` for message IDs (not used by
    Race);
  - in FC-B1 adoption the rewrite of the commit record is unchecked (it is
    re-derived at the next boot);
  - the two QUERY guards and one reservation guard are assessed equivalent
    from source, not proven.

## Suites (actual sources; only platform services are stubbed)

### `proto_test.cpp` (step 1)
- Runs several actual `RaceProtocol::Engine`s (`src/core/race_protocol.cpp` +
  `crc32.cpp`), each with its own NVS, clock and sequence counter.
- **Broker model:**
  - last-arrival retained, replayed on (re)connect;
  - per-group subscriptions;
  - test-controlled loss, delay/hold, duplication and reordering;
  - a per-tick receive-queue cap that drops overflow.
- **NVS model:**
  - power cut on the N-th write (the device stays dead until reboot);
  - write failures by key;
  - CRC corruption;
  - CRC-resealed binding mismatches.
- **Whole-run audits** (on every scenario's publish log):
  - every `r2/<X>` state comes from X (E15);
  - no (group, C, n, nonce, version) is published with two contents (E8);
  - `startRightVersion` changes only with a result or a holder change, and
    then equals that version (E12);
  - no device ever displays a lower version of the same room (checked on
    every delivery and tick, reboots included).
- **Randomized soak** (seeds 1..20, fixed; 2-entry queue on odd seeds), with:
  - 15% loss and 15% delay/reorder;
  - duplicates;
  - member reboots;
  - coordinator reboots and power cuts on coordinator writes;
  - flaky member NVS;
  - coordinator offline periods.

  After healing, every MEMBER converges to C's state, the score total equals
  the resolved rounds, nobody is listed ACTIVE without being a member, and
  there is no displayed rollback.

### `room_test.cpp` (step 2)
- The device under test is the actual `race.cpp` + `menu.cpp` with its
  actual engine. Peers are actual engines, connected by `race_fixture.h`'s
  broker model.
- Covers:
  - R1 (R1-1..R1-10) and R2 (R2-1..R2-8), rewritten for the room protocol;
  - the R3–R5 UI (U1..U13): claims, start right, outcomes, late claim, Leave
    row (member and owner), Back as navigation only, reboot "Reconnecting...",
    coordinator unavailable, frozen, storage pause, discovery and
    notification, room full.

### `race_test.cpp` (step 3)
- The history-browsing suite RB1..RB12, ported: the same cases, with packet
  injections replaced by the peer coordinator's real actions.
- RB4 is now "the room closes" (was RESET).
- RB3 now covers a missed result followed directly by the next round.

### Other steps
- `solo_test.cpp`, `friend_test.cpp` (step 4): unchanged tests.
- Step 5: C++11/17 syntax checks of every changed production file against
  host fakes, baseline vs candidate.
- Step 6: sizes.
- `mutate_room.py` (step 7): 90 targeted mutations of `race.cpp`,
  `race_protocol.cpp` and `identity.cpp`. Result: 87 caught; 3 survivors assessed equivalent (see the migration-order round for the third). The 2 QUERY survivors ("QUERY binding
  ignored", "QUERY not invalidated by Join") are redundant guards: every
  member-record change already clears the outstanding QUERY, and Join is
  reachable only from NONE. They are assessed equivalent from source, not
  proven. The first full run had 4 more survivors (a frozen member could
  guess, the duplicate gate, START entitlement, eviction order); tests
  U10b, E2 (no transient reactivation), E12 (stale entitlement) and E2b
  (departure order) now catch them.


## Engine heap round: the actual ESP32 build

**Problem:** the first real ESP32 build of the candidate failed to link:
`dram0_0_seg` overflowed by 1,744 bytes. The baseline leaves only 160 bytes
of static DRAM, and the engine was a static global.

**Fix** (`race.cpp` only):
- The engine is allocated once, in `raceServiceInit()`, with
  `new (std::nothrow)`, and kept for the device's lifetime (never freed).
- **Repeated initialization preserves an existing engine:** no new
  allocation, no replacement, no re-boot.
- **After an allocation failure, another `raceServiceInit()` call retries
  the allocation.** The firmware calls it once per boot (service init), so in
  practice Race stays unavailable until the next boot.
  - Note: the code comment above `raceServiceInit()` in `race.cpp` says "a
    repeated call does nothing". That is accurate only while an engine
    exists. The comment is left as reviewed (the committed sources match the
    reviewed patch); this README is the corrected description.
- If the allocation fails, `g_engine` is nullptr and every engine path checks
  for it:
  - MQTT handlers, the service tick, `syncRound`, `shownRoom` and
    `isRoundActive`;
  - row building, action and status text, the action and row handlers;
  - Leave, the local solve, and room voice (via `shownRoom`).
- In that case the Room shows "Race unavailable" with no actions, and nothing
  reads or writes the Race records.

**Tests** (`room_test`):
- **U18:** allocation failure at boot for a member with saved records:
  - "Race unavailable"; every input, inbound packet and tick is inert;
  - no publishes, records and counters byte-identical, Back and Race entry
    work.
- **U18b:** a later init call after the failure (as at the next boot) retries
  the allocation and restores the same membership.
- **U19:** repeated `raceServiceInit()`:
  - no allocation, same engine, no re-boot, writes or publishes;
  - an active engine is kept even if allocation would fail.
- Allocation failure is injected by replacing the no-throw `operator new` in
  the test binary.

## Hardware findings on 30ed514: defect fixes (uncommitted candidate)

Investigation: `investigation/`. Before/after:
`evidence/defect_fixes_before_after.txt` (the same new tests, built against
30ed514's sources and against the candidate).

1. **Guess history lost after Leave and rejoin.**
   - `race.cpp` now binds the history to the exact round: group,
     coordinator, n, nonce and the round's start version (`RoundBinding`).
   - An absent view (Leave completed, joining, Reconnecting) keeps it; any
     other view (another round, the lobby, another room) discards it.
   - It stays within one boot; power-cycle persistence is out of scope.
   - **Tests** (`room_test`):
     - HB1-HB3: restored after a completed Leave, a late acknowledgement and
       an engine reboot;
     - HB4-HB5: discarded for a later round or the lobby;
     - HB6-HB10: never shown in another room with the same round number,
       including rooms differing only in coordinator, n or nonce.
   - **Mutations:** 7, all caught.
2. **Score 1 not displayed.**
   - Cause: the score was right-aligned by ink width, but `printLine()`
     clips by left bearing + ink width; FreeSans "1" has a 3 px bearing.
   - Fix: new `Display::fitWidth()` (the clip measure), used for the score.
   - **Host fake:** `T::realGlyphs` makes it measure with the real
     FreeSans9pt7b glyph table (`font_metrics.h`, from Adafruit GFX 1.12.6)
     and clip like `display.cpp`.
   - **Tests:** SC1 (0-65535 drawn whole, ending 2 px from the edge) and SC2
     (the real Room shows "1" after a first win).
   - **Mutations:** 1, caught.
3. **Unread `*` stays.** (Revised by the 23293c5 findings below.)
   - **Text and Enigma chats:** selecting a history item (rotating onto it)
     reads it and every older received message of the same type in that
     conversation (`Notifications::readUpTo()`). Entering a chat, or a
     message only being in view, reads nothing.
   - **`notifications.cpp`:** unread counts are kept per stored type (Text,
     Enigma, Game). The Text badge is Text or Game; the Enigma badge is
     Enigma or Game.
   - **Per-conversation state:** COUNTED (exact per-type counts) or
     UNRESOLVED (counts unknown; per-type flags keep the badges lit).
     UNRESOLVED conversations are recounted from the stored messages in the
     background, once a scan is reliable.
   - **Stored-record loading:**
     - `summary2` is used when present and readable;
     - if `summary2` is present but unreadable: retried every 10 s, never
       overwritten, and never replaced from the older legacy record;
     - else the legacy `summary` is migrated: each conversation it lists
       becomes UNRESOLVED with its old categories and is recounted in the
       background (never an approximate count);
     - if the legacy record is unreadable or has an unexpected length:
       retried, nothing written;
     - while a record is unreadable, arrivals are tracked in RAM and
       merged in as UNRESOLVED once it is read;
     - every count read from `summary2` is verified once: each conversation
       listed with unread messages becomes UNRESOLVED (badges unchanged) and
       is recounted;
     - after every successful load a sweep recounts every stored
       conversation (`MessageStore::listConversations()`), including ones
       listed with zero counts. It recovers unread messages the stored
       summary missed: an arrival while it couldn't be read or written
       followed by a reboot, or a power loss after the message was stored
       (unread) but before it was counted;
     - the legacy record is never written.
   - **Background work** (recounts, the sweep, applying read-ups): one step
     at most every 25 ms of main-loop passes. A step is ONE of: one
     conversation listing (names only, at most 300 entries), up to 4 record
     reads, one record rewrite, one NVS write, or one retry of an
     unreadable summary. Nothing on the arrival, read or eviction path
     scans a conversation.
   - **Recovery time is not guaranteed.** A recount of a conversation with
     M records takes at least 1 + M/4 steps; the sweep, all conversations.
     That is a lower bound, not a completion time: read-ups and UNRESOLVED
     recounts go first, an unreliable listing or record repeats its step,
     a summary that stays unreadable holds recounts back until it loads,
     and while storage keeps failing the work may not complete at all.
   - **Saves:** the summary is written about 2 s after its first unsaved
     change (coalesced); a failed write is retried every 10 s without
     stopping the other work. An unsaved change lost to a power loss is
     recovered by the sweep from the messages' flags.
   - **Eviction** of an unread record makes its conversation UNRESOLVED (its
     badges stay lit) and it is recounted in the background.
   - **Tests:**
     - `notify_test` NR1-NR38 (step 4b; fake message store, built with
       `-DREAL_NOTIFICATIONS`): marking, categories, migration, unreadable
       records, reboot before recovery, the sweep. Since 23293c5, NR2 tests
       the read-up rule, `stayOn()` lets deferred writes happen where a test
       means them to be stored, and `notifyTick()` runs 25 ms service passes
       over a 10 s retry interval;
     - `storage_test` ST1-ST14 (step 4c): the same against the ACTUAL
       `storage_messages.cpp` on a host directory (`fake_fs/`), see below;
     - `reset()`/`reboot()` in both return every volatile notification
       variable to its power-on value (heap objects freed first); only the
       simulated NVS and message store survive a reboot.
   - **Mutations:** `mutate_notify.py`, 41, all caught by notify_test or
     storage_test (step 7b).

ESP32 build of the candidate:
- RAM 124,316 B, flash 1,194,837 B (`evidence/esp32_defectfix_final_build_log.txt`);
- static DRAM 124,324 of 124,580 B, **256 B headroom** (was 352 B at 30ed514);
- the extra 96 B of static DRAM are the per-type summaries, the notification
  load and recovery state, the round binding and two per-chat read-check
  variables.

## Hardware findings on 23293c5 (device A): fixes

Measured with `storage_bench` (step 6b) and asserted by `storage_test`
(step 4c): the ACTUAL `storage_messages.cpp`, `notifications.cpp`,
`text_message.cpp`, `enigma.cpp` and `menu.cpp` on a real host directory
(`fake_fs/LittleFS.*`: files and directories are real; every open, read,
write, rename and directory entry is counted; opens can be failed on
purpose). Host time is not device time; the operation counts carry over.

1. **Unread `*` does not clear.**
   - Cause: firmware up to 30ed514 cleared a stored message's unread flag
     only on a DOT action over it (or if it arrived with its chat open), so
     a long-used device holds a flag on nearly every received message.
     23293c5 recounts badges from those flags and read them one selected
     message at a time.
   - Fix: read up to the selection, scoped to group, contact and message
     type. Messages shown below the selection, or stored after it, stay
     unread. The selection records a read-up request (RAM, then the
     `readup` NVS record, written about 1 s later); the flags on flash are
     cleared in the background, a few records per step, and re-checked by
     a second pass before the request is marked done. Recounts treat
     messages a pending request covers as read. The badge follows at once
     when what stays unread can be counted from the chat's index with at
     most 4 record reads.
2. **Empty-draft Send opens the Number Guessing game** (Text and Enigma,
   direct chats). Existing behaviour since Phase 3 (also at 30ed514): the
   empty-line hook started Friend's challenge flow. Now an empty draft sends
   nothing and opens nothing, as in broadcast. Friend games start from
   Training Game > Number Guessing > Play with Friend (unchanged); Friend's
   own chat keeps its empty-line choice. Tests: friend_test R17, R18, F1f,
   F1g.
3. **Slower menus and chats.** Measured cause (8 conversations x 300
   messages, legacy flags), 23293c5 vs the fix:
   - background: one service call recounted a whole conversation, 600 file
     opens (an open per directory entry, then one per record), about every
     10 s per conversation; now at most 4 opens, one listing (300 names, no
     opens) or one rewrite per step;
   - encoder rotation onto an old unread message: up to 17 opens plus a
     record rewrite and an NVS write; now up to 15 opens, no write;
   - opening a chat: 608 opens (also at 30ed514); now 308, because
     directory listings read names only (`getNextFileName()`);
   - also fixed in `storage_messages.cpp`: a listing no longer counts an
     interrupted rewrite's `<seq>.msg.tmp` as a second record (ST13), and a
     failed open of one contact directory no longer ends the conversation
     listing early while reporting it complete (ST9).

`storage_test`: ST1 real round trip; ST2 legacy flags cleared by one
selection; ST3/ST3b/ST3c type, display-order and storage-order scoping;
ST4 entering clears nothing; ST5 reboot mid read-up; ST6 encoder pass does
no rewrite or NVS write; ST7 per-pass bounds and convergence, 8 x 300; ST8
NVS writes failing; ST9 unreliable directory; ST10 arrival during a
recount; ST11 failed rewrite retried; ST12 recount under way does not
relight the badge; ST13 leftover `.tmp`; ST14 one unreadable record.

### Retention 100 and the outstanding review fixes (uncommitted candidate)

- **Retention: 300 -> 100 messages per conversation** (approved).
  `MessageStore::kRetainedMessagesPerThread = 100`; `kMaxMessagesPerThread`
  stays 300 as the listing capacity, because conversations written by
  older firmware hold up to 300 records (it evicted at 300 before every
  write, so 300 `.msg` files is their bound). Appends evict at 100, one
  record per append, in the existing order (lowest sequence first, records
  pending in the outbox kept).
- **Trimming older history in the background**, including conversations
  that receive nothing new: the boot sweep (and the sweep after a recovered
  summary load) visits every conversation, trims it, then recounts it. One
  deletion per 25 ms step at most, oldest sequence first, pending-outbox
  records kept, message bytes of the retained records untouched (ST15).
  A deleted unread record is uncounted only after its deletion succeeded;
  a failed deletion stops that conversation's trim (nothing is skipped)
  and the sweep runs another pass after a growing pause (2 s doubling, at
  most 60 s) (ST17). The conversation open on screen (Text, Enigma or
  Friend chat: all register through `TextMessage::setOpenConversation()`)
  is not trimmed until it is left (ST22, draft kept). A conversation with
  more than 300 records (not producible by older firmware) is reported as
  an incomplete listing, which keeps the 300 LOWEST sequences, so trimming
  still proceeds oldest-first and the next pass trims the rest; recounts
  never accept a truncated listing (ST23/ST23b, also in reverse directory
  order).
- **Incremental conversation traversal.** The sweep no longer lists every
  group/contact in one call: `beginConversationWalk()` /
  `stepConversationWalk()` read at most 16 directory entries per step
  (ST21: 400 conversations in 4 groups plus 200 empty group directories,
  at most 20 directory entries per loop pass, none skipped).
- **Read-up never marked DONE while something is left.** An attempt is up
  to 3 passes; if reads or flag writes still fail, the request stays
  pending (not done), is retried after a growing pause (2 s doubling, at
  most 60 s), and the other jobs run in between; after a reboot it is
  loaded again and retried (ST19: writes failing for 600 s and across a
  reboot, the boot sweep still recounts another conversation, everything
  clears once writes work). Recounts that give way back off the same way.
- **Durability of a selection.** The read-up request is in RAM first and
  reaches NVS in the deferred save about 1 s later. Power lost before that
  save loses the selection: after reboot the messages are still unread,
  nothing is wrongly marked read, and the badge and counts follow the
  flags on flash (ST20). No unconditional reboot durability is claimed.
- **Listings report truncation**: `listMessageSequences()` and
  `findMessagesByPredicate()` report an incomplete listing instead of
  silently dropping records beyond 300.

Legacy histories in the tests are written in the record format directly
(`legacy_store.h`), each record read back through the ACTUAL
`MessageStore::loadMessage()`; the current append path cannot build a
300-record conversation.

New `storage_test` cases: ST15 trim to the newest 100 (pending kept, bytes
unchanged, counts, per-pass bounds); ST16 trimming across a reboot; ST17
failed deletions; ST18 unread eviction keeps counts and badges consistent;
ST19 read-up failures beyond three passes and across a reboot; ST20 power
loss before the deferred read-up save; ST21 400 conversations; ST22 open
conversation not trimmed; ST23/ST23b more records than the listing holds;
ST24 the listing/scan contract beyond capacity (both directory orders);
ST25 an append at 100 evicts at once; ST26 counts equal flash at every
loop pass while deletions fail and while trimming proceeds, and the kept
set is always a contiguous newest run; ST27 an unreadable old record stops
the trim (nothing newer deleted in its place). `notify_test` NR39: an
UNRESOLVED recount with unreliable listings backs off (bounded listings).

Evidence (`evidence/`): `run_retention100_output.txt` (full run, all steps
passed: notification/store mutations 67/67); `esp32_retention100_build_log.txt`
(RAM 124,340 B, flash 1,198,433 B; static DRAM 124,348 of 124,580 B,
headroom 232 B); `retention100_storage_test_before.txt` (the new storage
tests on the previous candidate: 12 checks fail; adapted only as in
`retention100_storage_test_before_adaptation.diff`; ST24-ST27 were added
later); `retention100_storage_bench.txt` (30ed514, 23293c5, previous and
this candidate, 8 x 300 and 400 x 20); `retention100_notify_test_nr39_on_mutant.txt`;
`devicea_BEFORE_23293c5_*.txt` (device A findings reproduced on 23293c5,
with that round's tests). Device timing is not verified.

Known limitation: a record that stays unreadable among the oldest blocks
that conversation's trimming (it is reported and retried every pass, never
skipped or guessed to be deletable, since it might be pending in the
outbox); the rest of the sweep continues.
`notify_test` NR13 now waits out the recount backoff after the scans
recover (30 s instead of 10 s).

Measurements (`storage_bench`, step 6b; host op counts, not device time):

| workload / build | 30ed514 | 23293c5 | previous candidate | this candidate |
|---|---|---|---|---|
| 8 x 300: open chat (legacy history) | 608 opens | 608 | 308 | 308 |
| 8 x 300: open chat after idle | 608 | 608 | 308 | 108 (trimmed to 100) |
| 8 x 300: background, max per loop pass | none | 600 opens | 4 opens / 300 entries | 4 opens / 300 entries / 1 remove |
| 8 x 300: all trimmed to 100 | never | never | never | after 73 s (simulated) |
| 400 x 20: background, max per loop pass | none | 40 opens / 421 entries | 4 opens / 401 entries | 4 opens / 20 entries |

## Heap diagnostics (for hardware acceptance)

`race.cpp` writes one serial line per heap reading:

`[RACE][HEAP] <event> uptime_s=<s> free=<bytes> min_since_boot=<bytes> largest_block=<bytes> (bytes, internal 8-bit)`

- The figures come from `heap_caps_get_free_size`,
  `heap_caps_get_minimum_free_size` and `heap_caps_get_largest_free_block`
  for `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- **Meaning of `min_since_boot`:** `heap_caps_get_minimum_free_size()` adds
  each matching heap region's own low-water mark (ESP-IDF
  `esp_heap_caps.h`).
  - Regions can reach their lows at different times, so the sum can be lower
    than the free total ever was at any one moment.
  - It is a conservative ("worst case") indication. It is not the exact
    minimum, and it doesn't say when the low occurred or what caused it.
  - The label is only a name. The comment above the diagnostics in
    `race.cpp` ("the lowest free bytes since boot") is imprecise in the same
    way; it is left unchanged in this documentation-only revision.
- **Events:**
  - `pre-alloc` and `engine-alloc-ok size=N` / `engine-alloc-FAILED size=N`
    at Race service init (`N` = `sizeof(Engine)`);
  - `room` on entering the Room (skipped within 5 s of the last reading);
  - `periodic` from the service tick, at most once per 60 s.
- The line is formatted into a stack buffer and carries no room, guess,
  credential or message data.
- **Tests** (`room_test`):
  - **U20:** before and after figures, with the fake heap lowered by the
    allocation size; the exact line shape; the capability mask.
  - **U20b:** a failed allocation logs FAILED with unchanged figures;
    repeated init logs nothing.
  - **U20c:** a burst of 200 packet deliveries logs nothing; 9–10 periodic
    lines over 600 s, each at least 60 s apart.
  - **U20d:** Room entry logs once, rate-limited to one per 5 s.
- room_test grows from 267 to 295 checks. `mutate_room.py` adds 9 H1
  mutations of the diagnostics. The two F7 init anchors follow the new
  `pre-alloc` line.
- The host fake is `fake/esp_heap_caps.h`. Serial lines are captured
  through `g_hostSerialSink`.
- These tests check labels, values and rate limits on the host only. They
  are **not** a hardware heap measurement.
- The UI suites reboot the service with a test-only helper that deletes the
  engine; the firmware never frees it.
- Mutations: 10 new F7 mutations, all caught. Missing guards crash (null
  engine) inside U18.

**ESP32 build** (actual, `pio run -e morse_buddy`;
`evidence/esp32_build_log.txt`):
- exit 0, no compiler warnings;
- RAM 124,212 B (37.9%), flash 1,190,921 B (82.6%);
- static DRAM 124,220 of 124,580 B, 360 B headroom (baseline: 124,420 B,
  160 B headroom);
- target `sizeof(Engine)` = 2,108 B, now on the heap.

**Not verified:** runtime heap availability at boot on the hardware.

## readStored round: no failed read's default is ever taken as stored data

**Defect:** stored `raceSeq` = 50, the first `getULong()` fails and returns
0, the second succeeds with 50. The helper then accepted 0, so the next
reservation issued 1. The same happened for the `idCounter` migration
(`raceSeq` stored as 0).

**Fix** (`identity.cpp`, `readStored()` only):
- `Preferences::getULong()` reports no status, so the key is always read
  twice, with defaults 0 and 0xFFFFFFFF.
- The value is accepted only if both reads agree. A failed read returns its
  own default, so a failure can only agree when the stored value equals that
  default, which is then the value returned.
- Any disagreement fails, leaving `*out` and both counters unchanged.

**Regressions** (`identity_test` S12 for `raceSeq` reservation, S13 for the
`idCounter` migration at boot):
- per-call read faults on the target key: none, first fails, second fails,
  both fail;
- stored values 0, 50 and 0xFFFFFFFF.

Every outcome must be either the exact stored value used correctly or a
refusal with the output and both counters unchanged, followed by correct
continuation once reads work.

**Before/after:** `evidence/regression_readstored_before_after.txt`. Previous
candidate (patch 77bb1966…): 4 failures, all "stored 50, first read fails"
in S12 and S13. Now: 0. Two new F6 mutations, both caught.

## Migration-order round: Race's floor stored before idCounter can change

**Problem:** migration was lazy, happening on the first Race reservation.
With `raceSeq` absent and `idCounter` at UINT32_MAX, a `nextId()` call
first stored 0, and the reservation then migrated from 0 and returned 1.

**Fix:** `identity.cpp` only.
- `Identity::init()` stores `raceSeq` = the current `idCounter` at boot.
- Until `raceSeq` exists, `nextId()` first tries to store it and does not
  write `idCounter` if that fails. That is what already happens when its own
  write fails: the same ID may be returned again.
- Reservations also store the floor first.
- An unreadable `idCounter` (or `raceSeq`) is told apart from a stored value
  (`readStored`), so nothing migrates from, or restarts at, a default.
- An exhausted migration is stored as `raceSeq` = 0xFFFFFFFF, so it stays
  exhausted after `idCounter` wraps.

**Limit (not a recovery claim):** the floor is `idCounter`'s current value
only. A wrap that already happened before this firmware (on any earlier
firmware) can't be detected from it, and no earlier maximum that was never
stored is recovered. Race numbers came from `idCounter` only in the
unreleased R3–R5 candidates.

**Regressions** (`identity_test` S8–S11, actual `identity.cpp`):
- `nextId()` before the first Race reservation, with and without `init()`;
- migration write failure (everything, or only `raceSeq`), then `nextId()`,
  then reboot;
- exhausted migration, then `nextId()` wrapping `idCounter`, then reboots;
- unreadable `idCounter` at boot.

**Fixture correction:** S4 now sets `idCounter = 5` directly. Calling
`nextId()` five times no longer models "Race numbers used earlier", because
those calls now store the floor first.

**Expectation change:** `room_test` U16 (a) now requires the exhausted
migration to be stored (`raceSeq` = 0xFFFFFFFF). It used to require
`raceSeq` absent.

**Before/after:** `evidence/regression_migration_before_after.txt` (previous
candidate patch bc0e0f6a… vs now).

**Mutations:** F4/F5, 7 new. "F5: reservation without the stored floor" is
assessed equivalent: with `raceSeq` absent, `readStored()` refuses anyway.

## Shared-counter round: Race sequence numbers isolated from nextId()

**Fix:** `Identity::reserveCounter()` now uses its own counter, `raceSeq` in
mb_core. `nextId()` and its `idCounter` are unchanged; they still wrap, as
before, for message IDs only.
- **Migration:** starts from `idCounter`, because every earlier Race number
  came from it. It is stored eagerly; see the migration-order round.
- **Read errors:** an unreadable value refuses (default 0xFFFFFFFF), never 0.
- **Limits:** no wrap at 0xFFFFFFFF; nothing is returned unless it was
  stored.

**Regression:** `identity_test.cpp` (step 1b, actual `identity.cpp`).
- Counter near UINT32_MAX with prior Race numbers; `nextId()` and Race
  reservations interleaved across the wrap of `idCounter`; exhaustion;
  reboots.
- Write and read failures.
- `room_test` U16 now covers exhaustion before and after migration. U15
  injects failures on `raceSeq`.
- Before/after: `evidence/regression_counter_before_after.txt`. These are the same
  sources against the previous candidate (patch 40d7972f…) and the current
  tree.
- Mutations: 4 new F4 mutations, plus the updated F1 identity mutations.

**ESP32 build at that time:** UNVERIFIED (registry access denied). It was
built later; see the status section and the engine heap round.

## Review round: durable numbers, freeze enforcement, durable exits

**Fixes:**

- **F1:** `Identity::reserveCounter()` reports failure and refuses to wrap
  past 0xFFFFFFFF (now with its own counter; see the shared-counter round). The engine sends nothing with an unreserved number:
  - Join, Leave, Start, Claim and createRoom are refused with "Storage
    error";
  - a QUERY stays outstanding with no number and is retried;
  - reconciliation is skipped until a later state.

  `nextId()` is unchanged.
- **F2:** while frozen, replies are ignored and `tick()` sends nothing (no
  resend, no QUERY). Only Leave works.
- **F3:** a terminal transition completes only once NONE is stored.
  - On failure the room is kept, the engine is in "Leave only" with
    `exitPending()`, and the write is retried every 2 s.
  - A frozen Leave first stores LEAVING with its LEAVE, so a reboot resumes
    leaving.
  - Other member-record writes that may fail (completion, refusal, boot and
    coordinator-derived) are retried until stored.

**Fault injection:**
- The protocol harness has its own identity-counter fault knob
  (`failCounter`), separate from the NVS model.
- The Race UI suites now link the **actual `identity.cpp`** (built with
  `-DREAL_IDENTITY`; the MAC comes from `fake_sys/WiFi.h`). Its counter
  lives in a fault-injecting `Storage::core()`.

**Regressions:**
- `proto_test` F1a–F3e and `room_test` U15–U17.
- Before/after: `evidence/regression_before_after.txt`. These are the same test
  sources built against the previous candidate (patch sha256 6684871c…) and
  the fixed one.
- Before the fixes: 28 failures, all in these new tests. After: 0. Every
  pre-existing test passes on both.
- 13 new targeted mutations (one per fix element), all caught.

**`run_all.sh`:**
- Every compile and every test binary is checked by exit status.
- A compiler error, a failing test, or an unexplained mutation survivor
  makes the script print `RESULT: FAILED` and exit 1.
- `|| true` remains only on display-only `grep` filters, never on a
  compiler.
- Shown with an injected compiler failure: `evidence/run_badcxx.txt`, exit 1.

## E1–E27 → tests (proto_test unless noted)

| E | Tests |
|---|---|
| E1 | E1 (no-entry cancellation, requester offline); E1 another device's STALE_BASIS → NONE, no reissue |
| E2 | E2 (ordering by lastSeq), E2b (eviction by smallest departedAt, floor). Ties by index: not reachable (one departure per commit), not tested |
| E3 | E3 (completion from a later RS), E3 REPLY OK + RS ≥ curVersion, QUERY when the RS lags |
| E4 | E4 (each field negative), E4 request replies (positive, cur ≠ record → gone), E4 QUERY replies |
| E5 | E5/FULL lost reply (terminal, no reissue), E5 Leave replaces JOIN, E5 JOINING + FULL/NOT_ALLOWED/UNKNOWN_ROOM/CLOSED, MEMBER rows, LEAVING + Leave anyway, "no longer in this room", START rejected; room_test U13 (Room full) |
| E6 | E6 power cut at each commit step |
| E7 | FC-A, FC-B1 (a) and (b), FC-B2, FC-A with closed writes nothing, 12 binding mismatches (slotId, slotSeq, group, n, nonce, version, RS coordinator, commit group/n/nonce/version/slotSeq) and commit coordinator |
| E8 | E8 exhaustion at 0xFFFFFFFE/0xFFFFFFFF, DATA_LOST at the limit, nothing after; audit "no version published twice" |
| E9 | E9 (close → new room by the other device, scores 0, old requests CLOSED/UNKNOWN_ROOM), E9 stale RS + resurrected slot repaired |
| E10 | E10 identity storage lost; E23 C unreachable (no timeout); room_test U9 (60 s without C) |
| E11 | E11 (QUERY invalidated, CLAIM resent identically), E11 each member step (JOIN, CLAIM) |
| E12 | E12; audit on startRightVersion; room_test U2 |
| E13 | E13 |
| E14 | E14; room_test U7 (Back), U8 (reboot/disconnect) |
| E15 | audit on every scenario |
| E16 | E16 sizes from the actual encoders; room_test R1-1 (scores beside names), U5/U6 (Leave row); R1/R2 and history browsing (room_test, race_test). Old/new firmware isolation: by the topic table only (source), not tested |
| E17 | E17 |
| E18 | E18; E18 power cut at every Create and Owner Leave step |
| E19 | E19 (FC-B1 CLOSED at max; non-consecutive → FC-B2) |
| E20 | E20 (CLOSED reply while a member elsewhere), E20 republish from the slot on demand |
| E21 | E21 (floor), E21 Reconnecting until RS; room_test U8 |
| E22 | E22; room_test U11 |
| E23 | E23 (both directions, equal and higher n, genuine replacement) |
| E24 | E24; E24 Leave/Join/reboot/terminal invalidators |
| E25 | E25 |
| E26 | **model 11:** E26 restore at exactly appliedV with every NVS write failing: restored, zero writes attempted; plus completion of the pending CLAIM |
| E27 | **model 12:** E27 frozen: a later RS (that would admit / complete) is not applied, no Start/claim, Leave works; E27 frozen with a pending CLAIM, one LEAVE sent; room_test U10 |

## Test-expectation changes (explicit)

**Replaced (they test the old INVITE/JOIN/ROUND/SOLVED/RESET protocol, which
this change removes):**
- `race_regress/race_test.cpp` (Phase 2A, 216 checks) and
  `race_regress/race2c_test.cpp` (Phase 2C-1, 92 checks), from the previous
  bundle. Not included and not built: they reference `g_phase`,
  `handleRace*Packet` and the old payload codecs. Also dropped:
  `mutate_browse.py` and `baseline_probe.cpp`, which target files this
  change doesn't touch (the shared browse helpers in number_guessing.cpp).
- Their still-relevant behaviours are re-tested on the new protocol:
  - replay window (R2-6, RB7, RB11);
  - Start/Continue/Guess entry (R2-1..R2-8);
  - "Round ended" (RB4);
  - outcome text (U3, RB5);
  - Ready gate (R2-5).
- The old "Start unconfirmed"/"Join unconfirmed"/"Invite unconfirmed"
  notices are gone: a coordinator change is committed and republished until
  its echo, and a member request is durable and resent.

**Results (final run, `evidence/run_r3r5_output.txt`):**
- proto_test: 1,028 checks;
- identity_test: 85 checks;
- room_test: 267 checks;
- race_test: 100 checks;
- solo: 52 checks;
- friend: 336 checks;
- 0 failures, no sanitizer reports;
- 500-seed soak: 7,268 checks, 0 failures (`evidence/soak500_output.txt`).

**room_test.cpp** (old 77 checks, rewritten):
- Joins, rounds and names now enter through actual peer engines.
- Rows carry right-aligned scores and end with "Leave room", so row texts
  gained " 0" and the focus clamps at the Leave row.
- The title range counts players, not the Leave row.
- R1-6 (removal) now uses real Leaves instead of editing the list.
- R1-9 is "the room closes" (was applyReset).
- R2-4 is "publish lost → still in Guess, republished until echoed" (was
  "Start unconfirmed").
- R2-8 Join is via the discovered row.

**race_test.cpp:** RB4 RESET → room closed; RB3 new round → missed result
followed by the next round; RB2c/RB11 count CLAIM requests (was
`race/solved` publishes).

## Fixture changes

- **`stubs.cpp`:**
  - `Identity::deviceId()` is now `DEADBEEFAAAA`, 12 hex characters like the
    real MAC id. The protocol validates ids as hex.
  - friend_test.cpp's one literal was updated to match.
  - `publishBinary` keeps the bytes, group and retained flag and calls
    `T::onPublished`.
  - `Storage::race()`, `Storage::core()` and `esp_random()` were added. The
    Race suites build with `-DREAL_IDENTITY` and link the actual
    `identity.cpp` instead of the Identity stubs.
- **`fake/Preferences.h`:** an in-memory store with the verified 2.0.17
  semantics (`putBytes` returns 0 on failure; `getBytes` returns 0 when the
  key is absent or the buffer is too small; `isKey`), plus write-failure
  and per-call read-failure injection.
- **Other fakes:**
  - `fake/Arduino.h`: `esp_random`, `delay` (declarations).
  - `fake/Adafruit_ST7789.h`: the boot-prompt methods, for the syntax check
    only.
  - `fake_sys/`: WiFi.h, LittleFS.h and MQTT.h stubs, for the syntax check
    only.
- **`race_stubs.cpp`:** counts `setRaceInvitePending(true)`.
