#!/usr/bin/env bash
# run_blit_matrix.sh — the GU colour gate across every blit path, asserting
# which one each run ACTUALLY used.
#
# Exists because a first attempt at this reported three passes while silently
# running the default mode three times (an unexpanded shell variable).  A green
# gate that cannot name the variant it exercised proves nothing, so every run
# here re-reads `EVT blit_mode req=N mode=N` out of its own log and fails if it
# is not the mode that was requested.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
export SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sb-morning}"

rc=0
for m in 0 1 2; do
  echo "===== blit_mode=${m} ====="
  timeout 600 ./tools/e2e/run_gu_color_test.sh --blit-mode="${m}" 2>&1 | tail -3
  last=$(ls -td tools/e2e/artifacts/gucolor-* 2>/dev/null | head -1)
  got=$(grep -ho "EVT blit_mode req=[0-9]* mode=[0-9]*" "$last"/*.log 2>/dev/null | head -1)
  echo "  ran: ${got:-<none found>}"
  case "$got" in
    "EVT blit_mode req=${m} mode=${m}") ;;
    *) echo "  FAIL: asked for mode ${m}, log says '${got}'"; rc=1 ;;
  esac
done

echo "===== gu_defer=1 (default blit mode) ====="
timeout 600 ./tools/e2e/run_gu_color_test.sh --gu-defer=1 2>&1 | tail -3

[ $rc -eq 0 ] && echo "BLIT MATRIX PASS" || echo "BLIT MATRIX FAIL"
exit $rc
