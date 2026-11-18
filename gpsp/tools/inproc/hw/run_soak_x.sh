#!/bin/bash
# run_soak_x.sh -- the PSP-3000 follow-up to run_soak.sh (docs/INPROC-SOAK.md):
# arm X (jit_small = 1, so H&S is resident on real hardware; make_stages.py
# --only X) then Z (the rig's own files back) with STOP.  Wake cycle stages X,
# cycle 1 collects X and stages Z + STOP: the console parks.
source "$(cd "$(dirname "$0")/../.." && pwd)/workdirs.sh"
# INPROC_SOAK_TREE: the gpsp tree to run (default this script's); INPROC_SOAK_OUT: stages + logs
INPROC_SOAK_OUT=${INPROC_SOAK_OUT:-$GBADHOC_WORK/inproc-soak-out}
cd "${INPROC_SOAK_TREE:-$(dirname "$0")/../../..}" || exit 9
O=$INPROC_SOAK_OUT; ST=$O/stage/3000; L=$O/logs/3000x
mkdir -p $L
echo "=== soak 3000 X start $(date +%H:%M:%S) pid $$ ===" >> $L/loop.log
python -u tools/rig/hw_loop.py --host F: --join "" --appdir GBADHOC-GBLINK \
  --arms X=$ST/X,Z=$ST/Z --order XZ --runs 1 --golden $ST/golden --logs $L \
  --keep-going --timeout 3600 --window-min-left 45 >> $L/loop.log 2>&1
echo "=== soak 3000 X hw_loop exit=$? $(date +%H:%M:%S) ===" >> $L/loop.log
