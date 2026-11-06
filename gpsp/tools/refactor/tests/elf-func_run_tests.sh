#!/usr/bin/env bash
# Runs the elf-func group's self-tests (func_diff T1, loop_diff T1b,
# inline_census T13) against the phase-T baseline.  Paths: EF_BASELINE,
# EF_PAD144, EF_SRC (see tests/elf_func_lib.py).  Exit 0 only if all pass.
cd "$(dirname "$0")" || exit 2
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
rc=0
for t in elf-func_test_func_diff.py elf-func_test_loop_diff.py elf-func_test_inline_census.py; do
  echo "== $t"
  "$PY" "$t" || rc=1
done
exit $rc
