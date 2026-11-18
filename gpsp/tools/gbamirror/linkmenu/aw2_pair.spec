# AW2 Link Cable room: console 1 hosts by menu, console 2 joins through the
# scan list, both play the RAM-gated battle (multigame fixture scripts, held
# until the link runs); the host ends by agreement 120 frames after its
# script is done, and both exit 240 frames after the link closed.
N=2
D=$(dirname "$SPEC")
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
EB32=${EB32:-$EB64}
MG=$D/../multigame/scripts
COMMON_INI='gba_dual_hash = 1\nlink_exit_after = 240\ngblink_end_after = 120'

I1_EB=$EB64
I1_MODEL=1
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_THEME=${THEME:-0}
I1_INI=$COMMON_INI
I1_CFG='standby = 0'
I1_BACKDROP=${BACKDROP:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/fx/aw2.565}
I1_SCRIPT=$MG/aw2_host.txt
I1_UI=$D/${HOST_UI:-aw2_host.ui}

I2_EB=$EB32
I2_MODEL=${JOIN_MODEL:-1}
I2_ROM=$HOME/gbamulti/roms/aw2.gba
I2_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I2_STATE=$HOME/gbamulti/fx/aw2.st0
I2_SAVE=$HOME/gbamulti/fx/aw2.sav
I2_THEME=${THEME:-0}
I2_INI=$COMMON_INI
I2_CFG='standby = 0'
I2_BACKDROP=${BACKDROP:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/fx/aw2.565}
I2_SCRIPT=$MG/aw2_join.txt
I2_UI=$D/${JOIN_UI:-aw2_join.ui}

TIMEOUT_S=1500
