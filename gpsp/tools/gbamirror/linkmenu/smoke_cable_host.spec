N=1
I1_EB=${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64
I1_ROM=$HOME/gbalink/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising (USA).gba"
I1_STATE=$HOME/gbalink/roms/aw2.st0
I1_SAVE=$HOME/gbalink/roms/aw2.sav
I1_UI=$(dirname "$SPEC")/smoke_cable_host.ui
I1_THEME=${THEME:-0}
TIMEOUT_S=300
I1_CFG='standby = 0'
