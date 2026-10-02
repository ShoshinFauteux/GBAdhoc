#!/usr/bin/env bash
# jc_variants.sh -- build the twin variants of the JIT_CODE_DISCIPLINE study
# (docs/JIT-CODE-DISCIPLINE.md).  Sequential: dr_build.sh builds in one
# rsynced tree.  Use your own DRPROF_HOME if another agent uses ~/drprof.
#
#   tools/drprof/jc_variants.sh [name ...]      (default: all)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CAND="-DDISPATCH_CACHE=1 -DSMC_RETIRE_WINDOW=1 -DSERIAL_IDLE_FAST=1"
DISC="-DJIT_CODE_DISCIPLINE=1"
declare -A V=(
  [base]=""                                         # tools/build.sh CORE_COMMON before candidate-dr
  [basedisc]="$DISC"
  [cand]="$CAND"                                    # candidate-dr's CORE_COMMON
  [candstats]="$CAND -DJIT_SYNC_STATS=1"            # + counts of the original sync path
  [disc]="$CAND $DISC"                              # the candidate + the discipline
  [discaudit]="$CAND $DISC -DJIT_CODE_AUDIT=1"      # + zone asserts + writer audit
  [auditneg]="$CAND $DISC -DJIT_CODE_AUDIT=1 -DJIT_CODE_AUDIT_NEGCTL=1"  # must FAIL the audit
  [abdisc]="$CAND $DISC -DJIT_CODE_AB=1"            # the hardware A/B build, mode 2
  [ablegacy]="$CAND $DISC -DJIT_CODE_AB=1 -DJIT_CODE_MODE_DEFAULT=0"  # ... mode 0
)
ORDER="base basedisc cand candstats disc discaudit auditneg abdisc ablegacy"
[ $# -gt 0 ] && ORDER="$*"
for v in $ORDER; do
  echo "=== $v: ${V[$v]}"
  "$HERE/dr_build.sh" "$v" "${V[$v]}" 2>&1 | tail -2
done
