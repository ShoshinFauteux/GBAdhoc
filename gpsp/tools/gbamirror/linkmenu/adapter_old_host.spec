# Wireless Adapter room, the other way: console 1 is an OLDER build hosting
# by harness host=1 group=GPSP07 (FireRed); console 2 (this build, FireRed
# links both ways) uses Join room 7, which scans, finds only the adapter
# room, and joins it.
N=2
D=$(dirname "$SPEC")
OLD=${OLD:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-hw-out/h6-link-harness64}
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$OLD
I1_ROM=$HOME/gbalink/roms/fr.gba
I1_ROMNAME="Pokemon - FireRed Version.gba"
I1_STATE=$HOME/gbalink/roms/fr.st0
I1_SAVE=$HOME/gbalink/roms/fr.sav
I1_INI='host = 1\ngroup = GPSP07\nautoexit_frames = 3000'
I1_CFG='standby = 0'
I1_UI=$D/empty.ui
I2_EB=$EB64
I2_ROM=$HOME/gbalink/roms/fr.gba
I2_ROMNAME="Pokemon - FireRed Version.gba"
I2_STATE=$HOME/gbalink/roms/fr.st0
I2_SAVE=$HOME/gbalink/roms/fr.sav
I2_CFG='standby = 0'
I2_UI=$D/adapter_join.ui
TIMEOUT_S=300
