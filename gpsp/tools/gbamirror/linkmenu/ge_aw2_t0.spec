# GE comparison walk (docs/GBA-LINK-UI.md): one console, the design's
# backdrop injected (ui_backdrop), theme 0.
N=1
D=$(dirname "$SPEC")
I1_EB=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_ROM=$HOME/gbamulti/roms/aw2.gba
I1_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I1_STATE=$HOME/gbamulti/fx/aw2.st0
I1_SAVE=$HOME/gbamulti/fx/aw2.sav
I1_THEME=0
I1_CONSOLE=0
I1_BACKDROP=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/fx}/aw2.565
I1_UI=$D/ge_aw2.ui
I1_CFG='standby = 0\nfilter = 2'
TIMEOUT_S=300
