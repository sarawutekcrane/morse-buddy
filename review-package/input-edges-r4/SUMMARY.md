# Input edge cases on `wip/chat-latency-r2`: Back into a full queue, loss on the completing pass

| | Git ID |
|---|---|
| Reviewed candidate | `cbc49609e36852f8c9ede89c167efb5cfec54b0f` |
| Refactor only: queue moved to `input_queue.h`, behaviour unchanged | `9eade43c5d8a1902c52b9d6e86ba3409a321fe3c` |
| This candidate (source, tests, build) | `511d13e586432e22141be9691c1401e70eac4e41` |

- **Diffs:** `production_since_reviewed.diff` (4 files) and `host_tests_since_reviewed.diff`.
- **Kept:** the previous round's Outbox retry fixes and bounded loading, unchanged.
- **Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. No persistent format. Devices A and B are untouched.

## The production queue under test

- The input event queue moved from `input.cpp` into `core/input_queue.h`, with no behaviour change (commit `9eade43`).
- `input.cpp` calls it exactly as before: `pushEvent()`, `popEvent()` and the rest are one-line forwards.
- When a test sets the device queue (`T::inputCap`), the host stub runs this same class. So the regressions exercise the production push and drop code, not a stand-in.

## 1. A Back arriving while the input queue is already full

- **Before.** `pushEvent()` dropped every event when full, including `ENCODER_LONG`. A loading chat whose held buffer and queue were full never saw the Back: in IN9 it was still open after 6 passes (Text and Enigma).
- **Now** (`InputQueue::push()`):
  - A Back reaching a full queue displaces the newest queued event that is not a Back.
  - The displaced event is counted in `droppedEventCount()`, so the loss is accounted for; in a loading chat it raises "Input lost while loading".
  - The order of everything kept is unchanged.
  - A queue made only of Backs drops the new one. The queue is not enlarged.
  - Every other event is dropped when full, as before, so ordinary ordering and navigation elsewhere are unchanged.
- **The drain is gone.** Because the queue now always admits a Back, the loading chats no longer drain a full queue. That drain existed only to let a Back in, and it discarded the *older* queued input to make room for newer. Now the oldest input is kept, in order, and the newest is dropped, counted and warned about (IN2c, IN2e).

**Tests:**
- **IN9u** checks the class: FIFO order, drop counting, which event a Back displaces, and a queue of Backs.
- **IN9** (Text and Enigma, cost model): the chat is loading with 32 events held. One input update brings 32 more (the queue is now full) and then a Back, before the chat can drain anything. The chat leaves in that same pass, and no pass exceeds the bounded load step (at most 6 opens), so there is no synchronous index completion.
- **IN2e** (Text and Enigma): the held buffer and the queue fill across two passes. Exactly the oldest 64 events are kept; 36 are dropped and warned about.

## 2. Input lost by the update of the pass in which loading completes

- **Before.** The pass ran the load step, then `Input::update()`, and checked the drop counter only if the chat was still loading. When the load finished in that pass, drops by that update were never reported (IN10: no warning, Text and Enigma).
- **Now** (both chats):
  - A pass that began while loading compares the drop counter right after its own `Input::update()`.
  - The warning is shown in the status row once the retained input has been replayed: "Input lost while loading".
  - Drops at any other time are only noted.
  - The counter is reset on entry, so a drop from a previous screen or visit is never attributed to the new one.
- **IN10** (Text and Enigma): 32 events are held, 31 queued, and exactly one record is left to load. On the next pass the load completes, then that pass's input update overflows the queue. The warning is shown and is not cleared at the loading-to-ready transition.
- **IN11 and IN11b** (Text and Enigma) guard against false warnings:
  - a drop on another screen before the visit (with SOS still drafted correctly);
  - a lossy visit followed by a clean one;
  - drops while the chat is already loaded.

**Unchanged:**
- Send replay suppression (`isReplayedSend()` at dispatch).
- Retained-event order.
- Bounded loading: there is no `completeIndexNow()` or other whole-index fallback.

## Before / after (host tests; host model at 63 ms per file open, not device timing)

| `storage_test` on | Result |
|---|---|
| `cbc4960` (reviewed), with only `input_queue.h` taken from `9eade43` | 177 passed, 7 failed: exactly IN9u, IN9 ×2, IN10 ×2 and IN2e ×2 (`evidence/storage_test_on_reviewed_cbc4960.txt`) |
| `9eade43` (refactor only, an earlier version of the tests) | the same IN9u/IN9/IN10 failures (`evidence/storage_test_on_9eade43_refactor_only.txt`) |
| `511d13e` | **184 passed, 0 failed** (`evidence/storage_test_on_511d13e.txt`) |

- **Full `run_all.sh` suites:** all passed (`evidence/run_all_suites_511d13e.txt`).
- **Mutations**, each script run alone, with no other job touching its output directories:
  - **`mutate_notify.py`: 153/155 caught**, 0 unexplained; the 2 survivors are those assessed equivalent in earlier rounds (`evidence/mutate_notify.txt`). The new mutants target `input_queue.h` and both chats; all are caught:
    - a Back dropped as before;
    - the displaced event not counted;
    - the oldest event displaced instead of the newest;
    - loss on the completing pass not reported;
    - drops outside loading reported;
    - an old drop attributed to the new visit;
    - the drain reintroduced.
  - **`mutate_room.py`: 104/107 caught**, 0 unexplained; its 3 existing "redundant guard" equivalents (`evidence/mutate_room.txt`).
  - An earlier full run on `1811aff` left 3 survivors: the drain and its separate drop check, made redundant by the queue fix. That is why they were removed.

## ESP32 build (`evidence/esp32/`)

- Built from `511d13e`, with the source verified file by file against the commit.
- RAM 124,516 B; flash 1,207,581 B (83.8%).
- **Static DRAM headroom: exactly 56 B, the same as the reviewed build.** The queue's static size is unchanged, and the new code uses only locals. No added heap.
- `firmware.bin` SHA-256 `8cb7fade436e4c92869f455c3f22e1fb471452cba5cda8cc0ed202a62ae53569`

## Limitations

- Device timing is not verified; all figures are host operation counts.
- Input beyond the finite capacity (32 held + 32 queued while a chat loads) is lost, with the warning. Input is not lossless.
- When a Back displaces an event in a full queue, that event is lost (counted).
- A drop caused by events produced just before a chat was entered, but converted in its first input update, would be reported for that visit. The tests cover a drained earlier screen, not this race.
