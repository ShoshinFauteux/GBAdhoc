#!/usr/bin/env bash
# sx.sh BUILD -- every ROM of the corpus: find where the mash script makes the
# game write its save, then rewind across the last of those writes (300, 45,
# 7, 1 frames and to the start of the history) and replay with the same
# inputs.  One line per game: chip, writes, writes gone back over, verdict.
set -uo pipefail
BUILD=$1
W=$HOME/rewind-agent; S=$W/s9; O=$S/sx; mkdir -p $O
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/workdirs.sh"
SAVS=${RW_SAVS:-$GBADHOC_ROMS}
ALL="--rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock --rewind-burst 4 --rewind-verify --rtc-pin 1700000000"
one() {
  rom=$1; n=$(basename "$rom" .gba); d=$O/$n; rm -rf "$d"; mkdir -p "$d"
  ( cd $W/$BUILD/sdl && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 900 ./gpsp_sdl --rom "$rom" \
      --bios-dir $W/bios --save $d/ref.sav --script $W/work/in/mash.inputs --ff --no-pacing \
      --log $d/ref.log --dump-dir $d --autoexit 14400 --rewind $ALL --rewind-hash $d/ref.hash >/dev/null 2>&1 )
  chip=$(grep -o 'rewind_save chip=[a-z0-9]*' $d/ref.log | cut -d= -f2)
  K=$(python3 - $d/ref.hash <<'PY'
import sys
prev = None; w = []
for line in open(sys.argv[1]):
    t = line.split()
    if t and t[0] == 'F':
        if prev is not None and t[7] != prev: w.append(int(t[1]))
        prev = t[7]
# the last write that leaves room after it, and how many there are
c = [f for f in w if f < 13000]
print('%d %d' % (c[-1] + 100 if c else 0, len(w)))
PY
)
  set -- $K; play=$1; nw=$2
  if [ "$play" = 0 ]; then echo "$n chip=$chip save_frames=$nw: never writes its save in this script"; return; fi
  python3 $S/tools/rewind_test.py test --bin $W/$BUILD/sdl/gpsp_sdl --bios $W/bios --rom "$rom" \
    --inputs $W/work/in/mash.inputs --play $play --work $d --allow-cut 16 -- $ALL > $d/test.log 2>&1
  echo "$n chip=$chip save_frames=$nw play=$play $(grep -o 'gone_back_over=[0-9]* ([0-9]* distinct) resumes_compared=[0-9]* (bad [0-9]*) rerun_saves_compared=[0-9]*' $d/test.log) $(grep -o 'compared=[0-9]* (bad [0-9]*)' $d/test.log | head -1) $(grep -o 'cut_moved=[0-9]*' $d/test.log) $(grep -c '^PASS' $d/test.log)P/$(grep -c '^FAIL' $d/test.log)F"
}
export -f one; export W S O BUILD ALL
ls $HOME/drcorpus/corpus/roms/*.gba | xargs -P 10 -I{} bash -c 'one "{}"' | sort
