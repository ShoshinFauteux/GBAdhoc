#!/usr/bin/env bash
# run_det_ppsspp.sh EBOOT_DIR ROM STATE SAV SCRIPTDIR OUTROOT [PAR]
# MIPS-dynarec determinism arms in PPSSPP.  SCRIPTDIR holds cold.txt,
# warm_tour.txt, warm_intro.txt from run_det_host.sh (same measured segment).
# Each arm = script + extra harness keys.  Runs PAR arms at a time.
set -u
EB=$1; ROM=$2; ST=$3; SAV=$4; SD=$5; OUT=$6; PAR=${7:-4}
LB=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/ini"
arm() { # name script keys...
  local n=$1 s=$2; shift 2
  { echo "audio_oracle = 1"; for k in "$@"; do echo "$k"; done; } > "$OUT/ini/$n.ini"
  echo "$n $s"; }
ARMS=$(
arm cold        cold
arm cold_rep    cold
arm warm_tour   warm_tour
arm warm_intro  warm_intro
arm jit_small   cold        "jit_small = 1"
arm rom_cap2    cold        "rom_cap = 2"
arm ballast     cold        "rom_ballast_kb = 5120"
arm warm_small  warm_tour   "jit_small = 1"
arm warm_cap2   warm_tour   "rom_cap = 2" "rom_ballast_kb = 3000"
arm paranoid    cold        "cache_paranoid = 1"
)
while read -r n s; do
  while [ "$(jobs -r | wc -l)" -ge "$PAR" ]; do sleep 2; done
  "$LB/ppsspp_run.sh" "$n" "$EB" "$ROM" "$ST" "$SAV" "$SD/$s.txt" "$OUT" "$OUT/ini/$n.ini" &
  sleep 3
done <<< "$ARMS"
wait
for n in $(echo "$ARMS" | awk '{print $1}'); do
  [ "$n" = cold ] && continue
  [ -f "$OUT/$n/frontend.log" ] || { echo "$n: no log"; continue; }
  printf '%-11s vs cold: ' "$n"
  python3 "$LB/cmp_hash.py" "$OUT/cold/shash.txt" "$OUT/$n/shash.txt" \
     --log-a "$OUT/cold/frontend.log" --log-b "$OUT/$n/frontend.log" \
     --mark "${MARK:-campaign_map}" --quiet
done
