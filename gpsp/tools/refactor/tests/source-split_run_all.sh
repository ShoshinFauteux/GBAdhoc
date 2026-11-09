#!/usr/bin/env bash
# source-split_run_all.sh -- every self-test of the source-split group
# (T3 pp_diff.sh, T4 split.py, T7 extract.py, T9 globals.py).  Run from WSL:
#   bash gpsp/tools/refactor/tests/source-split_run_all.sh
# The Python tests also run on Windows (python tests/source-split_test_*.py);
# pp_diff needs docker + pspdev/pspdev.  No PSP build is made.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/.." && pwd)
G=$(cd "$R/../.." && pwd)
PY=python3; command -v python3 >/dev/null || PY=python
OUT=$(mktemp -d "${TMPDIR:-/tmp}/srcsplit.XXXXXX"); trap 'rm -rf "$OUT"' EXIT
FAIL=0
step() { echo "== $1"; shift; "$@" || { echo "== FAILED: $*"; FAIL=$((FAIL + 1)); }; }
step "T4 split.py"   $PY "$HERE/source-split_test_split.py"
step "T9 globals.py" $PY "$HERE/source-split_test_globals.py"
step "T7 extract.py" $PY "$HERE/source-split_test_extract.py"
step "T4 split of the base (for T3 C3)" $PY "$R/split.py" split --cutlist "$HERE/fixtures/cutlist_01067e74.txt" --out "$OUT/split"
step "T3 pp_diff.sh" bash "$HERE/source-split_test_pp_diff.sh" "$G" "$OUT/split"
echo "source-split: $FAIL failing step(s)"
[ "$FAIL" -eq 0 ]
