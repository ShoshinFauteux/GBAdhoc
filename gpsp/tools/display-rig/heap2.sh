#!/usr/bin/env bash
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR (holds b-harness/, base-harness/, readme30 lib at ../readme30/lib)}
LIB=$D/../readme30/lib; R=$D/../readme30/roms
FIX=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/dynarec-profile-out/rig-stage-hnsdiag/base
cd "$(dirname "$0")"
brun() { tag=$1; eb=$2; shift 2; ( export "$@"; bash run.sh $tag $eb setups/browser.sh 600 ); }
prun() { tag=$1; eb=$2; shift 2; ( export "$@"; bash run.sh $tag $eb setups/game.sh 900 ); }
BS='140 press DOWN\n160 press CROSS'
for L in harness harness64; do
  case $L in harness) MINE=$D/b-harness; BASE=$D/base-harness;; *) MINE=$D/b-harness64; BASE=$D/base-harness64;; esac
  for sh in 0 1; do
    brun E-$L-br$sh-base $BASE BSCRIPT="$BS" FRAMES=120 CFG="ui_shell = $sh" &
    brun E-$L-br$sh-on   $MINE BSCRIPT="$BS" FRAMES=120 LDUMP=12 CFG="ui_shell = $sh\nambient_gba = 1" &
  done
  wait
done
# H&S oracle: features off vs on (hero art present by name, ambient + snapshot)
HS="ROMSRC=$FIX/roms/hns.gba"
prun B1-hns-off $D/b-harness ROMSRC=$FIX/roms/hns.gba ROMNAME=hns.gba STATE=$FIX/roms/hns.st0 SAV=$FIX/roms/hns.sav SCRIPT=$FIX/hns.txt LOADST=1 SHASH=1 FRAMES=900 CFG='scale = 1\nambient_gba = 0\nloading_art = 0' &
prun B2-hns-on  $D/b-harness ROMSRC=$FIX/roms/hns.gba ROMNAME=hns.gba STATE=$FIX/roms/hns.st0 SAV=$FIX/roms/hns.sav SCRIPT=$FIX/hns.txt LOADST=1 SHASH=1 FRAMES=900 HERO="$LIB/hero/Pokemon - Emerald Version (USA, Europe).565" CFG='scale = 0\nambient_gba = 2' &
prun B0-hns-base $D/base-harness ROMSRC=$FIX/roms/hns.gba ROMNAME=hns.gba STATE=$FIX/roms/hns.st0 SAV=$FIX/roms/hns.sav SCRIPT=$FIX/hns.txt LOADST=1 SHASH=1 FRAMES=900 CFG='scale = 1' &
wait
