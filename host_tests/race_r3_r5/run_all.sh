#!/bin/bash
# Race R3-R5 (room protocol, design revision 8) host-test bundle, with the
# R1/R2, history-browsing, Solo and Friend regressions, and the unread
# marking and badge suite (hardware finding 3).
# HOST ONLY -- not firmware, ESP32 timing/memory, flash/NVS hardware, broker
# or multi-device radio validation.
# Usage: ./run_all.sh from this directory (REPO defaults to the repository root).
# SKIP_MUTATIONS=1 skips step 7 (both mutation scripts); SOAK_SEEDS=n widens the randomized soak (default 20).
#
# Any compiler failure, test failure (non-zero exit) or unexplained mutation
# survivor fails this script: every compile and run is checked explicitly.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); REPO=${REPO:-$(cd "$H/../.." && pwd)}; export REPO
BASE=73884d9c56ab0387e3d52ff00bead8d813a4084d
CXX=${CXX:-g++}; R=$REPO/src/core
SAN="-g -fsanitize=address,undefined"
FCOMMON="$R/number_guessing.cpp $R/packet_codec.cpp $R/morse.cpp $R/enigma_crypto.cpp $R/enigma_keys.cpp $R/crc32.cpp $R/presence.cpp $R/text_message.cpp $R/enigma.cpp"
# Race UI suites: actual identity.cpp (its counter in Storage::core(), fault-injectable) instead of the Identity stub
# notify_test includes notifications.cpp and text_message.cpp itself
FNOTE="$R/number_guessing.cpp $R/packet_codec.cpp $R/morse.cpp $R/enigma_crypto.cpp $R/enigma_keys.cpp $R/crc32.cpp $R/presence.cpp $R/enigma.cpp"
UI="$H/stubs.cpp $H/race_stubs.cpp $R/identity.cpp $R/number_guessing.cpp $R/packet_codec.cpp $R/presence.cpp $R/race_protocol.cpp $R/crc32.cpp"
UIFLAGS="-DREAL_IDENTITY -I $H -I $H/fake_sys -I $H/fake -I $REPO/src"
OUT=$H/out; rm -rf "$OUT"; mkdir -p "$OUT/base/core"
CHANGED="race.cpp race_protocol.cpp identity.cpp storage_init.cpp mqtt_manager.cpp notifications.cpp text_message.cpp enigma.cpp"
FAILED=0
fail() { echo "!! FAILED: $*"; FAILED=1; }
for f in race.cpp identity.cpp identity.h storage_init.cpp storage_init.h mqtt_manager.cpp packet_codec.h notifications.cpp notifications.h text_message.cpp enigma.cpp; do
  git -C "$REPO" show $BASE:src/core/$f > "$OUT/base/core/$f"; done
echo "== toolchain: $($CXX --version | head -1)"
echo "== repo HEAD: $(git -C "$REPO" rev-parse HEAD)  (working tree: $(git -C "$REPO" status --porcelain | tr '\n' ' '))"

# build NAME SOURCES... : compiles (fails the script on a compiler error); warnings kept in NAME.warnings.txt
build() {
  local name=$1; shift
  if ! $CXX "$@" -o "$OUT/$name" 2> "$OUT/$name.warnings.txt"; then
    fail "compile $name"; sed 's/^/   /' "$OUT/$name.warnings.txt" | head -30; return 1
  fi
}
# run NAME : runs the test binary; its exit status decides (output in NAME.txt, summary lines shown)
run() {
  local name=$1 st=0
  "$OUT/$name" > "$OUT/$name.txt" 2>&1 || st=$?
  grep -E "FAIL|passed|audited|wire:|handed out|runtime error|ERROR" "$OUT/$name.txt" | sed 's/^/   /' || true  # display filter only
  [ $st -eq 0 ] || fail "$name exited with status $st"
}

echo "== 1. Room protocol E1-E27 + review findings F1-F3: ACTUAL race_protocol.cpp + crc32.cpp, several engines, broker + NVS + identity-counter fault models, ASan+UBSan"
build pt -std=gnu++17 $SAN -Wall -Wextra -I "$REPO/src" "$H/proto_test.cpp" "$R/race_protocol.cpp" "$R/crc32.cpp" && run pt

echo "== 1b. Race sequence numbers: ACTUAL identity.cpp near UINT32_MAX, interleaved with nextId(), exhaustion, reboots, write/read failures"
build idt -std=gnu++17 $SAN -Wall -Wextra -I "$H/fake_sys" -I "$H/fake" -I "$REPO/src" "$H/identity_test.cpp" "$R/identity.cpp" && run idt

