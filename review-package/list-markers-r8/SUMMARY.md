# Unread markers on the Text and Enigma friend and group lists (round 8, `wip/chat-latency-r2`)

| | Git ID |
|---|---|
| Pre-change source (on devices A and B) | `0f2e76ba34b6f259a756811f438a76ea4415c275` |
| This candidate (source and tests) | `5237d869ec1d47e8cfdd43479adb795abe7d69c1` |
| ESP32 build source: `src` identical to `5237d86` (only a test stub changed after it) | `277daa2b5d646b31fcd8a43161c74f25758fba46` |

**Preflight:**
- HEAD was `92bc2fc`, equal to origin, with a clean tree.
- `0f2e76b` is an ancestor; only round-7 package commits lay above it.
- `src` and `host_tests` were identical to `0f2e76b`.

**Commits on top of it:**
- `90843f8`: the feature and its tests.
- `277daa2`: a field reorder in `ListMenu`. The first ESP32 link overflowed DRAM by 48 B; see 5.
- `5237d86`: the test stub gains `hasUnreadIn()`. The first full suite run failed to link the Friend suite without it.

**Diffs:**
- `production_since_0f2e76b.diff`: 7 files, 1 new (`core/list_badges.h`).
- `host_tests_since_0f2e76b.diff`: including the new `list_marker_tests.inc`.
- `test_sources/`: every test, fake and script, as at `5237d86`.

**Not done:** nothing flashed, erased, uploaded or merged. No OTA or release change. `test/hw-functional-ui-fix4` was not touched. The devices are unchanged. No change to storage format, migration, retention, read queues, date headings, Friend game, Race, Outbox or networking.

## 1. What the rows are (inspected first)

| List (Text and Enigma alike) | Row | Marker scope |
|---|---|---|
| Group select (only with 2+ configured groups) | a configured group | Any conversation of that group: its Everyone and every 1:1. A group is not its Everyone conversation. |
| Recipient | a conversation in the selected group: online contacts, recent offline contacts, then Everyone | That one conversation (group code, contact key) |

- The Enigma contact menu (Chat / Sender Key Settings) is an action menu, not a friend or group row. It is unchanged.

## 2. Behaviour

- **The star:** a `*` marks a row while that scope may hold unread received messages of the list's category.
  - Text lists: TEXT or GAME records.
  - Enigma lists: ENIGMA or GAME records.
  - This is exactly the Main Menu badge rule. A Friend game record appears in both chats' history, so it counts in both, as it already did for the top-level markers. An Enigma message alone never lights a Text row, and vice versa (LM1, LM4).
- **The source:** `Notifications::hasUnreadIn(group, contact or nullptr, category)`. It is a new, narrow, read-only query over the same in-memory summaries the Main Menu badges use. It does no file, directory or NVS access (LM7).
  - The top-level, list and message markers therefore always describe the same state.
  - UNRESOLVED conversations, with unknown counts, answer by their category flags: conservatively lit, never treated as zero (LM5).
- **Clearing:** the star stays while any relevant message is unread (LM3: after reading 1 of 2). It clears when the summary count reaches 0, which happens only after the last read's rewrite has succeeded. While that write fails, the star stays on the shown list. Once the write succeeds, it clears there without re-entry (LM3).
- **Reading:** entering a list, rendering, highlighting rows, and opening a conversation read nothing (LM1, with flash checked). Only the existing cursor-moved-onto-message rule reads.
- **Refresh:** ListMenu polls the badges of the visible rows every pass, from RAM. An arrival or a completed read redraws just that row's badge cell on the list already shown (LM3, LM4, LM8).
- **Isolation:**
  - One friend's messages never mark another (LM1).
  - The same contact key in two groups stays apart (LM2: Bob unread in Work only).
  - Everyone is per group (LM2).
  - Sent-only and read-only conversations have no star (LM1).
- **Layout:**
  - These lists use an opt-in `RESERVED_COLUMN` badge layout. The star sits in a fixed cell at the right edge, measured with `fitWidth()`, the display's clipping measure. The label is cut, by whole glyphs, before that cell.
  - A 16-character name of wide glyphs (presence keeps 16 characters) cannot hide or overlap it, nor can the selection marker (LM6).
  - Order, names, selection and input are unchanged.
  - Other menus keep the default layout.

## 3. ListMenu changes (shared widget)

- **Badges are tracked per visible row**, at most `kBadgeCacheCap`. Before, a badged list of more than 8 items repainted its whole row region on every tick. Recipient lists hold up to 41. Now an idle 12-row list draws nothing in 30 ticks, and a star change draws one badge cell (LM8). Lists of 8 or fewer behave as before.
- **The opt-in `BadgeLayout::RESERVED_COLUMN`** replaces the default "badge after the label" for these lists only.
- **Per-row badge functions:** `BadgeFn` takes no argument, so each row index has a tiny template instance. The tables are `const` arrays, in `.flash.rodata` in the ESP32 image (`kRecipientRowBadges` 164 B, `kGroupRowBadges` 20 B, per chat). There is no RAM cost.

## 4. Tests

| `storage_test`, the same final tests | Result |
|---|---|
| on `0f2e76b` | 337 passed, **17 failed**: every LM check of a star (`evidence/storage_test_on_0f2e76b_same_tests.txt`). LM7 (no storage work) passes on both. |
| on `5237d86` | **354 passed, 0 failed** (`evidence/storage_test_on_5237d86.txt`) |

