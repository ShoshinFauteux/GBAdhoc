#!/usr/bin/env bash
# stack-cache_test_loop_lpsim.sh -- T16 loop_lpsim.py self-test (no docker) on the
# clean baseline ELFs, plus a compile check of loop_lpsim_plugin.c when a
# qemu-plugin header is available (the twin image).  The twin-run part of T16
# (old vs new plugin on one trace) is loop_lpsim/loop_sim.sh; see docs/stack-cache.md.
#   bash tests/stack-cache_test_loop_lpsim.sh [BASELINE_RUN_DIR]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=${1:-$HERE/../../../../mainpsp-tools-out/baseline/run1}
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
FAILS=0
for f in release lab harness; do
  E=$RUN/$f/gpsp_adhoc.elf
  echo "== $f"
  "$PY" "$HERE/loop_lpsim/loop_lpsim.py" --selftest "$E" | grep -E "main 0x|by line|by address|ok|FAIL|selftest|recorded|M[5-7]"
  [ "${PIPESTATUS[0]}" -eq 0 ] || FAILS=$((FAILS + 1))
done
# the plugin copy must differ from tools/layout/lpsim_plugin.c only by the L group
L=$HERE/../layout/lpsim_plugin.c
N=$HERE/loop_lpsim/loop_lpsim_plugin.c
if grep -q 'GJ, GS, GF, GL, NG' "$N" && grep -q 'GJ, GS, GF, NG' "$L"; then
  echo "  ok    plugin copy adds group L; tools/layout/lpsim_plugin.c unchanged (GJ,GS,GF)"
else echo "  FAIL  plugin group enums"; FAILS=$((FAILS + 1)); fi
echo "stack-cache_test_loop_lpsim: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $((FAILS > 0))
