#!/usr/bin/env bash
# sections-layout_run_all.sh -- every self-test and mutant test of T2 sect_cmp.py,
# T5 hotord_check.py and T10 layout_report.py, on the phase-T clean baseline.
#
#   bash tests/sections-layout_run_all.sh [BASELINE_DIR] [PAD144_DIR]
#     BASELINE_DIR  flavours.sh --baseline output with run1/ and run2/
#                   (default mainpsp-tools-out/baseline beside the checkout; local)
#     PAD144_DIR    a release flavour dir of the same commit built with
#                   LAYOUT_PAD_BYTES=144 (default mainpsp-tools-out/ beside the checkout:
#                   scratch-sections-layout/pad144-release); its pair checks are
#                   skipped (and said so) when it is missing
#   SL_DOCKER=0     skip the real-toolchain test (needs docker + pspdev/pspdev)
#   SL_LOCK=FILE    run the docker step under flock FILE
#
# Exit 0 only if every step passed.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(cd "$HERE/../../../../mainpsp-tools-out" 2>/dev/null && pwd)
B=${1:-$OUT/baseline}
P144=${2:-$OUT/scratch-sections-layout/pad144-release}
PY=python3; command -v python3 >/dev/null 2>&1 && python3 -c 1 2>/dev/null || PY=python
FAILS=0
step() {  # step <label> <want rc> <cmd...>
  local label="$1" want="$2"; shift 2
  echo "== $label"
  "$@"; local rc=$?
  if [ "$rc" -eq "$want" ]; then echo "== ok ($label, exit $rc)"; else echo "== FAILED ($label: exit $rc, wanted $want)"; FAILS=$((FAILS + 1)); fi
}
cd "$HERE"
# ---- T2
step "T2 list (24 SHF_ALLOC sections expected on 01067e74)" 0 $PY sect_cmp.py --list "$B/run1/release" --expect-sections 24
step "T2 G1 determinism run1 vs run2, all flavours" 0 $PY sect_cmp.py --runs "$B/run1" "$B/run2" --expect-sections 24
step "T2 PLAN's 25 is NOT what the ELF has (expect exit 4)" 4 $PY sect_cmp.py --list "$B/run1/release" --expect-sections 25
for f in release lab harness; do step "T2 selftest $f" 0 $PY sect_cmp.py --selftest "$B/run2/$f"; done
# --runs must not skip a flavour built in one run only (it used to: "G1 over release: ok")
RT=$(mktemp -d "${TMPDIR:-/tmp}/sl_runs.XXXXXX")
mkdir -p "$RT/r1/release" "$RT/r1/lab" "$RT/r2/release"
cp "$B/run1/release/gpsp_adhoc.elf" "$RT/r1/release/"; cp "$B/run1/lab/gpsp_adhoc.elf" "$RT/r1/lab/"
cp "$B/run2/release/gpsp_adhoc.elf" "$RT/r2/release/"
step "T2 --runs with lab missing from run 2 fails (expect exit 1)" 1 $PY sect_cmp.py --runs "$RT/r1" "$RT/r2"
rm -rf "${RT:?}"
if [ -f "$P144/gpsp_adhoc.elf" ]; then
  step "T2 pad 304 vs 144 is a G1 difference (expect exit 1)" 1 $PY sect_cmp.py "$B/run1/release" "$P144"
fi
# ---- T5
for f in release lab harness; do step "T5 selftest $f" 0 $PY hotord_check.py --selftest "$B/run2/$f"; done
# the order now defaults to the build's own commit (flavour.json); the cause of the
# 2026-11-07 "5 FAILED" is kept as a mutant: this tree's order on another commit's ELF
GS() {  # git show from this repo (git.exe from WSL on a Windows worktree)
  if git -C "$HERE" rev-parse --git-dir >/dev/null 2>&1; then git -C "$HERE" show "$1"
  else   # (not `git.exe ... | tr`: the pipe's status would be tr's, so a tree that is no repo
         # at all, e.g. a git-archive export, read as an empty order and ran the mutant)
    local o; o=$(git.exe -C "$(wslpath -w "$HERE" 2>/dev/null)" show "$1" 2>/dev/null) || return 1
    printf '%s\n' "$o" | tr -d '\r'
  fi
}
BC=$(sed -nE 's/.*"sourceCommit": *"([0-9a-f]+)".*/\1/p' "$B/run2/release/flavour.json" 2>/dev/null)
if [ -n "$BC" ] && GS "$BC:gpsp/psp/layout/hot_order.txt" > /dev/null 2>&1 &&
   ! cmp -s <(GS "$BC:gpsp/psp/layout/hot_order.txt") ../../psp/layout/hot_order.txt; then
  step "T5 selftest with THIS tree's order on $BC's ELF (mismatch: expect exit 1)" 1 \
    $PY hotord_check.py --selftest "$B/run2/release" --order ../../psp/layout/hot_order.txt --ord ../../psp/layout/hot.ord
else
  echo "(T5 order-mismatch mutant not run: this tree's hot_order.txt is ${BC:-the baseline}'s, or no git history to read it)"
fi
step "T5 release today, --sizes fail (stale: expect exit 1)" 1 $PY hotord_check.py --build "$B/run1/release"
step "T5 regen.sh opt-in wiring" 0 bash tests/sections-layout_test_regen.sh "$B/run2/release"
# ---- T10
if [ -f "$P144/gpsp_adhoc.elf" ]; then
  step "T10 selftest release + the 304/144 pair" 0 $PY layout_report.py --selftest "$B/run2/release" --ref "$P144"
else
  echo "(no $P144: the 304/144 pad pair is NOT checked)"
  step "T10 selftest release" 0 $PY layout_report.py --selftest "$B/run2/release"
fi
for f in lab harness; do step "T10 selftest $f" 0 $PY layout_report.py --selftest "$B/run2/$f"; done
if [ "${SL_DOCKER:-1}" != 0 ] && command -v docker >/dev/null 2>&1; then
  step "T10 real-toolchain cold-section mutant" 0 bash tests/sections-layout_test_coldsec.sh
else
  echo "(docker test skipped: SL_DOCKER=0 or no docker)"
fi
echo "sections-layout_run_all: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS step(s) FAILED")"
exit $((FAILS > 0))
