#!/usr/bin/env bash
# jc_stats.sh VARIANT [TAG] -- run every fixture of dr_suite.sh on a twin
# variant WITHOUT the profiler and collect the code-sync statistics line the
# core prints at exit (JIT_CODE_DISCIPLINE: `jitc ...`, JIT_SYNC_STATS:
# `jitlegacy ...`) plus any JIT_CODE_CHECK / JIT_CODE_AUDIT violation lines.
# FRAMES=N overrides every fixture's length (steady state = full - short).
# docs/JIT-CODE-DISCIPLINE.md.  Output: $DRPROF_HOME/runs/jcstats-TAG.txt
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
V=$1; TAG=${2:-$1}
H=${DRPROF_HOME:-$HOME/drprof}
OUT="$H/runs/jcstats-$TAG.txt"
: > "$OUT"
SUITE=$(python3 - "$HERE/dr_suite.sh" <<'EOF'
import sys
t = open(sys.argv[1]).read()
b = t.split('SUITE=${SUITE:-"', 1)[1].split('"}', 1)[0]
for l in b.strip().splitlines():
    if len(l.split()) == 6:
        print(l)
EOF
)
pids=()
while read -r name rom st sav scr fr; do
  [ -n "${ONLY:-}" ] && [[ ",$ONLY," != *",$name,"* ]] && continue
  ( PROF=0 SAV=$sav bash "$HERE/dr_profile.sh" "jc-$TAG/$name" "$rom" "$st" "$scr" "${FRAMES:-$fr}" "$V" \
       > "$H/runs/jc-$TAG.$name.log" 2>&1 ) &
  pids+=($!)
done <<< "$SUITE"
wait "${pids[@]}"
while read -r name rom st sav scr fr; do
  [ -n "${ONLY:-}" ] && [[ ",$ONLY," != *",$name,"* ]] && continue
  L="$H/runs/jc-$TAG.$name.log"
  frames=$(grep -o 'done frames=[0-9]*' "$L" | cut -d= -f2)
  stat=$(grep -E '^(jitc|jitlegacy) ' "$L" | tail -1)
  viol=$(grep -c 'VIOLATION' "$L")
  echo "$name frames=${frames:-?} violations_printed=$viol $stat" | tee -a "$OUT"
  grep 'VIOLATION' "$L" | head -3 | sed 's/^/    /' | tee -a "$OUT"
done <<< "$SUITE"
