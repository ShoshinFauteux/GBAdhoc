#!/usr/bin/env bash
# dr_suite.sh [VARIANT] [PREFIX] -- profile the whole fixture library.
#
# The fixtures (fx/ under $DRPROF_HOME) are the owner's own states; their
# provenance is in docs/DYNAREC-PROFILE.md.  Each line:
#   name rom state sav script frames
# Runs in parallel (one container each).  PROF=0 for oracle-only runs.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VARIANT=${1:-base}
PREFIX=${2:-$VARIANT}
ONLY=${ONLY:-}
SUITE=${SUITE:-"
aw2        aw2.gba     aw2.st0                  aw2.sav           aw2_psp_script.txt 6100
hns_heavy  hns.gba     heart_soul_heavy.st0     hns.sav           battle.txt         3100
hns_light  hns.gba     heart_soul_light.st0     hns.sav           battle.txt         3100
ub_rival   unbound.gba unbound_rival_high.st0   unbound.sav       battle.txt         3100
ub_double  unbound.gba unbound_double_high.st0  unbound.sav       battle.txt         3100
ub_ow      unbound.gba unbound_go.st0           unbound.sav       unbound_ow.txt     1750
em_battle  emerald.gba emerald_3000.st0         emerald_3000.sav  emerald_battle.txt 2100
"}
pids=()
while read -r name rom st sav scr fr; do
  [ -z "$name" ] && continue
  [ -n "$ONLY" ] && [[ ",$ONLY," != *",$name,"* ]] && continue
  ( SAV=$sav bash "$HERE/dr_profile.sh" "$PREFIX/$name" "$rom" "$st" "$scr" "$fr" "$VARIANT" \
      > "${DRPROF_HOME:-$HOME/drprof}/runs/$PREFIX.$name.out" 2>&1; echo "$name rc=$?" ) &
  pids+=($!)
done <<< "$SUITE"
wait "${pids[@]}"
