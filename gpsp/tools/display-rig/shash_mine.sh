#!/usr/bin/env bash
# The emulation-unchanged oracle on the AW2 fixture: per-frame guest state
# (shash.txt) with the display features off vs on, same build.
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR (holds b-harness/, base-harness/, readme30 lib at ../readme30/lib)}
FIX=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/dynarec-profile-out/rig-stage-hnsdiag/base
LIB=$D/../readme30/lib
cd "$(dirname "$0")"
common() { export ROMSRC=$FIX/roms/aw2.gba ROMNAME=aw2.gba STATE=$FIX/roms/aw2.st0 SAV=$FIX/roms/aw2.sav SCRIPT=$FIX/aw2.txt LOADST=1 SHASH=1 FRAMES=${NF:-3000}; }
run() { tag=$1; shift; ( common; export "$@"; bash run.sh $tag ${EB:-$D/b-harness} setups/game.sh 900 ) ; }
run A1-off  CFG='scale = 1\nambient_gba = 0\nloading_art = 0' GEAT=1500 &
run A2-on   HERO="$LIB/hero/Advance Wars 2 - Black Hole Rising (USA).565" CFG='scale = 1\nambient_gba = 2\nloading_art = 1' GEAT=1500 &
run A3-snap CFG='scale = 0\nambient_gba = 2\nloading_art = 1' GEAT=1500 &
wait
run A4-2x   HERO="$LIB/hero/Advance Wars 2 - Black Hole Rising (USA).565" CFG='scale_gba = 3\nambient_gba = 1' GEAT=1500 &
run A5-pal  CFG='scale = 0\nambient_gba = 1\nloading_art = 1' GEAT=1500 &
wait
