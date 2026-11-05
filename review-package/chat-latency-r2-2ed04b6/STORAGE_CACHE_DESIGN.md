# Chat entry, viewport reads and the Outbox scan: storage/cache approaches compared

Based on device A's capture on flashed 2ed04b6 (70 broadcast messages):

| measured on device A | value | what it is |
|---|---|---|
| `CHAT_INDEX read` | 4446 ms for 70 records | one LittleFS open per record (~63 ms each) |
| `CHAT_INDEX list` | 158 ms | directory names only (~2 ms per entry) |
| `historyViewport` / `historyRender` | 415 / 447 ms | the same visible rows read twice |
| cached re-entry | 864 ms | viewport + render reads only |
| `OUTBOX scan` | 5143 ms, found=0 | every record of every conversation loaded in one call |

Each record was read in full several times per boot: by the Notifications recount, by the Outbox scan, and at every chat entry that missed the one-slot index cache. The cost is the number of opens, so the fix is to stop repeating them.

## A. RAM only, no new persistent format (implemented in this candidate)

| piece | removes | cost |
|---|---|---|
| Record cache: 10 slots (slot table and bytes on the heap), exact bytes, write-through on every store write, dropped on removal, a failed write or an external change | the render re-reading what the viewport read; re-entry and rotation reads | about 4.5 KB heap at most |
| Index cache survives mutations of other conversations; its own writes and removals are patched in place (`patchIndexWrite()`/`patchIndexRemove()`) | reloads after arrivals, evictions and read-ups | none |
| Parked second index | the reload on Everyone -> 1:1 chat -> Everyone | about 4 KB heap (with the feed's sequence list) |
| Boot sweep visits each group's Everyone first; its recount feeds the index (`beginIndexFeed()`), or the parked index while a chat is open (never the index an open chat draws from) | the chat re-reading what the recount reads anyway, also when a 1:1 chat was opened first after boot | none |
| Outbox: incremental pass of header-only reads, once per boot after connecting; sends queued directly (`setOnPendingWrittenCallback()`) | the 5.1 s blocking scan and every rescan | none |
| Time budgets per step (about one open per pass on device A); background readers pause while a chat loads | 250–480 ms boot passes; background competing with the user's chat | none |
| Open chats re-sync with the store each pass; the selection follows its message (by sequence) across patches and reloads | a patched or reloaded index moving the selection onto another message (which selecting would read) | none |

What stays:
- **Cold entry.** Opening a conversation that the sweep has not reached yet, in the first seconds after boot, still costs one open per record. The work is spread over passes and goes first.
- **Indexes after a reboot.** All indexes are lost and rebuilt, one read per record, by the boot sweep.
- **The Outbox's boot pass.** Once per boot after connecting it still reads one header per stored record (spread, at most 4 per step; on device A about 63 ms each). Options C and D below would remove it.

## B. A persistent derived index per conversation (NOT implemented; for approval)

### File

`/messages/<group>/<contact>/index.dat`. It is not a `*.msg` file, so listings, the conversation walk, trimming and the Outbox ignore it.

- **Header:** magic, format version, firmware build id, entry count, a `clean` flag and a CRC32 of the entries.
- **Entries:** sequence, sort timestamp, message id, flags, direction and type, 36 B each, so about 3.6 KB for 100 records.

### Rules (derived, never authoritative)

1. **Validation on use.**
   - The index is used only if:
     - the header is `clean`;
     - the CRC matches;
     - the firmware build id matches;
     - the sequence set equals a names-only listing of the directory (158 ms for 70 on device A).
   - Anything else means the index is rebuilt from the records: the stepped load as it is now, then the file written.
   - So every listed record is always in the index; nothing can be silently omitted.
2. **Interrupted updates.**
   - Before mutating any record of a conversation, the store marks its index not clean: it rewrites the header, or removes the file.
   - After the mutation, the store rewrites the file through `.tmp` and rename, marked clean.
   - A power loss at any point leaves the index missing or not clean, so it is rebuilt.
   - This also covers a reused sequence number (possible only after the highest record was deleted), which the set comparison alone cannot see.
3. **Batched writes.**
   - A read-up job marks once at its start and writes once at its end.
   - Trimming does the same per conversation.
   - An arrival costs one mark and one rewrite.
4. **Older firmware.** Older firmware ignores `index.dat` but may change records afterwards. The firmware build id in the header makes the next build rebuild after any downgrade and upgrade.
5. **Unread and retention semantics unchanged.** The index caches header fields only. Unread counts after a power loss are still verified against the records, unless a later, separately approved step lets the boot recount trust a `clean` index.

### Expected effect (device A figures)

- First entry after boot: listing 158 ms + one open and a 3.6 KB read (~70–100 ms) + visible rows (fewer with the record cache), about 0.3–0.7 s instead of about 4.4 s or more.
- The boot sweep would be unchanged unless step 5 above is approved.

### Costs and risks

- Two extra small writes per record mutation (flash wear, about 60 ms each on device A).
- A versioned format to maintain.
- Every mutation path must go through the mark/rewrite protocol (external changes such as group deletion remove the directory and the index with it).

## C. An Outbox pending journal (NOT implemented; for approval)

- **Format:** `/outbox/<n>` markers, written before a pending record and removed after publishing.
- **Saving:** the boot pass would read the marker directory instead of one header per record (on device A, about 100 header reads, about 6 s of spread background work per boot, once connected).
- **Costs:**
  - one marker write and one removal per offline send;
  - a one-time migration pass for pending records written by older firmware;
  - the same crash ordering as B: marker first, so the worst case is a stale marker that the pass checks against the record and drops.

## D. Sharing more reads (partly implemented)

- **Implemented:** the recount already feeds the chat index.
- **Not implemented:**
  - It could also hand the pending flags it sees to the Outbox, removing the Outbox's own boot pass with no new format.
  - That needs the Outbox to depend on the sweep completing (the sweep waits for the stored summary to load), plus its own fallback pass.

## Recommendation

1. Ship A and capture device A again. `[PERF][CHAT_INDEX]` now also prints `cacheHit=1`, `parkedHit=1`, or `fed=1 steps= wall=`. `[PERF][OUTBOX] pass` reports wall and busy time.
2. Decide on B only if a first entry before the sweep has reached the conversation is still a problem in practice. B is the only option that removes the one-read-per-record floor across a reboot, and it is a new persistent format that needs your approval.
3. C and D can follow if the Outbox's once-per-boot header pass shows up in a capture.
