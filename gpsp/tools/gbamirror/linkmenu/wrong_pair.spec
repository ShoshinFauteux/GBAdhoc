# Wrong game: console 1 hosts AW2 by cable, console 2 runs Super Circuit.
N=2
D=$(dirname "$SPEC")
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$EB64
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_CFG='standby = 0'
I1_UI=$D/host_wait.ui
I2_EB=$EB64
I2_ROM=$HOME/gbamulti/roms/msc.gba
I2_ROMNAME="Mario Kart - Super Circuit (USA).gba"
I2_STATE=$HOME/gbamulti/fx/msc.st0
I2_CFG='standby = 0'
I2_UI=$D/wrong_join.ui
TIMEOUT_S=300
