#!/usr/bin/env bash
# mirror_pc_run.sh -- RAM_MIRROR_FIX's test, in WSL/Linux/macOS (docker image
# drprof-qemu; an ARM assembler here or in $MPAS_IMAGE).
#
#   tools/drprof/synth/mirror_pc_run.sh [build]
#
# Twins in $DRPROF_HOME (default ~/mirror-drprof): mfix (what ships), mnofix
# (-DGPSP_NO_RAM_MIRROR_FIX: the 4.6.0-beta.2 lookup), and the two mutants of
# tools/drprof/mutants/:
#   rm_no_canon     the lookup does not canonicalize (the lazy alias exits stay)
#   rm_eager_alias  alias exits are linked eagerly again (the lookup stays)
# Expected: mfix and the interpreter PASS; mnofix FAILs arm, exit, thumb,
# ewram, ewexit; rm_no_canon FAILs arm, thumb, ewram (so the call site in
# block_lookup_translate_* is the one exercised).  rm_eager_alias PASSes:
# with a canonical lookup an eager alias link is only slower, not wrong; it
# is caught by the twin's counters instead (TMGC: alias translations and RAM
# flushes, docs/BUILD-SWITCHES.md RAM_MIRROR_FIX).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../../.." && pwd)"
export DRPROF_HOME=${DRPROF_HOME:-$HOME/mirror-drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
AS_IMG=${MPAS_IMAGE:-armas:local}
mkdir -p "$DRPROF_HOME/fx" "$DRPROF_HOME/mp"
if [ ! -f "$DRPROF_HOME/fx/gba_bios.bin" ] && [ -f "$HOME/drprof/fx/gba_bios.bin" ]; then
  cp "$HOME/drprof/fx/gba_bios.bin" "$DRPROF_HOME/fx/"   # else the built-in BIOS
fi
cp "$HERE/mirror_pc.s" "$DRPROF_HOME/mp/"
if command -v arm-none-eabi-as >/dev/null; then
  (cd "$DRPROF_HOME/mp" && arm-none-eabi-as -mcpu=arm7tdmi mirror_pc.s -o m.o && arm-none-eabi-objcopy -O binary m.o mirror_pc.gba)
elif docker image inspect "$AS_IMG" >/dev/null 2>&1; then
  docker run --rm -v "$DRPROF_HOME/mp":/s -w /s "$AS_IMG" sh -c \
    "arm-none-eabi-as -mcpu=arm7tdmi mirror_pc.s -o m.o && arm-none-eabi-objcopy -O binary m.o mirror_pc.gba"
fi
[ -f "$DRPROF_HOME/mp/mirror_pc.gba" ] || { echo "mirror_pc.s: no ARM assembler"; exit 2; }
if [ "${1:-}" = build ]; then
  B="$TREE/tools/drprof/dr_build.sh"
  FEATURES=baked DRPROF_SRC=src-mfix bash "$B" mfix "" | tail -1
  FEATURES=baked DRPROF_SRC=src-mnofix bash "$B" mnofix "-DGPSP_NO_RAM_MIRROR_FIX" | tail -1
  for m in rm_no_canon rm_eager_alias; do
    FEATURES=baked DRPROF_SRC=src-$m DRPROF_PATCH="$TREE/tools/drprof/mutants/$m.patch" \
      bash "$B" "$m" "" | tail -1
  done
fi
cat > "$DRPROF_HOME/mp/run.sh" <<'EOS'
#!/bin/bash
# run.sh VARIANT TAG [extra options]
v=$1; t=$2; shift 2
rm -f d_$t.bin
DR_RAMDUMP=30 DR_RAMDUMP_FILE=/w/mp/d_$t.bin timeout 300 /opt/qemu/bin/qemu-mipsel -cpu 74Kf \
  /w/bin/dr_host_$v --rom /w/mp/mirror_pc.gba --bios-dir /w/fx --frames 40 "$@" > o_$t.txt 2>&1
echo "exit=$?" > e_$t.txt
EOS
docker run --rm -v "$DRPROF_HOME":/w -w /w/mp "$IMAGE" bash -c '
  bash run.sh mfix fix
  bash run.sh mfix int --option gpsp_drc=disabled
  bash run.sh mnofix nofix
  bash run.sh rm_no_canon rm_no_canon
  bash run.sh rm_eager_alias rm_eager_alias
'
cd "$DRPROF_HOME/mp" || exit 2
rc=0
expect() {   # expect pass|fail TAG [case ...]: the listed cases must be the failing ones
  want=$1; t=$2; shift 2
  if [ -f "d_$t.bin" ] && python3 "$HERE/mirror_pc_check.py" "d_$t.bin" > "r_$t.txt"; then got=pass; else got=fail; fi
  [ -f "d_$t.bin" ] || echo "no dump" > "r_$t.txt"
  fails=$(grep ' FAIL$' "r_$t.txt" | sed 's/ *got.*//' | tr '\n' ',' | sed 's/,$//')
  ok=1
  [ "$want" = "$got" ] || ok=0
  if [ $# -gt 0 ] && [ "$fails" != "$(IFS=,; echo "$*")" ]; then ok=0; fi
  printf '%-16s %-9s want %s, got %s%s %s\n' "$t" "$(cat "e_$t.txt")" "$want" "$got" \
    "${fails:+ ($fails)}" "$([ $ok = 1 ] && echo ok || echo MISMATCH)"
  [ $ok = 1 ] || rc=1
}
expect pass fix
expect pass int
expect fail nofix "arm canonical" "exit F2" "thumb canonical" "ewram canonical" "ewexit F5"
expect fail rm_no_canon "arm canonical" "thumb canonical" "ewram canonical"
expect pass rm_eager_alias
exit $rc
