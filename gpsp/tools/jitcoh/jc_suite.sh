#!/usr/bin/env bash
# jc_suite.sh VARIANT [PREFIX] -- the coherency checker over the whole
# dr_oracle fixture library (tools/drprof/dr_suite.sh's SUITE, the single
# source of truth), one container per fixture, in parallel.
#
#   ONLY=hns_heavy,aw2  FRAMES=600  JC_ARGS=...  jc_suite.sh jc-ship
#
# Writes $DRPROF_HOME/runs/jc/PREFIX/<fixture>/coh.txt and prints each
# fixture's model totals.  Exit 1 if any model flagged anything.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VARIANT=$1
PREFIX=${2:-$VARIANT}
ONLY=${ONLY:-}
DRPROF_HOME=${DRPROF_HOME:-$HOME/drprof}
SUITE=$(sed -n '/^SUITE=\${SUITE:-"/,/^"}/p' "$HERE/../drprof/dr_suite.sh" | sed '1d;$d')
pids=()
while read -r name rom st sav scr fr; do
  [ -z "$name" ] && continue
  [ -n "$ONLY" ] && [[ ",$ONLY," != *",$name,"* ]] && continue
  ( SAV=$sav bash "$HERE/jc_run.sh" "jc/$PREFIX/$name" "$rom" "$st" "$scr" "${FRAMES:-$fr}" "$VARIANT" \
      > /dev/null 2>&1 ) &
  pids+=($!)
done <<< "$SUITE"
wait "${pids[@]}"
bad=0
while read -r name rom st sav scr fr; do
  [ -z "$name" ] && continue
  [ -n "$ONLY" ] && [[ ",$ONLY," != *",$name,"* ]] && continue
  f=$DRPROF_HOME/runs/jc/$PREFIX/$name/coh.txt
  # dr_host prints `done frames=` only on a clean exit; qemu still runs the
  # plugin's atexit after a guest crash, so a coh.txt alone proves nothing.
  if [ ! -s "$f" ] || ! grep -q '^V ' "$f" ||
     ! grep -q '^done frames=' "$DRPROF_HOME/runs/jc/$PREFIX/$name/run.out"; then
    echo "$name: RUN FAILED ($(tail -2 "$DRPROF_HOME/runs/jc/$PREFIX/$name/run.out" 2>/dev/null | tr '\n' ' '))"
    bad=1; continue
  fi
  fr_done=$(grep -o 'frames=[0-9]*' "$f" | head -1)
  sums=$(awk '/^V /{split($4,a,"="); printf "%s=%s ", $2, a[2]}' "$f")
  echo "$name $fr_done incidents: $sums"
  awk '/^V /{split($4,a,"="); if (a[2]+0 > 0) exit 1}' "$f" || bad=1
done <<< "$SUITE"
exit $bad
