# A recipient row for every unread conversation (round 9, `wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Reviewed round-8 source and tests (base) | `5237d869ec1d47e8cfdd43479adb795abe7d69c1` |
| On devices A and B | `0f2e76ba34b6f259a756811f438a76ea4415c275` |
| This candidate: source, tests and ESP32 build (the same commit) | `0c73862cd08558cd98590f649367e2aa1ef86153` |
| Package commit | the commit that adds this directory, directly above `0c73862` (read it with `git log -1 -- review-package/recipient-rows-r9`) |

**Preflight:**
- HEAD was `0e6153d`, equal to origin, with a clean tree.
- `5237d86` is an ancestor. Only the round-8 package commits (`a53c340`, `0e6153d`) lay above it.
- `src` and `host_tests` were identical to `5237d86`.

**Commits on top of it:**
- `fd44b58`: the failing regressions LR1–LR8, and NVS read/write counters in the fake `Preferences`.
- `a1084ae`: the fix.
- `0c73862`: one insertion path, plus stronger tests and mutants.

**Diffs:**
- `production_since_5237d86.diff`: 5 files, 1 new (`core/recipient_rows.h`).
- `host_tests_since_5237d86.diff`.
- `test_sources/`: every test, fake and script, as at `0c73862`.

**Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. No force-push. `test/hw-functional-ui-fix4` was not touched. The devices are unchanged. Notification rules, storage format, retention, read queues, date headings and the round-8 layout are unchanged.

**Owner decision kept as is:**
- Text unread lights Morse only, and Enigma unread lights Enigma only.
- A Friend game record lights both, because the same record shows in both histories.
- Reading that record in either chat clears its share in both. Other unread records keep their own badges lit.
- No notification rule was changed. LR7 shows this rule on the recipient rows.

## 1. The problem, reproduced on round 8

On `5237d86`, a conversation with unread messages from a contact who is neither online nor recent lit its group row and the Main Menu. But the recipient list (online, recent, Everyone) had no row for it, so it could not be opened from the list.

**LR1** reproduces exactly this, in both chats: an unread conversation with a contact presence has never seen. On `5237d86` it has no row (`listed 0`).

## 2. The fix

**Rows** (Text and Enigma; shared code in `core/recipient_rows.h`):
- Online contacts, then recent offline contacts: unchanged, in the same order.
- Then each other conversation of the selected group that may hold unread messages of the list's category.
- Then exactly one Everyone row.
- Rows are keyed by contact id within the group, never by display name. A contact already listed is not added again.
- The name comes from Presence's RAM tables (`resolveDisplayName()`), with "(offline)" when not online. With no name known, the row shows the 12-character contact id.
- A name that is already shown gets the same `[last 4 of id]` suffix as before.
- Selecting the row sets the same `g_selectedContactKey` as any other row, so it opens that history (Enigma: through its contact menu).

**Source:** `Notifications::nextUnreadContactIn(group, category, cursor)`.
- It is a new read-only walk over the same in-memory summaries that the Main Menu, group and row stars use, with the same category rule (`summaryShows()`).
- UNRESOLVED summaries count by their category flags, so they are conservatively included.
- No file, directory, NVS or persistent index is used. Building, refreshing and drawing the rows made **0 file opens, 0 directory entries, 0 NVS reads and 0 NVS writes** (LR8). This was measured across entry, scrolling, an arrival's row being added, and redraws. The arrival's own store write is excluded.

**While the list is shown:**
- Each pass, a newly unread conversation without a row gains one, inserted just before Everyone.
- The selection keeps its identity, Everyone included (LR4).
- Rows are never removed because they were read. A row whose last read is saved loses its star but stays for the rest of that visit (LR5).
- If the read's write fails, the star stays.
- On the next entry, a row that is no longer online, recent or unread is omitted (LR5).

**Capacity (41 rows; arrays not enlarged):**
- At most 24 conversations are tracked, so the unread rows (24 at most) always fit beside Everyone.
- When online plus recent plus unread would exceed 41, each unread row added replaces the last row that is neither unread, selected nor Everyone. That is a read online or recent contact.
- So no unread conversation and no Everyone row can become unreachable. The trade-off: a read online or recent contact can drop off a full list. LR6 shows this with 20 online, 19 recent and 2 unread contacts outside them: 41 rows, 38 ordinary, one read row dropped. Arrivals on that full list are added too, and a selected replaceable row is kept.
- **Static DRAM:** no new static state. The row arrays are unchanged, and the table descriptor is built on the stack per call.

**Unchanged:**
- Displaying, highlighting or opening a recipient reads nothing (LR1: unread count is the same before and after opening).
- Per-message reading, date headings, retention, navigation, and the reserved star column.

## 3. Tests: the same final tests on both

| `storage_test` | Result |
|---|---|
| on `5237d86` | 355 passed, **22 failed**: every check of LR1–LR6 and LR8, both chats (`evidence/storage_test_on_5237d86_same_tests.txt`) |
| on `0c73862` | **377 passed, 0 failed** (`evidence/storage_test_on_0c73862.txt`) |

LR7 passes on both. It checks the rule kept by the owner's decision, which this round does not change.

**New tests** (`list_marker_tests.inc`; both chats; real Menu, ListMenu, chats, Notifications, Presence and store):

