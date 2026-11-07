# No recipient gives way to an unread sender (round 10, `wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Baseline: round-9 source | `0c73862cd08558cd98590f649367e2aa1ef86153` |
| Round-9 package (HEAD at start) | `e367b6313e46edd6480edfb8b2f9382580dff015` |
| This candidate: source, tests and ESP32 build | `2a5af03dcf0eeda7845c8a3e866ffd52252c09df` |
| Package commit | the commit that adds this directory, directly above `2a5af03` (`git log -1 -- review-package/recipient-rows-r10`) |

**Preflight:**
- HEAD was `e367b63`, equal to origin, with a clean tree.
- `src` and `host_tests` were identical to `0c73862`.

**Commits on top of it:**
- `805d1ee`: the regression first (LR9), with LR6's "one ordinary row dropped" expectation removed.
- `929adf1`: the fix, plus LR10.
- `2a5af03`: mutants. `src` is unchanged from `929adf1`.

**Diffs:**
- `production_since_0c73862.diff`: 6 files, all incremental against `0c73862`.
- `host_tests_since_0c73862.diff`.
- `test_sources/`: every test, fake and script, as at `2a5af03`.

**Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. No force-push. Devices unchanged. Notification rules, the 24-summary limit, read and marker rules, date headings, retention and the round-8 layout are unchanged.

## 1. The behaviour replaced, reproduced on `0c73862`

**LR9:**
- Setup: 40 distinct online and recent contacts plus Everyone.
- Steps: the cursor is on Everyone, then an unread message arrives from a contact X outside those 40.
- On round 9: the cursor stays on Everyone, but `dropReadRow()` removes the last original row before Everyone. That is **`D00000000019 (offline)`**: it can no longer be reached by navigation and cannot be opened. With 3 new senders, `D…0017`, `D…0018` and `D…0019` are all gone.
- Setup check: 20 online + 20 recent, with no contact in both. The recent 20 come from OFFLINE-only presence, which takes a recent slot without an online one. An ONLINE observation always takes a recent slot too, so 40 named contacts cannot be distinct.

**LR6** used to assert "ordinary 38 (one dropped)". It now requires every ordinary row.

## 2. The fix (`core/recipient_rows.h`, `text_message.cpp`, `enigma.cpp`)

**Room for every recipient that can exist at once:**
- 20 online + 20 recent + one row per tracked conversation (24) + one spare + Everyone = **66 rows**.
- ListMenu scrolls them in the existing round-8 layout and badge column. Its viewport and per-visible-row badge tracking are unchanged.
- No ordinary row is ever removed or reused. Order (online, recent, added unread, Everyone), selection by identity, group and mode isolation, and reading are as in round 9.

**Why it fits in RAM: one shared row store for both chats.**
- A chat's recipient list is only ever the top screen. Choosing a row pops the list and pushes the chat (Enigma: its contact menu), and every return to the list rebuilds it (`goBack()` sets the entry flag).
- So the Text and Enigma lists are never live together, and their rows can share storage.
- The shared store is 4,292 B. The two 41-row stores it replaces were 2,666 B each.

**One bounded exception, and why it cannot remove anything a user needs:**
- It can only be reached if, while the list stays open, new unread conversations keep appearing until all 66 rows are used. This needs summaries to be reused, which happens only after a read completes.
- In that case a new sender takes an **added** row whose conversation is no longer unread and that is not selected.
- Ordinary rows, Everyone, unread rows and the selected row are never taken.
- When all rows are used there are 25 added rows. At most 24 conversations are tracked and the new one is one of them, so at most 23 added rows are unread. That leaves at least two that are not, at most one of which is selected. The spare row is what makes this hold.
- LR10 drives this path with the real `clearUnread()`.

**Reads:**
- A newly read row loses its star after persistence and stays for the rest of the visit (LR5, LR10).
- If the read's write fails, the star stays (LR5).

**No storage work:** listing and navigating 41–44 rows made 0 file opens, 0 directory entries and 0 NVS reads or writes, in both modes (LR9, measured around both full walks).

## 3. Tests: the same final tests on both

| `storage_test` | Result |
|---|---|
| on `0c73862` | 391 passed, **24 failed**: LR9 12, LR6 8, LR10 4 (`evidence/storage_test_on_0c73862_same_tests.txt`) |
| on `2a5af03` | **415 passed, 0 failed** (`evidence/storage_test_on_2a5af03.txt`) |

