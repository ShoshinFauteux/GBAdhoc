#!/usr/bin/env bash
# Screenshots: scale modes, ambient (art / palette / snapshot), loading
# screens, GB/GBC, and the per-console Settings section.
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR (holds b-harness/, base-harness/, readme30 lib at ../readme30/lib)}
FIX=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/dynarec-profile-out/rig-stage-hnsdiag/base
LIB=$D/../readme30/lib; R=$D/../readme30/roms
cd "$(dirname "$0")"
EB=$D/b-harness
run() { tag=$1; shift; ( export "$@"; bash run.sh $tag $EB setups/game.sh 600 ) ; }
brun() { tag=$1; shift; ( export "$@"; bash run.sh $tag $EB setups/browser.sh 600 ) ; }
EM="ROMSRC=$R/emerald.gba"
EMH="$LIB/hero/Pokemon - Emerald Version (USA, Europe).565"
# GBA Emerald (state), each scale, ambient art on; plus fit with ambient off
for s in 0 1 2 3; do
  run S-gba-s$s ROMSRC=$R/emerald.gba ROMNAME=emerald.gba STATE=$R/states/emerald.st0 LOADST=1 HERO="$EMH" FRAMES=200 GEAT=150 LDUMP=2 CFG="scale_gba = $s\nambient_gba = 1" &
  [ $s = 1 ] && wait
done
wait
run S-gba-fit-off ROMSRC=$R/emerald.gba ROMNAME=emerald.gba STATE=$R/states/emerald.st0 LOADST=1 HERO="$EMH" FRAMES=200 GEAT=150 CFG="scale_gba = 1\nambient_gba = 0" &
run S-gba-1x-off  ROMSRC=$R/emerald.gba ROMNAME=emerald.gba STATE=$R/states/emerald.st0 LOADST=1 HERO="$EMH" FRAMES=200 GEAT=150 CFG="scale_gba = 0\nambient_gba = 0" &
# no art: palette fallback and snapshot fallback (AW2 has no art by name)
run S-noart-pal   ROMSRC=$FIX/roms/aw2.gba ROMNAME=aw2.gba STATE=$FIX/roms/aw2.st0 LOADST=1 FRAMES=420 GEAT=400 LDUMP=2 CFG="scale_gba = 1\nambient_gba = 1" &
wait
run S-noart-snap  ROMSRC=$FIX/roms/aw2.gba ROMNAME=aw2.gba STATE=$FIX/roms/aw2.st0 LOADST=1 FRAMES=420 GEAT=400 CFG="scale_gba = 0\nambient_gba = 2" &
run S-noart-light ROMSRC=$FIX/roms/aw2.gba ROMNAME=aw2.gba FRAMES=60 LDUMP=2 CFG="theme = 1\nambient_gba = 1" &
run S-gba-light   ROMSRC=$R/emerald.gba ROMNAME=emerald.gba HERO="$EMH" FRAMES=60 LDUMP=2 CFG="theme = 1" &
wait
# GBC Crystal (state, hero) at 1x / fit / 2x, GB Kirby (hero) at 2x
CRH="$LIB/hero/Pokemon - Crystal Version (USA, Europe) (Rev 1).565"
KBH="$LIB/hero/Kirby's Dream Land (USA, Europe).565"
for s in 0 1 3; do
  run S-gbc-s$s ROMSRC=$R/crystal.gbc ROMNAME=crystal.gbc STATE=$R/states/crystal.gbc.st0 LOADST=1 HERO="$CRH" FRAMES=200 GEAT=150 LDUMP=2 CFG="console = 2\nscale_gbc = $s\nambient_gbc = 1" &
done
wait
run S-gb-2x  ROMSRC=$R/kirby.gb ROMNAME=kirby.gb HERO="$KBH" FRAMES=700 GEAT=650 LDUMP=2 CFG="console = 1\nscale_gb = 3\nambient_gb = 1" &
run S-gb-1x-pal ROMSRC=$R/kirby.gb ROMNAME=kirby.gb FRAMES=700 GEAT=650 LDUMP=2 CFG="console = 1\nscale_gb = 0\nambient_gb = 1" &
# in-game Settings on a GB game (DISPLAY (GB))
run S-gb-uidemo ROMSRC=$R/kirby.gb ROMNAME=kirby.gb HERO="$KBH" FRAMES=900 UIDEMO=1 CFG="console = 1\nscale_gb = 1\nambient_gb = 1" &
wait
# browser: Settings per console (GBA -> GB -> GBC), then pick Emerald
BS='90 press START\n100 press DOWN\n106 press DOWN\n120 dump set_gba\n126 press CIRCLE\n140 press TRIANGLE\n200 press START\n210 press DOWN\n216 press DOWN\n230 dump set_gb\n236 press CIRCLE\n250 press TRIANGLE\n310 press START\n320 dump set_gbc\n326 press CIRCLE\n340 press TRIANGLE\n400 press CROSS'
brun S-browser-settings BSCRIPT="$BS" FRAMES=200 LDUMP=3 GEAT=150 CFG="console = 0\nui_shell = 1\nscale_gb = 3\nambient_gb = 2\ngb_palette_gb = 2" &
wait
