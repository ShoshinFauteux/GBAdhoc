# Wireless Adapter room, unchanged: console 1 (this build) hosts Emerald's
# room from the menu (HOST chooser -> Wireless Adapter, GPSP07); console 2 is
# an OLDER build joining it by harness join=1 group=GPSP07 (its Join room 7).
N=2
D=$(dirname "$SPEC")
OLD=${OLD:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-hw-out/h6-link-harness64}
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$EB64
I1_ROM=$HOME/gbamulti/roms/em.gba
I1_ROMNAME="Pokemon - Emerald Version.gba"
I1_STATE=$HOME/gbamulti/fx/em.st0
I1_SAVE=$HOME/gbamulti/fx/em-host.sav
I1_CFG='standby = 0'
I1_UI=$D/adapter_host.ui
I2_EB=$OLD
I2_ROM=$HOME/gbamulti/roms/em.gba
I2_ROMNAME="Pokemon - Emerald Version.gba"
I2_STATE=$HOME/gbamulti/fx/em.st0
I2_SAVE=$HOME/gbamulti/fx/em-join.sav
I2_INI='join = 1\ngroup = GPSP07\nautoexit_frames = 2400'
I2_CFG='standby = 0'
I2_UI=$D/empty.ui
TIMEOUT_S=300
