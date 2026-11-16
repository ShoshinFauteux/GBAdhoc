#!/usr/bin/env bash
# pad_relink.sh OUTDIR PIN TEXT BSS  -- relink the release ELF with the
#   stability-probe padding (docs/LAYOUT-PINNING.md), from the core archive
#   already in the tree (build it first with tools/build.sh release and the
#   same LAYOUT_PIN; PIN must be 1, pinning is built in since code-health
#   A-7).  Only the frontend link changes:
#     TEXT/BSS  LAYOUT_PAD_TEXT/BSS: TEXT moves the pins fe_head, fe_common,
#               fe_tail and core (tools/layout/pins.awk sweep), so all code
#               after main moves that many bytes mod 8 KiB; BSS links
#               psp/layout_pad.S first, every object's .bss moves (0 0 = off)
#   (FNPAD, the never-called LAYOUT_PAD_BYTES function in main_psp.o, is gone:
#   code-health C3-3 made that pad `pin fe_head`; the profile's pins apply)
#   OUTDIR gets gpsp_adhoc.elf and elf.nm (psp-nm -n -S).  Git Bash + docker.
set -euo pipefail
OUT=$1; PIN=$2; TEXT=$3; BSS=$4
cd "$(dirname "$0")/../.."
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
W="$PWD"; command -v cygpath >/dev/null && W=$(cygpath -w "$PWD")
PADV=""
[ "$TEXT$BSS" != "00" ] && PADV="LAYOUT_PAD_TEXT=$TEXT LAYOUT_PAD_BSS=$BSS"
# PROFILE=release (default) or harness64: must match the core archive in the tree
case "${PROFILE:-release}" in
  release)   DEFS='-DGPSP_PLAYABLE -DGPSP_PROFILE_RELEASE=1 -DPLAY_INPUT_FRESH=1'; MK='ME_CATCH=1' ;;
  harness64) DEFS='-DGPSP_PLAYABLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG -DGPSP_PROFILE_HARNESS=1 -DPLAY_INPUT_FRESH=1'
             MK='PSP_LARGE_MEMORY=1 ME_CATCH=1' ;;
  *) echo "PROFILE must be release or harness64"; exit 2 ;;
esac
docker run --rm -v "$W":/build -w /build/psp pspdev/pspdev sh -c "
  set -e; rm -f gpsp_adhoc.elf main_psp.o layout_pad.o
  make gpsp_adhoc.elf EXTRA_DEFS='$DEFS' \
    $MK LAYOUT_PIN=$PIN LAYOUT_FLAVOUR=${PROFILE:-release} $PADV > /tmp/l.log 2>&1 || { tail -20 /tmp/l.log; exit 1; }
  grep -q 'stubs out of order' /tmp/l.log && { echo BROKEN IMPORTS; exit 1; } || true
  psp-nm -n -S gpsp_adhoc.elf > /tmp/elf.nm; cp /tmp/elf.nm layout/.pad.nm"
mkdir -p "$OUT"; cp psp/gpsp_adhoc.elf "$OUT/"; mv psp/layout/.pad.nm "$OUT/elf.nm"
rm -f psp/main_psp.o psp/layout_pad.o psp/gpsp_adhoc.elf
