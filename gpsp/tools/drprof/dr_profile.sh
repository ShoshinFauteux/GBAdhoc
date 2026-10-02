#!/usr/bin/env bash
# dr_profile.sh -- one profiled twin run under qemu + the drprof plugin.
#
#   dr_profile.sh OUTDIR ROM STATE SCRIPT FRAMES [VARIANT] [extra dr_host args]
# SAV=file.sav (in fx/) is copied to a scratch path first: fixtures are never written.
#
# Paths are relative to $DRPROF_HOME/fx (fixtures) and $DRPROF_HOME/runs.
# Writes OUTDIR/{prof.txt,hash.txt,host.log,stubmap.txt,symcat.txt}.
# PROF=0 runs the same thing without the plugin (oracle / speed runs).
# HOT=<symbol> adds per-instruction-address counts (hot:<addr>) for one function.
set -euo pipefail
OUT=$1; ROM=$2; STATE=$3; SCRIPT=$4; FRAMES=$5; VARIANT=${6:-base}
shift 6 || shift $#
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
PROF=${PROF:-1}
mkdir -p "$DRPROF_HOME/runs/$OUT"
BIN=/w/bin/dr_host_$VARIANT
docker run --rm -v "$DRPROF_HOME":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  O=/w/runs/$OUT
  SCR=''; [ '$SCRIPT' != '-' ] && SCR='--script $SCRIPT'
  SV=''; [ -n '${SAV:-}' ] && cp '${SAV:-}' /tmp/run.sav && SV='--save /tmp/run.sav'
  PLUG=''
  if [ '$PROF' = 1 ]; then
    python3 /w/src/tools/drprof/dr_symcat.py $BIN.nm $BIN.members > \$O/symcat.txt
    CTL=\$(awk '\$4==\"drprof_ctl\"{print \$1}' $BIN.nm)
    ZARG=\$(awk '\$4==\"drprof_zone_arg\"{print \$1}' $BIN.nm)
    PLUG=\"-plugin /w/bin/drprof_plugin.so,out=\$O/prof.txt,symcat=\$O/symcat.txt,ctl=\$CTL,zarg=\$ZARG${HOT:+,hot=$HOT}\"
  fi
  time /opt/qemu/bin/qemu-mipsel -cpu 74Kf \$PLUG $BIN \
     --rom $ROM --bios-dir . \$SV --state $STATE \$SCR --frames $FRAMES \
     --hash \$O/hash.txt --log \$O/host.log --stubmap \$O/stubmap.txt $* 2>&1 | tail -3
"
