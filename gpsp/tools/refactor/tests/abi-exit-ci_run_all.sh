#!/usr/bin/env bash
# abi-exit-ci_run_all.sh -- every self-test and mutant test of T6, T8, T11 and T14.
#   bash tests/abi-exit-ci_run_all.sh <baseline run dir> <scratch out dir>
# Docker work (psp-gcc -E/-dM/-fsyntax-only and one 2-file link) is serialised on the
# shared build lock when MAINPSP_LOCK points at it (flock), as the phase T rules ask.
set -u
BASE=${1:?baseline run dir}; OUT=${2:?scratch out dir}
HERE=$(cd "$(dirname "$0")" && pwd)
LOCK=${MAINPSP_LOCK:-}
run() { if [ -n "$LOCK" ]; then flock "$LOCK" "$@"; else "$@"; fi; }
rc=0
run bash "$HERE/abi-exit-ci_test_strong.sh" "$BASE" "$OUT/t6"   || rc=1
run bash "$HERE/abi-exit-ci_test_exit.sh"   "$BASE" "$OUT/t8"   || rc=1
bash "$HERE/abi-exit-ci_test_ci_greps.sh" "$OUT/t11" "$OUT/t8/base/pp" || rc=1
run bash "$HERE/abi-exit-ci_test_abi.sh"    "$BASE" "$OUT/t14"  || rc=1
echo "abi-exit-ci ALL: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