**New tests** (`list_marker_tests.inc`; Text and Enigma; real Menu, ListMenu, chats, Notifications, Presence and store):

| Test | What it checks |
|---|---|
| LM1 | Recipient rows per friend, Everyone and category. Sent-only and read-only rows dark. Entering, highlighting and opening read nothing. Main Menu badges agree. |
| LM2 | Group rows aggregate their own group, Everyone included. The same contact in two groups stays apart. Reading Fam's Everyone clears Fam's star only. |
| LM3 | The star stays after reading 1 of 2, and while the last write fails. It clears on the shown list once that write succeeds. |
| LM4 | An arrival lights its friend's star on the shown list; an arrival of the other category does not. |
| LM5 | UNRESOLVED is conservative, per category. |
| LM6 | A 16-wide-glyph name and 8 friends scrolled through: stars exact on every visible row at every step, each in its own column, with the label ending before it (real glyph metrics). |
| LM7 | Showing, scrolling and refreshing the lists does 0 file opens, 0 directory entries and 0 NVS writes in UI passes. |
| LM8 | Idle long list: 0 draws. A star change: 1–2 draw items. |

**Test infrastructure:** the Settings stub gets an opt-in second group (`T::groupCount`).

**Other results:**
- `notify_test`: **119 passed** (`evidence/notify_test_on_5237d86.txt`).
- Full `run_all.sh` suites: **all passed** (`evidence/run_all_suites_5237d86.txt`).
- The first full run, on `277daa2`, failed one step: the Friend suite did not link, because the stub lacked `hasUnreadIn()`. Fixed in `5237d86`.
- **Mutations:**
  - `mutate_notify.py`: 206 mutants, 15 new. Final run on `5237d86`: **204/206 caught, 0 unexplained, 0 build errors**; the 2 survivors are the equivalents assessed in earlier rounds (`evidence/mutate_notify.txt`).
  - Targeted runs, before the full run, caught all 15 plus 2 re-anchored older mutants (the badge rule moved into `summaryShows()`).
  - Four survivors on the way were test gaps, now closed:
    - the Enigma group list was untested (LM2 now runs for both chats);
    - LM6's long name was never scrolled into view, and 16 ordinary characters still fit, so it now uses wide glyphs and scrolls the whole list.
  - `mutate_room.py`: **104/107**, with its 3 existing equivalents (`evidence/mutate_room.txt`). The shared stub and ListMenu changed, so it was re-run.

## 5. ESP32 (`evidence/esp32/`)

- Built from a `git archive` of `277daa2`: 88 files compared with the commit, 0 differ. Its `src`, `platformio.ini` and `partitions.csv` equal `5237d86`. `pio run`, espressif32@6.13.0. Not flashed.
- **The first build, of `90843f8`, did not link:** `dram0_0_seg` overflowed by 48 B. The new `badgeLayout_` byte had grown every global `ListMenu` from 24 to 28 B through alignment. `277daa2` reorders the fields, and `sizeof(ListMenu)` is 24 again with the ESP32 compiler.
- RAM 124,508 B; flash 1,216,105 B (84.3%, +7,152 B over `0f2e76b`); 0 warnings.
- **Static DRAM headroom: exactly 64 B** (used 124,516 of 124,580 B), unchanged.
- **Heap:** no new allocation.
- `firmware.bin` SHA-256 `ec30d2ebf2b7ae37940ffaa4027ccccb44055df39e8bd4511a6c3f86e82e7a87`

## 6. Hardware checklist (for the owner; nothing installed yet)

1. With unread Text from friend X only: Text > (group) shows `*` on X, none on others, none on Everyone. Enigma > (group) shows no `*` on X.
2. With unread Enigma in Everyone: Enigma list shows `*` on Everyone. Text list does not.
3. With 2 groups, and unread only in group G: the group list shows `*` on G only. Inside G, `*` on the right friend.
4. Open the list and scroll over a starred friend without opening it. Back out, re-enter: the star is still there.
5. Open X's chat without moving the cursor, and back out: still starred. Move onto 1 of 2 unread messages and back out: still starred. Read the last one and back out: the star clears within about a second on the list.
6. Stay on the list while a new message arrives from Y: Y's row gains `*` without leaving the list.
7. Long or duplicate names, for example 16 wide characters, or two contacts with the same name: the star stays visible at the right edge, also on the selected row, while scrolling.
8. The Main Menu Text and Enigma markers are lit exactly when some row in that mode is starred.

Report what is seen. These host results do not show device timing or rendering on the real panel.

## Limitations and observations (not fixed: out of scope)

- **The recipient list shows only online and recent contacts, plus Everyone** (existing behaviour). A conversation with unread messages from a contact not in those lists lights its group row and the Main Menu, but has no row of its own to star.
- **Presence device ids are 12 characters.** A conversation keyed by a longer id would not match its row. This is not seen in production data; noted from a test fixture.
- **Summaries hold up to 24 conversations** (existing). One beyond that is not counted, so neither its row nor the Main Menu is marked.
- **No hardware validation of this round yet.** The devices still run `0f2e76b`.
