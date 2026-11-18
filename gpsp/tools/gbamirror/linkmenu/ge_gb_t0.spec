# GE comparison walk (docs/GBA-LINK-UI.md): one console, the design's
# backdrop injected (ui_backdrop), theme 0.
N=1
D=$(dirname "$SPEC")
I1_EB=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_ROM=$HOME/gbdual/roms/red.gb
I1_ROMNAME="Pokemon - Crystal Version.gb"
I1_STATE=
I1_SAVE=
I1_THEME=0
I1_CONSOLE=1
I1_BACKDROP=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/fx}/gb.565
I1_UI=$D/ge_gb.ui
I1_CFG='standby = 0\nfilter = 2'
TIMEOUT_S=300
