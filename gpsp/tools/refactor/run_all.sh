#!/usr/bin/env bash
# run_all.sh -- every phase-T tool's self-test and mutant tests, in one go, against
# the clean 01067e74 baseline. Prints one table row per step and exits 0 only if
# every step passed. No PSP build is made. psp-gcc -E / -fsyntax-only and one tiny
# 2-file link run in docker (pspdev/pspdev), serialised on the shared build lock.
#
#   bash gpsp/tools/refactor/run_all.sh [BASELINE_DIR] [SCRATCH_DIR]
#     BASELINE_DIR  flavours.sh --baseline output with run1/ and run2/
#                   (default builds/mainpsp-tools-out/baseline)
#     SCRATCH_DIR   where step logs and test scratch go (default: a new mktemp dir)
#   PAD144=DIR      release flavour dir of 01067e74 built with LAYOUT_PAD_BYTES=144
#                   (default builds/mainpsp-tools-out/scratch-elf-func/pad144/release)
#   LOCK=FILE       flock file for docker steps (default builds/mainpsp-tools-out/build.lock)
#   ONLY=a,b        run only the named steps (see STEPS below)
#
# Run from WSL (needs python3, bash, flock, docker). Whole run: about 15 min.
# Do not edit files in this tree while it runs. bash reads this script as it goes,
# and on the /mnt/c (OneDrive, 9p) mount a Windows-side write was seen to leave a
# step's cwd stale (FileNotFoundError from os.getcwd); re-run that step with ONLY=.
set -u
R=$(cd "$(dirname "$0")" && pwd)               # gpsp/tools/refactor
T=$R/tests
O=$(cd "$R/../../../.." && pwd)/mainpsp-tools-out
B=${1:-$O/baseline}
S=${2:-$(mktemp -d "${TMPDIR:-/tmp}/refactor_run_all.XXXXXX")}
P144=${PAD144:-$O/scratch-elf-func/pad144/release}
LOCK=${LOCK:-$O/build.lock}
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
mkdir -p "$S/logs"
[ -f "$B/run1/release/gpsp_adhoc.elf" ] || { echo "no baseline at $B (run1/release/gpsp_adhoc.elf)"; exit 2; }

ROWS=(); FAILS=0
step() {   # step <name> <tools> <gates> <cmd...>
  local name="$1" tools="$2" gates="$3"; shift 3
  if [ -n "${ONLY:-}" ] && ! [[ ",$ONLY," == *",$name,"* ]]; then return; fi
  local log="$S/logs/$name.log" t0=$SECONDS rc
  echo "== $name ($tools) ..."
  ( "$@" ) > "$log" 2>&1; rc=$?
  local res=PASS; [ "$rc" -eq 0 ] || { res=FAIL; FAILS=$((FAILS + 1)); tail -15 "$log" | sed 's/^/     | /'; }
  local nok nfail
  nok=$(grep -cE '^\s*(ok|PASS)\b|== ok' "$log"); nfail=$(grep -cE '^\s*FAIL\b|== FAILED' "$log")
  ROWS+=("$(printf '%-10s %-48s %-9s %-4s %3s %6s %5ss' "$name" "$tools" "$gates" "$res" "$rc" "$nok/$nfail" "$((SECONDS - t0))")")
  echo "   -> $res (exit $rc, $((SECONDS - t0)) s, log $log)"
}

# STEPS
step flavours  "T15 flavours.sh --selftest"                    "G1-build" bash "$T/test_flavours.sh"
step det_check "T15 det_check.py runs + selftest x3"           "G1-det" bash -c '
  set -e; '"$PY"' "'"$R"'/det_check.py" --runs "'"$B"'/run1" "'"$B"'/run2"
  for f in release lab harness; do '"$PY"' "'"$R"'/det_check.py" --selftest "'"$B"'/run2/$f/gpsp_adhoc.elf"; done'
step elf_func  "T1 func_diff, T1b loop_diff, T13 inline_census" "G2 G2L IC" \
  env EF_BASELINE="$B" EF_PAD144="$P144/gpsp_adhoc.elf" PYTHON="$PY" bash "$T/elf-func_run_tests.sh"
step sect_lay  "T2 sect_cmp, T5 hotord_check, T10 layout_rep"  "G1 G3" \
  env SL_LOCK="$LOCK" bash "$T/sections-layout_run_all.sh" "$B" "$P144"
step src_split "T3 pp_diff, T4 split, T7 extract, T9 globals"  "G0" \
  env TMPDIR="$S" flock "$LOCK" bash "$T/source-split_run_all.sh"
step abi_exit  "T6 strong_syms, T8 exit_matrix, T11, T14"      "SS XS" \
  env MAINPSP_LOCK="$LOCK" bash "$T/abi-exit-ci_run_all.sh" "$B/run1" "$S/abi-exit-ci"
step stack     "T12 stack_depth"                               "ST" \
  env PYTHON="$PY" bash "$T/stack-cache_test_stack_depth.sh" "$B/run1"
step lpsim     "T16 loop_lpsim (static part)"                  "LP" \
  env PYTHON="$PY" bash "$T/stack-cache_test_loop_lpsim.sh" "$B/run1"
# P0a (PLAN-P-beta5 §3): the repaired tools' own unit tests, then the "b5 row set": every
# P0a repair with its mutant on another tree.  B5_TREES="TREE:RUN[:CUTLIST] ..." (a clean
# source tree of the commit, its flavours.sh --baseline run, the cutlist of that source);
# RUN2 / TWIN / P4_RUN as tests/p0a_b5.sh documents them (applied to the first tree).  With a
# cutlist the b5 rows include a REAL P4 build (split.py + the cutlist as a commit, flavours.sh
# --commit --against RUN, the whole generated gate line; ~15 min, under LOCK).
step p0a_unit  "P0a loop_anchor, flavours_gate, exit_matrix raw" "G2L XS G1" bash -c '
  set -e; '"$PY"' "'"$R"'/loop_anchor.py" --selftest; '"$PY"' "'"$R"'/flavours_gate.py" --selftest
  '"$PY"' "'"$T"'/p0a_test_exit_matrix.py" "'"$R"'/../../psp/main_psp.c"'
k=0
for spec in ${B5_TREES:-}; do
  IFS=: read -r bt br bc <<< "$spec"; k=$((k + 1))
  if [ $k = 1 ]; then XR2=${RUN2:-} XTW=${TWIN:-} XP4=${P4_RUN:-}; else XR2= XTW= XP4=; fi
  step "b5_$k" "P0a b5 rows on $(basename "$(dirname "$bt")")"    "all" \
    env RUN2="$XR2" TWIN="$XTW" P4_RUN="$XP4" LOCK="$LOCK" bash "$T/p0a_b5.sh" --tree "$bt" --run "$br" ${bc:+--cutlist "$bc"} --out "$S/b5_$k"
done

echo
printf '%-10s %-48s %-9s %-4s %3s %6s %6s\n' step tools gates res rc ok/F time
printf '%s\n' "${ROWS[@]}"
echo "(ok/F = 'ok'/'PASS' and 'FAIL' lines in the step log; a FAIL line inside a passing"
echo " step is an expected mutant verdict printed by the tool, not a failed check)"
echo "scratch + logs: $S"
echo "run_all: $([ $FAILS -eq 0 ] && echo "ALL PASS" || echo "$FAILS step(s) FAILED")"
exit $((FAILS > 0))
