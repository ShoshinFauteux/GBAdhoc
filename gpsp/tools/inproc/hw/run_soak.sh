#!/bin/bash
# run_soak.sh 3000|1000 -- the in-process return soak on ONE console
# (docs/INPROC-SOAK.md): a solo hw_loop over the arms make_stages.py wrote.
#   S = S1 (install + 100 returns)  U = UI arm  R E C H = fresh boots
#   T = S2 (100 returns, reverse)    Z = restore the rig (staged with STOP)
# 7 runs; the 7th cycle stages Z and sends STOP, so the console parks.
# Resume after a dead loop: run_soak.sh <model> <order> <runs> <logs suffix>,
# e.g. `3000 RECHTZ 6 b` when U finished on the card but was never collected
# (its log lands as auto001-x-*, the arm hw_loop cannot know).
source "$(cd "$(dirname "$0")/../.." && pwd)/workdirs.sh"
# INPROC_SOAK_TREE: the gpsp tree to run (default this script's); INPROC_SOAK_OUT: stages + logs
INPROC_SOAK_OUT=${INPROC_SOAK_OUT:-$GBADHOC_WORK/inproc-soak-out}
cd "${INPROC_SOAK_TREE:-$(dirname "$0")/../../..}" || exit 9
M=$1; ORDER=${2:-SURECHTZ}; RUNS=${3:-7}; O=$INPROC_SOAK_OUT; ST=$O/stage/$M; L=$O/logs/$M${4:-}
mkdir -p $L
case $M in
  3000) DRV=(--host F: --join "") ;;
  1000) DRV=(--host "" --join D:) ;;
  *) echo "model?"; exit 2 ;;
esac
echo "=== soak $M start $(date +%H:%M:%S) order $ORDER runs $RUNS pid $$ ===" >> $L/loop.log
python -u tools/rig/hw_loop.py "${DRV[@]}" --appdir GBADHOC-GBLINK \
  --arms S=$ST/S1,U=$ST/U,R=$ST/Fr,E=$ST/Fe,C=$ST/Fc,H=$ST/Fh,T=$ST/S2,Z=$ST/Z \
  --order $ORDER --runs $RUNS --golden $ST/golden --logs $L \
  --keep-going --timeout 3600 --window-min-left 45 >> $L/loop.log 2>&1
echo "=== soak $M hw_loop exit=$? $(date +%H:%M:%S) ===" >> $L/loop.log
