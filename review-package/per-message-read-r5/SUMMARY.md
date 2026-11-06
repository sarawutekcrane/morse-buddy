# Per-message read and date headings in the Text and Enigma chats (`wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Reviewed source | `511d13e586432e22141be9691c1401e70eac4e41` |
| Package-only commit above it (unchanged) | `836c53db0cbf6e8ee5ecb6e03b5f2798b1814e10` |
| This candidate (source, tests, ESP32 build) | `1609f069c8881b14fdead46190692e40686620c6` |

- **Diffs:** `production_since_511d13e.diff` (8 source files, one of them new: `core/history_dates.h`) and `host_tests_since_511d13e.diff`.
- **Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. No new persistent format (one NVS key is removed, see 3). Devices A and B are untouched. No Wi-Fi, MQTT, handover, partition or dependency change.
- **Kept:** bounded stepped loading ("Loading messages..."), the record and index caches, Back during loading, held input with its order and overflow rules, the Outbox, retention at 100, history, scores and the empty-draft Send fix. Their tests pass unchanged, apart from the expectation changes listed in 7.

## 1. Read semantics (supersedes "read up to the selection")

- **What reads a message.** A user move of the cursor onto a received message reads that message only. This covers a rotation, whether live or held while loading and replayed. Nothing older is read, nothing else on screen, and no other type, conversation or category.
- **What reads nothing:**
  - entering a chat;
  - drawing it;
  - the initial selection (compose, as before);
  - a selection that is restored or followed after a reload or deletion;
  - a load completing;
  - a cache or parked-index restore;
  - an arrival. A Text or Enigma arrival in the open chat now stays unread, counted and marked, with no tone. Before, it was saved as read.
- **Moving within one message's rows** is not a new move: a wrapped message is one message, with one marker and one read state.
- **Sent messages** have no marker and cause no write.
- **Identity.** A read is recorded by group, contact, sequence and message id. When it is applied, a record that now holds another message under the same sequence is not read (PM10).
- **Held moves.** Moves held during loading are replayed in order. Each one reads the message it actually lands on (PM3, PM8). Moves dropped by the full input queue read nothing. A Back with both queues full leaves at once and reads nothing (PM8).
- **Badges** stay lit until the last qualifying unread message is read (PM5). This also holds while the summary is UNRESOLVED, where its count is only a flag (PM5b).
- **Friend game:**
  - Its arrivals and its own screens are unchanged: an arrival in an open chat is still saved as read (`readIfOpen` defaults to true).
  - Game records shown in the Text or Enigma history follow the new per-message rule, as every history message does. Before, a selection read them by read-up, by type.
  - That is the one shared behaviour change: the read-up machinery is gone for those screens.

## 2. Persistence and failure behaviour

- **Kept in RAM only.** The pending reads are a heap list of up to 32 (`Notifications::readMovedOnto()`). Nothing goes to NVS, and no file is written on the encoder path while the list has room.
- **Applied in the background**, one record rewrite per background step, before any other job. It shares the one-step-per-pass token with the Outbox.
- **The count follows the flash.** It is decremented only after the rewrite clearing `FLAG_UNREAD` has succeeded. The marker and badge therefore always show what is on flash, and a failed write is never reported as a read (PM6, PM5b).
- **Exact power-loss window:** from the move until the rename of that record's rewrite completes. Normally that is the next background step, one or two main-loop passes later. The window is longer when:
  - an index load is pausing the background work;
  - a summary NVS write or the Outbox takes that pass;
  - earlier reads are queued ahead;
  - an earlier attempt failed and is in backoff.

  A power loss inside the window loses the read: the message stays unread on flash, is counted after the reboot and keeps its marker. It is never read wrongly. After the rename, the read survives a reboot. Both sides are tested in PM6 and ST20.
- **A failed rewrite** keeps the message unread and counted. The read is retried with backoff (2 s doubling, at most 60 s) and moved behind the others (PM6, ST11, ST19). Other background work goes on meanwhile (ST19).
- **A pending read is dropped only when:**
  - a reliable listing confirms the record is gone;
  - the record now holds another message;
  - the record is no longer unread.

  If an earlier attempt had failed, the conversation is recounted rather than trusted. This covers a rewrite that reported failure after its rename, and one whose failed rename removed the record (CE20).
