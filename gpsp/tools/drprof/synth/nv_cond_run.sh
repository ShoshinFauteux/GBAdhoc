#!/usr/bin/env bash
# nv_cond_run.sh -- NV_COND_FIX's test, in WSL/Linux (docker image drprof-qemu).
#
#   tools/drprof/synth/nv_cond_run.sh [build]
#
# Twins in $DRPROF_HOME (default ~/nvcond-drprof): fix (what ships), nofix
# (-DGPSP_NO_NV_COND_FIX), one per mutant, and the GBA_LINK pair (instance B is
# the same object with every global renamed).  Expected: fix, the interpreter
# and both pair slots PASS; nofix crashes on nv.gba and on the issue #10 word
# alone; both mutants FAIL.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../../.." && pwd)"
export DRPROF_HOME=${DRPROF_HOME:-$HOME/nvcond-drprof}
G="$HERE/nv_cond_gen.py"
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
mkdir -p "$DRPROF_HOME/fx" "$DRPROF_HOME/nv"
[ -f "$DRPROF_HOME/fx/gba_bios.bin" ] || cp "$HOME/drprof/fx/gba_bios.bin" "$DRPROF_HOME/fx/"
cd "$DRPROF_HOME/nv" || exit 2
python3 "$G" gen nv.gba
python3 "$G" gen nv_issue.gba --only issue
python3 "$G" gen nv_exits.gba --only exits
if [ "${1:-}" = build ]; then
  B="$TREE/tools/drprof/dr_build.sh"
  FEATURES=baked DRPROF_SRC=src-fix bash "$B" fix "" | tail -1
  FEATURES=baked DRPROF_SRC=src-nofix bash "$B" nofix "-DGPSP_NO_NV_COND_FIX" | tail -1
  for m in nv_emit_body nv_scan_keeps_cond; do
    FEATURES=baked DRPROF_SRC=src-$m DRPROF_PATCH="$TREE/tools/drprof/mutants/$m.patch" \
      bash "$B" "$m" "" | tail -1
  done
  docker run --rm -v "$DRPROF_HOME":/w -w /w/src-fix/tools/drprof "$IMAGE" bash -c \
    "make plugin >/dev/null && make pair VARIANT=fix FEATURES=baked 2>&1 | grep -E ' error |Error ' ; cp bin/dr_pair_fix /w/bin/"
fi
cat > run.sh <<'EOS'
#!/bin/bash
# run.sh VARIANT ROM TAG [extra options]
v=$1; r=$2; t=$3; shift 3
rm -f d_$t.bin
DR_RAMDUMP=30 DR_RAMDUMP_FILE=/w/nv/d_$t.bin timeout 300 /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
  /w/bin/dr_host_$v --rom /w/nv/$r --bios-dir /w/fx --frames 40 "$@" > o_$t.txt 2>&1
echo "exit=$?" > e_$t.txt
EOS
docker run --rm -v "$DRPROF_HOME":/w -w /w/nv "$IMAGE" bash -c '
  bash run.sh fix nv.gba fix
  bash run.sh fix nv.gba int --option gpsp_drc=disabled
  bash run.sh nofix nv.gba nofix
  bash run.sh fix nv_issue.gba fix_issue
  bash run.sh nofix nv_issue.gba nofix_issue
  bash run.sh fix nv_exits.gba fix_exits
  bash run.sh nofix nv_exits.gba nofix_exits
  bash run.sh nv_emit_body nv.gba nv_emit_body
  bash run.sh nv_scan_keeps_cond nv.gba nv_scan_keeps_cond
  rm -rf pair; mkdir pair
  timeout 300 /opt/qemu/bin/qemu-mipsel -cpu 74Kf /w/bin/dr_pair_fix --rom /w/nv/nv.gba --bios-dir /w/fx \
     --frames 40 --ram-dump pair:30:30:1 > o_pair.txt 2>&1; echo "exit=$?" > e_pair.txt
'
rc=0
expect() {   # expect pass|fail TAG [--only X]
  want=$1; t=$2; shift 2
  e=$(cat "e_$t.txt")
  if [ -f "d_$t.bin" ] && python3 "$G" check "d_$t.bin" "$@" > "r_$t.txt"; then got=pass; else got=fail; fi
  [ -f "d_$t.bin" ] || echo "no dump" > "r_$t.txt"
  printf '%-22s %-9s want %s, got %s: %s\n' "$t" "$e" "$want" "$got" "$(tail -1 "r_$t.txt")"
  [ "$want" = "$got" ] || rc=1
}
expect pass fix
expect pass int
expect fail nofix
expect pass fix_issue --only issue
expect fail nofix_issue --only issue
expect pass fix_exits --only exits
expect fail nofix_exits --only exits
expect fail nv_emit_body
expect fail nv_scan_keeps_cond
for s in 0 1; do
  if python3 "$G" check-ram pair/r${s}_000030.bin > r_pair$s.txt; then got=pass; else got=fail; rc=1; fi
  printf '%-22s %-9s want pass, got %s: %s\n' "pair slot $s" "$(cat e_pair.txt)" "$got" "$(tail -1 r_pair$s.txt)"
done
# Chandler Hardy's nv_cond.s (PR #26): needs arm-none-eabi-as, here or in an
# image ($NVAS_IMAGE, e.g. debian:stable-slim + binutils-arm-none-eabi).
AS_IMG=${NVAS_IMAGE:-armas:local}
mkdir -p "$DRPROF_HOME/nvc"; cp "$HERE/nv_cond.s" "$DRPROF_HOME/nvc/"
if command -v arm-none-eabi-as >/dev/null; then
  (cd "$DRPROF_HOME/nvc" && arm-none-eabi-as -mcpu=arm7tdmi nv_cond.s -o n.o && arm-none-eabi-objcopy -O binary n.o nv_cond.gba)
elif docker image inspect "$AS_IMG" >/dev/null 2>&1; then
  docker run --rm -v "$DRPROF_HOME/nvc":/s -w /s "$AS_IMG" sh -c \
    "arm-none-eabi-as -mcpu=arm7tdmi nv_cond.s -o n.o && arm-none-eabi-objcopy -O binary n.o nv_cond.gba"
fi
if [ -f "$DRPROF_HOME/nvc/nv_cond.gba" ]; then
  docker run --rm -v "$DRPROF_HOME":/w -w /w/nvc "$IMAGE" bash -c '
    rm -f d_fix.bin d_int.bin
    DR_RAMDUMP=10 DR_RAMDUMP_FILE=/w/nvc/d_fix.bin /opt/qemu/bin/qemu-mipsel -cpu 74Kf /w/bin/dr_host_fix \
      --rom /w/nvc/nv_cond.gba --bios-dir /w/fx --frames 12 >/dev/null 2>&1
    DR_RAMDUMP=10 DR_RAMDUMP_FILE=/w/nvc/d_int.bin /opt/qemu/bin/qemu-mipsel -cpu 74Kf /w/bin/dr_host_fix \
      --rom /w/nvc/nv_cond.gba --bios-dir /w/fx --frames 12 --option gpsp_drc=disabled >/dev/null 2>&1'
  for t in fix int; do
    if python3 "$HERE/nv_cond_check.py" "$DRPROF_HOME/nvc/d_$t.bin" > "r_nvc_$t.txt" 2>&1; then got=pass; else got=fail; rc=1; fi
    printf '%-22s %-9s want pass, got %s\n' "nv_cond.s $t" "" "$got"
  done
else
  echo "nv_cond.s: no ARM assembler, skipped"
fi
exit $rc
