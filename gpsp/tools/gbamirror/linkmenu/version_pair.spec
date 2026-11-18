# Version mismatch: console 1 is an OLDER build (claude/gbalink-hw ebd76a0,
# before the link menus) hosting a cable room by harness role, with the GB
# link protocol; console 2 (this build) joins it from the scan list.
N=2
D=$(dirname "$SPEC")
OLD=${OLD:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-hw-out/h6-link-harness64}
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$OLD
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_INI='gba_dual = 1\ngblink_role = host\ngroup = GC07AW2E'
I1_CFG='standby = 0'
I1_UI=$D/empty.ui
I2_EB=$EB64
I2_ROM=$HOME/gbamulti/roms/aw2.gba
I2_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I2_STATE=$HOME/gbamulti/fx/aw2.st0
I2_SAVE=$HOME/gbamulti/fx/aw2.sav
I2_CFG='standby = 0'
I2_UI=$D/version_join.ui
TIMEOUT_S=300