- **A full list (32), or no memory for it.** The read is applied at once, as a synchronous rewrite on that pass (PM9: 40 moves in one batch, 32 queued and 8 applied at once). If that write fails, the message stays unread and is not retried. This is logged.
- **A recount already under way** is restarted when a read lands, so it never finishes with the message it counted before the read (ST12).
- **Deletions.** Evicted, trimmed and group-deleted records drop their pending reads.
- **The explicit DOT read** (`clearUnread`) also counts only after a successful rewrite (PM6).

## 3. The old `readup` record

- **Policy:**
  - The earlier firmware's NVS record ("read up to this selection", key `readup`) is never applied. Its ranges are not turned into per-message reads.
  - It is erased once at boot. If erasing fails, it is ignored again and erased at a later boot.
  - Messages it had already cleared on flash stay read.
  - Messages it had not reached keep their truthful unread flag.
  - The boot sweep recounts every conversation from the flags.
  - Nothing already read is made unread.
- **PM12:**
  - Setup: messages 0–4 were already cleared; the record holds one done read-up up to message 4 and one pending read-up up to message 7.
  - After boot: 0–4 stay read and 5–9 are unread. The count is 5, equal to flash, and the key is erased.
  - The variant whose erase fails keeps the key, applies nothing, and erases it on the next boot.
  - On 511d13e the same test fails: messages 5–7 are read.

## 4. Date headings

- **Where.** A heading row comes before the first shown message of each calendar day in Thailand time (UTC+7). It uses the message's stored `effective_sort_timestamp`.
- **Grouping.** Sent and received messages are grouped together, in the history's existing order. Text and Enigma each draw their own history.
- **Format:** `dd/mm/yyyy`. The display font (FreeSans9pt7b) is ASCII only, so no Thai glyphs, and no font was added. The heading is grey and centred, and fits the 240 px width.
- **Untrusted timestamps** get "Unknown date" instead of an invented date. That means 0 (clock not set), before 2020-01-01, or from 2100 on. Consecutive unknown dates share one heading (DS2).
- **Presentation only:**
  - Headings are not stored or counted, and are never selectable.
  - The cursor moves over message rows only (DS2, DS4).
  - The viewport, row positions and partial redraws count heading rows (DS4, DS5).
  - Scrolling up to a message's first row also shows its heading.
- **Correct under changes:**
  - arrivals and eviction at the 100 cap while the chat is open (DS3);
  - retention cutting into a day, where the first remaining message still gets its heading (DS2b);
  - stepped loads and cache restores;
  - midnight at UTC+7 (DS1: 23:59:59 and 00:00:00 split the day; the UTC midnight does not).
- **The date function** was checked against `gmtime` shifted by 7 h at about 117,000 points over 2020–2099: 0 mismatches (`evidence/date_check/`).
- **Unread marker.** It is a yellow `*` in a fixed 8 px cell after the cursor cell, on a message's first row. The cell is reserved on every history row, so reading never re-wraps a message. Text history rows are therefore 8 px narrower than before.
- **Redraws.** A read, arrival or removal that the index absorbs redraws the shown rows. A signature of order, days and markers is compared on each storage change; it costs RAM only.

## 5. Performance and resources

Host model only (63 ms per file open). This is not device timing. The tests and workloads are identical for both columns (`evidence/measurements.txt`); Text is shown, and Enigma is within a few opens.

| Everyone, 90 messages | 511d13e | 1609f06 |
|---|---|---|
| Cold entry | 94 opens; at most 5 per pass | the same |
| Cached re-entry | 2 opens | the same |
| Scroll: 40 single moves over read messages | 36 opens, 0 writes; at most 1 open per pass | the same |
| Read: 20 moves over the newest, then 10 s | 303 opens, 27 rewrites, 270 directory entries, 5 NVS writes. Every older unread message is read (27). | 30 opens, 14 rewrites (the 14 unread messages moved onto), 0 directory entries, 2 NVS writes |
| Per pass while reading | UI 1 open, 0 writes; service 3 opens, 1 rewrite, 1 NVS write | UI 1 open, 0 writes; service 1 open, 1 rewrite, 1 NVS write |

- **No scans or NVS writes per encoder move.** A move does one index lookup and one RAM enqueue. Summary NVS writes stay coalesced (2 s).
- **One limit:** a single pass with more than 32 moves onto unread messages rewrites the excess synchronously in that pass (PM9: 8 rewrites in one pass).

