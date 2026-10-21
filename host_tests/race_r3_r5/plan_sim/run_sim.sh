#!/usr/bin/env bash
# SIMULATION of the plan r7.3 scripts (test only). Simulated esptool/pio; not hardware.
T=$(cd "$(dirname "$0")" && pwd); cd "$T"
export PATH="$T/bin:$PATH" SIM="$T" SIMFW="$T/fw" REPO="$T/repo" OUT="$T/out"
export COMMIT=$(git -C "$REPO" rev-parse simcand)
rm -rf "$OUT" "$T/out-tmp" calls.log; cp dev/pA.img pA.orig; cp dev/pB.img pB.orig
N=0; BAD=0
writes() { grep -c "write_flash" calls.log 2>/dev/null || true; }
runs() { grep -c "^run" calls.log 2>/dev/null || true; }
run() { local name=$1 expect=$2; shift 2; local st=0 w0 r0; w0=$(writes); r0=$(runs)
  out=$( "$@" 2>&1 ) || st=$?
  local got=$([ $st -eq 0 ] && echo PASS || echo STOP); local ok=OK
  [ $got = $expect ] || { ok=UNEXPECTED; BAD=$((BAD+1)); }; N=$((N+1))
  printf '%-66s exit=%-3s %s %s  [write_flash calls +%s, pio run calls +%s]\n' "$name" "$st" "$got" "$ok" $(( $(writes)-w0 )) $(( $(runs)-r0 ))
  [ $got = STOP ] && printf '      last line: %s\n' "$(echo "$out" | grep -v '^$' | tail -1 | cut -c1-120)"; true; }
check() { local what=$1; shift; if "$@"; then echo "      check: $what: yes"; else echo "      check: $what: NO"; BAD=$((BAD+1)); fi; }
same() { cmp -s "$1" "$2"; }
region_eq() { cmp -s <(dd if="$1" bs=4096 skip=$(($3/4096)) count=$(($4/4096)) status=none) <(dd if="$2" bs=4096 skip=$(($3/4096)) count=$(($4/4096)) status=none); }
img_ok() { [ "$(writes)" = "$1" ]; }

echo "== 0.3 build and stage"
OUT="$T/out-tmp" SIM_BUILD_FAIL=1 run "build failure piped through tee" STOP bash build.sh; rm -rf "$T/out-tmp"
(cd "$REPO" && echo x >> src/core/race.cpp); OUT="$T/out-tmp" run "dirty working tree" STOP bash build.sh; git -C "$REPO" checkout -q -- src/core/race.cpp; rm -rf "$T/out-tmp"
OUT="$T/out-tmp" COMMIT=fb126cce6230c0a43116cf0c537516c0d4ca7edf run "wrong commit (race.cpp hash)" STOP bash build.sh; rm -rf "$T/out-tmp"
run "build and stage images" PASS bash build.sh
check "manifest lists the four images incl. boot_app0.bin" grep -q " boot_app0.bin$" "$OUT/images/manifest.sha256"
check "staged images are read-only (mode bits)" bash -c '[ "$(stat -c %A "$OUT/images/firmware.bin" | cut -c3,6,9)" = "---" ] && [ "$(stat -c %A "$OUT/images/manifest.sha256" | cut -c3,6,9)" = "---" ]'
check "clone still clean after build" bash -c '[ -z "$(git -C "$REPO" status --porcelain)" ]'
run "rerun with images already staged" STOP bash build.sh

echo "== 0.4 backup"
DEV=A PORT=/dev/pOld run "old partition layout" STOP bash backup.sh
check "rejected backup has no .meta (unusable for install/rollback)" bash -c '! ls "$OUT"/backup-A-240ac400000c-*.bin.meta >/dev/null 2>&1'
DEV=A PORT=/dev/nope run "unreachable port" STOP bash backup.sh
DEV=A PORT=/dev/pA run "backup device A" PASS bash backup.sh; BKA=$(ls "$OUT"/backup-A-240ac400000a-*.bin)
DEV=B PORT=/dev/pB run "backup device B" PASS bash backup.sh; BKB=$(ls "$OUT"/backup-B-240ac400000b-*.bin)
check "backup A equals device A" same "$BKA" pA.orig

echo "== 0.5 install: every refusal must happen before any write (write_flash +0), and no pio run"
export SIM_NO_RUN=1
DEV=A PORT=/dev/pB BK="$BKA" run "A's backup, device B on the port" STOP bash install.sh
DEV=B PORT=/dev/pB BK="$BKA" run "DEV=B with A's backup" STOP bash install.sh
cp "$BKA" bk.save; truncate -s 4000000 "$BKA"; DEV=A PORT=/dev/pA BK="$BKA" run "truncated backup" STOP bash install.sh; cp bk.save "$BKA"
for f in firmware.bin bootloader.bin partitions.bin boot_app0.bin; do
  cp "$OUT/images/$f" img.save; chmod u+w "$OUT/images/$f"; printf 'Z' >> "$OUT/images/$f"
  DEV=A PORT=/dev/pA BK="$BKA" run "altered $f" STOP bash install.sh
  chmod u+w "$OUT/images/$f"; cp img.save "$OUT/images/$f"; chmod a-w "$OUT/images/$f"; done
