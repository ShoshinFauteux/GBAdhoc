#!/usr/bin/env bash
# camp.sh BUILD NAME -- the desktop campaign for a stage (WSL): the stage 8
# sets (test; fuzz + sleeps; target; cadence + synthesis skipped) on six
# games, plus the save sets of stage 9.
set -uo pipefail
BUILD=$1; NAME=$2
W=$HOME/rewind-agent; S=$W/s9; R=$HOME/drcorpus/corpus/roms
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/workdirs.sh"
SRC=${RW_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}   # the gpsp tree to copy
mkdir -p $S/tools $S/work/$NAME $S/logs/$NAME
cp "$SRC"/tools/rewind_*.py $S/tools/ && sed -i 's/\r$//' $S/tools/*.py
ALL="--rewind-inplace --rewind-dirty 3 --rewind-fast-restore --rewind-clock --rewind-slack-us 1 --rewind-pitch 256 --rewind-pace-us 3000 --rewind-burst 4 --rewind-budget-us 600 --rewind-pool 3145728 --rewind-ring 2097152 --rewind-step 4 --rewind-verify"
rom() { case $1 in
  d2) echo $R/driver_2_advance-beab41.gba;; em) echo $R/pokemon_emerald_version-605b89.gba;;
  gs) echo $R/golden_sun-1cdd7f.gba;; mc) echo $R/legend_of_zelda_the_the_mini-a10489.gba;;
  aria) echo $R/castlevania_aria_of_sorrow-e7470d.gba;; fe8) echo $R/fire_emblem_the_sacred_stone-005531.gba;;
  fr) echo $R/pokemon_firered_version-51901a.gba;; sa) echo $R/sonic_advance-49ecc8.gba;;
  *) ls $R/$1*.gba | head -1;; esac; }
inp() { [ $1 = d2 ] && echo $W/work/in/d2.inputs || echo $W/work/in/mash.inputs; }
job() { # tag game mode play ticks seeds extra...
  local tag=$1 g=$2 mode=$3 play=$4 ticks=$5 seeds=$6; shift 6
  python3 $S/tools/rewind_test.py $mode --bin $W/$BUILD/sdl/gpsp_sdl --bios $W/bios \
    --rom "$(rom $g)" --inputs "$(inp $g)" --play $play --ticks $ticks --seeds $seeds \
    --work $S/work/$NAME/$tag --allow-cut 16 -- "$@" > $S/logs/$NAME/$tag.log 2>&1
  echo "$tag rc=$? $(grep -c '^PASS' $S/logs/$NAME/$tag.log)P/$(grep -c '^FAIL' $S/logs/$NAME/$tag.log)F $(grep -h 'rewind_check save' $S/logs/$NAME/$tag.log | sed 's/rewind_check save: //' | tr '\n' '|' | cut -c1-260)"
}
export -f job rom inp; export W S R BUILD NAME ALL
{
for g in d2 em gs mc aria fe8; do
  echo "job A_$g $g test 4000 0 1 $ALL"
  echo "job B_$g $g fuzz 4000 12000 1,2,3 $ALL --rewind-sleep-every 977"
  echo "job C_$g $g fuzz 4000 12000 1,2,3 $ALL"
  echo "job T_$g $g test 4000 0 1 $ALL --rewind-target 2 --rewind-ring 262144"
  echo "job U_$g $g fuzz 4000 12000 1,2 $ALL --rewind-target 20 --rewind-stride 3 --rewind-period 2 --rewind-sound 2"
  echo "job V_$g $g fuzz 4000 12000 1 $ALL --rewind-target 20 --rewind-sleep-every 977"
  echo "job O_$g $g fuzz 4000 12000 1 $ALL --rewind-stop-at-save"
  echo "job I_$g $g fuzz 4000 12000 1 --rewind-burst 4 --rewind-verify"
done
# the save: games that write it while the storm runs, deeper into the game
for g in fe8 aria mc gs em fr sa; do
  echo "job S_$g $g fuzz 9000 14000 1,2,3 $ALL"
done
echo "job S2_fe8 fe8 fuzz 9000 14000 4,5,6 $ALL --rewind-target 20"
echo "job S3_fe8 fe8 fuzz 9000 14000 7 $ALL --rewind-ring 262144"
} > $S/work/$NAME/jobs.txt
cat $S/work/$NAME/jobs.txt | xargs -P 10 -I{} bash -c '{}' > $S/logs/$NAME.out 2>&1
sort $S/logs/$NAME.out
echo "#### DONE $(grep -c ' rc=0 ' $S/logs/$NAME.out) of $(wc -l < $S/work/$NAME/jobs.txt) jobs rc=0"
