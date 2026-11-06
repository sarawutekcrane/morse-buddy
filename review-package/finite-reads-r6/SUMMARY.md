# Finite per-message read list: no write on the cursor path (round 6, `wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Previous candidate (source, tests, build) | `1609f069c8881b14fdead46190692e40686620c6` |
| Its package-only commit | `5c2bebeabe4af087eaa5a84290d40ccdea864b32` |
| This candidate (source and tests) | `5af469509fa1d652ed23a06d14232ce090fa3958` |
| ESP32 build source: production identical to `5af4695` (`git diff 5139cf3 5af4695 -- src` is empty; only tests changed after it) | `5139cf318cf9379d48a273c0023de0215e0d1aa3` |
| Base of the cumulative diffs | `511d13e586432e22141be9691c1401e70eac4e41` |

**Preflight:**
- HEAD was `5c2bebe`, equal to origin, with a clean tree.
- `1609f06` is an ancestor; only the two package commits lay above it.
- `src` and `host_tests` were identical to `1609f06`.

**Diffs:**
- `production_since_1609f06.diff`: incremental, 4 files.
- `production_since_511d13e.diff`: cumulative.
- `host_tests_since_1609f06.diff` and `host_tests_since_511d13e.diff`: these include the new files.
- `test_sources/`: every test, fake and script source used, as at `5af4695`.

**Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. No persistent format. No change to Friend behaviour, the date format or storage migration.

## 1. The finite-capacity policy (`Notifications::readMovedOnto()`)

- **No storage work on the cursor path.** A move onto an unread message only asks to read it. `readMovedOnto()` never writes or lists storage, including when the list is full or cannot be allocated. The round-5 synchronous fallback rewrite is gone.
- **Accepted** (returns true) means one of:
  - the request was added to the RAM list of at most 32 entries, by exact identity: group, contact, sequence, message id;
  - the same message is already pending.
- **Refused** (returns false) when the list is full or cannot be allocated:
  - Nothing is recorded or consumed as a read.
  - The message stays unread and its `*` marker stays.
  - The chat shows "Read not saved (busy)" once that pass's input is handled, in the status row as for other outcomes.
  - The refusal is logged with its identity.
  - Moving onto the message again, once there is room, reads it.
  - The list fills either from a burst of moves, or from earlier requests waiting for storage to recover. Both are refused the same way.
  - This is an explicit failure path, not backpressure. The cursor and Back are never blocked.
- **Applying accepted requests:**
  - One per background step, before any other job, sharing the one-step-per-pass token with the Outbox. That is one rewrite, or one listing, per background pass.
  - The count follows only a successful rewrite.
  - A failed rewrite or read is retried with backoff (2 s doubling, at most 60 s) and moved behind the others, so unrelated work goes on (PO2: a recount completes while 32 reads keep failing).
  - Recovery needs no further cursor visit (PO2, PO4, PO5, PO6).
- **What is not promised:**
  - Not lossless under unlimited input, or under storage that never recovers. Refused reads leave messages visibly unread.
  - Accepted reads whose storage never recovers are retried about once a minute until power-off.
- **Power-loss window** (unchanged, RAM only):
  - It runs from the move to the rename of that record's rewrite.
  - It is longer while an index load pauses the background, while earlier reads are queued, or while a read is in backoff.
  - A power loss inside it leaves the message unread; it is never read wrongly.
- **Interactions checked:**
  - Leaving the chat, or moving in another conversation, does not affect accepted reads (PO5).
  - Eviction, trimming and group deletion drop their pending reads (round 5).
  - A sequence reused by another message is not read (PM10).
  - Allocation is retried on the next move (PO3).
  - Back with the list, the held buffer and the input queue all full leaves in that pass with at most 6 opens, reading nothing new (PO4).

## 2. Absence must be established (`recordConfirmedGone()`)

**Store contract, from `storage_messages.cpp`:**
- `listSequences()` sets `truncated` whenever a directory holds more records than the capacity (300). It then keeps the lowest sequences.
- `listMessageSequences()` sets `*outReliable = !openFailed && !truncated`.
- So a truncated listing was already never reliable, and a record outside the returned subset could not be taken as absent.
- PO6(e) shows this on `1609f06` and on this candidate: 301 records, the pending record outside the lowest 300, kept pending and read once readable.
- An explicit `!truncated` guard is added anyway. Its mutant is assessed equivalent for exactly this reason.

**A real gap that was fixed:**
- Arduino's `getNextFileName()` returns `""` both at the end of a directory and on a read error.
- A listing cut short by an error therefore looks complete and reliable.
- On `1609f06`, one such listing dropped a still-valid pending read (PO6(c) fails there).
- Now absence must be seen in two complete listings on two consecutive attempts, with backoff between them, and the record unreadable both times.
- A listing that shows the record resets this. PO6(c2) covers that: absent, present, absent is not a confirmation.
- On `1609f06`, a deleted record was dropped after one listing (PO6(a) fails there).

**Left pending:**
- an unreadable record (PO6(b));
- no memory for the listing (PO6(d));
- a directory that cannot be opened (PO6(f));
- a truncated listing (PO6(e)).

All are read once the condition clears, without another visit.

## 3. Tests: the same final tests on both candidates (`evidence/`)

| `storage_test` | Result |
|---|---|
| on `1609f06` | 294 passed, **17 failed** (`storage_test_on_1609f06_same_tests.txt`) |
| on `5af4695` | **311 passed, 0 failed** (`storage_test_on_5af4695.txt`) |

