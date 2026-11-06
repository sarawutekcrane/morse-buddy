# Per-message read: cancel only on trustworthy completion (round 7, `wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Reviewed round-6 source and tests | `5af469509fa1d652ed23a06d14232ce090fa3958` |
| Its package-only commit | `f1136f588cc3225c448213a10bd41bf29f5ec2d4` |
| This candidate (source, tests and ESP32 build: the same commit) | `0f2e76ba34b6f259a756811f438a76ea4415c275` |
| Base of the cumulative diffs | `511d13e586432e22141be9691c1401e70eac4e41` |

**Preflight:**
- HEAD was `f1136f5`, equal to origin, with a clean tree.
- `5af4695` is an ancestor; only the two package commits lay above it.
- `src` and `host_tests` were identical to `5af4695`.

**Diffs:**
- `production_since_5af4695.diff`: incremental, 3 files.
- `production_since_511d13e.diff`: cumulative.
- `host_tests_since_5af4695.diff` and `host_tests_since_511d13e.diff`: these include the new files.
- `test_sources/`: every test, fake and script source, as at `0f2e76b`.

**Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. No persistent journal or format. No filesystem refactoring.

## 1. The problem, reproduced

On `5af4695`, an accepted read was dropped after two consecutive listings that omitted its record. Through the Arduino API, a directory read error ends a listing exactly as the end of the directory does.

**PO7** (Text and Enigma) reproduces the reviewer's counterexample:
1. The read is accepted.
2. The record's opens fail.
3. Each listing in the read's own step ends at once.
4. The record stays on flash and unread throughout; this is checked from the raw file bytes, never through the store.

On `5af4695` the request is gone after the second attempt, and after recovery the message is never read.

The faults are tied to the read's own attempts:
- The target leaves the record cache by reading 12 other records.
- No outside-change signal is raised, so no recount, Outbox or chat-index listing runs.
- An open is counted only once per step time, and only the listing in that same step is cut.

## 2. The correction (`notifications.cpp`, `storage_messages.cpp`)

**No absence verdict any more.** `recordConfirmedGone()` (the directory scan) and `absentSeen` are removed. A record that cannot be loaded is unreadable for now, or gone; the API cannot tell these apart. Its request:
- keeps its slot;
- is retried with backoff (2 s doubling, at most 60 s);
- is moved behind the others.

One retry costs one failed open. No listing is made.

**Trustworthy completion only.** A request ends when:

| Event | How it ends |
|---|---|
| Its rewrite succeeded | Done; the count is decremented |
| Its record, loaded, is already read | Done; if an attempt had failed, the conversation is recounted |
| Its record, loaded, holds another message id (sequence reused) | Done, without touching that message (PM10) |
| The store reports that exact record's eviction as done (`removeRecordFile()` after a successful remove), or the trim's `deleteMessage()` succeeded | Cancelled by group, contact and sequence (PO8) |
| A failed removal | Nothing is reported or cancelled (PO8) |
| The same message read by its explicit DOT action (only after that rewrite succeeded) | Cancelled by group, contact, sequence and message id (PO11) |
| Its group is no longer configured, checked at its next attempt | Ended without writing (PO12) |

- An eviction whose header could not be read used to go unreported. It is now reported, conservatively as unread, so the count is recounted and its request ends (PO8 variant 2).
- Group deletion no longer cancels pending reads in the settings hook. That hook fires before the files are removed, and the per-file removes ignore their results. The configured-group check stands in for it.

**Uncertain disappearances are retained until reboot.** Examples are a rewrite whose rename failed after the old file was removed (`atomicRewrite()`), or a file removed outside the store. The request occupies one of the 32 slots until reboot, retried about once a minute. It never blocks other requests or other background work (PO9: a read in the same conversation and in another one, and a recount, all complete meanwhile). The finite-capacity refusal policy from round 6 is unchanged.

**Counts after ambiguous outcomes.**
- After a failed rewrite, the record is read back.
- If it cannot be loaded, or holds another id, the rewrite's outcome is unknown, and the conversation is marked for a recount (PO10: count 2 = flash 2 after the record was lost).
- A summary that is UNRESOLVED is only a category flag until the recount. So counts are not claimed to match flash at every moment of a failure window; they match once the recount has run.