| Test | Both chats |
|---|---|
| LR9 | The acceptance case, run 8 times: 1 or 3 new senders, and the opened row being either round 9's first dropped row or X. Each run checks: the cursor stays on Everyone; navigation reaches 41 + n distinct destinations (all 40 originals, every X, exactly one Everyone); the chosen row opens **its own** history (`isConversationOpen(group, id)`); and navigation does 0 file and 0 NVS operations. |
| LR10 | 40 ordinary rows, 24 senders (65 rows). X0's read completes on screen: its row stays, its star goes. X24 takes the spare (66). X1 is read and selected, then X25 arrives with all rows used: only X0 gives way. Every original, X1–X25 and Everyone stay; X1 stays selected; X25 opens its history. |
| LR6 | 39 ordinary + 2 unread at entry, then 2 more arrivals: every ordinary row stays (42, 43, 44 rows). |
| LR1–LR5, LR7, LR8, LM1–LM8 | Unchanged, passing. The list test helpers now walk up to 100 rows. |

**Other results:**
- Full `run_all.sh` suites: **all passed** (`evidence/run_all_suites_2a5af03.txt`), including the C++11 and C++17 checks.
- **Targeted mutants:** 26, re-anchored and new (`evidence/mutate_notify_targeted_round10.txt`). **24 caught, 2 assessed equivalent.**
  - Caught: rows full at round 9's 41 (fails LR6, LR9 and LR10, 24 checks); no spare row; reuse of an unread row, of Everyone, or of the selected row; nothing added when full.
  - Equivalent: "the unread rows wait for the next pass" (from round 9), and "a full list may reuse an ordinary row". The second is equivalent because the scan from the end always finds a reusable added row before reaching an ordinary one, by the count above.
- `mutate_notify.py` full run: **220/224, 0 unexplained, 0 build errors** (`evidence/mutate_notify.txt`). The 4 survivors are the 2 from earlier rounds and the 2 above.
- `mutate_room.py`: **104/107**, with its 3 existing equivalents (`evidence/mutate_room.txt`).

## 4. ESP32 (`evidence/esp32/`)

- Fresh `git archive` of `2a5af03`: 89 files compared, 0 differ. `pio run`, 0 warnings. Not flashed.

| | `0c73862` | `2a5af03` |
|---|---|---|
| RAM (PlatformIO) | 124,508 B | **123,468 B** (−1,040) |
| Flash | 1,216,765 B | **1,220,333 B** (+3,568: 25 more per-row badge instances and tables per chat, and code) |
| Static DRAM used / `dram0_0_seg` | 124,516 / 124,580 B | **123,476 / 124,580 B** |
| **Static DRAM headroom** | 64 B | **1,104 B** |

- **Heap:** no new allocation.
- **Stack:** unchanged from round 9. The presence arrays and about 100 B are used while building.
- The row store is zero-initialized `.bss`, with no constructor or guard.
- `firmware.bin` SHA-256 `f3888acaf7193a224f7862f71c67ac0ef761ff46598a9389cc869ab968ff99b2`

## 5. Hardware checklist (corrected)

**Correction to round 9's checklist:**
- Round 9 step 1 powered on A to send B a message. That puts A back in B's online and recent contacts, so the run tested an **ordinary** row, not the missing-contact case.
- With two devices, the missing-contact case cannot be produced without erasing data. B persists its recent contacts across reboots, and any message from A comes with A's ONLINE presence.
- The full-list case needs 40 other contacts.
- Both cases are covered by host tests only (LR1, LR9, LR10).

What two devices can check:
1. A online, B on Text > (group) with **Everyone** selected. A sends a Text. The cursor stays on Everyone, A's existing row gains `*`, and no second row for A appears. The Enigma list shows no `*` for A.
2. Turn the encoder through the whole list on B: every row appears once and the list wraps back to Everyone. Open A's row and confirm the chat is A's, without moving the cursor onto a message. Back out: still starred.
3. Read A's message, back out to the list: the star clears and A's row stays.
4. A sends one Friend game record and one Text. Both Morse and Enigma star A. Read only the game in Enigma: the Enigma star clears and the Morse star stays.

Report what is seen. These host results do not show device timing or panel rendering.

## Limitations

- At most 24 conversations are tracked (unchanged): one beyond that has no marker and no added row.
- A row is only reused in the pathological case of §2. Reaching it needs 25 new unread conversations during one visit. Only an added row that is no longer unread and not selected can be reused.
- No hardware validation of this round.