echo "== 2. Room UI (R1, R2, R3-R5, review U15-U17): ACTUAL menu.cpp + race.cpp + race_protocol.cpp + identity.cpp; peers are ACTUAL engines"
build room -std=gnu++17 $SAN -Wall -Wextra $UIFLAGS "$H/room_test.cpp" $UI && run room

echo "== 3. Race Guess history browsing (RB1-RB12, ported to the room protocol): same sources"
build rt -std=gnu++17 $SAN -Wall -Wextra $UIFLAGS "$H/race_test.cpp" $UI && run rt

echo "== 4. Solo and Friend regressions (unchanged tests): ACTUAL menu, number_guessing(_friend), presence, text_message, enigma"
build st -std=gnu++17 $SAN -Wall -Wextra -I "$H" -I "$H/fake" -I "$REPO/src" "$H/solo_test.cpp" "$H/stubs.cpp" "$R/packet_codec.cpp" && run st
build ft -std=gnu++17 $SAN -Wall -Wextra -I "$H" -I "$H/fake" -I "$REPO/src" "$H/friend_test.cpp" "$H/stubs.cpp" $FCOMMON && run ft
echo "== 4b. Unread marking and badges (hardware finding 3): ACTUAL menu, notifications, text_message, enigma"
build nt -std=gnu++17 $SAN -Wall -Wextra -DREAL_NOTIFICATIONS -I "$H" -I "$H/fake" -I "$REPO/src" "$H/notify_test.cpp" "$H/stubs.cpp" $FNOTE && run nt
W=$(cat "$OUT"/*.warnings.txt | grep -E '(race|race_protocol|identity|number_guessing|number_guessing_friend)\.(cpp|h):[0-9]+:[0-9]+: warning' | sed 's|.*/src/core/||' | sort -u | tr '\n' ' ' || true)
echo "   warnings in changed/related files (all builds): ${W:-none}"

echo "== 5. C++11 / C++17 compile (-fsyntax-only -Wall -Wextra; host fakes incl. fake_sys/) of every changed production file, baseline vs candidate"
echo "   (a compiler error fails the script; warnings are counted and listed)"
for std in gnu++11 gnu++17; do
  for tree in base cand; do
    n=0
    for f in $CHANGED; do
      if [ $tree = base ]; then [ $f = race_protocol.cpp ] && continue; I="-I $OUT/base"; P="$OUT/base/core"; else I=""; P="$R"; fi
      log="$OUT/syntax_${std}_${tree}_${f}.txt"
      if ! $CXX -std=$std -fsyntax-only -Wall -Wextra -I "$H/fake_sys" -I "$H/fake" $I -I "$REPO/src" "$P/$f" > "$log" 2>&1; then
        fail "syntax $std $tree $f"; sed 's/^/   /' "$log" | head -10
      fi
      c=$(grep -cE '(race|race_protocol|identity|storage_init|mqtt_manager|notifications|text_message|enigma)\.(cpp|h):[0-9]+:[0-9]+: warning' "$log" || true)
      [ "$c" -gt 0 ] && sed 's/^/   /' "$log" | grep warning | head -5
      n=$((n + c))
    done
    echo "   $std $tree: compiled, $n warnings"
  done
done

echo "== 6. Sizes: encoded by the ACTUAL codec (step 1 E16, step 2 U14) and the RAM of the engine on this host"
build sizes -std=gnu++17 -I "$REPO/src" "$H/sizes.cpp" && { "$OUT/sizes" | sed 's/^/   /' || fail "sizes"; }
echo "   (x86-64 host layout: ESP32 is 32-bit, so pointer members are half the size there; not measured on target)"

if [ "${SKIP_MUTATIONS:-0}" = 1 ]; then
  echo "== 7. Targeted mutations: SKIPPED (SKIP_MUTATIONS=1)"
else
  echo "== 7. Targeted mutations of race.cpp, race_protocol.cpp and identity.cpp (each must be caught)"
  python3 "$H/mutate_room.py" || fail "mutations (unexplained survivor or build error)"
  echo "== 7b. Targeted mutations of notifications.cpp, text_message.cpp and enigma.cpp (each must be caught)"
  python3 "$H/mutate_notify.py" || fail "notification mutations (survivor or build error)"
fi

if [ $FAILED -ne 0 ]; then echo "== RESULT: FAILED"; exit 1; fi
echo "== RESULT: all steps passed"
