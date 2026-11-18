# GE comparison walk (docs/GBA-LINK-UI.md): one console, the design's
# backdrop injected (ui_backdrop), theme 1.
N=1
D=$(dirname "$SPEC")
I1_EB=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_ROM=$HOME/gbamulti/roms/em.gba
I1_ROMNAME="Pokemon - Emerald Version.gba"
I1_STATE=$HOME/gbamulti/fx/em.st0
I1_SAVE=$HOME/gbamulti/fx/em-host.sav
I1_THEME=1
I1_CONSOLE=0
I1_BACKDROP=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/fx}/emerald.565
I1_UI=$D/ge_em.ui
I1_CFG='standby = 0\nfilter = 2'
TIMEOUT_S=300
