# Hardware findings on 30ed514: investigation reproductions (host only)

These are not part of `run_all.sh` and don't change firmware. Each one
records the behaviour of commit **30ed514** (before the defect fixes).

They are superseded as regression coverage by:
- `room_test` HB1-HB10 (guess history) and SC1-SC2 (score placement with real
  glyph bearings);
- `notify_test` NR1-NR10 (unread marking and badges).

Run against the fix candidate, `race_inv_test` H1/H2/H5 and
`unread_inv_test` N2 (which assert the old behaviour) are expected to fail.
Before/after results: `../evidence/defect_fixes_before_after.txt`.

| File | Defect | What it shows |
|---|---|---|
| `race_inv_test.cpp` | 1 (guess history), 2 (score: state side) | Includes `../room_test.cpp` (main renamed) for the ACTUAL race.cpp / menu.cpp / race_protocol.cpp / identity.cpp fixture |
| `score_clip_repro.cpp` | 2 (score: rendering) | The real FreeSans9pt7b glyph table, Adafruit GFX 1.12.6 `getTextBounds()`, and display.cpp's clip and race.cpp's right-alignment arithmetic |
| `unread_inv_test.cpp` | 3 (unread marker) | The ACTUAL notifications.cpp + text_message.cpp + menu.cpp, with the shared stubs minus their Notifications stub |

## Build and run (from `host_tests/race_r3_r5`)

```sh
H=$PWD; R=$H/../../src/core; O=/tmp/inv; mkdir -p $O
SAN="-g -fsanitize=address,undefined"

# Defects 1 and 2 (state side)
g++ -std=gnu++17 $SAN -DREAL_IDENTITY -I $H -I $H/fake_sys -I $H/fake -I $H/../../src \
  investigation/race_inv_test.cpp $H/stubs.cpp $H/race_stubs.cpp $R/identity.cpp $R/number_guessing.cpp \
  $R/packet_codec.cpp $R/presence.cpp $R/race_protocol.cpp $R/crc32.cpp -o $O/race_inv && $O/race_inv

# Defect 2 (rendering): FONTINC holds gfxfont.h, Fonts/FreeSans9pt7b.h (from the
# resolved Adafruit GFX Library) and a one-line Adafruit_GFX.h that includes gfxfont.h
g++ -std=gnu++17 -I "$FONTINC" investigation/score_clip_repro.cpp -o $O/score_clip && $O/score_clip   # exit 1 = defect present

# Defect 3: the shared stubs without their Notifications stub
python3 - $H/stubs.cpp $O/stubs_nonotify.cpp <<'PY'
import sys, re
s = open(sys.argv[1]).read()
s = re.sub(r"namespace Notifications \{\n.*?\n\}\n", "", s, count=1, flags=re.S)
open(sys.argv[2], "w").write(s)
PY
g++ -std=gnu++17 $SAN -I $H -I $H/fake -I $H/../../src investigation/unread_inv_test.cpp $O/stubs_nonotify.cpp \
  $R/number_guessing.cpp $R/packet_codec.cpp $R/morse.cpp $R/enigma_crypto.cpp $R/enigma_keys.cpp $R/crc32.cpp \
  $R/presence.cpp $R/enigma.cpp -o $O/unread_inv && $O/unread_inv
```

Recorded outputs: `outputs.txt`.
