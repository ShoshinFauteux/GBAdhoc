#!/usr/bin/env bash
# Startup heap: base (3237335) vs this branch, both memory layouts, by-path
# (harness) and browser (Shelf / Marquee) launches of Emerald with hero art.
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR (holds b-harness/, base-harness/, readme30 lib at ../readme30/lib)}
LIB=$D/../readme30/lib; R=$D/../readme30/roms
FIX=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/dynarec-profile-out/rig-stage-hnsdiag/base
cd "$(dirname "$0")"
EMH="$LIB/hero/Pokemon - Emerald Version (USA, Europe).565"
prun() { tag=$1; eb=$2; shift 2; ( export "$@"; bash run.sh $tag $eb setups/game.sh 600 ); }
brun() { tag=$1; eb=$2; shift 2; ( export "$@"; bash run.sh $tag $eb setups/browser.sh 600 ); }
BS='150 press CROSS'
for L in harness harness64; do
  case $L in harness) MINE=$D/b-harness; BASE=$D/base-harness;; *) MINE=$D/b-harness64; BASE=$D/base-harness64;; esac
  prun H-$L-path-base $BASE ROMSRC=$R/emerald.gba ROMNAME=emerald.gba HERO="$EMH" FRAMES=120 CFG='scale = 1' &
  prun H-$L-path-on   $MINE ROMSRC=$R/emerald.gba ROMNAME=emerald.gba HERO="$EMH" FRAMES=120 CFG='scale = 1\nambient_gba = 1' &
  prun H-$L-path-off  $MINE ROMSRC=$R/emerald.gba ROMNAME=emerald.gba HERO="$EMH" FRAMES=120 CFG='scale = 1\nloading_art = 0' &
  wait
  for sh in 0 1; do
    brun H-$L-br$sh-base $BASE BSCRIPT="$BS" FRAMES=120 CFG="ui_shell = $sh" &
    brun H-$L-br$sh-on   $MINE BSCRIPT="$BS" FRAMES=120 CFG="ui_shell = $sh\nambient_gba = 1" &
  done
  wait
done
# AW2 oracle on the base build too
( export ROMSRC=$FIX/roms/aw2.gba ROMNAME=aw2.gba STATE=$FIX/roms/aw2.st0 SAV=$FIX/roms/aw2.sav SCRIPT=$FIX/aw2.txt LOADST=1 SHASH=1 FRAMES=3000 CFG='scale = 1'; bash run.sh A0-base $D/base-harness setups/game.sh 900 )
