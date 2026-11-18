#!/usr/bin/env bash
# stack-cache_test_stack_depth.sh -- T12 stack_depth.py self-test and mutants on
# the clean baseline ELFs (no build).  Usage:
#   bash tests/stack-cache_test_stack_depth.sh [BASELINE_RUN_DIR]
# default BASELINE_RUN_DIR = mainpsp-tools-out/baseline/run1 beside the checkout (local, not in the repo).
# Per flavour: the control report (exit 0 = PASS with the reviewed stack_resolve.txt)
# and `stack_depth.py --selftest` (vsnprintf/fopen non-zero, .su agreement, mutants M1-M6).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=${1:-$HERE/../../../../mainpsp-tools-out/baseline/run1}
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
FAILS=0
for f in release lab harness; do
  E=$RUN/$f/gpsp_adhoc.elf
  [ -f "$E" ] || { echo "missing $E"; FAILS=$((FAILS + 1)); continue; }
  echo "== $f"
  "$PY" "$HERE/stack_depth.py" "$E" --su "$RUN/$f/su" --resolve "$HERE/stack_resolve.txt" --tree 0 \
    | grep -E "TOTAL|RAW|verdict|root "
  rc=${PIPESTATUS[0]}
  [ "$rc" -eq 0 ] || { echo "  FAIL control verdict exit $rc"; FAILS=$((FAILS + 1)); }
  "$PY" "$HERE/stack_depth.py" --selftest "$E" --su "$RUN/$f/su" --resolve "$HERE/stack_resolve.txt" \
    | grep -E "ok|FAIL|selftest|skipped"
  [ "${PIPESTATUS[0]}" -eq 0 ] || FAILS=$((FAILS + 1))
done
# unresolved control: without the resolve file the harness graph must NOT pass
E=$RUN/harness/gpsp_adhoc.elf
"$PY" "$HERE/stack_depth.py" "$E" --su "$RUN/harness/su" --tree 0 >/dev/null
rc=$?
if [ "$rc" -ne 0 ]; then echo "  ok    no resolve file: harness verdict is not PASS (exit $rc)"
else echo "  FAIL  no resolve file: harness passed with indirect calls unresolved"; FAILS=$((FAILS + 1)); fi
echo "stack-cache_test_stack_depth: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $((FAILS > 0))
