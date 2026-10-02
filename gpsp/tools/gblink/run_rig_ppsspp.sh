#!/usr/bin/env bash
# run_rig_ppsspp.sh -- a whole hardware-rig session (hw_loop's --order) in
# PPSSPP pairs, scored by the hardware scorer (score_rig.py).
#
#   run_rig_ppsspp.sh OUT RIG ORDER ROMDIR [JOBS]
#     RIG     prepare_gblink_rig.py's output
#     ORDER   hw_loop's --order string, e.g. YBJHNDUX
#     ROMDIR  crystal.gbc + red.gb (see run_stage_pair.sh)
#     JOBS    pairs at once (default 4); each pair runs in its OWN private
#             namespace (network, /dev/shm, /tmp), so pairs cannot hear or
#             trip over each other.
#
# Each run is run_stage_pair.sh with LAUNCH=together (both consoles start at
# once, as hw_loop relaunches them; J/H carry their own offsets) and
# --fat=join (the join is the PSP-1000 model, as on the rig).  Logs and saves
# are collected under hw_loop's names (OUT/logs/autoNNN-<arm>-<role>.log,
# autoNNN-<role>-<save>) and the session is scored by score_rig.py with the
# PPSSPP models (host PSP-2000, join PSP-1000).  PPSSPP has no USB handoff:
# this proves the staged files, keys, scripts and scorer, not the handoff.
set -uo pipefail
OUT="$1"; RIG="$2"; ORDER="$3"; ROMS="$4"; JOBS="${5:-4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/runs" "$OUT/logs"
n=${#ORDER}
run_one() {  # index arm
  local tag; tag=$(printf '%03d' "$1")
  unshare -rmnpf --mount-proc --propagation private bash -c \
    'mount -t tmpfs tmpfs /dev/shm && mount -t tmpfs tmpfs /tmp && ip link set lo up &&
     exec env LAUNCH=together "$0" "$1" "$2" "$3" "$4" --fat=join' \
    "$HERE/run_stage_pair.sh" "$OUT/runs/$tag-$2" "$RIG" "$2" "$ROMS" \
    > "$OUT/runs/$tag-$2.out" 2>&1
  echo "run $tag arm $2: exit $?"
}
i=0
while [ "$i" -lt "$n" ]; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 2; done
  run_one "$((i + 1))" "${ORDER:$i:1}" &
  i=$((i + 1))
done
wait
for d in "$OUT"/runs/[0-9][0-9][0-9]-?; do
  [ -d "$d" ] || continue
  b="$(basename "$d")"; tag="${b%%-*}"; arm="${b#*-}"
  for r in host join; do
    [ -f "$d/$r.log" ] && cp "$d/$r.log" "$OUT/logs/auto$tag-$arm-$r.log"
    for f in "$d/$r-"*.sav; do
      [ -f "$f" ] && cp "$f" "$OUT/logs/auto$tag-$r-$(basename "$f" | sed "s/^$r-//")"
    done
  done
done
python3 "$HERE/score_rig.py" --logs "$OUT/logs" --rig "$RIG" \
  --host-model PSP-2000 --join-model PSP-1000 | tee "$OUT/score_rig.txt"
exit "${PIPESTATUS[0]}"
