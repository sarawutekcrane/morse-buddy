# Capturing device A's chat-entry timing on this candidate (Windows PowerShell)

This candidate is not flashed. Flashing it is your decision; nothing here erases
storage, uploads a filesystem or touches OTA. A's message history is used as it is.

The serial port (115200 baud) prints these lines. Each line appears only when its work took 20 ms or more,
except the `cacheHit` and `parkedHit` lines, which always print.

| line | meaning |
|---|---|
| `[PERF][CHAT_INDEX] cacheHit=1 indexed=N` | entry reused the shared index: no listing, no reads |
| `[PERF][CHAT_INDEX] parkedHit=1 indexed=N` | entry swapped in the parked index (the last chat, or one the boot sweep built while another chat was open): no listing, no reads |
| `[PERF][CHAT_INDEX] total= list= read= ... enumerated= indexed= steps= wall= [fed=1]` | an index load. `total` is busy time (listing plus reads); `wall` is the time from start to finish, spread over `steps` main-loop passes. `fed=1` means the boot sweep's recount supplied the records. |
| `[PERF][CHAT_INDEX] incremental=1` | an appended message patched into the index |
| `[PERF][CHAT_SCREEN] total= path= entry= indexReload= ... historyViewport= historyRender=` | one chat tick (one main-loop pass) |
| `[PERF][OUTBOX] pass wall= busy= records= found= complete=` | the Outbox's pass over stored records: header reads only, once per boot after a group connects. `busy` is the time it actually blocked, spread over `wall`. |
| `[PERF][OUTBOX] publish` | a pending message was published |
| `[PERF][MQTT] setup / connect / subscribe / publishOnline / loop` | startup and network work. This is not chat-entry cost. Report it in its own column. |

## Steps

1. Close any serial monitor that holds the port (PlatformIO, Arduino IDE).
2. In PowerShell, run `[System.IO.Ports.SerialPort]::GetPortNames()` and put device A's port in the first line below
   (COM5 is an example).
3. Paste and run:

```powershell
$port = New-Object System.IO.Ports.SerialPort 'COM5',115200,'None',8,'One'
$port.DtrEnable = $false; $port.RtsEnable = $false   # opening the port does not reset the board
$port.ReadTimeout = 500; $port.Open()
$log = "$env:USERPROFILE\Desktop\deviceA_chat_r2.txt"
Write-Host "Logging to $log. Ctrl+C to stop."
try { while ($true) { try { $l = $port.ReadLine(); "$((Get-Date).ToString('HH:mm:ss.fff')) $l" | Tee-Object -FilePath $log -Append } catch [TimeoutException] {} } } finally { $port.Close() }
```

4. Do two separate windows, and note their wall-clock times:
   - **A. Settled.**
     1. Power on and wait about one minute, until no `[PERF][OUTBOX] pass` or `[PERF][MQTT]` lines are coming.
     2. Text > (group) > Everyone. Wait until the chat is drawn, then long-press Back.
     3. Open Everyone again, then open a 1:1 chat, then Everyone once more.
   - **B. Straight after boot.**
     1. Power on.
     2. As soon as the Main Menu shows, open Text > (group) > Everyone.
     3. While it shows "Loading messages...", rotate a few times, then long-press Back. Back should leave at once.
     4. Open Everyone again and wait until it is drawn.
5. Press Ctrl+C, then extract the lines:

```powershell
Select-String -Path "$env:USERPROFILE\Desktop\deviceA_chat_r2.txt" -Pattern 'PERF\]\[(CHAT_|OUTBOX|MQTT)' | ForEach-Object { $_.Line }
```

## Reading the lines

- Chat entry is the `CHAT_INDEX` and `CHAT_SCREEN` lines between your key presses. `MQTT` and `OUTBOX` lines in the
  same seconds are startup work running alongside it. They are not part of entry cost, and a chat that is loading
  pauses the Outbox pass and the Notifications sweep.
- The log shows only what the firmware printed. A missing line means the work took under 20 ms, not that it
  never ran. A Wi-Fi disconnect would need its own evidence; these lines do not show one.
