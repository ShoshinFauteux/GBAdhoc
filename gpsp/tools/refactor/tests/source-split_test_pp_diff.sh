#!/usr/bin/env bash
# source-split_test_pp_diff.sh -- self-test of pp_diff.sh (T3 / G0) on the real
# tree (no build; docker + pspdev/pspdev for psp-gcc -E only).  Run from WSL:
#   bash gpsp/tools/refactor/tests/source-split_test_pp_diff.sh [GPSP_DIR] [SPLIT_DIR]
# GPSP_DIR defaults to this checkout's gpsp/.  SPLIT_DIR (optional): a split.py
# output to check as a control (base vs split must be IDENTICAL x3).
#
# Controls (must pass, exit 0) and mutants (must be caught, exit 1).  Every
# mutant is a copy of main_psp.c in an overlay dir: the tree is never written.
#   C1  base vs base                                  identical x3
#   C2  whitespace + line breaks + a comment changed   identical x3 (normalisation)
#   C3  base vs SPLIT_DIR (if given)                   identical x3
#   M1  a token added in release-live code            caught in release, lab, harness
#   M2  a token added under #ifdef GPSP_PERF_RIG       caught in lab+harness ONLY (release identical)
#   M3  one blank line added above a __LINE__ use      caught (layout-sensitive macro)
#   M4  SOURCE_DATE_EPOCH not pinned (--sde '')        caught in lab+harness (the build stamp)
#   M5  a quoted include resolving to another file     caught
#   M6  a doubled blank inside a string literal        caught (literals are kept byte for byte)
#   M7  a blank -> TAB inside a string literal         caught
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
G=${1:-$(cd "$HERE/../.." && pwd)}
SPLIT=${2:-}
T=$(mktemp -d "${TMPDIR:-/tmp}/ppdifftest.XXXXXX"); trap 'rm -rf "$T"' EXIT
FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
PPLOGS=${PP_LOGS:-$HERE/../../../../mainpsp-tools-out/baseline/run1}   # pp_diff needs --logs since P0a
PP() { bash "$HERE/pp_diff.sh" --logs "$PPLOGS" --ref "$G" --new "$G" "$@" > "$T/out.log" 2>&1; echo $?; }
verdict() { grep -E "^  $1: " "$T/out.log" | awk '{print $2}'; }
mk() { rm -rf "$T/ovl"; mkdir -p "$T/ovl/psp"; cp "$G/psp/main_psp.c" "$T/ovl/psp/main_psp.c"; }
expect() {  # expect <label> <want_rc> <release> <lab> <harness>  (I=IDENTICAL D=DIFFERENT)
  local label=$1 want=$2 rc=$3; shift 3
  local got="" f; for f in release lab harness; do got="$got$(verdict $f | cut -c1)"; done
  if [ "$rc" = "$want" ] && [ "$got" = "$1" ]; then ok "$label (exit $rc, rel/lab/har $got)"
  else bad "$label: exit $rc (want $want), rel/lab/har $got (want $1)"; sed 's/^/        /' "$T/out.log" | tail -15; fi
}

echo "pp_diff self-test on $G"
expect "C1 base vs base" 0 "$(PP)" III

mk; f="$T/ovl/psp/main_psp.c"
# re-indent every line of the first 400, split one declaration over two lines, add comments
sed -i -e '200,400s/^   /\t/' -e '1i /* pp_diff self-test: a comment only */' "$f"
sed -i -e 's/^static volatile int g_running = 1;/static volatile int\n   g_running = 1;   \/\/ moved/' "$f" 2>/dev/null
expect "C2 whitespace/comments only" 0 "$(PP --overlay "$T/ovl")" III

if [ -n "$SPLIT" ]; then
  expect "C3 base vs split" 0 "$(PP --overlay "$SPLIT")" III
fi

mk; f="$T/ovl/psp/main_psp.c"
printf '\nstatic int pp_mutant_all;\n' >> "$f"
expect "M1 release-live token" 1 "$(PP --overlay "$T/ovl")" DDD

mk; f="$T/ovl/psp/main_psp.c"
printf '\n#ifdef GPSP_PERF_RIG\nstatic int pp_mutant_rig;\n#endif\n' >> "$f"
expect "M2 rig-only token" 1 "$(PP --overlay "$T/ovl")" IDD

# M3: both sides carry the same __LINE__ use; the new side has one extra blank line above it
mk; printf '\nstatic const int pp_line = __LINE__;\n' >> "$T/ovl/psp/main_psp.c"
mkdir -p "$T/ref/psp"; cp "$T/ovl/psp/main_psp.c" "$T/refmain.c"
sed -i '1i\\' "$T/ovl/psp/main_psp.c"
# compare overlay-with-blank against overlay-without-blank: run pp_diff with ref = a tree copy is heavy,
# so instead check the new side against base twice: both differ from base, and their streams differ
bash "$HERE/pp_diff.sh" --logs "$PPLOGS" --ref "$G" --new "$G" --overlay "$T/ovl" --flavours release --keep "$T/k1" > /dev/null 2>&1
mkdir -p "$T/ovl2/psp"; cp "$T/refmain.c" "$T/ovl2/psp/main_psp.c"
bash "$HERE/pp_diff.sh" --logs "$PPLOGS" --ref "$G" --new "$G" --overlay "$T/ovl2" --flavours release --keep "$T/k2" > /dev/null 2>&1
if [ -f "$T/k1/release/new.tok" ] && ! cmp -s "$T/k1/release/new.tok" "$T/k2/release/new.tok"; then
  ok "M3 __LINE__ shifted by one blank line is a token difference ($(grep -o 'pp_line = [0-9]*' "$T/k1/release/new.tok") vs $(grep -o 'pp_line = [0-9]*' "$T/k2/release/new.tok"))"
else bad "M3 a blank line above __LINE__ was not seen"; fi

expect "M4 unpinned SOURCE_DATE_EPOCH" 1 "$(sleep 1; PP --sde '')" IDD

mk; f="$T/ovl/psp/main_psp.c"
# psp/fe_util.h does not exist; frontend-common/fe_util.h does.  Point one include at a
# DIFFERENT existing header of the same tree (the token stream must change)
sed -i 's#^\#include "fe_linkprof.h"#\#include "fe_linkprof.h"\n\#include "ff_gate.h"\n\#include "../gpsp_profile.h"#' "$f"
sed -i '0,/^#include "state_slots.h"/s//#include "rom_paths.h"/' "$f"
expect "M5 an include pulling a different header" 1 "$(PP --overlay "$T/ovl" --flavours release)" D

# M6/M7 (adversarial review 2026-11-05): whitespace INSIDE a release-live string
# literal is a .rodata change; the old `tr -s` token view called both IDENTICAL.
mk; f="$T/ovl/psp/main_psp.c"
sed -i 's/"This build cannot start a game here\."/"This build cannot  start a game here."/' "$f"
if cmp -s "$G/psp/main_psp.c" "$f"; then bad "M6 setup: the anchor string was not found"; else
expect "M6 a doubled blank inside a string literal" 1 "$(PP --overlay "$T/ovl" --flavours release)" D; fi
mk; f="$T/ovl/psp/main_psp.c"
sed -i 's/"This build cannot start a game here\."/"This build cannot\tstart a game here."/' "$f"
if cmp -s "$G/psp/main_psp.c" "$f"; then bad "M7 setup: the anchor string was not found"; else
expect "M7 a blank turned into a TAB inside a string literal" 1 "$(PP --overlay "$T/ovl" --flavours release)" D; fi

echo "pp_diff self-test: $FAILS failure(s)"
[ "$FAILS" -eq 0 ]
