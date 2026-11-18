#!/usr/bin/env bash
# regen.sh [--check | --check-only] [HOT_BUDGET]  -- regenerate the pinned order
#   (psp/layout/hot_order.txt and hot.ord) from the committed profile
#   (tools/layout/profile_weights.txt) and the function sizes of the tree's
#   current LAYOUT_PIN=1 build.  docs/LAYOUT-PINNING.md.  Git Bash or WSL, with docker.
#
# Run after `tools/build.sh <profile>` (pinning is built in), then
# rebuild: the order only changes the link, so tools/build.sh again.  When the
# per-frame path itself changes (new hot functions, not just new code
# elsewhere), refresh the profile first: tools/layout/README in the doc.
#
# Opt-in T5 checks (tools/refactor/hotord_check.py; neither changes what a
# plain `regen.sh` does, and no build runs regen.sh):
#   --check       report how stale the CURRENT order is against psp/gpsp_adhoc.elf
#                 (never fatal), regenerate, then require the NEW order to pass
#                 hotord_check with --sizes fail --no-placement: every entry matches
#                 exactly one function, no clone rename, sizes equal, hot.ord ==
#                 hot_order.txt (placement is skipped: the ELF has the old order).
#                 Exit 1 if it does not.
#   --check-only  no docker, no regeneration: check the committed order against
#                 psp/gpsp_adhoc.elf and exit with hotord_check's status (0 clean,
#                 1 stale/renamed/ambiguous/misplaced: an entry the ELF did not
#                 link in hot.ord's order, e.g. re-sectioned .text.unlikely.X,
#                 or an order regenerated but not yet rebuilt).  This is the step a build or CI
#                 runs to fail on a stale order.
set -euo pipefail
CHECK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --check)      CHECK=1; shift ;;
    --check-only) CHECK=2; shift ;;
    *) break ;;
  esac
done
cd "$(dirname "$0")/../.."
ROOT="$PWD"
BUDGET=${1:-${HOT_BUDGET:-6144}}
PY=python3; command -v python3 >/dev/null || PY=python
HOTORD="$PY tools/refactor/hotord_check.py --elf psp/gpsp_adhoc.elf --order psp/layout/hot_order.txt --ord psp/layout/hot.ord"
if [ "$CHECK" = 2 ]; then
  [ -f psp/gpsp_adhoc.elf ] || { echo "build first: tools/build.sh release"; exit 1; }
  exec $HOTORD --sizes fail
fi
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
W="$ROOT"; command -v cygpath >/dev/null && W=$(cygpath -w "$ROOT")
[ -f psp/gpsp_adhoc.elf ] && [ -f gpsp_libretro_psp1.a ] || {
  echo "build first: tools/build.sh release"; exit 1; }
if [ "$CHECK" = 1 ]; then
  echo "== hotord_check: the current order against psp/gpsp_adhoc.elf (informational)"
  $HOTORD --sizes warn || echo "== (the current order is stale; regenerating)"
fi
mkdir -p .layout
docker run --rm -v "$W":/b -w /b pspdev/pspdev sh -c '
  set -e
  psp-nm -n -S psp/gpsp_adhoc.elf > .layout/elf.nm
  cd .layout && rm -f mips_stub.o && psp-ar x ../gpsp_libretro_psp1.a mips_stub.o
  psp-nm -n -S mips_stub.o > mips_stub.nm
  for l in libc.a libgcc.a libm.a libcglue.a; do
    psp-nm -A --defined-only "$(psp-gcc -print-file-name=$l)" 2>/dev/null || true
  done > lib.nm'
grep -q " T update_gba$" .layout/elf.nm || { echo "no update_gba in the ELF?"; exit 1; }
# function sizes must come from a PINNED link (-ffunction-sections), whose
# .text starts with mips_stub.o's first routine
grep -Eq "^08804040 ([0-9a-f]+ )?T mips_update_gba$" .layout/elf.nm || {
  echo "psp/gpsp_adhoc.elf is not a LAYOUT_PIN=1 build: LAYOUT_PIN=1 tools/build.sh first"; exit 1; }
$PY tools/layout/gen_layout.py generate --weights tools/layout/profile_weights.txt \
  --psp-nm .layout/elf.nm --asm-nm .layout/mips_stub.nm --hot-budget "$BUDGET" \
  --frame-path tools/layout/psp_frame_path.txt --lib-nm .layout/lib.nm \
  --out-order psp/layout/hot_order.txt --out-ld psp/layout/hot.ord
if [ "$CHECK" = 1 ]; then
  echo "== hotord_check: the regenerated order against psp/gpsp_adhoc.elf"
  # the ELF was linked with the OLD order: names and sizes only, not placement
  $HOTORD --sizes fail --no-placement || { echo "regen.sh: the regenerated order fails T5 (hotord_check.py)"; exit 1; }
fi