**Power-loss window:** unchanged. Accepted reads are RAM only. A power loss between the move and the rename of the record's rewrite leaves the message unread.

## 3. Tests: the same final tests on both candidates

| `storage_test` | Result |
|---|---|
| on `5af4695` | 322 passed, **12 failed** (`evidence/storage_test_on_5af4695_same_tests.txt`): PO6(a), PO7, PO8 variant 2, PO9, PO10, PO11, PO12 |
| on `0f2e76b` | **334 passed, 0 failed** (`evidence/storage_test_on_0f2e76b.txt`) |

**New (`pm_uncertain_tests.inc`):**

| Test | What it checks |
|---|---|
| PO7 | Six consecutive failed attempts, each with its listing cut. The request stays pending at every pass, and the record stays on flash and unread at every pass. After recovery, the message is read without another visit. |
| PO8 | Successful eviction cancels exactly its request, and another request in the same conversation stays and is applied. A failed removal does not cancel. An eviction with an unreadable header still cancels. |
| PO9 | An unreported disappearance: one slot kept for 300 s, with 4–12 attempts. The same conversation's other message, another conversation's reads and a recount all progress. Each background pass makes at most 1 rewrite, 8 opens and 300 entries listed. |
| PO10 | A failed rename loses the record: the request is kept and the count is recounted. |
| PO11 | The DOT read ends exactly its request; the other stays and is applied. |
| PO12 | A deleted group's request ends without a rewrite. |

**Changed:** PO6(a). An unreported deletion is now expected to stay pending; its name says "round 7; was: dropped after two absent listings". PO6(b)–(f) are unchanged and pass. Round-6 tests (full list, allocation failure, refusal message, Back with every queue full, per-message read, date headings) pass unchanged.

**Test infrastructure:** the Settings stub gets `T::groupConfigured`.

**Other results:**
- `notify_test`: 119 passed.
- Full `run_all.sh` suites: `evidence/run_all_suites_0f2e76b.txt`.
- **Mutations:**
  - `mutate_notify.py`: 191 mutants, 8 new; the 4 obsolete absence-confirmation mutants and their equivalent entry are removed, and 2 anchors updated (`evidence/mutate_notify.txt`).
  - `mutate_room.py`: `evidence/mutate_room.txt`.

## 4. Performance

- Nothing returns to the cursor enqueue path: `readMovedOnto()` still neither writes nor lists.
- **The uncertain-request retry path (PO9):** about once a minute, one failed open, no listing. Over 300 s: 8 attempts; per background pass at most 4 opens and 0 writes.
- No existing performance assertion was loosened.
- **Limit still visible:** replaying 40 held moves in one pass makes about 44 file opens in that UI pass (PO1). Input handling is not uniformly latency-bounded.

## 5. ESP32 (`evidence/esp32/`)

- Built from a `git archive` of `0f2e76b`: 87 files compared with the commit, 0 differ. `pio run`, espressif32@6.13.0. Not flashed.
- RAM 124,508 B; flash 1,208,953 B (83.9%); 0 warnings.
- **Static DRAM headroom: exactly 64 B** (used 124,516 of 124,580 B), unchanged.
- **Heap:**
  - The pending-read list is 2,820 B (32 × 88 B), only while reads are pending.
  - The 1,200 B transient listing for absence checks is removed.
  - The Job is 1,268 B.
- `firmware.bin` SHA-256 `178f584474ea6a93271c6f4aea30d8d791530bbfd7c291c77f1589107f17b6b3`

## Limitations

- Device timing is not verified.
- A record that vanishes without a removal the store can report keeps its request (one slot) until reboot.
- Reads refused when the list is full or memory is short are not retried; the message stays visibly unread.
- Accepted reads are RAM only (the power-loss window above).
- Counts may be a category flag (UNRESOLVED) until the recount after an ambiguous rewrite.
- If removing a deleted group's files fails, those records stay on flash, but the group is gone. Their pending reads end at the next attempt, and the messages stay as they were.