**ESP32** (`evidence/esp32/`; `pio run`, espressif32@6.13.0, built from a `git archive` of `1609f06` and verified file by file; not flashed):
- RAM 124,508 B, flash 1,208,581 B (83.8%), 0 warnings.
- **Static DRAM headroom: exactly 64 B** (used 124,516 of 124,580 B). It was 56 B on 511d13e: the read-up statics are gone, and the new state is 10 B (a pointer, and per chat a signature and a flag).
- `firmware.bin` SHA-256 `1cfc58f1fe0d463413b2f931b382970282d37d960dc584a0c7b048ea6e734408`
- **Heap:**
  - The pending reads take 2,820 B, only while one is pending, and are freed when empty. Before, the read-up record took 740 B and stayed allocated once anything had been read.
  - Confirming a record gone takes 1,200 B briefly.
  - The background Job shrinks from 1,364 B to 1,268 B.

## 6. Validation

- **`storage_test`** (real store on a host directory; real Notifications, Text, Enigma, Outbox):
  - `1609f06`: **274 passed, 0 failed**.
  - The same tests on `511d13e` (`-DNOTIFY_READUP`, a compile shim only): 196 passed, 78 failed, exactly the new and changed tests (`evidence/storage_test_on_511d13e_final_tests.txt`).
  - ST12 and ST20 pass on both: their outcome is the same under either semantics.
- **`notify_test`:** 119 passed.
- **Full `run_all.sh` suites** (mutation step run separately): all passed (`evidence/run_all_suites_1609f06.txt`).
- **Mutations**, each script run alone:
  - **`mutate_notify.py`** (178 mutants: 40 new for this round, 17 obsolete read-up mutants removed, one anchor updated for the new signature): see `evidence/mutate_notify.txt`. The first full run (`evidence/mutate_notify_first_run_4fe335b.txt`) left 7 unexplained survivors. Each was an observed test gap, now closed by a test:
    - an Enigma arrival in the open chat read: the arrival helper bypassed the chats' handlers (now PM7 and DS3 go through the dispatcher);
    - a read waiting behind long background work (PM11);
    - a read not restarting a recount under way (ST12: the count is exact whenever no recount of it runs);
    - a read decrementing an UNRESOLVED flag (PM5b);
    - the Enigma viewport ignoring headings (DS4);
    - the Text and Enigma cursor row ignoring its heading (DS5: the focused row becomes the top row inside a tall message).
  - **`mutate_room.py`:** 104/107 caught; its 3 survivors are the existing "redundant guard" equivalents (`evidence/mutate_room.txt`, run on `4fe335b`, whose production and race test files equal `1609f06`).

## 7. Test changes, reported honestly

**New:**
- PM1, PM1b, PM2–PM12, PM5b;
- DS1–DS5, DS2b;
- a printed measurement block (`host_tests/race_r3_r5/pm_tests.inc`).

**Expectations changed for the new semantics** (each test name says "round 5; was: ..."):

| Test | Now | Before |
|---|---|---|
| ST2 | selecting the newest reads 1; 47 stay unread | all 48 read |
| ST3 | the older Text message `a1` stays unread | it was read |
| ST3b | a message passed over in a two-move batch is read | it stayed unread |
| ST5 | reboot after a read: the 89 others stay unread | a read-up resumed |
| ST8 | 39 unread stay unread | 0 |
| ST11 | only that message read, after 5 failures | — |
| ST12 | the read message is listed first, 100 records, count checked exact | — |
| ST19 | a failing read stays pending, is lost at reboot, the message stays unread | a persisted read-up completed |
| ST20 | power lost before the rewrite | power lost before the read-up save |
| NR1 | the badge follows the rewrite | the badge followed at once |
| NR2, NR4 | per-message | read-up |
| NR6 | an arrival in the open chat stays unread | it was read |

**Fixture changes:**
- `freshDevice()` drops the store's caches after wiping the host directory (a wiped device keeps nothing); stale records were served across tests.
- `Preferences::remove()` added to the fake NVS.
- An opt-in word wrap (`T::realWrap`) in the fake display; the default stays one row, as before.
- The shared stub follows the new Notifications API.
- `legacy_store.h` prints which field differs.

## Limitations

- Device timing is not verified; all figures are host operation counts.
- Reads are not durable until their rewrite. A power loss in the window above loses them (the message stays unread).
- More than 32 moves onto unread messages in one pass: the rest are rewritten synchronously in that pass, and one that fails is not retried.
- Untrusted timestamps are judged only by range (before 2020 or from 2100). A wrong but plausible sender clock gets a plausible wrong date.
- The Friend game's own screens keep their old read rules. Game records in the Text and Enigma history now follow per-message read.
- History rows are 8 px narrower (the marker cell), so long messages may wrap one row earlier than before.