chmod u+w "$OUT/images/manifest.sha256"; cp "$OUT/images/manifest.sha256" man.save
grep -v boot_app0 man.save > "$OUT/images/manifest.sha256"; DEV=A PORT=/dev/pA BK="$BKA" run "manifest missing boot_app0.bin" STOP bash install.sh
# an altered firmware with a manifest regenerated to match: caught by the size gate
chmod u+w "$OUT/images/firmware.bin"; cp "$OUT/images/firmware.bin" fw.save; head -c $((0x160001)) /dev/zero > "$OUT/images/firmware.bin"
(cd "$OUT/images" && sha256sum bootloader.bin partitions.bin boot_app0.bin firmware.bin > manifest.sha256)
DEV=A PORT=/dev/pA BK="$BKA" run "oversize firmware with regenerated manifest" STOP bash install.sh
cp fw.save "$OUT/images/firmware.bin"; chmod a-w "$OUT/images/firmware.bin"; cp man.save "$OUT/images/manifest.sha256"; chmod a-w "$OUT/images/manifest.sha256"
cp dev/pA.img devA.save; printf '\x01' | dd of=dev/pA.img bs=1 seek=$((0x8040)) conv=notrunc status=none
DEV=A PORT=/dev/pA BK="$BKA" run "device partition table changed since backup" STOP bash install.sh; cp devA.save dev/pA.img
check "device A unchanged by all refusals" same dev/pA.img pA.orig
check "device B unchanged by all refusals" same dev/pB.img pB.orig
OLDBK=$(ls "$OUT"/backup-A-240ac400000c-*.bin); DEV=A PORT=/dev/pOld BK="$OLDBK" run "install with the rejected (old-layout) backup" STOP bash install.sh
SIM_WRITE_FAIL=1 DEV=A PORT=/dev/pA BK="$BKA" run "write failure piped through tee" STOP bash install.sh
cp pA.orig dev/pA.img
SIM_CORRUPT=1 DEV=A PORT=/dev/pA BK="$BKA" run "silent corruption during write (verify_flash)" STOP bash install.sh
cp pA.orig dev/pA.img
mv "$REPO/.pio" "$T/pio.moved"
DEV=A PORT=/dev/pA BK="$BKA" run "install A (no .pio in the clone; pio run forbidden)" PASS bash install.sh
DEV=B PORT=/dev/pB BK="$BKB" run "install B (no .pio in the clone; pio run forbidden)" PASS bash install.sh
mv "$T/pio.moved" "$REPO/.pio"
check "A: 0x1000/0x8000/0xE000/0x10000 hold the staged images" bash -c 'for p in "0x1000 bootloader.bin" "0x8000 partitions.bin" "0xe000 boot_app0.bin" "0x10000 firmware.bin"; do set -- $p; n=$(wc -c < "$OUT/images/$2"); cmp -s -n $n <(dd if=dev/pA.img bs=1 skip=$(($1)) count=$n status=none) "$OUT/images/$2" || exit 1; done'
check "A: ota_1 unchanged" region_eq dev/pA.img pA.orig $((0x170000)) $((0x160000))
check "A: NVS and LittleFS unchanged" region_eq dev/pA.img pA.orig $((0x2D0000)) $((0x130000))
unset SIM_NO_RUN
printf 'NEWNVS' | dd of=dev/pA.img bs=1 seek=$((0x2D0100)) conv=notrunc status=none; cp dev/pA.img pA.after

echo "== 9.2 / 9.3 rollback (device-bound backup only)"
DEV=A PORT=/dev/pB BK="$BKA" run "9.2: A's backup, device B on the port" STOP bash rollback92.sh
cp "$BKA" bk.save; chmod u+w "$BKA"; printf 'Q' | dd of="$BKA" bs=1 seek=100 conv=notrunc status=none
DEV=A PORT=/dev/pA BK="$BKA" run "9.2: tampered backup" STOP bash rollback92.sh; cp bk.save "$BKA"
DEV=A PORT=/dev/pA BK="$BKA" run "9.2: restore below NVS on device A" PASS bash rollback92.sh
check "A below NVS equals the backup" region_eq dev/pA.img "$BKA" 0 $((0x2D0000))
check "A NVS and LittleFS kept from after installation" region_eq dev/pA.img pA.after $((0x2D0000)) $((0x130000))
DEV=A PORT=/dev/pB BK="$BKA" run "9.3: A's backup, device B on the port" STOP bash restore93.sh
DEV=A PORT=/dev/pA BK="$BKA" run "9.3: full restore on device A" PASS bash restore93.sh
check "A equals its original image" same dev/pA.img pA.orig
check "B untouched by the refused rollbacks (equals its post-install state)" region_eq dev/pB.img pB.orig $((0x2D0000)) $((0x130000))
check "no command in the plan scripts runs pio run -t upload" bash -c '! grep -q "run.*-t upload" calls.log'
echo "== $N cases, $BAD unexpected"
