#!/usr/bin/env bash
# run_det_host.sh ROM STATE SAV TOUR OUTDIR
# Host determinism arms (x86 dynarec + interpreter).  Every arm runs the SAME
# measured segment: a `state` reload followed by TOUR.  Only what happened
# BEFORE the reload differs.  Compare with cmp_hash.py (last segment).
set -u
ROM=$1; ST=$2; SAV=$3; TOUR=$4; OUT=$5
LB=$(dirname "$0")
BIOS=${BIOS:-$HOME/gbalink/bios}
mkdir -p "$OUT"
grep -v '^evt end' "$TOUR" > "$OUT/tour.txt"
mk() { # name prefix-lines
  { printf '%s\n' "$2"; echo "state"; cat "$OUT/tour.txt"; } > "$OUT/$1.txt"; }
mk cold      "wait 40"
mk warm_tour "wait 40
$(cat "$OUT/tour.txt")"
mk warm_intro "wait 40
hold A+B+SELECT+START 10
wait 2000"
mk late_load "wait 520"
run() { # arm script binary [extra args]
  local arm=$1 sc=$2 bin=$3; shift 3
  cp "$SAV" "$OUT/$arm.sav"
  "$LB/$bin" --rom "$ROM" --bios-dir "$BIOS" --save "$OUT/$arm.sav" --state "$ST" \
     --script "$OUT/$sc.txt" --frames 20000 --hash "$OUT/$arm.hash" \
     --log "$OUT/$arm.log" "$@" 2>&1 | tail -1 | sed "s/^/$arm: /"
}
run cold       cold       lb_host
run cold_rep   cold       lb_host
run warm_tour  warm_tour  lb_host
run warm_intro warm_intro lb_host
run late_load  late_load  lb_host --load-at 500
[ -x "$LB/lb_host_interp" ] && run interp cold lb_host_interp
for a in cold_rep warm_tour warm_intro late_load interp; do
  [ -f "$OUT/$a.hash" ] || continue
  printf '%-11s vs cold: ' "$a"; python3 "$LB/cmp_hash.py" "$OUT/cold.hash" "$OUT/$a.hash" --quiet
done