| Test | What it checks |
|---|---|
| LR1 | An absent contact's unread conversation has one starred row that opens its history and reads nothing. Another contact with unread messages of the other category only gets no row. An unread Everyone still has exactly one row. |
| LR2 | Two groups. The same id is read in Fam and unread in Work; two "Sam"s; a contact unread only in Work is listed in Work and not in Fam. Each opens its own history. |
| LR3 | An unread contact that becomes online or recent is listed once: on the same visit and on the next entry. |
| LR4 | An arrival from an absent sender adds its row on the open list, just before Everyone. The selection (a friend, or Everyone) stays on its row, and confirm opens that row. |
| LR5 | While the read's write fails, the row and star stay. After it succeeds, the star clears and the row stays. The next entry omits it. |
| LR6 | Full list (20 online + 19 recent + 2 unread outside them): 41 rows, both unread starred, Everyone present. An arrival with Everyone selected, and another with the replaceable row selected: added, and selection kept. |
| LR7 | One unread game and one unread Text from Bob: both modes star Bob. Reading only the game in Enigma clears the Enigma star; the Morse star stays (game 0, text 1 on flash). |
| LR8 | List UI storage counts: 0 opens, 0 directory entries, 0 NVS reads, 0 NVS writes. |

**Test infrastructure:**
- The fake `Preferences` counts reads (`getBytes`, `isKey`, `getULong`) and writes (`putBytes`, `remove`).
- The test stub gains `nextUnreadContactIn()`.

**Other results:**
- Full `run_all.sh` suites: **all passed** (`evidence/run_all_suites_0c73862.txt`), including `notify_test` 119 and the C++11 and C++17 syntax checks.
- **Targeted mutations:** 15 new mutants (`evidence/mutate_notify_targeted_round9.txt`). **14 caught; 1 assessed equivalent:** "entry: the unread rows wait for the next pass". The per-pass refresh adds the same rows about 20 ms later, before Everyone, keeping the selection.
- Three survivors on the way were acted on:
  - The duplicate entry-time loop was redundant, so it was removed and the entry now uses the refresh path.
  - A redundant Everyone check was removed.
  - A missing cross-group test was added (LR2).
- `mutate_notify.py`, full run: **218/221 caught, 0 unexplained, 0 build errors** (`evidence/mutate_notify.txt`). The 3 survivors are the 2 equivalents from earlier rounds and the 1 above.
- `mutate_room.py`: **104/107**, with its 3 existing equivalents (`evidence/mutate_room.txt`).

## 4. ESP32 (`evidence/esp32/`)

- Built from a `git archive` of `0c73862`: 89 files compared with the commit, 0 differ. `pio run`, espressif32@6.13.0, 0 warnings. Not flashed.
- **RAM 124,508 B**, unchanged.
- **Flash 1,216,765 B** (84.4%): +660 B over round 8.
- **Static DRAM headroom: exactly 64 B** (used 124,516 of 124,580 B), unchanged.
- **Heap:** no new allocation.
- **Stack:** while the list is built or refreshed, the table descriptor, two key buffers and a 41 B name buffer (about 100 B) sit on top of the existing presence arrays. Those arrays were already on the stack before.
- The shared functions exist once in the image (weak symbols), not once per chat.
- `firmware.bin` SHA-256 `cf2857c37911f35a3f7d7149b3f769b54091171feb4890f2965dbf707dc4021a`

## 5. Correction to the round-8 summary

Round 8 said the top-level, list and message markers "therefore always describe the same state". That holds **only within the tracked summaries: at most 24 conversations.**
- A conversation beyond those 24 is not counted. Neither its row nor the group row nor the Main Menu is marked, and since this round it gets no extra recipient row either.
- An UNRESOLVED summary is a category flag, not a count, until its recount.
- The 24-summary limit is unchanged and out of scope.

## 6. Two-device hardware checklist (A sends, B checks; nothing installed yet)

1. On B, make sure A is neither online nor in B's recent list: A powered off and B rebooted, or A never seen. Power on A only to send B a Text message, then switch A off. On B: Main Menu Morse lit. Text > (group) shows a `*` row for A (name, or a 12-character id) before Everyone. Enigma shows no row for A.
2. Open A's row on B without moving the cursor, then back out: still starred. Move onto the message and back out: the star clears within about a second, and A's row stays until you leave the list. Re-enter: A's row is gone if A is still offline and not recent.
3. With B's Text list open, Everyone selected, and A offline and absent: A sends one message. A's row appears before Everyone, and the cursor stays on Everyone.
4. A sends one Friend game invite or record, plus one Text message. On B both Morse and Enigma star A. In Enigma, read only the game record; Enigma's star clears and Morse's stays.
5. With two groups, where A is unread only in group 2: group 1's list has no row for A; group 2's does.

Report what is seen. These host results do not show device timing or rendering on the real panel.

## Limitations

- Only tracked summaries (24) produce rows, as with every marker.
- On a full list (41 rows), a read online or recent contact can be left out to make room for unread conversations. Nothing unread, and not Everyone, is left out.
- A contact known only by id shows the id until presence learns its name. Rows added during a visit are named when added and are not renamed during that visit.
- No hardware validation of this round yet.