The 17 failures on `1609f06`:
- **PM9 and PO1:** 8 rewrites in the UI pass (writes 8, renames 8).
- **PO2:** the full list silently dropped 5 more selections, with no report.
- **PO3:** no memory meant the read was written at once on the cursor path.
- **PO6(a):** a deleted record dropped after one listing.
- **PO6(c) and (c2):** a pending read dropped by a listing cut short.

**New tests** (`pm_overflow_tests.inc`; Text and Enigma; real store, Notifications and chats):

| Test | What it checks |
|---|---|
| PO1 | 40 distinct unread selections replayed in one pass after a cold load (32 held, 8 queued). Right after the pass: 32 pending, 45 still unread, "busy" shown. The UI passes do no write, rename, remove, listing or NVS write. Then exactly bob[44..13] are read, bob[12..5] stay unread. Counts equal flash at every pass, and each background pass does at most 1 rewrite, 8 opens and 1 NVS write. |
| PO2 | Writes failing. 32 moves fill the list (no "busy" yet); 5 more are refused (reported, unread). For 60 s the list stays at 32, nothing is written, counts equal flash at every pass, and Carol's recount completes. After the chat is left and writes recover, exactly the 32 accepted reads are written. |
| PO3 | No memory for the list in the UI pass: refused, reported, no write, not retried. Once memory returns, moves onto the same message read it. |
| PO4 | Back with the read list (32), the held buffer (32) and the input queue (32) full, during a cold load in the device-cost model: it leaves in that pass. |
| PO5 | Moves in Bob, then Carol, with writes failing, then leaving. Exactly bob[4], bob[3] and carol[4] are read after recovery. |
| PO6(a)–(f), (c2) | Deletion versus unreadable records, cut listings (once; absent then present then absent), no memory, no directory and truncated listings, as in 2. |

- **Changed:** PM9 now expects refusal rather than synchronous rewrites (32 read, 13 unread, no UI write). Its name says "round 6; was: 8 rewritten synchronously".
- **Test infrastructure added:**
  - the fake filesystem's `endListing` (a `readdir` error reported as the end, as the Arduino API does);
  - a nothrow scalar `operator new` failure switch;
  - the shared stub follows the new `bool` result.

**Other results:**
- `notify_test`: 119 passed.
- Full `run_all.sh` suites: all passed (`evidence/run_all_suites_5af4695.txt`).
- **Mutations**, each script run alone:
  - `mutate_notify.py`: 187 mutants; 10 new in this round, and 1 removed (it targeted the deleted synchronous fallback). Final run on `5af4695`: **184/187 caught, 0 unexplained, 0 build errors** (`evidence/mutate_notify.txt`).
  - The first full run, on `5139cf3` (`evidence/mutate_notify_first_run_5139cf3.txt`), caught 181/187:
    - 1 unexplained survivor, "absence remembered across a listing that shows the record". This was a test gap, now closed by PO6(c2).
    - 2 build errors: the two "refused read is not reported" mutants did not compile. They are fixed and now caught by PO1.
  - Survivors assessed equivalent:
    - the explicit `!truncated` guard (see 2);
    - the two from earlier rounds.
  - `mutate_room.py`: 104/107, with its 3 existing "redundant guard" equivalents (`evidence/mutate_room.txt`). The race files are unchanged since round 4.

## 4. Per-pass measurements (`evidence/measurements.txt`; host counts, not device timing)

| Workload | UI pass, max | Background pass, max |
|---|---|---|
| Read 20 moves over the newest (14 unread) | 1 open, 0 writes | 1 open, 1 rewrite, 1 NVS write. Unchanged since round 5. |
| PO1: 40-move burst (refusals) | 44 opens, **0 writes, 0 listings**. Before: 53 opens, **8 writes**. | 4 opens, 1 rewrite |
| PO2: 60 s of failing writes, list full | at most 16 opens, 45 directory entries: the chat's own stepped reload after a failed rewrite (existing behaviour). 0 writes. | 4 opens, 0 writes, 5 directory entries, 1 NVS write |

- Cold entry, cached re-entry and scrolling are unchanged.
- **Limit (existing, not changed this round):** replaying many held moves in one pass measures each row it lands on. 40 moves cost about 44 opens in that pass, about 2.8 s in the 63 ms model.

## 5. ESP32 (`evidence/esp32/`)

- Built from a `git archive` of `5139cf3`, whose `src`, `platformio.ini` and `partitions.csv` are identical to `5af4695`. Verified file by file; `pio run`, espressif32@6.13.0. Not flashed.
- RAM 124,508 B; flash 1,208,933 B (83.8%); 0 warnings.
- **Static DRAM headroom: exactly 64 B** (used 124,516 of 124,580 B), unchanged. The two new per-chat flags fit in existing padding.
- **Heap:** unchanged.
  - The pending-read list is 2,820 B (32 × 88 B), only while reads are pending; `absentSeen` fits in existing padding.
  - The listing that confirms absence takes 1,200 B briefly, now only in the background.
  - The Job is 1,268 B.
- `firmware.bin` SHA-256 `21f90e9c41a0c574490cfdd5747d379d710eb9b05d00d629adccba8ef7e15e29`

## Limitations

- Device timing is not verified.
- Reads refused for a full list or no memory are not retried automatically. The message stays visibly unread and must be moved onto again.
- Accepted reads are RAM only (the power-loss window above).
- A directory error that ends listings early twice in a row, on two attempts, with the record also unreadable both times, still drops the read. The message then stays unread; it is never read wrongly.
