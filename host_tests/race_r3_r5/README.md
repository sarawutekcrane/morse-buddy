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
