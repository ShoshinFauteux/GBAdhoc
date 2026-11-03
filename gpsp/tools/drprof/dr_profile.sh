#!/usr/bin/env bash
# dr_profile.sh -- one profiled twin run under qemu + the drprof plugin.
#
#   dr_profile.sh OUTDIR ROM STATE SCRIPT FRAMES [VARIANT] [extra dr_host args]
# STATE '-' boots the ROM from power-on (no savestate); SCRIPT '-' runs no input.
# SAV=file.sav (in fx/) is copied to a scratch path first: fixtures are never written.
#
# Paths are relative to $DRPROF_HOME/fx (fixtures) and $DRPROF_HOME/runs.
# Writes OUTDIR/{prof.txt,hash.txt,host.log,stubmap.txt,symcat.txt}.
# PROF=0 runs the same thing without the plugin (oracle / speed runs).
# BLK=1 adds the block-life table OUTDIR/blk.txt (drprof_plugin.c on_blk).
# HOT=<symbol> adds per-instruction-address counts (hot:<addr>) for one function.
set -euo pipefail
OUT=$1; ROM=$2; STATE=$3; SCRIPT=$4; FRAMES=$5; VARIANT=${6:-base}
shift 6 || shift $#
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
PROF=${PROF:-1}
mkdir -p "$DRPROF_HOME/runs/$OUT"
BIN=/w/bin/dr_host_$VARIANT
docker run --rm -e DR_RAMDUMP -e DR_RAMDUMP_FILE -e SMC_EVT_FILE -e SMC_EVT_LO -e SMC_EVT_HI -e EXEC_STALE_ABORT -v "$DRPROF_HOME":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  O=/w/runs/$OUT
  SCR=''; [ '$SCRIPT' != '-' ] && SCR='--script $SCRIPT'
  ST=''; [ '$STATE' != '-' ] && ST='--state $STATE'
  SV=''; [ -n '${SAV:-}' ] && cp '${SAV:-}' /tmp/run.sav && SV='--save /tmp/run.sav'
  PLUG=''
  if [ '$PROF' = 1 ]; then
    python3 /w/src/tools/drprof/dr_symcat.py $BIN.nm $BIN.members > \$O/symcat.txt
    CTL=\$(awk '\$4==\"drprof_ctl\"{print \$1}' $BIN.nm)
    ZARG=\$(awk '\$4==\"drprof_zone_arg\"{print \$1}' $BIN.nm)
    BLKA=''; [ '${BLK:-0}' = 1 ] && BLKA=\",blk=\$(awk '\$4==\"drprof_blk_arg\"{print \$1}' $BIN.nm),bout=\$O/blk.txt\"
    PLUG=\"-plugin /w/bin/drprof_plugin.so,out=\$O/prof.txt,symcat=\$O/symcat.txt,ctl=\$CTL,zarg=\$ZARG\$BLKA${HOT:+,hot=$HOT}\"
  fi
  time /opt/qemu/bin/qemu-mipsel -cpu 74Kf \$PLUG $BIN \
     --rom $ROM --bios-dir . \$SV \$ST \$SCR --frames $FRAMES \
     --hash \$O/hash.txt --log \$O/host.log --stubmap \$O/stubmap.txt $* 2>&1 | tail -12
"
