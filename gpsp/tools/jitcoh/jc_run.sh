#!/usr/bin/env bash
# jc_run.sh -- one twin run under qemu with the coherency checker plugin.
#
#   jc_run.sh OUTDIR ROM STATE SCRIPT FRAMES VARIANT [dr_host args...]
#
# Same fixtures and arguments as tools/drprof/dr_profile.sh (SAV=file.sav in
# fx/ is copied to a scratch path first).  Writes OUTDIR/{coh.txt,hash.txt,
# host.log} under $DRPROF_HOME/runs.  JC_ARGS adds plugin args (default:
# ioff=0.37.64.101,maxev=4000,jshift=20 -- ioff is DOT-separated; jshift=20 models
# the PSP LARGE tier's 64-byte cache alignment; dlru=1 models D-cache eviction, ~2x slower).
# JC_QENV=VAR=value sets a guest environment variable (e.g. MALLOC_PERTURB_=85:
# heap garbage, to stand in for a console's used heap).
set -euo pipefail
OUT=$1; ROM=$2; STATE=$3; SCRIPT=$4; FRAMES=$5; VARIANT=$6
shift 6
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
IMAGE=${DRPROF_IMAGE:-drprof-qemu}
JC_ARGS=${JC_ARGS:-ioff=0.37.64.101,maxev=4000,jshift=20}
mkdir -p "$DRPROF_HOME/runs/$OUT"
BIN=/w/bin/dr_host_$VARIANT
NM=$DRPROF_HOME/bin/dr_host_$VARIANT.nm
sym()  { awk -v s="$1" '$NF==s{print $1; exit}' "$NM"; }
# The caches are .space arrays in mips_stub.S (nm gives no size): the twin
# uses the non-SMALL sizes of gpsp_config.h, 10 MiB ROM + 512 KiB RAM, which
# are also the PSP's LARGE tier (dr_host.c publishes the same in drprof_ctl).
PARGS="out=/w/runs/$OUT/coh.txt,rom=$(sym rom_translation_cache):a00000,ram=$(sym ram_translation_cache):80000"
PARGS="$PARGS,mbox=$(sym jitcoh_mbox),mark=$(sym drprof_host_mark),ctl=$(sym drprof_ctl)"
PARGS="$PARGS,xpc=$(sym drprof_xlat_pc)"
PARGS="$PARGS,$JC_ARGS"
docker run --rm -v "$DRPROF_HOME":/w -w /w/fx "$IMAGE" bash -c "
  set -e
  O=/w/runs/$OUT
  SCR=''; [ '$SCRIPT' != '-' ] && SCR='--script $SCRIPT'
  SV=''; [ -n '${SAV:-}' ] && cp '${SAV:-}' /tmp/run.sav && SV='--save /tmp/run.sav'
  ( time /opt/qemu/bin/qemu-mipsel -cpu 74Kf ${JC_QENV:+-E $JC_QENV} -plugin /w/bin/jitcoh_plugin.so,$PARGS $BIN \
     --rom $ROM --bios-dir . \$SV --state $STATE \$SCR --frames $FRAMES \
     --hash \$O/hash.txt --log \$O/host.log $* ) > \$O/run.out 2>&1
  tail -4 \$O/run.out
"
