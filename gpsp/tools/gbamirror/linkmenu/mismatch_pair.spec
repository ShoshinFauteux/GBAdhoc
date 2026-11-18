# ROM mismatch: same game code, one byte different inside the first sampled
# slice ($HOME/linkmenu-fx/aw2_mod.gba: aw2.gba with 0xFF at 0x200).
N=2
D=$(dirname "$SPEC")
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$EB64
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_CFG='standby = 0'
I1_UI=$D/mismatch_host.ui
I2_EB=$EB64
I2_ROM=$HOME/linkmenu-fx/aw2_mod.gba
I2_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I2_STATE=$HOME/gbamulti/fx/aw2.st0
I2_SAVE=$HOME/gbamulti/fx/aw2.sav
I2_CFG='standby = 0'
I2_UI=$D/mismatch_join.ui
TIMEOUT_S=300
