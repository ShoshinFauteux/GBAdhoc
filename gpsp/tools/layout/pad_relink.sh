#!/usr/bin/env bash
# pad_relink.sh OUTDIR PIN TEXT BSS [FNPAD]  -- relink the release ELF with the
#   stability-probe padding (docs/LAYOUT-PINNING.md), from the core archive
#   already in the tree (build it first with tools/build.sh release and the
#   same LAYOUT_PIN).  Only the frontend link changes:
#     TEXT/BSS  psp/layout_pad.S (LAYOUT_PAD_TEXT/BSS): every later object's
#               .text and .bss move by that many bytes (0 0 = not linked)
#     FNPAD     LAYOUT_PAD_BYTES: a never-called cold function in main_psp.o
#   OUTDIR gets gpsp_adhoc.elf and elf.nm (psp-nm -n -S).  Git Bash + docker.
set -euo pipefail
OUT=$1; PIN=$2; TEXT=$3; BSS=$4; FNPAD=${5:-0}
cd "$(dirname "$0")/../.."
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
W="$PWD"; command -v cygpath >/dev/null && W=$(cygpath -w "$PWD")
PADV=""
[ "$TEXT$BSS" != "00" ] && PADV="LAYOUT_PAD_TEXT=$TEXT LAYOUT_PAD_BSS=$BSS"
# PROFILE=release (default) or harness64: must match the core archive in the tree
case "${PROFILE:-release}" in
  release)   DEFS='-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_PROFILE_RELEASE=1'; MK='ME_CATCH=1' ;;
  harness64) DEFS='-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG -DGPSP_PROFILE_HARNESS=1'
             MK='PSP_LARGE_MEMORY=1 ME_CATCH=1' ;;
  *) echo "PROFILE must be release or harness64"; exit 2 ;;
esac
docker run --rm -v "$W":/build -w /build/psp pspdev/pspdev sh -c "
  set -e; rm -f gpsp_adhoc.elf main_psp.o layout_pad.o
  make gpsp_adhoc.elf EXTRA_DEFS='$DEFS' \
    $MK LAYOUT_PIN=$PIN LAYOUT_PAD_BYTES=$FNPAD $PADV > /tmp/l.log 2>&1 || { tail -20 /tmp/l.log; exit 1; }
  grep -q 'stubs out of order' /tmp/l.log && { echo BROKEN IMPORTS; exit 1; } || true
  psp-nm -n -S gpsp_adhoc.elf > /tmp/elf.nm; cp /tmp/elf.nm layout/.pad.nm"
mkdir -p "$OUT"; cp psp/gpsp_adhoc.elf "$OUT/"; mv psp/layout/.pad.nm "$OUT/elf.nm"
rm -f psp/main_psp.o psp/layout_pad.o psp/gpsp_adhoc.elf
