# Review fixes on `wip/chat-latency-r2`: Outbox read failures, held-input overflow

| | Git ID |
|---|---|
| Reviewed candidate | `2fde185a11c33cbc7a6cc25f33917dc08517b4ed` |
| This candidate (source, tests, build) | `cbc49609e36852f8c9ede89c167efb5cfec54b0f` |

- **Diffs:** `production_since_reviewed.diff` (8 source files) and `host_tests_since_reviewed.diff`, both from the reviewed candidate to this one.
- **Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. No new persistent format. Devices A and B are untouched.
- **Kept:** the retention, unread, history, score and empty-draft fixes; their tests pass unchanged.

## 1. Outbox: a queued record whose read fails at publication

**Before.** `publishStep()` dropped the queued reference whenever `loadMessage()` failed. After the boot pass, nothing would look for that record again, so a transient read failure left a pending message stranded on flash. OB8 shows it: 0 published.

**Now:**
- **Absence must be confirmed.** A failed read is checked against a names-only listing of the conversation. Only a reliable listing without the record counts as confirmed absence, and only then is the entry dropped (OB10: one read, no retries).
- **Inconclusive failures are retried.** That covers an unreadable record, a failed listing, or no memory for the listing. The entry stays queued with a per-entry backoff (1 s doubling, at most 60 s) and moves behind the others, so it never blocks them (OB9). No storage pass is started for it.
- **No longer pending means done.** A record that reads back as no longer pending is dropped, as before. The pending flag is still cleared only after a successful publish.
- **Bounded pass retries.**
  - An incomplete discovery pass is now always repeated after a growing pause (5 s doubling, at most 300 s).
  - Before, an incomplete first pass after boot was repeated at once, every step: OB7b counts 682 passes in 300 s on the reviewed candidate, and 1 now.
  - "Changed outside the store" now means changed since the last pass started.
- **Shared step, newly found.** The tests now reboot the Outbox too. That showed its boot pass and the Notifications sweep could each do a 300-entry listing in the same main-loop pass (ST7: 600 directory entries, on the reviewed candidate as well). Now at most one background storage step runs per pass (`claimBackgroundStorageStep()`, `hooks.cpp`), and the two take turns so neither starves.

**Regressions:**
- **OB8:** after the boot pass, a record written through the normal store path fails its read when published, then reads recover with no reboot, no new message and no outside change. It is published once and its flag cleared. A second variant also fails the conversation listing.
- **OB9:** a persistently unreadable record does not stop another from publishing, and is retried at most a few times a minute.
- **OB10:** a record confirmed gone is dropped after one read.
- **OB7b:** incomplete passes back off.
- **OB7:** failure during discovery, kept as before.

## 2. Held-input overflow and allocation failure

**Before.** When `hold()` failed, Text and Enigma called `completeIndexNow()`: the whole index in one pass. In the cost model that is 88–93 opens in one pass (about 5.5–6 s), with a queued Back waiting behind it.

**Now** (both chats):
- **No synchronous load.** Input that the held buffer cannot take (full, or not allocatable) stays in the input layer's own queue. The chat pops only what it can hold (`Input::peekEvent()`). The load goes on at its bounded pace (4 reads or about 20 ms per pass).
- **Back leaves at once.** A Back anywhere in the input queue leaves at once (`Input::hasQueuedEvent()`), also after an overflow.
- **When even the queue is full** (32 held + 32 queued, or 32 queued with no memory for the held buffer):
  - The queue is drained so that a Back pressed next still gets through.
  - The drained events are lost, as are those the full queue dropped (`Input::droppedEventCount()`).
  - The loss is shown: "Input lost while loading" on the loading screen, then in the status row once the chat is drawn. It clears like a Send outcome.
  - What was kept stays in order. **Input is not lossless beyond this capacity;** this is the visible failure behaviour.
- **Mid-batch and final reloads are bounded steps too.** An event met by a reload is put back where it came from (`HeldInput::unpop()`, or `Input::unpopEvent()`), so nothing is skipped and order is kept.
- **Send suppression is unchanged.** Every event, retained or live, goes through `isReplayedSend()` at dispatch.

**Regressions** (Text and Enigma, cost model at 63 ms per open; every pass at most 6 opens):

| Test | What it checks |
|---|---|
| IN2 | 40 key events over 4 passes while a cold chat loads: draft complete, nothing dropped |
| IN2b | Back queued after the overflowing event: left in that pass |
| IN2c | more input than the device can buffer: loss shown, kept input in order |
| IN2d | held buffer and input queue both full, then Back: left at once |
| IN3 | no memory for the held buffer: SOS kept |
| IN3b | no memory, then Back: left at once |
| IN4d | Send and its replayed Short after an overflow: one attempt |
| IN5b | PARISTEST keyed in real time while loading with overflow, and PARIS with no memory: same draft as live |
| IN5c | 48 events with no memory: loss shown, never a silently different draft |
| IN8 | a failed Send write makes the index stale mid-batch: the reload is stepped, and a key or rotation after it is kept |

IN2 and IN3 no longer accept a full synchronous completion as passing.

## Before / after (host tests; host model, not device timing)

- **`storage_test`:**
  - on `2fde185`: 140 passed, 32 failed (`evidence/storage_test_on_reviewed_2fde185.txt`; shim in `evidence/before_run_shim/`). The 32 are exactly the regressions above plus ST7.
  - on `cbc4960`: **172 passed, 0 failed** (`evidence/storage_test_on_cbc4960.txt`).
- **Full `run_all.sh` suites:** all passed (`evidence/run_all_suites_cbc4960.txt`).
- **Mutations**, run alone, with no other job touching their output directories:
  - `mutate_notify.py` (store, both chats, Notifications, Outbox; new mutants for every fixed path): **145/147 caught**, 0 unexplained. The 2 survivors were assessed equivalent before this round; the reasons are in the script (`evidence/mutate_notify.txt`).
  - An earlier run on `ccb542f` left 8 survivors and 1 broken mutant; each led to a test fix (IN2d, IN8 with a rotation, the Send-replay waits, rebooting the Outbox in tests), and the shared background step came out of that.
  - `mutate_room.py`: **104/107 caught**, 0 unexplained; the 3 survivors are its existing "redundant guard" equivalents (`evidence/mutate_room.txt`).

## ESP32 build (`evidence/esp32/`)

- Built from `cbc4960`, with the source verified file by file against the commit.
- RAM 124,516 B; flash 1,207,545 B (83.8%).
- **Static DRAM headroom 56 B:** the reviewed candidate had 96 B, 2ed04b6 had 232 B.
- `firmware.bin` SHA-256 `fe37774e3b26707573b60fe68b0c7bdf0e628ac00683e05fbe3e4f1ac256d721`
- **Added static RAM (+40 B):** per chat, a lost-input flag and a drop counter; the input layer's drop counter; the Outbox's pass-retry pause; the shared step token. An intermediate version kept a carried event per chat (40 B headroom); it was replaced by leaving that input in the input queue.
- **Added heap:**
  - Outbox queue entries now carry a retry time and a failure count: 16 × 64 B instead of 16 × 56 B, only while records are queued.
  - Checking a failed read for confirmed absence takes 1,200 B briefly.
  - Held input is unchanged (32 events, heap, only while used).

## Limitations

- Device timing is not verified; all figures are host operation counts in the cost model.
- Input beyond the capacity above is lost (and shown), not kept.
- A persistently unreadable queued record is retried at most about once a minute, indefinitely; it is never dropped while unconfirmed.
- The Friend chat still loads its index in one call.
- Morse tested with representative sequences (SOS, PARIS, PARISTEST, E/T), not exhaustively.
